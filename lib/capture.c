/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * An HTTP server for still captures. This version correctly handles stride
 * in the raw buffer before creating a DNG file, fixing all image corruption.
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

// libcamera C++ interface
extern struct still_source *libcamera_get_still_source(struct video_source *s);
extern void libcamera_still_source_set_callback(struct still_source *ssrc,
                                                void (*cb)(void *, struct still_buffer *),
                                                void *data);

typedef struct {
    uint8_t *data;
    tsize_t size;
    tsize_t pos;
    tsize_t capacity;
} TiffMemoryBuffer;

// Forward declarations
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
    if (whence == SEEK_SET) new_pos = off;
    else if (whence == SEEK_CUR) new_pos += off;
    else if (whence == SEEK_END) new_pos = buffer->size + off;
    if (new_pos > (toff_t)buffer->capacity) return (toff_t)-1;
    buffer->pos = new_pos;
    return buffer->pos;
}
static tsize_t tiffReadProc(thandle_t fd, tdata_t buf, tsize_t size) {
    (void)fd; (void)buf; (void)size;
    return 0; 
}
static int tiffCloseProc(thandle_t fd) { 
    (void)fd;
    return 0;
}
static toff_t tiffSizeProc(thandle_t fd) { return ((TiffMemoryBuffer*)fd)->size; }

// NEW HELPER: Creates a new, contiguous buffer by removing stride padding.
static void* create_contiguous_buffer(const struct still_buffer* buffer) {
    if (!buffer->mem || buffer->stride == 0) return NULL;
    size_t valid_bytes_per_row = (size_t)buffer->width * 3 / 2;
    if (buffer->stride < valid_bytes_per_row) return NULL;
    size_t contiguous_size = valid_bytes_per_row * buffer->height;
    uint8_t* contiguous_buffer = (uint8_t*)malloc(contiguous_size);
    if (!contiguous_buffer) return NULL;
    uint8_t* src_ptr = (uint8_t*)buffer->mem;
    uint8_t* dst_ptr = contiguous_buffer;
    for (unsigned int y = 0; y < buffer->height; ++y) {
        memcpy(dst_ptr, src_ptr, valid_bytes_per_row);
        src_ptr += buffer->stride;
        dst_ptr += valid_bytes_per_row;
    }
    return contiguous_buffer;
}

// NEW HELPER: Unpacks a 12-bit packed buffer into a 16-bit buffer.
static uint16_t* unpack_12bit_to_16bit(const uint8_t* packed_buffer, size_t num_pixels) {
    size_t packed_size = num_pixels * 3 / 2;
    uint16_t* unpacked_buffer = (uint16_t*)malloc(num_pixels * sizeof(uint16_t));
    if (!unpacked_buffer) return NULL;
    for (size_t i = 0, j = 0; i < packed_size; i += 3, j += 2) {
        unpacked_buffer[j] = ((uint16_t)packed_buffer[i] << 4) | ((uint16_t)packed_buffer[i+1] >> 4);
        unpacked_buffer[j+1] = (((uint16_t)packed_buffer[i+1] & 0x0F) << 8) | (uint16_t)packed_buffer[i+2];
    }
    return unpacked_buffer;
}

