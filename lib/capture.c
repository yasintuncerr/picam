/* SPDX-License-Identifier: LGPL-2.1-or-later */

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

// libcamera C++ arayüzü
extern struct still_source *libcamera_get_still_source(struct video_source *s);
extern void libcamera_still_source_set_callback(struct still_source *ssrc,
                                                void (*cb)(void *, struct still_buffer *),
                                                void *data);

// Bellek içi TIFF işlemleri için özel yapı
typedef struct {
    uint8_t *data;
    tsize_t size;
    tsize_t pos;
    tsize_t capacity;
} TiffBuffer;

// Forward declarations ve diğer yardımcı fonksiyonlar (değişiklik yok)
static void *client_thread_func(void *arg);
static void *http_server_thread(void *arg);
static void still_capture_ready_cb(void *data, struct still_buffer *buffer);
static void* convert_raw_to_tiff_memory(struct still_buffer* raw_buffer, tsize_t* tiff_size);

static int send_all(int fd, const void *buf, size_t len) {
    const char *ptr = (const char *)buf;
    while (len > 0) {
        ssize_t sent = write(fd, ptr, len);
        if (sent <= 0) {
            if (sent < 0) perror("write in send_all");
            return -1;
        }
        ptr += sent;
        len -= sent;
    }
    return 0;
}

static tsize_t tiffWriteProc(thandle_t fd, tdata_t buf, tsize_t size) {
    TiffBuffer* buffer = (TiffBuffer*)fd;
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

static tsize_t tiffReadProc(thandle_t fd, tdata_t buf, tsize_t size) { (void)fd; (void)buf; (void)size; return 0; }
static toff_t tiffSeekProc(thandle_t fd, toff_t off, int whence) {
    TiffBuffer* buffer = (TiffBuffer*)fd;
    toff_t new_pos = buffer->pos;
    if (whence == SEEK_SET) new_pos = off;
    else if (whence == SEEK_CUR) new_pos += off;
    else if (whence == SEEK_END) new_pos = buffer->size + off;
    if (new_pos > (toff_t)buffer->size) return (toff_t)-1;
    buffer->pos = new_pos;
    return buffer->pos;
}
static int tiffCloseProc(thandle_t fd) { (void)fd; return 0; }
static toff_t tiffSizeProc(thandle_t fd) { TiffBuffer* buffer = (TiffBuffer*)fd; return buffer->size; }


// NIHAYET DÜZELTİLMİŞ FONKSİYON
static void* convert_raw_to_tiff_memory(struct still_buffer* raw_buffer, tsize_t* tiff_size) {
    TiffBuffer tiff_buffer;
    tiff_buffer.capacity = raw_buffer->bytesused + 4096;
    tiff_buffer.data = (uint8_t*)malloc(tiff_buffer.capacity);
    if (!tiff_buffer.data) return NULL;
    tiff_buffer.pos = 0;
    tiff_buffer.size = 0;

    TIFF* tif = TIFFClientOpen("in-memory", "w", (thandle_t)&tiff_buffer,
                               tiffReadProc, tiffWriteProc, tiffSeekProc,
                               tiffCloseProc, tiffSizeProc, NULL, NULL);
    if (!tif) {
        free(tiff_buffer.data);
        return NULL;
    }
    
    // --- TEMEL TIFF/DNG ETİKETLERİ ---
    TIFFSetField(tif, TIFFTAG_SUBFILETYPE, 0);
    TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, raw_buffer->width);
    TIFFSetField(tif, TIFFTAG_IMAGELENGTH, raw_buffer->height);
    TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 1);
    TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 12);
    TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_NONE);
    TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_CFA);
    TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    TIFFSetField(tif, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
    TIFFSetField(tif, TIFFTAG_MAKE, "Raspberry Pi");
    TIFFSetField(tif, TIFFTAG_MODEL, "imx477");

    uint16_t dng_version[] = {1, 4, 0, 0};
    TIFFSetField(tif, TIFFTAG_DNGVERSION, dng_version);
    
    // --- KRİTİK DÜZELTMELER ---
    // 1. Bayer desenini logdaki SBGGR12'ye göre BGGR olarak ayarla
    uint16_t cfa_pattern[] = {2, 1, 1, 0}; // 0=R, 1=G, 2=B  (BG/GR -> 2,1 / 1,0)
    TIFFSetField(tif, TIFFTAG_CFAPATTERN, cfa_pattern);
    
    // 2. BlackLevel ve WhiteLevel'ı DİZİ olarak tanımla ve pointer ile geç
    uint16_t black_levels[] = {1024, 1024, 1024, 1024}; // Her renk kanalı için
    TIFFSetField(tif, TIFFTAG_BLACKLEVEL, 4, black_levels);
    
    uint16_t white_level[] = {4095};
    TIFFSetField(tif, TIFFTAG_WHITELEVEL, 1, white_level);

    // 3. dcraw'ın renkleri doğru işlemesi için standart renk matrisi ekle
    float color_matrix1[] = { 1.475, -0.55, -0.15, -0.4, 1.25, 0.1, 0.0, 0.1, 0.75 };
    TIFFSetField(tif, TIFFTAG_COLORMATRIX1, 9, color_matrix1);

    if (TIFFWriteRawStrip(tif, 0, raw_buffer->mem, raw_buffer->bytesused) < 0) {
        fprintf(stderr, "TIFFWriteRawStrip failed\n");
        TIFFClose(tif);
        free(tiff_buffer.data);
        return NULL;
    }

    TIFFClose(tif);
    *tiff_size = tiff_buffer.size;
    return tiff_buffer.data;
}


