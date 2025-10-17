/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * libcamera source C interface declarations
 *
 * Copyright (C) 2022 Ideas on Board Oy
 * Copyright (C) 2025 Yasin Tunçer
 *
 * These declarations allow C code to call libcamera-source functions
 */
#ifndef __LIBCAMERA_SOURCE_C_H__
#define __LIBCAMERA_SOURCE_C_H__

#ifdef HAVE_LIBCAMERA

#ifdef __cplusplus
extern "C" {
#endif

struct still_source;
struct still_buffer;
//struct video_source;

/* Get the still_source interface from a libcamera video_source */
struct still_source *libcamera_get_still_source(struct video_source *s);

/* Set a callback for when still capture is ready */
void libcamera_still_source_set_callback(struct still_source *ssrc,
                                         void (*cb)(void *, struct still_buffer *),
                                         void *data);

#ifdef __cplusplus
} // extern "C"
#endif

#endif /* HAVE_LIBCAMERA */

#endif /* __LIBCAMERA_SOURCE_C_H__ */
