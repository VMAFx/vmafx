<!-- markdownlint-disable MD060 -->
# Tiny-AI extractor template

This page is the recipe for adding a new tiny-AI feature extractor to
`core/src/feature/`. It pairs with [ADR-0250](../adr/0250-tiny-ai-extractor-template.md)
and the shared scaffolding header
[`core/src/dnn/tiny_extractor_template.h`](../../core/src/dnn/tiny_extractor_template.h).

## Why a template

Every tiny-AI extractor opens a `VmafDnnSession`, resolves an ONNX path
via a `model_path` feature option with an env-var fallback, and (when the
model is colour-sensitive) converts BT.709 limited-range YUV → RGB with
nearest-neighbour chroma upsampling. Without the template each new
extractor copies ~70 LOC of identical plumbing. With the template a new
single-frame extractor is ~30 LOC of model-specific tensor wiring.

## What the template ships

Three `static inline` helpers + one struct-literal-emitting macro,
documented inline in
[`tiny_extractor_template.h`](../../core/src/dnn/tiny_extractor_template.h):

| Symbol | Purpose |
|---|---|
| `vmaf_tiny_ai_resolve_model_path(name, option, env_var)` | Feature-option-then-env-var lookup. Returns NULL with a single user-facing log line when neither is set. |
| `vmaf_tiny_ai_open_session(name, path, &out)` | `vmaf_dnn_session_open` wrapper with the standard `<name>: vmaf_dnn_session_open(<path>) failed: <rc>` log line on error. |
| `vmaf_tiny_ai_yuv8_to_rgb8_planes(pic, dst_r, dst_g, dst_b)` | BT.709 limited-range 8-bit YUV → RGB with nearest-neighbour chroma upsample. Bit-exact with the per-extractor copies it replaces. |
| `vmaf_tiny_ai_yuv_to_rgb8_planes(pic, dst_r, dst_g, dst_b)` | 8/10/12/16-bit planar YUV → RGB8 wrapper for ImageNet-family models that keep an RGB8 tensor ABI. |
| `VMAF_TINY_AI_MODEL_PATH_OPTION(state_t, help)` | Emits the standard `model_path` row of a per-extractor `VmafOption[]` table. |

