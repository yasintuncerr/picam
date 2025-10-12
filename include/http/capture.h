/* SPDX-License-Identifier: LGPL-2.1-or-later 
 * 
 * HTTP capture handling
 * 
 * Copyright (C) 2025 Yasin Tunçer
 *
 * Contact: Yasin Tunçer <yasintuncerr@gmail.com>
*/

#ifndef __CAPTURE_H__
#define __CAPTURE_H__

struct still_source;
struct v4l2_pix_format;
struct http_server;

/**
 * http_capture_new - Create a new HTTP capture
 * @param port: Port number to run the HTTP server on
 * 
 * Create a new HTTP capture instance that starts an HTTP server on the specified port.
 * The server will handle incoming requests for capturing images.
 * Returns a pointer to the newly created HTTP capture instance, or NULL on failure.
 */
struct http_server *http_capture_new(int port);
void http_capture_destroy(struct http_server *server);
#endif /* __CAPTURE_H__ */

