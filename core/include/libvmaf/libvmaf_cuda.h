/**
 *
 *  Copyright 2016-2023 Netflix, Inc.
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

#ifndef LIBVMAF_LIBVMAF_CUDA_H
#define LIBVMAF_LIBVMAF_CUDA_H

#include "libvmaf/libvmaf.h"
#include "libvmaf/macros.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @typedef VmafCudaState
 * @brief   Opaque CUDA backend handle pinning one device + compute stream.
 *
 * Created by @ref vmaf_cuda_state_init and released by
 * @ref vmaf_cuda_state_free. Holds the imported or freshly-created
 * `CUcontext`, the per-session CUDA stream, the device-side picture pool, and
 * the per-feature kernel scratch allocations. Hand to a @ref VmafContext via
 * @ref vmaf_cuda_import_state to enable CUDA feature extraction.
 *
 * Same lifetime model as `VmafSyclState`, `VmafHipState`, and
 * `VmafMetalState`. One state pins one device; callers that want multi-GPU
 * fan-out create one state per device and one VmafContext per state.
 */
typedef struct VmafCudaState VmafCudaState;

/**
 * @struct VmafCudaConfiguration
 * @brief  Configuration for @ref vmaf_cuda_state_init.
 *
 * Lets the caller hand in a pre-existing `CUcontext` (e.g. one already created
 * by the host application's CUDA driver setup, or one shared with NVENC /
 * NVDEC). Safe to zero-initialise — when @p cu_ctx is NULL libvmaf creates a
 * fresh context on the current CUDA device.
 *
 * @field cu_ctx Optional caller-owned `CUcontext` (cast to `void *` so this
 *               header stays free of `<cuda.h>`). NULL → libvmaf creates a
 *               new context. When non-NULL the caller retains ownership; the
 *               context must outlive the VmafCudaState.
 */
typedef struct VmafCudaConfiguration {
    void *cu_ctx; /**< Optional CUcontext (cast from `CUcontext`); NULL → create one. */
} VmafCudaConfiguration;

/**
 * Initialize VmafCudaState.
 * VmafCudaState can optionally be configured with VmafCudaConfiguration.
 *
 * @param cu_state The CUDA state to open.
 *
 * @param cfg      Optional configuration parameters.
 *
 *
 * @return 0 on success, or < 0 (a negative errno code) on error.
 *
 * @thread-safety Not thread-safe. Allocate one VmafCudaState per driver thread.
 */
VMAF_EXPORT int vmaf_cuda_state_init(VmafCudaState **cu_state, VmafCudaConfiguration cfg);

/**
 * Free VmafCudaState allocated by `vmaf_cuda_state_init()`.
 *
 * Must be called AFTER `vmaf_close()` on any VmafContext that imported
 * this state via `vmaf_cuda_import_state()`, because `vmaf_close()`
 * destroys the underlying CUDA stream and context. Calling
 * `vmaf_cuda_state_free()` first would leave `vmaf_close()` with a
 * dangling state.
 *
 * @param cu_state CUDA state to free. Safe to pass NULL.
 *
 * @return 0 on success, or < 0 (a negative errno code) on error.
 *
 * @thread-safety Not thread-safe. Call after vmaf_close() on every
 *               context that imported this state.
 */
VMAF_EXPORT int vmaf_cuda_state_free(VmafCudaState *cu_state);

/**
 * Import VmafCudaState for use during CUDA feature extraction.
 *
 * @param vmaf VMAF context allocated with `vmaf_init()`.
 *
 * @param cu_state CUDA state allocated with `vmaf_cuda_state_init()`.
 *
 * @return 0 on success, or < 0 (a negative errno code) on error.
 *
 * @thread-safety Not thread-safe. Call before vmaf_use_features_from_model()
 *               and vmaf_read_pictures() on the same context.
 */
VMAF_EXPORT int vmaf_cuda_import_state(VmafContext *vmaf, VmafCudaState *cu_state);

