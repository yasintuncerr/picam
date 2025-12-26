/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Copyright (C) 2020, Raspberry Pi (Trading) Ltd.
 *
 * mjpeg_encoder.cpp - mjpeg video encoder.
 */

#include <chrono>
#include <iostream>
#include <thread>
#include <vector>
#include <cstring>

#include <libcamera/libcamera.h>
#include "mjpeg_encoder.hpp"

// ----------------------------------------------------------------------------
//                              COMMON CODE
// ----------------------------------------------------------------------------

MjpegEncoder::MjpegEncoder()
    : abortEncode_(false), abortOutput_(false), index_(0)
{
    // Output thread her zaman çalışır
    output_thread_ = std::thread(&MjpegEncoder::outputThread, this);

#ifdef ENABLE_HW_MJPEG
    // --- HW INIT ---
    fd_m2m_ = -1;
    hw_initialized_ = false;
    cap_buffers_ = nullptr;
    cap_mem_ = nullptr;
    cap_buf_cnt_ = 0;
    
    // HW modunda tek bir encode thread yeterli (seri işlem)
    encode_thread_ = std::thread(std::bind(&MjpegEncoder::encodeThread, this, 0));
    std::cout << "[MjpegEncoder] Using Hardware Encoding (/dev/video11) with High Quality" << std::endl;
#else
    // --- SW INIT ---
    // SW modunda çoklu thread
    for (int i = 0; i < NUM_ENC_THREADS; i++)
        encode_thread_[i] = std::thread(std::bind(&MjpegEncoder::encodeThread, this, i));
    std::cout << "[MjpegEncoder] Using Software Encoding (libjpeg)" << std::endl;
#endif
}

MjpegEncoder::~MjpegEncoder()
{
    // Encode threadlerini durdur
    abortEncode_ = true;
    encode_cond_var_.notify_all();

#ifdef ENABLE_HW_MJPEG
    if (encode_thread_.joinable()) encode_thread_.join();
    hw_uninit();
#else
    for (int i = 0; i < NUM_ENC_THREADS; i++) {
        if (encode_thread_[i].joinable())
            encode_thread_[i].join();
    }
#endif

    // Output threadini durdur
    abortOutput_ = true;
    output_cond_var_.notify_all();
    if (output_thread_.joinable()) output_thread_.join();
}

void MjpegEncoder::EncodeBuffer(void *mem, void *dest, unsigned int size,
                                StreamInfo const &info, int64_t timestamp_us,
                                unsigned int cookie, int fd)
{
    std::lock_guard<std::mutex> lock(encode_mutex_);
    EncodeItem item = { mem, dest, size, info, timestamp_us, index_++, cookie, fd };
    
    encode_queue_.push(item);
    encode_cond_var_.notify_all();
}

StreamInfo MjpegEncoder::getStreamInfo(libcamera::Stream *stream)
{
    libcamera::StreamConfiguration const &cfg = stream->configuration();
    StreamInfo info;
    info.width = cfg.size.width;
    info.height = cfg.size.height;
    info.stride = cfg.stride;
    info.pixel_format = cfg.pixelFormat;
    info.colour_space = cfg.colorSpace;
    return info;
}

// ----------------------------------------------------------------------------
//                          HARDWARE IMPLEMENTATION
// ----------------------------------------------------------------------------
#ifdef ENABLE_HW_MJPEG

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#define M2M_DEVICE "/dev/video11"
#define CAPTURE_BUFFER_COUNT 4
#define JPEG_QUALITY 90  // Kaliteyi artırdık (0-100 arası, varsayılan düşüktü)

void MjpegEncoder::hw_uninit()
{
    if (fd_m2m_ >= 0) {
        int type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        ioctl(fd_m2m_, VIDIOC_STREAMOFF, &type);
        type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        ioctl(fd_m2m_, VIDIOC_STREAMOFF, &type);

        if (cap_mem_) {
            for (int i = 0; i < cap_buf_cnt_; i++) {
                if (cap_mem_[i] != MAP_FAILED && cap_buffers_)
                    munmap(cap_mem_[i], cap_buffers_[i].m.planes[0].length);
            }
            free(cap_mem_);
            cap_mem_ = nullptr;
        }
        if (cap_buffers_) {
            free(cap_buffers_);
            cap_buffers_ = nullptr;
        }
        close(fd_m2m_);
        fd_m2m_ = -1;
    }
}

