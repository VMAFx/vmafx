/**
 *
 *  Copyright 2016-2026 Netflix, Inc.
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

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "feature_extractor.h"
#include "log.h"
#include "picture.h"

#ifdef HAVE_NVTX
#include <nvtx3/nvToolsExt.h>
#endif

#if VMAF_FLOAT_FEATURES
extern VmafFeatureExtractor vmaf_fex_float_psnr;
extern VmafFeatureExtractor vmaf_fex_float_adm;
extern VmafFeatureExtractor vmaf_fex_float_motion;
extern VmafFeatureExtractor vmaf_fex_float_moment;
extern VmafFeatureExtractor vmaf_fex_float_vif;
extern VmafFeatureExtractor vmaf_fex_speed_chroma;
extern VmafFeatureExtractor vmaf_fex_speed_temporal;
#endif
extern VmafFeatureExtractor vmaf_fex_float_ssim;
extern VmafFeatureExtractor vmaf_fex_float_ms_ssim;
extern VmafFeatureExtractor vmaf_fex_ssim;
extern VmafFeatureExtractor vmaf_fex_ssimulacra2;
extern VmafFeatureExtractor vmaf_fex_ciede;
extern VmafFeatureExtractor vmaf_fex_psnr;
extern VmafFeatureExtractor vmaf_fex_psnr_hvs;
extern VmafFeatureExtractor vmaf_fex_integer_adm;
extern VmafFeatureExtractor vmaf_fex_integer_motion;
/* vmaf_fex_integer_motion_v2 (CPU): pipelined variant registered alongside
 * vmaf_fex_integer_motion. Tests look up "motion_v2" by name; both CPU
 * extractors coexist — motion handles the 3-frame default mode,
 * motion_v2 handles the pipelined fused-SAD path. GPU variants
 * (_cuda, _sycl, _hip, _metal) are separate TUs already present below. */
extern VmafFeatureExtractor vmaf_fex_integer_motion_v2;
extern VmafFeatureExtractor vmaf_fex_integer_vif;
extern VmafFeatureExtractor vmaf_fex_cambi;
#if HAVE_CUDA
extern VmafFeatureExtractor vmaf_fex_integer_adm_cuda;
extern VmafFeatureExtractor vmaf_fex_integer_vif_cuda;
extern VmafFeatureExtractor vmaf_fex_integer_motion_cuda;
extern VmafFeatureExtractor vmaf_fex_integer_motion_v2_cuda;
extern VmafFeatureExtractor vmaf_fex_psnr_cuda;
extern VmafFeatureExtractor vmaf_fex_float_moment_cuda;
extern VmafFeatureExtractor vmaf_fex_ciede_cuda;
extern VmafFeatureExtractor vmaf_fex_float_ssim_cuda;
/* Real integer_ssim CUDA extractor — bit-exact with CPU vmaf_fex_ssim
 * (ADR-0564). Provided by cuda/ssim_cuda.c. Distinct from
 * vmaf_fex_float_ssim_cuda which uses floating-point Gaussian weights. */
extern VmafFeatureExtractor vmaf_fex_integer_ssim_cuda;
extern VmafFeatureExtractor vmaf_fex_float_ms_ssim_cuda;
extern VmafFeatureExtractor vmaf_fex_psnr_hvs_cuda;
extern VmafFeatureExtractor vmaf_fex_float_psnr_cuda;
extern VmafFeatureExtractor vmaf_fex_float_motion_cuda;
extern VmafFeatureExtractor vmaf_fex_float_vif_cuda;
extern VmafFeatureExtractor vmaf_fex_ssimulacra2_cuda;
extern VmafFeatureExtractor vmaf_fex_float_adm_cuda;
/* T3-15 / ADR-0360: cambi CUDA twin (Strategy II hybrid). */
extern VmafFeatureExtractor vmaf_fex_cambi_cuda;
/* ADR-0965: speed_{chroma,temporal} CUDA twins — CHECK_CUDA → CHECK_CUDA_GOTO
 * repair pass complete.  Real GPU kernels (means, cov, indterm, backward-sub,
 * score); host-side eigendecomp + QR via speed_internal.c (ADR-0964). */
extern VmafFeatureExtractor vmaf_fex_speed_chroma_cuda;
extern VmafFeatureExtractor vmaf_fex_speed_temporal_cuda;
#endif
#if HAVE_SYCL
extern VmafFeatureExtractor vmaf_fex_integer_vif_sycl;
extern VmafFeatureExtractor vmaf_fex_integer_adm_sycl;
extern VmafFeatureExtractor vmaf_fex_integer_motion_sycl;
extern VmafFeatureExtractor vmaf_fex_integer_motion_v2_sycl;
extern VmafFeatureExtractor vmaf_fex_psnr_sycl;
extern VmafFeatureExtractor vmaf_fex_float_moment_sycl;
extern VmafFeatureExtractor vmaf_fex_ciede_sycl;
extern VmafFeatureExtractor vmaf_fex_float_ssim_sycl;
/* Real integer_ssim SYCL extractor (ADR-0564). int64 moments + float32
 * SSIM formula (fp64-free per ADR-0220); places=4-5 vs CPU. */
extern VmafFeatureExtractor vmaf_fex_integer_ssim_sycl;
extern VmafFeatureExtractor vmaf_fex_float_ms_ssim_sycl;
extern VmafFeatureExtractor vmaf_fex_psnr_hvs_sycl;
extern VmafFeatureExtractor vmaf_fex_float_psnr_sycl;
extern VmafFeatureExtractor vmaf_fex_float_motion_sycl;
extern VmafFeatureExtractor vmaf_fex_float_vif_sycl;
extern VmafFeatureExtractor vmaf_fex_ssimulacra2_sycl;
extern VmafFeatureExtractor vmaf_fex_float_adm_sycl;
/* T3-15 / ADR-0371: cambi SYCL twin (Strategy II hybrid, closes CUDA→SYCL
 * parity gap). */
extern VmafFeatureExtractor vmaf_fex_cambi_sycl;
/* ADR-0964: speed_{chroma,temporal} SYCL twins.  Same hybrid split as the
 * CUDA twins, plus the AdaptiveCpp vs DPC++ kernel adaptations described
 * in feature/sycl/speed_chroma_sycl.cpp. */
extern VmafFeatureExtractor vmaf_fex_speed_chroma_sycl;
extern VmafFeatureExtractor vmaf_fex_speed_temporal_sycl;
#endif
#if HAVE_HIP
/* HIP first-consumer kernel — T7-10 / ADR-0241. Registration succeeds
 * but `init()` returns -ENOSYS until the runtime PR (T7-10b) replaces
 * the kernel-template helper bodies with real HIP calls. */
