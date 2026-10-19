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
 * Port of Netflix/vmaf 700124a4c test_picture_wrap_integration.c (ADR-2949):
 * the cases are upstream's. Adapted: the test links the public libvmaf
 * (libvmaf.so.3 on the VMAFx API) and includes no private header, the model
 * is destroyed and the decoders are closed on every path, and the frame
 * buffers are sized from the chroma extents vmaf_picture_alloc() gives
 * (ADR-1483; the same as upstream's at the even 320x240 used here).
 */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "libvmaf/libvmaf.h"
#include "test.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

#define DEC_W 320u
#define DEC_H 240u
#define DEC_FRAMES 5u

typedef struct UserVideoDecoder {
    unsigned w, h, bpc;
    enum VmafPixelFormat pix_fmt;
    uint8_t *frame_buffers[DEC_FRAMES];
    unsigned frame_count;
} UserVideoDecoder;

/* Plane widths and heights in bytes and rows: chroma rounded up. */
static void decoder_planes(const UserVideoDecoder *dec, size_t *y_sz, size_t *uv_sz,
                           ptrdiff_t stride[3])
{
    const unsigned ss_hor = dec->pix_fmt != VMAF_PIX_FMT_YUV444P;
    const unsigned ss_ver = dec->pix_fmt == VMAF_PIX_FMT_YUV420P;
    const unsigned w_c = (dec->w + ss_hor) >> ss_hor;
    const unsigned h_c = (dec->h + ss_ver) >> ss_ver;
    const unsigned hbd = dec->bpc > 8u;
    const size_t luma_row = (size_t)dec->w << hbd;
    const size_t chroma_row = (size_t)w_c << hbd;
    stride[0] = (ptrdiff_t)luma_row;
    stride[1] = stride[2] = (ptrdiff_t)chroma_row;
    *y_sz = (size_t)stride[0] * dec->h;
    *uv_sz = (size_t)stride[1] * h_c;
}

static void user_decoder_close(UserVideoDecoder *dec)
{
    for (unsigned i = 0; i < dec->frame_count; i++) {
        free(dec->frame_buffers[i]);
        dec->frame_buffers[i] = NULL;
    }
    dec->frame_count = 0;
}

static int user_decoder_init(UserVideoDecoder *dec, unsigned w, unsigned h, unsigned bpc,
                             enum VmafPixelFormat pix_fmt)
{
    memset(dec, 0, sizeof(*dec));
    dec->w = w;
    dec->h = h;
    dec->bpc = bpc;
    dec->pix_fmt = pix_fmt;
    size_t y_sz = 0;
    size_t uv_sz = 0;
    ptrdiff_t stride[3];
    decoder_planes(dec, &y_sz, &uv_sz, stride);
    const size_t frame_sz = y_sz + (2u * uv_sz);
    for (unsigned i = 0; i < DEC_FRAMES; i++) {
        dec->frame_buffers[i] = malloc(frame_sz);
        if (!dec->frame_buffers[i]) {
            user_decoder_close(dec);
            return -1;
        }
        dec->frame_count = i + 1u;
        memset(dec->frame_buffers[i], (int)(i * 10u), frame_sz);
    }
    return 0;
}

static int user_decoder_get_frame(const UserVideoDecoder *dec, unsigned frame_idx, void *data[3],
                                  ptrdiff_t stride[3])
{
    if (frame_idx >= dec->frame_count) {
        return -1;
    }
    size_t y_sz = 0;
    size_t uv_sz = 0;
    decoder_planes(dec, &y_sz, &uv_sz, stride);
    uint8_t *buf = dec->frame_buffers[frame_idx];
    data[0] = buf;
    data[1] = buf + y_sz;
    data[2] = buf + y_sz + uv_sz;
    return 0;
}

static VmafPictureWrapped wrapped_frame(const UserVideoDecoder *dec, void *const data[3],
                                        const ptrdiff_t stride[3])
{
    VmafPictureWrapped wrapped;
    memset(&wrapped, 0, sizeof(wrapped));
    wrapped.pix_fmt = dec->pix_fmt;
    wrapped.bpc = dec->bpc;
    wrapped.w = dec->w;
    wrapped.h = dec->h;
    for (unsigned p = 0; p < 3u; p++) {
        wrapped.data[p] = data[p];
        wrapped.stride[p] = stride[p];
    }
    return wrapped;
}

/* Wrap frame `i` of each decoder and hand the pair to the context. */
static int read_wrapped_pair(VmafContext *vmaf, const UserVideoDecoder *ref_dec,
                             const UserVideoDecoder *dist_dec, unsigned i)
{
    void *ref_data[3];
    void *dist_data[3];
    ptrdiff_t ref_stride[3];
    ptrdiff_t dist_stride[3];
    if (user_decoder_get_frame(ref_dec, i, ref_data, ref_stride) ||
        user_decoder_get_frame(dist_dec, i, dist_data, dist_stride)) {
        return -1;
    }
    VmafPicture ref_pic;
    VmafPicture dist_pic;
    int err = vmaf_picture_wrap(&ref_pic, wrapped_frame(ref_dec, ref_data, ref_stride));
    if (err) {
        return err;
    }
    err = vmaf_picture_wrap(&dist_pic, wrapped_frame(dist_dec, dist_data, dist_stride));
    if (err) {
        (void)vmaf_picture_unref(&ref_pic);
        return err;
    }
    return vmaf_read_pictures(vmaf, &ref_pic, &dist_pic, i);
}

/* Score the decoders' frames through wrapped pictures; the context is closed
 * before the frame buffers are freed. */
static int score_wrapped(const UserVideoDecoder *ref_dec, const UserVideoDecoder *dist_dec,
                         double *vmaf_score)
{
    VmafConfiguration cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.log_level = VMAF_LOG_LEVEL_INFO;
    VmafContext *vmaf = NULL;
    int err = vmaf_init(&vmaf, cfg);
    if (err) {
        return err;
    }
    VmafModelConfig model_cfg;
    memset(&model_cfg, 0, sizeof(model_cfg));
    VmafModel *model = NULL;
    err = vmaf_model_load(&model, &model_cfg, "vmaf_v0.6.1");
    if (!err) {
        err = vmaf_use_features_from_model(vmaf, model);
    }
    for (unsigned i = 0; i < DEC_FRAMES && !err; i++) {
        err = read_wrapped_pair(vmaf, ref_dec, dist_dec, i);
    }
    if (!err) {
        err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    }
    if (!err) {
        err = vmaf_score_pooled(vmaf, model, VMAF_POOL_METHOD_MEAN, vmaf_score, 0, DEC_FRAMES - 1u);
    }
    const int close_err = vmaf_close(vmaf);
    if (model) {
        vmaf_model_destroy(model);
    }
    return err ? err : close_err;
}

static char *test_picture_wrap_with_vmaf_zero_copy(void)
{
    UserVideoDecoder ref_dec;
    UserVideoDecoder dist_dec;
    int err = user_decoder_init(&ref_dec, DEC_W, DEC_H, 8, VMAF_PIX_FMT_YUV420P);
    mu_assert("failed to init ref decoder", !err);
    err = user_decoder_init(&dist_dec, DEC_W, DEC_H, 8, VMAF_PIX_FMT_YUV420P);
    if (err) {
        user_decoder_close(&ref_dec);
    }
    mu_assert("failed to init dist decoder", !err);

    double vmaf_score = 0.0;
    err = score_wrapped(&ref_dec, &dist_dec, &vmaf_score);
    user_decoder_close(&ref_dec);
    user_decoder_close(&dist_dec);
    mu_assert("problem while scoring wrapped pictures", !err);
    return NULL;
}

typedef struct UserFrameContext {
    unsigned frame_idx;
    int cleanup_called;
    void *buffer_ptr;
} UserFrameContext;

static int user_frame_cleanup(VmafPicture *pic, void *cookie)
{
    (void)pic;
    UserFrameContext *ctx = (UserFrameContext *)cookie;
    ctx->cleanup_called = 1;
    return 0;
}

static char *test_picture_wrap_with_cleanup_callback(void)
{
    const unsigned w = DEC_W;
    const unsigned h = DEC_H;
    const size_t y_sz = (size_t)w * h;
    const size_t uv_sz = (size_t)(w / 2u) * (h / 2u);
    const size_t frame_sz = y_sz + (2u * uv_sz);

    uint8_t *user_buffer = malloc(frame_sz);
    mu_assert("failed to allocate user buffer", user_buffer != NULL);
    memset(user_buffer, 128, frame_sz);

    UserFrameContext ctx = {.frame_idx = 0, .cleanup_called = 0, .buffer_ptr = user_buffer};

    VmafPicture pic;
    VmafPictureWrapped pic_wrapped;
    memset(&pic_wrapped, 0, sizeof(pic_wrapped));
    pic_wrapped.pix_fmt = VMAF_PIX_FMT_YUV420P;
    pic_wrapped.bpc = 8;
    pic_wrapped.w = w;
    pic_wrapped.h = h;
    pic_wrapped.data[0] = user_buffer;
    pic_wrapped.data[1] = user_buffer + y_sz;
    pic_wrapped.data[2] = user_buffer + y_sz + uv_sz;
    pic_wrapped.stride[0] = (ptrdiff_t)w;
    pic_wrapped.stride[1] = pic_wrapped.stride[2] = (ptrdiff_t)(w / 2u);
    pic_wrapped.cookie = &ctx;
    pic_wrapped.release_picture = user_frame_cleanup;

    int err = vmaf_picture_wrap(&pic, pic_wrapped);
    const int early = ctx.cleanup_called;
    if (!err) {
        err = vmaf_picture_unref(&pic);
    }
    const int intact = ((uint8_t *)ctx.buffer_ptr)[0] == 128;
    free(user_buffer);
    mu_assert("problem during vmaf_picture_wrap or vmaf_picture_unref", !err);
    mu_assert("callback called too early", early == 0);
    mu_assert("user cleanup callback not called", ctx.cleanup_called == 1);
    mu_assert("user buffer was corrupted", intact);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_picture_wrap_with_vmaf_zero_copy);
    mu_run_test(test_picture_wrap_with_cleanup_callback);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
