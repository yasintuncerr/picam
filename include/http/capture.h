/* SPDX-License-Identifier: LGPL-2.1-or-later 
 * 
 * HTTP capture handling
 * 
 * Copyright (C) 2025 Yasin Tunçer
 *
 * Contact: Yasin Tunçer <yasintuncerr@gmail.com>
*/

#ifndef __HTTP_CAPTURE_H__
#define __HTTP_CAPTURE_H__

struct video_source;
struct events;
struct http_server;

/**
 * http_capture_new - Create a new HTTP capture server
 * @param port: Port number to run the HTTP server on
 * @param vid_src: Video source instance (can be NULL)
 * @param events: Event loop instance
 * 
 * Create a new HTTP capture instance that starts an HTTP server on the specified port.
 * The server will handle incoming requests for capturing images.
 * Returns a pointer to the newly created HTTP capture instance, or NULL on failure.
 */
struct http_server *http_capture_new(int port, struct video_source *vid_src, 
                                     struct events *events);

/**
 * http_capture_destroy - Destroy HTTP capture server
 * @param server: HTTP server instance to destroy
 */
void http_capture_destroy(struct http_server *server);

#endif /* __HTTP_CAPTURE_H__ */