/**
 *
 *  Copyright 2026 Lusoris
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
 * perceptual_weight.h (internal) — store + weight derivation for Pelorus-driven
 * perceptual spatial-pooling weighting. The public surface lives in
 * libvmaf/perceptual_weight.h; this header is the libvmaf-internal contract
 * between libvmaf.c (which owns the store inside VmafContext) and
 * perceptual_weight.c (which parses blobs and derives weights).
 *
 * GOLDEN-GATE ISOLATION: vmaf_perceptual_weight_at_index returns exactly 1.0
 * for any frame that has no stored summary, and the pooling caller takes its
 * byte-identical legacy path whenever the store is disabled. See ADR-1118.
 */
#ifndef LIBVMAF_FEATURE_PERCEPTUAL_WEIGHT_H
#define LIBVMAF_FEATURE_PERCEPTUAL_WEIGHT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Per-frame derived salience summary. One slot per picture index that carried
 * a valid Pelorus blob. `present` distinguishes "no side-data for this index"
 * (weight 1.0) from a stored zero-salience summary (also weight ~1.0 but a
 * deliberate producer statement). */
typedef struct VmafPerceptualFrameSummary {
    bool present;   /* a valid summary was ingested for this index           */
    float salience; /* normalized [0,1] banding/variance activity for frame  */
} VmafPerceptualFrameSummary;

/* The store hangs off VmafContext. Zero-initialised state is "disabled, empty,
 * default strength applied lazily" — a freshly memset VmafContext is inert. */
typedef struct VmafPerceptualWeightStore {
    bool enabled;                          /* opt-in master switch (default OFF) */
    bool strength_set;                     /* false => use default 1.0           */
    double strength;                       /* per-frame weight = 1 + strength*s  */
    VmafPerceptualFrameSummary *summaries; /* indexed by picture index           */
    size_t capacity;                       /* number of allocated slots          */
} VmafPerceptualWeightStore;

/* Release the summary array. Safe on a zero-initialised store (no-op). */
void vmaf_perceptual_weight_store_destroy(VmafPerceptualWeightStore *store);

/*
 * Parse a UUID-prefixed Pelorus blob, derive the frame salience, and store it
 * at `pic_index`. Mirrors the public vmaf_set_perceptual_sidedata contract:
 *   0        success
 *   -EINVAL  store or blob NULL
 *   -ENOENT  not a Pelorus blob (cleanly ignored)
 *   -EPROTO  ABI-major mismatch (cleanly ignored; caller logs)
 *   -ENOMEM  allocation failure
 * Never reads out of bounds; tolerates absent sections and grid==0.
 */
int vmaf_perceptual_weight_ingest(VmafPerceptualWeightStore *store, const uint8_t *blob, size_t len,
                                  unsigned pic_index);

/*
 * Per-frame pooling weight for `pic_index`. Returns exactly 1.0 when the store
 * is disabled, when no summary was stored for the index, or when the derived
 * weight would be non-finite. Otherwise returns 1 + strength * salience,
 * clamped to a finite, positive value. This is the ONLY function the pooling
 * path calls; its 1.0 default is the golden-gate guarantee.
 */
double vmaf_perceptual_weight_at_index(const VmafPerceptualWeightStore *store, unsigned pic_index);

/* True when weighting is enabled AND at least one summary is present, i.e. the
 * pooling path may diverge from its legacy arithmetic. When false the caller
 * MUST take the byte-identical legacy path. */
bool vmaf_perceptual_weight_active(const VmafPerceptualWeightStore *store);

#ifdef __cplusplus
}
#endif

#endif /* LIBVMAF_FEATURE_PERCEPTUAL_WEIGHT_H */