extern VmafFeatureExtractor vmaf_fex_psnr_hip;
/* HIP second-consumer kernel — T7-10b / ADR-0254. First real kernel:
 * float_psnr_hip. With `enable_hipcc=true` the HSACO is embedded and
 * the kernel runs on device; without it init() returns -ENOSYS. */
extern VmafFeatureExtractor vmaf_fex_float_psnr_hip;
/* HIP third-consumer kernel — T7-10b follow-up / ADR-0257. Same
 * scaffold posture as the first consumer: registration succeeds,
 * `init()` returns -ENOSYS until T7-10b. Mirrors
 * `vmaf_fex_ciede_cuda` field-for-field. */
extern VmafFeatureExtractor vmaf_fex_ciede_hip;
/* HIP fourth-consumer kernel — T7-10b follow-up / ADR-0258. Same
 * scaffold posture; emits four `float_moment_*` features. */
extern VmafFeatureExtractor vmaf_fex_float_moment_hip;
/* HIP sixth consumer — ADR-0267. Same posture as the first consumer:
 * registration succeeds, `init()` returns -ENOSYS until T7-10b.
 * (float_ansnr_hip removed: ADR-0720.) */
extern VmafFeatureExtractor vmaf_fex_integer_motion_v2_hip;
/* HIP integer_motion consumer — ADR-0468 scaffold, registration
 * landed in ADR-0523 (PR #1283), promoted to real kernel with
 * `VMAF_FEATURE_EXTRACTOR_HIP` set + selectable dispatch by
 * ADR-0530. Carries `VMAF_FEATURE_EXTRACTOR_HIP` so
 * `compute_fex_flags()` selects it when a HIP state is imported
 * (otherwise the CPU twin wins by tie-break order). Emits
 * VMAF_integer_feature_motion_score, _motion2_score, and
 * _motion3_score (mirrors `vmaf_fex_integer_motion_cuda`). */
extern VmafFeatureExtractor vmaf_fex_integer_motion_hip;
/* HIP seventh-consumer kernel — T7-10b follow-up / ADR-0273. Same
 * scaffold posture; mirrors the CUDA twin
 * `feature/cuda/float_motion_cuda.c` and pins the temporal-extractor
 * shape with a raw-pixel cache + blurred-frame ping-pong slot pair. */
extern VmafFeatureExtractor vmaf_fex_float_motion_hip;
/* HIP eighth-consumer kernel — T7-10b follow-up / ADR-0274. Same
 * scaffold posture; mirrors the CUDA twin
 * `feature/cuda/integer_ssim_cuda.c` and pins the two-dispatch +
 * five intermediate float buffers shape. v1: scale=1 only. */
extern VmafFeatureExtractor vmaf_fex_float_ssim_hip;
/* HIP CAMBI banding-detector — port of integer_cambi_cuda.c (ADR-0360).
 * Strategy II hybrid: three GPU kernels + CPU residual via cambi_internal.h.
 * With `enable_hipcc=true` the HSACO is loaded and the kernels run on device;
 * without it init() returns -ENOSYS. Bit-exact at places=4 w.r.t. the CPU
 * and CUDA twins. */
extern VmafFeatureExtractor vmaf_fex_cambi_hip;
/* HIP eleventh-consumer kernel — integer VIF HIP port. Eight kernels
 * (four vertical + four horizontal, one per scale) in
 * feature/hip/integer_vif/vif_statistics.hip. Mirrors
 * `vmaf_fex_integer_vif_cuda` field-for-field. With `enable_hipcc=true`
 * the HSACO blob is loaded and the kernels run on device; without it
 * init() returns -ENOSYS. Emits the same four VMAF_integer_feature_vif_scaleN
 * features as the CPU and CUDA twins. */
extern VmafFeatureExtractor vmaf_fex_integer_vif_hip;
/* HIP twelfth-consumer kernel — T7-10b batch-2 / ADR-0468. Mirrors
 * `feature/cuda/float_adm_cuda.c` — 4-stage DWT+CSF+CM pipeline,
 * 16 launches per frame, host double-precision reduction. With
 * `enable_hipcc=true` the HSACO runs on device; without it init()
 * returns -ENOSYS (scaffold posture). */
extern VmafFeatureExtractor vmaf_fex_float_adm_hip;
/* ADR-0533: full HIP-extractor registration sweep. Each of the symbols
 * below already lived in libvmaf/src/feature/hip/ mirroring its
 * CUDA twin, but the TUs were never compiled into the HIP runtime
 * archive and never declared in this file. The companion meson edit
 * adds the source files to `hip_sources`; the entries below make
 * `vmaf_get_feature_extractor_by_name(<name>)` resolve. Scaffold
 * posture: `init()` returns -ENOSYS unless the kernel TU carries a real
 * HSACO blob compiled under `enable_hipcc=true` (matches the prior
 * consumers' contract — psnr_hip / ciede_hip / etc.). */
extern VmafFeatureExtractor vmaf_fex_float_vif_hip;
extern VmafFeatureExtractor vmaf_fex_integer_adm_hip;
extern VmafFeatureExtractor vmaf_fex_integer_ms_ssim_hip;
extern VmafFeatureExtractor vmaf_fex_psnr_hvs_hip;
extern VmafFeatureExtractor vmaf_fex_integer_ssim_hip;
extern VmafFeatureExtractor vmaf_fex_ssimulacra2_hip;
/* ADR-0964: speed_{chroma,temporal} HIP twins.  Same hybrid GPU/CPU
 * split as the CUDA twins; wavefront-64 adaptations and host-side
 * eigendecomp + QR via feature/speed_internal.c. */
extern VmafFeatureExtractor vmaf_fex_speed_chroma_hip;
extern VmafFeatureExtractor vmaf_fex_speed_temporal_hip;
#endif
#if HAVE_METAL
/* Metal feature extractors — T8-1c through T8-1j / ADR-0421, plus
 * T8-2b (float_ms_ssim) per ADR-0490. All consumers below are fully
 * implemented as Obj-C++ .mm dispatch files in feature/metal/;
 * kernels live under feature/metal/, compiled via xcrun into the
 * __TEXT,__metallib section of libvmaf. ADR-0545 retired the dead
 * scaffold sources (integer_adm_metal, integer_cambi_metal, ...) and
 * the orphan extern for integer_adm_metal that referenced them. */
