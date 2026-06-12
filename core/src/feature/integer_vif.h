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

#ifndef FEATURE_VIF_H_
#define FEATURE_VIF_H_

#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include "cpu.h"

/* Enhancement gain imposed on vif, must be >= 1.0, where 1.0 means the gain is completely disabled */
#ifndef DEFAULT_VIF_ENHN_GAIN_LIMIT
#define DEFAULT_VIF_ENHN_GAIN_LIMIT (100.0)
#endif // !DEFAULT_VIF_ENHN_GAIN_LIMIT

/*
 * log2 LUT shrink (ADR-0500): after the clz-based normalisation in log2_32 / log2_64
* ADR-0500
 * the mantissa is always in [32768..65535] — bit 15 is always 1.  Stripping bit 15
 * (masking with 0x7FFF) gives a 15-bit index in [0..32767], so the table needs only
 * 32768 entries (64 KB) instead of the original 128 KB.  This halves L2 pressure and
 * TLB coverage on the hot vif_statistic_avx512 gather path.
 * Bit-exactness is preserved: same uint16 values, same arithmetic.
 */
#define VIF_LOG2_TABLE_SIZE 32768u
#define VIF_LOG2_TABLE_OFFSET 0x8000u

static const uint16_t vif_filter1d_table[4][18] = {
    {489, 935, 1640, 2640, 3896, 5274, 6547, 7455, 7784, 7455, 6547, 5274, 3896, 2640, 1640, 935,
     489, 0},
    {1244, 3663, 7925, 12590, 14692, 12590, 7925, 3663, 1244, 0},
    {3571, 16004, 26386, 16004, 3571, 0},
    {10904, 43728, 10904, 0}};

static const int vif_filter1d_width[4] = {17, 9, 5, 3};

typedef struct VifBuffer {
    void *data;

    void *ref;
    void *dis;
    uint16_t *mu1;
    uint16_t *mu2;
    uint32_t *mu1_32;
    uint32_t *mu2_32;
    uint32_t *ref_sq;
    uint32_t *dis_sq;
    uint32_t *ref_dis;

    struct {
        uint32_t *mu1;
        uint32_t *mu2;
        uint32_t *ref;
        uint32_t *dis;
        uint32_t *ref_dis;
        uint32_t *ref_convol;
        uint32_t *dis_convol;
    } tmp;

    ptrdiff_t stride;
    ptrdiff_t stride_16;
    ptrdiff_t stride_32;
    ptrdiff_t stride_tmp;
} VifBuffer;

typedef struct VifResiduals {
    int64_t accum_num_log;
    int64_t accum_den_log;
    int64_t accum_num_non_log;
    int64_t accum_den_non_log;
} VifResiduals;

typedef struct VifPublicState {
    VifBuffer buf;
    uint16_t log2_table[VIF_LOG2_TABLE_SIZE];
    double vif_enhn_gain_limit;
} VifPublicState;

static inline void PADDING_SQ_DATA(const VifBuffer *buf, int w, unsigned fwidth_half)
{
    for (unsigned f = 1; f <= fwidth_half; ++f) {
        int left_point = -(int)f;
        int right_point = f;
        buf->tmp.mu1[left_point] = buf->tmp.mu1[right_point];
        buf->tmp.mu2[left_point] = buf->tmp.mu2[right_point];
        buf->tmp.ref[left_point] = buf->tmp.ref[right_point];
        buf->tmp.dis[left_point] = buf->tmp.dis[right_point];
        buf->tmp.ref_dis[left_point] = buf->tmp.ref_dis[right_point];

        left_point = w - 1 - f;
        right_point = w - 1 + f;
        buf->tmp.mu1[right_point] = buf->tmp.mu1[left_point];
        buf->tmp.mu2[right_point] = buf->tmp.mu2[left_point];
        buf->tmp.ref[right_point] = buf->tmp.ref[left_point];
        buf->tmp.dis[right_point] = buf->tmp.dis[left_point];
        buf->tmp.ref_dis[right_point] = buf->tmp.ref_dis[left_point];
    }
}

static inline void PADDING_SQ_DATA_2(const VifBuffer *buf, int w, unsigned fwidth_half)
{
    for (unsigned f = 1; f <= fwidth_half; ++f) {
        int left_point = -(int)f;
        int right_point = f;
        buf->tmp.ref_convol[left_point] = buf->tmp.ref_convol[right_point];
        buf->tmp.dis_convol[left_point] = buf->tmp.dis_convol[right_point];

        left_point = w - 1 - f;
        right_point = w - 1 + f;
        buf->tmp.ref_convol[right_point] = buf->tmp.ref_convol[left_point];
        buf->tmp.dis_convol[right_point] = buf->tmp.dis_convol[left_point];
    }
}

void vif_statistic_8(struct VifPublicState *s, float *num, float *den, unsigned w, unsigned h);
void vif_statistic_16(struct VifPublicState *s, float *num, float *den, unsigned w, unsigned h,
                      int bpc, int scale);

/*
 * Compute vif residuals on a vertically filtered line
 * This is a support method for block based vip_statistic_xxx method and is typically called
 * only when to is not a multiple of the block size, with from = (to / block_size) + block_size
 */
VifResiduals vif_compute_line_residuals(VifPublicState *s, unsigned from, unsigned to, int scale);

#include "compat_builtin.h"

static inline int32_t log2_32(const uint16_t *log2_table, uint32_t temp)
{
    int k = __builtin_clz(temp);
    k = 16 - k;
    temp = temp >> k;
    /* After normalization temp is in [32768..65535]; strip the MSB to get the
     * 15-bit index into the compact VIF_LOG2_TABLE_SIZE-entry table. */
    return log2_table[temp & (VIF_LOG2_TABLE_SIZE - 1u)] + 2048 * k;
}

static inline int32_t log2_64(const uint16_t *log2_table, uint64_t temp)
{
    assert(temp >= 0x20000);
    int k = __builtin_clzll(temp);
    k = 48 - k;
    temp = temp >> k;
    /* After normalization temp is in [32768..65535]; strip the MSB to get the
     * 15-bit index into the compact VIF_LOG2_TABLE_SIZE-entry table. */
    return log2_table[temp & (VIF_LOG2_TABLE_SIZE - 1u)] + 2048 * k;
}

#endif /* _FEATURE_VIF_H_ */
