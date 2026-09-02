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
 * perceptual_weight.h — public C API for Pelorus-driven perceptual
 * spatial-pooling weighting (integration plan workstream B1).
 *
 * A producer in the Pelorus GPU pre-encode pipeline (vf_pelorus_deband /
 * vf_pelorus_analyze) attaches a per-frame side-data blob to each distorted
 * AVFrame as AV_FRAME_DATA_SEI_UNREGISTERED (see libvmaf/pelorus/interop.h).
 * The blob carries per-cell banding-risk and variance maps. When perceptual
 * weighting is enabled, libvmaf uses those maps to re-weight VMAF's pooling:
 * frames whose spatial regions carry high banding risk count MORE toward the
 * pooled score, so a pooled VMAF tracks the perceptually salient distortions a
 * deband-aware encoder cares about.
 *
 * GOLDEN-GATE ISOLATION (normative — see ADR-1118 and AGENTS.md):
 *   The weighting is INERT unless BOTH conditions hold for a frame:
 *     (a) perceptual weighting is enabled via vmaf_set_perceptual_weight_enabled
 *         (default OFF), AND
 *     (b) a valid Pelorus side-data blob was registered for that frame index
 *         via vmaf_set_perceptual_sidedata.
 *   When either is false the per-frame weight is exactly 1.0 and the pooling
 *   arithmetic is byte-identical to a build without this feature. The Netflix
 *   golden reference pairs carry NO Pelorus side-data, so they score
 *   bit-exact. Do NOT change that invariant.
 */
#ifndef LIBVMAF_PERCEPTUAL_WEIGHT_H
#define LIBVMAF_PERCEPTUAL_WEIGHT_H

#include <stddef.h>
#include <stdint.h>

#include "libvmaf/libvmaf.h"
#include "libvmaf/macros.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Enable or disable Pelorus-driven perceptual spatial-pooling weighting.
 *
 * Disabled by default. While disabled, every registered side-data blob is
 * retained but never consulted, and pooling is byte-identical to upstream
 * VMAF. Toggling at runtime is permitted; it affects subsequent pooling
 * calls only (no already-computed per-frame VMAF scores are mutated).
 *
 * @param vmaf     The VMAF context allocated with `vmaf_init()`.
 * @param enabled  Non-zero to enable weighting, zero to disable.
 *
 * @return 0 on success, or -EINVAL when @vmaf is NULL.
 *
 * @thread-safety Not thread-safe. Use one VmafContext per thread.
 */
VMAF_EXPORT int vmaf_set_perceptual_weight_enabled(VmafContext *vmaf, int enabled);

/**
 * Set the perceptual-weighting strength.
 *
 * The per-frame weight is `1 + strength * salience`, where `salience` is a
 * normalized [0, 1] banding/variance-derived activity score for the frame.
 * `strength` of 0 makes the weighting a no-op even when enabled; the default
 * is 1.0. Negative values are rejected.
 *
 * @param vmaf      The VMAF context allocated with `vmaf_init()`.
 * @param strength  Non-negative weighting strength (default 1.0).
 *
 * @return 0 on success, -EINVAL when @vmaf is NULL or @strength is negative
 *         or not finite.
 *
 * @thread-safety Not thread-safe. Use one VmafContext per thread.
 */
VMAF_EXPORT int vmaf_set_perceptual_weight_strength(VmafContext *vmaf, double strength);

/**
 * Register a Pelorus side-data blob for a picture index.
 *
 * Parses the UUID-prefixed blob via the vendored Pelorus interop ABI
 * (libvmaf/pelorus/interop.h), extracts the banding/variance summaries plus
 * per-cell maps, and stashes a frame-level salience summary keyed by
 * @p pic_index. The blob itself is not retained — only the derived summary is
 * copied — so the caller may free @p blob immediately after this call.
 *
 * The blob is the full AV_FRAME_DATA_SEI_UNREGISTERED payload (16-byte UUID
 * prefix + flat PelorusSideData image). A buffer that is not a Pelorus blob
 * (wrong UUID/magic) is rejected with -ENOENT and leaves the frame unweighted.
 * An ABI-major mismatch is rejected with -EPROTO and logged; that frame stays
 * unweighted (graceful degrade, never a crash — forward/back-compat rules
 * R1-R6 in interop.h).
 *
 * A blob whose grid is empty (grid_cols == 0 || grid_rows == 0, e.g. today's
 * deband placeholder that carries only frame-level scalars) degrades to a
 * frame-level-scalar salience instead of a per-cell mean — never an error.
 *
 * This function is a no-op store with no effect on scoring unless perceptual
 * weighting is also enabled via vmaf_set_perceptual_weight_enabled.
 *
 * @param vmaf       The VMAF context allocated with `vmaf_init()`.
 * @param blob       The side-data payload (UUID-prefixed Pelorus blob).
 * @param len        Length of @p blob in bytes.
 * @param pic_index  Picture index this side-data describes.
 *
 * @return 0 on success (summary stored),
 *         -ENOENT when @p blob is not a Pelorus blob (cleanly ignored),
 *         -EPROTO on an ABI-major mismatch (cleanly ignored, logged),
 *         -EINVAL when @vmaf or @p blob is NULL,
 *         -ENOMEM on allocation failure.
 *
 * @thread-safety Not thread-safe. Use one VmafContext per thread.
 */
VMAF_EXPORT int vmaf_set_perceptual_sidedata(VmafContext *vmaf, const uint8_t *blob, size_t len,
                                             unsigned pic_index);

#ifdef __cplusplus
}
#endif

#endif /* LIBVMAF_PERCEPTUAL_WEIGHT_H */
