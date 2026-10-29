/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * A y4m stream whose bit depth is below the picture's reads into the picture
 * left-shifted to the picture's depth (Netflix/vmaf PR 1682): an 8-bit
 * reference scored against a 10-bit distorted clip at a common bit depth. The
 * direct reader, video_input_fetch_into_vmaf_picture(), copied the stream's
 * bytes as they were, so an 8-bit frame filled half of a 10-bit picture and
 * the next frame was read from the wrong offset. A stream deeper than the
 * picture, or an 8-bit picture, is refused.
 *
 * POSIX only: the clips live in memory behind fmemopen(), like
 * test_video_input_odd_dims.
 */

/* NOLINTNEXTLINE(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp) — POSIX feature-test macro for fmemopen (ADR-0141 / ADR-0278) */
#define _POSIX_C_SOURCE 200809L

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

#include "test.h"

#include "libvmaf/picture.h"
#include "vidinput.h"

#define FRAME_CNT 2u
#define CLIP_W 6u
#define CLIP_H 4u
#define HEADER_MAX 80u

typedef struct {
    unsigned depth;
    const char *chroma;
} Stream;

typedef struct {
    uint8_t *bytes;
    size_t len;
} Buffer;

/* A sample of the stream, below 2^depth. */
static unsigned sample_of(unsigned depth, unsigned plane, unsigned row, unsigned col,
                          unsigned frame)
{
    const unsigned v = (17u * plane) + (31u * row) + (7u * col) + (101u * frame) + 1u;
    return v & ((1u << depth) - 1u);
}

static unsigned plane_w(unsigned plane)
{
    return plane ? (CLIP_W + 1u) / 2u : CLIP_W;
}

static unsigned plane_h(unsigned plane)
{
    return plane ? (CLIP_H + 1u) / 2u : CLIP_H;
}

static size_t frame_bytes(unsigned depth)
{
    const size_t bytes_per_sample = depth > 8u ? 2u : 1u;
    size_t total = 0;
    for (unsigned p = 0; p < 3u; p++)
        total += (size_t)plane_w(p) * plane_h(p) * bytes_per_sample;
    return total;
}

static size_t write_frame(unsigned depth, unsigned frame, uint8_t *dst)
{
    size_t at = 0;
    for (unsigned p = 0; p < 3u; p++) {
        for (unsigned row = 0; row < plane_h(p); row++) {
            for (unsigned col = 0; col < plane_w(p); col++) {
                const unsigned v = sample_of(depth, p, row, col, frame);
                dst[at++] = (uint8_t)(v & 0xffu);
                if (depth > 8u)
                    dst[at++] = (uint8_t)(v >> 8);
            }
        }
    }
    return at;
}

static int build_clip(const Stream *stream, Buffer *out)
{
    static const char FRAME_TAG[] = "FRAME\n";
    const size_t tag_len = sizeof(FRAME_TAG) - 1u;
    char header[HEADER_MAX] = {0};
    const int header_len = snprintf(header, sizeof(header), "YUV4MPEG2 W%u H%u F25:1 Ip A1:1 C%s\n",
                                    CLIP_W, CLIP_H, stream->chroma);
    if (header_len <= 0 || (size_t)header_len >= sizeof(header))
        return -1;
    out->len = (size_t)header_len + (FRAME_CNT * (tag_len + frame_bytes(stream->depth)));
    out->bytes = malloc(out->len);
    if (!out->bytes)
        return -1;
    size_t at = (size_t)header_len;
    (void)memcpy(out->bytes, header, at);
    for (unsigned n = 0; n < FRAME_CNT; n++) {
        (void)memcpy(out->bytes + at, FRAME_TAG, tag_len);
        at += tag_len;
        at += write_frame(stream->depth, n, out->bytes + at);
    }
    return at == out->len ? 0 : -1;
}

static unsigned picture_sample(const VmafPicture *pic, unsigned plane, unsigned row, unsigned col)
{
    const uint8_t *line = (const uint8_t *)pic->data[plane] + ((size_t)row * pic->stride[plane]);
    if (pic->bpc <= 8u)
        return line[col];
    uint16_t v = 0;
    (void)memcpy(&v, line + ((size_t)col * 2u), sizeof(v));
    return v;
}

/* True when every sample of plane `p` is the stream's sample of `frame`
 * shifted left to the picture's depth. */
