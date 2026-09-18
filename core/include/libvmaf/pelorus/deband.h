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
 * deband.h — parameter contract for the Pelorus smart-deband filter.
 *
 * Shared by the FFmpeg vf_pelorus_deband_vulkan filter (which mirrors these
 * fields into its compute-shader push constants) and the vmafx autotune loop
 * (which sweeps deband strength against VMAF as the oracle). The wire form of
 * the push constants lives in shaders/pelorus_deband.comp; this struct is the
 * host-side, AVOption-addressable view of the same parameters.
 *
 * Algorithm: a psychovisual debander modeled on flash3kyuu_deband (f3kdb) with
 * the core flat-test taken from FFmpeg's vf_deband.c (4-tap rotated reference
 * sampling, per-plane threshold, average-or-keep), extended with TPDF/blue-noise
 * grain injection, a local-variance detail-protection mask, and 16-bit-internal
 * dither-down. See docs/metrics/deband.md and docs/research/0101-smart-deband.md.
 */
#ifndef PELORUS_DEBAND_H
#define PELORUS_DEBAND_H

#include <stdint.h>

#include "libvmaf/pelorus/pelorus.h"

#ifdef __cplusplus
extern "C" {
#endif

enum pel_deband_sample_mode {
    PEL_DEBAND_SAMPLE_COLUMN = 1,    /* 2 vertical taps                          */
    PEL_DEBAND_SAMPLE_SQUARE = 2,    /* 4 rotated taps (== vf_deband; DEFAULT)   */
    PEL_DEBAND_SAMPLE_ROW = 3,       /* 2 horizontal taps                        */
    PEL_DEBAND_SAMPLE_SQUARE_ROT = 4 /* 4 taps + per-frame ring rotation       */
};

enum pel_deband_blur_mode {
    PEL_DEBAND_BLUR_AVERAGE = 0, /* flat iff |center-avg| < thr (vf_deband=1) */
    PEL_DEBAND_BLUR_ALLREFS = 1  /* flat iff every tap within thr (vf_deband=0)*/
};

enum pel_deband_dither_mode {
    PEL_DEBAND_DITHER_NONE = 0,
    PEL_DEBAND_DITHER_BAYER8 = 1,   /* ordered 8x8 Bayer                     */
    PEL_DEBAND_DITHER_BLUENOISE = 2 /* hashed TPDF (DEFAULT)                 */
};

/* Flag bits, mirrored 1:1 into the shader push-constant `flags` word. */
enum pel_deband_flags {
    PEL_DEBAND_FLAG_DYNAMIC_GRAIN = 1u << 0,  /* re-seed grain each frame      */
    PEL_DEBAND_FLAG_PROTECT_DETAIL = 1u << 1, /* gate off textured regions     */
    PEL_DEBAND_FLAG_COUPLING = 1u << 2        /* all planes must agree (4:4:4)  */
};

/*
 * Deband parameters. Thresholds and grain amplitudes are NORMALIZED in [0,1]
 * relative to full range, independent of bit depth (the shader works in a
 * 16-bit internal domain). Per-plane order is {Y, Cb, Cr, A}.
 */
typedef struct PelorusDebandParams {
    int32_t range;       /* reference-sampling radius in pixels (1..31)      */
    float thr[4];        /* per-plane normalized threshold                   */
    float grain[4];      /* per-plane normalized grain amplitude             */
    float softness;      /* blend transition width (0 = hard vf_deband)      */
    float detail_thr;    /* detail-mask activity threshold (normalized)      */
    int32_t sample_mode; /* enum pel_deband_sample_mode                      */
    int32_t blur_mode;   /* enum pel_deband_blur_mode                        */
    int32_t dither_mode; /* enum pel_deband_dither_mode                      */
    uint32_t planes;     /* bitmask of planes to process (default 0xF)       */
    uint32_t flags;      /* enum pel_deband_flags                            */
    int32_t out_depth;   /* output bit depth (8/10/12/16; 0 = same as input) */
} PelorusDebandParams;

/* Fill p with the dark-scene pre-encode defaults (range=15, square sampling,
 * average+soft blend, blue-noise dynamic grain, detail protection on). */
void pel_deband_params_default(PelorusDebandParams *p);

/* Validate p against documented ranges. Returns PEL_OK or PEL_ERR_RANGE; on
 * error and if `what` is non-NULL, *what points at a static field name. */
pel_result pel_deband_params_validate(const PelorusDebandParams *p, const char **what);

#ifdef __cplusplus
}
#endif
#endif /* PELORUS_DEBAND_H */
