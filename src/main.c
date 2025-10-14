/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * UVC gadget test application
 *
 * Copyright (C) 2010-2018 Laurent Pinchart
 *
 * Contact: Laurent Pinchart <laurent.pinchart@ideasonboard.com>
 */

#include <signal.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>

#include "config.h"
#include "configfs.h"
#include "events.h"
#include "stream.h"
#include "libcamera-source.h"
#include "v4l2-source.h"
#include "test-source.h"
#include "jpg-source.h"
#include "slideshow-source.h"
#include "capture.h"

static void usage(const char *argv0)
{
	fprintf(stderr, "Usage: %s [options] <uvc device>\n", argv0);
	fprintf(stderr, "Available options are\n");
#ifdef HAVE_LIBCAMERA
	fprintf(stderr, " -c <index|id> libcamera camera name\n");
#endif
	fprintf(stderr, " -d device	V4L2 source device\n");
	fprintf(stderr, " -i image	MJPEG image\n");
	fprintf(stderr, " -s directory	directory of slideshow images\n");
	fprintf(stderr, " -p port	HTTP capture server port (default: disabled)\n");
	fprintf(stderr, " -h		Print this help screen and exit\n");
	fprintf(stderr, "\n");
	fprintf(stderr, " <uvc device>	UVC device instance specifier\n");
	fprintf(stderr, "\n");

	fprintf(stderr, "  For ConfigFS devices the <uvc device> parameter can take the form of a shortened\n");
	fprintf(stderr, "  function specifier such as: 'uvc.0', or if multiple gadgets are configured, the\n");
	fprintf(stderr, "  gadget name should be included to prevent ambiguity: 'g1/functions/uvc.0'.\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "  For legacy g_webcam UVC instances, this parameter will identify the UDC that the\n");
	fprintf(stderr, "  UVC function is bound to.\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "  The parameter is optional, and if not provided the first UVC function on the first\n");
	fprintf(stderr, "  gadget identified will be used.\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "  HTTP Capture Server:\n");
	fprintf(stderr, "  When -p port is specified, an HTTP server will be started on the given port.\n");
	fprintf(stderr, "  Currently only libcamera sources support still image capture via HTTP.\n");
	fprintf(stderr, "  Usage: curl http://<ip>:<port>/capture > image.raw\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "Example usage:\n");
	fprintf(stderr, "    %s uvc.1\n", argv0);
	fprintf(stderr, "    %s g1/functions/uvc.1\n", argv0);
	fprintf(stderr, "    %s -c 0 -p 8080 uvc.0  # libcamera with HTTP capture on port 8080\n", argv0);
	fprintf(stderr, "\n");
	fprintf(stderr, "    %s musb-hdrc.0.auto\n", argv0);
}


/* Necessary for and only used by signal handler. */
static struct events *sigint_events;

static void sigint_handler(int signal __attribute__((unused)))
{
	/* Stop the main loop when the user presses CTRL-C */
	events_stop(sigint_events);
}

int main(int argc, char *argv[])
{
	char *function = NULL;
#ifdef HAVE_LIBCAMERA
	char *camera = NULL;
#endif
	char *cap_device = NULL;
	char *img_path = NULL;
	char *slideshow_dir = NULL;
	int http_port = 0;
	struct uvc_function_config *fc;
	struct uvc_stream *stream = NULL;
	struct video_source *src = NULL;
	struct http_server *http_server = NULL;
	struct events events;
	int ret = 0;
	int opt;

	while ((opt = getopt(argc, argv, "c:d:i:s:p:h")) != -1) {
		switch (opt) {
#ifdef HAVE_LIBCAMERA
		case 'c':
			camera = optarg;
			break;
#endif
		case 'd':
			cap_device = optarg;
			break;

		case 'i':
			img_path = optarg;
			break;

		case 's':
			slideshow_dir = optarg;
			break;
		case 'p':
			http_port = atoi(optarg);
			if (http_port <= 0 || http_port > 65535) {
				fprintf(stderr, "Invalid port number '%s'\n", optarg);
				fprintf(stderr, "Port must be between 1 and 65535\n");
				return 1;
			}
			break;

		case 'h':
			usage(argv[0]);
			return 0;

		default:
			fprintf(stderr, "Invalid option '-%c'\n", opt);
			usage(argv[0]);
			return 1;
		}
	}

	if (argv[optind] != NULL)
		function = argv[optind];

	fc = configfs_parse_uvc_function(function);
	if (!fc) {
		printf("Failed to identify function configuration\n");
		return 1;
	}

	if (cap_device != NULL && img_path != NULL) {
		printf("Both capture device and still image specified\n");
		printf("Please specify only one\n");
		return 1;
	}

	/*
	 * Create the events handler. Register a signal handler for SIGINT,
	 * received when the user presses CTRL-C. This will allow the main loop
	 * to be interrupted, and resources to be freed cleanly.
	 */
	events_init(&events);

	sigint_events = &events;
	signal(SIGINT, sigint_handler);

	/* Create and initialize a video source. */
	if (cap_device)
		src = v4l2_video_source_create(cap_device);
#ifdef HAVE_LIBCAMERA
	else if (camera)
		src = libcamera_source_create(camera);
#endif
	else if (img_path)
		src = jpg_video_source_create(img_path);
	else if (slideshow_dir)
		src = slideshow_video_source_create(slideshow_dir);
	else
		src = test_video_source_create();
	if (src == NULL) {
		ret = 1;
		goto done;
	}

	if (cap_device)
		v4l2_video_source_init(src, &events);

#ifdef HAVE_LIBCAMERA
	if (camera)
		libcamera_source_init(src, &events);
#endif

	if (http_port > 0) {
#ifdef HAVE_LIBCAMERA
		if (camera) {
			printf("Starting HTTP capture server on port %d\n", http_port);
			http_server = http_capture_new(http_port, src);
			if (!http_server) {
				fprintf(stderr, "Failed to create HTTP capture server\n");
				ret = 1;
				goto done;
			}
			printf("HTTP capture server started successfully\n");
			printf("Usage: curl http://<ip>:%d/capture > image.raw\n", http_port);
		} else {
			fprintf(stderr, "Warning: HTTP capture server is only supported with libcamera sources (-c option)\n");
			fprintf(stderr, "Ignoring -p %d option\n", http_port);
		}
#else
		fprintf(stderr, "Warning: HTTP capture server requires libcamera support\n");
		fprintf(stderr, "This build was compiled without libcamera. Ignoring -p %d option\n", http_port);
#endif
	}
	/* Create and initialise the stream. */
	stream = uvc_stream_new(fc->video);
	if (stream == NULL) {
		ret = 1;
		goto done;
	}

	uvc_stream_set_event_handler(stream, &events);
	uvc_stream_set_video_source(stream, src);
	uvc_stream_init_uvc(stream, fc);

	/* Main capture loop */
	events_loop(&events);

done:
	/* Cleanup */
	printf("\nShutting down...\n");
	
	if (http_server) {
		printf("Stopping HTTP capture server\n");
		http_capture_destroy(http_server);
	}

	uvc_stream_delete(stream);
	video_source_destroy(src);
	events_cleanup(&events);
	configfs_free_uvc_function(fc);

	return ret;
}
