/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Copyright (C) 2020, Raspberry Pi (Trading) Ltd.
 *
 * mjpeg_encoder.hpp - mjpeg video encoder.
 */

#pragma once
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include <libcamera/libcamera.h>

#ifndef ENABLE_HW_MJPEG
#include <jpeglib.h>
#else
#include <linux/videodev2.h>
#endif


struct StreamInfo
{
	StreamInfo() : width(0), height(0), stride(0) {}
	unsigned int width;
	unsigned int height;
	unsigned int stride;
	libcamera::PixelFormat pixel_format;
	std::optional<libcamera::ColorSpace> colour_space;
};

typedef std::function<void(void *mem, size_t bytes_used, int64_t timestamp_us, unsigned int cookie)> OutputReadyCallback;

class MjpegEncoder
{
public:
	MjpegEncoder();
	~MjpegEncoder();
	void SetOutputReadyCallback(OutputReadyCallback callback) { output_ready_callback_ = callback; }

	void EncodeBuffer(void *mem, void *dest, unsigned int size,
			  StreamInfo const &info, int64_t timestamp_us,
			  unsigned int cookie, int fd);
	StreamInfo getStreamInfo(libcamera::Stream *stream);

private:
	struct EncodeItem {
		void *mem;
		void *dest;
		unsigned int size;
		StreamInfo info;
		int64_t timestamp_us;
		unsigned int index;
		unsigned int cookie;
		int fd;
	};

	struct OutputItem {
		void *mem;
		size_t bytes_used;
		int64_t timestamp_us;
		unsigned int cookie;
		unsigned int index;
	};

	void encodeThread(int num);
	void outputThread();

	OutputReadyCallback output_ready_callback_;

	std::mutex encode_mutex_;
	std::condition_variable encode_cond_var_;
	std::queue<EncodeItem> encode_queue_;

	std::mutex output_mutex_;
	std::condition_variable output_cond_var_;


#ifdef ENABLE_HW_MJPEG
	std::queue<OutputItem> output_queue_;
	std::thread encode_thread_;
#else
	static const int NUM_ENC_THREADS = 4;
	std::thread encode_thread_[NUM_ENC_THREADS];
	std::queue<OutputItem> output_queue_[NUM_ENC_THREADS];	
#endif

	std::thread output_thread_;
	std::atomic<bool> abortEncode_;
	std::atomic<bool> abortOutput_;
	unsigned int index_;

#ifdef ENABLE_HW_MJPEG
	// --- Hardware Specific Members ---
	int fd_m2m_;
	bool hw_initialized_;
	struct v4l2_buffer *cap_buffers_;
	void **cap_mem_;
	int cap_buf_cnt_;

	int hw_init(const StreamInfo &info);
	void hw_uninit();
	int hw_process(EncodeItem &item, size_t &bytes_used);
#else
	// --- Software Specific Members ---
	void encodeJPEG(struct jpeg_compress_struct &cinfo, EncodeItem &item,
			uint8_t *&encoded_buffer, size_t &buffer_len);
#endif
};