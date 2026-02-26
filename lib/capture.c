/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * An HTTP server that serves DNG or JPEG captures, manages video controls,
 * and persists capture profiles.
 *
 * Endpoints:
 *   GET /capture              — DNG or JPEG capture (uses saved profile if no params)
 *   GET /video_controls?...   — Apply video (viewfinder) controls
 *   GET /reset_controls       — Reset video controls to defaults
 *   GET /capture_controls?... — Save capture profile (with params) or get current (no params)
 *   GET /capture_controls/reset — Reset capture profile to factory defaults
 *   GET /test_capture         — Quick JPEG test capture using saved profile
 *
 * Copyright (C) 2025 Yasin Tunçer
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   /* strncasecmp */
#include <sys/socket.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdint.h>

#include "config.h"
#include "capture.h"
#include "capture_controls.h"
#include "still-source.h"
#include "video-source.h"

#ifdef HAVE_LIBCAMERA
// libcamera C++ interface
extern struct still_source *libcamera_get_still_source(struct video_source *s);
extern void libcamera_still_source_set_callback(struct still_source *ssrc,
                                                void (*cb)(void *, struct still_buffer *),
                                                void *data);
extern int libcamera_apply_video_controls(struct video_source *s,
                                           const capture_controls_t *cc);
extern int libcamera_reset_controls(struct video_source *s);
extern int libcamera_capture_jpeg(struct video_source *s,
                                   void **out_buf, size_t *out_size,
                                   int quality);
extern void libcamera_get_camera_status(struct video_source *s,
                                         int64_t *out_exposure_us,
                                         float *out_gain);
#endif

// Forward declarations
static void *client_thread_func(void *arg);
static void *http_server_thread(void *arg);
static void still_capture_ready_cb(void *data, struct still_buffer *buffer);

/* ─────────────────────────────────────────────────────────────────────────
 * find_query_param()
 *
 * HTTP isteğinin request line'ından (ör. "GET /capture?gain=2.0&awb=cloudy HTTP/1.1")
 * istenen parametreyi bulur ve değerinin başına işaret eden pointer döndürür.
 * Bulunamazsa NULL.
 * ───────────────────────────────────────────────────────────────────────── */
static const char *find_query_param(const char *request_line, const char *key)
{
    const char *qs = strchr(request_line, '?');
    if (!qs) return NULL;
    qs++;  /* '?' sonrası */

    size_t klen = strlen(key);
    const char *p = qs;

    while (*p && *p != ' ' && *p != '\r' && *p != '\n') {
        /* Her parametre 'key=value' biçimindedir; '&' ile ayrılır */
        if (strncasecmp(p, key, klen) == 0 && p[klen] == '=') {
            return p + klen + 1;  /* değerin başı */
        }
        /* Sonraki '&'ye atla */
        const char *amp = strchr(p, '&');
        if (!amp) break;
        p = amp + 1;
    }
    return NULL;
}

/* ─────────────────────────────────────────────────────────────────────────
 * has_any_query_params()
 *
 * Request line'da '?' var mı kontrol eder — parametre gönderilip
 * gönderilmediğini anlamak için.
 * ───────────────────────────────────────────────────────────────────────── */
static bool has_any_query_params(const char *request_line)
{
    const char *qs = strchr(request_line, '?');
    if (!qs) return false;
    /* '?' sonrasında en az bir karakter ve boşluk veya satır sonu değil */
    qs++;
    return (*qs && *qs != ' ' && *qs != '\r' && *qs != '\n');
}

/* ─────────────────────────────────────────────────────────────────────────
 * parse_controls_from_request()
 *
 * HTTP request line'ından capture_controls_t doldurur.
 * Desteklenen parametreler:
 *
 *   exposure=<us>          0 veya yok → AE otomatik
 *   gain=<float>           0 veya yok → AG otomatik
 *   awb=auto|incandescent|tungsten|fluorescent|indoor|daylight|cloudy|custom|manual
 *   colour_gain_r=<float>  (awb=manual ile birlikte)
 *   colour_gain_b=<float>  (awb=manual ile birlikte)
 *   brightness=<-100..100> 0 nötr, yok → değiştirilmez
 *   contrast=<0..100>      50 = varsayılan (1.0), yok → değiştirilmez
 *   saturation=<0..100>    50 = varsayılan (1.0), yok → değiştirilmez
 *   sharpness=<0..100>     50 = varsayılan (1.0), yok → değiştirilmez
 *   format=dng|jpeg        varsayılan: dng
 *   quality=<1..100>       JPEG kalitesi, varsayılan: 90
 * ───────────────────────────────────────────────────────────────────────── */
