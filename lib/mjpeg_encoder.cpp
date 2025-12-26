/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Copyright (C) 2020, Raspberry Pi (Trading) Ltd.
 *
 * mjpeg_encoder.cpp - mjpeg video encoder.
 */

#include <chrono>
#include <iostream>
#include <pthread.h>

#include <libcamera/libcamera.h>
#include "mjpeg_encoder.hpp"

// ----------------------------------------------------------------------------
//                              COMMON CODE
// ----------------------------------------------------------------------------

MjpegEncoder::MjpegEncoder()
	: abortEncode_(false), abortOutput_(false), index_(0)
{
	output_thread_ = std::thread(&MjpegEncoder::outputThread, this);

#ifdef ENABLE_HW_MJPEG
	// HW IMPLEMENTATION INIT
	fd_m2m_ = -1;
	hw_initialized_ = false;
	cap_buffers_ = nullptr;
	cap_mem_ = nullptr;
	cap_buf_cnt_ = 0;
	// HW modunda tek thread yeterli
	encode_thread_ = std::thread(std::bind(&MjpegEncoder::encodeThread, this, 0));
	std::cout << "[MjpegEncoder] Using Hardware Encoding (/dev/video11)" << std::endl;
#else
	// CPU IMPLEMENTATION INIT
	for (int i = 0; i < NUM_ENC_THREADS; i++)
		encode_thread_[i] = std::thread(std::bind(&MjpegEncoder::encodeThread, this, i));
	std::cout << "[MjpegEncoder] Using Software Encoding (libjpeg)" << std::endl;
#endif
}

MjpegEncoder::~MjpegEncoder()
{
	abortEncode_ = true;
	
#ifdef ENABLE_HW_MJPEG
	if (encode_thread_.joinable()) encode_thread_.join();
	hw_uninit();
#else
	for (int i = 0; i < NUM_ENC_THREADS; i++)
		encode_thread_[i].join();
#endif

	abortOutput_ = true;
	output_thread_.join();
}

void MjpegEncoder::EncodeBuffer(void *mem, void *dest, unsigned int size,
				StreamInfo const &info, int64_t timestamp_us,
				unsigned int cookie, int fd)
{
	std::lock_guard<std::mutex> lock(encode_mutex_);
	// fd parametresini yapıya ekle (CPU modunda kullanılmasa bile)
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
#include <string.h>

#define M2M_DEVICE "/dev/video11"
#define CAPTURE_BUFFER_COUNT 4

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
		}
		if (cap_buffers_) free(cap_buffers_);
		close(fd_m2m_);
	}
}

int MjpegEncoder::hw_init(const StreamInfo &info)
{
	if (hw_initialized_) return 0;
	fd_m2m_ = open(M2M_DEVICE, O_RDWR, 0);
	if (fd_m2m_ < 0) return -1;

	struct v4l2_format fmt = {};
	fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	fmt.fmt.pix_mp.width = info.width;
	fmt.fmt.pix_mp.height = info.height;
	fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_YUV420;
	fmt.fmt.pix_mp.num_planes = 1;
	ioctl(fd_m2m_, VIDIOC_S_FMT, &fmt);

	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_MJPEG;
	ioctl(fd_m2m_, VIDIOC_S_FMT, &fmt);

	struct v4l2_requestbuffers req = {};
	req.count = 1;
	req.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	req.memory = V4L2_MEMORY_DMABUF;
	ioctl(fd_m2m_, VIDIOC_REQBUFS, &req);

	req.count = CAPTURE_BUFFER_COUNT;
	req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	req.memory = V4L2_MEMORY_MMAP;
	ioctl(fd_m2m_, VIDIOC_REQBUFS, &req);
	cap_buf_cnt_ = req.count;

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
		ioctl(fd_m2m_, VIDIOC_QBUF, &buf);
	}

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

	struct v4l2_buffer buf = {};
	struct v4l2_plane planes[1];
	buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	buf.memory = V4L2_MEMORY_DMABUF;
	buf.length = 1;
	buf.m.planes = planes;
	planes[0].bytesused = item.size;
	planes[0].length = item.size;
	planes[0].m.fd = item.fd;
	ioctl(fd_m2m_, VIDIOC_QBUF, &buf);

	struct v4l2_buffer cap_buf = {};
	struct v4l2_plane cap_planes[1];
	cap_buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	cap_buf.memory = V4L2_MEMORY_MMAP;
	cap_buf.length = 1;
	cap_buf.m.planes = cap_planes;
	ioctl(fd_m2m_, VIDIOC_DQBUF, &cap_buf);

	bytes_used = cap_buf.m.planes[0].bytesused;
	memcpy(item.dest, cap_mem_[cap_buf.index], bytes_used);
	
	ioctl(fd_m2m_, VIDIOC_QBUF, &cap_buf);

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
	(void)num; // Unused in HW mode
	EncodeItem encode_item;
	while (true) {
		{
			std::unique_lock<std::mutex> lock(encode_mutex_);
			while (true) {
				using namespace std::chrono_literals;
				if (abortEncode_ && encode_queue_.empty()) return;
				if (!encode_queue_.empty()) {
					encode_item = encode_queue_.front();
					encode_queue_.pop();
					break;
				}
				encode_cond_var_.wait_for(lock, 200ms);
			}
		}

		size_t bytes_used = 0;
		hw_process(encode_item, bytes_used);

		OutputItem output_item = { encode_item.dest, bytes_used, encode_item.timestamp_us, encode_item.index, encode_item.cookie };
		std::lock_guard<std::mutex> lock(output_mutex_);
		output_queue_.push(output_item);
		output_cond_var_.notify_one();
	}
}

