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


void print_still_buffer_info(const struct still_buffer *buffer) {
    if (buffer == NULL) {
        printf("Error: Buffer is NULL.\n");
        return;
    }

    printf("Still Buffer Info:\n");
    printf(" Size: %u bytes\n", buffer->size);
    printf(" Bytes Used: %u bytes\n", buffer->bytesused);
    // timeval long türünde olduğu için %ld format belirleyicisi kullanılır.
    printf(" Timestamp: %ld.%06ld seconds\n", buffer->timestamp.tv_sec, (long)buffer->timestamp.tv_usec);
    printf(" Error: %s\n", buffer->error ? "Yes" : "No");
    printf(" Width: %u pixels\n", buffer->width);
    printf(" Height: %u pixels\n", buffer->height);
    printf(" Pixel Format: 0x%X\n", buffer->pixelformat);
    printf(" Bit Depth: %u bits\n", buffer->bit_depth);

    printf(" Black Levels: R=%u, G1=%u, G2=%u, B=%u\n",
           buffer->black_level[0], buffer->black_level[1], buffer->black_level[2], buffer->black_level[3]);
    printf(" White Level: %u\n", buffer->white_level);
    printf(" White Balance Gains: R=%f, G=%f, B=%f\n",
           buffer->white_balance_gains[0], buffer->white_balance_gains[1], buffer->white_balance_gains[2]);
    printf(" Color Correction Matrix:\n");
    for (int i = 0; i < 3; ++i) {
        printf("  [%f, %f, %f]\n",
               buffer->color_correction_matrix[i * 3],
               buffer->color_correction_matrix[i * 3 + 1],
               buffer->color_correction_matrix[i * 3 + 2]);
    }
}
