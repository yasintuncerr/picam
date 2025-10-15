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
#include <stdint.h>
#include <tiffio.h>

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

// TIFF memory write context
typedef struct {
    uint8_t *data;
    size_t size;
    size_t capacity;
} tiff_mem_context;

static tsize_t tiff_mem_write(thandle_t handle, tdata_t data, tsize_t size) {
    tiff_mem_context *ctx = (tiff_mem_context *)handle;
    
    // Expand buffer if needed
    if (ctx->size + size > ctx->capacity) {
        size_t new_capacity = ctx->capacity * 2 + size;
        uint8_t *new_data = realloc(ctx->data, new_capacity);
        if (!new_data) return 0;
        ctx->data = new_data;
        ctx->capacity = new_capacity;
    }
    
    memcpy(ctx->data + ctx->size, data, size);
    ctx->size += size;
    return size;
}

static toff_t tiff_mem_seek(thandle_t handle, toff_t offset, int whence) {
    tiff_mem_context *ctx = (tiff_mem_context *)handle;
    
    switch (whence) {
        case SEEK_SET:
            ctx->size = offset;
            break;
        case SEEK_CUR:
            ctx->size += offset;
            break;
        case SEEK_END:
            // Not typically used for writing
            break;
    }
    
    return ctx->size;
}

static int tiff_mem_close(thandle_t handle) {
    (void)handle;
    return 0;
}

static toff_t tiff_mem_size(thandle_t handle) {
    tiff_mem_context *ctx = (tiff_mem_context *)handle;
    return ctx->size;
}

static int create_tiff_from_bayer(const uint8_t *bayer_data, unsigned int width, 
                                   unsigned int height, uint32_t pixelformat,
                                   uint8_t **tiff_output, size_t *tiff_size) {
    TIFF *tif;
    tiff_mem_context ctx = {0};
    
    // Initial allocation
    ctx.capacity = width * height * 2 + 8192; // Bayer data + headers
    ctx.data = malloc(ctx.capacity);
    if (!ctx.data) return -1;
    
    // Open TIFF in memory
    tif = TIFFClientOpen("memory", "w", (thandle_t)&ctx,
                         NULL, tiff_mem_write, tiff_mem_seek,
                         tiff_mem_close, tiff_mem_size, NULL, NULL);
    
    if (!tif) {
        free(ctx.data);
        return -1;
    }
    
    // Set TIFF tags for Bayer CFA image
    TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, width);
    TIFFSetField(tif, TIFFTAG_IMAGELENGTH, height);
    TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 12);
    TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 1);
    TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_CFA);
    TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_NONE);
    TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, height);
    
    // CFA Pattern for RGGB (Bayer pattern)
    // TIFFTAG_CFAREPEATPATTERNDIM = [2, 2]
    uint16_t cfa_repeat[2] = {2, 2};
    TIFFSetField(tif, TIFFTAG_CFAREPEATPATTERNDIM, cfa_repeat);
    
    // TIFFTAG_CFAPATTERN for RGGB = [0,1,1,2] where 0=Red, 1=Green, 2=Blue
    uint8_t cfa_pattern[4] = {0, 1, 1, 2}; // RGGB
    TIFFSetField(tif, TIFFTAG_CFAPATTERN, 4, cfa_pattern);
    
    // Write software tag
    TIFFSetField(tif, TIFFTAG_SOFTWARE, "UVC Gadget Capture");
    
    // Set pixel format as custom tag (if needed later)
    char pixfmt_desc[64];
    snprintf(pixfmt_desc, sizeof(pixfmt_desc), "PixelFormat: 0x%08X", pixelformat);
    TIFFSetField(tif, TIFFTAG_IMAGEDESCRIPTION, pixfmt_desc);
    
    // Calculate bytes per row (12-bit packed means 1.5 bytes per pixel)
    // For simplicity, assuming data is provided as 16-bit values
    size_t bytes_per_row = width * 2; // 16-bit storage for 12-bit data
    
    // Write image data
    for (unsigned int row = 0; row < height; row++) {
        if (TIFFWriteScanline(tif, (void *)(bayer_data + row * bytes_per_row), row, 0) < 0) {
            TIFFClose(tif);
            free(ctx.data);
            return -1;
        }
    }
    
    TIFFClose(tif);
    
    *tiff_output = ctx.data;
    *tiff_size = ctx.size;
    
    return 0;
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
        close_client(client);
        return;
    }

    char header[1024];
    uint8_t *tiff_data = NULL;
    size_t tiff_size = 0;
    
    printf("Received still image: %ux%u, format=0x%08X, size=%u bytes\n",
           buffer->width, buffer->height, buffer->pixelformat, buffer->bytesused);
    
    // Create TIFF from Bayer data
    printf("Creating TIFF from Bayer data...\n");
    int ret = create_tiff_from_bayer(buffer->mem, buffer->width, buffer->height,
                                      buffer->pixelformat, &tiff_data, &tiff_size);
    
    if (ret < 0 || !tiff_data) {
        fprintf(stderr, "Failed to create TIFF\n");
        const char *err_response = 
            "HTTP/1.1 500 Internal Server Error\r\n"
            "Content-Length: 20\r\n"
            "\r\n"
            "TIFF creation failed";
        write(client->fd, err_response, strlen(err_response));
        close_client(client);
        return;
    }
    
    // HTTP Response header
    int header_len = snprintf(header, sizeof(header),
                              "HTTP/1.1 200 OK\r\n"
                              "Content-Type: image/tiff\r\n"
                              "Content-Disposition: attachment; filename=\"capture.tiff\"\r\n"
                              "Content-Length: %zu\r\n"
                              "X-Image-Width: %u\r\n"
                              "X-Image-Height: %u\r\n"
                              "X-Pixel-Format: 0x%08X\r\n"
                              "X-Bits-Per-Sample: 12\r\n"
                              "X-CFA-Pattern: RGGB\r\n"
                              "X-Timestamp: %ld.%06ld\r\n"
                              "\r\n",
                              tiff_size,
                              buffer->width,
                              buffer->height,
                              buffer->pixelformat,
                              buffer->timestamp.tv_sec,
                              buffer->timestamp.tv_usec);

    // Send HTTP header
    ssize_t write_ret = write(client->fd, header, header_len);
    if (write_ret > 0) {
        // Send TIFF data
        write_ret = write(client->fd, tiff_data, tiff_size);
        if (write_ret > 0) {
            printf("Sent TIFF image (%ux%u, %zu bytes) to fd %d\n", 
                   buffer->width, buffer->height, tiff_size, client->fd);
        }
    }
    
    free(tiff_data);
    
    if (write_ret < 0) {
        perror("write failed");
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
                "Content-Length: 24\r\n"
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
    printf("Captures will be delivered as TIFF files with Bayer CFA metadata\n");
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