// --- Geri kalan kod (still_capture_ready_cb, client_thread_func, vb.) aynı ---
static void still_capture_ready_cb(void *data, struct still_buffer *buffer_from_camera) {
    struct http_client_session *session = (struct http_client_session *)data;
    void *data_copy = NULL;
    if (buffer_from_camera && !buffer_from_camera->error && buffer_from_camera->bytesused > 0) {
        data_copy = malloc(buffer_from_camera->bytesused);
        if (data_copy) {
            memcpy(data_copy, buffer_from_camera->mem, buffer_from_camera->bytesused);
        } else {
            fprintf(stderr, "Failed to allocate memory for data copy\n");
        }
    }
    pthread_mutex_lock(&session->mtx);
    if (data_copy) {
        session->buffer_data = data_copy;
        session->captured_data = *buffer_from_camera;
        session->captured_data.mem = data_copy;
    } else {
        session->buffer_data = NULL;
        session->captured_data.error = true;
    }
    session->capture_complete = true;
    pthread_cond_signal(&session->cond);
    pthread_mutex_unlock(&session->mtx);
}

static void *client_thread_func(void *arg) {
    struct http_client_session *session = (struct http_client_session *)arg;
    char request_buf[2048] = {0};
    void *tiff_data = NULL;
    tsize_t tiff_size = 0;

    ssize_t n = read(session->fd, request_buf, sizeof(request_buf) - 1);
    if (n <= 0) {
        if (n < 0) perror("read");
        goto cleanup;
    }
    if (strstr(request_buf, "GET /capture") == request_buf) {
        if (!session->server->still_src) {
            const char *response = "HTTP/1.1 503 Service Unavailable\r\n\r\nNo still source available";
            write(session->fd, response, strlen(response));
        } else {
            libcamera_still_source_set_callback(session->server->still_src, still_capture_ready_cb, session);
            if (still_source_capture(session->server->still_src) < 0) {
                const char *response = "HTTP/1.1 500 Internal Server Error\r\n\r\nFailed to start capture";
                write(session->fd, response, strlen(response));
            } else {
                pthread_mutex_lock(&session->mtx);
                while (!session->capture_complete) {
                    pthread_cond_wait(&session->cond, &session->mtx);
                }
                if (session->buffer_data && !session->captured_data.error) {
                    tiff_data = convert_raw_to_tiff_memory(&session->captured_data, &tiff_size);
                    if (tiff_data) {
                        char http_header[256];
                        int http_header_len = snprintf(http_header, sizeof(http_header),
                                                  "HTTP/1.1 200 OK\r\n"
                                                  "Content-Type: image/tiff\r\n"
                                                  "Content-Disposition: attachment; filename=\"capture.dng\"\r\n"
                                                  "Content-Length: %zu\r\n\r\n",
                                                  (size_t)tiff_size);
                        if (send_all(session->fd, http_header, http_header_len) == 0) {
                            if (send_all(session->fd, tiff_data, tiff_size) == 0) {
                                printf("Sent DNG/TIFF image (%zu bytes) to fd %d\n", (size_t)tiff_size, session->fd);
                            }
                        }
                    } else {
                        const char *response = "HTTP/1.1 500 Internal Server Error\r\n\r\nFailed to create TIFF file.";
                        write(session->fd, response, strlen(response));
                    }
                } else {
                    const char *response = "HTTP/1.1 500 Internal Server Error\r\n\r\nCapture failed.";
                    write(session->fd, response, strlen(response));
                }
                pthread_mutex_unlock(&session->mtx);
            }
        }
    } else {
        const char *response = "HTTP/1.1 404 Not Found\r\n\r\n404 Not Found";
        write(session->fd, response, strlen(response));
    }
cleanup:
    printf("Connection closed: fd %d\n", session->fd);
    close(session->fd);
    if (session->buffer_data) free(session->buffer_data);
    if (tiff_data) free(tiff_data);
    pthread_mutex_destroy(&session->mtx);
    pthread_cond_destroy(&session->cond);
    free(session);
    return NULL;
}

