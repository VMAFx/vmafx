/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  TransNet V2 shot-boundary detector (T6-3a) — 100-frame sliding window.
 *
 *  Backed by an ONNX model with the contract
 *
 *      input  "frames"           : float32 NCHW-like [1, 100, 3, 27, 48]
 *                                  (100-frame window, RGB, 27x48 thumbnails)
 *      output "boundary_logits"  : float32 [1, 100]
 *                                  (per-frame shot-boundary logits)
 *
 *  The 27x48 thumbnail size and 100-frame window match the published
 *  Soucek & Lokoc 2020 TransNet V2 architecture. The extractor downsamples
 *  the luma plane (broadcast across the 3 RGB channels for the placeholder
 *  smoke graph), maintains an internal 100-slot ring buffer, runs the
 *  network once per `extract()` call, and emits the centre-of-window
 *  shot-boundary probability for the current frame index.
 *
 *  Per-frame plumbing (T6-3a scope)
 *  --------------------------------
 *  This PR ships only the *feature* side: per-frame
 *  ``shot_boundary_probability`` (sigmoid of the model logit) and a
 *  binary ``shot_boundary`` flag thresholded at 0.5. The downstream
 *  per-shot CRF predictor (which would aggregate the boundary list into
 *  shot intervals and emit a CRF target per shot) lives behind backlog
 *  item T6-3b — it consumes these per-frame probabilities through the
 *  feature collector.
 *
 *  Edge handling
 *  -------------
 *  Until the ring has 100 frames buffered we replicate the most-recent
 *  available frame across the missing slots, so the network always sees
 *  a well-formed [1, 100, 3, 27, 48] tensor. The reported probability is
 *  always taken from the slot corresponding to the *most recent push*
 *  (equivalent to the centre of the window once the ring is full;
 *  before that, the prediction is delayed-onset and downstream
 *  consumers should treat the first ~50 frames as warm-up).
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

#define TRANSNET_V2_WINDOW 100u
#define TRANSNET_V2_CHANNELS 3u
#define TRANSNET_V2_HEIGHT 27u
#define TRANSNET_V2_WIDTH 48u
/* Number of float32 elements in one frame (RGB 27x48). */
#define TRANSNET_V2_FRAME_ELEMS                                                                    \
    ((size_t)TRANSNET_V2_CHANNELS * (size_t)TRANSNET_V2_HEIGHT * (size_t)TRANSNET_V2_WIDTH)
#define TRANSNET_V2_BOUNDARY_THRESHOLD 0.5

typedef struct TransNetV2State {
    char *model_path; /**< feature option, owned by opt.c */
    VmafDnnSession *sess;
    unsigned w, h;
    /* Ring buffer of pre-resized frame tensors — one slot per frame in the
     * 100-frame window. Each slot holds a [3, 27, 48] float32 block (RGB
     * broadcast from luma for the placeholder graph). next_slot is where
     * the next push will land; n_buffered is capped at WINDOW. */
    float *frames[TRANSNET_V2_WINDOW];
    unsigned n_buffered;
    unsigned next_slot;
    /* Scratch input tensor laid out [1, 100, 3, 27, 48] in row-major. The
     * 100 frame planes are gathered out of the ring at extract() time. */
    float *input_tensor;
    /* Output logits buffer [1, 100]. */
    float *output_logits;
} TransNetV2State;

/* Sigmoid in double precision — clamped against ±large args to avoid
 * exp() overflow on a poorly-trained placeholder graph that may produce
 * extreme logits. */
static double safe_sigmoid(double x)
{
    if (x >= 30.0)
        return 1.0;
    if (x <= -30.0)
        return 0.0;
    return 1.0 / (1.0 + exp(-x));
}