static capture_controls_t parse_controls_from_request(const char *request_line)
{
    capture_controls_t cc = capture_controls_default();
    const char *v;

    /* exposure */
    v = find_query_param(request_line, "exposure");
    if (v) {
        long us = strtol(v, NULL, 10);
        cc.exposure_us = (us > 0) ? (int64_t)us : 0;
    }

    /* gain — "gain=" ≠ "colour_gain_r=" (tam anahtar eşleşmesi) */
    v = find_query_param(request_line, "gain");
    if (v) {
        float g = strtof(v, NULL);
        cc.gain = (g > 0.0f) ? g : 0.0f;
    }

    /* awb modu */
    v = find_query_param(request_line, "awb");
    if (v) {
        if      (strncasecmp(v, "incandescent", 12) == 0) cc.awb_mode = AWB_INCANDESCENT;
        else if (strncasecmp(v, "tungsten",      8) == 0) cc.awb_mode = AWB_TUNGSTEN;
        else if (strncasecmp(v, "fluorescent",  11) == 0) cc.awb_mode = AWB_FLUORESCENT;
        else if (strncasecmp(v, "indoor",        6) == 0) cc.awb_mode = AWB_INDOOR;
        else if (strncasecmp(v, "daylight",      8) == 0) cc.awb_mode = AWB_DAYLIGHT;
        else if (strncasecmp(v, "cloudy",        6) == 0) cc.awb_mode = AWB_CLOUDY;
        else if (strncasecmp(v, "custom",        6) == 0) cc.awb_mode = AWB_CUSTOM;
        else if (strncasecmp(v, "manual",        6) == 0) cc.awb_mode = AWB_MANUAL;
        else                                               cc.awb_mode = AWB_AUTO;
    }

    /* colour gains (yalnızca awb=manual ile anlamlı) */
    v = find_query_param(request_line, "colour_gain_r");
    if (v) cc.colour_gain_r = strtof(v, NULL);

    v = find_query_param(request_line, "colour_gain_b");
    if (v) cc.colour_gain_b = strtof(v, NULL);

    /* brightness: -100 … +100 */
    v = find_query_param(request_line, "brightness");
    if (v) {
        int bv = (int)strtol(v, NULL, 10);
        cc.brightness = (bv < -100) ? -100 : (bv > 100) ? 100 : bv;
    }

    /* contrast: 0 … 100 */
    v = find_query_param(request_line, "contrast");
    if (v) {
        int cv = (int)strtol(v, NULL, 10);
        cc.contrast = (cv < 0) ? 0 : (cv > 100) ? 100 : cv;
    }

    /* saturation: 0 … 100 */
    v = find_query_param(request_line, "saturation");
    if (v) {
        int sv = (int)strtol(v, NULL, 10);
        cc.saturation = (sv < 0) ? 0 : (sv > 100) ? 100 : sv;
    }

    /* sharpness: 0 … 100 */
    v = find_query_param(request_line, "sharpness");
    if (v) {
        int shv = (int)strtol(v, NULL, 10);
        cc.sharpness = (shv < 0) ? 0 : (shv > 100) ? 100 : shv;
    }

    /* format: dng | jpeg */
    v = find_query_param(request_line, "format");
    if (v) {
        if (strncasecmp(v, "jpeg", 4) == 0 || strncasecmp(v, "jpg", 3) == 0)
            cc.format = CAPTURE_FMT_JPEG;
        else
            cc.format = CAPTURE_FMT_DNG;
    }

    /* quality: 1 … 100 */
    v = find_query_param(request_line, "quality");
    if (v) {
        int q = (int)strtol(v, NULL, 10);
        cc.jpeg_quality = (q < 1) ? 1 : (q > 100) ? 100 : q;
    }

    return cc;
}


// Callback to receive the final DNG data from the C++ side.
static void still_capture_ready_cb(void *data, struct still_buffer *buffer_from_camera) {
    struct http_client_session *session = (struct http_client_session *)data;

    pthread_mutex_lock(&session->mtx);
    session->captured_data.mem = NULL;
    session->captured_data.error = true;

    if (buffer_from_camera && !buffer_from_camera->error && buffer_from_camera->mem && buffer_from_camera->bytesused > 0) {
        session->captured_data.mem = malloc(buffer_from_camera->bytesused);
        if (session->captured_data.mem) {
            memcpy(session->captured_data.mem, buffer_from_camera->mem, buffer_from_camera->bytesused);

            session->captured_data.bytesused = buffer_from_camera->bytesused;
            session->captured_data.timestamp = buffer_from_camera->timestamp;
            session->captured_data.error = false;
        } else {
            fprintf(stderr, "Failed to allocate memory for client DNG buffer\n");
        }
    }

    session->capture_complete = true;
    pthread_cond_signal(&session->cond);
    pthread_mutex_unlock(&session->mtx);
}

/* ─────────────────────────────────────────────────────────────────────────
 * send_json_response() — yardımcı: basit JSON yanıt gönder
 * ───────────────────────────────────────────────────────────────────────── */
static void send_json_response(int fd, int status_code, const char *status_text,
                                const char *json_body)
{
    char header[512];
    int body_len = (int)strlen(json_body);
    int hlen = snprintf(header, sizeof(header),
                        "HTTP/1.1 %d %s\r\n"
                        "Content-Type: application/json\r\n"
                        "Access-Control-Allow-Origin: *\r\n"
                        "Access-Control-Allow-Methods: GET, OPTIONS\r\n"
                        "Content-Length: %d\r\n\r\n",
                        status_code, status_text, body_len);
    write(fd, header, hlen);
    write(fd, json_body, body_len);
}

/* ─────────────────────────────────────────────────────────────────────────
 * awb_mode_to_string() — AWB modunu string'e çevirir
 * ───────────────────────────────────────────────────────────────────────── */
static const char *awb_mode_to_string(awb_mode_t mode)
{
    switch (mode) {
    case AWB_INCANDESCENT: return "incandescent";
    case AWB_TUNGSTEN:     return "tungsten";
    case AWB_FLUORESCENT:  return "fluorescent";
    case AWB_INDOOR:       return "indoor";
    case AWB_DAYLIGHT:     return "daylight";
    case AWB_CLOUDY:       return "cloudy";
    case AWB_CUSTOM:       return "custom";
    case AWB_MANUAL:       return "manual";
    case AWB_AUTO:
    default:               return "auto";
    }
}

/* ─────────────────────────────────────────────────────────────────────────
 * send_profile_json() — Mevcut profili JSON olarak HTTP yanıtında döner
 * ───────────────────────────────────────────────────────────────────────── */
