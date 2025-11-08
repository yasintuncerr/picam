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
#include "video-source.h"
}
#include "dng-writer.h"


using namespace libcamera;
using namespace std::placeholders;

#define to_libcamera_source(ptr, member) container_of(ptr, struct libcamera_source, member)
	
typedef void(*still_capture_ready_t)(void *data, struct still_buffer *buffer);


/*
 * libcamera source structure
 */
struct libcamera_source {
	struct video_source video_src;
#ifdef STILL_CAPTURE
	struct still_source still_src;
#endif
	/* Common resources */
	std::unique_ptr<CameraManager> cm;
	std::unique_ptr<CameraConfiguration> config;
	std::shared_ptr<Camera> camera;
	ControlList controls;
	
	int pfds[2];
	
	/* Video Stream resources */
	struct {
		FrameBufferAllocator *allocator;
		std::vector<std::unique_ptr<Request>> requests;
		std::queue<Request *> completed_requests;
		MjpegEncoder *encoder;
		std::unordered_map<FrameBuffer *, Span<uint8_t>> mapped_buffers_;
		struct video_buffer_set buffers;
		bool stream_on;
	} video;

#ifdef STILL_CAPTURE
	/* Still Stream resources */
	struct {
		FrameBufferAllocator *allocator;
		std::queue<Request *> completed_requests;
		std::unordered_map<FrameBuffer *, Span<uint8_t>> mapped_buffers_;
		bool capture_in_progress;
		still_capture_ready_t capture_ready_cb;
		void *capture_ready_data;
		std::unique_ptr<DNGWriter> dngWriter;
	} still;
#endif

	void mapBuffer(const std::unique_ptr<FrameBuffer> &buffer, bool is_still);
	void requestComplete(Request *request);
	void outputReady(void *mem, size_t bytesused, int64_t timestamp, unsigned int cookie);

#ifdef STILL_CAPTURE
	
	int captureStill(int64_t exposure_us, float gain);
	void DNGOutputReady(DngBufferPtr buffer);
	// DNG Output Ready Callback will be handled like output ready of MJPEG Encoder
#endif

};

/*--------------------------------------------------------------------------------
 * ----- Implementation of libcamera source Member Functions ---- *
 * -------------------------------------------------------------------------------
 */
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
		return;
	}

	bool is_still = request->findBuffer(config->at(1).stream()) != nullptr;
	bool is_video = request->findBuffer(config->at(0).stream()) != nullptr;
	if(is_video) {
		video.completed_requests.push(request);
		write(pfds[1], "v", 1);
	} else if (is_still) {
		still.completed_requests.push(request);
		write(pfds[1], "s", 1);
	} else {
		std::cerr << "requestComplete: unknown stream" << std::endl;
	}
}

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

#ifdef STILL_CAPTURE
int libcamera_source::captureStill(int64_t exposure_us, float gain)
{
	std::unique_ptr<Request> request = camera->createRequest();
	if (!request) {
		return -ENOMEM;
	}
	

	ControlList &ctrls = request->controls();
	if (exposure_us > 0) {
        ctrls.set(controls::AeEnable, false);
        ctrls.set(controls::ExposureTime, exposure_us);
        ctrls.set(controls::AnalogueGain, (gain > 0.0f) ? gain : 1.0f);

    } else if (gain > 0.0f) {
        ctrls.set(controls::AeEnable, true);
        ctrls.set(controls::AnalogueGain, gain);

    } else {
        ctrls.set(controls::AeEnable, true);
    }
	

	FrameBuffer *buffer_to_use = still.mapped_buffers_.begin()->first;
	Stream *stream = config->at(1).stream();
	int ret = request->addBuffer(stream, buffer_to_use);
	if (ret < 0) {
		return ret;
	}

	still.capture_in_progress = true;

	Request *raw_req = request.release();
	ret = camera->queueRequest(raw_req);
	if (ret < 0) {
		delete raw_req;
		still.capture_in_progress = false;
		return ret;
	}
	return 0;
}

void libcamera_source::DNGOutputReady(DngBufferPtr buffer)
{
    if (still.capture_ready_cb) {
        still.capture_ready_cb(still.capture_ready_data, buffer.get());
    }
    still.capture_in_progress = false;
}

