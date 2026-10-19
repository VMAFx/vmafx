/**
 *
 *  Copyright 2016-2026 Netflix, Inc.
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *     Licensed under the BSD+Patent License (the "License");
 *     you may not use this file except in compliance with the License.
 *     You may obtain a copy of the License at
 *
 *         https://opensource.org/licenses/BSDplusPatent
 *
 *     Unless required by applicable law or agreed to in writing, software
 *     distributed under the License is distributed on an "AS IS" BASIS,
 *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *     See the License for the specific language governing permissions and
 *     limitations under the License.
 *
 */

/*
 * Picture colourspace / format / size conversion through zimg.
 *
 * Port of Netflix/vmaf 0497a0f29 with the fork's API shape (ADR-1822): the
 * source colour is an argument of vmaf_picture_convert_context_init_with_color()
 * because VmafPicture carries no colour field here. The conversion body is
 * upstream's, split into small helpers and without the `goto` of upstream's
 * init (HISS-01, HISS-04).
 *
 * Without HAVE_ZIMG (-Denable_zimg=false, the default) every entry point
 * fails with -ENOTSUP and logs why.
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <libvmaf/picture.h>

#include "log.h"
#include "mem.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `NULL` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

#ifdef HAVE_ZIMG

#include <zimg.h>

#define CONVERT_TMP_ALIGN 64u
#define CONVERT_BPC_MIN 8u
#define CONVERT_BPC_MAX 16u
#define CONVERT_ERR_LEN 256u

static int pix_fmt_to_zimg(enum VmafPixelFormat pix_fmt, zimg_color_family_e *family,
                           unsigned *subsample_w, unsigned *subsample_h)
{
    static const struct {
        enum VmafPixelFormat pix_fmt;
        zimg_color_family_e family;
        unsigned subsample_w;
        unsigned subsample_h;
    } table[] = {
        {VMAF_PIX_FMT_YUV400P, ZIMG_COLOR_GREY, 0, 0},
        {VMAF_PIX_FMT_YUV420P, ZIMG_COLOR_YUV, 1, 1},
        {VMAF_PIX_FMT_YUV422P, ZIMG_COLOR_YUV, 1, 0},
        {VMAF_PIX_FMT_YUV444P, ZIMG_COLOR_YUV, 0, 0},
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (table[i].pix_fmt != pix_fmt)
            continue;
        *family = table[i].family;
        *subsample_w = table[i].subsample_w;
        *subsample_h = table[i].subsample_h;
        return 0;
    }
    vmaf_log(VMAF_LOG_LEVEL_ERROR,
             "vmaf_picture_convert_context_init: unsupported pixel format %d "
             "(supported: YUV400P, YUV420P, YUV422P, YUV444P)\n",
             (int)pix_fmt);
    return -EINVAL;
}

static int matrix_to_zimg(enum VmafColorMatrixCoefficients matrix, zimg_matrix_coefficients_e *out)
{
    switch (matrix) {
    case VMAF_COLOR_MATRIX_BT709:
        *out = ZIMG_MATRIX_709;
        return 0;
    case VMAF_COLOR_MATRIX_BT2020_NCL:
        *out = ZIMG_MATRIX_2020_NCL;
        return 0;
    case VMAF_COLOR_MATRIX_ICTCP:
        *out = ZIMG_MATRIX_ICTCP;
        return 0;
    default:
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "vmaf_picture_convert_context_init: unsupported color matrix %d "
                 "(supported: BT709, BT2020_NCL, ICTCP)\n",
                 (int)matrix);
        return -EINVAL;
    }
}

static int trc_to_zimg(enum VmafColorTransferCharacteristic trc,
                       zimg_transfer_characteristics_e *out)
{
    switch (trc) {
    case VMAF_COLOR_TRC_BT709:
        *out = ZIMG_TRANSFER_709;
        return 0;
    case VMAF_COLOR_TRC_SMPTE2084:
        *out = ZIMG_TRANSFER_ST2084;
        return 0;
    default:
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "vmaf_picture_convert_context_init: unsupported transfer characteristic "
                 "%d (supported: BT709, SMPTE2084)\n",
                 (int)trc);
        return -EINVAL;
    }
}

static int primaries_to_zimg(enum VmafColorPrimaries primaries, zimg_color_primaries_e *out)
{
    switch (primaries) {
    case VMAF_COLOR_PRIMARIES_BT709:
        *out = ZIMG_PRIMARIES_709;
        return 0;
    case VMAF_COLOR_PRIMARIES_BT2020:
        *out = ZIMG_PRIMARIES_2020;
        return 0;
    case VMAF_COLOR_PRIMARIES_SMPTE432:
        *out = ZIMG_PRIMARIES_ST432_1;
        return 0;
    default:
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "vmaf_picture_convert_context_init: unsupported color primaries %d "
                 "(supported: BT709, BT2020, SMPTE432)\n",
                 (int)primaries);
        return -EINVAL;
    }
}

static int range_to_zimg(enum VmafColorRange range, zimg_pixel_range_e *out)
{
    switch (range) {
    case VMAF_COLOR_RANGE_LIMITED:
        *out = ZIMG_RANGE_LIMITED;
        return 0;
    case VMAF_COLOR_RANGE_FULL:
        *out = ZIMG_RANGE_FULL;
        return 0;
    default:
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "vmaf_picture_convert_context_init: unsupported color range %d "
                 "(supported: LIMITED, FULL)\n",
                 (int)range);
        return -EINVAL;
    }
}

static int color_to_zimg(zimg_image_format *fmt, const VmafColor *color)
{
    int err = matrix_to_zimg(color->matrix, &fmt->matrix_coefficients);
    if (err)
        return err;
    err = trc_to_zimg(color->trc, &fmt->transfer_characteristics);
    if (err)
        return err;
    err = primaries_to_zimg(color->primaries, &fmt->color_primaries);
    if (err)
        return err;
    return range_to_zimg(color->range, &fmt->pixel_range);
}

static int format_to_zimg(zimg_image_format *fmt, enum VmafPixelFormat pix_fmt, unsigned bpc,
                          unsigned w, unsigned h, const VmafColor *color)
{
    zimg_color_family_e family;
    unsigned subsample_w;
    unsigned subsample_h;
    int err = pix_fmt_to_zimg(pix_fmt, &family, &subsample_w, &subsample_h);
    if (err)
        return err;

    zimg_image_format_default(fmt, ZIMG_API_VERSION);
    fmt->width = w;
    fmt->height = h;
    fmt->pixel_type = bpc > 8 ? ZIMG_PIXEL_WORD : ZIMG_PIXEL_BYTE;
    fmt->depth = bpc;
    fmt->color_family = family;
    fmt->subsample_w = subsample_w;
    fmt->subsample_h = subsample_h;
    return color_to_zimg(fmt, color);
}

static zimg_resample_filter_e resample_filter_to_zimg(enum VmafResampleFilter f)
{
    switch (f) {
    case VMAF_RESAMPLE_BILINEAR:
        return ZIMG_RESIZE_BILINEAR;
    case VMAF_RESAMPLE_LANCZOS:
        return ZIMG_RESIZE_LANCZOS;
    case VMAF_RESAMPLE_BICUBIC:
    case VMAF_RESAMPLE_DEFAULT:
    default:
        return ZIMG_RESIZE_BICUBIC;
    }
}

static unsigned n_planes(const VmafPicture *pic)
{
    return pic->pix_fmt == VMAF_PIX_FMT_YUV400P ? 1u : 3u;
}

static void image_buffer_const_from_picture(zimg_image_buffer_const *buf, const VmafPicture *pic)
{
    memset(buf, 0, sizeof(*buf));
    buf->version = ZIMG_API_VERSION;
    for (unsigned i = 0; i < n_planes(pic); i++) {
        buf->plane[i].data = pic->data[i];
        buf->plane[i].stride = pic->stride[i];
        buf->plane[i].mask = ZIMG_BUFFER_MAX;
    }
}

static void image_buffer_from_picture(zimg_image_buffer *buf, const VmafPicture *pic)
{
    memset(buf, 0, sizeof(*buf));
    buf->version = ZIMG_API_VERSION;
    for (unsigned i = 0; i < n_planes(pic); i++) {
        buf->plane[i].data = pic->data[i];
        buf->plane[i].stride = pic->stride[i];
        buf->plane[i].mask = ZIMG_BUFFER_MAX;
    }
}

static bool color_is_specified(const VmafColor *color)
{
    return color->range != VMAF_COLOR_RANGE_UNKNOWN &&
           color->primaries != VMAF_COLOR_PRIMARIES_UNKNOWN &&
           color->trc != VMAF_COLOR_TRC_UNKNOWN && color->matrix != VMAF_COLOR_MATRIX_UNKNOWN;
}

struct VmafPictureConvertContext {
    zimg_filter_graph *graph;
    void *tmp;
    VmafPictureConvertTarget target;
    struct {
        enum VmafPixelFormat pix_fmt;
        unsigned bpc;
        unsigned w;
        unsigned h;
    } src;
};

/* Last zimg error as text; never fails, falls back to a fixed string. */
static void zimg_error_text(char *buf, size_t len)
{
    if (zimg_get_last_error(buf, len) == ZIMG_ERROR_SUCCESS)
        return;
    strncpy(buf, "no error text available", len - 1u);
    buf[len - 1u] = '\0';
}