static void send_profile_json(int fd, const capture_controls_t *cc)
{
    char body[1024];
    snprintf(body, sizeof(body),
        "{"
        "\"exposure_us\":%lld,"
        "\"gain\":%.2f,"
        "\"awb_mode\":\"%s\","
        "\"colour_gain_r\":%.2f,"
        "\"colour_gain_b\":%.2f,"
        "\"brightness\":%d,"
        "\"contrast\":%d,"
        "\"saturation\":%d,"
        "\"sharpness\":%d,"
        "\"format\":\"%s\","
        "\"jpeg_quality\":%d"
        "}",
        (long long)cc->exposure_us,
        cc->gain,
        awb_mode_to_string(cc->awb_mode),
        cc->colour_gain_r,
        cc->colour_gain_b,
        cc->brightness,
        cc->contrast,
        cc->saturation,
        cc->sharpness,
        (cc->format == CAPTURE_FMT_JPEG) ? "jpeg" : "dng",
        cc->jpeg_quality);

    send_json_response(fd, 200, "OK", body);
}

/* ─────────────────────────────────────────────────────────────────────────
 * Profile JSON persistence
 * ───────────────────────────────────────────────────────────────────────── */
int capture_profile_save(const capture_controls_t *cc, const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "capture_profile_save: cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }
    fprintf(f,
        "{\n"
        "  \"exposure_us\": %lld,\n"
        "  \"gain\": %.4f,\n"
        "  \"awb_mode\": %d,\n"
        "  \"colour_gain_r\": %.4f,\n"
        "  \"colour_gain_b\": %.4f,\n"
        "  \"brightness\": %d,\n"
        "  \"contrast\": %d,\n"
        "  \"saturation\": %d,\n"
        "  \"sharpness\": %d,\n"
        "  \"format\": %d,\n"
        "  \"jpeg_quality\": %d\n"
        "}\n",
        (long long)cc->exposure_us, cc->gain, (int)cc->awb_mode,
        cc->colour_gain_r, cc->colour_gain_b,
        cc->brightness, cc->contrast, cc->saturation, cc->sharpness,
        (int)cc->format, cc->jpeg_quality);
    fclose(f);
    fprintf(stdout, "Capture profile saved to %s\n", path);
    return 0;
}

int capture_profile_load(capture_controls_t *cc, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;  /* dosya yok → varsayılan kullanılır */

    int awb_int = 0, fmt_int = 0;
    long long exp_ll = 0;

    /* Basit fscanf ile JSON parse (güvenli, bilinen format) */
    int matched = fscanf(f,
        " { \"exposure_us\" : %lld ,"
        " \"gain\" : %f ,"
        " \"awb_mode\" : %d ,"
        " \"colour_gain_r\" : %f ,"
        " \"colour_gain_b\" : %f ,"
        " \"brightness\" : %d ,"
        " \"contrast\" : %d ,"
        " \"saturation\" : %d ,"
        " \"sharpness\" : %d ,"
        " \"format\" : %d ,"
        " \"jpeg_quality\" : %d",
        &exp_ll, &cc->gain, &awb_int,
        &cc->colour_gain_r, &cc->colour_gain_b,
        &cc->brightness, &cc->contrast, &cc->saturation, &cc->sharpness,
        &fmt_int, &cc->jpeg_quality);
    fclose(f);

    if (matched >= 11) {
        cc->exposure_us = (int64_t)exp_ll;
        cc->awb_mode = (awb_mode_t)awb_int;
        cc->format = (capture_format_t)fmt_int;
        fprintf(stdout, "Capture profile loaded from %s\n", path);
        return 0;
    }
    return -1;
}

/* ─────────────────────────────────────────────────────────────────────────
 * Multi-profile persistence: JSON array of named profiles
 *
 * Format:
 * [{"name":"...","exposure_us":0,"gain":0.0,...}, ...]
 * ───────────────────────────────────────────────────────────────────────── */

/* URL decode helper (in-place) — decodes %XX and '+' */
static void url_decode(char *dst, const char *src, size_t dst_size)
{
    size_t di = 0;
    while (*src && di < dst_size - 1) {
        if (*src == '%' && src[1] && src[2]) {
            char hex[3] = { src[1], src[2], 0 };
            dst[di++] = (char)strtol(hex, NULL, 16);
            src += 3;
        } else if (*src == '+') {
            dst[di++] = ' ';
            src++;
        } else {
            dst[di++] = *src++;
        }
    }
    dst[di] = '\0';
}

/* Extract param value into buf (URL-decoded, stops at '&' or space) */
static int extract_param_str(const char *request_line, const char *key, char *buf, size_t bufsz)
{
    const char *v = find_query_param(request_line, key);
    if (!v) return -1;
    /* Copy until '&', ' ', '\r', '\n' or end */
    char raw[256];
    size_t i = 0;
    while (v[i] && v[i] != '&' && v[i] != ' ' && v[i] != '\r' && v[i] != '\n' && i < sizeof(raw)-1) {
        raw[i] = v[i]; i++;
    }
    raw[i] = '\0';
    url_decode(buf, raw, bufsz);
    return 0;
}

int profiles_save_all(const named_profile_t *profiles, int count, const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "profiles_save_all: cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }
    fprintf(f, "[\n");
    for (int i = 0; i < count; i++) {
        const capture_controls_t *cc = &profiles[i].controls;
        fprintf(f,
            "  {\"name\":\"%s\","
            "\"exposure_us\":%lld,"
            "\"gain\":%.4f,"
            "\"awb_mode\":%d,"
            "\"colour_gain_r\":%.4f,"
            "\"colour_gain_b\":%.4f,"
            "\"brightness\":%d,"
            "\"contrast\":%d,"
            "\"saturation\":%d,"
            "\"sharpness\":%d,"
            "\"format\":%d,"
            "\"jpeg_quality\":%d}%s\n",
            profiles[i].name,
            (long long)cc->exposure_us, cc->gain, (int)cc->awb_mode,
            cc->colour_gain_r, cc->colour_gain_b,
            cc->brightness, cc->contrast, cc->saturation, cc->sharpness,
            (int)cc->format, cc->jpeg_quality,
            (i < count - 1) ? "," : "");
    }
    fprintf(f, "]\n");
    fclose(f);
    fprintf(stdout, "Profiles saved to %s (%d profiles)\n", path, count);
    return 0;
}