static bool plane_matches(const VmafPicture *pic, unsigned depth, unsigned frame, unsigned p)
{
    const unsigned shift = pic->bpc - depth;
    for (unsigned row = 0; row < pic->h[p]; row++) {
        for (unsigned col = 0; col < pic->w[p]; col++) {
            const unsigned want = sample_of(depth, p, row, col, frame) << shift;
            const unsigned got = picture_sample(pic, p, row, col);
            if (got != want) {
                (void)fprintf(stderr, "\n  frame %u plane %u row %u col %u: read %u, want %u\n",
                              frame, p, row, col, got, want);
                return false;
            }
        }
    }
    return true;
}

static bool picture_matches(const VmafPicture *pic, unsigned depth, unsigned frame)
{
    for (unsigned p = 0; p < 3u; p++) {
        if (!plane_matches(pic, depth, frame, p))
            return false;
    }
    return true;
}

typedef struct {
    int open_rc;
    int fetch_rc[FRAME_CNT + 1u];
    bool matches[FRAME_CNT];
} ReadRun;

static int fetch_one(video_input *vid, unsigned bpc, unsigned depth, unsigned frame, bool *matches)
{
    VmafPicture pic;
    if (vmaf_picture_alloc(&pic, VMAF_PIX_FMT_YUV420P, bpc, CLIP_W, CLIP_H))
        return -100;
    const int rc = video_input_fetch_into_vmaf_picture(vid, &pic);
    if (matches)
        *matches = rc == 1 && picture_matches(&pic, depth, frame);
    (void)vmaf_picture_unref(&pic);
    return rc;
}

/* Every frame of the clip, then one fetch past its end, into `bpc` pictures. */
static ReadRun read_clip(const Stream *stream, unsigned bpc)
{
    ReadRun r;
    memset(&r, 0, sizeof(r));
    Buffer buf = {0};
    r.open_rc = build_clip(stream, &buf);
    FILE *fin = r.open_rc ? NULL : fmemopen(buf.bytes, buf.len, "rb");
    video_input vid;
    r.open_rc = fin ? video_input_open(&vid, fin) : -1;
    if (r.open_rc) {
        if (fin)
            (void)fclose(fin);
        free(buf.bytes);
        return r;
    }
    for (unsigned n = 0; n < FRAME_CNT; n++)
        r.fetch_rc[n] = fetch_one(&vid, bpc, stream->depth, n, &r.matches[n]);
    r.fetch_rc[FRAME_CNT] = fetch_one(&vid, bpc, stream->depth, FRAME_CNT, NULL);
    video_input_close(&vid);
    free(buf.bytes);
    return r;
}

static char *expect_upshift(const Stream *stream, unsigned bpc)
{
    const ReadRun r = read_clip(stream, bpc);
    mu_assert("the reader refused the clip", r.open_rc == 0);
    for (unsigned n = 0; n < FRAME_CNT; n++) {
        mu_assert("a frame did not read", r.fetch_rc[n] == 1);
        mu_assert("a frame did not read back shifted to the picture's depth", r.matches[n]);
    }
    mu_assert("the clip did not end after its last frame", r.fetch_rc[FRAME_CNT] == 0);
    return NULL;
}

static char *test_8bit_stream_into_10bit_picture(void)
{
    const Stream stream = {8u, "420jpeg"};
    return expect_upshift(&stream, 10u);
}

static char *test_10bit_stream_into_12bit_picture(void)
{
    const Stream stream = {10u, "420p10"};
    return expect_upshift(&stream, 12u);
}

static char *test_same_depth_unchanged(void)
{
    const Stream stream = {10u, "420p10"};
    return expect_upshift(&stream, 10u);
}

static char *test_deeper_stream_refused(void)
{
    const Stream stream = {12u, "420p12"};
    const ReadRun r = read_clip(&stream, 10u);
    mu_assert("the reader refused the clip", r.open_rc == 0);
    mu_assert("a 12-bit stream into a 10-bit picture must be refused", r.fetch_rc[0] == -1);
    return NULL;
}

static char *test_8bit_picture_refused(void)
{
    const Stream stream = {10u, "420p10"};
    const ReadRun r = read_clip(&stream, 8u);
    mu_assert("the reader refused the clip", r.open_rc == 0);
    mu_assert("a 10-bit stream into an 8-bit picture must be refused", r.fetch_rc[0] == -1);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_8bit_stream_into_10bit_picture);
    mu_run_test(test_10bit_stream_into_12bit_picture);
    mu_run_test(test_same_depth_unchanged);
    mu_run_test(test_deeper_stream_refused);
    mu_run_test(test_8bit_picture_refused);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
