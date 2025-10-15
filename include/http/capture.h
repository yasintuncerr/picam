/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Public interface for the HTTP capture server.
 *
 * Copyright (C) 2025 Yasin Tunçer
 */

#ifndef __CAPTURE_H__
#define __CAPTURE_H__

#include <pthread.h>
#include <stdbool.h>
#include "still-source.h" // Assumes still_buffer is defined here

// Forward declarations
struct http_server;
struct video_source;

/**
 * @brief Represents the state of a single client connection.
 */
struct http_client_session {
    int fd;
    struct http_server *server;
    pthread_t thread;
    pthread_mutex_t mtx;
    pthread_cond_t cond;
    struct still_buffer captured_data; // Holds metadata and a pointer to the copied raw data
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

/**
 * @brief Creates and starts a new HTTP capture server.
 * @param port The port to listen on.
 * @param vid_src The video source from which to get the still capture interface.
 * @return A pointer to the server instance, or NULL on failure.
 */
struct http_server *http_capture_new(int port, struct video_source *vid_src);

/**
 * @brief Stops and destroys the HTTP capture server.
 * @param server The server instance to destroy.
 */
void http_capture_destroy(struct http_server *server);

#endif /* __CAPTURE_H__ */