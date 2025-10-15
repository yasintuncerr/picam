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
    unsigned int bit_depth;

    unsigned int black_level[4];
    unsigned int white_level;
    float white_balance_gains[3];
    float color_correction_matrix[9];
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

void print_still_buffer_info(const struct still_buffer *buffer);
#endif /* __STILL_SOURCE_H__ */