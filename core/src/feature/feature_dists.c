/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  DISTS (Deep Image Structure and Texture Similarity) full-reference
 *  extractor. Backed by a two-input ONNX model that accepts
 *  ImageNet-normalised RGB planes and emits a scalar distance:
 *
 *    YUV420P/422P/444P 8/10/12/16-bit
 *       -> upsample chroma to 4:4:4
 *       -> normalise high-bit-depth samples to the 8-bit domain
 *       -> BT.709 limited-range YUV->RGB (float) -> round to uint8 planes
 *       -> vmaf_tensor_from_rgb_imagenet()  (ref and dist)
 *       -> vmaf_dnn_session_run() with named bindings "ref" / "dist"
 *       -> scalar "dists_sq" feature
 *
 *  Model resolution mirrors LPIPS: the feature option ``model_path`` wins;
 *  otherwise ``VMAF_DISTS_SQ_MODEL_PATH`` is consulted. Missing model means
 *  init() returns -EINVAL so callers never run on accidental dummy weights.
 *  Disabled-DNN builds return -ENOSYS before model-path probing, matching
 *  the tiny-AI optional-runtime contract shared with LPIPS / FastDVDnet.
 */

#include <assert.h>
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

#include "dnn/tensor_io.h"
#include "dnn/tiny_extractor_template.h"

typedef struct DistsSqState {
    char *model_path; /**< feature option, owned by opt.c */
    VmafDnnSession *sess;
    unsigned w, h;
    uint8_t *rgb8_ref[3];
    uint8_t *rgb8_dist[3];
    float *tensor_ref;  /**< 3 * w * h floats, NCHW */
    float *tensor_dist; /**< 3 * w * h floats, NCHW */
} DistsSqState;

static void dists_sq_release(DistsSqState *s)
{
    if (!s)
        return;
    for (int i = 0; i < 3; ++i) {
        if (s->rgb8_ref[i]) {
            aligned_free(s->rgb8_ref[i]);
            s->rgb8_ref[i] = NULL;
        }
        if (s->rgb8_dist[i]) {
            aligned_free(s->rgb8_dist[i]);
            s->rgb8_dist[i] = NULL;
        }
    }
    if (s->tensor_ref) {
        aligned_free(s->tensor_ref);
        s->tensor_ref = NULL;
    }
    if (s->tensor_dist) {
        aligned_free(s->tensor_dist);
        s->tensor_dist = NULL;
    }
    if (s->sess) {
        (void)vmaf_dnn_session_close(s->sess);
        s->sess = NULL;
    }
}

static int dists_sq_init(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                         unsigned w, unsigned h)
{
    DistsSqState *s = fex->priv;

    if (pix_fmt == VMAF_PIX_FMT_YUV400P)
        return -EINVAL;
    if (bpc != 8 && bpc != 10 && bpc != 12 && bpc != 16) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "dists_sq: bpc=%u not supported\n", bpc);
        return -ENOTSUP;
    }

    int rc = vmaf_tiny_ai_require_runtime("dists_sq");
    if (rc < 0)
        return rc;

    const char *path =
        vmaf_tiny_ai_resolve_model_path("dists_sq", s->model_path, "VMAF_DISTS_SQ_MODEL_PATH");
    if (!path)
        return -EINVAL;

    rc = vmaf_tiny_ai_open_session("dists_sq", path, &s->sess);
    if (rc < 0)
        return rc;
    assert(s->sess != NULL);

    s->w = w;
    s->h = h;
    const size_t plane = (size_t)w * (size_t)h;

    for (int i = 0; i < 3; ++i) {
        s->rgb8_ref[i] = (uint8_t *)aligned_malloc(plane, 32);
        s->rgb8_dist[i] = (uint8_t *)aligned_malloc(plane, 32);
        if (!s->rgb8_ref[i] || !s->rgb8_dist[i])
            goto oom;
    }
    s->tensor_ref = (float *)aligned_malloc(3u * plane * sizeof(float), 32);
    s->tensor_dist = (float *)aligned_malloc(3u * plane * sizeof(float), 32);
    if (!s->tensor_ref || !s->tensor_dist)
        goto oom;

    return 0;

