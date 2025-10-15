/* SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Abstract still source for RAW image capture with full metadata.
 *
 * Copyright (C) 2025 Yasin Tunçer
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

/**
 * @brief This structure holds the raw buffer and ALL metadata needed for processing.
 * This is the DEFINITIVE version for the RAW data pipeline.
 */
struct still_buffer {
    // Core buffer info
    unsigned int size;
    unsigned int bytesused;
    struct timeval timestamp;
    bool error;
    void *mem;

    // Essential image geometry info
    unsigned int width;
    unsigned int height;
    unsigned int stride; // CRITICAL for handling padded buffers from libcamera
    uint32_t pixelformat;

    // --- CRITICAL RAW METADATA for PATHOLOGY DATASET ---
    // These fields are NON-NEGOTIABLE for correct color reproduction.
    unsigned int bit_depth;               // e.g., 12 for 12-bit data
    unsigned int black_level[4];          // Sensor's black point for each color channel
    unsigned int white_level;             // Sensor's saturation point
    float white_balance_gains[3];         // To correct the color cast of the light source
    float color_correction_matrix[9];     // To convert sensor colors to standard sRGB colors
};

struct still_source_ops {
    void(*destroy)(struct still_source *src);
    int(*set_format)(struct still_source *src, struct v4l2_pix_format *fmt);
    int(*alloc_buffer)(struct still_source *src);
    int(*free_buffer)(struct still_source *src);
    int(*capture)(struct still_source *src);
    struct still_buffer *(*get_buffer)(struct still_source *src);
};

struct still_source {
    const struct still_source_ops *ops;
};


// --- Interface Functions ---

void still_source_destroy(struct still_source *src);
int still_source_set_format(struct still_source *src, struct v4l2_pix_format *fmt);
int still_source_alloc_buffer(struct still_source *src);
int still_source_free_buffer(struct still_source *src);
int still_source_capture(struct still_source *src);
struct still_buffer *still_source_get_buffer(struct still_source *src);

#endif /* __STILL_SOURCE_H__ */