int MjpegEncoder::hw_init(const StreamInfo &info)
{
    if (hw_initialized_) return 0;

    fd_m2m_ = open(M2M_DEVICE, O_RDWR, 0);
    if (fd_m2m_ < 0) {
        std::cerr << "Failed to open " << M2M_DEVICE << ": " << std::strerror(errno) << std::endl;
        return -1;
    }

    // 1. JPEG QUALITY SETTING (CRITICAL FOR NOISE REDUCTION)
    struct v4l2_control ctrl;
    ctrl.id = V4L2_CID_JPEG_COMPRESSION_QUALITY;
    ctrl.value = JPEG_QUALITY;
    if (ioctl(fd_m2m_, VIDIOC_S_CTRL, &ctrl) < 0) {
        std::cerr << "HW: Failed to set JPEG Quality to " << JPEG_QUALITY << " (Continuing anyway)" << std::endl;
    } else {
        std::cout << "HW: JPEG Quality set to " << JPEG_QUALITY << std::endl;
    }

    // 2. OUTPUT FORMAT (Input to Encoder: YUV420/NV12)
    struct v4l2_format fmt = {};
    fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    fmt.fmt.pix_mp.width = info.width;
    fmt.fmt.pix_mp.height = info.height;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_YUV420; 
    fmt.fmt.pix_mp.num_planes = 1;
    // CRITICAL FIX: Stride (bytesperline) for green frame issue
    fmt.fmt.pix_mp.plane_fmt[0].bytesperline = info.stride;
    fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;

    if (ioctl(fd_m2m_, VIDIOC_S_FMT, &fmt) < 0) {
        std::cerr << "HW: Failed to set output fmt: " << std::strerror(errno) << std::endl;
        return -1;
    }

    // 3. CAPTURE FORMAT (Output from Encoder: MJPEG)
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    fmt.fmt.pix_mp.width = info.width;
    fmt.fmt.pix_mp.height = info.height;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_MJPEG;
    fmt.fmt.pix_mp.num_planes = 1;
    fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
    
    if (ioctl(fd_m2m_, VIDIOC_S_FMT, &fmt) < 0) {
        std::cerr << "HW: Failed to set capture fmt: " << std::strerror(errno) << std::endl;
        return -1;
    }

    // 4. Request Buffers (OUTPUT - DMABUF)
    struct v4l2_requestbuffers req = {};
    req.count = 1;
    req.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    req.memory = V4L2_MEMORY_DMABUF;
    if (ioctl(fd_m2m_, VIDIOC_REQBUFS, &req) < 0) return -1;

    // 5. Request Buffers (CAPTURE - MMAP)
    req.count = CAPTURE_BUFFER_COUNT;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(fd_m2m_, VIDIOC_REQBUFS, &req) < 0) return -1;
    cap_buf_cnt_ = req.count;

    // 6. MMAP Capture Buffers
    cap_buffers_ = (struct v4l2_buffer *)calloc(cap_buf_cnt_, sizeof(*cap_buffers_));
    cap_mem_ = (void **)calloc(cap_buf_cnt_, sizeof(void *));

    for (int i = 0; i < cap_buf_cnt_; i++) {
        struct v4l2_buffer buf = {};
        struct v4l2_plane planes[1];
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        buf.length = 1;
        buf.m.planes = planes;
        
        ioctl(fd_m2m_, VIDIOC_QUERYBUF, &buf);
        cap_buffers_[i] = buf;
        cap_mem_[i] = mmap(NULL, buf.m.planes[0].length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_m2m_, buf.m.planes[0].m.mem_offset);
        
        // Queue immediately
        ioctl(fd_m2m_, VIDIOC_QBUF, &buf);
    }

    // 7. Stream ON
    int type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    ioctl(fd_m2m_, VIDIOC_STREAMON, &type);
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    ioctl(fd_m2m_, VIDIOC_STREAMON, &type);

    hw_initialized_ = true;
    return 0;
}

