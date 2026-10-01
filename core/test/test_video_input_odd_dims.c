/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Frames with an odd width or height read back sample for sample
 * (T-VIDINPUT-ODD-DIMENSION-READBACK-UNTESTED-2026-10-01).
 *
 * A subsampled plane occupies ceil(width / dec) * ceil(height / dec) samples
 * in a raw or y4m file. Upstream Netflix/vmaf's direct readers drive their
 * reads from the picture's planes, which there carry the floor, so part of
 * every odd-sized frame stays in the stream and the next frame is read from
 * the wrong offset (Netflix/vmaf PR #1604). The fork's VmafPicture carries the
 * ceiling (Research-0094), so the same readers consume a whole frame, and the
 * buffered reader the `vmaf` tool uses reads the frame in one piece.
 *
 * Nothing checked that. Every sample here is a function of its plane, row,
 * column and frame, so a plane read at the wrong offset or with the wrong row
 * pitch cannot match by accident, and a reader that leaves bytes behind fails
 * on the second frame or finds a frame past the end of the clip.
 *
 * POSIX only: the clips live in memory behind fmemopen(), like
 * test_y4m_411_oob.
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
 * documented /std:clatest C23 feature set does not include `nullptr`, and this
 * file mirrors the C spelling of the surface it exercises. ADR-1138. */

#include "test.h"

#include "libvmaf/picture.h"
#include "vidinput.h"

#define FRAME_CNT 3u
#define HEADER_MAX 64u

typedef struct {
    unsigned w;
    unsigned h;
    enum VmafPixelFormat pix_fmt;
    /* NULL for a raw clip, otherwise the y4m chroma tag. */
    const char *y4m_chroma;
} Clip;

typedef struct {
    uint8_t *bytes;
    size_t len;
} Buffer;

static uint8_t sample_of(unsigned plane, unsigned row, unsigned col, unsigned frame)
{
    return (uint8_t)((17u * plane) + (31u * row) + (7u * col) + (101u * frame) + 1u);
}

/* The plane size a file stores: the ceiling of the subsampled dimension. */
static void file_plane_dims(const Clip *clip, unsigned plane, unsigned *pw, unsigned *ph)
{
    const bool sub_w = plane && clip->pix_fmt != VMAF_PIX_FMT_YUV444P;
    const bool sub_h = plane && clip->pix_fmt == VMAF_PIX_FMT_YUV420P;
    *pw = sub_w ? (clip->w + 1u) / 2u : clip->w;
    *ph = sub_h ? (clip->h + 1u) / 2u : clip->h;
}

static size_t frame_bytes(const Clip *clip)
{
    size_t total = 0;
    for (unsigned p = 0; p < 3u; p++) {
        unsigned pw = 0;
        unsigned ph = 0;
        file_plane_dims(clip, p, &pw, &ph);
        total += (size_t)pw * ph;
    }
    return total;
}

static size_t write_frame(const Clip *clip, unsigned frame, uint8_t *dst)
{
    size_t at = 0;
    for (unsigned p = 0; p < 3u; p++) {
        unsigned pw = 0;
        unsigned ph = 0;
        file_plane_dims(clip, p, &pw, &ph);
        for (unsigned row = 0; row < ph; row++) {
            for (unsigned col = 0; col < pw; col++) {
                dst[at++] = sample_of(p, row, col, frame);
            }
        }
    }
    return at;
}

/* FRAME_CNT frames in the layout a raw file or a y4m stream uses. */
static int build_clip(const Clip *clip, Buffer *out)
{
    static const char FRAME_TAG[] = "FRAME\n";
    const size_t tag_len = clip->y4m_chroma ? sizeof(FRAME_TAG) - 1u : 0u;
    char header[HEADER_MAX] = {0};
    int header_len = 0;
    if (clip->y4m_chroma) {
        header_len = snprintf(header, sizeof(header), "YUV4MPEG2 W%u H%u F25:1 Ip A1:1 C%s\n",
                              clip->w, clip->h, clip->y4m_chroma);
        if (header_len <= 0 || (size_t)header_len >= sizeof(header)) {
            return -1;
        }
    }

    out->len = (size_t)header_len + (FRAME_CNT * (tag_len + frame_bytes(clip)));
    out->bytes = malloc(out->len);
    if (!out->bytes) {
        return -1;
    }
    size_t at = (size_t)header_len;
    (void)memcpy(out->bytes, header, at);
    for (unsigned n = 0; n < FRAME_CNT; n++) {
        (void)memcpy(out->bytes + at, FRAME_TAG, tag_len);
        at += tag_len;
        at += write_frame(clip, n, out->bytes + at);
    }
    return at == out->len ? 0 : -1;
}

static int open_clip(const Clip *clip, const Buffer *buf, video_input *vid)
{
    FILE *fin = fmemopen(buf->bytes, buf->len, "rb");
    if (!fin) {
        return -1;
    }
    const int err = clip->y4m_chroma ?
                        video_input_open(vid, fin) :
                        raw_input_open(vid, fin, clip->w, clip->h, (int)clip->pix_fmt, 8u);
    if (err) {
        (void)fclose(fin);
    }
    return err;
}

/* True when `rows` rows of `cols` samples at `data` hold plane `plane` of
 * frame `frame`. */
static bool plane_matches(const uint8_t *data, ptrdiff_t stride, unsigned cols, unsigned rows,
                          unsigned plane, unsigned frame)
{
    for (unsigned row = 0; row < rows; row++) {
        for (unsigned col = 0; col < cols; col++) {
            if (data[(row * stride) + col] != sample_of(plane, row, col, frame)) {
                (void)fprintf(stderr, "\n  frame %u plane %u row %u col %u: read %u, file has %u\n",
                              frame, plane, row, col, (unsigned)data[(row * stride) + col],
                              (unsigned)sample_of(plane, row, col, frame));
                return false;
            }
        }
    }
    return true;
}

/* The picture must carry every sample the file stores for a plane, which is
 * what keeps the readers below in step with the stream. */
static bool picture_carries_file_planes(const Clip *clip, const VmafPicture *pic)
{
    for (unsigned p = 0; p < 3u; p++) {
        unsigned pw = 0;
        unsigned ph = 0;
        file_plane_dims(clip, p, &pw, &ph);
        if (pic->w[p] != pw || pic->h[p] != ph) {
            return false;
        }
    }
    return true;
}

/* The direct reader: video_input_fetch_into_vmaf_picture(). */
static char *direct_read_frame(const Clip *clip, video_input *vid, unsigned frame, int expect)
{
    VmafPicture pic;
    mu_assert("vmaf_picture_alloc failed",
              !vmaf_picture_alloc(&pic, clip->pix_fmt, 8u, clip->w, clip->h));
    const bool geometry = picture_carries_file_planes(clip, &pic);
    const int ret = geometry ? video_input_fetch_into_vmaf_picture(vid, &pic) : -1;
    bool ok = ret == expect;
    for (unsigned p = 0; ok && expect == 1 && p < 3u; p++) {
        ok = plane_matches((const uint8_t *)pic.data[p], pic.stride[p], pic.w[p], pic.h[p], p,
                           frame);
    }
    (void)vmaf_picture_unref(&pic);
    mu_assert("the picture does not carry the planes the file stores", geometry);
    mu_assert("the direct reader lost step with the stream", ok);
    return NULL;
}

/* The buffered reader: video_input_fetch_frame(), read the way the `vmaf`
 * tool's copy_picture_data() reads it. */
static char *buffered_read_frame(const Clip *clip, video_input *vid, unsigned frame, int expect)
{
    video_input_info info;
    video_input_get_info(vid, &info);
    video_input_ycbcr ycbcr;
    const int ret = video_input_fetch_frame(vid, ycbcr, NULL);
    mu_assert("the buffered reader lost step with the stream", ret == expect);
    for (unsigned p = 0; expect == 1 && p < 3u; p++) {
        const int xdec = p && !(info.pixel_fmt & 1);
        const int ydec = p && !(info.pixel_fmt & 2);
        const uint8_t *data =
            ycbcr[p].data + ((size_t)(info.pic_y >> ydec) * ycbcr[p].stride) + (info.pic_x >> xdec);
        unsigned pw = 0;
        unsigned ph = 0;
        file_plane_dims(clip, p, &pw, &ph);
        mu_assert("the buffered reader returned the wrong samples",
                  plane_matches(data, (ptrdiff_t)ycbcr[p].stride, pw, ph, p, frame));
    }
    return NULL;
}

typedef char *(*ReadFrame)(const Clip *clip, video_input *vid, unsigned frame, int expect);

/* Every frame reads back, and the clip then ends: a reader that leaves bytes
 * in the stream fails one of the two. */
static char *read_whole_clip(const Clip *clip, ReadFrame read_frame)
{
    Buffer buf = {0};
    if (build_clip(clip, &buf)) {
        free(buf.bytes);
        return "could not build the clip";
    }
    video_input vid;
    if (open_clip(clip, &buf, &vid)) {
        free(buf.bytes);
        return "the reader refused the clip";
    }
    char *msg = NULL;
    for (unsigned n = 0; !msg && n < FRAME_CNT; n++) {
        msg = read_frame(clip, &vid, n, 1);
    }
    if (!msg) {
        msg = read_frame(clip, &vid, FRAME_CNT, 0);
    }
    video_input_close(&vid);
    free(buf.bytes);
    return msg;
}

static char *check_clip(const Clip *clip)
{
    mu_assert_msg(read_whole_clip(clip, direct_read_frame));
    mu_assert_msg(read_whole_clip(clip, buffered_read_frame));
    return NULL;
}

/* positive: even dimensions, where the floor and the ceiling agree. */
static char *test_even_dimensions_read_back(void)
{
    const Clip raw = {20u, 20u, VMAF_PIX_FMT_YUV420P, NULL};
    const Clip y4m = {20u, 20u, VMAF_PIX_FMT_YUV420P, "420jpeg"};
    mu_assert_msg(check_clip(&raw));
    return check_clip(&y4m);
}

/* The case of Netflix/vmaf PR #1604: 19x19 4:2:0 stores 10x10 chroma. */
static char *test_odd_420_reads_back(void)
{
    const Clip raw = {19u, 19u, VMAF_PIX_FMT_YUV420P, NULL};
    const Clip y4m = {19u, 19u, VMAF_PIX_FMT_YUV420P, "420jpeg"};
    mu_assert_msg(check_clip(&raw));
    return check_clip(&y4m);
}

/* boundary: only one dimension odd, and 4:2:2, which subsamples the width
 * alone. The 4:2:2 clip is raw: the y4m reader resamples C422 chroma to the
 * jpeg siting, so its output is not the file's samples. */
static char *test_single_odd_dimension_reads_back(void)
{
    const Clip odd_width = {19u, 20u, VMAF_PIX_FMT_YUV420P, NULL};
    const Clip odd_height = {20u, 19u, VMAF_PIX_FMT_YUV420P, "420jpeg"};
    const Clip raw_422 = {19u, 19u, VMAF_PIX_FMT_YUV422P, NULL};
    mu_assert_msg(check_clip(&odd_width));
    mu_assert_msg(check_clip(&odd_height));
    return check_clip(&raw_422);
}

/* negative: a clip cut short inside its last frame is an error (-1), not an
 * end of stream (0) and not a frame (1). */
static char *truncated_clip_fails(const Clip *clip, bool direct)
{
    Buffer buf = {0};
    if (build_clip(clip, &buf)) {
        free(buf.bytes);
        return "could not build the clip";
    }
    buf.len -= frame_bytes(clip) / 2u;
    video_input vid;
    if (open_clip(clip, &buf, &vid)) {
        free(buf.bytes);
        return "the reader refused the clip";
    }
    const ReadFrame read_frame = direct ? direct_read_frame : buffered_read_frame;
    char *msg = NULL;
    for (unsigned n = 0; !msg && n + 1u < FRAME_CNT; n++) {
        msg = read_frame(clip, &vid, n, 1);
    }
    if (!msg) {
        msg = read_frame(clip, &vid, FRAME_CNT - 1u, -1);
    }
    video_input_close(&vid);
    free(buf.bytes);
    return msg;
}

static char *test_truncated_frame_is_an_error(void)
{
    const Clip raw = {19u, 19u, VMAF_PIX_FMT_YUV420P, NULL};
    const Clip y4m = {19u, 19u, VMAF_PIX_FMT_YUV420P, "420jpeg"};
    mu_assert_msg(truncated_clip_fails(&raw, true));
    mu_assert_msg(truncated_clip_fails(&raw, false));
    mu_assert_msg(truncated_clip_fails(&y4m, true));
    return truncated_clip_fails(&y4m, false);
}

char *run_tests(void)
{
    mu_run_test(test_even_dimensions_read_back);
    mu_run_test(test_odd_420_reads_back);
    mu_run_test(test_single_odd_dimension_reads_back);
    mu_run_test(test_truncated_frame_is_an_error);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