oom:
    dists_sq_release(s);
    return -ENOMEM;
}

static int dists_sq_extract(VmafFeatureExtractor *fex, VmafPicture *ref_pic,
                            VmafPicture *ref_pic_90, VmafPicture *dist_pic,
                            VmafPicture *dist_pic_90, unsigned index,
                            VmafFeatureCollector *feature_collector)
{
    (void)ref_pic_90;
    (void)dist_pic_90;
    DistsSqState *s = fex->priv;
    assert(s != NULL);
    assert(s->sess != NULL);

    if (ref_pic->w[0] != s->w || ref_pic->h[0] != s->h)
        return -ERANGE;
    if (dist_pic->w[0] != s->w || dist_pic->h[0] != s->h)
        return -ERANGE;

    int rc =
        vmaf_tiny_ai_yuv_to_rgb8_planes(ref_pic, s->rgb8_ref[0], s->rgb8_ref[1], s->rgb8_ref[2]);
    if (rc < 0)
        return rc;
    rc = vmaf_tiny_ai_yuv_to_rgb8_planes(dist_pic, s->rgb8_dist[0], s->rgb8_dist[1],
                                         s->rgb8_dist[2]);
    if (rc < 0)
        return rc;

    rc = vmaf_tensor_from_rgb_imagenet(s->rgb8_ref[0], s->w, s->rgb8_ref[1], s->w, s->rgb8_ref[2],
                                       s->w, (int)s->w, (int)s->h, s->tensor_ref);
    if (rc < 0)
        return rc;
    rc = vmaf_tensor_from_rgb_imagenet(s->rgb8_dist[0], s->w, s->rgb8_dist[1], s->w,
                                       s->rgb8_dist[2], s->w, (int)s->w, (int)s->h, s->tensor_dist);
    if (rc < 0)
        return rc;

    const int64_t shape[4] = {1, 3, (int64_t)s->h, (int64_t)s->w};
    VmafDnnInput inputs[2] = {
        {.name = "ref", .data = s->tensor_ref, .shape = shape, .rank = 4},
        {.name = "dist", .data = s->tensor_dist, .shape = shape, .rank = 4},
    };
    float score_buf[1] = {0.0f};
    VmafDnnOutput outputs[1] = {
        {.name = "score", .data = score_buf, .capacity = 1, .written = 0},
    };

    rc = vmaf_dnn_session_run(s->sess, inputs, 2u, outputs, 1u);
    if (rc < 0)
        return rc;
    if (outputs[0].written == 0u)
        return -EIO;

    return vmaf_feature_collector_append(feature_collector, "dists_sq", (double)score_buf[0],
                                         index);
}

static int dists_sq_close(VmafFeatureExtractor *fex)
{
    DistsSqState *s = fex->priv;
    if (!s)
        return 0;
    dists_sq_release(s);
    memset(s, 0, sizeof(*s));
    return 0;
}

static const VmafOption dists_sq_options[] = {
    VMAF_TINY_AI_MODEL_PATH_OPTION(
        DistsSqState, "Filesystem path to the DISTS-Sq ONNX model (two-input, 'ref'/'dist'). "
                      "Overrides the VMAF_DISTS_SQ_MODEL_PATH env var."),
    {0},
};

static const char *dists_sq_provided_features[] = {"dists_sq", NULL};

VmafFeatureExtractor vmaf_fex_dists_sq = {
    .name = "dists_sq",
    .init = dists_sq_init,
    .extract = dists_sq_extract,
    .close = dists_sq_close,
    .options = dists_sq_options,
    .priv_size = sizeof(DistsSqState),
    .provided_features = dists_sq_provided_features,
    .chars = {0},
};
