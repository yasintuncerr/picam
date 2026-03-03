/* SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Unified camera control structure for capture and video endpoints.
 *
 * Copyright (C) 2025 Yasin Tunçer
 * Contact: Yasin Tunçer <yasintuncerr@gmail.com>
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AWB_AUTO         = 0,
    AWB_INCANDESCENT = 1,
    AWB_TUNGSTEN     = 2,
    AWB_FLUORESCENT  = 3,
    AWB_INDOOR       = 4,
    AWB_DAYLIGHT     = 5,
    AWB_CLOUDY       = 6,
    AWB_CUSTOM       = 7,
    AWB_MANUAL       = 8,   /* colour_gain_r / colour_gain_b manuel ayar */
} awb_mode_t;

typedef enum {
    CAPTURE_FMT_DNG  = 0,
    CAPTURE_FMT_JPEG = 1,
} capture_format_t;

typedef struct {
    int64_t  exposure_us;
    float    gain;
    awb_mode_t awb_mode;
    float    colour_gain_r;
    float    colour_gain_b;
    int      brightness;
    int      contrast;
    int      saturation;
    int      sharpness;
    capture_format_t format;
    int      jpeg_quality;
} capture_controls_t;

static inline capture_controls_t capture_controls_default(void)
{
    capture_controls_t c;
    c.exposure_us   = 0;
    c.gain          = 0.0f;
    c.awb_mode      = AWB_AUTO;
    c.colour_gain_r = 0.0f;
    c.colour_gain_b = 0.0f;
    c.brightness    = -999;
    c.contrast      = -1;
    c.saturation    = -1;
    c.sharpness     = -1;
    c.format        = CAPTURE_FMT_DNG;
    c.jpeg_quality  = 90;
    return c;
}

#ifdef __cplusplus
}
#endif