/* Bilinear-ish nearest-neighbour resize from the input luma plane to a
 * TRANSNET_V2_HEIGHT x TRANSNET_V2_WIDTH grid, broadcasting the result
 * to all three RGB channels. Caller guarantees `dst` holds
 * TRANSNET_V2_FRAME_ELEMS floats laid out as [3, H, W].
 *
 * Nearest-neighbour rather than bilinear because the placeholder graph
 * is smoke-only (the published TransNet V2 reference uses bilinear);
 * T6-3a-followup will switch to bilinear when real upstream weights
 * land.
 */
static void luma_to_thumbnail(const VmafPicture *pic, float *dst)
{
    const unsigned src_w = pic->w[0];
    const unsigned src_h = pic->h[0];
    const size_t stride = pic->stride[0];
    /* Channel-0 plane (luma) — write a 27x48 grid first, then memcpy
     * across the other two RGB channels so the placeholder net sees a
     * sane RGB-shaped input. */
    float *plane0 = dst;
    if (pic->bpc == 8) {
        const uint8_t *src = pic->data[0];
        const float inv = 1.0f / 255.0f;
        for (unsigned i = 0; i < TRANSNET_V2_HEIGHT; ++i) {
            const unsigned src_i = (i * src_h) / TRANSNET_V2_HEIGHT;
            const uint8_t *row = src + (size_t)src_i * stride;
            float *out = plane0 + (size_t)i * (size_t)TRANSNET_V2_WIDTH;
            for (unsigned j = 0; j < TRANSNET_V2_WIDTH; ++j) {
                const unsigned src_j = (j * src_w) / TRANSNET_V2_WIDTH;
                out[j] = (float)row[src_j] * inv;
            }
        }
    } else {
        const uint16_t *src = (const uint16_t *)pic->data[0];
        const size_t stride_px = stride / 2u;
        const float inv = 1.0f / (float)((1u << pic->bpc) - 1u);
        for (unsigned i = 0; i < TRANSNET_V2_HEIGHT; ++i) {
            const unsigned src_i = (i * src_h) / TRANSNET_V2_HEIGHT;
            const uint16_t *row = src + (size_t)src_i * stride_px;
            float *out = plane0 + (size_t)i * (size_t)TRANSNET_V2_WIDTH;
            for (unsigned j = 0; j < TRANSNET_V2_WIDTH; ++j) {
                const unsigned src_j = (j * src_w) / TRANSNET_V2_WIDTH;
                out[j] = (float)row[src_j] * inv;
            }
        }
    }
    const size_t plane_elems = (size_t)TRANSNET_V2_HEIGHT * (size_t)TRANSNET_V2_WIDTH;
    /* Broadcast luma to channels 1 and 2 (G and B) — placeholder graph
     * doesn't care about colour. Real upstream weights will need an
     * actual RGB decode; T6-3a-followup. */
    memcpy(dst + plane_elems, plane0, plane_elems * sizeof(float));
    memcpy(dst + 2u * plane_elems, plane0, plane_elems * sizeof(float));
}

/* Index of the slot holding frame `t-k` (k in [0, n_buffered)) given
 * that `next_slot` is where the next push will land. The slot for
 * `t-0` (the most recent push) is `(next_slot + WINDOW - 1) % WINDOW`. */
static unsigned slot_for_offset(const TransNetV2State *s, unsigned k)
{
    const unsigned head = (s->next_slot + TRANSNET_V2_WINDOW - 1u) % TRANSNET_V2_WINDOW;
    return (head + TRANSNET_V2_WINDOW - k) % TRANSNET_V2_WINDOW;
}

/* Model-path resolution lives in `dnn/tiny_extractor_template.h`
 * (`vmaf_tiny_ai_resolve_model_path`), shared with feature_lpips.c /
 * feature_mobilesal.c / fastdvdnet_pre.c. */

/* Free every aligned_malloc()-backed buffer hanging off `s` and zero the
 * slot pointers. Called from both the OOM unwind in init() and from
 * close() — keeps the cleanup logic in one place so a future buffer
 * addition can't get out of sync between the two call sites. */
