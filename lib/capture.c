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

#include "capture.h"
#include "still-source.h"

// libcamera C++ arayüzü
extern struct still_source *libcamera_get_still_source(struct video_source *s);
extern void libcamera_still_source_set_callback(struct still_source *ssrc,
                                                void (*cb)(void *, struct still_buffer *),
                                                void *data);

// Forward declarations
static void *client_thread_func(void *arg);
static void *http_server_thread(void *arg);
static void still_capture_ready_cb(void *data, struct still_buffer *buffer);

/**
 * @brief Sends all data in a buffer over a socket, handling partial writes.
 */
static int send_all(int fd, const void *buf, size_t len) {
    const char *ptr = (const char *)buf;
    while (len > 0) {
        ssize_t sent = write(fd, ptr, len);
        if (sent <= 0) {
            // Hata veya bağlantı kapandı durumu
            if (sent < 0) perror("write in send_all");
            return -1;
        }
        ptr += sent;
        len -= sent;
    }
    return 0;
}

/**
 * @brief The callback function executed by libcamera when a still frame is ready.
 * **ÖNEMLİ DEĞİŞİKLİK:** Bu fonksiyon artık gelen buffer'daki veriyi kopyalar.
 */
static void still_capture_ready_cb(void *data, struct still_buffer *buffer_from_camera) {
    struct http_client_session *session = (struct http_client_session *)data;
    void *data_copy = NULL;

    // Gelen veriyi kopyalamak için yeni bellek alanı ayır
    if (buffer_from_camera && !buffer_from_camera->error && buffer_from_camera->bytesused > 0) {
        data_copy = malloc(buffer_from_camera->bytesused);
        if (data_copy) {
            memcpy(data_copy, buffer_from_camera->mem, buffer_from_camera->bytesused);
        } else {
            fprintf(stderr, "Failed to allocate memory for data copy\n");
        }
    }

    pthread_mutex_lock(&session->mtx);

    // Kopyalanan veriyi ve bilgilerini session yapısına aktar
    if (data_copy) {
        session->buffer_data = data_copy; // Kopyanın adresini sakla
        session->captured_data = *buffer_from_camera; // Diğer bilgileri kopyala
        session->captured_data.mem = data_copy; // Mem pointer'ını kopyanın adresi olarak ayarla
    } else {
        // Bellek ayrılamadı veya kamera hatası var
        session->buffer_data = NULL;
        session->captured_data.error = true;
    }
    
    session->capture_complete = true;
    pthread_cond_signal(&session->cond); // Client thread'ini uyandır
    pthread_mutex_unlock(&session->mtx);
}

/**
 * @brief Thread function to handle a single client connection.
 */
static void *client_thread_func(void *arg) {
    struct http_client_session *session = (struct http_client_session *)arg;
    char request_buf[2048] = {0};

    ssize_t n = read(session->fd, request_buf, sizeof(request_buf) - 1);
    if (n <= 0) {
        if (n < 0) perror("read");
        goto cleanup;
    }
    printf("Request from fd %d:\n%.*s\n", session->fd, (int)strcspn(request_buf, "\r\n"), request_buf);

    if (strstr(request_buf, "GET /capture") == request_buf) {
        if (!session->server->still_src) {
            const char *response = "HTTP/1.1 503 Service Unavailable\r\n\r\nNo still source available";
            write(session->fd, response, strlen(response));
        } else {
            libcamera_still_source_set_callback(session->server->still_src, still_capture_ready_cb, session);

            printf("waiting wamrmup")
            sleep(2); // Kamera ısınması için bekle
            if (still_source_capture(session->server->still_src) < 0) {
                const char *response = "HTTP/1.1 500 Internal Server Error\r\n\r\nFailed to start capture";
                write(session->fd, response, strlen(response));
            } else {
                pthread_mutex_lock(&session->mtx);
                while (!session->capture_complete) {
                    pthread_cond_wait(&session->cond, &session->mtx);
                }
                
                if (session->buffer_data && !session->captured_data.error) {
                    char header[256];
                    int header_len = snprintf(header, sizeof(header),
                                              "HTTP/1.1 200 OK\r\n"
                                              "Content-Type: application/octet-stream\r\n"
                                              "Content-Length: %u\r\n\r\n",
                                              session->captured_data.bytesused);

                    if (send_all(session->fd, header, header_len) == 0) {
                        if (send_all(session->fd, session->captured_data.mem, session->captured_data.bytesused) == 0) {
                             printf("Sent raw image (%u bytes) to fd %d\n", session->captured_data.bytesused, session->fd);
                        }
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
    if (session->buffer_data) {
        free(session->buffer_data); // Kopyalanan veriyi serbest bırak
    }
    pthread_mutex_destroy(&session->mtx);
    pthread_cond_destroy(&session->cond);
    free(session);
    return NULL;
}

/**
 * @brief Main server thread that listens for and accepts new connections.
 */
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

        struct http_client_session *session = malloc(sizeof(struct http_client_session));
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
    struct http_server *server = calloc(1, sizeof(struct http_server));
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