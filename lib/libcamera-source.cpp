/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * libcamera source
 *
 * Copyright (C) 2022 Ideas on Board Oy
 *
 * Contact: Daniel Scally <dan.scally@ideasonboard.com>
 */

#include <errno.h>
#include <fcntl.h>
#include <iostream>
#include <memory.h>
#include <queue>
#include <stdlib.h>
#include <string>
#include <string.h>
#include <unistd.h>
#include <map>
#include <sys/mman.h>

#include <libcamera/libcamera.h>
#include <linux/videodev2.h>

#include "config.h"
#include "mjpeg_encoder.hpp"

extern "C" {
#include "events.h"
#include "libcamera-source.h"
#include "tools.h"
#include "video-buffers.h"
#include "still-source.h"
}

using namespace libcamera;
using namespace std::placeholders;

#define to_libcamera_source(s) container_of(s, struct libcamera_source, video_src)

typedef void(*still_capture_ready_t)(void *data, struct still_buffer *buffer);

struct libcamera_source {
	struct video_source video_src;
	struct still_source still_src;

	std::unique_ptr<CameraManager> cm;
	std::unique_ptr<CameraConfiguration> config;
	std::shared_ptr<Camera> camera;
	ControlList controls;

	/* Video Stream resources */
    struct {
		Stream *stream;
		FrameBufferAllocator *allocator;
		std::vector<std::unique_ptr<Request>> requests;
		std::queue<Request *> completed_requests;
		MjpegEncoder *encoder;
		std::unordered_map<FrameBuffer *, Span<uint8_t>> mapped_buffers_;
		struct video_buffer_set buffers;
	} video;

	/* Still Stream resources */
	struct {
		Stream *stream;
		FrameBufferAllocator *allocator;
		std::queue<Request *> completed_requests;
		std::unordered_map<FrameBuffer *, Span<uint8_t>> mapped_buffers_;
		bool capture_in_progress;
		still_capture_ready_t capture_ready_cb;
		void *capture_ready_data;
	} still;

	int pfds[2];

	void mapBuffer(const std::unique_ptr<FrameBuffer> &buffer, bool is_still);
	void requestComplete(Request *request);
	void outputReady(void *mem, size_t bytesused, int64_t timestamp, unsigned int cookie);
	int captureStill();

};

void libcamera_source::mapBuffer(const std::unique_ptr<FrameBuffer> &buffer, bool is_still)
{
	size_t buffer_size = 0;
	for (unsigned int i = 0; i < buffer->planes().size(); i++) {
		const FrameBuffer::Plane &plane = buffer->planes()[i];
		buffer_size += plane.length;

		if (i == buffer->planes().size() -1 ||
			plane.fd.get() != buffer->planes()[i + 1].fd.get()) {

				void *memory = mmap(NULL, buffer_size, PROT_READ | PROT_WRITE,
								 MAP_SHARED, plane.fd.get(), 0);
				
				if (memory == MAP_FAILED) {
					std::cerr << "mmap failed: " << strerror(errno) << std::endl;
					buffer_size = 0;
					continue;
				}

				Span<uint8_t> mapped_span(static_cast<uint8_t *>(memory), buffer_size);
				if (is_still)
					still.mapped_buffers_[buffer.get()] = mapped_span;
				else
					video.mapped_buffers_[buffer.get()] = mapped_span;
				
				buffer_size = 0;
			}
		}
}

void libcamera_source::requestComplete(Request *request)
{
	if (request->status() == Request::RequestCancelled) {
		delete request;
		return;
	}

	Stream *request_stream = request->buffers().begin()->first;

	if(request_stream == video.stream) {
		video.completed_requests.push(request);
		write(pfds[1], "v", 1);
	} else if (request_stream == still.stream) {
		still.completed_requests.push(request);
		write(pfds[1], "s", 1);
	} else {
		std::cerr << "requestComplete: unknown stream" << std::endl;
		delete request;
	}
};

void libcamera_source::outputReady(void *mem, size_t bytesused, int64_t timestamp, unsigned int cookie)
{
	struct video_buffer buffer;

	buffer.index = cookie;
	buffer.mem = mem;
	buffer.bytesused = bytesused;
	buffer.timestamp.tv_sec = timestamp / 1000000;
	buffer.timestamp.tv_usec = timestamp % 1000000;

	video_src.handler(video_src.handler_data, &video_src, &buffer);
}