static void context_free(VmafPictureConvertContext *c)
{
    if (!c)
        return;
    if (c->graph)
        zimg_filter_graph_free(c->graph);
    if (c->tmp)
        aligned_free(c->tmp);
    free(c);
}

static int validate_init_args(const VmafPicture *src, const VmafColor *src_color,
                              const VmafPictureConvertTarget *target)
{
    if (!color_is_specified(src_color)) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "vmaf_picture_convert_context_init: source picture "
                                       "color metadata must be fully specified\n");
        return -EINVAL;
    }
    if (!color_is_specified(&target->color)) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "vmaf_picture_convert_context_init: target color "
                                       "metadata must be fully specified\n");
        return -EINVAL;
    }
    if (target->bpc < CONVERT_BPC_MIN || target->bpc > CONVERT_BPC_MAX) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "vmaf_picture_convert_context_init: unsupported target "
                 "bit depth %u (supported: 8 to 16)\n",
                 target->bpc);
        return -EINVAL;
    }
    if (src->w[0] == 0 || src->h[0] == 0) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "vmaf_picture_convert_context_init: source picture has no size\n");
        return -EINVAL;
    }
    return 0;
}

static int build_graph(VmafPictureConvertContext *c, const VmafPicture *src,
                       const VmafColor *src_color)
{
    zimg_image_format src_fmt;
    zimg_image_format dst_fmt;
    int err = format_to_zimg(&src_fmt, src->pix_fmt, src->bpc, src->w[0], src->h[0], src_color);
    if (err) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "vmaf_picture_convert_context_init: source picture "
                                       "cannot be converted\n");
        return err;
    }
    err = format_to_zimg(&dst_fmt, c->target.pix_fmt, c->target.bpc, c->target.w, c->target.h,
                         &c->target.color);
    if (err) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "vmaf_picture_convert_context_init: target picture "
                                       "cannot be converted\n");
        return err;
    }

    zimg_graph_builder_params params;
    const zimg_graph_builder_params *params_ptr = &params;
    zimg_graph_builder_params_default(&params, ZIMG_API_VERSION);
    /* Match FFmpeg's zscale (agamma=1): exact transfer functions are ~20x
     * slower for PQ and change scores negligibly. (Netflix/vmaf 5c3f4fb90) */
    params.allow_approximate_gamma = 1;
    if (c->target.resample_filter != VMAF_RESAMPLE_DEFAULT)
        params.resample_filter = resample_filter_to_zimg(c->target.resample_filter);

    c->graph = zimg_filter_graph_build(&src_fmt, &dst_fmt, params_ptr);
    if (!c->graph) {
        char err_msg[CONVERT_ERR_LEN];
        zimg_error_text(err_msg, sizeof(err_msg));
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "vmaf_picture_convert_context_init: zimg_filter_graph_build failed: %s\n",
                 err_msg);
        return -EINVAL;
    }
    return 0;
}

