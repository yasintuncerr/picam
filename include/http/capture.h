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

#include <pthread.h>
#include <stdbool.h>
#include "still-source.h"

struct video_source;
struct http_server;

/**
 * @brief Represents the state of a single client connection.
 */
struct http_client_session {
    int fd;
    struct http_server *server;
    pthread_t thread;

    pthread_mutex_t mtx;
    pthread_cond_t cond;

    // Kopyalanan veriyi ve bilgilerini tutacak yapı
    struct still_buffer captured_data;
    void *buffer_data; // Malloc ile ayrılan ve kopyalanan verinin adresi

    bool capture_complete;
};

/**
 * @brief Represents the main HTTP server.
 */
struct http_server {
    int listen_fd;
    struct still_source *still_src;
    pthread_t server_thread;
    volatile bool running;
};

struct http_server *http_capture_new(int port, struct video_source *vid_src);
void http_capture_destroy(struct http_server *server);

#endif /* __CAPTURE_H__ */