The `init` / `extract` / `close` lifecycle stays per-extractor — model
shapes, ring buffers, output names, and emitted score names differ
enough that a generic lifecycle macro costs more than it saves
(rationale in ADR-0250's `## Alternatives considered`).

## Recipe — single-frame extractor (LPIPS / MobileSal shape)

The shortest case: one input frame in, one scalar feature out.

```c
/**
 *  Copyright 2026 <author>
 *  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
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

typedef struct MyExtractorState {
    char *model_path; /* feature option, owned by opt.c */
    VmafDnnSession *sess;
    unsigned w, h;
    uint8_t *rgb8[3];   /* per-channel uint8 RGB scratch */
    float *tensor_in;   /* 3 * w * h floats, NCHW */
    float *out_buf;     /* model output buffer */
} MyExtractorState;

static void my_release(MyExtractorState *s)
{
    if (!s)
        return;
    for (int i = 0; i < 3; ++i) {
        if (s->rgb8[i]) {
            aligned_free(s->rgb8[i]);
            s->rgb8[i] = NULL;
        }
    }
    if (s->tensor_in) {
        aligned_free(s->tensor_in);
        s->tensor_in = NULL;
    }
    if (s->out_buf) {
        aligned_free(s->out_buf);
        s->out_buf = NULL;
    }
    if (s->sess) {
        (void)vmaf_dnn_session_close(s->sess);
        s->sess = NULL;
    }
}

static int my_init(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                   unsigned w, unsigned h)
{
    MyExtractorState *s = fex->priv;
    if (pix_fmt == VMAF_PIX_FMT_YUV400P || bpc != 8)
        return -ENOTSUP;

    const char *path =
        vmaf_tiny_ai_resolve_model_path("my_extractor", s->model_path, "VMAF_MY_MODEL_PATH");
    if (!path)
        return -EINVAL;

    int rc = vmaf_tiny_ai_open_session("my_extractor", path, &s->sess);
    if (rc < 0)
        return rc;

    s->w = w;
    s->h = h;
    const size_t plane = (size_t)w * (size_t)h;
    for (int i = 0; i < 3; ++i) {
        s->rgb8[i] = (uint8_t *)aligned_malloc(plane, 32);
        if (!s->rgb8[i])
            goto oom;
    }
    s->tensor_in = (float *)aligned_malloc(3u * plane * sizeof(float), 32);
    s->out_buf = (float *)aligned_malloc(plane * sizeof(float), 32);
    if (!s->tensor_in || !s->out_buf)
        goto oom;
    return 0;
oom:
    my_release(s);
    return -ENOMEM;
}

static int my_extract(VmafFeatureExtractor *fex, VmafPicture *ref, VmafPicture *ref90,
                      VmafPicture *dist, VmafPicture *dist90, unsigned index,
                      VmafFeatureCollector *fc)
{
    (void)ref90;
    (void)dist90;
    MyExtractorState *s = fex->priv;
    int rc = vmaf_tiny_ai_yuv_to_rgb8_planes(dist, s->rgb8[0], s->rgb8[1], s->rgb8[2]);
    if (rc < 0)
        return rc;
    rc = vmaf_tensor_from_rgb_imagenet(s->rgb8[0], s->w, s->rgb8[1], s->w, s->rgb8[2], s->w,
                                       (int)s->w, (int)s->h, s->tensor_in);
    if (rc < 0)
        return rc;
    const int64_t shape[4] = {1, 3, (int64_t)s->h, (int64_t)s->w};
    const size_t plane = (size_t)s->w * (size_t)s->h;
    VmafDnnInput inputs[1] = {
        {.name = "input", .data = s->tensor_in, .shape = shape, .rank = 4u},
    };
    VmafDnnOutput outputs[1] = {
        {.name = "score", .data = s->out_buf, .capacity = plane, .written = 0u},
    };
    rc = vmaf_dnn_session_run(s->sess, inputs, 1u, outputs, 1u);
    if (rc < 0)
        return rc;
    /* derive scalar feature from outputs[0].data + outputs[0].written ... */
    return vmaf_feature_collector_append(fc, "my_score", /* derived */ 0.0, index);
}

static int my_close(VmafFeatureExtractor *fex)
{
    MyExtractorState *s = fex->priv;
    if (!s)
        return 0;
    my_release(s);
    memset(s, 0, sizeof(*s));
    return 0;
}

static const VmafOption my_options[] = {
    VMAF_TINY_AI_MODEL_PATH_OPTION(MyExtractorState,
        "Filesystem path to the my-extractor ONNX model. "
        "Overrides the VMAF_MY_MODEL_PATH env var."),
    {NULL},
};

static const char *my_provided[] = {"my_score", NULL};

VmafFeatureExtractor vmaf_fex_my = {
    .name = "my_extractor",
    .init = my_init,
    .extract = my_extract,
    .close = my_close,
    .options = my_options,
    .priv_size = sizeof(MyExtractorState),
    .provided_features = my_provided,
    .chars = {0},
};
```

That's ~150 LOC, but the boilerplate (release helper + path/session
plumbing + option-table macro) is ~30 LOC; the rest is your model-
specific tensor wiring. Compare to the pre-template baseline of
~300 LOC for `feature_lpips.c`.

## Recipe variants

