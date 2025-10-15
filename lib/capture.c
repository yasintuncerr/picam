/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * An HTTP server to trigger still captures and serve them as PNG files.
 * This version requests processed RGB from libcamera and encodes it to PNG.
 *
 * Copyright (C) 2025 Yasin Tunçer
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdint.h>

#include "capture.h"
#include "still-source.h"

// Define this in one C file before including the header
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

// libcamera C++ interface
extern struct still_source *libcamera_get_still_source(struct video_source *s);
extern void libcamera_still_source_set_callback(struct still_source *ssrc,
                                                void (*cb)(void *, struct still_buffer *),
                                                void *data);

// Forward declarations for static functions
static void *client_thread_func(void *arg);
static void *http_server_thread(void *arg);
static void still_capture_ready_cb(void *data, struct still_buffer *buffer);
static unsigned char* convert_rgb_to_png_memory(const struct still_buffer* raw_buffer, int* png_size);


/**
 * @brief Encodes a raw RGB buffer into an in-memory PNG file using stb_image_write.
 * The caller is responsible for freeing the returned buffer with stbi_image_free().
 */
static unsigned char* convert_rgb_to_png_memory(const struct still_buffer* rgb_buffer, int* png_size) {
    if (!rgb_buffer || !rgb_buffer->mem || !png_size) return NULL;

    // stbi_write_png_to_mem does all the heavy lifting.
    // It allocates memory, writes the PNG data, and returns the pointer and size.
    int stride_bytes = rgb_buffer->width * 3; // For RGB888, each pixel is 3 bytes
    return stbi_write_png_to_mem(
        (const unsigned char*)rgb_buffer->mem,
        stride_bytes,
        rgb_buffer->width,
        rgb_buffer->height,
        3, // 3 components: R, G, B
        png_size
    );
}

// Callback invoked by libcamera when a still RGB frame is ready.
static void still_capture_ready_cb(void *data, struct still_buffer *buffer_from_camera) {
    struct http_client_session *session = (struct http_client_session *)data;
    void *data_copy = NULL;

    if (buffer_from_camera && !buffer_from_camera->error && buffer_from_camera->bytesused > 0) {
        data_copy = malloc(buffer_from_camera->bytesused);
        if (data_copy) {
            memcpy(data_copy, buffer_from_camera->mem, buffer_from_camera->bytesused);
        }
    }

    pthread_mutex_lock(&session->mtx);
    if (data_copy) {
        session->captured_data = *buffer_from_camera;
        session->captured_data.mem = data_copy;
    } else {
        session->captured_data.mem = NULL;
        session->captured_data.error = true;
    }
    session->capture_complete = true;
    pthread_cond_signal(&session->cond);
    pthread_mutex_unlock(&session->mtx);
}

// Thread function to handle a single client request.
static void *client_thread_func(void *arg) {
    struct http_client_session *session = (struct http_client_session *)arg;
    char request_buf[2048] = {0};
    unsigned char *png_data = NULL;
    int png_size = 0;

    if (read(session->fd, request_buf, sizeof(request_buf) - 1) <= 0) {
        goto cleanup;
    }

    if (strstr(request_buf, "GET /capture") == request_buf) {
        if (!session->server->still_src) {
            const char *response = "HTTP/1.1 503 Service Unavailable\r\n\r\n";
            write(session->fd, response, strlen(response));
        } else {
            libcamera_still_source_set_callback(session->server->still_src, still_capture_ready_cb, session);
            if (still_source_capture(session->server->still_src) < 0) {
                const char *response = "HTTP/1.1 500 Internal Server Error\r\n\r\nCapture trigger failed";
                write(session->fd, response, strlen(response));
            } else {
                pthread_mutex_lock(&session->mtx);
                while (!session->capture_complete) {
                    pthread_cond_wait(&session->cond, &session->mtx);
                }
                
                if (session->captured_data.mem && !session->captured_data.error) {
                    png_data = convert_rgb_to_png_memory(&session->captured_data, &png_size);
                    if (png_data) {
                        char http_header[256];
                        int len = snprintf(http_header, sizeof(http_header),
                                           "HTTP/1.1 200 OK\r\n"
                                           "Content-Type: image/png\r\n"
                                           "Content-Disposition: attachment; filename=\"capture.png\"\r\n"
                                           "Content-Length: %d\r\n\r\n", png_size);
                        write(session->fd, http_header, len);
                        write(session->fd, png_data, png_size);
                    } else {
                        const char *response = "HTTP/1.1 500 Internal Server Error\r\n\r\nPNG conversion failed";
                        write(session->fd, response, strlen(response));
                    }
                } else {
                    const char *response = "HTTP/1.1 500 Internal Server Error\r\n\r\nCapture failed";
                    write(session->fd, response, strlen(response));
                }
                pthread_mutex_unlock(&session->mtx);
            }
        }
    } else {
        const char *response = "HTTP/1.1 404 Not Found\r\n\r\n";
        write(session->fd, response, strlen(response));
    }

cleanup:
    close(session->fd);
    if (session->captured_data.mem) free(session->captured_data.mem);
    if (png_data) free(png_data);
    pthread_mutex_destroy(&session->mtx);
    pthread_cond_destroy(&session->cond);
    free(session);
    return NULL;
}

// Main server thread to accept new connections.
static void *http_server_thread(void *arg) {
    struct http_server *server = (struct http_server *)arg;
    while (server->running) {
        int client_fd = accept(server->listen_fd, NULL, NULL);
        if (client_fd < 0) continue;

        struct http_client_session *session = (struct http_client_session*)malloc(sizeof(*session));
        if (!session) {
            close(client_fd);
            continue;
        }
        
        memset(session, 0, sizeof(*session));
        session->fd = client_fd;
        session->server = server;
        pthread_mutex_init(&session->mtx, NULL);
        pthread_cond_init(&session->cond, NULL);

        if (pthread_create(&session->thread, NULL, client_thread_func, session) != 0) {
            close(client_fd);
            free(session);
        }
        pthread_detach(session->thread);
    }
    return NULL;
}

struct http_server *http_capture_new(int port, struct video_source *video_src) {
    struct http_server *server = (struct http_server*)calloc(1, sizeof(*server));
    if (!server) return NULL;

    server->still_src = video_src ? libcamera_get_still_source(video_src) : NULL;
    server->running = true;
    server->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server->listen_fd < 0) {
        free(server);
        return NULL;
    }

    int opt = 1;
    setsockopt(server->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in serv_addr = {0};
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port = htons(port);

    if (bind(server->listen_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0 ||
        listen(server->listen_fd, 10) < 0) {
        close(server->listen_fd);
        free(server);
        return NULL;
    }

    if (pthread_create(&server->server_thread, NULL, http_server_thread, server) != 0) {
        close(server->listen_fd);
        free(server);
        return NULL;
    }

    return server;
}

void http_capture_destroy(struct http_server *server) {
    if (!server) return;
    server->running = false;
    shutdown(server->listen_fd, SHUT_RDWR);
    close(server->listen_fd);
    pthread_join(server->server_thread, NULL);
    free(server);
}