#endif


/* --------------------------------------------------------------------------------
 * ----- Static Function Declarations ---- *
 * --------------------------------------------------------------------------------
*/

/* Forward declarations of libcamera source Static Functions */
static void libcamera_source_video_process(libcamera_source *src);
static void libcamera_source_still_process(libcamera_source *src);
static void process_camera_events(void *d);

/* Video Source Static Functions  Declarations */
static void libcamera_source_video_destroy(struct video_source *s);
static int libcamera_source_video_set_format(struct video_source *s, struct v4l2_pix_format *fmt);
static int libcamera_source_video_set_frame_rate(struct video_source *s, unsigned int fps);
static int libcamera_source_video_alloc_buffers(struct video_source *s, unsigned int nbufs);
static int libcamera_source_video_export_buffers(struct video_source *s, struct video_buffer_set **bufs);
static int libcamera_source_video_import_buffers(struct video_source *s, struct video_buffer_set *buffers);


/* Still Source Static Functions  Declarations */
#ifdef STILL_CAPTURE
static int libcamera_source_still_set_format(struct still_source *s, struct v4l2_pix_format *fmt);
static int libcamera_source_still_alloc_buffer(struct still_source *s);
static int libcamera_source_still_free_buffer(struct still_source *s);
static int libcamera_source_still_capture(struct still_source *s, int64_t exposure_us, float gain);
static int libcamera_source_still_capture_off(struct still_source *s);
#endif


/*--------------------------------------------------------------------------------
 * ----- Implementation of libcamera source Static Functions ---- *
 * -------------------------------------------------------------------------------
 */
static void process_camera_events(void *d)
{
	struct libcamera_source *src = (struct libcamera_source *)d;

	char signal_char;

	read(src->pfds[0], &signal_char, 1);

	if(signal_char == 'v') libcamera_source_video_process(src);

#ifdef STILL_CAPTURE
	else if (signal_char == 's') libcamera_source_still_process(src);
#endif
}

static void libcamera_source_video_process(libcamera_source *src)
{
	Stream *stream = src->config->at(0).stream();
	struct video_buffer buffer;
	Request *request;

	/*
	 * We need to perform a read here or the fd will stay active each time
	 * the event loop cycles.
	 */

	if (src->video.completed_requests.empty())
		return;

	request = src->video.completed_requests.front();
	src->video.completed_requests.pop();

	/* We have only a single buffer per request, so just pick the first */
	FrameBuffer *framebuf = request->buffers().begin()->second;

	/*
	 * If we have an encoder, then rather than simply detailing the buffer
	 * here and passing it back to the sink we need to queue it to the
	 * encoder. The encoder will queue that buffer to the sink after
	 * compression.
	 */
	if (src->video_src.type == VIDEO_SOURCE_ENCODED) {
		int64_t timestamp_ns = framebuf->metadata().timestamp;
		StreamInfo info = src->video.encoder->getStreamInfo(stream);
		auto span = src->video.mapped_buffers_.find(framebuf);
		void *mem = span->second.data();
		void *dest = src->video.buffers.buffers[request->cookie()].mem;
		unsigned int size = span->second.size();

		src->video.encoder->EncodeBuffer(mem, dest, size, info, timestamp_ns / 1000, request->cookie());

		return;
	}

	buffer.index = request->cookie();

	/* TODO: Correct this for formats libcamera treats as multiplanar */
	buffer.size = framebuf->planes()[0].length;
	buffer.mem = NULL;
	buffer.bytesused = framebuf->metadata().planes()[0].bytesused;
	buffer.timestamp.tv_usec = framebuf->metadata().timestamp;
	buffer.error = false;

	src->video_src.handler(src->video_src.handler_data, &src->video_src, &buffer);
}

