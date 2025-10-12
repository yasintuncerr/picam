/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Abstract still source
 * Copyright (C) 2025 Yasin Tunçer
 *
 * Contact: Yasin Tunçer <yasintuncerr@gmail.com>
 */

#ifndef __STILL_SOURCE_H__
#define __STILL_SOURCE_H__

#include <stdbool.h>
#include <stddef.h>
#include <sys/time.h>
#include <stdint.h>


struct v4l2_pix_format;
struct still_source;
struct still_buffer;

struct still_source_ops {
    void(*destroy)(struct still_source *src);
    int(*set_format)(struct still_source *src, struct v4l2_pix_format *fmt);
    int(*alloc_buffer)(struct still_source *src);
    int(*free_buffer)(struct still_source *src);
    int(*capture)(struct still_source *src);
    struct still_buffer *(*get_buffer)(struct still_source *src);
};

struct still_buffer {
    unsigned int size;
    unsigned int bytesused;
    struct timeval timestamp;
    bool error;
    void *mem;
    unsigned int width;
    unsigned int height;
    uint32_t pixelformat;
};


struct still_source {
    const struct still_source_ops *ops;
    
};


void still_source_destroy(struct still_source *src);
int still_source_set_format(struct still_source *src, struct v4l2_pix_format *fmt);
int still_source_alloc_buffer(struct still_source *src);
int still_source_free_buffer(struct still_source *src);
int still_source_capture(struct still_source *src);
struct still_buffer *still_source_get_buffer(struct still_source *src);

#endif /* __STILL_SOURCE_H__ */