void MjpegEncoder::outputThread()
{
	OutputItem item;
	uint64_t index = 0;
	while (true) {
		{
			std::unique_lock<std::mutex> lock(output_mutex_);
			while (true) {
				using namespace std::chrono_literals;
				bool abort = abortOutput_ ? true : false;
				
				if (!output_queue_.empty() && output_queue_.front().index == index) {
					item = output_queue_.front();
					output_queue_.pop();
					goto got_item;
				}
				if (abort && output_queue_.empty()) return;
				output_cond_var_.wait_for(lock, 200ms);
			}
		}
	got_item:
		if (output_ready_callback_)
			output_ready_callback_(item.mem, item.bytes_used, item.timestamp_us, item.cookie);
		index++;
	}
}

#else 

// ----------------------------------------------------------------------------
//                          SOFTWARE IMPLEMENTATION (CPU)
// ----------------------------------------------------------------------------

#if JPEG_LIB_VERSION_MAJOR > 9 || (JPEG_LIB_VERSION_MAJOR == 9 && JPEG_LIB_VERSION_MINOR >= 4)
typedef size_t jpeg_mem_len_t;
#else
typedef unsigned long jpeg_mem_len_t;
#endif

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
			while (true)
			{
				using namespace std::chrono_literals;
				if (abortEncode_ && encode_queue_.empty())
				{
					jpeg_destroy_compress(&cinfo);
					return;
				}
				if (!encode_queue_.empty())
				{
					encode_item = encode_queue_.front();
					encode_queue_.pop();
					break;
				}
				else
					encode_cond_var_.wait_for(lock, 200ms);
			}
		}

		uint8_t *encoded_buffer = (uint8_t *)encode_item.dest;
		size_t buffer_len = encode_item.size;

		encodeJPEG(cinfo, encode_item, encoded_buffer, buffer_len);

		OutputItem output_item = {
			encoded_buffer,
			buffer_len,
			encode_item.timestamp_us,
			encode_item.index,
			encode_item.cookie
		};
		std::lock_guard<std::mutex> lock(output_mutex_);
		output_queue_[num].push(output_item);
		output_cond_var_.notify_one();
	}
}

void MjpegEncoder::outputThread()
{
	OutputItem item;
	uint64_t index = 0;
	while (true)
	{
		{
			std::unique_lock<std::mutex> lock(output_mutex_);
			while (true)
			{
				using namespace std::chrono_literals;
				bool abort = abortOutput_ ? true : false;
				for (auto &q : output_queue_)
				{
					if (abort && !q.empty()) abort = false;
					if (!q.empty() && q.front().index == index)
					{
						item = q.front();
						q.pop();
						goto got_item;
					}
				}
				if (abort) return;
				output_cond_var_.wait_for(lock, 200ms);
			}
		}
	got_item:
		output_ready_callback_(item.mem, item.bytes_used, item.timestamp_us, item.cookie);
		index++;
	}
}

#endif