#ifdef STILL_CAPTURE
static void libcamera_source_still_process(libcamera_source *src)
{
    if (src->still.completed_requests.empty())
        return;

    Request *request = src->still.completed_requests.front();
    src->still.completed_requests.pop();

    Stream *stream = src->config->at(1).stream();
    FrameBuffer *frameBuffer = request->buffers().at(stream);
    
    auto span_it = src->still.mapped_buffers_.find(frameBuffer);
    if (span_it == src->still.mapped_buffers_.end()) {
        std::cerr << "Could not find mapped buffer for DNG writing" << std::endl;
        // Hata durumunda C-stili callback'i yine de çağırmak önemlidir.
        struct still_buffer buffer = {};
        buffer.error = true;
        if (src->still.capture_ready_cb)
            src->still.capture_ready_cb(src->still.capture_ready_data, &buffer);
            
        delete request;
        src->still.capture_in_progress = false;
        return;
    }


    const StreamConfiguration &config = stream->configuration();
    const ControlList &metadata = request->metadata();
    void *data = span_it->second.data();
    
	std::thread([src, camera = src->camera, config, metadata, frameBuffer, data, request]() {
		src->still.dngWriter->writeToBuffer(camera.get(), config, metadata, frameBuffer, data);
		delete request;
	}).detach();
}

#endif

/* --------------------------------------------------------------------------------
 * ----- Video Source Static Functions ---- *
 * -------------------------------------------------------------------------------
 */
static void libcamera_source_video_destroy(struct video_source *s)
{
	struct libcamera_source *src = to_libcamera_source(s, video_src);

	src->camera->requestCompleted.disconnect(src);

	/* Closing the event notification file descriptors */
	close(src->pfds[0]);
	close(src->pfds[1]);

	src->camera->release();
	src->camera.reset();
	src->cm->stop();
	delete src;
}

static int libcamera_source_video_set_format(struct video_source *s,
				       struct v4l2_pix_format *fmt)
{
	struct libcamera_source *src = to_libcamera_source(s, video_src);
	StreamConfiguration &streamConfig = src->config->at(0);
	__u32 chosen_pixelformat = fmt->pixelformat;

	streamConfig.size.width = fmt->width;
	streamConfig.size.height = fmt->height;
	streamConfig.pixelFormat = PixelFormat(chosen_pixelformat);

	src->config->validate();

#ifdef CONFIG_CAN_ENCODE
	/*
	 * If the user requests MJPEG but the camera can't supply it, try again
	 * with YUV420 and initialise an MjpegEncoder to compress the data.
	 */
	if (chosen_pixelformat == V4L2_PIX_FMT_MJPEG &&
	    streamConfig.pixelFormat.fourcc() != chosen_pixelformat) {
		std::cout << "MJPEG format not natively supported; encoding YUV420" << std::endl;

		src->video.encoder = new MjpegEncoder();
		src->video.encoder->SetOutputReadyCallback(std::bind(&libcamera_source::outputReady, src, _1, _2, _3, _4));

		streamConfig.pixelFormat = PixelFormat(V4L2_PIX_FMT_YUV420);
		src->video_src.type = VIDEO_SOURCE_ENCODED;

		src->config->validate();
	}
#endif

	if (fmt->pixelformat != streamConfig.pixelFormat.fourcc())
		std::cerr << "Warning: set_format: Requested format unavailable" << std::endl;

	std::cout << "setting format to " << streamConfig.toString() << std::endl;

	/*
	 * No .configure() call at this stage, because we need to pick up the
	 * number of buffers to use later on so we'd need to call it then too.
	 */

	fmt->width = streamConfig.size.width;
	fmt->height = streamConfig.size.height;
	fmt->pixelformat = src->video.encoder ? V4L2_PIX_FMT_MJPEG : streamConfig.pixelFormat.fourcc();
	fmt->field = V4L2_FIELD_ANY;

	/* TODO: Can we use libcamera helpers to get image size / stride? */
	fmt->sizeimage = fmt->width * fmt->height * 2;


#ifdef STILL_CAPTURE
	v4l2_pix_format still_fmt;
	still_fmt.width = 4056;
	still_fmt.height = 3040;
	still_fmt.pixelformat = V4L2_PIX_FMT_SRGGB12;
	libcamera_source_still_set_format(&src->still_src, &still_fmt);
#endif
	return 0;
}

static int libcamera_source_video_set_frame_rate(struct video_source *s, unsigned int fps)
{
	struct libcamera_source *src = to_libcamera_source(s, video_src);
	int64_t frame_time = 1000000 / fps;

	src->controls.set(controls::FrameDurationLimits,
			  Span<const int64_t, 2>({ frame_time, frame_time }));

	return 0;
}

