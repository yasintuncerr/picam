/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * libcamera video source
 *
 * Copyright (C) 2022 Ideas on Board Oy.
 * Copyright (C) 2022 Kieran Bingham
 * Copyright (C) 2025 Yasin Tunçer
 */
#ifndef __LIBCAMERA_SOURCE_H__
#define __LIBCAMERA_SOURCE_H__

#include "video-source.h"

struct events;
struct still_source;
struct still_buffer;

#ifdef __cplusplus
extern "C" {
#endif

struct video_source *libcamera_source_create(const char *devname);
void libcamera_source_init(struct video_source *src, struct events *events);

struct still_source *libcamera_get_still_source(struct video_source *s);
void libcamera_still_source_set_callback(struct still_source *ssrc,
                                         void (*cb)(void *, struct still_buffer *),
                                         void *data);

#ifdef __cplusplus
} // extern "C"
#endif

#endif /* __LIBCAMERA_SOURCE_H__ */