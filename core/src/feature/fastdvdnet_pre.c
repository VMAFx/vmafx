/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  FastDVDnet temporal pre-filter (T6-7) — 5-frame-window denoiser.
 *
 *  Backed by an ONNX model with the contract
 *
 *      input  "frames"   : float32 NCHW [1, 5, H, W]
 *      output "denoised" : float32 NCHW [1, 1, H, W]
 *
 *  where the channel axis stacks the five luma planes
 *  ``[t-2, t-1, t, t+1, t+2]`` and the network emits a denoised version
 *  of frame ``t``. See docs/ai/models/fastdvdnet_pre.md and
 *  docs/adr/0215-fastdvdnet-pre-filter.md for the full surface contract.
 *
 *  Pre-filter, not a metric
 *  ------------------------
 *  This extractor is structurally a feature extractor (it registers in
 *  ``feature_extractor_list[]`` and is discoverable by name) but its
 *  *purpose* is to denoise frames before they reach the encoder pipeline,
 *  not to score a (ref, dist) pair. The score it appends —
 *  ``fastdvdnet_pre_l1_residual`` — is the per-frame mean-absolute
 *  difference between the input centre frame and the denoised output,
 *  capped to [0, 1]. It exists so the existing per-frame plumbing has a
 *  scalar to stash; downstream consumers that actually need the denoised
 *  frame buffer should pull it through the FFmpeg ``vmaf_pre`` filter
 *  shape (T6-7b will wire that path; this PR ships the libvmaf-side
 *  contract only).
 *
 *  Frame window edge handling
 *  --------------------------
 *  At the start of a clip we don't yet have ``t-2`` and ``t-1``; at the
 *  end we don't yet have ``t+1`` and ``t+2``. Both sides clamp to the
 *  available end frame (replicate-edge), which matches FastDVDnet's
 *  reference reflection-pad-light behaviour and lets the extractor emit
 *  a score on every frame index.
 *
 *  When libvmaf is built with -Denable_dnn=false, init() returns
 *  -ENOSYS before model-path probing so callers see the shared
 *  optional-runtime contract rather than a missing-model error.
 */

#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "libvmaf/dnn.h"
#include "libvmaf/picture.h"

#include "config.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "log.h"
#include "mem.h"
#include "opt.h"

#include "dnn/tiny_extractor_template.h"

#define FASTDVDNET_PRE_WINDOW 5u
#define FASTDVDNET_PRE_CENTRE 2u

typedef struct FastDvdnetPreState {
    char *model_path; /**< feature option, owned by opt.c */
    VmafDnnSession *sess;
    unsigned w, h;
    /* Ring buffer of normalised luma planes — five floats per pixel,
     * indexed circularly by frame index modulo FASTDVDNET_PRE_WINDOW.
     * `n_buffered` tracks how many distinct frames we've seen so far,
     * capped at FASTDVDNET_PRE_WINDOW. */
    float *frames[FASTDVDNET_PRE_WINDOW];
    unsigned n_buffered;
    unsigned next_slot;
    /* Scratch input tensor laid out [1, 5, H, W] in NCHW order. The
     * five frame planes are gathered out of the ring at extract() time. */
    float *input_tensor;
    float *output_tensor;
} FastDvdnetPreState;

/* Centre-and-scale luma plane (any bpc) into [0, 1] floats, row-major.
 * Caller guarantees `dst` holds w*h floats. */
static void luma_to_unit_float(const VmafPicture *pic, float *dst)
{
    const unsigned w = pic->w[0];
    const unsigned h = pic->h[0];
    const size_t stride = pic->stride[0];
    if (pic->bpc == 8) {
        const uint8_t *src = pic->data[0];
        const float inv = 1.0f / 255.0f;
        for (unsigned i = 0; i < h; ++i) {
            const uint8_t *row = src + (size_t)i * stride;
            float *out = dst + (size_t)i * (size_t)w;
            for (unsigned j = 0; j < w; ++j) {
                out[j] = (float)row[j] * inv;
            }
        }
    } else {
        const uint16_t *src = (const uint16_t *)pic->data[0];
        const size_t stride_px = stride / 2u;
        const float inv = 1.0f / (float)((1u << pic->bpc) - 1u);
        for (unsigned i = 0; i < h; ++i) {
            const uint16_t *row = src + (size_t)i * stride_px;
            float *out = dst + (size_t)i * (size_t)w;
            for (unsigned j = 0; j < w; ++j) {
                out[j] = (float)row[j] * inv;
            }
        }
    }
}