static int libcamera_source_video_export_buffers(struct video_source *s,
					   struct video_buffer_set **bufs)
{
	struct libcamera_source *src = to_libcamera_source(s, video_src);
	Stream *stream = src->config->at(0).stream();
	const std::vector<std::unique_ptr<FrameBuffer>> &buffers = src->video.allocator->buffers(stream);
	struct video_buffer_set *vid_buf_set;
	unsigned int i;

	for (i = 0; i < buffers.size(); i++) {
		const std::unique_ptr<FrameBuffer> &buffer = buffers[i];

		src->video.buffers.buffers[i].size = buffer->planes()[0].length;
		src->video.buffers.buffers[i].dmabuf = buffer->planes()[0].fd.get();
	}

	vid_buf_set = video_buffer_set_new(buffers.size());
	if (!vid_buf_set)
		return -ENOMEM;

	for (i = 0; i < src->video.buffers.nbufs; ++i) {
		struct video_buffer *buffer = &src->video.buffers.buffers[i];

		vid_buf_set->buffers[i].size = buffer->size;
		vid_buf_set->buffers[i].dmabuf = buffer->dmabuf;
	}

	*bufs = vid_buf_set;

	return 0;
}

static int libcamera_source_video_import_buffers(struct video_source *s,
					   struct video_buffer_set *buffers)
{
	struct libcamera_source *src = to_libcamera_source(s, video_src);

	for (unsigned int i = 0; i < buffers->nbufs; i++)
		src->video.buffers.buffers[i].mem = buffers->buffers[i].mem;

	return 0;
}

static int libcamera_source_video_queue_buffer(struct video_source *s,
					 struct video_buffer *buf)
{
	struct libcamera_source *src = to_libcamera_source(s, video_src);

	for (std::unique_ptr<Request> &r : src->video.requests) {
		if (r->cookie() == buf->index) {
			r->reuse(Request::ReuseBuffers);
			src->camera->queueRequest(r.get());

			break;
		}
	}

	return 0;
}

static int libcamera_source_video_free_buffers(struct video_source *s)
{
	struct libcamera_source *src = to_libcamera_source(s, video_src);
	Stream *stream = src->config->at(0).stream();

	for (auto &[buf, span] : src->video.mapped_buffers_)
		munmap(span.data(), span.size());

	src->video.mapped_buffers_.clear();

	src->video.allocator->free(stream);
	delete src->video.allocator;
	src->video.allocator = nullptr;


#ifdef STILL_CAPTURE
	libcamera_source_still_free_buffer(&src->still_src);
#endif

	return 0;
}

static int libcamera_source_video_stream_on(struct video_source *s)
{
	struct libcamera_source *src = to_libcamera_source(s, video_src);
	Stream *stream = src->config->at(0).stream();
	int ret;

	const std::vector<std::unique_ptr<FrameBuffer>> &buffers = src->video.allocator->buffers(stream);

	for (unsigned int i = 0; i < buffers.size(); ++i) {
		std::unique_ptr<Request> request = src->camera->createRequest(i);
		if (!request) {
			std::cerr << "failed to create request" << std::endl;
			return -ENOMEM;
		}

		const std::unique_ptr<FrameBuffer> &buffer = buffers[i];
		ret = request->addBuffer(stream, buffer.get());
		if (ret < 0) {
			std::cerr << "failed to set buffer for request" << std::endl;
			return ret;
		}

		src->video.requests.push_back(std::move(request));
	}

	ret = src->camera->start(&src->controls);
	if (ret) {
		std::cerr << "failed to start camera" << std::endl;
		return ret;
	}

	for (std::unique_ptr<Request> &request : src->video.requests) {
		ret = src->camera->queueRequest(request.get());
		if (ret) {
			std::cerr << "failed to queue request" << std::endl;
			src->camera->stop();
			return ret;
		}
	}

	/*
	 * Given our event handling code is designed for V4L2 file descriptors
	 * and lacks a way to trigger an event manually, we're using a pipe so
	 * that we can watch the read end and write to the other end when
	 * requestComplete() is ran.
	 */
	events_watch_fd(src->video_src.events, src->pfds[0], EVENT_READ,
			process_camera_events, src);

	src->video.stream_on = true;
	return 0;
}

