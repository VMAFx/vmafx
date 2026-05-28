/**
 *
 *  Copyright 2016-2023 Netflix, Inc.
 *  Copyright 2021 NVIDIA Corporation.
 *  Copyright 2026 Lusoris and Claude (Anthropic)
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
 * Backend-agnostic GPU picture pool.
 *
 * History: this was originally `cuda/ring_buffer.{c,h}`, the CUDA-specific
 * VmafPicture round-robin pool. The pool's *shape* (config + alloc / sync /
 * free callbacks + cookie + pthread mutex round-robin) was always
 * backend-agnostic — only the include directory, the `VmafRingBuffer*`
 * names, and the `cuda/` location implied otherwise. ADR-0239 promotes
 * the file out of `cuda/` and renames the symbols so SYCL and Vulkan can
 * stop hand-rolling the same pool.
 *
 * Each backend supplies:
 *   - `pic_cnt` slots
 *   - `alloc_picture_callback`   — populate `pic[i]->data[]` (host/device)
 *   - `synchronize_picture_callback` (optional) — wait for the slot's
 *     prior GPU work to drain before vending it
 *   - `free_picture_callback`    — release the per-slot GPU resources
 *   - `cookie`                    — a per-backend pointer the callbacks
 *     read for VkInstance / VkDevice / SYCL queue / CUstream / etc.
 */

#ifndef VMAF_SRC_GPU_PICTURE_POOL_H_
#define VMAF_SRC_GPU_PICTURE_POOL_H_

#include "picture.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VmafGpuPicturePoolConfig {
    unsigned pic_cnt;
    int (*alloc_picture_callback)(VmafPicture *pic, void *cookie);
    int (*synchronize_picture_callback)(VmafPicture *pic, void *cookie);
    int (*free_picture_callback)(VmafPicture *pic, void *cookie);
    void *cookie;
} VmafGpuPicturePoolConfig;

typedef struct VmafGpuPicturePool VmafGpuPicturePool;

int vmaf_gpu_picture_pool_init(VmafGpuPicturePool **pool, VmafGpuPicturePoolConfig cfg);

int vmaf_gpu_picture_pool_close(VmafGpuPicturePool *pool);

/* Returns the next slot in round-robin order. The synchronize callback
 * fires before the slot is vended, so callers can drain prior GPU work
 * without a separate wait. */
int vmaf_gpu_picture_pool_fetch(VmafGpuPicturePool *pool, VmafPicture *pic);

#ifdef __cplusplus
}
#endif

#endif /* VMAF_SRC_GPU_PICTURE_POOL_H_ */
