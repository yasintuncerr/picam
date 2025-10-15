/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Abstract still source
 * Copyright (C) 2025 Yasin Tunçer
 *
 * Contact: Yasin Tunçer <yasintuncerr@gmail.com>
 */

#include "still-source.h"
#include <stdio.h> 
#include <stdlib.h>

void still_source_destroy(struct still_source *src)
{
    if (src)
        src->ops->destroy(src);
}

int still_source_set_format(struct still_source *src, struct v4l2_pix_format *fmt)
{
    return src->ops->set_format(src, fmt);
}

int still_source_alloc_buffer(struct still_source *src)
{
    return src->ops->alloc_buffer(src);
}

int still_source_free_buffer(struct still_source *src)
{
    return src->ops->free_buffer(src);
}

int still_source_capture(struct still_source *src)
{
    return src->ops->capture(src);
}

struct still_buffer *still_source_get_buffer(struct still_source *src)
{
    return src->ops->get_buffer(src);
}

static inline void print_still_buffer_info(const struct still_buffer *buffer) {
    if (!buffer) return;

    printf("--- Still Buffer Info (RGB) ---\n");
    printf("  Size: %u bytes\n", buffer->size);
    printf("  Bytes Used: %u bytes\n", buffer->bytesused);
    printf("  Dimensions: %u x %u\n", buffer->width, buffer->height);
    printf("  Stride: %u bytes\n", buffer->stride);
    printf("  Pixel Format: %.4s (0x%X)\n", (char*)&buffer->pixelformat, buffer->pixelformat);
    printf("  Timestamp: %ld.%06ld s\n", buffer->timestamp.tv_sec, (long)buffer->timestamp.tv_usec);
    printf("  Error: %s\n", buffer->error ? "Yes" : "No");
    printf("--------------------------------\n");
}