extern VmafFeatureExtractor vmaf_fex_integer_motion_v2_metal;
extern VmafFeatureExtractor vmaf_fex_integer_psnr_metal;
extern VmafFeatureExtractor vmaf_fex_float_ssim_metal;
extern VmafFeatureExtractor vmaf_fex_integer_motion_metal;
extern VmafFeatureExtractor vmaf_fex_float_psnr_metal;
extern VmafFeatureExtractor vmaf_fex_float_motion_metal;
extern VmafFeatureExtractor vmaf_fex_float_moment_metal;
extern VmafFeatureExtractor vmaf_fex_float_ms_ssim_metal;
#endif
/* SpEED-QA NR metric scaffold — ADR-0253. */
extern VmafFeatureExtractor vmaf_fex_speed_qa;
extern VmafFeatureExtractor vmaf_fex_lpips;
extern VmafFeatureExtractor vmaf_fex_dists_sq;
extern VmafFeatureExtractor vmaf_fex_fastdvdnet_pre;
extern VmafFeatureExtractor vmaf_fex_mobilesal;
extern VmafFeatureExtractor vmaf_fex_transnet_v2;
extern VmafFeatureExtractor vmaf_fex_null;

#if HAVE_RUST_TAD
/* ADR-0707: TAD (Temporal Absolute Difference) — Rust/cbindgen pilot extractor.
 * Only registered when the Rust staticlib is linked
 * (enable_rust_features=true). */
extern VmafFeatureExtractor vmaf_fex_tad;
#endif

