/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Helpers shared by the VMAFx API tests (ADR-1852, RC4 WP2): error checks,
 * a log-callback capture, and host frames over tightly packed YUV buffers.
 */

#ifndef VMAFX_TEST_UTIL_H
#define VMAFX_TEST_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vmafx/vmafx.h"

/* NOLINTBEGIN(modernize-use-nullptr): C header. The fork builds C as C23,
 * where clang-tidy also proposes the `nullptr` keyword, but MSVC's documented
 * /std:clatest C23 feature set does not include `nullptr` and the required
 * Windows builds compile the tests with cl.exe (C2065). ADR-1138. */

/* True when `*error` holds `status` and names `subject` of `kind`; the error
 * is released and `*error` reset either way. */
static inline bool vt_failed(VmafxError **error, VmafxStatus status, const char *subject,
                             uint32_t kind)
{
    const bool ok = *error && vmafx_error_status(*error) == status &&
                    strcmp(vmafx_error_subject(*error), subject) == 0 &&
                    vmafx_error_subject_kind(*error) == kind &&
                    vmafx_error_function(*error)[0] != '\0' &&
                    vmafx_error_message(*error)[0] != '\0';
    vmafx_error_free(*error);
    *error = NULL;
    return ok;
}

/* ---- Log capture ------------------------------------------------------------ */

typedef struct VtLog {
    unsigned count;
    unsigned errors;
    uint32_t last_level;
    char last[512];
} VtLog;

static inline void vt_log_callback(uint32_t level, const char *message, void *user)
{
    VtLog *const log = user;
    log->count++;
    if (level == VMAFX_LOG_LEVEL_ERROR) {
        log->errors++;
    }
    log->last_level = level;
    size_t len = 0;
    while (len < sizeof(log->last) - 1u && message[len] != '\0') {
        len++;
    }
    memcpy(log->last, message, len);
    log->last[len] = '\0';
}

/* A context whose messages go to `log` at `level`. */
static inline VmafxContext *vt_logged_context(VtLog *log, uint32_t level)
{
    VmafxContextConfig config = VMAFX_CONTEXT_CONFIG_INIT;
    config.log_level = level;
    config.log_callback = vt_log_callback;
    config.log_user = log;
    VmafxContext *context = NULL;
    return vmafx_context_create(&config, &context, NULL) == VMAFX_OK ? context : NULL;
}

/* A context scoring PSNR with the default configuration; NULL on failure. */
static inline VmafxContext *vt_psnr_context(void)
{
    VmafxContext *context = NULL;
    if (vmafx_context_create(NULL, &context, NULL) != VMAFX_OK) {
        return NULL;
    }
    if (vmafx_context_use_feature(context, "psnr", NULL, NULL) != VMAFX_OK) {
        (void)vmafx_context_destroy(context, NULL);
        return NULL;
    }
    return context;
}

/* ---- Frames ------------------------------------------------------------------- */

static inline VmafxFrameDesc vt_desc(uint32_t pix_fmt, uint32_t bpc, uint32_t w, uint32_t h)
{
    VmafxFrameDesc desc = VMAFX_FRAME_DESC_INIT;
    desc.pix_fmt = pix_fmt;
    desc.bpc = bpc;
    desc.w = w;
    desc.h = h;
    return desc;
}

/* Width, height and bytes per row of each plane of a tightly packed frame. */
static inline void vt_plane_geometry(const VmafxFrameDesc *d, unsigned w[3], unsigned h[3],
                                     size_t row[3])
{
    const bool ss_hor = d->pix_fmt != VMAFX_PIXEL_FORMAT_YUV444P;
    const bool ss_ver = d->pix_fmt == VMAFX_PIXEL_FORMAT_YUV420P;
    const bool chroma = d->pix_fmt != VMAFX_PIXEL_FORMAT_YUV400P;
    w[0] = d->w;
    h[0] = d->h;
    w[1] = w[2] = chroma ? (ss_hor ? (d->w + 1u) / 2u : d->w) : 0u;
    h[1] = h[2] = chroma ? (ss_ver ? (d->h + 1u) / 2u : d->h) : 0u;
    const size_t bytes = d->bpc > 8u ? 2u : 1u;
    for (unsigned p = 0; p < 3u; p++) {
        row[p] = (size_t)w[p] * bytes;
    }
}