// FINAL DNG CONVERSION FUNCTION
static void* convert_raw_to_dng_memory(const struct still_buffer* raw_buffer, tsize_t* dng_size) {
    if (!raw_buffer || !raw_buffer->mem || !dng_size) return NULL;

    void* contiguous_packed_buffer = create_contiguous_buffer(raw_buffer);
    if (!contiguous_packed_buffer) {
        fprintf(stderr, "Failed to create stride-less buffer.\n");
        return NULL;
    }

    size_t num_pixels = raw_buffer->width * raw_buffer->height;
    uint16_t* unpacked_data = unpack_12bit_to_16bit((const uint8_t*)contiguous_packed_buffer, num_pixels);
    free(contiguous_packed_buffer);

    if (!unpacked_data) {
        fprintf(stderr, "Failed to unpack 12-bit raw data.\n");
        return NULL;
    }

    TiffMemoryBuffer tiff_buffer = {0};
    tiff_buffer.capacity = (num_pixels * sizeof(uint16_t)) + 8192;
    tiff_buffer.data = (uint8_t*)malloc(tiff_buffer.capacity);
    if (!tiff_buffer.data) {
        free(unpacked_data);
        return NULL;
    }

    TIFF* tif = TIFFClientOpen("in-memory-dng", "w", (thandle_t)&tiff_buffer, tiffReadProc, tiffWriteProc, tiffSeekProc, tiffCloseProc, tiffSizeProc, NULL, NULL);
    if (!tif) {
        free(tiff_buffer.data);
        free(unpacked_data);
        return NULL;
    }
    
    TIFFSetField(tif, TIFFTAG_SUBFILETYPE, 0);
    TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, raw_buffer->width);
    TIFFSetField(tif, TIFFTAG_IMAGELENGTH, raw_buffer->height);
    TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 16); // Data is now in 16-bit containers
    TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_NONE);
    TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_CFA);
    TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 1);
    TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    TIFFSetField(tif, TIFFTAG_MAKE, "picam");
    TIFFSetField(tif, TIFFTAG_MODEL, "IMX477");

    uint16_t dng_version[] = {1, 4, 0, 0};
    TIFFSetField(tif, TIFFTAG_DNGVERSION, dng_version);
    
    uint16_t cfa_pattern[4] = {0, 1, 1, 2}; // Default RGGB
    if (raw_buffer->pixelformat == 0x32314742) { // 'BG12'
        cfa_pattern[0] = 2; cfa_pattern[1] = 1; cfa_pattern[2] = 1; cfa_pattern[3] = 0;
    }
    TIFFSetField(tif, TIFFTAG_CFAPATTERN, 4, cfa_pattern);
    
    uint32_t black_levels[4];
    for(int i = 0; i < 4; ++i) black_levels[i] = raw_buffer->black_level[i];
    TIFFSetField(tif, TIFFTAG_BLACKLEVEL, 4, black_levels);
    
    uint32_t white_level[] = {raw_buffer->white_level};
    TIFFSetField(tif, TIFFTAG_WHITELEVEL, 1, white_level);

    float as_shot_neutral[3];
    as_shot_neutral[0] = 1.0f / raw_buffer->white_balance_gains[0];
    as_shot_neutral[1] = 1.0f / raw_buffer->white_balance_gains[1];
    as_shot_neutral[2] = 1.0f / raw_buffer->white_balance_gains[2];
    TIFFSetField(tif, TIFFTAG_ASSHOTNEUTRAL, 3, as_shot_neutral);

    TIFFSetField(tif, TIFFTAG_COLORMATRIX1, 9, raw_buffer->color_correction_matrix);
    TIFFSetField(tif, TIFFTAG_CALIBRATIONILLUMINANT1, 21);

    if (TIFFWriteRawStrip(tif, 0, unpacked_data, num_pixels * sizeof(uint16_t)) < 0) {
        TIFFClose(tif);
        free(tiff_buffer.data);
        free(unpacked_data);
        return NULL;
    }

    TIFFClose(tif);
    *dng_size = tiff_buffer.size;
    free(unpacked_data);
    return tiff_buffer.data;
}

// ... The rest of the file (HTTP server, client handler, etc.) is the same as before ...
// (You can copy the rest from the previous correct version)
static void still_capture_ready_cb(void *data, struct still_buffer *buffer_from_camera) {
    struct http_client_session *session = (struct http_client_session *)data;
    void *data_copy = NULL;
    if (buffer_from_camera && !buffer_from_camera->error && buffer_from_camera->bytesused > 0) {
        data_copy = malloc(buffer_from_camera->bytesused);
        if (data_copy) memcpy(data_copy, buffer_from_camera->mem, buffer_from_camera->bytesused);
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
static void *client_thread_func(void *arg) {
    struct http_client_session *session = (struct http_client_session *)arg;
    char request_buf[2048] = {0};
    void *dng_data = NULL;
    tsize_t dng_size = 0;
    if (read(session->fd, request_buf, sizeof(request_buf) - 1) <= 0) goto cleanup;
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
                while (!session->capture_complete) pthread_cond_wait(&session->cond, &session->mtx);
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