static int libcamera_source_video_stream_off(struct video_source *s)
{
	struct libcamera_source *src = to_libcamera_source(s, video_src);

	src->camera->stop();
	events_unwatch_fd(src->video_src.events, src->pfds[0], EVENT_READ);
	src->video.requests.clear();

	while (!src->video.completed_requests.empty())
		src->video.completed_requests.pop();

	if (src->video_src.type == VIDEO_SOURCE_ENCODED) {
		delete src->video.encoder;
		src->video.encoder = nullptr;
	}

	/*
	 * We need to reinitialise this here, as if the user selected an
	 * unsupported MJPEG format the encoding routine will have overriden
	 * this setting.
	 */
	src->video_src.type = VIDEO_SOURCE_DMABUF;
	src->video.stream_on = false;
#ifdef STILL_CAPTURE
	libcamera_source_still_capture_off(&src->still_src);
#endif


	return 0;
}

static int libcamera_source_video_alloc_buffers(struct video_source *s, unsigned int nbufs)
{
	struct libcamera_source *src = to_libcamera_source(s, video_src);
	StreamConfiguration &streamConfig = src->config->at(0);
	int ret;

	streamConfig.bufferCount = nbufs;
	if (src->config->size() > 1) {
		StreamConfiguration &stillStreamConfig = src->config->at(1);
		stillStreamConfig.bufferCount = 1;
	}
	ret = src->camera->configure(src->config.get());
	if (ret) {
		std::cerr << "failed to configure the camera" << std::endl;
		return ret;
	}

	Stream *stream = src->config->at(0).stream();
	FrameBufferAllocator *allocator;

	allocator = new FrameBufferAllocator(src->camera);

	ret = allocator->allocate(stream);
	if (ret < 0) {
		std::cerr << "failed to allocate buffers" << std::endl;
		return ret;
	}

	src->video.allocator = allocator;

	const std::vector<std::unique_ptr<FrameBuffer>> &buffers = allocator->buffers(stream);
	src->video.buffers.nbufs = buffers.size();

	if (src->video_src.type == VIDEO_SOURCE_ENCODED) {
		for (const std::unique_ptr<FrameBuffer> &buffer : buffers)
			src->mapBuffer(buffer, false);
	}

	src->video.buffers.buffers = (video_buffer *)calloc(src->video.buffers.nbufs, sizeof(*src->video.buffers.buffers));
	if (!src->video.buffers.buffers) {
		std::cerr << "failed to allocate buffers" << std::endl;
		return -ENOMEM;
	}

	for (unsigned int i = 0; i < buffers.size(); ++i) {
		src->video.buffers.buffers[i].index = i;
		src->video.buffers.buffers[i].dmabuf = -1;
	}

#ifdef STILL_CAPTURE
	return libcamera_source_still_alloc_buffer(&src->still_src);
#endif

	return ret;
}

/* Video source operations */
static const struct video_source_ops libcamera_source_video_ops = {
	.destroy 		= libcamera_source_video_destroy,
	.set_format 	= libcamera_source_video_set_format,
	.set_frame_rate = libcamera_source_video_set_frame_rate,
	.alloc_buffers 	= libcamera_source_video_alloc_buffers,
	.export_buffers = libcamera_source_video_export_buffers,
	.import_buffers = libcamera_source_video_import_buffers,
	.free_buffers 	= libcamera_source_video_free_buffers,
	.stream_on 		= libcamera_source_video_stream_on,
	.stream_off 	= libcamera_source_video_stream_off,
	.queue_buffer 	= libcamera_source_video_queue_buffer,
	.fill_buffer 	= NULL,
};



/* --------------------------------------------------------------------------------
 * ----- Still Source Static Functions ---- *
 * -------------------------------------------------------------------------------
 */