int profiles_load_all(named_profile_t *profiles, int *count, int max, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;

    /* Read entire file */
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize <= 2 || fsize > 65536) { fclose(f); return -1; }

    char *buf = (char *)malloc(fsize + 1);
    if (!buf) { fclose(f); return -1; }
    fread(buf, 1, fsize, f);
    buf[fsize] = '\0';
    fclose(f);

    /* Simple JSON array parser — expects [{...},{...},...] */
    int n = 0;
    char *p = buf;

    while (n < max) {
        /* Find next '{' */
        p = strchr(p, '{');
        if (!p) break;

        named_profile_t *np = &profiles[n];
        capture_controls_t *cc = &np->controls;
        *cc = capture_controls_default();
        np->name[0] = '\0';

        /* Parse name */
        char *nm = strstr(p, "\"name\":");
        if (nm && nm < strchr(p, '}')) {
            nm = strchr(nm + 7, '"');
            if (nm) {
                nm++;
                char *ne = strchr(nm, '"');
                if (ne) {
                    size_t len = (size_t)(ne - nm);
                    if (len >= sizeof(np->name)) len = sizeof(np->name) - 1;
                    memcpy(np->name, nm, len);
                    np->name[len] = '\0';
                }
            }
        }

        /* Parse numeric fields with sscanf on the object substring */
        char *end_brace = strchr(p, '}');
        if (!end_brace) break;

        long long exp_ll = 0;
        int awb_int = 0, fmt_int = 0;

        /* Scan individual fields */
        char *fld;
        fld = strstr(p, "\"exposure_us\":");
        if (fld && fld < end_brace) sscanf(fld + 14, "%lld", &exp_ll);
        cc->exposure_us = (int64_t)exp_ll;

        fld = strstr(p, "\"gain\":");
        if (fld && fld < end_brace) sscanf(fld + 7, "%f", &cc->gain);

        fld = strstr(p, "\"awb_mode\":");
        if (fld && fld < end_brace) { sscanf(fld + 11, "%d", &awb_int); cc->awb_mode = (awb_mode_t)awb_int; }

        fld = strstr(p, "\"colour_gain_r\":");
        if (fld && fld < end_brace) sscanf(fld + 16, "%f", &cc->colour_gain_r);

        fld = strstr(p, "\"colour_gain_b\":");
        if (fld && fld < end_brace) sscanf(fld + 16, "%f", &cc->colour_gain_b);

        fld = strstr(p, "\"brightness\":");
        if (fld && fld < end_brace) sscanf(fld + 13, "%d", &cc->brightness);

        fld = strstr(p, "\"contrast\":");
        if (fld && fld < end_brace) sscanf(fld + 11, "%d", &cc->contrast);

        fld = strstr(p, "\"saturation\":");
        if (fld && fld < end_brace) sscanf(fld + 13, "%d", &cc->saturation);

        fld = strstr(p, "\"sharpness\":");
        if (fld && fld < end_brace) sscanf(fld + 12, "%d", &cc->sharpness);

        fld = strstr(p, "\"format\":");
        if (fld && fld < end_brace) { sscanf(fld + 9, "%d", &fmt_int); cc->format = (capture_format_t)fmt_int; }

        fld = strstr(p, "\"jpeg_quality\":");
        if (fld && fld < end_brace) sscanf(fld + 15, "%d", &cc->jpeg_quality);

        if (np->name[0]) n++;  /* Only count entries with a name */
        p = end_brace + 1;
    }

    free(buf);
    *count = n;
    fprintf(stdout, "Profiles loaded from %s (%d profiles)\n", path, n);
    return 0;
}

/* ─────────────────────────────────────────────────────────────────────────
 * handle_profiles() — GET /profiles — list all named profiles as JSON
 * ───────────────────────────────────────────────────────────────────────── */
static void handle_profiles_list(struct http_client_session *session)
{
    pthread_mutex_lock(&session->server->profile_mtx);
    int cnt = session->server->profile_count;
    const char *active = session->server->active_profile_name;

    /* Build JSON array */
    char *body = (char *)malloc(cnt * 512 + 256);
    if (!body) {
        pthread_mutex_unlock(&session->server->profile_mtx);
        send_json_response(session->fd, 500, "Internal Server Error",
                           "{\"status\":\"error\",\"message\":\"OOM\"}");
        return;
    }

    int off = 0;
    off += sprintf(body + off, "{\"active\":\"%s\",\"profiles\":[", active);
    for (int i = 0; i < cnt; i++) {
        const named_profile_t *np = &session->server->profiles[i];
        const capture_controls_t *cc = &np->controls;
        off += sprintf(body + off,
            "{\"name\":\"%s\","
            "\"exposure_us\":%lld,"
            "\"gain\":%.2f,"
            "\"awb_mode\":\"%s\","
            "\"brightness\":%d,"
            "\"contrast\":%d,"
            "\"saturation\":%d,"
            "\"sharpness\":%d,"
            "\"format\":\"%s\","
            "\"jpeg_quality\":%d}%s",
            np->name,
            (long long)cc->exposure_us, cc->gain,
            awb_mode_to_string(cc->awb_mode),
            cc->brightness, cc->contrast, cc->saturation, cc->sharpness,
            (cc->format == CAPTURE_FMT_JPEG) ? "jpeg" : "dng",
            cc->jpeg_quality,
            (i < cnt - 1) ? "," : "");
    }
    off += sprintf(body + off, "]}");
    pthread_mutex_unlock(&session->server->profile_mtx);

    send_json_response(session->fd, 200, "OK", body);
    free(body);
}

