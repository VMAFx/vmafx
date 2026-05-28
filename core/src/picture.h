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
    /* ADR-0530: HIP-backed picture pool (hipMalloc / hipMallocAsync).
     * Reserved tag for the future HIP device picture pool — currently
     * the HIP backend's picture_hip.{c,h} is a stub and pictures
     * arrive as VMAF_PICTURE_BUFFER_TYPE_HOST, with the HIP feature
     * extractors performing their own HtoD copy. The tag exists so
     * the dispatch check at feature_extractor.c can refuse a mixed
     * (e.g. CUDA-buffer-into-HIP-extractor) backing once a real HIP
     * picture pool lands, and so callers using a HIP picture pool
     * outside of libvmaf's tree can already mark their pictures
     * correctly. */
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

int vmaf_picture_priv_init(VmafPicture *pic);

int vmaf_picture_ref(VmafPicture *dst, VmafPicture *src);

int vmaf_picture_set_release_callback(VmafPicture *pic, void *cookie,
                                      int (*release_picture)(VmafPicture *pic, void *cookie));

#endif /* VMAF_SRC_PICTURE_INCLUDED */