| Variant | Example | Extra wiring |
|---|---|---|
| **Single-frame, distorted-only** | `feature_mobilesal.c` (PR #208) | As above. |
| **Single-frame, full-reference** | `feature_lpips.c` | Add a second RGB scratch + tensor for `ref`; bind two `VmafDnnInput`s named `"ref"` and `"dist"`. |
| **Sliding window (small N)** | `fastdvdnet_pre.c` (5 frames) | Add a ring buffer of N planes + `next_slot` + `n_buffered`; gather into a `[1, N, H, W]` input tensor; replicate-edge clamp at clip boundaries. |
| **Sliding window (large N)** | [`transnet_v2.c`](../../core/src/feature/transnet_v2.c) (100 frames) | Same shape as above with bigger N. See [Large sliding windows](#large-sliding-windows-transnet_v2) below. |

Each variant's lifecycle is hand-written — the template stays out of
the way of the per-frame data shape.

### Large sliding windows (`transnet_v2`)

The large-N row used to read "planned `feature_transnet_v2.c`". The extractor
shipped — as [`core/src/feature/transnet_v2.c`](../../core/src/feature/transnet_v2.c),
not `feature_transnet_v2.c` — and is registered in
[`core/src/meson.build`](../../core/src/meson.build) and documented in
[`docs/metrics/features.md`](../../docs/metrics/features.md#transnetv2--transnet-v2-shot-boundary-detector-tiny-ai-nr--single-input).
What it actually does, for anyone copying the recipe:

- **Window**: a 100-slot ring buffer of 27x48 luma thumbnails. The input
  tensor is `[1, 100, 3, 27, 48]` (`"frames"`); the output is `[1, 100]`
  per-frame logits (`"boundary_logits"`). The 27x48 / 100-frame geometry is
  the published Soucek & Lokoc 2020 architecture, not a fork choice.
- **No decimation.** The "consider strided/decimated submission" note was a
  suggestion, and the shipped extractor does not take it: the network runs
  **once per `extract()` call**, every frame, and only the slot for the most
  recent push is read out. That is the dominant per-frame cost of the feature
  and is why `transnet_v2` is not in any default model.
- **Warm-up, not padding.** Until the ring holds 100 frames the most recent
  frame is replicated across the empty slots, so the tensor is always
  well-formed. The reported probability is therefore delayed-onset —
  treat roughly the first 50 frames as warm-up rather than as scores.
- **Per-shot aggregation is not shipped.** The extractor emits per-frame
  `shot_boundary_probability` (sigmoid of the logit) and a `shot_boundary`
  flag thresholded at 0.5. Turning those into shot intervals and a per-shot
  CRF target is backlog item T6-3b and does not exist in the tree.
- **Disabled-DNN builds** return `-ENOSYS` from `init()` before any model-path
  probing, per the shared optional-runtime contract.

## Do / don't

- **Do** call `vmaf_tiny_ai_require_runtime` after pixel-format / bit-depth
  validation and before model-path probing. Disabled-DNN builds must report
  `-ENOSYS` from the shared optional-runtime contract, not a misleading
  missing-model error.
- **Do** call `vmaf_tiny_ai_resolve_model_path` even if your extractor
  only honours an env var; it logs the standard error message on
  failure so users get one consistent diagnostic.
- **Do** route session opening through `vmaf_tiny_ai_open_session` so
  the failure log line is uniform and `vmaf_dnn_validate_onnx` is run
  before `CreateSession` (the public API performs this; the helper
  doesn't bypass it).
- **Do** ship a smoke unit test under `core/test/test_<name>.c`
  mirroring `test_lpips.c` — exercises registration + options-table
  contract.
- **Don't** add `getenv` calls outside `vmaf_tiny_ai_resolve_model_path`
  unless the value is genuinely orthogonal (e.g. a per-test
  smoke-toggle); the resolver is the canonical lookup.
- **Don't** open-code BT.709 YUV→RGB; the helper is bit-exact with the
  shared `ciede.c` convention and any drift breaks comparison numbers
  across extractors.
- **Don't** introduce additional macros around the lifecycle. ADR-0221
  spells out why we chose helpers + a single option-table macro over a
  full-lifecycle macro framework.

## Documentation bar

Per [ADR-0042](../adr/0042-tinyai-docs-required-per-pr.md) every
tiny-AI PR ships docs in the same PR. Minimum:

1. **Model card** under `docs/ai/models/<name>.md` — input/output
   contract, weights provenance, licence.
2. **Roadmap row** in `docs/ai/roadmap.md` — flip from planned to
   shipped.
3. **Registry entry** in `model/tiny/registry.json` — sha256, license,
   `smoke: true` for placeholder weights.
4. **Unit test** in `core/test/test_<name>.c`.
5. **ADR** under `docs/adr/` — design + alternatives.

The template doesn't change the documentation bar — it just shrinks
the C-side footprint so authors spend their effort on (1) and (5).
