/* SPDX-License-Identifier: LGPL-2.1-or-later */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <stdbool.h>

#include "events.h"
#include "capture.h"
#include "still-source.h"

// libcamera C++ interface
extern struct still_source *libcamera_get_still_source(struct video_source *s);
extern void libcamera_still_source_set_callback(struct still_source *ssrc,
                                                void (*cb)(void *, struct still_buffer *),
                                                void *data);

#define MAX_CLIENTS 10

struct http_client {
    int fd;
    struct http_server *server;  
    bool capture_pending;       
};

struct http_server {
    int listen_fd;
    struct events *events;
    struct still_source *still_src;
    struct http_client clients[MAX_CLIENTS];
};

// Forward declarations
static void still_capture_ready_cb(void *data, struct still_buffer *buffer);
static void handle_http_request(void *priv);
static void accept_connection(void *priv);
static void close_client(struct http_client *client);

static void close_client(struct http_client *client) {
    if (client->fd != -1) {
        events_unwatch_fd(client->server->events, client->fd, EVENT_READ);
        close(client->fd);
        printf("Connection closed: fd %d\n", client->fd);
        client->fd = -1;
    }
    client->capture_pending = false;
}

static void still_capture_ready_cb(void *data, struct still_buffer *buffer) {
    struct http_client *client = (struct http_client *)data;

    if (!client || client->fd == -1 || !client->capture_pending) {
        fprintf(stderr, "Client no longer valid or capture not pending\n");
        return;
    }

    client->capture_pending = false;

    if (buffer->error) {
        fprintf(stderr, "Capture failed\n");
        const char *response = 
            "HTTP/1.1 500 Internal Server Error\r\n"
            "Content-Length: 15\r\n"
            "\r\n"
            "Capture failed.";
        write(client->fd, response, strlen(response));
    } else {
        char header[512];
        
        int header_len = snprintf(header, sizeof(header),
                                  "HTTP/1.1 200 OK\r\n"
                                  "Content-Type: application/octet-stream\r\n"
                                  "Content-Disposition: attachment; filename=\"capture.raw\"\r\n"
                                  "Content-Length: %u\r\n"
                                  "X-Image-Size: %u\r\n"
                                  "X-Timestamp: %ld.%06ld\r\n"
                                  "\r\n",
                                  buffer->bytesused,
                                  buffer->size,
                                  buffer->timestamp.tv_sec,
                                  buffer->timestamp.tv_usec);

        ssize_t ret = write(client->fd, header, header_len);
        if (ret > 0) {
            ret = write(client->fd, buffer->mem, buffer->bytesused);
            if (ret > 0) {
                printf("Sent raw image (%u bytes) to fd %d\n", 
                       buffer->bytesused, client->fd);
            }
        }
        
        if (ret < 0) {
            perror("write failed");
        }
    }

    close_client(client);
}

static void handle_http_request(void *priv) {
    struct http_client *client = (struct http_client *)priv;
    char buffer[2048] = {0};

    ssize_t n = read(client->fd, buffer, sizeof(buffer) - 1);
    if (n <= 0) {
        if (n < 0) {
            perror("read");
        }
        close_client(client);
        return;
    }

    buffer[n] = '\0';
    printf("Request from fd %d:\n%.*s\n", client->fd, 
           (int)strcspn(buffer, "\r\n"), buffer);

    // Request parsing
    if (strstr(buffer, "GET /capture") == buffer || 
        strstr(buffer, "GET /capture ") == buffer) {
        
        if (!client->server->still_src) {
            const char *response = 
                "HTTP/1.1 503 Service Unavailable\r\n"
                "Content-Length: 26\r\n"
                "\r\n"
                "No still source available";
            write(client->fd, response, strlen(response));
            close_client(client);
            return;
        }

        if (client->capture_pending) {
            const char *response = 
                "HTTP/1.1 429 Too Many Requests\r\n"
                "Content-Length: 24\r\n"
                "\r\n"
                "Capture already pending";
            write(client->fd, response, strlen(response));
            close_client(client);
            return;
        }

        libcamera_still_source_set_callback(client->server->still_src, 
                                           still_capture_ready_cb, 
                                           client);
        
        int ret = still_source_capture(client->server->still_src);
        if (ret < 0) {
            fprintf(stderr, "Failed to start capture: %d\n", ret);
            const char *response = 
                "HTTP/1.1 500 Internal Server Error\r\n"
                "Content-Length: 21\r\n"
                "\r\n"
                "Failed to start capture";
            write(client->fd, response, strlen(response));
            close_client(client);
            return;
        }

        client->capture_pending = true;
        
        events_unwatch_fd(client->server->events, client->fd, EVENT_READ);
        
    } else if (strstr(buffer, "GET / ") == buffer ||
               strstr(buffer, "GET /\r") == buffer) {
        const char *response = 
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: 33\r\n"
            "\r\n"
            "UVC Gadget Capture Server\n"
            "Use: GET /capture";
        write(client->fd, response, strlen(response));
        close_client(client);
        
    } else {
        const char *response = 
            "HTTP/1.1 404 Not Found\r\n"
            "Content-Length: 13\r\n"
            "\r\n"
            "404 Not Found";
        write(client->fd, response, strlen(response));
        close_client(client);
    }
}

