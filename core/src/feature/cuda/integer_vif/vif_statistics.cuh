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

#include "cuda_device_ptr.cuh"
#include "cuda_helper.cuh"
#include "cuda/integer_vif_cuda.h"

#include "common.h"

/* integer_vif.c's residual variance runs in device code here: one definition,
 * x86's value without the undefined conversion below INT32_MIN (ADR-1561). */
#define VMAF_IVIF_FUNC static __device__ __forceinline__
#include "feature/integer_vif_sv_sq.h"

/*
 * The CPU's log2 table (vif_log2_table.h::vif_log2_table_generate()), a
 * global of this module. The host fills it once at init, before the first
 * frame (vmaf_cuda_vif_upload_log2_table() in integer_vif_cuda.c), with the
 * values its math library gives the CPU extractor. The statistic reads its
 * logarithms from it and evaluates none: a device's log2f() need not round
 * as the host's does (ADR-1435), so equality with the CPU must not depend on
 * which device library and which host library meet (ADR-1462).
 *
 * A module global and not a kernel argument, so that the upstream kernels of
 * filter1d.cu keep their signatures and that file stays as upstream wrote it.
 */
extern "C" {
__device__ uint16_t vif_cuda_log2_table[VIF_LOG2_TABLE_SIZE];

/*
 * Copy the table between the module's global and a device buffer of
 * VIF_LOG2_TABLE_SIZE uint16 values: into the global when `to_module` is not
 * 0 (the extractor's init), out of it when it is 0 (test_cuda_vif_log2_table
 * reads back what the kernels see). A kernel, because the CUDA loader this
 * project uses binds the legacy cuModuleGetGlobal(), which a current-API
 * context refuses.
 */
__global__ void vif_cuda_log2_table_transfer(VmafCudaBuffer staging, unsigned to_module)
{
    const unsigned i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= VIF_LOG2_TABLE_SIZE) {
        return;
    }
    uint16_t *staged = VMAF_CUDA_DPTR(uint16_t, staging.data);
    if (to_module != 0u) {
        vif_cuda_log2_table[i] = staged[i];
    } else {
        staged[i] = vif_cuda_log2_table[i];
    }
}
}