static int allocate_tmp(VmafPictureConvertContext *c)
{
    size_t tmp_size = 0;
    if (zimg_filter_graph_get_tmp_size(c->graph, &tmp_size)) {
        char err_msg[CONVERT_ERR_LEN];
        zimg_error_text(err_msg, sizeof(err_msg));
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "vmaf_picture_convert_context_init: zimg_filter_graph_get_tmp_size "
                 "failed: %s\n",
                 err_msg);
        return -EINVAL;
    }
    if (tmp_size == 0)
        return 0;
    c->tmp = aligned_malloc(tmp_size, CONVERT_TMP_ALIGN);
    return c->tmp ? 0 : -ENOMEM;
}

int vmaf_picture_convert_context_init_with_color(VmafPictureConvertContext **ctx,
                                                 const VmafPicture *src, const VmafColor *src_color,
                                                 const VmafPictureConvertTarget *target)
{
    if (!ctx || !src || !src_color || !target)
        return -EINVAL;
    int err = validate_init_args(src, src_color, target);
    if (err)
        return err;

    VmafPictureConvertContext *c = calloc(1, sizeof(*c));
    if (!c)
        return -ENOMEM;
    c->target = *target;
    c->target.w = target->w ? target->w : src->w[0];
    c->target.h = target->h ? target->h : src->h[0];
    c->src.pix_fmt = src->pix_fmt;
    c->src.bpc = src->bpc;
    c->src.w = src->w[0];
    c->src.h = src->h[0];

    err = build_graph(c, src, src_color);
    if (!err)
        err = allocate_tmp(c);
    if (err) {
        context_free(c);
        return err;
    }
    *ctx = c;
    return 0;
}