int libcamera_source::captureStill()
{
	if (still.capture_in_progress || !still.stream || still.mapped_buffers_.empty()) {
        return -EBUSY;
    }

	std::unique_ptr<Request> request = camera->createRequest();
    if (!request) {
        return -ENOMEM;
    }

	
	FrameBuffer *buffer_to_use = still.mapped_buffers_.begin()->first;

    int ret = request->addBuffer(still.stream, buffer_to_use);
    if (ret < 0) {
        return ret;
    }

	still.capture_in_progress = true;

    ret = camera->queueRequest(request.release());
    if (ret < 0) {
        still.capture_in_progress = false;
        return ret;
    }
    return 0;
}

static void process_camera_events(void *d)
{
	struct libcamera_source *src = (struct libcamera_source *)d;

	char signal_char;

	read(src->pfds[0], &signal_char, 1);

	if(signal_char == 'v') {
		libcamera_source_video_process(src);
	} else if (signal_char == 's') {
		libcamera_source_still_process(src);
	}
}

static void libcamera_source_video_process(libcamera_source *src)
{
	if(src->video.completed_requests.empty())
		return;

	Request *request = src->video.completed_requests.front();
	src->video.completed_requests.pop();

	/*
	 * We have only a single request to process , so just pick the first
	 */
	FrameBuffer *fb = request->buffers().begin()->second;

	/*
	 * If we have an encoder, than rather than simplt detailing the buffer
	 * here and passing it back to the sink we need to queue it to the 
	 * encoder. The encoder will queue that buffer to the sink after 
	 * compression.
	 */
	if(src->video_src.type == VIDEO_SOURCE_ENCODED) {
		if (!src->video.encoder) { delete request; return; 	}

		int64_t timestamp_ns = fb->metadata().timestamp;
		StreamInfo info = src->video.encoder->getStreamInfo(src->video.stream);
		auto span_it = src->video.mapped_buffers_.find(fb);
		
		if(span_it == src->video.mapped_buffers_.end()) return;
		
		void *mem = span_it->second.data();
		size_t size = span_it->second.size();
		void *dest = src->video.buffers.buffers[request->cookie()].mem;

		src->video.encoder->EncodeBuffer(mem, dest, size, info, timestamp_ns / 1000, request->cookie());
		return;
	} 
	// No encoder, DMABUF
	struct video_buffer buffer;
	buffer.index = request->cookie();
	buffer.size = fb->planes()[0].length;
	buffer.mem = NULL; // DMABUF mode
	buffer.bytesused = fb->metadata().planes()[0].bytesused;
	buffer.timestamp.tv_usec = fb->metadata().timestamp;
	buffer.error = false;

	src->video_src.handler(src->video_src.handler_data, &src->video_src, &buffer);	
}

static void libcamera_source_still_process(libcamera_source *src)
{
	if(src->still.completed_requests.empty())
		return;

	Request *request = src->still.completed_requests.front();
	src->still.completed_requests.pop();

	/*
	 * We have only a single request to process , so just pick the first
	 */
	FrameBuffer *fb = request->buffers().begin()->second;

	auto span_it = src->still.mapped_buffers_.find(fb);
	if(span_it == src->still.mapped_buffers_.end()) {
		src->still.capture_in_progress = false;
		return;
	}

	struct still_buffer buffer;
    buffer.mem = span_it->second.data();
    buffer.size = span_it->second.size();
    buffer.bytesused = fb->metadata().planes()[0].bytesused;
    buffer.timestamp.tv_sec = fb->metadata().timestamp / 1000000;
    buffer.timestamp.tv_usec = fb->metadata().timestamp % 1000000;
    buffer.error = false;
	
	if(src->still.capture_ready_cb)
		src->still.capture_ready_cb(src->still.capture_ready_data, &buffer);
	src->still.capture_in_progress = false;
	delete request;
}

static void libcamera_source_destroy(struct video_source *s)
{
	struct libcamera_source *src = to_libcamera_source(s);

	if(!src) return;
	
	if(src->camera) {
		src->camera->requestCompleted.disconnect(src);
		src->camera->stop();
	}
	
	if (src->pfds[0] != -1)  close(src->pfds[0]);
	if (src->pfds[1] != -1)  close(src->pfds[1]);
	
	for (auto const [key, val] : src->video.mapped_buffers_) munmap(val.data(), val.size());
	for (auto const [key, val] : src->still.mapped_buffers_) munmap(val.data(), val.size());

	if(src->video.allocator) delete src->video.allocator;
	if(src->still.allocator) delete src->still.allocator;
	
	if(src->video.encoder) delete src->video.encoder;
	

	free(src->video.buffers.buffers);
	if(src->camera) src->camera->release();
	if (src->cm) src->cm->stop();
	delete src;
}

