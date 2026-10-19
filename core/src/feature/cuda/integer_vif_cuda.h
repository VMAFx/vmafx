/**
 *
 *  Copyright 2016-2023 Netflix, Inc.
 *  Copyright 2021 NVIDIA Corporation.
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
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

#include <stdbool.h>
#include <stdint.h>
#include "integer_vif.h"
#include "common.h"

/* Enhancement gain imposed on vif, must be >= 1.0, where 1.0 means the gain is completely disabled */
#ifndef DEFAULT_VIF_ENHN_GAIN_LIMIT
#define DEFAULT_VIF_ENHN_GAIN_LIMIT (100.0)
#endif // !DEFAULT_VIF_ENHN_GAIN_LIMIT

/* NOLINTBEGIN(modernize-use-using): C header included by C and C++ translation units; C has no using. ADR-1138. */
typedef struct VifBufferCuda {
    VmafCudaState cu_state;

    VmafCudaBuffer *data;
    VmafCudaBuffer *accum_data;

    CUdeviceptr ref;
    CUdeviceptr dis;
    uint16_t *mu1;
    uint16_t *mu2;
    uint32_t *mu1_32;
    uint32_t *mu2_32;
    uint32_t *ref_sq;
    uint32_t *dis_sq;
    uint32_t *ref_dis;
    int64_t *accum;
    void *accum_host;
    void *cpu_param_buf;
    struct {
        uint32_t *mu1;
        uint32_t *mu2;
        uint32_t *ref;
        uint32_t *dis;
        uint32_t *ref_dis;
        uint32_t *ref_convol;
        uint32_t *dis_convol;
        uint32_t *padding;
    } tmp;

    /* Row pitches (bytes) of the reference and distorted pictures scale 0
     * reads, each the picture's own stride[0] (set per frame: a picture's
     * pitch is its allocator's, an imported plane's its producer's). */
    ptrdiff_t stride;
    ptrdiff_t dis_stride;
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
/* NOLINTEND(modernize-use-using) */

extern const unsigned char filter1d_ptx[];

#ifndef DEVICE_CODE
/**
 * Copy the log2 table of a loaded filter1d module to or from a device buffer
 * of VIF_LOG2_TABLE_SIZE uint16 values, through the module's
 * `vif_cuda_log2_table_transfer` kernel, and wait for it.
 *
 * @param to_module  true: staging -> module (what init does); false: module
 *                   -> staging (a test reads back what the kernels see).
 *
 * @return 0 on success, or < 0 (a negative errno code) on error.
 */
int vmaf_cuda_vif_log2_table_transfer(VmafCudaState *cu_state, CUmodule module,
                                      VmafCudaBuffer *staging, bool to_module);

/**
 * Fill the log2 table of a loaded filter1d module with the CPU extractor's
 * values, vif_log2_table_generate() (ADR-1462). The statistic kernels read
 * every logarithm from that table, so this runs once per module load, before
 * the first frame.
 *
 * @return 0 on success, or < 0 (a negative errno code) on error.
 */
int vmaf_cuda_vif_upload_log2_table(VmafCudaState *cu_state, CUmodule module);
#endif /* DEVICE_CODE */

#endif /* _FEATURE_VIF_CUDA_H_ */