int MjpegEncoder::hw_process(EncodeItem &item, size_t &bytes_used)
{
    if (!hw_initialized_ && hw_init(item.info) < 0) return -1;

    // 1. Queue Output (Source YUV)
    struct v4l2_buffer buf = {};
    struct v4l2_plane planes[1];
    memset(&buf, 0, sizeof(buf));
    memset(planes, 0, sizeof(planes));

    buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    buf.memory = V4L2_MEMORY_DMABUF;
    buf.index = 0; 
    buf.length = 1;
    buf.m.planes = planes;
    
    // Doğru uzunluk ve FD ataması
    planes[0].bytesused = item.size; 
    planes[0].length = item.size;
    planes[0].m.fd = item.fd;
    planes[0].data_offset = 0;

    if (ioctl(fd_m2m_, VIDIOC_QBUF, &buf) < 0) {
        std::cerr << "HW: QBUF Output failed: " << std::strerror(errno) << std::endl;
        return -1;
    }

    // 2. Dequeue Capture (Encoded MJPEG) - Blocking wait
    struct v4l2_buffer cap_buf = {};
    struct v4l2_plane cap_planes[1];
    memset(&cap_buf, 0, sizeof(cap_buf));
    memset(cap_planes, 0, sizeof(cap_planes));

    cap_buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    cap_buf.memory = V4L2_MEMORY_MMAP;
    cap_buf.length = 1;
    cap_buf.m.planes = cap_planes;

    if (ioctl(fd_m2m_, VIDIOC_DQBUF, &cap_buf) < 0) {
        std::cerr << "HW: DQBUF Capture failed: " << std::strerror(errno) << std::endl;
        return -1;
    }

    // 3. Copy Data
    bytes_used = cap_buf.m.planes[0].bytesused;
    if (cap_buf.index < (unsigned)cap_buf_cnt_) {
        memcpy(item.dest, cap_mem_[cap_buf.index], bytes_used);
    }

    // 4. Re-Queue Capture Buffer
    ioctl(fd_m2m_, VIDIOC_QBUF, &cap_buf);

    // 5. Dequeue Output Buffer (Release Source)
    struct v4l2_buffer out_buf = {};
    struct v4l2_plane out_planes[1];
    out_buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    out_buf.memory = V4L2_MEMORY_DMABUF;
    out_buf.length = 1;
    out_buf.m.planes = out_planes;
    
    ioctl(fd_m2m_, VIDIOC_DQBUF, &out_buf);

    return 0;
}

void MjpegEncoder::encodeThread(int num)
{
    (void)num;
    EncodeItem encode_item;
    while (true) {
        {
            std::unique_lock<std::mutex> lock(encode_mutex_);
            while (encode_queue_.empty() && !abortEncode_) {
                encode_cond_var_.wait_for(lock, std::chrono::milliseconds(200));
            }
            if (abortEncode_ && encode_queue_.empty()) return;
            
            encode_item = encode_queue_.front();
            encode_queue_.pop();
        }

        size_t bytes_used = 0;
        if (hw_process(encode_item, bytes_used) < 0) {
            bytes_used = 0; // Hata durumunda 0 byte gönder
        }

        OutputItem output_item = { 
            encode_item.dest, 
            bytes_used, 
            encode_item.timestamp_us, 
            encode_item.cookie, 
            encode_item.index 
        };
        
        std::lock_guard<std::mutex> lock(output_mutex_);
        output_queue_.push(output_item);
        output_cond_var_.notify_one();
    }
}

void MjpegEncoder::outputThread()
{
    OutputItem item = {}; // Düzeltilmiş: Değişken başlatıldı
    while (true)
    {
        {
            std::unique_lock<std::mutex> lock(output_mutex_);
            bool found = false;
            
            while (!found && !abortOutput_) {
                for (int i = 0; i < NUM_ENC_THREADS; i++) {
                    if (!output_queue_[i].empty()) {
                        item = output_queue_[i].front();
                        output_queue_[i].pop();
                        found = true;
                        break;
                    }
                }
                if (!found)
                    output_cond_var_.wait_for(lock, std::chrono::milliseconds(200));
            }
            if (abortOutput_ && !found) return;
        }

        if (output_ready_callback_)
            output_ready_callback_(item.mem, item.bytes_used, item.timestamp_us, item.cookie);
    }
}

#else 