static int libcamera_source_video_set_format(struct video_source *s,
									struct v4l2_pix_format *fmt)
{
	struct libcamera_source *src = to_libcamera_source(s);
	StreamConfiguration &cfg = src->config->at(0);
	__u32 chosen_pixelformat = fmt->pixelformat;

	cfg.size.width = fmt->width;
	cfg.size.height = fmt->height;
	cfg.pixelFormat = PixelFormat(fmt->pixelformat);

    	
#ifdef CONFIG_CAN_ENCODE

    if (chosen_pixelformat == V4L2_PIX_FMT_MJPEG && cfg.pixelFormat.fourcc() != chosen_pixelformat) {

		std::cout << "MJPEG format not natively supported; encoding YUV420" << std::endl;
		if (!src->video.encoder) {
            src->video.encoder = new MjpegEncoder();
            src->video.encoder->SetOutputReadyCallback(
                std::bind(&libcamera_source::outputReady, src, _1, _2, _3, _4));
        }
        cfg.pixelFormat = PixelFormat(V4L2_PIX_FMT_YUV420);
		src->video_src.type = VIDEO_SOURCE_ENCODED;

	}
#endif

	if (src->config->validate() == CameraConfiguration::Invalid) {
        std::cerr << "Error: Final video configuration is invalid." << std::endl;
        return -EINVAL;
	}

    std::cout << "Validated video format to " << cfg.toString() << std::endl;


	/*
	 * No .configure() call at this stage because we need to pickup the 
	 * number of buffers to use later on so we'd need to call it then too
	*/

	fmt->pixelformat = (src->video_src.type == VIDEO_SOURCE_ENCODED) ? V4L2_PIX_FMT_MJPEG : cfg.pixelFormat.fourcc();
    fmt->width = cfg.size.width;
    fmt->height = cfg.size.height;
    fmt->field = V4L2_FIELD_ANY;
    fmt->sizeimage = fmt->width * fmt->height * 2;

	return 0;
}

static int libcamera_source_video_set_frame_rate(struct video_source *s, unsigned int fps)
{
	struct libcamera_source *src = to_libcamera_source(s);

	int64_t frame_time = 1000000 / fps;

	src->controls.set(controls::FrameDurationLimits,
					Span<const int64_t, 2>({frame_time, frame_time}));
	return 0;
} 

static int libcamera_source_video_export_buffers(struct video_source *s, struct video_buffer_set **bufs)
{
	struct libcamera_source *src = to_libcamera_source(s);
	const auto &buffers = src->video.allocator->buffers(src->video.stream);


	struct video_buffer_set *exported_set = video_buffer_set_new(buffers.size());
	if (!exported_set)
		return -ENOMEM;

	for (unsigned int i = 0; i < buffers.size(); i++) {
		exported_set->buffers[i].index = buffers[i]->planes()[0].length;
		exported_set->buffers[i].size = buffers[i]->planes()[0].fd.get();
	}

	*bufs = exported_set;
	return 0;
}

static int libcamera_source_video_import_buffers(struct video_source *s, 
						struct video_buffer_set *bufs)
{
	struct libcamera_source *src = to_libcamera_source(s);
	if(src->video_src.type != VIDEO_SOURCE_ENCODED) {
		return 0;
	}

	if(bufs->nbufs != src->video.buffers.nbufs) {
		return -EINVAL;
	}

	for (unsigned int i = 0; i < src->video.buffers.nbufs; i++) {
		src->video.buffers.buffers[i].mem = bufs->buffers[i].mem;
	}
	return 0;
}

static int libcamera_source_video_queue_buffer(struct video_source *s, struct video_buffer *buf)
{
	struct libcamera_source *src = to_libcamera_source(s);

	if(!src || !buf)
		return -EINVAL;
	
	for (auto &request : src->video.requests) {
		if (request->cookie() == buf->index) {
			request->reuse(Request::ReuseBuffers);

			int ret = src->camera->queueRequest(request.get());
			if (ret < 0) {
				std::cerr << "Failed to re-queue video request: "
						<< strerror(-ret) << std::endl;
				return ret;
			}

			break;


		}
	}
	return 0;
}