#ifdef STILL_CAPTURE
static int libcamera_source_still_free_buffer(struct still_source *s)
{
    struct libcamera_source *src = to_libcamera_source(s, still_src);
    Stream *stream = src->config->at(1).stream();
    
    for (auto &[buf, span] : src->still.mapped_buffers_)
        munmap(span.data(), span.size());
    
    src->still.mapped_buffers_.clear();
    
    if (src->still.allocator) {
        src->still.allocator->free(stream);
        delete src->still.allocator;
        src->still.allocator = nullptr;
    }
    
    // dngWriter'ı bu şekilde güvenle temizle
    if (src->still.dngWriter) {
        src->still.dngWriter.reset();
    }
    
    return 0;
}

static int libcamera_source_still_set_format(struct still_source *s,
				       struct v4l2_pix_format *fmt)
{
	struct libcamera_source *src = to_libcamera_source(s, still_src);
	
	StreamConfiguration &streamConfig = src->config->at(1);
	__u32 chosen_pixelformat 	= fmt->pixelformat;
	streamConfig.size.width 	= fmt->width;
	streamConfig.size.height 	= fmt->height;
	streamConfig.pixelFormat 	= PixelFormat(chosen_pixelformat);
	
	src->config->validate();
	
	fmt->width = streamConfig.size.width;
	fmt->height = streamConfig.size.height;
	fmt->pixelformat = streamConfig.pixelFormat.fourcc();
	fmt->field = V4L2_FIELD_ANY;
	
	if (!src->still.dngWriter) {
		src->still.dngWriter = std::make_unique<DNGWriter>();
		src->still.dngWriter->setCallback(std::bind(&libcamera_source::DNGOutputReady, src, _1));
	}
	return 0;
}

static int libcamera_source_still_alloc_buffer(struct still_source *s)
{
	struct libcamera_source *src = to_libcamera_source(s, still_src);

	if(src->config->at(1).stream()) {
		src->still.allocator = new FrameBufferAllocator(src->camera);
		int ret = src->still.allocator->allocate(src->config->at(1).stream());
		
		if (ret < 0) {
			std::cerr << "Failed to allocate still buffers: " << std::endl;
			delete src->still.allocator;
			src->still.allocator = nullptr;
			return ret;
		}

		const auto &still_buffers = src->still.allocator->buffers(src->config->at(1).stream());
		
		for (const auto &buffer : still_buffers) {
			src->mapBuffer(buffer, true);
		}
	}
	
	return 0;
}

static int libcamera_source_still_capture_off(struct still_source *s)
{

	struct libcamera_source *src = to_libcamera_source(s, still_src);

	if (!src->video.stream_on)
		return -EBUSY;

	src->still.capture_in_progress = false;
	while (!src->still.completed_requests.empty())
		src->still.completed_requests.pop();
	
	return 0;
}

static int libcamera_source_still_capture(struct still_source *s, int64_t exposure_us, float gain)
{
	struct libcamera_source *src = to_libcamera_source(s, still_src);

	

	if (!src->video.stream_on)
		return -EBUSY;

	return src->captureStill(exposure_us, gain);
}

static const struct still_source_ops libcamera_source_still_ops = {
	.set_format 	= libcamera_source_still_set_format,
	.alloc_buffer 	= libcamera_source_still_alloc_buffer,
	.free_buffer 	= libcamera_source_still_free_buffer,
	.capture 		= (int (*)(still_source*, int64_t, float)) libcamera_source_still_capture,
	.capture_off	= libcamera_source_still_capture_off,
};

#endif
/* --------------------------------------------------------------------------------
 * ----- Public API Functions ---- *
 * -------------------------------------------------------------------------------
 */
/* Helper to generate a user-friendly camera name from its properties */
std::string cameraName(Camera *camera)
{
	const ControlList &props = camera->properties();
	std::string name;

	const auto &location = props.get(properties::Location);
	if (location) {
		switch (*location) {
		case properties::CameraLocationFront:
			name = "Internal Front Camera";
			break;
		case properties::CameraLocationBack:
			name = "Internal back camera";
			break;
		case properties::CameraLocationExternal:
			name = "External camera";
			const auto &model = props.get(properties::Model);
			if (model)
				name = *model;
			break;
		}
	}
	name += " (" + camera->id() + ")";

	return name;
}

struct video_source *libcamera_source_create(const char *devname)
{
	struct libcamera_source *src;
	int ret;