static VmafFeatureExtractor *feature_extractor_list[] = {
#if VMAF_FLOAT_FEATURES
    &vmaf_fex_float_psnr, &vmaf_fex_float_adm, &vmaf_fex_float_vif, &vmaf_fex_float_motion,
    &vmaf_fex_float_moment, &vmaf_fex_speed_chroma, &vmaf_fex_speed_temporal,
#endif
    &vmaf_fex_float_ms_ssim, &vmaf_fex_float_ssim, &vmaf_fex_ssim, &vmaf_fex_ssimulacra2,
    &vmaf_fex_ciede, &vmaf_fex_psnr, &vmaf_fex_psnr_hvs, &vmaf_fex_integer_adm,
    &vmaf_fex_integer_motion, &vmaf_fex_integer_motion_v2, &vmaf_fex_integer_vif, &vmaf_fex_cambi,
#if HAVE_SYCL
    /* SYCL before CUDA: when multiple GPU backends are compiled in,
     * the first matching extractor wins.  SYCL is the preferred backend
     * because it supports the widest range of Intel hardware.
     * Use --no_sycl to fall back to CUDA.
     *
     * ADR-0545: each extractor symbol appears EXACTLY ONCE here.  The
     * prior arrangement registered six SYCL twins twice
     * (psnr_sycl / float_moment_sycl / ciede_sycl / float_ssim_sycl /
     * float_ms_ssim_sycl / psnr_hvs_sycl), which caused the ctx pool's
     * by-feature-name iterator to instantiate two contexts per twin and
     * run their init/extract/flush hooks twice per pic. */
    &vmaf_fex_integer_vif_sycl, &vmaf_fex_integer_adm_sycl, &vmaf_fex_integer_motion_sycl,
    &vmaf_fex_integer_motion_v2_sycl, &vmaf_fex_psnr_sycl, &vmaf_fex_float_moment_sycl,
    /* ADR-0564: integer_ssim_sycl provides real int64-moment integer_ssim.
     * float32 SSIM formula (fp64-free ADR-0220); expected places=4-5 vs CPU. */
    &vmaf_fex_integer_ssim_sycl, &vmaf_fex_ciede_sycl, &vmaf_fex_float_ssim_sycl,
    &vmaf_fex_float_ms_ssim_sycl, &vmaf_fex_psnr_hvs_sycl, &vmaf_fex_float_psnr_sycl,
    &vmaf_fex_float_motion_sycl, &vmaf_fex_float_vif_sycl, &vmaf_fex_ssimulacra2_sycl,
    &vmaf_fex_float_adm_sycl,
    /* T3-15 / ADR-0371: cambi SYCL twin (closes last CUDA→SYCL parity gap). */
    &vmaf_fex_cambi_sycl,
    /* ADR-0964: speed_{chroma,temporal} SYCL twins.  Wire the existing
     * sycl/speed_{chroma,temporal}_sycl.cpp TUs into the registry so
     * `vmaf_get_feature_extractor_by_name("speed_chroma_sycl")` resolves.
     * Hybrid GPU/CPU split — see core/src/feature/speed_internal.h. */
    &vmaf_fex_speed_chroma_sycl, &vmaf_fex_speed_temporal_sycl,
#endif
#if HAVE_CUDA
    &vmaf_fex_integer_adm_cuda, &vmaf_fex_integer_vif_cuda, &vmaf_fex_integer_motion_cuda,
    &vmaf_fex_integer_motion_v2_cuda, &vmaf_fex_psnr_cuda, &vmaf_fex_float_moment_cuda,
    /* ADR-0564: integer_ssim_cuda provides real bit-exact integer_ssim on CUDA.
     * Placed before float_ssim_cuda so the registry picks the integer path when
     * both CUDA and CPU are compiled in. */
    &vmaf_fex_integer_ssim_cuda, &vmaf_fex_ciede_cuda, &vmaf_fex_float_ssim_cuda,
    &vmaf_fex_float_ms_ssim_cuda, &vmaf_fex_psnr_hvs_cuda, &vmaf_fex_float_psnr_cuda,
    &vmaf_fex_float_motion_cuda, &vmaf_fex_float_vif_cuda, &vmaf_fex_ssimulacra2_cuda,
    &vmaf_fex_float_adm_cuda,
    /* T3-15 / ADR-0360: cambi CUDA twin (Strategy II hybrid). */
    &vmaf_fex_cambi_cuda,
    /* ADR-0965: speed_{chroma,temporal} CUDA twins — repair pass complete.
     * Hybrid GPU/CPU split: GPU runs means/cov/indterm/backward-sub/score
     * kernels; CPU runs eigendecomp + QR (unavoidable serial constraint).
     * places=4 vs CPU reference (ADR-0214). */
    &vmaf_fex_speed_chroma_cuda, &vmaf_fex_speed_temporal_cuda,
#endif
#if HAVE_HIP
    /* T7-10 first consumer (ADR-0241): registration succeeds even on
     * the scaffold-only build so a caller asking for `psnr_hip` gets
     * the cleaner "extractor found, runtime not ready (-ENOSYS)"
     * surface instead of "no such extractor". The runtime PR
     * (T7-10b) keeps this row verbatim and adds its siblings. */
    &vmaf_fex_psnr_hip,
    /* T7-10b second consumer (ADR-0254): first real kernel. With
     * `enable_hipcc=true` the HSACO is loaded and the kernel runs
     * on device; without it init() returns -ENOSYS (scaffold posture).
     * Emits `float_psnr` (luma-only, same as the CUDA twin). */
    &vmaf_fex_float_psnr_hip,
    /* Third consumer (ADR-0257): `ciede_hip` mirrors
     * `integer_ciede_cuda.c`'s call graph. Same scaffold-only
     * registration posture — registers, `init()` returns -ENOSYS
     * until T7-10b. */
    &vmaf_fex_ciede_hip,
    /* Fourth consumer (ADR-0258): `float_moment_hip` mirrors
     * `integer_moment_cuda.c`'s call graph; emits four
     * `float_moment_*` features once the runtime kernel arrives. */
    &vmaf_fex_float_moment_hip,
    /* T7-10b sixth consumer (ADR-0267): same scaffold-posture registration
     * as the first consumer. (float_ansnr_hip removed: ADR-0720.) */
    &vmaf_fex_integer_motion_v2_hip,
    /* integer_motion_hip — ADR-0468 scaffold, registration landed
     * in ADR-0523 (PR #1283), promoted to real HIP-flagged
     * dispatch by ADR-0530. Mirrors `integer_motion_cuda.c` —
     * TEMPORAL flag, per-frame SAD reduction, motion3 5-frame
     * window. The per-extractor flag filter in
     * `vmaf_get_feature_extractor_by_feature_name` is what picks
     * the HIP path; placement order in this list is irrelevant. */
    &vmaf_fex_integer_motion_hip,
    /* Seventh consumer (ADR-0273): `float_motion_hip` mirrors
     * `float_motion_cuda.c`'s call graph (TEMPORAL flag,
     * raw-pixel cache + blurred-frame ping-pong, `flush()`
     * tail-frame motion2 emission); emits two features
     * (`VMAF_feature_motion_score`, `VMAF_feature_motion2_score`)
     * once the runtime kernel arrives. */
    &vmaf_fex_float_motion_hip,
    /* Eighth consumer (ADR-0274): `float_ssim_hip` mirrors
     * `integer_ssim_cuda.c`'s call graph (two-dispatch separable
     * Gaussian, five intermediate float buffers, per-block
     * float-partial readback); emits one feature (`float_ssim`)
     * once the runtime kernel arrives. v1 is scale=1 only. */
    &vmaf_fex_float_ssim_hip,
    /* HIP CAMBI banding-detector (ADR-0360 port): Strategy II hybrid,
     * three GPU kernels + CPU residual via cambi_internal.h. Bit-exact
     * at places=4 w.r.t. the CPU and CUDA twins. */
    &vmaf_fex_cambi_hip,
    /* Eleventh consumer: integer_vif_hip — real HIP port of the integer
     * VIF extractor. Eight kernels (four vertical + four horizontal, one
     * per scale); with `enable_hipcc=true` the HSACO blob is loaded and
     * the kernels run on device; without it init() returns -ENOSYS. */
    &vmaf_fex_integer_vif_hip,
    /* Twelfth consumer (ADR-0468): `float_adm_hip` mirrors
     * `float_adm_cuda.c` — 4-stage DWT+CSF+CM pipeline, 16
     * launches per frame (4 stages × 4 scales), host double-precision
     * WG reduction. Emits adm2 + 4 scale scores + debug features. */
    &vmaf_fex_float_adm_hip,
    /* ADR-0533: full HIP-extractor registration sweep. Each entry below
     * mirrors a CUDA twin that has been a long-standing extractor in
     * `feature_extractor_list[]` (float_vif_cuda / integer_adm_cuda /
     * integer_ms_ssim_cuda / psnr_hvs_cuda / integer_ssim_cuda /
     * ssimulacra2_cuda). The HIP TUs already shipped the
     * `VmafFeatureExtractor vmaf_fex_*_hip` symbols and pinned the
     * extractor-name strings; the entries here wire the name lookup so
     * the registry surface matches what `libvmaf/src/feature/hip/` ships.
     * Scaffold posture preserved — `init()` returns -ENOSYS unless the
     * TU's kernel half is real (ssimulacra2 + integer_ms_ssim ship full
     * HSACO blobs; the rest stay scaffold-only until the next batch). */
    &vmaf_fex_float_vif_hip, &vmaf_fex_integer_adm_hip, &vmaf_fex_integer_ms_ssim_hip,
    &vmaf_fex_psnr_hvs_hip, &vmaf_fex_integer_ssim_hip, &vmaf_fex_ssimulacra2_hip,
    /* ADR-0964: speed_{chroma,temporal} HIP twins.  Real on-device kernels
     * under enable_hipcc=true; otherwise init() returns -ENOSYS (scaffold
     * posture mirroring the other HIP consumers).  CPU-side eigendecomp +
     * QR via feature/speed_internal.c. */
    &vmaf_fex_speed_chroma_hip, &vmaf_fex_speed_temporal_hip,
#endif
#if HAVE_METAL
    /* T8-1 first consumer (ADR-0361): registration succeeds even on
     * the scaffold-only build so a caller asking for `motion_v2_metal`
     * gets the cleaner "extractor found, runtime not ready (-ENOSYS)"
     * surface instead of "no such extractor". The runtime PR (T8-1b /
     * T8-1c) keeps this row verbatim and adds its siblings. */
    &vmaf_fex_integer_motion_v2_metal,
    /* T8-1 batch-1 additional consumers (ADR-0361). */
    &vmaf_fex_integer_psnr_metal, &vmaf_fex_float_ssim_metal, &vmaf_fex_integer_motion_metal,
    /* T8-1 batch-2 additional consumers (ADR-0361): 3 float features.
     * float_ansnr_metal removed: ADR-0720 (ansnr backend drop). */
    &vmaf_fex_float_psnr_metal, &vmaf_fex_float_motion_metal, &vmaf_fex_float_moment_metal,
    /* T8-2a: float_ms_ssim_metal — 5-scale MS-SSIM pyramid on Metal
     * (ADR-0435). Real kernel dispatch replacing the -ENOSYS scaffold. */
    &vmaf_fex_float_ms_ssim_metal,
#endif
    &vmaf_fex_speed_qa, &vmaf_fex_lpips, &vmaf_fex_dists_sq, &vmaf_fex_fastdvdnet_pre,
    &vmaf_fex_mobilesal, &vmaf_fex_transnet_v2,
#if HAVE_RUST_TAD
    /* ADR-0707: TAD Rust pilot — CPU-only, off by default, no GPU twins. */
    &vmaf_fex_tad,
#endif
    &vmaf_fex_null, NULL};

