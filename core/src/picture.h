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

#ifndef VMAF_SRC_PICTURE_INCLUDED
#define VMAF_SRC_PICTURE_INCLUDED

#ifdef HAVE_CUDA
#ifdef DEVICE_CODE
#include <cuda.h>
typedef struct VmafCudaState VmafCudaState;
#else
#include <ffnvcodec/dynlink_cuda.h>
#include "libvmaf/libvmaf_cuda.h"
#endif
#endif
#include "libvmaf/picture.h"

enum VmafPictureBufferType {
    VMAF_PICTURE_BUFFER_TYPE_HOST = 0,
    VMAF_PICTURE_BUFFER_TYPE_CUDA_HOST_PINNED,
    VMAF_PICTURE_BUFFER_TYPE_CUDA_DEVICE,
    VMAF_PICTURE_BUFFER_TYPE_SYCL_DEVICE,
    /* ADR-0726: Vulkan backend removed. Enum value deleted — no source file
     * referenced VMAF_PICTURE_BUFFER_TYPE_VULKAN_DEVICE after ADR-0726.
     * Any future Vulkan revival must use a new ADR and a new value. */
    /* ADR-0530 / ADR-0613: HIP-backed picture pool (hipMalloc).
     * picture_hip.{c,h} is fully implemented as of ADR-0613: the
     * previous -ENOSYS stub was replaced with a real hipMalloc /
     * hipFree allocation path.  HIP pictures are now allocated on
     * the device and no longer arrive as VMAF_PICTURE_BUFFER_TYPE_HOST
     * with a caller-side HtoD copy.  The tag allows the dispatch check
     * in feature_extractor.c to reject mixed backings
     * (e.g. CUDA-buffer-into-HIP-extractor). */
    VMAF_PICTURE_BUFFER_TYPE_HIP_DEVICE,
};

typedef struct VmafPicturePrivate {
    void *cookie;
    int (*release_picture)(VmafPicture *pic, void *cookie);
#ifdef HAVE_CUDA
    struct {
        CUcontext ctx;
        CUevent ready, finished;
        CUstream str;
        VmafCudaState *state;
    } cuda;
#endif
    enum VmafPictureBufferType buf_type;
} VmafPicturePrivate;

#ifdef __cplusplus
extern "C" {
#endif

int vmaf_picture_priv_init(VmafPicture *pic);

int vmaf_picture_ref(VmafPicture *dst, VmafPicture *src);

/* Drain all picture-buffer pool entries, freeing each via aligned_free().
 * Call at session teardown or from unit tests to prevent LSan
 * false-positive reports from pooled buffers held in the global pic_pool. */
void vmaf_picture_pool_flush(void);

int vmaf_picture_set_release_callback(VmafPicture *pic, void *cookie,
                                      int (*release_picture)(VmafPicture *pic, void *cookie));

#ifdef __cplusplus
}
#endif

#endif /* VMAF_SRC_PICTURE_INCLUDED */