/* Index of the slot holding frame `t-k` (k in [0, n_buffered)) given
 * that `next_slot` is where the next push will land. The slot for
 * `t-0` (the most recent push) is `(next_slot + WINDOW - 1) % WINDOW`. */
static unsigned slot_for_offset(const FastDvdnetPreState *s, unsigned k)
{
    const unsigned head = (s->next_slot + FASTDVDNET_PRE_WINDOW - 1u) % FASTDVDNET_PRE_WINDOW;
    return (head + FASTDVDNET_PRE_WINDOW - k) % FASTDVDNET_PRE_WINDOW;
}

/* Free every aligned_malloc()-backed buffer hanging off `s` and zero the
 * slot pointers. Called from both the OOM unwind in init() and from
 * close() — keeps the cleanup logic in one place so a future buffer
 * addition can't get out of sync between the two call sites. */
static void release_buffers(FastDvdnetPreState *s)
{
    for (unsigned i = 0; i < FASTDVDNET_PRE_WINDOW; ++i) {
        if (s->frames[i]) {
            aligned_free(s->frames[i]);
            s->frames[i] = NULL;
        }
    }
    if (s->input_tensor) {
        aligned_free(s->input_tensor);
        s->input_tensor = NULL;
    }
    if (s->output_tensor) {
        aligned_free(s->output_tensor);
        s->output_tensor = NULL;
    }
}

static int allocate_buffers(FastDvdnetPreState *s, size_t plane)
{
    for (unsigned i = 0; i < FASTDVDNET_PRE_WINDOW; ++i) {
        s->frames[i] = (float *)aligned_malloc(plane * sizeof(float), 32u);
        if (!s->frames[i])
            return -ENOMEM;
    }
    s->input_tensor = (float *)aligned_malloc(FASTDVDNET_PRE_WINDOW * plane * sizeof(float), 32u);
    s->output_tensor = (float *)aligned_malloc(plane * sizeof(float), 32u);
    if (!s->input_tensor || !s->output_tensor)
        return -ENOMEM;
    return 0;
}

static int fastdvdnet_pre_init(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt,
                               unsigned bpc, unsigned w, unsigned h)
{
    FastDvdnetPreState *s = fex->priv;

    if (pix_fmt == VMAF_PIX_FMT_UNKNOWN)
        return -EINVAL;
    if (bpc != 8 && bpc != 10 && bpc != 12 && bpc != 16) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "fastdvdnet_pre: bpc=%u not supported\n", bpc);
        return -ENOTSUP;
    }

    int rc = vmaf_tiny_ai_require_runtime("fastdvdnet_pre");
    if (rc < 0)
        return rc;

    const char *path = vmaf_tiny_ai_resolve_model_path("fastdvdnet_pre", s->model_path,
                                                       "VMAF_FASTDVDNET_PRE_MODEL_PATH");
    if (!path)
        return -EINVAL;

    rc = vmaf_tiny_ai_open_session("fastdvdnet_pre", path, &s->sess);
    if (rc < 0)
        return rc;
    assert(s->sess != NULL);

    s->w = w;
    s->h = h;
    s->n_buffered = 0u;
    s->next_slot = 0u;

    rc = allocate_buffers(s, (size_t)w * (size_t)h);
    if (rc < 0) {
        release_buffers(s);
        vmaf_dnn_session_close(s->sess);
        s->sess = NULL;
        return rc;
    }
    return 0;
}

/* Stack the five window slots into the [1, 5, H, W] input tensor.
 * Slots beyond the buffered count replicate the closest available end
 * frame (centre-clamp at start, head-clamp at end of clip). */
static void gather_window(const FastDvdnetPreState *s, float *dst)
{
    const size_t plane = (size_t)s->w * (size_t)s->h;
    const unsigned have = s->n_buffered;
    /* Choose a "centre" frame within the ring whose *available* offset
     * range covers [-2, +2] when possible; at clip start the centre is
     * the most recent frame so all five slots replicate forward. */
    for (unsigned k = 0; k < FASTDVDNET_PRE_WINDOW; ++k) {
        /* Channel order: [t-2, t-1, t, t+1, t+2].  We only have past
         * frames in the ring, so at runtime t is the most recent push,
         * and t+1 / t+2 replicate t. */
        unsigned offset_back; /* 0 = most recent, 1 = previous, ... */
        if (k <= FASTDVDNET_PRE_CENTRE) {
            offset_back = FASTDVDNET_PRE_CENTRE - k;
        } else {
            offset_back = 0u;
        }
        if (offset_back >= have)
            offset_back = have - 1u;
        const unsigned slot = slot_for_offset(s, offset_back);
        memcpy(dst + (size_t)k * plane, s->frames[slot], plane * sizeof(float));
    }
}