VmafFeatureExtractor *vmaf_get_feature_extractor_by_name(const char *name)
{
    if (!name)
        return NULL;

    VmafFeatureExtractor *fex = NULL;
    for (unsigned i = 0; (fex = feature_extractor_list[i]); i++) {
        if (!strcmp(name, fex->name))
            return fex;
    }

    return NULL;
}

int vmaf_feature_extractor_list_audit(void)
{
    /* ADR-0545: O(N^2) over the static registry — N ≤ ~60 even with every
   * backend compiled in, so the cost is negligible (one-shot at
   * vmaf_init()).  Compare both pointer identity (catches a symbol
   * registered twice) and the `name` string (catches two distinct
   * extractor objects that happen to publish the same name).  Either
   * is a registry bug. */
    int dup_count = 0;
    for (unsigned i = 0; feature_extractor_list[i]; i++) {
        const VmafFeatureExtractor *const a = feature_extractor_list[i];
        if (!a->name)
            continue;
        for (unsigned j = i + 1; feature_extractor_list[j]; j++) {
            const VmafFeatureExtractor *const b = feature_extractor_list[j];
            if (!b->name)
                continue;
            if (a == b || !strcmp(a->name, b->name)) {
                vmaf_log(VMAF_LOG_LEVEL_ERROR,
                         "feature_extractor_list duplicate: \"%s\" at index %u and %u\n", a->name,
                         i, j);
                dup_count++;
            }
        }
    }
    if (dup_count > 0) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "feature_extractor_list has %d duplicate registration(s); "
                 "see ADR-0544. Each duplicate burns one ctx-pool entry and "
                 "one init/extract/flush call per picture.\n",
                 dup_count);
        return -EINVAL;
    }
    return 0;
}

VmafFeatureExtractor *vmaf_get_feature_extractor_by_feature_name(const char *name, unsigned flags)
{
    if (!name)
        return NULL;

    VmafFeatureExtractor *fex = NULL;

    /* First pass: prefer an extractor that matches one of the requested
     * backend flags.
     *
     * When flags == 0 (no GPU context — CPU-only caller path such as
     * vmaf_use_features_from_model without a GPU state), we must NOT
     * return a GPU-flagged extractor even though the old "flags && ..."
     * guard was always false for flags=0 and let every extractor through.
     * In an all-backends build, GPU-flagged twins sort before the CPU twin
     * in feature_extractor_list (e.g. integer_adm_sycl before integer_adm),
     * so the SYCL variant was being returned; its init guard
     * (!fex->sycl_state) then rejected every frame with -EINVAL.
     * Fix (ADR-1100): when flags == 0, skip any GPU-flagged extractor so
     * the CPU twin is selected instead.  When flags != 0, preserve the
     * existing exact-match semantics. */
    const unsigned gpu_mask =
        VMAF_FEATURE_EXTRACTOR_CUDA | VMAF_FEATURE_EXTRACTOR_SYCL | VMAF_FEATURE_EXTRACTOR_HIP;
    for (unsigned i = 0; (fex = feature_extractor_list[i]); i++) {
        if (!fex->provided_features)
            continue;
        if (flags == 0) {
            if (fex->flags & gpu_mask)
                continue;
        } else if (!(fex->flags & flags)) {
            continue;
        }
        const char *fname = NULL;
        for (unsigned j = 0; (fname = fex->provided_features[j]); j++) {
            if (!strcmp(name, fname))
                return fex;
        }
    }
    /* ADR-0530 fallback: if no flagged extractor was found, fall back to
   * any extractor providing the feature (including the CPU twin).
   * This preserves the ADR-0519 posture that lets a partially-covered
   * GPU backend (e.g. HIP, which has not yet promoted every kernel
   * out of the -ENOSYS scaffold) run the requested model by routing
   * the unflagged features through their CPU twins. Without this
   * fallback, enabling the HIP flag mask in `compute_fex_flags()`
   * would break the default VMAF model (only motion / vif / ssimu2
   * have HIP-flagged registrations today; adm2 / motion2 / etc. would
   * fail to resolve). The CUDA backend has full coverage so the
   * fallback is a no-op for it. */
    if (flags) {
        for (unsigned i = 0; (fex = feature_extractor_list[i]); i++) {
            if (!fex->provided_features)
                continue;
            const char *fname = NULL;
            for (unsigned j = 0; (fname = fex->provided_features[j]); j++) {
                if (!strcmp(name, fname))
                    return fex;
            }
        }
    }
    return NULL;
}

static int vmaf_fex_ctx_parse_options(VmafFeatureExtractorContext *fex_ctx)
{
    const VmafOption *opt = NULL;
    for (unsigned i = 0; (opt = &fex_ctx->fex->options[i]); i++) {
        if (!opt->name)
            break;
        const VmafDictionaryEntry *entry = vmaf_dictionary_get(&fex_ctx->opts_dict, opt->name, 0);
        int err = vmaf_option_set(opt, fex_ctx->fex->priv, entry ? entry->val : NULL);
        if (err)
            return -EINVAL;
    }

    return 0;
}

int vmaf_feature_extractor_context_create(VmafFeatureExtractorContext **fex_ctx,
                                          VmafFeatureExtractor *fex, VmafDictionary *opts_dict)
{
    VmafFeatureExtractorContext *f = *fex_ctx = malloc(sizeof(*f));
    if (!f)
        return -ENOMEM;
    memset(f, 0, sizeof(*f));

    VmafFeatureExtractor *x = malloc(sizeof(*x));
    if (!x)
        goto free_f;
    memcpy(x, fex, sizeof(*x));

    f->fex = x;
    if (f->fex->priv_size) {
        void *priv = malloc(f->fex->priv_size);
        if (!priv)
            goto free_x;
        memset(priv, 0, f->fex->priv_size);
        f->fex->priv = priv;
    }

    f->opts_dict = opts_dict;
    if (f->fex->options && f->fex->priv) {
        int err = vmaf_fex_ctx_parse_options(f);
        if (err) {
            /* parse_options failure leaks f->fex->priv (which is `priv`)
             * + f->fex (which is `x`) + f without this cleanup chain. */
            free(f->fex->priv);
            free(x);
            free(f);
            /* NULL the caller's handle so it cannot be dereferenced after a
             * failed create call. ASan/LeakSan: avoids dangling-pointer UAF.
             * CERT MEM30-C. */
            *fex_ctx = NULL;
            return err;
        }
    }

    return 0;

free_x:
    free(x);
free_f:
    free(f);
    /* NULL the caller's handle so it cannot be dereferenced after a failed
     * create call. ASan/LeakSan: avoids dangling-pointer UAF. CERT MEM30-C. */
    *fex_ctx = NULL;
    return -ENOMEM;
}

