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
		FrameBufferAllocator *allocator;
		std::vector<std::unique_ptr<Request>> requests;
		std::queue<Request *> completed_requests;
		MjpegEncoder *encoder;
		std::unordered_map<FrameBuffer *, Span<uint8_t>> mapped_buffers_;
		struct video_buffer_set buffers;
		int64_t latest_exposure_time;
    	float latest_analogue_gain;
	} video;

	/* Still Stream resources */
	struct {
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
	bool streaming;
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
		return;
	}
	const ControlList &metadata = request->metadata();
	bool is_still = request->findBuffer(config->at(1).stream()) != nullptr;
	bool is_video = request->findBuffer(config->at(0).stream()) != nullptr;
	if(is_video) {
		auto exp = metadata.get(controls::ExposureTime);
        if (exp)
            this->video.latest_exposure_time = *exp;

        auto gain = metadata.get(controls::AnalogueGain);
        if (gain)
            this->video.latest_analogue_gain = *gain;
		video.completed_requests.push(request);
		write(pfds[1], "v", 1);
	} else if (is_still) {

		still.completed_requests.push(request);
		printf("Video Exposure: %lldus, Gain: %.2f\n",
			  (long long)this->video.latest_exposure_time,
			  this->video.latest_analogue_gain);
		auto exp = metadata.get(controls::ExposureTime);
		if (exp)
			printf("  Still Exposure: %lldus\n", (long long)*exp);

		auto gain = metadata.get(controls::AnalogueGain);
		if (gain)
			printf("  Still Gain: %.2f\n", *gain);

		
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

int libcamera_source::captureStill()
{
	//if (still.capture_in_progress || still.mapped_buffers_.empty()) {
	//	return -EBUSY;
	//}

	std::unique_ptr<Request> request = camera->createRequest();
	if (!request) {
		return -ENOMEM;
	}

	/*if (this->video.latest_exposure_time > 0 && this->video.latest_analogue_gain > 0.0f) {
        printf("Applying controls from running stream: Exposure %lldus, Gain %.2f\n",
       										(long long)this->video.latest_exposure_time,
       										this->video.latest_analogue_gain);

        request->controls().set(controls::ExposureTime, this->video.latest_exposure_time);
        request->controls().set(controls::AnalogueGain, this->video.latest_analogue_gain);
    } else {
       
	*/

	// 1. Manuel pozlama moduna geç, OTOMATİK BEYAZ DENGESİNİ AÇIK BIRAK.
    request->controls().set(controls::AeEnable, false);
    request->controls().set(controls::AwbEnable, true); // Renkler için AWB algoritması çalışsın.

    // 2. Yüksek pozlama, yüksek analog ve dijital kazanç değerleri belirle.
    int64_t manual_exposure = 500000; // 0.5 saniye
    float manual_analogue_gain = 8.0f;
    float manual_digital_gain = 2.0f; // Dijital kazancı da devreye sokuyoruz.

    // 3. Video akışının frame rate limitini bu tek kare için EZ.
    //    Maksimum 2 saniyeye kadar pozlamaya izin ver.
    libcamera::Span<const int64_t, 2> frame_duration_limits({0, 2000000});
    request->controls().set(controls::FrameDurationLimits, frame_duration_limits);

    // 4. Tüm manuel ayarlarımızı isteğe ekle.
    request->controls().set(controls::ExposureTime, manual_exposure);
    request->controls().set(controls::AnalogueGain, manual_analogue_gain);
    request->controls().set(controls::DigitalGain, manual_digital_gain);

    printf(">>> FINAL ATTEMPT: Manual Exposure/Gain + AWB + FrameDuration Override <<<\n");
    printf("  - Target Exposure: %ldus\n", manual_exposure);
    printf("  - Target AnalogueGain: %.2f\n", manual_analogue_gain);
    printf("  - Target DigitalGain: %.2f\n", manual_digital_gain);

    //}

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

static void libcamera_source_video_process(libcamera_source *src);
static void libcamera_source_still_process(libcamera_source *src);

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
	Stream *stream = src->config->at(0).stream();
	struct video_buffer buffer;
	Request *request;
	//char buf;

	/*
	 * We need to perform a read here or the fd will stay active each time
	 * the event loop cycles.
	 */
	//read(src->pfds[0], &buf, 1);

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


static void libcamera_source_still_process(libcamera_source *src)
{
    Stream *stream = src->config->at(1).stream();
    Request *request;

    if(src->still.completed_requests.empty())
        return;

    request = src->still.completed_requests.front();
    src->still.completed_requests.pop();

    FrameBuffer *fb = request->buffers().begin()->second;

    struct still_buffer buffer;
    buffer.size = fb->planes()[0].length;
    
    // Mapped buffer'dan gerçek memory pointer'ını al
    auto span = src->still.mapped_buffers_.find(fb);
    if (span != src->still.mapped_buffers_.end()) {
        buffer.mem = span->second.data();
    } else {
        buffer.mem = NULL;
    }
    
    buffer.bytesused = fb->metadata().planes()[0].bytesused;
    buffer.timestamp.tv_sec = fb->metadata().timestamp / 1000000;
    buffer.timestamp.tv_usec = fb->metadata().timestamp % 1000000;
    buffer.error = false;
    buffer.width = stream->configuration().size.width;
    buffer.height = stream->configuration().size.height;
    buffer.pixelformat = stream->configuration().pixelFormat.fourcc();

    if(src->still.capture_ready_cb)
        src->still.capture_ready_cb(src->still.capture_ready_data, &buffer);
    
    src->still.capture_in_progress = false;
    delete request; // Bu doğru - raw pointer olarak alındı
}

static void libcamera_source_destroy(struct video_source *s)
{
	struct libcamera_source *src = to_libcamera_source(s);

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
	struct libcamera_source *src = to_libcamera_source(s);
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

	if (src->config) {
		StreamConfiguration &stillConfig = src->config->at(1);
		stillConfig.pixelFormat = PixelFormat(V4L2_PIX_FMT_SRGGB12);
		stillConfig.size.width = 4056;
		stillConfig.size.height = 3040;
		stillConfig.bufferCount = 1;
		
		if (src->config->validate() == CameraConfiguration::Invalid) {
			std::cout << "Still Capture couldn't set" << std::endl;
		}
	}

	return 0;
}

static int libcamera_source_video_set_frame_rate(struct video_source *s, unsigned int fps)
{
	struct libcamera_source *src = to_libcamera_source(s);
	int64_t frame_time = 1000000 / fps;

	src->controls.set(controls::FrameDurationLimits,
			  Span<const int64_t, 2>({ frame_time, frame_time }));

	return 0;
}

static int libcamera_source_video_export_buffers(struct video_source *s,
					   struct video_buffer_set **bufs)
{
	struct libcamera_source *src = to_libcamera_source(s);
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
	struct libcamera_source *src = to_libcamera_source(s);

	for (unsigned int i = 0; i < buffers->nbufs; i++)
		src->video.buffers.buffers[i].mem = buffers->buffers[i].mem;

	return 0;
}

static int libcamera_source_video_queue_buffer(struct video_source *s,
					 struct video_buffer *buf)
{
	struct libcamera_source *src = to_libcamera_source(s);

	for (std::unique_ptr<Request> &r : src->video.requests) {
		if (r->cookie() == buf->index) {
			r->reuse(Request::ReuseBuffers);
			src->camera->queueRequest(r.get());

			break;
		}
	}

	return 0;
}

static int libcamera_source_free_buffers(struct video_source *s)
{
	struct libcamera_source *src = to_libcamera_source(s);
	Stream *stream = src->config->at(0).stream();

	for (auto &[buf, span] : src->video.mapped_buffers_)
		munmap(span.data(), span.size());

	src->video.mapped_buffers_.clear();

	src->video.allocator->free(stream);
	delete src->video.allocator;
	src->video.allocator = nullptr;

	stream = src->config->at(1).stream();
	if (stream) {
		for (auto &[buf, span] : src->still.mapped_buffers_)
			munmap(span.data(), span.size());
		
		src->still.mapped_buffers_.clear();

		src->still.allocator->free(stream);
		delete src->still.allocator;
		src->still.allocator = nullptr;
	}

	return 0;
}


static int libcamera_source_stream_on(struct video_source *s)
{
	struct libcamera_source *src = to_libcamera_source(s);
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

	return 0;
}


static int libcamera_source_stream_off(struct video_source *s)
{
	struct libcamera_source *src = to_libcamera_source(s);

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

	return 0;
}



static int libcamera_source_alloc_buffers(struct video_source *s, unsigned int nbufs)
{
	struct libcamera_source *src = to_libcamera_source(s);
	StreamConfiguration &streamConfig = src->config->at(0);
	int ret;

	streamConfig.bufferCount = nbufs;
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


	if(src->config->at(1).stream()) {
		src->still.allocator = new FrameBufferAllocator(src->camera);
		ret = src->still.allocator->allocate(src->config->at(1).stream());
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


	return ret;
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
	libcamera_source *src = container_of(ssrc, libcamera_source, still_src);
	return src->captureStill();
}

static const struct still_source_ops libcamera_source_still_ops = {
	.destroy = nullptr,
	.set_format = nullptr,
	.alloc_buffer = nullptr,
	.free_buffer = nullptr,
	.capture = still_source_capture_wrapper,
	.get_buffer = nullptr,
};

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


	src->video.latest_exposure_time = 0;
    src->video.latest_analogue_gain = 0.0f;

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

	src->still_src.ops = &libcamera_source_still_ops;

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

	src->config =
		src->camera->generateConfiguration( { StreamRole::VideoRecording, StreamRole::StillCapture });
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
	struct libcamera_source *src = to_libcamera_source(s);

	src->video_src.events = events;
}

extern "C" struct still_source *libcamera_get_still_source(struct video_source *s) {
	struct libcamera_source *src = to_libcamera_source(s);
	return &src->still_src;
}

extern "C" void libcamera_still_source_set_callback(struct still_source *ssrc, still_capture_ready_t cb, void *data) {
	auto *src = container_of(ssrc, libcamera_source, still_src);
	src->still.capture_ready_cb = cb;
	src->still.capture_ready_data = data;
};