/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 *  TransNet V2 shot-boundary detector (T6-3a) — 100-frame windows.
 *
 *  Backed by an ONNX model with the contract
 *
 *      input  "frames" : float32 [1, 100, 3, 27, 48]
 *                        (100 frames of 27x48 RGB thumbnails, 0..255)
 *      output (one)    : float32 [1, 100] per-frame shot-boundary logits
 *
 *  The shipped graph names its output `output_0` (tf2onnx); the extractor
 *  binds it by position, so a re-export under another name still runs.
 *
 *  Windows (ADR-1527)
 *  ------------------
 *  The extractor reproduces upstream TransNet V2's `predict_frames()`: the
 *  clip is padded with 25 copies of its first frame in front and copies of
 *  its last frame behind, windows of 100 frames advance by 50, and each
 *  window reports its middle 50 frames (slots 25..74). Window k covers
 *  frames 50k-25 .. 50k+74 and runs as soon as frame 50k+74 has been read;
 *  the windows the clip's end leaves open run in flush() with the last
 *  frame repeated. A frame's probability is therefore written up to 74
 *  frames after it is read, and every frame sees at least 25 frames of
 *  context on each side, as upstream does. A logit read from the window's
 *  last slot (the frame just read) sees no frame after it and stays below
 *  0.5 on a hard cut.
 *
 *  Samples are fed in the 0..255 range upstream's frames have: the
 *  ColorHistograms branch casts them to integers and bins them with
 *  `>> 5`, so a 0..1 range collapses every histogram into one bin and the
 *  network sees no cut. The luma plane is broadcast to the three channels
 *  and resized by nearest neighbour (upstream decodes RGB and scales with
 *  ffmpeg); both are documented limits of the C path.
 *
 *  The shot-boundary features mark the last frame of a shot, as upstream's
 *  `predictions_to_scenes()` reads them.
 *
 *  When libvmaf is built with -Denable_dnn=false, init() returns
 *  -ENOSYS before model-path probing so callers see the shared
 *  optional-runtime contract rather than a missing-model error.
 */

#include <assert.h>

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe, and this file mirrors
 * the C spelling of the surface it exercises. ADR-1138. */
#include <errno.h>
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
#include "transnet_v2_score.h"

#include "dnn/tiny_extractor_template.h"

#define TRANSNET_V2_WINDOW 100u
/* Upstream predict_frames(): windows advance by 50 frames and report their
 * middle 50 (slots 25..74), with 25 frames of padding before the clip. */
#define TRANSNET_V2_STRIDE 50u
#define TRANSNET_V2_CONTEXT 25u
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
    /* Thumbnails of the last 100 frames read; frame f lives in slot
     * f % WINDOW. Each slot holds a [3, 27, 48] float32 block. */
    float *frames[TRANSNET_V2_WINDOW];
    /* Frames read so far; the next frame must carry this index. */
    unsigned n_read;
    /* First frame whose probability has not been written; a multiple of
     * TRANSNET_V2_STRIDE (the window that writes it starts there). */
    unsigned next_emit;
    /* Scratch input tensor laid out [1, 100, 3, 27, 48] in row-major. */
    float *input_tensor;
    /* Output logits buffer [1, 100]. */
    float *output_logits;
} TransNetV2State;

/* Nearest-neighbour resize of the luma plane to a 27x48 grid in the 0..255
 * range upstream's RGB frames have (ADR-1527), broadcast to the three
 * channels. Samples above 8 bits are scaled by 255 / (2^bpc - 1). Caller
 * guarantees `dst` holds TRANSNET_V2_FRAME_ELEMS floats laid out [3, H, W]. */