/* ─────────────────────────────────────────────────────────────────────────
 * handle_profiles_save() — GET /profiles/save?name=X&... — save/update
 * ───────────────────────────────────────────────────────────────────────── */
static void handle_profiles_save(struct http_client_session *session, const char *request_buf)
{
    char name[64];
    if (extract_param_str(request_buf, "name", name, sizeof(name)) != 0 || !name[0]) {
        send_json_response(session->fd, 400, "Bad Request",
                           "{\"status\":\"error\",\"message\":\"Missing 'name' parameter\"}");
        return;
    }

    capture_controls_t cc = parse_controls_from_request(request_buf);

    pthread_mutex_lock(&session->server->profile_mtx);

    /* Find existing or add new */
    int idx = -1;
    for (int i = 0; i < session->server->profile_count; i++) {
        if (strcmp(session->server->profiles[i].name, name) == 0) {
            idx = i;
            break;
        }
    }

    if (idx >= 0) {
        /* Update existing */
        session->server->profiles[idx].controls = cc;
    } else if (session->server->profile_count < MAX_PROFILES) {
        /* Add new */
        idx = session->server->profile_count++;
        strncpy(session->server->profiles[idx].name, name, sizeof(session->server->profiles[idx].name) - 1);
        session->server->profiles[idx].name[sizeof(session->server->profiles[idx].name) - 1] = '\0';
        session->server->profiles[idx].controls = cc;
    } else {
        pthread_mutex_unlock(&session->server->profile_mtx);
        send_json_response(session->fd, 400, "Bad Request",
                           "{\"status\":\"error\",\"message\":\"Max profiles reached (20)\"}");
        return;
    }

    /* Also set as active */
    session->server->saved_capture_profile = cc;
    strncpy(session->server->active_profile_name, name, sizeof(session->server->active_profile_name) - 1);
    session->server->active_profile_name[sizeof(session->server->active_profile_name) - 1] = '\0';

    /* Persist both */
    profiles_save_all(session->server->profiles, session->server->profile_count, CAPTURE_PROFILES_PATH);
    capture_profile_save(&cc, CAPTURE_PROFILE_PATH);

    pthread_mutex_unlock(&session->server->profile_mtx);

    send_json_response(session->fd, 200, "OK",
                       "{\"status\":\"ok\",\"message\":\"Profile saved\"}");
}

/* ─────────────────────────────────────────────────────────────────────────
 * handle_profiles_delete() — GET /profiles/delete?name=X
 * ───────────────────────────────────────────────────────────────────────── */
static void handle_profiles_delete(struct http_client_session *session, const char *request_buf)
{
    char name[64];
    if (extract_param_str(request_buf, "name", name, sizeof(name)) != 0 || !name[0]) {
        send_json_response(session->fd, 400, "Bad Request",
                           "{\"status\":\"error\",\"message\":\"Missing 'name' parameter\"}");
        return;
    }

    pthread_mutex_lock(&session->server->profile_mtx);

    int idx = -1;
    for (int i = 0; i < session->server->profile_count; i++) {
        if (strcmp(session->server->profiles[i].name, name) == 0) {
            idx = i;
            break;
        }
    }

    if (idx < 0) {
        pthread_mutex_unlock(&session->server->profile_mtx);
        send_json_response(session->fd, 404, "Not Found",
                           "{\"status\":\"error\",\"message\":\"Profile not found\"}");
        return;
    }

    /* Shift remaining profiles */
    for (int i = idx; i < session->server->profile_count - 1; i++) {
        session->server->profiles[i] = session->server->profiles[i + 1];
    }
    session->server->profile_count--;

    /* If active profile was deleted, reset to defaults */
    bool was_active = (strcmp(session->server->active_profile_name, name) == 0);
    if (was_active) {
        session->server->saved_capture_profile = capture_controls_default();
        session->server->active_profile_name[0] = '\0';
        capture_profile_save(&session->server->saved_capture_profile, CAPTURE_PROFILE_PATH);
    }

    profiles_save_all(session->server->profiles, session->server->profile_count, CAPTURE_PROFILES_PATH);
    pthread_mutex_unlock(&session->server->profile_mtx);

    if (was_active) {
        send_json_response(session->fd, 200, "OK",
                           "{\"status\":\"ok\",\"message\":\"Active profile deleted, reset to defaults\",\"reset\":true}");
    } else {
        send_json_response(session->fd, 200, "OK",
                           "{\"status\":\"ok\",\"message\":\"Profile deleted\",\"reset\":false}");
    }
}

/* ─────────────────────────────────────────────────────────────────────────
 * handle_profiles_load() — GET /profiles/load?name=X — set as active
 * ───────────────────────────────────────────────────────────────────────── */
static void handle_profiles_load_by_name(struct http_client_session *session, const char *request_buf)
{
    char name[64];
    if (extract_param_str(request_buf, "name", name, sizeof(name)) != 0 || !name[0]) {
        send_json_response(session->fd, 400, "Bad Request",
                           "{\"status\":\"error\",\"message\":\"Missing 'name' parameter\"}");
        return;
    }

    pthread_mutex_lock(&session->server->profile_mtx);

    int idx = -1;
    for (int i = 0; i < session->server->profile_count; i++) {
        if (strcmp(session->server->profiles[i].name, name) == 0) {
            idx = i;
            break;
        }
    }

    if (idx < 0) {
        pthread_mutex_unlock(&session->server->profile_mtx);
        send_json_response(session->fd, 404, "Not Found",
                           "{\"status\":\"error\",\"message\":\"Profile not found\"}");
        return;
    }

    session->server->saved_capture_profile = session->server->profiles[idx].controls;
    strncpy(session->server->active_profile_name, name, sizeof(session->server->active_profile_name) - 1);
    session->server->active_profile_name[sizeof(session->server->active_profile_name) - 1] = '\0';

    capture_profile_save(&session->server->saved_capture_profile, CAPTURE_PROFILE_PATH);
    pthread_mutex_unlock(&session->server->profile_mtx);

    send_json_response(session->fd, 200, "OK",
                       "{\"status\":\"ok\",\"message\":\"Profile loaded\"}");
}