static int libcamera_source_free_buffers(struct video_source *s)
{
	struct libcamera_source *src = to_libcamera_source(s);

	if(!src)
		return 0;

	for (auto const& [key, val] : src->video.mapped_buffers_) {
		munmap(val.data(), val.size());
	}

	src->video.mapped_buffers_.clear();

	for (auto const& [key, val] : src->still.mapped_buffers_) {
		munmap(val.data(), val.size());
	}

	src->still.mapped_buffers_.clear();

	if(src->video.allocator) {
		delete src->video.allocator;
		src->video.allocator = nullptr;
	}
	if(src->still.allocator) {
		delete src->still.allocator;
		src->still.allocator = nullptr;
	}

	free(src->video.buffers.buffers);
	src->video.buffers.buffers = nullptr;
	src->video.buffers.nbufs = 0;

	return 0;
}

static int libcamera_source_stream_on(struct video_source *s) 
{
	struct libcamera_source *src = to_libcamera_source(s);
	int ret;

	const auto &video_buffers = src->video.allocator->buffers(src->video.stream);

	for (unsigned int i = 0; i < video_buffers.size(); i++) {
		std::unique_ptr<Request> request = src->camera->createRequest(i);
		if (!request) {
			std::cerr << "Failed to create request" << std::endl;
			return -ENOMEM;
		}

		ret = request->addBuffer(src->video.stream, video_buffers[i].get());
		
		if (ret < 0) {
			std::cerr << "Failed to add video buffer to request: " 
					<< strerror(-ret) << std::endl;
			return ret;
		}


		src->video.requests.push_back(std::move(request));

	}

	int ret = src->camera->start(&src->controls);
	if (ret < 0) {
		std::cerr << "Failed to start camera: " << strerror(-ret) << std::endl;
		return ret;
	}


	for (auto &request : src->video.requests) {
		ret = src->camera->queueRequest(request.get());
		if (ret < 0) {
			std::cerr << "Failed to queue video request: "
					<< strerror(-ret) << std::endl;
			src->camera->stop();
			return ret;
		}
	}

	return 0;
}

static int libcamera_source_stream_off(struct video_source *s)
{
	struct libcamera_source *src = to_libcamera_source(s);

	if(!src || !src->camera)
		return -EINVAL;

	src->camera->stop();

	src->video.requests.clear();

	while(!src->video.completed_requests.empty()) {
		Request *req = src->video.completed_requests.front();
		src->video.completed_requests.pop();
		delete req;
	}

	return 0;
}

static int libcamera_source_alloc_buffers(struct video_source *s, unsigned int nbufs)
{
	struct libcamera_source *src = to_libcamera_source(s);
	int ret;

	StreamConfiguration &videoConfig = src->config->at(0);
	videoConfig.bufferCount = nbufs;

	if(src->camera->configure(src->config.get()) < 0) {
		std::cerr << "Failed to configure camera" << std::endl;
		return -EINVAL;
	}

	src->video.stream = videoConfig.stream();

	src->video.allocator = new FrameBufferAllocator(src->camera);
	ret = src->video.allocator->allocate(src->video.stream);
	if (ret < 0) {
		std::cerr << "Failed to allocate video buffers: " << std::endl;
		delete src->video.allocator;
		src->video.allocator = nullptr;
		return ret;
	}

	const auto &video_buffers = src->video.allocator->buffers(src->video.stream);
	if (src->video_src.type == VIDEO_SOURCE_ENCODED) {
		for (const auto &buffer : video_buffers) {
			src->mapBuffer(buffer, false); // is_still = false
		}
	}

	src->video.buffers.nbufs = video_buffers.size();
	src->video.buffers.buffers = (struct video_buffer *)calloc(
		src->video.buffers.nbufs, sizeof(struct video_buffer));
	if (!src->video.buffers.buffers) {
		std::cerr << "Failed to allocate video buffer structures" << std::endl;
		return -ENOMEM;
	}

	for (unsigned int i = 0; i < video_buffers.size(); i++) {
		src->video.buffers.buffers[i].index = i;
		src->video.buffers.buffers[i].dmabuf = -1;
	}

	if(src->still.stream) {
		src->still.allocator = new FrameBufferAllocator(src->camera);
		ret = src->still.allocator->allocate(src->still.stream);
		if (ret < 0) {
			std::cerr << "Failed to allocate still buffers: " << std::endl;
			delete src->still.allocator;
			src->still.allocator = nullptr;
			return ret;
		}


		const auto &still_buffers = src->still.allocator->buffers(src->still.stream);
		for (const auto &buffer : still_buffers) {
			src->mapBuffer(buffer, true); // is_still = true
		}
	}

	return 0;
}