static void luma_to_thumbnail(const VmafPicture *pic, float *dst)
{
    const unsigned src_w = pic->w[0];
    const unsigned src_h = pic->h[0];
    const size_t stride = pic->stride[0];
    float *plane0 = dst;
    if (pic->bpc == 8) {
        const uint8_t *src = pic->data[0];
        for (unsigned i = 0; i < TRANSNET_V2_HEIGHT; ++i) {
            const unsigned src_i = (i * src_h) / TRANSNET_V2_HEIGHT;
            const uint8_t *row = src + (size_t)src_i * stride;
            float *out = plane0 + (size_t)i * (size_t)TRANSNET_V2_WIDTH;
            for (unsigned j = 0; j < TRANSNET_V2_WIDTH; ++j) {
                const unsigned src_j = (j * src_w) / TRANSNET_V2_WIDTH;
                out[j] = (float)row[src_j];
            }
        }
    } else {
        const uint16_t *src = (const uint16_t *)pic->data[0];
        const size_t stride_px = stride / 2u;
        const float scale = 255.0f / (float)((1u << pic->bpc) - 1u);
        for (unsigned i = 0; i < TRANSNET_V2_HEIGHT; ++i) {
            const unsigned src_i = (i * src_h) / TRANSNET_V2_HEIGHT;
            const uint16_t *row = src + (size_t)src_i * stride_px;
            float *out = plane0 + (size_t)i * (size_t)TRANSNET_V2_WIDTH;
            for (unsigned j = 0; j < TRANSNET_V2_WIDTH; ++j) {
                const unsigned src_j = (j * src_w) / TRANSNET_V2_WIDTH;
                out[j] = (float)row[src_j] * scale;
            }
        }
    }
    const size_t plane_elems = (size_t)TRANSNET_V2_HEIGHT * (size_t)TRANSNET_V2_WIDTH;
    /* Broadcast luma to channels 1 and 2 (G and B). */
    memcpy(dst + plane_elems, plane0, plane_elems * sizeof(float));
    memcpy(dst + 2u * plane_elems, plane0, plane_elems * sizeof(float));
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

    char env_path[VMAF_TINY_AI_ENV_PATH_MAX];
    const char *path = vmaf_tiny_ai_resolve_model_path(
        "transnet_v2", s->model_path, "VMAF_TRANSNET_V2_MODEL_PATH", env_path, sizeof(env_path));
    if (!path) {
        return -EINVAL;
    }
    if (strstr(path, "placeholder") != NULL) {
        vmaf_log(VMAF_LOG_LEVEL_WARNING,
                 "transnet_v2: loading placeholder model '%s' (not for production use)\n", path);
    }
    rc = vmaf_tiny_ai_open_session("transnet_v2", path, &s->sess);
    if (rc < 0) {
        return rc;
    }
    assert(s->sess != NULL);

    s->w = w;
    s->h = h;
    s->n_read = 0u;
    s->next_emit = 0u;

    rc = allocate_buffers(s);
    if (rc < 0) {
        release_buffers(s);
        vmaf_dnn_session_close(s->sess);
        s->sess = NULL;
        return rc;
    }
    return 0;
}

/* Stack window `start` .. `start + 99` into the [1, 100, 3, 27, 48] input
 * tensor. Frames before the clip are its first frame and frames after the
 * last one read are that frame, the padding upstream adds; every frame the
 * window needs is still in the ring (see the header comment). */
static void gather_window(const TransNetV2State *s, int64_t start, float *dst)
{
    const int64_t last = (int64_t)s->n_read - 1;
    for (unsigned k = 0; k < TRANSNET_V2_WINDOW; ++k) {
        int64_t f = start + (int64_t)k;
        f = f < 0 ? 0 : (f > last ? last : f);
        const unsigned slot = (unsigned)(f % (int64_t)TRANSNET_V2_WINDOW);
        memcpy(dst + (size_t)k * TRANSNET_V2_FRAME_ELEMS, s->frames[slot],
               TRANSNET_V2_FRAME_ELEMS * sizeof(float));
    }
}

/* Write the probability and the flag of frame `index` from `logit`. */
static int emit_frame(VmafFeatureCollector *feature_collector, double logit, unsigned index)
{
    double prob;
    double flag;
    int rc = vmaf_transnet_v2_scores(logit, TRANSNET_V2_BOUNDARY_THRESHOLD, &prob, &flag);
    if (rc < 0) {
        vmaf_log(VMAF_LOG_LEVEL_WARNING,
                 "transnet_v2: non-finite logit at frame %u (logit=%g), failing frame\n", index,
                 logit);
        return rc;
    }
    rc = vmaf_feature_collector_append(feature_collector, "shot_boundary_probability", prob, index);
    if (rc < 0)
        return rc;
    return vmaf_feature_collector_append(feature_collector, "shot_boundary", flag, index);
}

/* Run the window that reports frames next_emit .. next_emit + 49 and write
 * those of them the clip has. The output is bound by position: the model
 * has one output, whatever the exporter named it. */