int vmaf_feature_extractor_context_init(VmafFeatureExtractorContext *fex_ctx,
                                        enum VmafPixelFormat pix_fmt, unsigned bpc, unsigned w,
                                        unsigned h)
{
    if (!fex_ctx)
        return -EINVAL;
    if (fex_ctx->is_initialized)
        return -EINVAL;
    if (!pix_fmt)
        return -EINVAL;

    if (fex_ctx->fex->init && !fex_ctx->is_initialized) {
        int err = fex_ctx->fex->init(fex_ctx->fex, pix_fmt, bpc, w, h);
        if (err)
            return err;
    }

    fex_ctx->is_initialized = true;
    return 0;
}

int vmaf_feature_extractor_context_extract(VmafFeatureExtractorContext *fex_ctx, VmafPicture *ref,
                                           VmafPicture *ref_90, VmafPicture *dist,
                                           VmafPicture *dist_90, unsigned pic_index,
                                           VmafFeatureCollector *vfc)
{
    if (!fex_ctx)
        return -EINVAL;
    if (!ref)
        return -EINVAL;
    if (!dist)
        return -EINVAL;
    if (!vfc)
        return -EINVAL;
    if (!fex_ctx->fex->extract)
        return -EINVAL;

    VmafPicturePrivate *ref_priv = ref->priv;
    if (fex_ctx->fex->flags & VMAF_FEATURE_EXTRACTOR_CUDA) {
        if (ref_priv->buf_type != VMAF_PICTURE_BUFFER_TYPE_CUDA_DEVICE) {
            vmaf_log(VMAF_LOG_LEVEL_ERROR, "picture buf_type mismatch: cuda fex (%s), cpu buf\n",
                     fex_ctx->fex->name);
            return -EINVAL;
        }
    } else {
        if (ref_priv->buf_type == VMAF_PICTURE_BUFFER_TYPE_CUDA_DEVICE) {
            vmaf_log(VMAF_LOG_LEVEL_ERROR, "picture buf_type mismatch: cpu fex (%s), cuda buf\n",
                     fex_ctx->fex->name);
            return -EINVAL;
        }
    }
    /* ADR-0530: HIP-flagged extractors accept both HOST and HIP_DEVICE
   * pictures. HOST is the current production path — the HIP picture
   * pool (picture_hip.{c,h}) is still a stub, so pictures arrive as
   * HOST from the YUV reader and the HIP TUs perform their own HtoD
   * copy (e.g. msh_launch() in integer_motion_hip.c). HIP_DEVICE is
   * reserved for the future picture pool. The symmetry rule is that
   * a HIP extractor must NOT see a foreign GPU buffer (CUDA / SYCL /
   * Vulkan), and a non-HIP extractor must NOT see a HIP_DEVICE buf.
   * The CUDA mismatch is already caught by the block above; the
   * remaining cross-GPU mismatches are caught here. */
    if (fex_ctx->fex->flags & VMAF_FEATURE_EXTRACTOR_HIP) {
        if (ref_priv->buf_type != VMAF_PICTURE_BUFFER_TYPE_HOST &&
            ref_priv->buf_type != VMAF_PICTURE_BUFFER_TYPE_HIP_DEVICE) {
            vmaf_log(VMAF_LOG_LEVEL_ERROR,
                     "picture buf_type mismatch: hip fex (%s), non-host/non-hip buf\n",
                     fex_ctx->fex->name);
            return -EINVAL;
        }
    } else {
        if (ref_priv->buf_type == VMAF_PICTURE_BUFFER_TYPE_HIP_DEVICE) {
            vmaf_log(VMAF_LOG_LEVEL_ERROR, "picture buf_type mismatch: cpu fex (%s), hip buf\n",
                     fex_ctx->fex->name);
            return -EINVAL;
        }
    }

#ifdef HAVE_NVTX
    nvtxRangePushA(fex_ctx->fex->name);
#endif

    if (!fex_ctx->is_initialized) {
        int err = vmaf_feature_extractor_context_init(fex_ctx, ref->pix_fmt, ref->bpc, ref->w[0],
                                                      ref->h[0]);
        if (err)
            return err;
    }

    int err = fex_ctx->fex->extract(fex_ctx->fex, ref, ref_90, dist, dist_90, pic_index, vfc);
    if (err) {
        vmaf_log(VMAF_LOG_LEVEL_WARNING, "problem with feature extractor \"%s\" at index %d\n",
                 fex_ctx->fex->name, pic_index);
    }

#ifdef HAVE_NVTX
    nvtxRangePop();
#endif

    return err;
}

int vmaf_feature_extractor_context_submit(VmafFeatureExtractorContext *fex_ctx, VmafPicture *ref,
                                          VmafPicture *ref_90, VmafPicture *dist,
                                          VmafPicture *dist_90, unsigned pic_index)
{
    if (!fex_ctx)
        return -EINVAL;
    if (!ref)
        return -EINVAL;
    if (!dist)
        return -EINVAL;
    if (!fex_ctx->fex->submit)
        return -EINVAL;

    if (!fex_ctx->is_initialized) {
        int err = vmaf_feature_extractor_context_init(fex_ctx, ref->pix_fmt, ref->bpc, ref->w[0],
                                                      ref->h[0]);
        if (err)
            return err;
    }

    return fex_ctx->fex->submit(fex_ctx->fex, ref, ref_90, dist, dist_90, pic_index);
}

int vmaf_feature_extractor_context_submit_nocopy(VmafFeatureExtractorContext *fex_ctx,
                                                 unsigned pic_index)
{
    if (!fex_ctx)
        return -EINVAL;
    if (!fex_ctx->fex->submit)
        return -EINVAL;
    if (!fex_ctx->is_initialized)
        return -EINVAL;

    return fex_ctx->fex->submit(fex_ctx->fex, NULL, NULL, NULL, NULL, pic_index);
}