/* ─────────────────────────────────────────────────────────────────────────
 * handle_capture() — /capture endpoint
 * ───────────────────────────────────────────────────────────────────────── */
static void handle_capture(struct http_client_session *session, const char *request_buf)
{
#ifdef HAVE_LIBCAMERA
    if (!session->server->still_src) {
        const char *response = "HTTP/1.1 503 Service Unavailable\r\n"
                          "Access-Control-Allow-Origin: *\r\n"
                          "Access-Control-Allow-Methods: GET, OPTIONS\r\n"
                          "\r\n";
        write(session->fd, response, strlen(response));
        return;
    }

    /* Parametre var mı? Yoksa saved profile kullan */
    capture_controls_t cc;
    if (has_any_query_params(request_buf)) {
        cc = parse_controls_from_request(request_buf);
    } else {
        pthread_mutex_lock(&session->server->profile_mtx);
        cc = session->server->saved_capture_profile;
        pthread_mutex_unlock(&session->server->profile_mtx);
    }

    /* ─── JPEG modu: video encoder'dan al ─── */
    if (cc.format == CAPTURE_FMT_JPEG) {
        void *jpeg_buf = NULL;
        size_t jpeg_size = 0;
        int quality = cc.jpeg_quality > 0 ? cc.jpeg_quality : 90;

        fprintf(stdout, "HTTP capture: JPEG mode, quality=%d\n", quality);

        int ret = libcamera_capture_jpeg(session->server->video_src, &jpeg_buf, &jpeg_size, quality);
        if (ret == 0 && jpeg_buf && jpeg_size > 0) {
            char http_header[512];
            int len = snprintf(http_header, sizeof(http_header),
                      "HTTP/1.1 200 OK\r\n"
                      "Content-Type: image/jpeg\r\n"
                      "Content-Disposition: inline; filename=\"capture_%ld.jpg\"\r\n"
                      "Access-Control-Allow-Origin: *\r\n"
                      "Content-Length: %zu\r\n\r\n",
                      (long)time(NULL), jpeg_size);

            write(session->fd, http_header, len);
            write(session->fd, jpeg_buf, jpeg_size);
            free(jpeg_buf);
        } else {
            const char *response = "HTTP/1.1 500 Internal Server Error\r\n"
                              "Access-Control-Allow-Origin: *\r\n"
                              "\r\n"
                              "JPEG capture failed";
            write(session->fd, response, strlen(response));
            if (jpeg_buf) free(jpeg_buf);
        }
        return;
    }

    /* ─── DNG modu: mevcut pipeline ─── */
    if (cc.exposure_us > 0) {
        fprintf(stdout, "HTTP capture: Manual exposure: %lld us\n", (long long)cc.exposure_us);
    }
    if (cc.gain > 0.0f) {
        fprintf(stdout, "HTTP capture: Manual gain: %.2f\n", cc.gain);
    }
    if (cc.awb_mode != AWB_AUTO) {
        fprintf(stdout, "HTTP capture: AWB mode: %d\n", cc.awb_mode);
    }

    libcamera_still_source_set_callback(session->server->still_src, still_capture_ready_cb, session);

    if (still_source_capture(session->server->still_src, &cc) < 0) {
        const char *response = "HTTP/1.1 500 Internal Server Error\r\n\r\nCapture trigger failed";
        write(session->fd, response, strlen(response));
    } else {
        pthread_mutex_lock(&session->mtx);
        while (!session->capture_complete) {
            pthread_cond_wait(&session->cond, &session->mtx);
        }
        
        if (session->captured_data.mem && !session->captured_data.error) {
            char http_header[512];
            int len = snprintf(http_header, sizeof(http_header),
                      "HTTP/1.1 200 OK\r\n"
                      "Content-Type: image/dng\r\n"
                      "Content-Disposition: attachment; filename=\"capture.dng\"\r\n"
                      "Access-Control-Allow-Origin: *\r\n"
                      "Content-Length: %u\r\n\r\n",
                      session->captured_data.bytesused);

            write(session->fd, http_header, len);
            write(session->fd, session->captured_data.mem, session->captured_data.bytesused);
        } else {
            const char *response = "HTTP/1.1 500 Internal Server Error\r\n"
                              "Access-Control-Allow-Origin: *\r\n"
                              "\r\n"
                              "Capture or DNG creation failed";

            write(session->fd, response, strlen(response));
        }
        pthread_mutex_unlock(&session->mtx);
    }
#else
    const char *response = "HTTP/1.1 501 Not Implemented\r\n\r\nCapture requires libcamera support";
    write(session->fd, response, strlen(response));
#endif
}

/* ─────────────────────────────────────────────────────────────────────────
 * handle_video_controls() — /video_controls endpoint
 * ───────────────────────────────────────────────────────────────────────── */
static void handle_video_controls(struct http_client_session *session, const char *request_buf)
{
#ifdef HAVE_LIBCAMERA
    capture_controls_t cc = parse_controls_from_request(request_buf);
    int ret = libcamera_apply_video_controls(session->server->video_src, &cc);
    if (ret == 0) {
        send_json_response(session->fd, 200, "OK",
                           "{\"status\":\"ok\",\"message\":\"Video controls updated\"}");
    } else {
        send_json_response(session->fd, 500, "Internal Server Error",
                           "{\"status\":\"error\",\"message\":\"Failed to apply video controls\"}");
    }
#else
    send_json_response(session->fd, 501, "Not Implemented",
                       "{\"status\":\"error\",\"message\":\"Requires libcamera support\"}");
#endif
}