/**
 * @enum  VmafCudaPicturePreallocationMethod
 * @brief Storage tier used by @ref vmaf_cuda_preallocate_pictures.
 *
 * Picks where the pool's per-picture sample buffers live, trading H2D copy
 * cost against host-visibility cost:
 *
 *   - `NONE`        — no preallocation; allocator falls back to per-frame
 *                     malloc + cudaMalloc. Useful for diagnostic builds only.
 *   - `DEVICE`      — `cudaMalloc` on the active device. Lowest H2D transfer
 *                     cost per frame (caller copies directly), highest
 *                     latency for host-visible debug paths.
 *   - `HOST`        — pageable host memory (plain `malloc`). Cheapest to
 *                     allocate; H2D copy uses a bounce buffer internally.
 *   - `HOST_PINNED` — `cudaMallocHost` page-locked memory. Best when the host
 *                     is the source of frame data (file decode + upload),
 *                     since the DMA engine can stream directly without a
 *                     pageable bounce.
 *
 * Stable enumerator values — append-only across libvmaf releases.
 */
enum VmafCudaPicturePreallocationMethod {
    VMAF_CUDA_PICTURE_PREALLOCATION_METHOD_NONE = 0,
    VMAF_CUDA_PICTURE_PREALLOCATION_METHOD_DEVICE,
    VMAF_CUDA_PICTURE_PREALLOCATION_METHOD_HOST,
    VMAF_CUDA_PICTURE_PREALLOCATION_METHOD_HOST_PINNED,
};

/**
 * @struct VmafCudaPictureConfiguration
 * @brief  Picture-pool configuration for @ref vmaf_cuda_preallocate_pictures.
 *
 * CUDA equivalent of @ref VmafPictureConfiguration — adds the storage-tier
 * selector so the caller controls whether the pool lives on the device or in
 * (pinned) host memory.
 *
 * @field pic_params           Per-picture geometry shared by every pool slot.
 * @field pic_params.w         Luma width in samples.
 * @field pic_params.h         Luma height in samples.
 * @field pic_params.bpc       Bits per component (8, 10, 12, or 16).
 * @field pic_params.pix_fmt   Planar pixel format (see @ref VmafPixelFormat).
 * @field pic_prealloc_method  Storage tier — see
 *                             @ref VmafCudaPicturePreallocationMethod.
 */
typedef struct VmafCudaPictureConfiguration {
    /** Per-picture shape (width/height/bpc/pixel-format). */
    struct {
        unsigned w, h;                /**< Per-plane width / height. */
        unsigned bpc;                 /**< Bits per component. */
        enum VmafPixelFormat pix_fmt; /**< Pixel format. */
    } pic_params;
    /** Selector for how each VmafPicture's data buffers are placed. */
    enum VmafCudaPicturePreallocationMethod pic_prealloc_method;
} VmafCudaPictureConfiguration;

/**
 * Config and preallocate VmafPictures for use during CUDA feature extraction.
 * The preallocated VmafPicture data buffers are set according to
 * cfg.pic_prealloc_method.
 *
 * @param vmaf VMAF context allocated with `vmaf_init()`.
 *
 * @param cfg VmafPicture parameter configuration.
 *
 * @return 0 on success, or < 0 (a negative errno code) on error.
 *
 * @thread-safety Not thread-safe. Call before vmaf_read_pictures() on the
 *               same context.
 */
VMAF_EXPORT int vmaf_cuda_preallocate_pictures(VmafContext *vmaf, VmafCudaPictureConfiguration cfg);

/**
 * Fetch a preallocated VmafPicture for use during CUDA feature extraction.
 * pictures are allocated during `vmaf_cuda_preallocate_pictures` and data
 * buffers are set according to cfg.pic_prealloc_method.
 *
 * @param vmaf VMAF context allocated with `vmaf_init()` and
 *             initialized with `vmaf_cuda_preallocate_pictures()`.
 *
 * @param pic Preallocated picture. Must not be NULL.
 *
 * @return 0 on success, or < 0 (a negative errno code) on error.
 *
 * @thread-safety Not thread-safe. Use one VmafContext per driver thread.
 */
VMAF_EXPORT int vmaf_cuda_fetch_preallocated_picture(VmafContext *vmaf, VmafPicture *pic);

#ifdef __cplusplus
}
#endif

#endif /* LIBVMAF_LIBVMAF_CUDA_H */