	if (!devname) {
		std::cerr << "No camera identifier was passed" << std::endl;
		return NULL;
	}

	src = new libcamera_source;


	/*
	 * Event handling in libuvcgadget currently depends on select(), but
	 * unlike a V4L2 devnode there's no file descriptor for completed
	 * libcamera Requests. We'll spoof the events using a pipe for now,
	 * but...
	 *
	 * TODO: Replace event handling with libevent
	 */

	ret = pipe2(src->pfds, O_NONBLOCK);
	if (ret) {
		std::cerr << "failed to create pipe" << std::endl;
		goto err_free_src;
	}

	src->video_src.ops = &libcamera_source_video_ops;
	src->video_src.type = VIDEO_SOURCE_DMABUF;

#ifdef STILL_CAPTURE
	src->still_src.ops = &libcamera_source_still_ops;
#endif
	src->cm = std::make_unique<CameraManager>();
	src->cm->start();

	if (src->cm->cameras().empty()) {
		std::cout << "No cameras were identified on the system" << std::endl;
		goto err_close_pipe;
	}

	/* TODO: make a separate way to list libcamera cameras */
	for (auto const &camera : src->cm->cameras())
		printf("- %s\n", cameraName(camera.get()).c_str());

	/*
	 * Camera selection is by ID or index. Camera ID's start with a slash.
	 * If the first character is a digit, assume we're indexing, otherwise
	 * treat it as an ID.
	 */
	if (std::isdigit(devname[0])) {
		unsigned long index = std::atoi(devname);

		if (index >= src->cm->cameras().size()) {
			std::cerr << "No camera at index " << index << std::endl;
			goto err_close_pipe;
		}

		src->camera = src->cm->cameras()[index];
	} else {
		src->camera = src->cm->get(std::string(devname));
		if (!src->camera) {
			std::cerr << "found no camera matching " << devname << std::endl;
			goto err_close_pipe;
		}
	}

	ret = src->camera->acquire();
	if (ret) {
		fprintf(stderr, "failed to acquire camera\n");
		goto err_close_pipe;
	}

	std::cout << "Using camera " << cameraName(src->camera.get()) << std::endl;
#ifdef STILL_CAPTURE
	src->config =
		src->camera->generateConfiguration( { StreamRole::VideoRecording, StreamRole::StillCapture });
#else
	src->config =
		src->camera->generateConfiguration( { StreamRole::VideoRecording });
#endif	
	
	if (!src->config) {	
		std::cerr << "failed to generate camera config" << std::endl;
		goto err_release_camera;
	}
	
	src->camera->requestCompleted.connect(src, &libcamera_source::requestComplete);

	{
		/*
		 * We enable AutoFocus by default if it's supported by the camera.
		 * Keep the infoMap scoped to calm the compiler worrying about
		 * jumping over the reference with the gotos.
		 */
		const ControlInfoMap &infoMap = src->camera->controls();
		if (infoMap.find(&controls::AfMode) != infoMap.end()) {
			std::cout << "Enabling continuous auto-focus" << std::endl;
			src->controls.set(controls::AfMode, controls::AfModeContinuous);
		}
		src->controls.set(controls::AwbEnable, true);
		

		std::cout << "Enabling Auto Exposure and Auto Gain" << std::endl;
		src->controls.set(controls::AeEnable, true);
	}

	return &src->video_src;

err_release_camera:
	src->camera->release();
err_close_pipe:
	close(src->pfds[0]);
	close(src->pfds[1]);
	src->cm->stop();
err_free_src:
	delete src;

	return NULL;
}

void libcamera_source_init(struct video_source *s, struct events *events)
{
	struct libcamera_source *src = to_libcamera_source(s, video_src);

	src->video_src.events = events;
}

extern "C" void libcamera_still_source_set_callback(struct still_source *ssrc, still_capture_ready_t cb, void *data) {
	auto *src = to_libcamera_source(ssrc, still_src);
	src->still.capture_ready_cb = cb;
	src->still.capture_ready_data = data;
};

struct still_source *libcamera_get_still_source(struct video_source *s)
{
	struct libcamera_source *src = to_libcamera_source(s, video_src);
	return &src->still_src;
}