static void release_buffers(TransNetV2State *s)
{
    for (unsigned i = 0; i < TRANSNET_V2_WINDOW; ++i) {
        if (s->frames[i]) {
            aligned_free(s->frames[i]);
            s->frames[i] = NULL;
        }
    }
    if (s->input_tensor) {
        aligned_free(s->input_tensor);
        s->input_tensor = NULL;
    }
    if (s->output_logits) {
        aligned_free(s->output_logits);
        s->output_logits = NULL;
    }
}

static int allocate_buffers(TransNetV2State *s)
{
    for (unsigned i = 0; i < TRANSNET_V2_WINDOW; ++i) {
        s->frames[i] = (float *)aligned_malloc(TRANSNET_V2_FRAME_ELEMS * sizeof(float), 32u);
        if (!s->frames[i])
            return -ENOMEM;
    }
    s->input_tensor = (float *)aligned_malloc(
        (size_t)TRANSNET_V2_WINDOW * TRANSNET_V2_FRAME_ELEMS * sizeof(float), 32u);
    s->output_logits = (float *)aligned_malloc((size_t)TRANSNET_V2_WINDOW * sizeof(float), 32u);
    if (!s->input_tensor || !s->output_logits)
        return -ENOMEM;
    return 0;
}

static int transnet_v2_init(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                            unsigned w, unsigned h)
{
    TransNetV2State *s = fex->priv;

    if (pix_fmt == VMAF_PIX_FMT_UNKNOWN)
        return -EINVAL;
    if (bpc != 8 && bpc != 10 && bpc != 12 && bpc != 16) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "transnet_v2: bpc=%u not supported\n", bpc);
        return -ENOTSUP;
    }

    int rc = vmaf_tiny_ai_require_runtime("transnet_v2");
    if (rc < 0) {
        return rc;
    }

    const char *path = vmaf_tiny_ai_resolve_model_path("transnet_v2", s->model_path,
                                                       "VMAF_TRANSNET_V2_MODEL_PATH");
    if (!path) {
        return -EINVAL;
    }
    rc = vmaf_tiny_ai_open_session("transnet_v2", path, &s->sess);
    if (rc < 0) {
        return rc;
    }
    assert(s->sess != NULL);

    s->w = w;
    s->h = h;
    s->n_buffered = 0u;
    s->next_slot = 0u;

    rc = allocate_buffers(s);
    if (rc < 0) {
        release_buffers(s);
        vmaf_dnn_session_close(s->sess);
        s->sess = NULL;
        return rc;
    }
    return 0;
}

/* Stack the 100 window slots into the [1, 100, 3, 27, 48] input tensor.
 * Slots beyond the buffered count replicate the most-recent available
 * frame (head-clamp at clip start). */
static void gather_window(const TransNetV2State *s, float *dst)
{
    const unsigned have = s->n_buffered;
    for (unsigned k = 0; k < TRANSNET_V2_WINDOW; ++k) {
        /* Channel order: slot k is `t - (WINDOW - 1 - k)` so the most
         * recent push lands at the LAST slot (index WINDOW-1) and we
         * read the corresponding logit out of `output_logits[WINDOW-1]`
         * in extract(). At clip start, slots that would correspond to
         * pre-clip frames replicate the oldest available frame. */
        unsigned offset_back = TRANSNET_V2_WINDOW - 1u - k;
        if (offset_back >= have)
            offset_back = have - 1u;
        const unsigned slot = slot_for_offset(s, offset_back);
        memcpy(dst + (size_t)k * TRANSNET_V2_FRAME_ELEMS, s->frames[slot],
               TRANSNET_V2_FRAME_ELEMS * sizeof(float));
    }
}