static const struct video_source_ops libcamera_source_video_ops = {
	.destroy 		= libcamera_source_destroy,
	.set_format 	= libcamera_source_video_set_format,
	.set_frame_rate = libcamera_source_video_set_frame_rate,
	.alloc_buffers 	= libcamera_source_alloc_buffers,
	.export_buffers = libcamera_source_video_export_buffers,
	.import_buffers = libcamera_source_video_import_buffers,
	.free_buffers 	= libcamera_source_free_buffers,
	.stream_on 		= libcamera_source_stream_on,
	.stream_off 	= libcamera_source_stream_off,
	.queue_buffer 	= libcamera_source_video_queue_buffer,
	.fill_buffer 	= NULL,
};

static int still_source_capture_wrapper(struct still_source *ssrc) {
	return container_of(
		ssrc,
		libcamera_source,
		still_src
	)->captureStill();
};

static const struct still_source_ops libcamera_source_still_ops = {
	.capture = still_source_capture_wrapper,
	.destroy = nullptr,
	.set_format = nullptr,
	.alloc_buffer = nullptr,
	.free_buffer = nullptr,
	.get_buffer = nullptr,
};

struct video_source *libcamera_source_create(const char *devname)
{
	if (!devname) return nullptr;

	auto *src = new libcamera_source();

	if (pipe2(src->pfds, O_NONBLOCK) < 0) {
		std::cerr << "Failed to create pipe: " << strerror(errno) << std::endl;
		delete src;
		return nullptr;
	}

	src->video_src.ops = &libcamera_source_video_ops;
	src->video_src.type = VIDEO_SOURCE_DMABUF;
	src->still_src.ops = &libcamera_source_still_ops;

	src->cm = std::make_unique<CameraManager>();
	int ret = src->cm->start();

	if (ret < 0 || src->cm->cameras().empty()) {
		delete src;
		return nullptr;
	}

	if (std::isdigit(devname[0])) {
		unsigned long index = std::atoi(devname);

		if (index >= src->cm->cameras().size()) {
			std::cerr << "Camera index out of range" << std::endl;
			delete src;
			return nullptr;
		}

		src->camera = src->cm->cameras()[index];
		if (!src->camera) {
			std::cerr << "Failed to get camera at index " << index << std::endl;
			delete src;
			return nullptr;
		}
	} else {
		src->camera = src->cm->get(devname);
		if (!src->camera) {
			std::cerr << "Camera '" << devname << "' not found" << std::endl;
			std::cerr << "Available cameras:" << std::endl;
			for (const auto &cam : src->cm->cameras()) {
				std::cerr << "  - " << cam->id() << std::endl;
			}
			delete src;
			return nullptr;
		}
	}

	ret = src->camera->acquire();
	if (ret < 0) {
		std::cerr << "Failed to acquire camera" << std::endl;
		delete src;
		return nullptr;
	}

	src->config = src->camera->generateConfiguration(
		{StreamRole::VideoRecording, StreamRole::StillCapture});
	
	if (src->config) {
		StreamConfiguration &stillConfig = src->config->at(1);
		stillConfig.pixelFormat = PixelFormat(V4L2_PIX_FMT_SRGGB12);
		stillConfig.size.width = 4056;
		stillConfig.size.height = 3040;
		stillConfig.bufferCount = 1;
		
		if (src->config->validate() == CameraConfiguration::Invalid) {
			std::cout << "Dual stream config invalid, falling back to video only" << std::endl;
			src->config = nullptr;
		} else {
			src->still.stream = stillConfig.stream();
		}
	}
	
	if (!src->config) {
		src->config = src->camera->generateConfiguration(
			{StreamRole::VideoRecording});
		if (!src->config) {
			std::cerr << "Failed to generate camera configuration" << std::endl;
			src->camera->release();
			delete src;
			return nullptr;
		}
	}

	src->camera->requestCompleted.connect(src, &libcamera_source::requestComplete);

	const ControlInfoMap &infoMap = src->camera->controls();
	if (infoMap.find(&controls::AfMode) != infoMap.end()) {
		src->controls.set(controls::AfMode, controls::AfModeContinuous);
	}

	return &src->video_src;
}

void libcamera_source_init(struct video_source *s, struct events *events)
{
	struct libcamera_source *src = to_libcamera_source(s);

	src->video_src.events = events;
	events_watch_fd(events, src->pfds[0], EVENT_READ, 
		process_camera_events, src);
}

extern "C" struct still_source *libcamera_get_still_source(struct video_source *s) {
	return &to_libcamera_source(s)->still_src;
}

extern "C" void libcamera_still_source_set_callback(struct still_source *ssrc, still_capture_ready_t cb, void *data) {
	auto *src = container_of(ssrc, libcamera_source, still_src);
	src->still.capture_ready_cb = cb;
	src->still.capture_ready_data = data;
}