/* ─────────────────────────────────────────────────────────────────────────
 * handle_reset_controls() — /reset_controls endpoint
 * ───────────────────────────────────────────────────────────────────────── */
static void handle_reset_controls(struct http_client_session *session)
{
#ifdef HAVE_LIBCAMERA
    int ret = libcamera_reset_controls(session->server->video_src);
    if (ret == 0) {
        send_json_response(session->fd, 200, "OK",
                           "{\"status\":\"ok\",\"message\":\"Controls reset to defaults\"}");
    } else {
        send_json_response(session->fd, 500, "Internal Server Error",
                           "{\"status\":\"error\",\"message\":\"Failed to reset controls\"}");
    }
#else
    send_json_response(session->fd, 501, "Not Implemented",
                       "{\"status\":\"error\",\"message\":\"Requires libcamera support\"}");
#endif
}

/* ─────────────────────────────────────────────────────────────────────────
 * handle_capture_controls() — /capture_controls endpoint
 *
 * GET /capture_controls          → mevcut profili JSON olarak döner
 * GET /capture_controls?param=.. → profili günceller ve kaydeder
 * ───────────────────────────────────────────────────────────────────────── */
static void handle_capture_controls(struct http_client_session *session, const char *request_buf)
{
    if (has_any_query_params(request_buf)) {
        /* Parametre var → profili güncelle */
        capture_controls_t cc = parse_controls_from_request(request_buf);

        pthread_mutex_lock(&session->server->profile_mtx);
        session->server->saved_capture_profile = cc;
        session->server->profile_loaded = true;
        pthread_mutex_unlock(&session->server->profile_mtx);

        /* Persist to disk */
        capture_profile_save(&cc, CAPTURE_PROFILE_PATH);

        send_json_response(session->fd, 200, "OK",
                           "{\"status\":\"ok\",\"message\":\"Capture profile saved\"}");
    } else {
        /* Parametre yok → mevcut profili döndür */
        pthread_mutex_lock(&session->server->profile_mtx);
        capture_controls_t cc = session->server->saved_capture_profile;
        pthread_mutex_unlock(&session->server->profile_mtx);

        send_profile_json(session->fd, &cc);
    }
}

/* ─────────────────────────────────────────────────────────────────────────
 * handle_capture_controls_reset() — /capture_controls/reset endpoint
 * ───────────────────────────────────────────────────────────────────────── */
static void handle_capture_controls_reset(struct http_client_session *session)
{
    capture_controls_t cc = capture_controls_default();

    pthread_mutex_lock(&session->server->profile_mtx);
    session->server->saved_capture_profile = cc;
    pthread_mutex_unlock(&session->server->profile_mtx);

    capture_profile_save(&cc, CAPTURE_PROFILE_PATH);

    send_json_response(session->fd, 200, "OK",
                       "{\"status\":\"ok\",\"message\":\"Capture profile reset to defaults\"}");
}

/* ─────────────────────────────────────────────────────────────────────────
 * handle_test_capture() — /test_capture endpoint
 *
 * Hızlı JPEG test çekimi: saved profile'daki ayarlarla bir JPEG alıp döner.
 * ───────────────────────────────────────────────────────────────────────── */
static void handle_test_capture(struct http_client_session *session)
{
#ifdef HAVE_LIBCAMERA
    pthread_mutex_lock(&session->server->profile_mtx);
    capture_controls_t cc = session->server->saved_capture_profile;
    pthread_mutex_unlock(&session->server->profile_mtx);

    int quality = cc.jpeg_quality > 0 ? cc.jpeg_quality : 90;
    void *jpeg_buf = NULL;
    size_t jpeg_size = 0;

    fprintf(stdout, "Test capture: JPEG quality=%d\n", quality);

    int ret = libcamera_capture_jpeg(session->server->video_src, &jpeg_buf, &jpeg_size, quality);
    if (ret == 0 && jpeg_buf && jpeg_size > 0) {
        char http_header[512];
        int len = snprintf(http_header, sizeof(http_header),
                  "HTTP/1.1 200 OK\r\n"
                  "Content-Type: image/jpeg\r\n"
                  "Content-Disposition: inline; filename=\"test_%ld.jpg\"\r\n"
                  "Access-Control-Allow-Origin: *\r\n"
                  "Cache-Control: no-cache\r\n"
                  "Content-Length: %zu\r\n\r\n",
                  (long)time(NULL), jpeg_size);

        write(session->fd, http_header, len);
        write(session->fd, jpeg_buf, jpeg_size);
        free(jpeg_buf);
    } else {
        send_json_response(session->fd, 500, "Internal Server Error",
                           "{\"status\":\"error\",\"message\":\"Test JPEG capture failed\"}");
        if (jpeg_buf) free(jpeg_buf);
    }
#else
    send_json_response(session->fd, 501, "Not Implemented",
                       "{\"status\":\"error\",\"message\":\"Requires libcamera support\"}");
#endif
}

/* ─────────────────────────────────────────────────────────────────────────
 * handle_camera_status() — /camera_status endpoint
 *
 * Kameradan anlık ölçülen exposure ve gain değerlerini döner.
 * UI'da Auto→Manual geçişinde gerçek değerleri göstermek için kullanılır.
 * ───────────────────────────────────────────────────────────────────────── */
static void handle_camera_status(struct http_client_session *session)
{
#ifdef HAVE_LIBCAMERA
    int64_t exp_us = 0;
    float gain = 0.0f;
    libcamera_get_camera_status(session->server->video_src, &exp_us, &gain);

    char body[256];
    snprintf(body, sizeof(body),
        "{\"exposure_us\":%lld,\"gain\":%.2f}",
        (long long)exp_us, gain);
    send_json_response(session->fd, 200, "OK", body);
#else
    send_json_response(session->fd, 200, "OK",
                       "{\"exposure_us\":20000,\"gain\":1.0}");
#endif
}