static int transnet_v2_extract(VmafFeatureExtractor *fex, VmafPicture *ref_pic,
                               VmafPicture *ref_pic_90, VmafPicture *dist_pic,
                               VmafPicture *dist_pic_90, unsigned index,
                               VmafFeatureCollector *feature_collector)
{
    (void)ref_pic_90;
    (void)dist_pic;
    (void)dist_pic_90;
    TransNetV2State *s = fex->priv;
    assert(s != NULL);
    assert(s->sess != NULL);

    if (ref_pic->w[0] != s->w || ref_pic->h[0] != s->h)
        return -ERANGE;

    /* Push the new (downsampled, RGB-broadcast) frame into the ring at
     * next_slot. */
    luma_to_thumbnail(ref_pic, s->frames[s->next_slot]);
    s->next_slot = (s->next_slot + 1u) % TRANSNET_V2_WINDOW;
    if (s->n_buffered < TRANSNET_V2_WINDOW)
        s->n_buffered += 1u;

    /* Build the [1, 100, 3, 27, 48] input tensor. */
    gather_window(s, s->input_tensor);

    const int64_t in_shape[5] = {1, (int64_t)TRANSNET_V2_WINDOW, (int64_t)TRANSNET_V2_CHANNELS,
                                 (int64_t)TRANSNET_V2_HEIGHT, (int64_t)TRANSNET_V2_WIDTH};
    VmafDnnInput inputs[1] = {
        {.name = "frames", .data = s->input_tensor, .shape = in_shape, .rank = 5},
    };
    VmafDnnOutput outputs[1] = {
        {.name = "boundary_logits",
         .data = s->output_logits,
         .capacity = (size_t)TRANSNET_V2_WINDOW,
         .written = 0u},
    };

    int rc = vmaf_dnn_session_run(s->sess, inputs, 1u, outputs, 1u);
    if (rc < 0)
        return rc;
    if (outputs[0].written == 0u)
        return -EIO;

    /* The most recent frame's logit is at slot WINDOW-1 (matches the
     * gather_window layout above). Emit both the sigmoid probability
     * and a binary flag thresholded at 0.5 — downstream consumers
     * (per-shot CRF predictor T6-3b, ffmpeg shot-cut filter) bind to
     * these two feature names. */
    const double logit = (double)s->output_logits[TRANSNET_V2_WINDOW - 1u];
    const double prob = safe_sigmoid(logit);
    const double flag = prob >= TRANSNET_V2_BOUNDARY_THRESHOLD ? 1.0 : 0.0;

    rc = vmaf_feature_collector_append(feature_collector, "shot_boundary_probability", prob, index);
    if (rc < 0)
        return rc;
    return vmaf_feature_collector_append(feature_collector, "shot_boundary", flag, index);
}

static int transnet_v2_close(VmafFeatureExtractor *fex)
{
    TransNetV2State *s = fex->priv;
    if (!s)
        return 0;
    release_buffers(s);
    if (s->sess)
        vmaf_dnn_session_close(s->sess);
    memset(s, 0, sizeof(*s));
    return 0;
}

static const VmafOption transnet_v2_options[] = {
    VMAF_TINY_AI_MODEL_PATH_OPTION(
        TransNetV2State, "Filesystem path to the TransNet V2 ONNX model "
                         "(input 'frames' [1, 100, 3, 27, 48], output 'boundary_logits' [1, 100]). "
                         "Overrides the VMAF_TRANSNET_V2_MODEL_PATH env var."),
    {0},
};

static const char *transnet_v2_provided_features[] = {"shot_boundary_probability", "shot_boundary",
                                                      NULL};

VmafFeatureExtractor vmaf_fex_transnet_v2 = {
    .name = "transnet_v2",
    .init = transnet_v2_init,
    .extract = transnet_v2_extract,
    .close = transnet_v2_close,
    .options = transnet_v2_options,
    .priv_size = sizeof(TransNetV2State),
    .provided_features = transnet_v2_provided_features,
    /* Explicit zero-init so GCC LTO sees the full struct layout
     * across TUs (silences -Wlto-type-mismatch; ADR-0181 precedent
     * shared with feature_lpips.c). */
    .chars = {0},
};
