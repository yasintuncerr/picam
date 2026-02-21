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

/* AWB modları — libcamera destekli tüm modlar + manual */
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

/*
 * Birleşik kamera kontrol yapısı.
 * HTTP parametrelerinden parse edilir, captureStill() ve video kontrolüne iletilir.
 *
 * Normalizasyon kuralları:
 *   exposure_us  : 0       → AE açık (otomatik)
 *                  >0      → manuel, mikrosaniye
 *   gain         : 0.0     → AG açık (otomatik)
 *                  >0.0    → manuel kazanç (ör. 1.0 … 16.0)
 *   awb_mode     : AWB_AUTO … AWB_MANUAL
 *   colour_gain_r/b : 0.0  → yok sayılır (sadece AWB_MANUAL'da kullanılır)
 *   brightness   : -999    → yok say (sentinel), aralık -100…+100, 0 = nötr
 *   contrast     : -1      → yok say, aralık 0…100, 50 = 1.0
 *   saturation   : -1      → yok say, aralık 0…100, 50 = 1.0
 *   sharpness    : -1      → yok say, aralık 0…100, 50 = 1.0
 *
 * Kullanıcı → libcamera dönüşümleri:
 *   brightness  : val/100.0              → [-1.0, +1.0]
 *   contrast    : val/50.0               → [ 0.0,  2.0]  (50 → 1.0)
 *   saturation  : val/50.0               → [ 0.0,  2.0]  (50 → 1.0)
 *   sharpness   : val/50.0               → [ 0.0,  2.0]  (50 → 1.0)
 */
typedef struct {
    /* Pozlama */
    int64_t  exposure_us;     /* 0 = AE auto */
    float    gain;            /* 0.0 = AG auto */

    /* Beyaz denge */
    awb_mode_t awb_mode;
    float    colour_gain_r;   /* AWB_MANUAL: kırmızı kazanç, ör. 2.0 */
    float    colour_gain_b;   /* AWB_MANUAL: mavi kazanç,  ör. 1.8 */

    /* Görüntü kalitesi  (-1 = ayarlanmamış, varsayılan kullanılır) */
    int      brightness;      /* -100 … +100,  0 nötr, -999 sentinel */
    int      contrast;        /*    0 … 100,  50 = 1.0 */
    int      saturation;      /*    0 … 100,  50 = 1.0 */
    int      sharpness;       /*    0 … 100,  50 = 1.0 */
} capture_controls_t;

/* Sıfır-başlatılmış, otomatik modda varsayılan kontroller */
static inline capture_controls_t capture_controls_default(void)
{
    capture_controls_t c;
    c.exposure_us   = 0;
    c.gain          = 0.0f;
    c.awb_mode      = AWB_AUTO;
    c.colour_gain_r = 0.0f;
    c.colour_gain_b = 0.0f;
    c.brightness    = -999; /* "ayarlanmamış" sentinel */
    c.contrast      = -1;
    c.saturation    = -1;
    c.sharpness     = -1;
    return c;
}

#ifdef __cplusplus
}
#endif
