/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * An HTTP server to trigger still captures and serve them as DNG files.
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
#include <tiffio.h>
#include <linux/videodev2.h>

#include "capture.h"
#include "still-source.h"

// libcamera C++ interface (provided by another object file)
extern struct still_source *libcamera_get_still_source(struct video_source *s);
extern void libcamera_still_source_set_callback(struct still_source *ssrc,
                                                void (*cb)(void *, struct still_buffer *),
                                                void *data);

// A structure to manage an in-memory buffer for libtiff
typedef struct {
    uint8_t *data;
    tsize_t size;
    tsize_t pos;
    tsize_t capacity;
} TiffMemoryBuffer;

// Forward declarations for static functions
static void *client_thread_func(void *arg);
static void *http_server_thread(void *arg);
static void still_capture_ready_cb(void *data, struct still_buffer *buffer);
static void* convert_raw_to_dng_memory(const struct still_buffer* raw_buffer, tsize_t* dng_size);

// In-memory I/O handlers for libtiff
static tsize_t tiffWriteProc(thandle_t fd, tdata_t buf, tsize_t size) {
    TiffMemoryBuffer* buffer = (TiffMemoryBuffer*)fd;
    if (buffer->pos + size > buffer->capacity) {
        tsize_t new_capacity = (buffer->pos + size) * 2;
        uint8_t *new_data = (uint8_t*)realloc(buffer->data, new_capacity);
        if (!new_data) return 0;
        buffer->data = new_data;
        buffer->capacity = new_capacity;
    }
    memcpy(buffer->data + buffer->pos, buf, size);
    buffer->pos += size;
    if (buffer->pos > buffer->size) buffer->size = buffer->pos;
    return size;
}

static toff_t tiffSeekProc(thandle_t fd, toff_t off, int whence) {
    TiffMemoryBuffer* buffer = (TiffMemoryBuffer*)fd;
    toff_t new_pos = buffer->pos;

    if (whence == SEEK_SET)      new_pos = off;
    else if (whence == SEEK_CUR) new_pos += off;
    else if (whence == SEEK_END) new_pos = buffer->size + off;

    // The restrictive check is now removed. We only check against capacity.
    if (new_pos < 0 || new_pos > (toff_t)buffer->capacity) {
        return (toff_t)-1;
    }

    buffer->pos = new_pos;
    return buffer->pos;
}

static tsize_t tiffReadProc(thandle_t fd, tdata_t buf, tsize_t size) { (void)fd; (void)buf; (void)size; return 0; }
static int tiffCloseProc(thandle_t fd) { (void)fd; return 0; }
static toff_t tiffSizeProc(thandle_t fd) { return ((TiffMemoryBuffer*)fd)->size; }

/**
 * @brief Converts a raw buffer with metadata into an in-memory DNG file.
 * The caller is responsible for freeing the returned buffer.
 */
static void* convert_raw_to_dng_memory(const struct still_buffer* raw_buffer, tsize_t* dng_size) {
    if (!raw_buffer || !raw_buffer->mem || !dng_size) return NULL;

    TiffMemoryBuffer tiff_buffer = {0};
    tiff_buffer.capacity = raw_buffer->bytesused + 8192; // Raw data + metadata headroom
    tiff_buffer.data = (uint8_t*)malloc(tiff_buffer.capacity);
    if (!tiff_buffer.data) return NULL;

    TIFF* tif = TIFFClientOpen("in-memory-dng", "w", (thandle_t)&tiff_buffer,
                               tiffReadProc, tiffWriteProc, tiffSeekProc,
                               tiffCloseProc, tiffSizeProc, NULL, NULL);
    if (!tif) {
        free(tiff_buffer.data);
        return NULL;
    }

    // --- Start of DNG Tag Configuration ---

    TIFFSetField(tif, TIFFTAG_SUBFILETYPE, 0);
    TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, raw_buffer->width);
    TIFFSetField(tif, TIFFTAG_IMAGELENGTH, raw_buffer->height);
    TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, raw_buffer->bit_depth);
    TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_NONE);
    TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_CFA);
    TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 1);
    TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    TIFFSetField(tif, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
    TIFFSetField(tif, TIFFTAG_MAKE, "picam");
    TIFFSetField(tif, TIFFTAG_MODEL, "IMX477");

    uint16_t dng_version[] = {1, 4, 0, 0};
    TIFFSetField(tif, TIFFTAG_DNGVERSION, dng_version);

    // FIX 1: Make the pixel format check more robust using the hex value from logs
    uint16_t cfa_pattern[4] = {0, 1, 1, 2}; // Default RGGB
    if (raw_buffer->pixelformat == 0x32314742) { // This is 'BG12' from your log
        // BGGR Pattern: 2=B, 1=G, 0=R -> [2, 1, 1, 0]
        cfa_pattern[0] = 2; cfa_pattern[1] = 1; cfa_pattern[2] = 1; cfa_pattern[3] = 0;
    }
    TIFFSetField(tif, TIFFTAG_CFAPATTERN, 4, cfa_pattern);

    uint32_t black_levels[4];
    for(int i = 0; i < 4; ++i) black_levels[i] = raw_buffer->black_level[i];
    TIFFSetField(tif, TIFFTAG_BLACKLEVEL, 4, black_levels);

    uint32_t white_level[] = {raw_buffer->white_level};
    TIFFSetField(tif, TIFFTAG_WHITELEVEL, 1, white_level);

    float as_shot_neutral[3]; // DNG stores 1/gain for white balance
    as_shot_neutral[0] = 1.0f / raw_buffer->white_balance_gains[0];
    as_shot_neutral[1] = 1.0f / raw_buffer->white_balance_gains[1];
    as_shot_neutral[2] = 1.0f / raw_buffer->white_balance_gains[2];
    TIFFSetField(tif, TIFFTAG_ASSHOTNEUTRAL, 3, as_shot_neutral);

    TIFFSetField(tif, TIFFTAG_COLORMATRIX1, 9, raw_buffer->color_correction_matrix);
    TIFFSetField(tif, TIFFTAG_CALIBRATIONILLUMINANT1, 21); // D65 Illuminant

    if (TIFFWriteRawStrip(tif, 0, raw_buffer->mem, raw_buffer->bytesused) < 0) {
        TIFFClose(tif);
        free(tiff_buffer.data);
        return NULL;
    }

    // This call finalizes the directory and is where the original error occurred.
    TIFFClose(tif);
    
    *dng_size = tiff_buffer.size;
    return tiff_buffer.data;
}

// Callback invoked by libcamera when a still frame is ready.
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
    void *dng_data = NULL;
    tsize_t dng_size = 0;

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
                    dng_data = convert_raw_to_dng_memory(&session->captured_data, &dng_size);
                    if (dng_data) {
                        char http_header[256];
                        int len = snprintf(http_header, sizeof(http_header),
                                           "HTTP/1.1 200 OK\r\n"
                                           "Content-Type: image/dng\r\n"
                                           "Content-Disposition: attachment; filename=\"capture.dng\"\r\n"
                                           "Content-Length: %zu\r\n\r\n", (size_t)dng_size);
                        write(session->fd, http_header, len);
                        write(session->fd, dng_data, dng_size);
                    } else {
                        const char *response = "HTTP/1.1 500 Internal Server Error\r\n\r\nDNG conversion failed";
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
    if (dng_data) free(dng_data);
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