static double mean_abs_residual(const float *centre, const float *denoised, size_t n)
{
    double acc = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const float d = centre[i] - denoised[i];
        acc += d < 0.0f ? (double)-d : (double)d;
    }
    return n > 0u ? acc / (double)n : 0.0;
}

static int fastdvdnet_pre_extract(VmafFeatureExtractor *fex, VmafPicture *ref_pic,
                                  VmafPicture *ref_pic_90, VmafPicture *dist_pic,
                                  VmafPicture *dist_pic_90, unsigned index,
                                  VmafFeatureCollector *feature_collector)
{
    (void)ref_pic_90;
    (void)dist_pic;
    (void)dist_pic_90;
    FastDvdnetPreState *s = fex->priv;
    assert(s != NULL);
    assert(s->sess != NULL);

    if (ref_pic->w[0] != s->w || ref_pic->h[0] != s->h)
        return -ERANGE;

    /* Push the new luma frame into the ring at next_slot. */
    luma_to_unit_float(ref_pic, s->frames[s->next_slot]);
    s->next_slot = (s->next_slot + 1u) % FASTDVDNET_PRE_WINDOW;
    if (s->n_buffered < FASTDVDNET_PRE_WINDOW)
        s->n_buffered += 1u;

    /* Build the [1, 5, H, W] input tensor. */
    gather_window(s, s->input_tensor);

    const size_t plane = (size_t)s->w * (size_t)s->h;
    const int64_t in_shape[4] = {1, (int64_t)FASTDVDNET_PRE_WINDOW, (int64_t)s->h, (int64_t)s->w};
    VmafDnnInput inputs[1] = {
        {.name = "frames", .data = s->input_tensor, .shape = in_shape, .rank = 4},
    };
    VmafDnnOutput outputs[1] = {
        {.name = "denoised", .data = s->output_tensor, .capacity = plane, .written = 0u},
    };

    int rc = vmaf_dnn_session_run(s->sess, inputs, 1u, outputs, 1u);
    if (rc < 0)
        return rc;
    if (outputs[0].written == 0u)
        return -EIO;

    /* Score is the mean-abs residual between the centre input frame
     * and the denoised output — strictly a sanity scalar, not a quality
     * metric. The denoised frame buffer itself is consumed by the
     * FFmpeg-side filter that T6-7b will land. */
    const float *centre_plane = s->input_tensor + (size_t)FASTDVDNET_PRE_CENTRE * plane;
    const double residual = mean_abs_residual(centre_plane, s->output_tensor, plane);
    return vmaf_feature_collector_append(feature_collector, "fastdvdnet_pre_l1_residual", residual,
                                         index);
}

static int fastdvdnet_pre_close(VmafFeatureExtractor *fex)
{
    FastDvdnetPreState *s = fex->priv;
    if (!s)
        return 0;
    release_buffers(s);
    if (s->sess)
        vmaf_dnn_session_close(s->sess);
    memset(s, 0, sizeof(*s));
    return 0;
}

static const VmafOption fastdvdnet_pre_options[] = {
    VMAF_TINY_AI_MODEL_PATH_OPTION(
        FastDvdnetPreState,
        "Filesystem path to the FastDVDnet ONNX model (5-frame window 'frames' input, "
        "single-frame 'denoised' output). Overrides the VMAF_FASTDVDNET_PRE_MODEL_PATH env var."),
    {0},
};

static const char *fastdvdnet_pre_provided_features[] = {"fastdvdnet_pre_l1_residual", NULL};

VmafFeatureExtractor vmaf_fex_fastdvdnet_pre = {
    .name = "fastdvdnet_pre",
    .init = fastdvdnet_pre_init,
    .extract = fastdvdnet_pre_extract,
    .close = fastdvdnet_pre_close,
    .options = fastdvdnet_pre_options,
    .priv_size = sizeof(FastDvdnetPreState),
    .provided_features = fastdvdnet_pre_provided_features,
    /* Explicit zero-init so GCC LTO sees the full struct layout
     * across TUs (silences -Wlto-type-mismatch; ADR-0181 precedent
     * shared with feature_lpips.c). */
    .chars = {0},
};