static int run_next_window(TransNetV2State *s, VmafFeatureCollector *feature_collector)
{
    gather_window(s, (int64_t)s->next_emit - (int64_t)TRANSNET_V2_CONTEXT, s->input_tensor);

    const int64_t in_shape[5] = {1, (int64_t)TRANSNET_V2_WINDOW, (int64_t)TRANSNET_V2_CHANNELS,
                                 (int64_t)TRANSNET_V2_HEIGHT, (int64_t)TRANSNET_V2_WIDTH};
    const VmafDnnInput inputs[1] = {
        {.name = "frames", .data = s->input_tensor, .shape = in_shape, .rank = 5},
    };
    VmafDnnOutput outputs[1] = {
        {.name = NULL,
         .data = s->output_logits,
         .capacity = (size_t)TRANSNET_V2_WINDOW,
         .written = 0u},
    };
    int rc = vmaf_dnn_session_run(s->sess, inputs, 1u, outputs, 1u);
    if (rc < 0)
        return rc;
    if (outputs[0].written != (size_t)TRANSNET_V2_WINDOW)
        return -EIO;

    for (unsigned j = 0; j < TRANSNET_V2_STRIDE && s->next_emit + j < s->n_read; ++j) {
        const double logit = (double)s->output_logits[TRANSNET_V2_CONTEXT + j];
        rc = emit_frame(feature_collector, logit, s->next_emit + j);
        if (rc < 0)
            return rc;
    }
    s->next_emit += TRANSNET_V2_STRIDE;
    return 0;
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
    /* The windows are kept by frame index; a skipped index would put a
     * frame in another frame's slot. */
    if (index != s->n_read) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "transnet_v2: frame %u read after frame %u; frames must "
                 "arrive in order from 0 without gaps\n",
                 index, s->n_read);
        return -EINVAL;
    }

    luma_to_thumbnail(ref_pic, s->frames[index % TRANSNET_V2_WINDOW]);
    s->n_read += 1u;

    /* The window starting at next_emit - 25 is complete once its last frame
     * (next_emit + 74) has been read. */
    if (s->n_read == s->next_emit + TRANSNET_V2_WINDOW - TRANSNET_V2_CONTEXT)
        return run_next_window(s, feature_collector);
    return 0;
}

/* Run the windows the end of the clip leaves open, with its last frame
 * repeated behind it. At most two remain: windows run as soon as their
 * last frame is read, so the first open one ends past the clip. */
static int transnet_v2_flush(VmafFeatureExtractor *fex, VmafFeatureCollector *feature_collector)
{
    TransNetV2State *s = fex->priv;
    if (!s || !s->sess)
        return 1;
    for (unsigned i = 0; i < 2u && s->next_emit < s->n_read; ++i) {
        const int rc = run_next_window(s, feature_collector);
        if (rc < 0)
            return rc;
    }
    return s->next_emit < s->n_read ? -EIO : 1;
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
                         "(input 'frames' [1, 100, 3, 27, 48], one output [1, 100] of logits). "
                         "Overrides the VMAF_TRANSNET_V2_MODEL_PATH env var."),
    {0},
};

static const char *transnet_v2_provided_features[] = {"shot_boundary_probability", "shot_boundary",
                                                      NULL};

// NOLINTNEXTLINE(misc-use-internal-linkage): cross-TU registry pattern — external linkage required; referenced as `extern VmafFeatureExtractor vmaf_fex_transnet_v2` by feature_extractor.cpp's feature_extractor_list[] (ADR-0278).
VmafFeatureExtractor vmaf_fex_transnet_v2 = {
    .name = "transnet_v2",
    .init = transnet_v2_init,
    .extract = transnet_v2_extract,
    .flush = transnet_v2_flush,
    .close = transnet_v2_close,
    .options = transnet_v2_options,
    .priv_size = sizeof(TransNetV2State),
    .provided_features = transnet_v2_provided_features,
    /* Windows are kept by frame index: every frame, in order, on the
     * calling thread (ADR-1527). */
    .flags = VMAF_FEATURE_EXTRACTOR_TEMPORAL,
    /* Explicit zero-init so GCC LTO sees the full struct layout
     * across TUs (silences -Wlto-type-mismatch; ADR-0181 precedent
     * shared with feature_lpips.c). */
    .chars = {0},
};

/* NOLINTEND(modernize-use-nullptr) */