namespace
{

__device__ __forceinline__ uint16_t get_best16_from32(uint32_t temp, int *x)
{
    int k = __clz(temp);
    k = 16 - k;
    temp = temp >> (unsigned)k;
    *x = -k;
    return temp;
}

__device__ __forceinline__ uint16_t get_best16_from64(uint64_t temp, int *x)
{
    int k = __clzll(temp);
    if (k > 48) {
        k -= 48;
        temp = temp << (unsigned)k;
        *x = k;
    } else if (k < 47) {
        k = 48 - k;
        temp = temp >> (unsigned)k;
        *x = -k;
    } else {
        *x = 0;
        if ((temp >> 16u) != 0u) {
            temp = temp >> 1u;
            *x = -1;
        }
    }
    return (uint16_t)temp;
}

/* log2_table[v & (VIF_LOG2_TABLE_SIZE - 1)] for a normalised mantissa `v`,
 * 32768 to 65535, masked as log2_32() / log2_64() of integer_vif.h mask. */
__device__ __forceinline__ uint16_t log2_lookup(uint16_t v)
{
    return vif_cuda_log2_table[v & (VIF_LOG2_TABLE_SIZE - 1u)];
}

/* float equivalent of 2. (2 * 65536) */
constexpr int32_t VIF_SIGMA_NSQ = (int32_t)(65536u << 1u);

/* The three variance terms of one pixel after the integer rounding. */
struct VifSigmas {
    int32_t sigma1_sq;
    int32_t sigma2_sq;
    int32_t sigma12;
};

/* (a * b + 2^31) >> 32: a product of two 32-bit moments rounded to 32 bits. */
__device__ __forceinline__ uint32_t vif_round_product(uint32_t a, uint32_t b)
{
    return (uint32_t)((((uint64_t)a * b) + 2147483648) >> 32u);
}

__device__ __forceinline__ VifSigmas vif_sigmas(uint32_t mu1, uint32_t mu2, uint32_t xx_filt,
                                                uint32_t yy_filt, uint32_t xy_filt)
{
    const uint32_t mu1_sq_val = vif_round_product(mu1, mu1);
    const uint32_t mu2_sq_val = vif_round_product(mu2, mu2);
    const uint32_t mu1_mu2_val = vif_round_product(mu1, mu2);

    const int32_t sigma1_sq = (int32_t)(xx_filt - mu1_sq_val);
    const int32_t sigma2_sq = (int32_t)(yy_filt - mu2_sq_val);
    const int32_t sigma12 = (int32_t)(xy_filt - mu1_mu2_val);

    VifSigmas sg;
    sg.sigma1_sq = max(sigma1_sq, 0);
    sg.sigma2_sq = max(sigma2_sq, 0);
    sg.sigma12 = sigma12;
    return sg;
}

/* The gain g = sigma12 / sigma1_sq of the VIF model; 0 unless both variances
 * are non-zero and the covariance is positive. */
__device__ __forceinline__ double vif_gain(const VifSigmas &sg)
{
    // eps is zero, an int will not be less then 1.0e-10, it can be
    // changed to one
    const double eps = 65536 * 1.0e-10;
    double g = 0.0;

    // if sigma1_sq > 0 then sigma1_sq >= 1 and thus greater eps => only
    // the case sigma1_sq == 0 matters

    // as g can only be < 0 if sigma12 is < 0 we can also check for that
    const double tmp = sg.sigma12 / (sg.sigma1_sq + eps);
    if (sg.sigma12 > 0 && sg.sigma1_sq != 0 && sg.sigma2_sq != 0) {
        g = tmp;
    }
    return g;
}

/* The pixel's contribution when sigma1_sq >= sigma_nsq: the log-domain
 * numerator and denominator. */
__device__ __forceinline__ void vif_accumulate_log(const VifSigmas &sg, double g, uint32_t sv_sq,
                                                   vif_accums &thread_accum)
{
    const uint32_t log_den_stage1 = (uint32_t)VIF_SIGMA_NSQ + (uint32_t)sg.sigma1_sq;
    int x;
    const uint16_t log_den1 = get_best16_from32(log_den_stage1, &x);

    /**
     * log values are taken from the look-up table the host generates
     * (vif_log2_table_generate()) and uploads; den_val in float is log2(1 +
     * sigma1_sq/2) here it is converted to equivalent of
     * log2(2+sigma1_sq) - log2(2) i.e log2(2*65536+sigma1_sq) - 17
     * multiplied by 2048 as log_value = log2(i)*2048 i=16384 to 65535
     * generated using log_value x because best 16 bits are taken
     */
    thread_accum.num_x++;
    thread_accum.x += x;
    const int64_t den_val = log2_lookup(log_den1);

    int64_t num_val = 0;
    if (sg.sigma12 >= 0) {
        // num_val = log2f(1.0f + (g * g * sigma1_sq) / (sv_sq +
        // sigma_nsq));
        /**
         * In floating-point numerator = log2((1.0f + (g * g *
         * sigma1_sq)/(sv_sq + sigma_nsq))
         *
         * In Fixed-point the above is converted to
         * numerator = log2((sv_sq + sigma_nsq)+(g * g * sigma1_sq))-
         * log2(sv_sq + sigma_nsq)
         */
        int x1;
        int x2;
        const uint32_t numer1 = (sv_sq + VIF_SIGMA_NSQ);
        const int64_t numer1_tmp = (int64_t)((g * g * sg.sigma1_sq)) + numer1; // numerator
        const uint16_t numlog = get_best16_from64((uint64_t)numer1_tmp, &x1);

        // we do not check against numer1 > 0 as sv_sq >= and sigma_nsq >
        // 0 and therefore the sum is > 0
        const uint16_t denlog = get_best16_from64((uint64_t)numer1, &x2);
        thread_accum.x2 += (x2 - x1);
        num_val = log2_lookup(numlog) - log2_lookup(denlog);
    }
    thread_accum.num_log += num_val;
    thread_accum.den_log += den_val;
}

/* One pixel's VIF statistics, added to the thread's accumulators. */
__device__ __forceinline__ void vif_statistic_pixel(uint32_t mu1, uint32_t mu2, uint32_t xx_filt,
                                                    uint32_t yy_filt, uint32_t xy_filt,
                                                    double vif_enhn_gain_limit,
                                                    vif_accums &thread_accum)
{
    const VifSigmas sg = vif_sigmas(mu1, mu2, xx_filt, yy_filt, xy_filt);
    double g = vif_gain(sg);
    const int32_t sigma2_sq = sg.sigma2_sq;
    const int32_t sigma12 = sg.sigma12;
    const uint32_t sv_sq = vif_sv_sq(sigma2_sq, g, sigma12);

    g = min(g, vif_enhn_gain_limit);

    if (sg.sigma1_sq >= VIF_SIGMA_NSQ) {
        vif_accumulate_log(sg, g, sv_sq, thread_accum);
    } else {
        thread_accum.num_non_log += sg.sigma2_sq;
        thread_accum.den_non_log += 1;
    }
}

template <typename aligned_dtype = uint4>
__device__ __forceinline__ void
vif_statistic_calculation(const aligned_dtype &mu1, const aligned_dtype &mu2,
                          const aligned_dtype &xx_filt, const aligned_dtype &yy_filt,
                          const aligned_dtype &xy_filt, int cur_col, int w,
                          double vif_enhn_gain_limit, vif_accums &thread_accum)
{
    const uint32_t *mu1_val = reinterpret_cast<const uint32_t *>(&mu1);
    const uint32_t *mu2_val = reinterpret_cast<const uint32_t *>(&mu2);
    const uint32_t *xx_filt_val = reinterpret_cast<const uint32_t *>(&xx_filt);
    const uint32_t *yy_filt_val = reinterpret_cast<const uint32_t *>(&yy_filt);
    const uint32_t *xy_filt_val = reinterpret_cast<const uint32_t *>(&xy_filt);

    constexpr int aligned_dtype_values = sizeof(aligned_dtype) / sizeof(int32_t);
    // calculate thread relative sums for all preloaded values
    for (int v = 0; v < aligned_dtype_values; ++v) {
        if (cur_col + v < w) {
            vif_statistic_pixel(mu1_val[v], mu2_val[v], xx_filt_val[v], yy_filt_val[v],
                                xy_filt_val[v], vif_enhn_gain_limit, thread_accum);
        }
    }
}

} /* namespace */