/* Bytes of one tightly packed frame. */
static inline size_t vt_frame_bytes(const VmafxFrameDesc *d)
{
    unsigned w[3];
    unsigned h[3];
    size_t row[3];
    vt_plane_geometry(d, w, h, row);
    return row[0] * h[0] + row[1] * h[1] + row[2] * h[2];
}

/* A host frame holding a copy of the tightly packed `data`. */
static inline VmafxFrame *vt_copy_frame(const VmafxFrameDesc *d, const uint8_t *data)
{
    VmafxFrame *frame = NULL;
    if (vmafx_frame_create_host(NULL, d, &frame, NULL) != VMAFX_OK) {
        return NULL;
    }
    VmafxFramePlanes planes = VMAFX_FRAME_PLANES_INIT;
    if (vmafx_frame_planes(frame, &planes, NULL) != VMAFX_OK) {
        vmafx_frame_unref(frame);
        return NULL;
    }
    unsigned w[3];
    unsigned h[3];
    size_t row[3];
    vt_plane_geometry(d, w, h, row);
    for (unsigned p = 0; p < planes.n_planes; p++) {
        uint8_t *dst = planes.data[p];
        for (unsigned y = 0; y < h[p]; y++, data += row[p], dst += planes.stride[p]) {
            memcpy(dst, data, row[p]);
        }
    }
    return frame;
}

/* Counts release callbacks. A release runs on the thread that drops the last
 * reference: tests that count use contexts without worker threads. */
static inline void vt_count_release(void *user)
{
    (*(unsigned *)user)++;
}

/* A host frame borrowing the tightly packed `data`; `released` counts the
 * release callback. */
static inline VmafxFrame *vt_wrap_frame(const VmafxFrameDesc *d, uint8_t *data, unsigned *released)
{
    unsigned w[3];
    unsigned h[3];
    size_t row[3];
    vt_plane_geometry(d, w, h, row);
    VmafxHostPlanes planes = VMAFX_HOST_PLANES_INIT;
    const unsigned n_planes = d->pix_fmt == VMAFX_PIXEL_FORMAT_YUV400P ? 1u : 3u;
    for (unsigned p = 0; p < n_planes; p++) {
        planes.data[p] = data;
        planes.stride[p] = row[p];
        data += row[p] * h[p];
    }
    planes.release = released ? vt_count_release : NULL;
    planes.user = released;
    VmafxFrame *frame = NULL;
    return vmafx_frame_wrap_host(NULL, d, &planes, &frame, NULL) == VMAFX_OK ? frame : NULL;
}

/* Deterministic content: a gradient that differs per frame and per `seed`;
 * samples above 8 bits are masked to the depth. */
static inline void vt_fill(const VmafxFrameDesc *d, uint8_t *data, unsigned seed)
{
    const size_t n = vt_frame_bytes(d);
    const size_t offset = (size_t)seed * 29u;
    for (size_t i = 0; i < n; i++) {
        data[i] = (uint8_t)((i * 7u + (i / 64u) * 13u + offset) & 0xffu);
    }
    if (d->bpc > 8u) {
        const uint16_t max = (uint16_t)((1u << d->bpc) - 1u);
        for (size_t i = 0; i + 1u < n; i += 2u) {
            uint16_t sample = 0;
            memcpy(&sample, data + i, sizeof(sample));
            sample = (uint16_t)(sample & max);
            memcpy(data + i, &sample, sizeof(sample));
        }
    }
}

/* Bit-for-bit equality of two doubles (no memcmp of a floating object). */
static inline bool vt_same_bits(double a, double b)
{
    uint64_t x = 0;
    uint64_t y = 0;
    memcpy(&x, &a, sizeof(x));
    memcpy(&y, &b, sizeof(y));
    return x == y;
}

/* NOLINTEND(modernize-use-nullptr) */

#endif /* VMAFX_TEST_UTIL_H */