static void accept_connection(void *priv) {
    struct http_server *server = (struct http_server *)priv;
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    
    int client_fd = accept(server->listen_fd, 
                          (struct sockaddr *)&client_addr, 
                          &client_len);

    if (client_fd < 0) {
        perror("accept");
        return;
    }

    int i;
    for (i = 0; i < MAX_CLIENTS; ++i) {
        if (server->clients[i].fd == -1) {
            server->clients[i].fd = client_fd;
            server->clients[i].server = server; 
            server->clients[i].capture_pending = false;
            
            int flags = fcntl(client_fd, F_GETFL, 0);
            fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);
            
            events_watch_fd(server->events, client_fd, EVENT_READ, 
                          handle_http_request, &server->clients[i]);
            
            printf("New connection accepted: fd %d from %s:%d\n", 
                   client_fd,
                   inet_ntoa(client_addr.sin_addr),
                   ntohs(client_addr.sin_port));
            return;
        }
    }

    fprintf(stderr, "Max clients (%d) reached, connection rejected\n", 
            MAX_CLIENTS);
    close(client_fd);
}

struct http_server *http_capture_new(int port, struct video_source *video_src, 
                                     struct events *events) {
    if (!events) {
        fprintf(stderr, "events cannot be NULL\n");
        return NULL;
    }

    struct http_server *server = calloc(1, sizeof(struct http_server));
    if (!server) {
        perror("malloc");
        return NULL;
    }

    server->events = events;
    server->still_src = video_src ? libcamera_get_still_source(video_src) : NULL;

    for (int i = 0; i < MAX_CLIENTS; ++i) {
        server->clients[i].fd = -1;
        server->clients[i].server = server;
        server->clients[i].capture_pending = false;
    }

    server->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server->listen_fd < 0) {
        perror("socket");
        free(server);
        return NULL;
    }

    // Socket options
    int opt = 1;
    if (setsockopt(server->listen_fd, SOL_SOCKET, SO_REUSEADDR, 
                   &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        close(server->listen_fd);
        free(server);
        return NULL;
    }

    // Bind
    struct sockaddr_in serv_addr = {0};
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port = htons(port);

    if (bind(server->listen_fd, (struct sockaddr *)&serv_addr, 
             sizeof(serv_addr)) < 0) {
        perror("bind");
        close(server->listen_fd);
        free(server);
        return NULL;
    }

    // Listen
    if (listen(server->listen_fd, 5) < 0) {
        perror("listen");
        close(server->listen_fd);
        free(server);
        return NULL;
    }

    events_watch_fd(events, server->listen_fd, EVENT_READ, 
                   accept_connection, server);

    printf("HTTP capture server listening on port %d\n", port);
    if (!server->still_src) {
        printf("WARNING: No still source available - /capture will fail\n");
    }
    
    return server;
}

void http_capture_destroy(struct http_server *server) {
    if (!server) return;

    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (server->clients[i].fd != -1) {
            close_client(&server->clients[i]);
        }
    }

    if (server->listen_fd != -1) {
        events_unwatch_fd(server->events, server->listen_fd, EVENT_READ);
        close(server->listen_fd);
    }

    free(server);
    printf("HTTP capture server destroyed\n");
}