static void *http_server_thread(void *arg) {
    struct http_server *server = (struct http_server *)arg;
    printf("HTTP server thread started, listening...\n");
    while (server->running) {
        int client_fd = accept(server->listen_fd, NULL, NULL);
        if (client_fd < 0) {
            if (!server->running) break;
            perror("accept");
            continue;
        }
        struct http_client_session *session = (struct http_client_session*)malloc(sizeof(struct http_client_session));
        if (!session) {
            close(client_fd);
            continue;
        }
        session->fd = client_fd;
        session->server = server;
        session->buffer_data = NULL;
        session->capture_complete = false;
        pthread_mutex_init(&session->mtx, NULL);
        pthread_cond_init(&session->cond, NULL);
        if (pthread_create(&session->thread, NULL, client_thread_func, session) != 0) {
            perror("pthread_create for client");
            close(client_fd);
            free(session);
        }
        pthread_detach(session->thread);
    }
    printf("HTTP server thread shutting down.\n");
    return NULL;
}

struct http_server *http_capture_new(int port, struct video_source *video_src) {
    struct http_server *server = (struct http_server*)calloc(1, sizeof(struct http_server));
    if (!server) {
        perror("malloc for server");
        return NULL;
    }
    server->still_src = video_src ? libcamera_get_still_source(video_src) : NULL;
    server->running = true;
    server->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server->listen_fd < 0) {
        perror("socket");
        free(server);
        return NULL;
    }
    int opt = 1;
    setsockopt(server->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in serv_addr = {0};
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port = htons(port);
    if (bind(server->listen_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("bind");
        close(server->listen_fd);
        free(server);
        return NULL;
    }
    if (listen(server->listen_fd, 10) < 0) {
        perror("listen");
        close(server->listen_fd);
        free(server);
        return NULL;
    }
    if (pthread_create(&server->server_thread, NULL, http_server_thread, server) != 0) {
        perror("pthread_create for server");
        close(server->listen_fd);
        free(server);
        return NULL;
    }
    printf("HTTP capture server listening on port %d\n", port);
    if (!server->still_src) {
        printf("WARNING: No still source available - /capture will fail\n");
    }
    return server;
}

void http_capture_destroy(struct http_server *server) {
    if (!server) return;
    printf("Destroying HTTP capture server...\n");
    server->running = false;
    shutdown(server->listen_fd, SHUT_RDWR);
    close(server->listen_fd);
    pthread_join(server->server_thread, NULL);
    free(server);
    printf("HTTP capture server destroyed\n");
}