// ----------------------------------------------------------------------------
//                          SOFTWARE IMPLEMENTATION (CPU)
// ----------------------------------------------------------------------------

typedef unsigned long jpeg_mem_len_t;

void MjpegEncoder::encodeJPEG(struct jpeg_compress_struct &cinfo, EncodeItem &item,
                              uint8_t *&encoded_buffer, size_t &buffer_len)
{
    cinfo.image_width = item.info.width;
    cinfo.image_height = item.info.height;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_YCbCr;
    cinfo.restart_interval = 0;

    jpeg_set_defaults(&cinfo);
    cinfo.raw_data_in = TRUE;
    // SW modunda kaliteyi 50'de tutabiliriz veya artırabiliriz
    jpeg_set_quality(&cinfo, 50, TRUE);

    jpeg_mem_len_t jpeg_mem_len = buffer_len;
    jpeg_mem_dest(&cinfo, &encoded_buffer, &jpeg_mem_len);
    jpeg_start_compress(&cinfo, TRUE);

    int stride2 = item.info.stride / 2;
    uint8_t *Y = (uint8_t *)item.mem;
    uint8_t *U = (uint8_t *)Y + item.info.stride * item.info.height;
    uint8_t *V = (uint8_t *)U + stride2 * (item.info.height / 2);
    uint8_t *Y_max = U - item.info.stride;
    uint8_t *U_max = V - stride2;
    uint8_t *V_max = U_max + stride2 * (item.info.height / 2);

    JSAMPROW y_rows[16];
    JSAMPROW u_rows[8];
    JSAMPROW v_rows[8];

    for (uint8_t *Y_row = Y, *U_row = U, *V_row = V; cinfo.next_scanline < item.info.height;)
    {
        for (int i = 0; i < 16; i++, Y_row += item.info.stride)
            y_rows[i] = std::min(Y_row, Y_max);
        for (int i = 0; i < 8; i++, U_row += stride2, V_row += stride2) {
            u_rows[i] = std::min(U_row, U_max);
            v_rows[i] = std::min(V_row, V_max);
        }

        JSAMPARRAY rows[] = { y_rows, u_rows, v_rows };
        jpeg_write_raw_data(&cinfo, rows, 16);
    }

    jpeg_finish_compress(&cinfo);
    buffer_len = jpeg_mem_len;
}

void MjpegEncoder::encodeThread(int num)
{
    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;
    EncodeItem encode_item;

    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);

    while (true)
    {
        {
            std::unique_lock<std::mutex> lock(encode_mutex_);
            while (encode_queue_.empty() && !abortEncode_) {
                encode_cond_var_.wait_for(lock, std::chrono::milliseconds(200));
            }
            if (abortEncode_ && encode_queue_.empty()) {
                jpeg_destroy_compress(&cinfo);
                return;
            }
            encode_item = encode_queue_.front();
            encode_queue_.pop();
        }

        uint8_t *encoded_buffer = (uint8_t *)encode_item.dest;
        size_t buffer_len = encode_item.size;

        encodeJPEG(cinfo, encode_item, encoded_buffer, buffer_len);

        OutputItem output_item = {
            encoded_buffer,
            buffer_len,
            encode_item.timestamp_us,
            encode_item.cookie,
            encode_item.index
        };
        std::lock_guard<std::mutex> lock(output_mutex_);
        output_queue_[num].push(output_item);
        output_cond_var_.notify_one();
    }
}

void MjpegEncoder::outputThread()
{
    OutputItem item = {}; // Başlatılmamış değişken düzeltmesi
    while (true)
    {
        {
            std::unique_lock<std::mutex> lock(output_mutex_);
            bool found = false;
            
            while (!found && !abortOutput_) {
                for (int i = 0; i < NUM_ENC_THREADS; i++) {
                    if (!output_queue_[i].empty()) {
                        item = output_queue_[i].front();
                        output_queue_[i].pop();
                        found = true;
                        break;
                    }
                }
                if (!found)
                    output_cond_var_.wait_for(lock, std::chrono::milliseconds(200));
            }
            if (abortOutput_ && !found) return;
        }

        if (output_ready_callback_)
            output_ready_callback_(item.mem, item.bytes_used, item.timestamp_us, item.cookie);
    }
}
#endif