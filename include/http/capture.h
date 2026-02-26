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
#include "still-source.h"
#include "capture_controls.h"

/* Default path for persistent capture profile */
#define CAPTURE_PROFILE_PATH "/home/picam/.picam_capture_profile.json"

/* Path for named profiles list */
#define CAPTURE_PROFILES_PATH "/home/picam/.picam_profiles.json"
#define MAX_PROFILES 20

/* A named profile entry */
typedef struct {
    char name[64];
    capture_controls_t controls;
} named_profile_t;

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
    struct video_source *video_src;   /* video source for video_controls/reset endpoints */
    struct still_source *still_src;
    pthread_t server_thread;
    volatile bool running;

    /* Saved capture profile — used when /capture is called without params */
    capture_controls_t  saved_capture_profile;
    bool                profile_loaded;
    pthread_mutex_t     profile_mtx;

    /* Named profiles */
    named_profile_t     profiles[MAX_PROFILES];
    int                 profile_count;
    char                active_profile_name[64];
};

/**
 * @brief Creates and starts a new HTTP capture server.
 * @param port The port to listen on.
 * @param vid_src The video source from which to get the still capture interface.
 * @return A pointer to the server instance, or NULL on failure.
 */
struct http_server *http_capture_new(int port, struct video_source *video_src);

/**
 * @brief Stops and destroys the HTTP capture server.
 * @param server The server instance to destroy.
 */
void http_capture_destroy(struct http_server *server);

/* Profile persistence (JSON) */
int  capture_profile_save(const capture_controls_t *cc, const char *path);
int  capture_profile_load(capture_controls_t *cc, const char *path);

/* Multi-profile persistence */
int  profiles_save_all(const named_profile_t *profiles, int count, const char *path,
                       const char *active_name);
int  profiles_load_all(named_profile_t *profiles, int *count, int max, const char *path,
                       char *active_name, size_t active_name_sz);

#endif /* __CAPTURE_H__ */