/* ─────────────────────────────────────────────────────────────────────────
 * client_thread_func() — Her HTTP client bağlantısı için thread
 * ───────────────────────────────────────────────────────────────────────── */
static void *client_thread_func(void *arg) {
    struct http_client_session *session = (struct http_client_session *)arg;
    char request_buf[2048] = {0};

    if (read(session->fd, request_buf, sizeof(request_buf) - 1) <= 0) {
        goto cleanup;
    }

    /* ─── Route matching ─── */
    if (strncmp(request_buf, "GET /profiles/save", 18) == 0) {
        handle_profiles_save(session, request_buf);
    }
    else if (strncmp(request_buf, "GET /profiles/delete", 20) == 0) {
        handle_profiles_delete(session, request_buf);
    }
    else if (strncmp(request_buf, "GET /profiles/load", 18) == 0) {
        handle_profiles_load_by_name(session, request_buf);
    }
    else if (strncmp(request_buf, "GET /profiles", 13) == 0 &&
             (request_buf[13] == ' ' || request_buf[13] == '?' || request_buf[13] == '\r')) {
        handle_profiles_list(session);
    }
    else if (strncmp(request_buf, "GET /camera_status", 18) == 0) {
        handle_camera_status(session);
    }
    else if (strncmp(request_buf, "GET /capture_controls/reset", 27) == 0) {
        handle_capture_controls_reset(session);
    }
    else if (strncmp(request_buf, "GET /capture_controls", 21) == 0) {
        handle_capture_controls(session, request_buf);
    }
    else if (strncmp(request_buf, "GET /test_capture", 17) == 0) {
        handle_test_capture(session);
    }
    else if (strncmp(request_buf, "GET /capture", 12) == 0) {
        handle_capture(session, request_buf);
    }
    else if (strncmp(request_buf, "GET /video_controls", 19) == 0) {
        handle_video_controls(session, request_buf);
    }
    else if (strncmp(request_buf, "GET /reset_controls", 19) == 0) {
        handle_reset_controls(session);
    }
    else {
        const char *response = "HTTP/1.1 404 Not Found\r\n\r\n";
        write(session->fd, response, strlen(response));
    }

cleanup:
    close(session->fd);
    if (session->captured_data.mem) free(session->captured_data.mem);
    pthread_mutex_destroy(&session->mtx);
    pthread_cond_destroy(&session->cond);
    free(session);
    return NULL;
}


static void *http_server_thread(void *arg) {
    struct http_server *server = (struct http_server *)arg;
    while (server->running) {
        int client_fd = accept(server->listen_fd, NULL, NULL);
        if (client_fd < 0) continue;
        struct http_client_session *session = (struct http_client_session*)malloc(sizeof(*session));
        if (!session) {
            close(client_fd);
            continue;
        }
        memset(session, 0, sizeof(*session));
        session->fd = client_fd;
        session->server = server;
        pthread_mutex_init(&session->mtx, NULL);
        pthread_cond_init(&session->cond, NULL);
        if (pthread_create(&session->thread, NULL, client_thread_func, session) != 0) {
            close(client_fd);
            free(session);
        }
        pthread_detach(session->thread);
    }
    return NULL;
}

struct http_server *http_capture_new(int port, struct video_source *video_src) {
    struct http_server *server = (struct http_server*)calloc(1, sizeof(*server));
    if (!server) return NULL;
    
    server->video_src = video_src;

    /* Initialize saved capture profile */
    server->saved_capture_profile = capture_controls_default();
    server->profile_count = 0;
    server->active_profile_name[0] = '\0';
    pthread_mutex_init(&server->profile_mtx, NULL);

    /* Try to load persisted profile */
    if (capture_profile_load(&server->saved_capture_profile, CAPTURE_PROFILE_PATH) == 0) {
        server->profile_loaded = true;
        fprintf(stdout, "Loaded saved capture profile\n");
    } else {
        server->profile_loaded = false;
        fprintf(stdout, "No saved capture profile, using defaults\n");
    }

    /* Load named profiles */
    if (profiles_load_all(server->profiles, &server->profile_count, MAX_PROFILES, CAPTURE_PROFILES_PATH) == 0) {
        fprintf(stdout, "Loaded %d named profiles\n", server->profile_count);
    } else {
        fprintf(stdout, "No named profiles found, starting empty\n");
    }

#ifdef HAVE_LIBCAMERA
    server->still_src = libcamera_get_still_source(video_src);
#else
    server->still_src = NULL;
    (void)video_src; // Suppress unused parameter warning
#endif
    
    server->running = true;
    server->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server->listen_fd < 0) {
        pthread_mutex_destroy(&server->profile_mtx);
        free(server);
        return NULL;
    }
    int opt = 1;
    setsockopt(server->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in serv_addr = {0};
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port = htons(port);
    if (bind(server->listen_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0 ||
        listen(server->listen_fd, 10) < 0) {
        close(server->listen_fd);
        pthread_mutex_destroy(&server->profile_mtx);
        free(server);
        return NULL;
    }
    if (pthread_create(&server->server_thread, NULL, http_server_thread, server) != 0) {
        close(server->listen_fd);
        pthread_mutex_destroy(&server->profile_mtx);
        free(server);
        return NULL;
    }
    return server;
}

void http_capture_destroy(struct http_server *server) {
    if (!server) return;
    server->running = false;
    shutdown(server->listen_fd, SHUT_RDWR);
    close(server->listen_fd);
    pthread_join(server->server_thread, NULL);
    pthread_mutex_destroy(&server->profile_mtx);
    free(server);
}