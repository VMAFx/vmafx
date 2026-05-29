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

/**
 * resolution_dispatch.h — runtime resolution-aware CUDA kernel variant dispatch
 *
 * ADR-0753: CUDA kernel optimisations that win at one resolution can be neutral
 * or harmful at another.  This header provides a single classification function
 * that maps a (width, height) pair to a coarse workload class, enabling each
 * feature extractor to branch on workload class at kernel-launch time rather
 * than compiling separate binaries.
 *
 * Policy table (from profiling data in Research-0748/0749/0751; ADR-0753):
 *
 *   Kernel                                  WS_SMALL   WS_MEDIUM  WS_LARGE
 *   -----------------------------------------------------------------------
 *   adm_cm_line_kernel_8 __launch_bounds__  SKIP       APPLY      SKIP
 *   filter1d_8_horizontal __ldg + bounds    SKIP       APPLY      APPLY
 *   ssim_vert_combine     __ldg + bounds    SKIP       APPLY      APPLY
 *   ms_ssim_decimate      smem tiling       SKIP       SKIP       SKIP
 *
 * Kernels currently dispatched via this header:
 *   1. adm_cm_line_kernel_8 / adm_cm_line_kernel_8_no_bounds
 *      (integer_adm/adm_cm.cu + integer_adm_cuda.c::adm_cm_device)
 *   2. filter1d_8_horizontal_kernel_2_17_9 / _no_bounds
 *      (integer_vif/filter1d.cu + integer_vif_cuda.c::filter1d_8)
 *   3. calculate_ssim_vert_combine / _no_bounds
 *      (integer_ssim/ssim_score.cu + integer_ssim_cuda.c::submit_fex_cuda)
 *
 * When adding a new resolution-aware kernel, add a row to the table above
 * and an entry in the kernel list (see AGENTS.md "How to add a new variant").
 *
 * Usage pattern:
 *
 *   #include "feature/cuda/resolution_dispatch.h"
 *
 *   enum WorkloadSize ws = vmaf_cuda_workload_class(pic_w, pic_h);
 *   CUfunction fn = (ws == WS_MEDIUM)
 *                   ? s->func_adm_cm_line_kernel_8          // with __launch_bounds__
 *                   : s->func_adm_cm_line_kernel_8_no_bounds;
 *   cuLaunchKernel(fn, ...);
 *
 * How to add a new resolution-aware variant (see AGENTS.md for the full recipe):
 *   1. Add a new `CUfunction` pointer for the "other" variant to the extractor
 *      state struct.
 *   2. Load it in `init_fex_cuda` via `cuModuleGetFunction`.
 *   3. At the kernel-launch site, call `vmaf_cuda_workload_class(w, h)` and
 *      branch on the result.  Keep the branch as a single if/else — no nested
 *      policy trees.
 *   4. Add a row to the policy table in ADR-0753.
 */

#ifndef VMAF_CUDA_RESOLUTION_DISPATCH_H
#define VMAF_CUDA_RESOLUTION_DISPATCH_H

/* ---------------------------------------------------------------------------
 * WorkloadSize — coarse workload class derived from luma pixel count.
 *
 * Thresholds (exclusive upper bound on pixel count):
 *   WS_SMALL  : w*h <  1280*720   = 921600
 *   WS_MEDIUM : w*h <  3840*2160  = 8294400
 *   WS_LARGE  : w*h >= 8294400
 *
 * These are proxy thresholds, not exact resolution gates.  A 720×480 frame
 * (345600 px) is WS_SMALL; a 1920×800 letterbox (1536000 px) is WS_MEDIUM.
 * The thresholds were chosen to match the three profiling operating points
 * (576p, 1080p, 4K) that produced the policy table in ADR-0753.
 * --------------------------------------------------------------------------- */
enum WorkloadSize {
    WS_SMALL = 0,  /* < 720p pixel count: launch-overhead-bound, no occupancy opt   */
    WS_MEDIUM = 1, /* 720p .. <4K:        occupancy effects measurable (8–32 waves)  */
    WS_LARGE = 2,  /* >= 4K pixel count:  kernels saturate; smem tiling beneficial   */
};

/* Pixel-count thresholds — exposed so callers can write policy comments
 * that cite the same numbers without hard-coding magic constants. */
#define VMAF_CUDA_PIXEL_THRESHOLD_MEDIUM (1280 * 720) /* 921600  */
#define VMAF_CUDA_PIXEL_THRESHOLD_LARGE (3840 * 2160) /* 8294400 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * vmaf_cuda_workload_class() — classify (w, h) into a WorkloadSize bucket.
 *
 * Pure C; no CUDA headers required.  Safe to call from every CUDA feature
 * extractor's submit callback (once per submitted frame pair).
 *
 * @param w  Luma plane width  in pixels (must be > 0).
 * @param h  Luma plane height in pixels (must be > 0).
 * @return   WS_SMALL, WS_MEDIUM, or WS_LARGE.
 */
enum WorkloadSize vmaf_cuda_workload_class(int w, int h);

#ifdef __cplusplus
}
#endif

#endif /* VMAF_CUDA_RESOLUTION_DISPATCH_H */