static bool source_matches(const VmafPictureConvertContext *ctx, const VmafPicture *src)
{
    return src->pix_fmt == ctx->src.pix_fmt && src->bpc == ctx->src.bpc &&
           src->w[0] == ctx->src.w && src->h[0] == ctx->src.h;
}

int vmaf_picture_convert(VmafPictureConvertContext *ctx, VmafPicture *dst, const VmafPicture *src)
{
    if (!ctx || !dst || !src)
        return -EINVAL;
    if (!source_matches(ctx, src)) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "vmaf_picture_convert: source picture does not match "
                                       "the format the context was initialized with\n");
        return -EINVAL;
    }

    int err =
        vmaf_picture_alloc(dst, ctx->target.pix_fmt, ctx->target.bpc, ctx->target.w, ctx->target.h);
    if (err) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "vmaf_picture_convert: could not allocate target picture "
                 "(pix_fmt %d, bpc %u, %ux%u)\n",
                 (int)ctx->target.pix_fmt, ctx->target.bpc, ctx->target.w, ctx->target.h);
        return err;
    }

    zimg_image_buffer_const src_buf;
    zimg_image_buffer dst_buf;
    image_buffer_const_from_picture(&src_buf, src);
    image_buffer_from_picture(&dst_buf, dst);

    if (zimg_filter_graph_process(ctx->graph, &src_buf, &dst_buf, ctx->tmp, NULL, NULL, NULL,
                                  NULL)) {
        char err_msg[CONVERT_ERR_LEN];
        zimg_error_text(err_msg, sizeof(err_msg));
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "vmaf_picture_convert: zimg_filter_graph_process failed: %s\n", err_msg);
        err = vmaf_picture_unref(dst);
        if (err) {
            vmaf_log(VMAF_LOG_LEVEL_ERROR,
                     "vmaf_picture_convert: could not release the target picture (%d)\n", err);
        }
        return -EINVAL;
    }
    return 0;
}

int vmaf_picture_convert_context_close(VmafPictureConvertContext *ctx)
{
    if (!ctx)
        return -EINVAL;
    context_free(ctx);
    return 0;
}

#else /* !HAVE_ZIMG */

int vmaf_picture_convert_context_init_with_color(VmafPictureConvertContext **ctx,
                                                 const VmafPicture *src, const VmafColor *src_color,
                                                 const VmafPictureConvertTarget *target)
{
    (void)ctx;
    (void)src;
    (void)src_color;
    (void)target;
    vmaf_log(VMAF_LOG_LEVEL_ERROR,
             "vmaf_picture_convert_context_init_with_color: libvmaf was built without "
             "zimg support (configure with -Denable_zimg=true)\n");
    return -ENOTSUP;
}

int vmaf_picture_convert(VmafPictureConvertContext *ctx, VmafPicture *dst, const VmafPicture *src)
{
    (void)ctx;
    (void)dst;
    (void)src;
    vmaf_log(VMAF_LOG_LEVEL_ERROR, "vmaf_picture_convert: libvmaf was built without zimg "
                                   "support (configure with -Denable_zimg=true)\n");
    return -ENOTSUP;
}

int vmaf_picture_convert_context_close(VmafPictureConvertContext *ctx)
{
    (void)ctx;
    vmaf_log(VMAF_LOG_LEVEL_ERROR,
             "vmaf_picture_convert_context_close: libvmaf was built without zimg support "
             "(configure with -Denable_zimg=true)\n");
    return -ENOTSUP;
}

#endif /* HAVE_ZIMG */

/* NOLINTEND(modernize-use-nullptr) */
