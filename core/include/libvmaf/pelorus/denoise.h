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
 * VENDORED FROM VMAFx/pelorus@818d844 — DO NOT EDIT. Append-only ABI; single
 * source of truth is pelorus. Re-sync via scripts/sync-pelorus-interop.sh.
 * See docs/adr/1113-vendor-pelorus-interop-abi.md.
 *
 * Local edit vs the pelorus original: the intra-pelorus #include below is
 * rewritten from "pelorus/pelorus.h" to "libvmaf/pelorus/pelorus.h" so it resolves
 * under core/include/. Nothing else is changed.
 */

/*
 * denoise.h — parameter contract for the Pelorus temporal denoise filter.
 *
 * Shared by the FFmpeg vf_pelorus_denoise_vulkan filter (which mirrors these
 * fields into its compute-shader push constants) and the vmafx autotune loop
 * (which sweeps `sigma` against encoded-VMAF-at-bitrate as the oracle, ADR-0106).
 * The wire form of the push constants lives in shaders/pelorus_denoise.comp;
 * this struct is the host-side, AVOption-addressable view of the same params.
 *
 * Algorithm (PEL_DENOISER_BILATERAL_TEMPORAL): an edge-preserving
 * spatio-temporal denoiser in a single Vulkan compute pass over a CAUSAL window
 * [cur, prev_1 .. prev_n]. The spatial term is an NLM-lite joint bilateral whose
 * range weight is a small-patch SSD (a real edge has high patch SSD, driving the
 * weight to zero, so it refuses to average across edges). The temporal term
 * averages the same-coordinate sample of each previous frame, gated per pixel by
 * a similarity threshold (a delta above `temporal_cut` breaks the walk so motion
 * / scene-cuts cannot ghost) and decayed by `temporal_decay` per frame. The two
 * are blended (`blend`) and applied dry/wet (`strength`). No motion compensation
 * in v0.x — same-coordinate taps only; MC is a later filter (PEL_DENOISE_FLAG_
 * MOTION_COMP is reserved, off). Thresholds are NORMALIZED in [0,1], independent
 * of bit depth (the shader works in a normalized domain). Per-plane order is
 * {Y, Cb, Cr, A}. With meta=1 the filter emits PEL_SEC_DENOISE residual stats.
 * See docs/metrics/denoise.md and ADR-0111 (benchmark methodology proving it).
 */
#ifndef PELORUS_DENOISE_H
#define PELORUS_DENOISE_H

#include <stdint.h>

#include "libvmaf/pelorus/pelorus.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Which Pelorus denoiser ran — echoed into PelorusDenoiseSection.denoiser_id. */
enum pel_denoiser_id {
    PEL_DENOISER_NONE = 0,
    PEL_DENOISER_BILATERAL_TEMPORAL = 1 /* edge-preserving spatio-temporal NLM-lite */
};

/* Flag bits, mirrored 1:1 into the shader push-constant `flags` word. */
enum pel_denoise_flags {
    PEL_DENOISE_FLAG_TEMPORAL = 1u << 0,       /* use the temporal term         */
    PEL_DENOISE_FLAG_MOTION_COMP = 1u << 1,    /* reserved (MC is a later filter)*/
    PEL_DENOISE_FLAG_PROTECT_DETAIL = 1u << 2, /* damp strength on texture/edges */
    PEL_DENOISE_FLAG_AUTO_SIGMA = 1u << 3      /* reserved (variance-fed sigma)  */
};

/*
 * Denoise parameters. Sigmas/cuts are NORMALIZED in [0,1] relative to full
 * range, independent of bit depth. Per-plane arrays are {Y, Cb, Cr, A}.
 * Defaults are the conservative param-contract preset (so the vmafx autotune
 * loop sweeps strength/sigma UP from a safe floor); see pel_denoise_params_default.
 */
typedef struct PelorusDenoiseParams {
    float sigma_s[4];     /* per-plane spatial range sigma (edge sensitivity)   */
    float sigma_t[4];     /* per-plane temporal gate bandwidth                  */
    float strength[4];    /* per-plane dry/wet mix: out = mix(in, filtered, s)  */
    float blend;          /* spatial<->temporal blend (0 = spatial, 1 = temporal)*/
    float temporal_decay; /* per-frame trust falloff (older frames weigh less)  */
    float temporal_cut;   /* per-pixel scene-cut/fast-motion clamp (normalized) */
    int32_t patch_radius; /* spatial window radius in pixels (0 = temporal-only)*/
    int32_t n_prev;       /* temporal depth — previous frames held in VRAM      */
    int32_t denoiser_id;  /* enum pel_denoiser_id                               */
    uint32_t planes;      /* bitmask of planes to process (default 0xF)         */
    uint32_t flags;       /* enum pel_denoise_flags                             */
} PelorusDenoiseParams;

/* Maximum temporal depth the filter holds (cur + PEL_DENOISE_MAX_PREV prev). */
#define PEL_DENOISE_MAX_PREV 4

/* Fill p with the conservative pre-encode defaults (sigma 0.03/0.04, strength
 * 0.30/0.20, 3-frame causal temporal window, detail protection on). */
void pel_denoise_params_default(PelorusDenoiseParams *p);

/* Validate p against documented ranges. Returns PEL_OK or PEL_ERR_RANGE; on
 * error and if `what` is non-NULL, *what points at a static field name. */
pel_result pel_denoise_params_validate(const PelorusDenoiseParams *p, const char **what);

#ifdef __cplusplus
}
#endif
#endif /* PELORUS_DENOISE_H */