int vmaf_feature_extractor_context_collect(VmafFeatureExtractorContext *fex_ctx, unsigned pic_index,
                                           VmafFeatureCollector *vfc)
{
    if (!fex_ctx)
        return -EINVAL;
    if (!vfc)
        return -EINVAL;
    if (!fex_ctx->fex->collect)
        return -EINVAL;

    return fex_ctx->fex->collect(fex_ctx->fex, pic_index, vfc);
}

int vmaf_feature_extractor_context_flush(VmafFeatureExtractorContext *fex_ctx,
                                         VmafFeatureCollector *vfc)
{
    if (!fex_ctx)
        return -EINVAL;
    if (!fex_ctx->is_initialized)
        return -EINVAL;
    if (fex_ctx->is_closed)
        return 0;

    int err = 0;
    if (fex_ctx->fex->flush) {
        while (!(err = fex_ctx->fex->flush(fex_ctx->fex, vfc))) {
            /* drain all flush-emitted features */
        }
    }
    return err < 0 ? err : 0;
}

int vmaf_feature_extractor_context_close(VmafFeatureExtractorContext *fex_ctx)
{
    if (!fex_ctx)
        return -EINVAL;
    if (!fex_ctx->is_initialized)
        return -EINVAL;
    if (fex_ctx->is_closed)
        return 0;

    int err = 0;
    if (fex_ctx->fex->close)
        err = fex_ctx->fex->close(fex_ctx->fex);
    fex_ctx->is_closed = true;
    return err;
}

int vmaf_feature_extractor_context_destroy(VmafFeatureExtractorContext *fex_ctx)
{
    if (!fex_ctx)
        return -EINVAL;

    if (fex_ctx->fex) {
        /* free(NULL) is well-defined per C99 §7.20.3.2 / POSIX free(3);
     * the NULL guard is redundant. CodeQL cpp/guarded-free. */
        free(fex_ctx->fex->priv);
        free(fex_ctx->fex);
    }
    if (fex_ctx->opts_dict)
        vmaf_dictionary_free(&fex_ctx->opts_dict);
    free(fex_ctx);
    return 0;
}

int vmaf_fex_ctx_pool_create(VmafFeatureExtractorContextPool **pool, unsigned n_threads)
{
    if (!pool)
        return -EINVAL;
    if (!n_threads)
        return -EINVAL;

    VmafFeatureExtractorContextPool *const p = *pool = malloc(sizeof(*p));
    if (!p)
        goto fail;
    memset(p, 0, sizeof(*p));

    p->n_threads = n_threads;

    p->cnt = 0;
    p->capacity = 8;
    const size_t fex_list_sz = sizeof(*(p->fex_list)) * p->capacity;
    p->fex_list = malloc(fex_list_sz);
    if (!p->fex_list)
        goto free_p;
    memset(p->fex_list, 0, fex_list_sz);

    if (pthread_mutex_init(&(p->lock), NULL) != 0)
        goto free_fex_list;
    return 0;

free_fex_list:
    free(p->fex_list);
free_p:
    free(p);
fail:
    /* NULL the caller's handle so it cannot be dereferenced after a failed
     * pool create. ASan/LeakSan: avoids dangling-pointer UAF. CERT MEM30-C. */
    *pool = NULL;
    return -ENOMEM;
}

static struct fex_list_entry *get_fex_list_entry(VmafFeatureExtractorContextPool *pool,
                                                 VmafFeatureExtractor *fex,
                                                 VmafDictionary *opts_dict)
{
    if (!pool)
        return NULL;
    if (!fex)
        return NULL;

    for (unsigned i = 0; i < pool->cnt; i++) {
        struct fex_list_entry *entry = &pool->fex_list[i];
        if (!strcmp(fex->name, entry->fex->name) &&
            !vmaf_dictionary_compare(opts_dict, entry->opts_dict)) {
            return entry;
        }
    }

    struct fex_list_entry entry = {0};

    entry.fex = fex;
    const unsigned n_threads = (fex->flags & VMAF_FEATURE_EXTRACTOR_TEMPORAL ? 1 : pool->n_threads);
    atomic_init(&entry.capacity, n_threads);
    atomic_init(&entry.in_use, 0);
    pthread_cond_init(&(entry.full), NULL);
    size_t ctx_array_sz = sizeof(entry.ctx_list[0]) * entry.capacity;
    entry.ctx_list = malloc(ctx_array_sz);
    if (!entry.ctx_list)
        goto fail;
    memset(entry.ctx_list, 0, ctx_array_sz);
    /* vmaf_dictionary_copy returns -ENOMEM on alloc failure; the
     * caller must check, otherwise entry.opts_dict ends up
     * partially-constructed and the comparison at line 750 lies.
     *
     * NULL opts_dict is the common case (e.g. vmaf_use_feature(... NULL),
     * model-loaded features without options, test_feature_extractor's
     * direct vmaf_fex_ctx_pool_aquire(... NULL ...) call). For NULL
     * opts_dict we leave entry.opts_dict at its zero-initialised NULL
     * — matching the pre-PR #296 silent-no-op semantics that
     * vmaf_dictionary_compare(NULL, NULL) handles correctly. Only
     * dispatch to the copy when there's actually something to copy. */
    if (opts_dict != NULL) {
        if (vmaf_dictionary_copy(&opts_dict, &entry.opts_dict) != 0)
            goto free_ctx_list;
    }

    if (pool->cnt >= pool->capacity) {
        assert(pool->capacity > 0);
        const size_t capacity = (size_t)pool->capacity * 2;
        struct fex_list_entry *fex_list =
            realloc(pool->fex_list, sizeof(*(pool->fex_list)) * capacity);
        if (!fex_list)
            goto free_opts_dict;
        pool->fex_list = fex_list;
        pool->capacity = capacity;
    }

    pool->fex_list[pool->cnt] = entry;
    return &pool->fex_list[pool->cnt++];

free_opts_dict:
    /* opts_dict is owned by entry once vmaf_dictionary_copy succeeded;
     * realloc failure leaves us holding both. Free in reverse-init
     * order. */
    vmaf_dictionary_free(&entry.opts_dict);
free_ctx_list:
    free(entry.ctx_list);
fail:
    return NULL;
}

