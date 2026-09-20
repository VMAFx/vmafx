/**
 *
 *  Copyright 2016-2023 Netflix, Inc.
 *  Copyright 2021 NVIDIA Corporation.
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

#ifndef FEATURE_VIF_CUDA_H_
#define FEATURE_VIF_CUDA_H_

#include <stdint.h>
#include "integer_vif.h"
#include "common.h"

/* Enhancement gain imposed on vif, must be >= 1.0, where 1.0 means the gain is completely disabled */
#ifndef DEFAULT_VIF_ENHN_GAIN_LIMIT
#define DEFAULT_VIF_ENHN_GAIN_LIMIT (100.0)
#endif // !DEFAULT_VIF_ENHN_GAIN_LIMIT

/* CUDA kernels dereference these fields, while the Driver API host side
 * stores the same 64-bit addresses as CUdeviceptr values.  The two views are
 * layout-identical and avoid treating a device address as a host pointer. */
#if defined(__CUDACC__)
typedef uint16_t *VifCudaU16Ptr;
typedef uint32_t *VifCudaU32Ptr;
typedef int64_t *VifCudaI64Ptr;
#else
typedef CUdeviceptr VifCudaU16Ptr;
typedef CUdeviceptr VifCudaU32Ptr;
typedef CUdeviceptr VifCudaI64Ptr;
#endif

typedef struct VifBufferCuda {
    VmafCudaState cu_state;

    VmafCudaBuffer *data;
    VmafCudaBuffer *accum_data;

    CUdeviceptr ref;
    CUdeviceptr dis;
    VifCudaU16Ptr mu1;
    VifCudaU16Ptr mu2;
    VifCudaU32Ptr mu1_32;
    VifCudaU32Ptr mu2_32;
    VifCudaU32Ptr ref_sq;
    VifCudaU32Ptr dis_sq;
    VifCudaU32Ptr ref_dis;
    VifCudaI64Ptr accum;
    void *accum_host;
    void *cpu_param_buf;
    struct {
        VifCudaU32Ptr mu1;
        VifCudaU32Ptr mu2;
        VifCudaU32Ptr ref;
        VifCudaU32Ptr dis;
        VifCudaU32Ptr ref_dis;
        VifCudaU32Ptr ref_convol;
        VifCudaU32Ptr dis_convol;
        VifCudaU32Ptr padding;
    } tmp;

    ptrdiff_t stride;
    ptrdiff_t rd_stride; /* stride (bytes) for half-res ref/dis downsampled buffers */
    ptrdiff_t stride_16;
    ptrdiff_t stride_32;
    ptrdiff_t stride_64;
    ptrdiff_t stride_tmp;
} VifBufferCuda;

typedef struct filter_table_stuct {
    uint16_t filter[4][18];
} filter_table_stuct;

typedef struct filter_width_struct {
    int w[4];
} filter_width_struct;

typedef struct vif_accums {
    int64_t x;
    int64_t x2;
    int64_t num_x;
    int64_t num_log;
    int64_t den_log;
    int64_t num_non_log;
    int64_t den_non_log;
} vif_accums;

extern const unsigned char filter1d_ptx[];

#endif /* _FEATURE_VIF_CUDA_H_ */