static int ctx_pool_ensure_slot_ctx(struct fex_list_entry *entry, int i, VmafFeatureExtractor *fex,
                                    VmafDictionary *opts_dict)
{
    if (entry->ctx_list[i].fex_ctx)
        return 0;

    VmafDictionary *d = NULL;
    if (opts_dict) {
        int err = vmaf_dictionary_copy(&opts_dict, &d);
        if (err) {
            (void)vmaf_dictionary_free(&d);
            return err;
        }
    }
    VmafFeatureExtractorContext *f = NULL;
    int err = vmaf_feature_extractor_context_create(&f, entry->fex, d);
    if (err) {
        (void)vmaf_dictionary_free(&d);
        return err;
    }
    entry->ctx_list[i].fex_ctx = f;
    if (f->fex->flags & VMAF_FEATURE_FRAME_SYNC) {
        f->fex->framesync = (fex->framesync);
    }
    return 0;
}

static int ctx_pool_claim_slot(struct fex_list_entry *entry, VmafFeatureExtractor *fex,
                               VmafDictionary *opts_dict, VmafFeatureExtractorContext **fex_ctx)
{
    for (int i = 0; i < atomic_load(&entry->capacity); i++) {
        int err = ctx_pool_ensure_slot_ctx(entry, i, fex, opts_dict);
        if (err)
            return err;
        if (!entry->ctx_list[i].in_use) {
            *fex_ctx = entry->ctx_list[i].fex_ctx;
            entry->ctx_list[i].in_use = true;
            break;
        }
    }
    atomic_fetch_add(&entry->in_use, 1);
    return 0;
}

int vmaf_fex_ctx_pool_aquire(VmafFeatureExtractorContextPool *pool, VmafFeatureExtractor *fex,
                             VmafDictionary *opts_dict, VmafFeatureExtractorContext **fex_ctx)
{
    if (!pool)
        return -EINVAL;
    if (!fex)
        return -EINVAL;
    if (!fex_ctx)
        return -EINVAL;

    pthread_mutex_lock(&(pool->lock));
    int err = 0;

    struct fex_list_entry *entry = get_fex_list_entry(pool, fex, opts_dict);
    if (!entry) {
        err = -EINVAL;
        goto unlock;
    }

    while (atomic_load(&entry->capacity) == atomic_load(&entry->in_use))
        pthread_cond_wait(&(entry->full), &(pool->lock));

    err = ctx_pool_claim_slot(entry, fex, opts_dict, fex_ctx);

unlock:
    pthread_mutex_unlock(&(pool->lock));
    return err;
}

int vmaf_fex_ctx_pool_release(VmafFeatureExtractorContextPool *pool,
                              VmafFeatureExtractorContext *fex_ctx)
{
    if (!pool)
        return -EINVAL;
    if (!fex_ctx)
        return -EINVAL;

    pthread_mutex_lock(&(pool->lock));
    int err = 0;

    VmafFeatureExtractor *fex = fex_ctx->fex;
    struct fex_list_entry *entry = NULL;
    for (unsigned i = 0; i < pool->cnt; i++) {
        if (!strcmp(fex->name, pool->fex_list[i].fex->name) &&
            !vmaf_dictionary_compare(fex_ctx->opts_dict, pool->fex_list[i].opts_dict)) {
            entry = &pool->fex_list[i];
            break;
        }
    }

    if (!entry) {
        err = -EINVAL;
        goto unlock;
    }

    for (int i = 0; i < atomic_load(&entry->capacity); i++) {
        if (fex_ctx == entry->ctx_list[i].fex_ctx) {
            entry->ctx_list[i].in_use = false;
            atomic_fetch_sub(&entry->in_use, 1);
            pthread_cond_signal(&(entry->full));
            goto unlock;
        }
    }
    err = -EINVAL;

unlock:
    pthread_mutex_unlock(&(pool->lock));
    return err;
}

int vmaf_fex_ctx_pool_flush(VmafFeatureExtractorContextPool *pool,
                            VmafFeatureCollector *feature_collector)
{
    if (!pool)
        return -EINVAL;
    if (!pool->fex_list)
        return -EINVAL;
    pthread_mutex_lock(&(pool->lock));

    for (unsigned i = 0; i < pool->cnt; i++) {
        VmafFeatureExtractor *fex = pool->fex_list[i].fex;
        if (!(fex->flags & VMAF_FEATURE_EXTRACTOR_TEMPORAL))
            continue;
        for (int j = 0; j < atomic_load(&pool->fex_list[i].capacity); j++) {
            VmafFeatureExtractorContext *fex_ctx = pool->fex_list[i].ctx_list[j].fex_ctx;
            if (!fex_ctx)
                continue;
            vmaf_feature_extractor_context_flush(fex_ctx, feature_collector);
        }
    }

    pthread_mutex_unlock(&(pool->lock));
    return 0;
}

int vmaf_fex_ctx_pool_destroy(VmafFeatureExtractorContextPool *pool)
{
    if (!pool)
        return -EINVAL;
    if (!pool->fex_list)
        goto free_pool;
    pthread_mutex_lock(&(pool->lock));

    for (unsigned i = 0; i < pool->cnt; i++) {
        if (!pool->fex_list[i].ctx_list)
            continue;
        for (int j = 0; j < atomic_load(&pool->fex_list[i].capacity); j++) {
            VmafFeatureExtractorContext *fex_ctx = pool->fex_list[i].ctx_list[j].fex_ctx;
            if (!fex_ctx)
                continue;
            vmaf_feature_extractor_context_close(fex_ctx);
            vmaf_feature_extractor_context_destroy(fex_ctx);
        }
        /* opts_dict is held once per entry (not per ctx slot). Free
         * outside the j loop to avoid the previous double-free on
         * pools where n_threads > 1 (vmaf_dictionary_free nulls the
         * pointer, so subsequent calls were free(NULL) no-ops, but
         * the intent was clearly per-entry). */
        vmaf_dictionary_free(&pool->fex_list[i].opts_dict);
        free(pool->fex_list[i].ctx_list);
    }
    free(pool->fex_list);
    /* POSIX requires unlock + destroy before free; freeing a locked or
     * un-destroyed mutex is undefined behaviour. Unlock first (we hold
     * the lock from the pthread_mutex_lock above), then destroy.
     * ASan/LeakSan: mutex state is embedded in pool — freeing the pool
     * before destroying the mutex leaks POSIX TSD resources on glibc. */
    (void)pthread_mutex_unlock(&(pool->lock));
    (void)pthread_mutex_destroy(&(pool->lock));

free_pool:
    free(pool);
    return 0;
}
