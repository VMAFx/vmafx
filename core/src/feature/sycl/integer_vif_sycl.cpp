/**
 *
 *  Copyright 2016-2026 Netflix, Inc.
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
 * SYCL/DPC++ VIF (Visual Information Fidelity) feature extractor.
 *
 * Implements 4-scale separable Gaussian filtering using SYCL kernels.
 * Based on separable VIF algorithm (filter1d_vert + filter1d_hori).
 *
 * Algorithm:
 *   - For each of 4 scales, apply a separable symmetric Gaussian filter:
 *     1. Vertical pass: filter ref/dis, compute mu, sigma^2, cross-products
 *        Simultaneously compute reduction-filtered data for next scale
 *     2. Horizontal pass: complete the 2D convolution, compute VIF statistics
 *        Downsample result into rd_ref/rd_dis for next scale
 *   - Accumulate 7 int64 statistics per scale on the GPU
 *   - Download and compute final VIF scores on the CPU
 *
 * Pattern: init -> submit (non-blocking GPU work) -> collect (wait + scores)
 * Uses shared frame buffers (ref/dis Y planes uploaded once per frame).
 */

#include <sycl/sycl.hpp>

#include "sycl_compat.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <utility>

#include "config.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature_name.h"
#include "sycl/common.h"
#include "log.h"

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

static constexpr int VIF_NUM_SCALES = 4;
static constexpr int VIF_FILTER_MAX_WIDTH = 17;
static constexpr int VIF_FILTER_TABLE_PAD = 18;

// Filter coefficients per scale (sum to 65536 = 2^16)
static constexpr uint32_t vif_filter1d_table[VIF_NUM_SCALES][VIF_FILTER_TABLE_PAD] = {
    {489, 935, 1640, 2640, 3896, 5274, 6547, 7455, 7784, 7455, 6547, 5274, 3896, 2640, 1640, 935,
     489, 0},
    {1244, 3663, 7925, 12590, 14692, 12590, 7925, 3663, 1244, 0},
    {3571, 16004, 26386, 16004, 3571, 0},
    {10904, 43728, 10904, 0},
};

static constexpr int vif_fwidth[VIF_NUM_SCALES] = {17, 9, 5, 3};
static constexpr int vif_fwidth_rd[VIF_NUM_SCALES] = {9, 5, 3, 0};

static constexpr int64_t SIGMA_NSQ = 131072; // 2 * 65536

static constexpr int LOG2_LUT_SIZE = 32768;

/* ------------------------------------------------------------------ */
/* Per-scale accumulator struct (7 x int64_t = 56 bytes)              */
/* ------------------------------------------------------------------ */

struct vif_accums {
    int64_t x;
    int64_t x2;
    int64_t num_x;
    int64_t num_log;
    int64_t den_log;
    int64_t num_non_log;
    int64_t den_non_log;
};
static constexpr int ACCUM_FIELDS = 7;

/* ------------------------------------------------------------------ */
/* Extractor private state                                             */
/* ------------------------------------------------------------------ */

struct VifStateSycl {
    unsigned width, height;
    unsigned bpc;

    bool debug;
    bool vif_skip_scale0;
    double vif_enhn_gain_limit;

    VmafDictionary *feature_name_dict;

    // SYCL device buffers
    uint32_t *d_tmp_mu1;
    uint32_t *d_tmp_mu2;
    uint32_t *d_tmp_ref;
    uint32_t *d_tmp_dis;
    uint32_t *d_tmp_ref_dis;
    uint32_t *d_tmp_ref_convol;
    uint32_t *d_tmp_dis_convol;
    uint32_t *d_rd_ref;
    uint32_t *d_rd_dis;
    int64_t *d_accum;     // 4 scales x 7 int64 = 224 bytes
    uint32_t *d_log2_lut; // 32768 entries

    // Host-side accumulator download buffer
    int64_t *h_accum;

    // Fused V+H kernel mode: uses SLM intermediates, skips tmp buffers.
    // Saves ~70 MB VRAM at 4K but may be slower on some GPUs due to
    // SLM pressure and reduced occupancy.
    bool use_fused;

    // Deferred submit/collect state
    unsigned pending_index;
    bool has_pending;
};

/* ------------------------------------------------------------------ */
/* Options                                                             */
/* ------------------------------------------------------------------ */

static const VmafOption options[] = {
    {
        .name = "debug",
        .help = "debug mode: enable additional output",
        .offset = offsetof(VifStateSycl, debug),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val = {.b = true},
    },
    {
        .name = "vif_enhn_gain_limit",
        .help = "enhancement gain imposed on VIF, must be >= 1.0, "
                "where 1.0 means the gain is unrestricted",
        .alias = "egl",
        .offset = offsetof(VifStateSycl, vif_enhn_gain_limit),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val = {.d = 100.0},
        .min = 1.0,
        .max = 100.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "vif_fused",
        .help = "use fused V+H kernel (saves VRAM, may be slower on some GPUs)",
        .offset = offsetof(VifStateSycl, use_fused),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val = {.b = false},
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "vif_skip_scale0",
        .help = "skip scale 0 (finest scale) VIF computation; "
                "score0 is forced to 0.0 (parity with CPU option)",
        .alias = "ssclz",
        .offset = offsetof(VifStateSycl, vif_skip_scale0),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val = {.b = false},
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {nullptr}};

/* ------------------------------------------------------------------ */
/* Device-side helpers (used inside kernels)                           */
/* ------------------------------------------------------------------ */

static inline int dev_mirror(int idx, int sup)
{
    if (idx < 0)
        return -idx;
    if (idx >= sup)
        return 2 * (sup - 1) - idx;
    return idx;
}

static inline uint32_t dev_get_best16_from32(uint32_t val, int &exp_out)
{
    if (val == 0) {
        exp_out = 0;
        return 0;
    }
    int msb = 31;
    while (msb >= 0 && !((val >> msb) & 1))
        msb--;
    int k = msb - 15;
    if (k < 0)
        k = 0;
    val >>= k;
    exp_out = -k;
    return val & 0xFFFF;
}

static inline uint32_t dev_get_best16_from64(uint64_t val, int &exp_out)
{
    if (val == 0) {
        exp_out = 0;
        return 0;
    }
    int clz = 0;
    uint64_t tmp = val;
    if (!(tmp >> 32)) {
        clz += 32;
        tmp <<= 32;
    }
    if (!(tmp >> 48)) {
        clz += 16;
        tmp <<= 16;
    }
    if (!(tmp >> 56)) {
        clz += 8;
        tmp <<= 8;
    }
    if (!(tmp >> 60)) {
        clz += 4;
        tmp <<= 4;
    }
    if (!(tmp >> 62)) {
        clz += 2;
        tmp <<= 2;
    }
    if (!(tmp >> 63)) {
        clz += 1;
    }

    int const k = clz;
    if (k > 48) {
        val <<= (k - 48);
        exp_out = k - 48;
    } else if (k < 47) {
        val >>= (48 - k);
        exp_out = -(48 - k);
    } else {
        exp_out = 0;
        if (val >> 16) {
            val >>= 1;
            exp_out = -1;
        }
    }
    return static_cast<uint32_t>(val) & 0xFFFF;
}

/* ------------------------------------------------------------------ */ /* Profiling helper                                                    */
/* ------------------------------------------------------------------ */

static inline void sycl_profile_event(VmafSyclState *state, const char *name, sycl::event ev)
{
    if (vmaf_sycl_profiling_is_enabled(state)) {
        ev.wait();
        uint64_t t0 = ev.get_profiling_info<sycl::info::event_profiling::command_start>();
        uint64_t t1 = ev.get_profiling_info<sycl::info::event_profiling::command_end>();
        vmaf_sycl_profiling_record(state, name, t1 - t0);
    }
}

/* ------------------------------------------------------------------ */ /* SYCL Kernel: Vertical Pass                                         */
/* ------------------------------------------------------------------ */

/**
 * Vertical 1D convolution for one scale.
 *
 * Uses shared local memory (SLM) tiling for data reuse: cooperatively loads
 * a tile of (WG_Y + fw - 1) × WG_X pixels into SLM, then each thread reads
 * from SLM instead of global memory. Adjacent output rows share fw-1 input
 * rows, so this reduces global memory reads by ~fw× vs naive per-thread loads.
 *
 * Interior workgroups (no mirror padding needed) use an optimized fast path
 * that skips boundary checks during tile load.
 */
template <int SCALE> struct VifVertParams {
    const void *ref_data;
    const void *dis_data;
    unsigned width;
    unsigned height;
    unsigned src_stride;
    unsigned bpc;
    unsigned shift;
    unsigned round;
    unsigned square_shift;
    unsigned square_round;
    uint32_t coeff[VIF_FILTER_MAX_WIDTH];
    uint32_t reduction_coeff[VIF_FILTER_MAX_WIDTH];
    uint32_t *output[7];
};

struct VifVertSums {
    uint32_t mu1;
    uint32_t mu2;
    uint64_t ref;
    uint64_t dis;
    uint64_t ref_dis;
    uint32_t ref_reduction;
    uint32_t dis_reduction;
};

template <int SCALE>
static inline uint32_t vif_vert_read(const VifVertParams<SCALE> &p, const void *src, int y, int x)
{
    if constexpr (SCALE == 0) {
        if (p.bpc <= 8)
            return static_cast<const uint8_t *>(src)[y * p.src_stride + x];
        return static_cast<const uint16_t *>(src)[y * (p.src_stride / 2) + x];
    }
    return static_cast<const uint32_t *>(src)[y * p.src_stride + x] & 0xFFFF;
}

template <int SCALE, typename Tile>
static inline void vif_vert_load_tile(const VifVertParams<SCALE> &p, sycl::nd_item<2> item,
                                      const Tile &ref_tile, const Tile &dis_tile)
{
    constexpr int fw = vif_fwidth[SCALE];
    constexpr int tile_height = 16 + fw - 1;
    constexpr unsigned tile_elements = tile_height * 16;
    unsigned const lane = item.get_local_linear_id();
    int const origin_y = (int)(item.get_group(0) * 16) - fw / 2;
    int const origin_x = (int)(item.get_group(1) * 16);
    bool const interior =
        origin_y >= 0 && origin_y + tile_height <= (int)p.height && origin_x + 16 <= (int)p.width;
    for (unsigned i = lane; i < tile_elements; i += 256) {
        unsigned const row = i / 16;
        unsigned const col = i % 16;
        int y = origin_y + (int)row;
        int x = origin_x + (int)col;
        if (!interior) {
            y = dev_mirror(y, (int)p.height);
            if (std::cmp_greater_equal(x, p.width)) {
                ref_tile[row][col] = 0;
                dis_tile[row][col] = 0;
                continue;
            }
            x = dev_mirror(x, (int)p.width);
        }
        ref_tile[row][col] = vif_vert_read(p, p.ref_data, y, x);
        dis_tile[row][col] = vif_vert_read(p, p.dis_data, y, x);
    }
}

template <int SCALE, typename Tile>
static inline VifVertSums vif_vert_accumulate(const VifVertParams<SCALE> &p, const Tile &ref_tile,
                                              const Tile &dis_tile, unsigned row, unsigned col)
{
    constexpr int fw = vif_fwidth[SCALE];
    constexpr int reduction_width = vif_fwidth_rd[SCALE];
    constexpr int reduction_start = (fw - reduction_width) / 2;
    VifVertSums sums = {};
#pragma unroll
    for (int tap = 0; tap < fw; tap++) {
        uint32_t const ref = ref_tile[row + tap][col];
        uint32_t const dis = dis_tile[row + tap][col];
        uint32_t const weighted_ref = p.coeff[tap] * ref;
        uint32_t const weighted_dis = p.coeff[tap] * dis;
        sums.mu1 += weighted_ref;
        sums.mu2 += weighted_dis;
        sums.ref += (uint64_t)weighted_ref * ref;
        sums.dis += (uint64_t)weighted_dis * dis;
        sums.ref_dis += (uint64_t)weighted_ref * dis;
        if constexpr (reduction_width > 0) {
            if (tap >= reduction_start && tap < reduction_start + reduction_width) {
                uint32_t const coeff = p.reduction_coeff[tap - reduction_start];
                sums.ref_reduction += coeff * ref;
                sums.dis_reduction += coeff * dis;
            }
        }
    }
    return sums;
}

template <int SCALE>
static inline void vif_vert_store(const VifVertParams<SCALE> &p, const VifVertSums &sums,
                                  unsigned index)
{
    uint32_t ref = (uint32_t)((sums.ref + p.square_round) >> p.square_shift);
    uint32_t dis = (uint32_t)((sums.dis + p.square_round) >> p.square_shift);
    uint32_t ref_dis = (uint32_t)((sums.ref_dis + p.square_round) >> p.square_shift);
    uint32_t ref_reduction = 0;
    uint32_t dis_reduction = 0;
    if constexpr (vif_fwidth_rd[SCALE] > 0) {
        ref_reduction = (sums.ref_reduction + p.round) >> p.shift;
        dis_reduction = (sums.dis_reduction + p.round) >> p.shift;
    }
    p.output[0][index] = (sums.mu1 + p.round) >> p.shift;
    p.output[1][index] = (sums.mu2 + p.round) >> p.shift;
    p.output[2][index] = ref;
    p.output[3][index] = dis;
    p.output[4][index] = ref_dis;
    p.output[5][index] = ref_reduction;
    p.output[6][index] = dis_reduction;
}

template <int SCALE>
static VifVertParams<SCALE> vif_vert_params(const void *ref_data, const void *dis_data,
                                            unsigned width, unsigned height, unsigned src_stride,
                                            unsigned bpc, uint32_t *const output[7])
{
    VifVertParams<SCALE> p = {};
    p.ref_data = ref_data;
    p.dis_data = dis_data;
    p.width = width;
    p.height = height;
    p.src_stride = src_stride;
    p.bpc = bpc;
    p.shift = SCALE == 0 ? bpc : 16;
    p.round = SCALE == 0 ? 1u << (bpc - 1) : 32768;
    p.square_shift = SCALE == 0 ? (bpc - 8) * 2 : 16;
    p.square_round = p.square_shift == 0 ? 0 : 1u << (p.square_shift - 1);
    for (int tap = 0; tap < vif_fwidth[SCALE]; tap++)
        p.coeff[tap] = vif_filter1d_table[SCALE][tap];
    if constexpr (vif_fwidth_rd[SCALE] > 0) {
        for (int tap = 0; tap < vif_fwidth_rd[SCALE]; tap++)
            p.reduction_coeff[tap] = vif_filter1d_table[SCALE + 1][tap];
    }
    for (int i = 0; i < 7; i++)
        p.output[i] = output[i];
    return p;
}

template <int SCALE>
static sycl::event launch_vif_vert_impl(sycl::queue &q, const void *ref_data, const void *dis_data,
                                        unsigned width, unsigned height, unsigned src_stride,
                                        unsigned bpc, uint32_t *tmp_mu1, uint32_t *tmp_mu2,
                                        uint32_t *tmp_ref, uint32_t *tmp_dis, uint32_t *tmp_ref_dis,
                                        uint32_t *tmp_ref_convol, uint32_t *tmp_dis_convol)
{
    uint32_t *output[7] = {tmp_mu1,     tmp_mu2,        tmp_ref,       tmp_dis,
                           tmp_ref_dis, tmp_ref_convol, tmp_dis_convol};
    VifVertParams<SCALE> const p =
        vif_vert_params<SCALE>(ref_data, dis_data, width, height, src_stride, bpc, output);
    constexpr int tile_height = 16 + vif_fwidth[SCALE] - 1;
    sycl::range<2> const global(((height + 15) / 16) * 16, ((width + 15) / 16) * 16);
    sycl::range<2> const local(16, 16);
    return q.submit([&](sycl::handler &cgh) {
        sycl::local_accessor<uint32_t, 2> const ref_tile(sycl::range<2>(tile_height, 16), cgh);
        sycl::local_accessor<uint32_t, 2> const dis_tile(sycl::range<2>(tile_height, 16), cgh);
        cgh.parallel_for(sycl::nd_range<2>(global, local), [=](sycl::nd_item<2> item) {
            vif_vert_load_tile(p, item, ref_tile, dis_tile);
            item.barrier(sycl::access::fence_space::local_space);
            unsigned const x = item.get_global_id(1);
            unsigned const y = item.get_global_id(0);
            if (x >= p.width || y >= p.height)
                return;
            VifVertSums const sums = vif_vert_accumulate(
                p, ref_tile, dis_tile, item.get_local_id(0), item.get_local_id(1));
            vif_vert_store(p, sums, y * p.width + x);
        });
    });
}

static sycl::event launch_vif_vert(sycl::queue &q, const void *ref_data, const void *dis_data,
                                   int scale, unsigned width, unsigned height, unsigned src_stride,
                                   unsigned bpc, uint32_t *tmp_mu1, uint32_t *tmp_mu2,
                                   uint32_t *tmp_ref, uint32_t *tmp_dis, uint32_t *tmp_ref_dis,
                                   uint32_t *tmp_ref_convol, uint32_t *tmp_dis_convol)
{
    switch (scale) {
    case 0:
        return launch_vif_vert_impl<0>(q, ref_data, dis_data, width, height, src_stride, bpc,
                                       tmp_mu1, tmp_mu2, tmp_ref, tmp_dis, tmp_ref_dis,
                                       tmp_ref_convol, tmp_dis_convol);
    case 1:
        return launch_vif_vert_impl<1>(q, ref_data, dis_data, width, height, src_stride, bpc,
                                       tmp_mu1, tmp_mu2, tmp_ref, tmp_dis, tmp_ref_dis,
                                       tmp_ref_convol, tmp_dis_convol);
    case 2:
        return launch_vif_vert_impl<2>(q, ref_data, dis_data, width, height, src_stride, bpc,
                                       tmp_mu1, tmp_mu2, tmp_ref, tmp_dis, tmp_ref_dis,
                                       tmp_ref_convol, tmp_dis_convol);
    default:
        return launch_vif_vert_impl<3>(q, ref_data, dis_data, width, height, src_stride, bpc,
                                       tmp_mu1, tmp_mu2, tmp_ref, tmp_dis, tmp_ref_dis,
                                       tmp_ref_convol, tmp_dis_convol);
    }
}

/* ------------------------------------------------------------------ */
/* SYCL Kernel: Horizontal Pass + VIF (subgroup-optimized, v2)        */
/* ------------------------------------------------------------------ */

/**
 * Optimized VIF horizontal kernel using two-phase reduction:
 *   Phase 1: Subgroup shuffle reduction (hardware-level, no barriers)
 *   Phase 2: Small local-memory tree across subgroup leaders
 *
 * With SIMD-16 subgroups, a 256-thread WG has 16 subgroups.
 * Phase 1 reduces 16→1 per subgroup (4 shuffle steps, no barriers).
 * Phase 2 reduces 16→1 across leaders (only 16 threads active).
 * Total barriers: 3 (vs 8 in the original tree reduction).
 *
 * SIMD-16 is required across the Intel target matrix. The previous SIMD-32
 * specialisations exhausted all 128 registers and spilled on Lunar Lake and
 * Battlemage; compiling only the spill-free SIMD-16 form also keeps the AOT
 * image honest with the runtime path selected on those devices.
 */
struct VifAccumTerms {
    int64_t value[ACCUM_FIELDS];
};

struct VifHorizontalSums {
    uint32_t mu1;
    uint32_t mu2;
    uint64_t ref;
    uint64_t dis;
    uint64_t ref_dis;
    uint32_t ref_reduction;
    uint32_t dis_reduction;
};

static inline VifAccumTerms vif_log_terms(int32_t sigma1, int32_t sigma2, int32_t sigma12,
                                          float gain_limit, const uint32_t *log2_lut)
{
    VifAccumTerms terms = {};
    float gain = 0.0f;
    float residual = 0.0f;
    float gain_energy = 0.0f;
    if (sigma12 > 0 && sigma1 != 0 && sigma2 != 0) {
        gain = (float)sigma12 / (float)sigma1;
        residual = (float)sigma2 - gain * (float)sigma12;
        if (residual < 0.0f)
            residual = 0.0f;
        gain = sycl::fmin(gain, gain_limit);
        gain_energy = gain * gain * (float)sigma1;
    }
    uint32_t const denominator_stage = (uint32_t)((int64_t)SIGMA_NSQ + sigma1);
    int denominator_exp = 0;
    uint32_t const denominator = dev_get_best16_from32(denominator_stage, denominator_exp);
    terms.value[0] = denominator_exp;
    terms.value[2] = 1;
    terms.value[4] = log2_lut[denominator - 32768];
    if (sigma12 >= 0) {
        uint32_t const residual_noise = (uint32_t)residual + (uint32_t)SIGMA_NSQ;
        uint64_t const numerator_stage = (uint64_t)(int64_t)gain_energy + (uint64_t)residual_noise;
        int numerator_exp = 0;
        int residual_exp = 0;
        uint32_t const numerator = dev_get_best16_from64(numerator_stage, numerator_exp);
        uint32_t const residual_denominator =
            dev_get_best16_from64((uint64_t)residual_noise, residual_exp);
        terms.value[1] = residual_exp - numerator_exp;
        terms.value[3] =
            (int32_t)log2_lut[numerator - 32768] - (int32_t)log2_lut[residual_denominator - 32768];
    }
    return terms;
}

static inline VifAccumTerms vif_pixel_terms(int32_t sigma1, int32_t sigma2, int32_t sigma12,
                                            float gain_limit, const uint32_t *log2_lut)
{
    if (sigma1 >= (int32_t)SIGMA_NSQ)
        return vif_log_terms(sigma1, sigma2, sigma12, gain_limit, log2_lut);
    VifAccumTerms terms = {};
    terms.value[5] = sigma2;
    terms.value[6] = 1;
    return terms;
}

template <int MAX_SUBGROUPS, typename LocalMemory>
static inline void vif_reduce_terms(sycl::nd_item<2> item, const LocalMemory &lmem, int64_t *accum,
                                    const VifAccumTerms &terms)
{
    sycl::sub_group const subgroup = item.get_sub_group();
    int64_t subgroup_values[ACCUM_FIELDS];
    for (int field = 0; field < ACCUM_FIELDS; field++) {
        subgroup_values[field] =
            sycl::reduce_over_group(subgroup, terms.value[field], sycl::plus<int64_t>());
    }
    uint32_t const subgroup_id = subgroup.get_group_linear_id();
    if (subgroup.get_local_linear_id() == 0) {
        for (int field = 0; field < ACCUM_FIELDS; field++)
            lmem[field * MAX_SUBGROUPS + subgroup_id] = subgroup_values[field];
    }
    item.barrier(sycl::access::fence_space::local_space);
    if (item.get_local_linear_id() != 0)
        return;
    int64_t final_values[ACCUM_FIELDS] = {};
    uint32_t const subgroup_count = subgroup.get_group_linear_range();
    for (uint32_t subgroup_idx = 0; subgroup_idx < subgroup_count; subgroup_idx++) {
        for (int field = 0; field < ACCUM_FIELDS; field++)
            final_values[field] += lmem[field * MAX_SUBGROUPS + subgroup_idx];
    }
    for (int field = 0; field < ACCUM_FIELDS; field++) {
        sycl::atomic_ref<int64_t, sycl::memory_order::relaxed, sycl::memory_scope::device,
                         sycl::access::address_space::global_space> const output(accum[field]);
        output.fetch_add(final_values[field]);
    }
}

template <int SCALE> struct VifHorizontalParams {
    unsigned width;
    unsigned height;
    float gain_limit;
    const uint32_t *input[7];
    int64_t *accum;
    uint32_t *reduced_ref;
    uint32_t *reduced_dis;
    const uint32_t *log2_lut;
    uint32_t coeff[VIF_FILTER_MAX_WIDTH];
    uint32_t reduction_coeff[VIF_FILTER_MAX_WIDTH];
};

template <int SCALE>
static inline void vif_hori_interior(const VifHorizontalParams<SCALE> &p, unsigned base,
                                     VifHorizontalSums &sums)
{
    constexpr int width = vif_fwidth[SCALE];
    constexpr int half = width / 2;
    constexpr int reduction_width = vif_fwidth_rd[SCALE];
    constexpr int reduction_start = (width - reduction_width) / 2;
    unsigned const center = base + half;
    uint32_t const center_coeff = p.coeff[half];
    sums.mu1 += center_coeff * p.input[0][center];
    sums.mu2 += center_coeff * p.input[1][center];
    sums.ref += (uint64_t)center_coeff * p.input[2][center];
    sums.dis += (uint64_t)center_coeff * p.input[3][center];
    sums.ref_dis += (uint64_t)center_coeff * p.input[4][center];
    if constexpr (reduction_width > 0) {
        uint32_t const reduction_coeff = p.reduction_coeff[reduction_width / 2];
        sums.ref_reduction += reduction_coeff * p.input[5][center];
        sums.dis_reduction += reduction_coeff * p.input[6][center];
    }
#pragma unroll
    for (int tap = 0; tap < half; tap++) {
        unsigned const low = base + tap;
        unsigned const high = base + width - 1 - tap;
        uint32_t const coeff = p.coeff[tap];
        sums.mu1 += coeff * (p.input[0][low] + p.input[0][high]);
        sums.mu2 += coeff * (p.input[1][low] + p.input[1][high]);
        sums.ref += (uint64_t)coeff * ((uint64_t)p.input[2][low] + p.input[2][high]);
        sums.dis += (uint64_t)coeff * ((uint64_t)p.input[3][low] + p.input[3][high]);
        sums.ref_dis += (uint64_t)coeff * ((uint64_t)p.input[4][low] + p.input[4][high]);
        if constexpr (reduction_width > 0) {
            if (tap >= reduction_start && tap < reduction_start + reduction_width / 2) {
                uint32_t const reduction_coeff = p.reduction_coeff[tap - reduction_start];
                sums.ref_reduction += reduction_coeff * (p.input[5][low] + p.input[5][high]);
                sums.dis_reduction += reduction_coeff * (p.input[6][low] + p.input[6][high]);
            }
        }
    }
}

template <int SCALE>
static inline void vif_hori_border(const VifHorizontalParams<SCALE> &p, int x, unsigned row,
                                   VifHorizontalSums &sums)
{
    constexpr int width = vif_fwidth[SCALE];
    constexpr int half = width / 2;
    constexpr int reduction_width = vif_fwidth_rd[SCALE];
    constexpr int reduction_start = (width - reduction_width) / 2;
#pragma unroll
    for (int tap = 0; tap < width; tap++) {
        int const sample_x = dev_mirror(x - half + tap, (int)p.width);
        unsigned const index = row + sample_x;
        uint32_t const coeff = p.coeff[tap];
        sums.mu1 += coeff * p.input[0][index];
        sums.mu2 += coeff * p.input[1][index];
        sums.ref += (uint64_t)coeff * p.input[2][index];
        sums.dis += (uint64_t)coeff * p.input[3][index];
        sums.ref_dis += (uint64_t)coeff * p.input[4][index];
        if constexpr (reduction_width > 0) {
            if (tap >= reduction_start && tap < reduction_start + reduction_width) {
                uint32_t const reduction_coeff = p.reduction_coeff[tap - reduction_start];
                sums.ref_reduction += reduction_coeff * p.input[5][index];
                sums.dis_reduction += reduction_coeff * p.input[6][index];
            }
        }
    }
}

static inline VifAccumTerms vif_terms_from_sums(const VifHorizontalSums &sums, float gain_limit,
                                                const uint32_t *log2_lut)
{
    uint32_t const filtered_ref = (sums.ref + 32768) >> 16;
    uint32_t const filtered_dis = (sums.dis + 32768) >> 16;
    uint32_t const filtered_ref_dis = (sums.ref_dis + 32768) >> 16;
    uint32_t const mu1_sq = ((uint64_t)sums.mu1 * sums.mu1 + 2147483648ULL) >> 32;
    uint32_t const mu2_sq = ((uint64_t)sums.mu2 * sums.mu2 + 2147483648ULL) >> 32;
    uint32_t const mu1_mu2 = ((uint64_t)sums.mu1 * sums.mu2 + 2147483648ULL) >> 32;
    int32_t sigma1 = (int32_t)(filtered_ref - mu1_sq);
    int32_t sigma2 = (int32_t)(filtered_dis - mu2_sq);
    int32_t const sigma12 = (int32_t)(filtered_ref_dis - mu1_mu2);
    if (sigma1 < 0)
        sigma1 = 0;
    if (sigma2 < 0)
        sigma2 = 0;
    return vif_pixel_terms(sigma1, sigma2, sigma12, gain_limit, log2_lut);
}

template <int SCALE>
static inline void vif_hori_downsample(const VifHorizontalParams<SCALE> &p,
                                       const VifHorizontalSums &sums, unsigned x, unsigned y)
{
    if constexpr (vif_fwidth_rd[SCALE] > 0) {
        if ((x % 2 == 0) && (y % 2 == 0)) {
            unsigned const stride = (p.width + 1U) / 2U;
            unsigned const index = (y / 2) * stride + x / 2;
            p.reduced_ref[index] = ((sums.ref_reduction + 32768) >> 16) & 0xFFFF;
            p.reduced_dis[index] = ((sums.dis_reduction + 32768) >> 16) & 0xFFFF;
        }
    }
}

template <int SCALE>
static VifHorizontalParams<SCALE> vif_hori_params(unsigned width, unsigned height, float gain_limit,
                                                  const uint32_t *const input[7], int64_t *accum,
                                                  uint32_t *reduced_ref, uint32_t *reduced_dis,
                                                  const uint32_t *log2_lut)
{
    VifHorizontalParams<SCALE> p = {};
    p.width = width;
    p.height = height;
    p.gain_limit = gain_limit;
    p.accum = accum;
    p.reduced_ref = reduced_ref;
    p.reduced_dis = reduced_dis;
    p.log2_lut = log2_lut;
    for (int i = 0; i < 7; i++)
        p.input[i] = input[i];
    for (int tap = 0; tap < vif_fwidth[SCALE]; tap++)
        p.coeff[tap] = vif_filter1d_table[SCALE][tap];
    if constexpr (vif_fwidth_rd[SCALE] > 0) {
        for (int tap = 0; tap < vif_fwidth_rd[SCALE]; tap++)
            p.reduction_coeff[tap] = vif_filter1d_table[SCALE + 1][tap];
    }
    return p;
}

template <int SCALE>
static sycl::event
launch_vif_hori_impl(sycl::queue &q, unsigned width, unsigned height, float vif_enhn_gain_limit,
                     const uint32_t *tmp_mu1, const uint32_t *tmp_mu2, const uint32_t *tmp_ref,
                     const uint32_t *tmp_dis, const uint32_t *tmp_ref_dis,
                     const uint32_t *tmp_ref_convol, const uint32_t *tmp_dis_convol, int64_t *accum,
                     uint32_t *rd_ref, uint32_t *rd_dis, const uint32_t *log2_lut)
{
    const uint32_t *input[7] = {tmp_mu1,     tmp_mu2,        tmp_ref,       tmp_dis,
                                tmp_ref_dis, tmp_ref_convol, tmp_dis_convol};
    VifHorizontalParams<SCALE> const p = vif_hori_params<SCALE>(
        width, height, vif_enhn_gain_limit, input, accum, rd_ref, rd_dis, log2_lut);
    sycl::range<2> const global(((height + 15) / 16) * 16, ((width + 15) / 16) * 16);
    sycl::range<2> const local(16, 16);
    return q.submit([&](sycl::handler &cgh) {
        sycl::local_accessor<int64_t, 1> const lmem(sycl::range<1>(ACCUM_FIELDS * 16), cgh);
        cgh.parallel_for(sycl::nd_range<2>(global, local),
                         [=](sycl::nd_item<2> item) VMAF_SYCL_REQD_SG_SIZE(16) {
                             unsigned const x = item.get_global_id(1);
                             unsigned const y = item.get_global_id(0);
                             bool const valid = x < p.width && y < p.height;
                             VifHorizontalSums sums = {};
                             VifAccumTerms terms = {};
                             if (valid) {
                                 unsigned const row = y * p.width;
                                 int const signed_x = (int)x;
                                 if (signed_x >= vif_fwidth[SCALE] / 2 &&
                                     signed_x < (int)p.width - vif_fwidth[SCALE] / 2)
                                     vif_hori_interior(p, row + x - vif_fwidth[SCALE] / 2, sums);
                                 else
                                     vif_hori_border(p, (int)x, row, sums);
                                 terms = vif_terms_from_sums(sums, p.gain_limit, p.log2_lut);
                             }
                             vif_reduce_terms<16>(item, lmem, p.accum, terms);
                             if (valid)
                                 vif_hori_downsample(p, sums, x, y);
                         });
    });
}

static sycl::event launch_vif_hori(sycl::queue &q, int scale, unsigned width, unsigned height,
                                   float vif_enhn_gain_limit, uint32_t *tmp_mu1,
                                   uint32_t *tmp_mu2, uint32_t *tmp_ref, uint32_t *tmp_dis,
                                   uint32_t *tmp_ref_dis, uint32_t *tmp_ref_convol,
                                   uint32_t *tmp_dis_convol, int64_t *accum, uint32_t *rd_ref,
                                   uint32_t *rd_dis, const uint32_t *log2_lut)
{
    switch (scale) {
    case 0:
        return launch_vif_hori_impl<0>(q, width, height, vif_enhn_gain_limit, tmp_mu1, tmp_mu2,
                                       tmp_ref, tmp_dis, tmp_ref_dis, tmp_ref_convol,
                                       tmp_dis_convol, accum, rd_ref, rd_dis, log2_lut);
    case 1:
        return launch_vif_hori_impl<1>(q, width, height, vif_enhn_gain_limit, tmp_mu1, tmp_mu2,
                                       tmp_ref, tmp_dis, tmp_ref_dis, tmp_ref_convol,
                                       tmp_dis_convol, accum, rd_ref, rd_dis, log2_lut);
    case 2:
        return launch_vif_hori_impl<2>(q, width, height, vif_enhn_gain_limit, tmp_mu1, tmp_mu2,
                                       tmp_ref, tmp_dis, tmp_ref_dis, tmp_ref_convol,
                                       tmp_dis_convol, accum, rd_ref, rd_dis, log2_lut);
    default:
        return launch_vif_hori_impl<3>(q, width, height, vif_enhn_gain_limit, tmp_mu1, tmp_mu2,
                                       tmp_ref, tmp_dis, tmp_ref_dis, tmp_ref_convol,
                                       tmp_dis_convol, accum, rd_ref, rd_dis, log2_lut);
    }
}

/* ------------------------------------------------------------------ */
/* SYCL Kernel: Fused Vertical + Horizontal Pass (single dispatch)    */
/* ------------------------------------------------------------------ */

/**
 * Fused VIF kernel V3: SLM intermediate with WG_Y=8 for better occupancy.
 *
 * Eliminates 7 intermediate global memory buffers and halves VIF kernel
 * launches (8→4).  Uses WG_Y=8 (instead of 16) to keep s_vert SLM at
 * ~7KB (scale 0) → total SLM ~15KB → 4 WGs/DSS (vs 2 with WG_Y=16).
 *
 * Phases:
 *   1. Cooperative load of input tile with V+H halos into s_ref/s_dis
 *   2. Cooperative vertical convolution → s_vert (7 channels in SLM)
 *   3. Horizontal convolution from s_vert + VIF statistics + downsample
 *   4. Subgroup + cross-subgroup reduction → atomic accumulation
 *
 * SLM budget (scale 0, FW=17, WG_Y=8, WG_X=16):
 *   s_ref:  (8+16) × (16+16) × 4 =  3,072 B
 *   s_dis:  same                   =  3,072 B
 *   s_vert: 7 × 8 × 32 × 4       =  7,168 B
 *   lmem:   7 × 16 × 8            =    896 B
 *   Total:                         ≈ 14.2 KB → 4 WGs/DSS
 */
template <int SCALE> struct VifFusedParams {
    const void *ref_data;
    const void *dis_data;
    unsigned width;
    unsigned height;
    unsigned src_stride;
    unsigned bpc;
    unsigned shift;
    unsigned round;
    unsigned square_shift;
    unsigned square_round;
    float gain_limit;
    int64_t *accum;
    uint32_t *reduced_ref;
    uint32_t *reduced_dis;
    const uint32_t *log2_lut;
    uint32_t coeff[VIF_FILTER_MAX_WIDTH];
    uint32_t reduction_coeff[VIF_FILTER_MAX_WIDTH];
};

template <int SCALE>
static inline uint32_t vif_fused_read(const VifFusedParams<SCALE> &p, const void *src, int y, int x)
{
    if constexpr (SCALE == 0) {
        if (p.bpc <= 8)
            return static_cast<const uint8_t *>(src)[y * p.src_stride + x];
        return static_cast<const uint16_t *>(src)[y * (p.src_stride / 2) + x];
    }
    return static_cast<const uint32_t *>(src)[y * p.src_stride + x] & 0xFFFF;
}

template <int SCALE, typename Tile>
static inline void vif_fused_load_tile(const VifFusedParams<SCALE> &p, sycl::nd_item<2> item,
                                       const Tile &ref_tile, const Tile &dis_tile)
{
    constexpr int width = vif_fwidth[SCALE];
    constexpr int tile_height = 8 + width - 1;
    constexpr int tile_width = 16 + width - 1;
    constexpr unsigned tile_elements = tile_height * tile_width;
    unsigned const lane = item.get_local_linear_id();
    int const origin_y = (int)(item.get_group(0) * 8) - width / 2;
    int const origin_x = (int)(item.get_group(1) * 16) - width / 2;
    bool const interior = origin_y >= 0 && origin_y + tile_height <= (int)p.height &&
                          origin_x >= 0 && origin_x + tile_width <= (int)p.width;
    for (unsigned i = lane; i < tile_elements; i += 128) {
        unsigned const row = i / tile_width;
        unsigned const col = i % tile_width;
        int y = origin_y + (int)row;
        int x = origin_x + (int)col;
        if (!interior) {
            y = dev_mirror(y, (int)p.height);
            x = dev_mirror(x, (int)p.width);
        }
        ref_tile[i] = vif_fused_read(p, p.ref_data, y, x);
        dis_tile[i] = vif_fused_read(p, p.dis_data, y, x);
    }
}

template <int SCALE, typename Tile>
static inline VifVertSums vif_fused_vertical_sums(const VifFusedParams<SCALE> &p,
                                                  const Tile &ref_tile, const Tile &dis_tile,
                                                  unsigned row, unsigned col)
{
    constexpr int width = vif_fwidth[SCALE];
    constexpr int tile_width = 16 + width - 1;
    constexpr int reduction_width = vif_fwidth_rd[SCALE];
    constexpr int reduction_start = (width - reduction_width) / 2;
    VifVertSums sums = {};
#pragma unroll
    for (int tap = 0; tap < width; tap++) {
        unsigned const index = (row + (unsigned)tap) * tile_width + col;
        uint32_t const ref = ref_tile[index];
        uint32_t const dis = dis_tile[index];
        uint32_t const weighted_ref = p.coeff[tap] * ref;
        uint32_t const weighted_dis = p.coeff[tap] * dis;
        sums.mu1 += weighted_ref;
        sums.mu2 += weighted_dis;
        sums.ref += (uint64_t)weighted_ref * ref;
        sums.dis += (uint64_t)weighted_dis * dis;
        sums.ref_dis += (uint64_t)weighted_ref * dis;
        if constexpr (reduction_width > 0) {
            if (tap >= reduction_start && tap < reduction_start + reduction_width) {
                uint32_t const coeff = p.reduction_coeff[tap - reduction_start];
                sums.ref_reduction += coeff * ref;
                sums.dis_reduction += coeff * dis;
            }
        }
    }
    return sums;
}

template <int SCALE, typename Tile>
static inline void vif_fused_store_vertical(const VifFusedParams<SCALE> &p, const Tile &vertical,
                                            const VifVertSums &sums, unsigned index)
{
    constexpr unsigned total = 8 * (16 + vif_fwidth[SCALE] - 1);
    vertical[0 * total + index] = (sums.mu1 + p.round) >> p.shift;
    vertical[1 * total + index] = (sums.mu2 + p.round) >> p.shift;
    vertical[2 * total + index] = (sums.ref + p.square_round) >> p.square_shift;
    vertical[3 * total + index] = (sums.dis + p.square_round) >> p.square_shift;
    vertical[4 * total + index] = (sums.ref_dis + p.square_round) >> p.square_shift;
    if constexpr (vif_fwidth_rd[SCALE] > 0) {
        vertical[5 * total + index] = (sums.ref_reduction + p.round) >> p.shift;
        vertical[6 * total + index] = (sums.dis_reduction + p.round) >> p.shift;
    }
}

template <int SCALE, typename InputTile, typename VerticalTile>
static inline void vif_fused_vertical(const VifFusedParams<SCALE> &p, sycl::nd_item<2> item,
                                      const InputTile &ref_tile, const InputTile &dis_tile,
                                      const VerticalTile &vertical)
{
    constexpr unsigned tile_width = 16 + vif_fwidth[SCALE] - 1;
    constexpr unsigned total = 8 * tile_width;
    for (unsigned i = item.get_local_linear_id(); i < total; i += 128) {
        unsigned const row = i / tile_width;
        unsigned const col = i % tile_width;
        VifVertSums const sums = vif_fused_vertical_sums(p, ref_tile, dis_tile, row, col);
        vif_fused_store_vertical(p, vertical, sums, i);
    }
}

template <int SCALE, typename VerticalTile>
static inline uint32_t vif_fused_value(const VerticalTile &vertical, unsigned channel, unsigned row,
                                       unsigned col)
{
    constexpr unsigned tile_width = 16 + vif_fwidth[SCALE] - 1;
    constexpr unsigned total = 8 * tile_width;
    return vertical[channel * total + row * tile_width + col];
}

template <int SCALE, typename VerticalTile>
static inline void vif_fused_horizontal_center(const VifFusedParams<SCALE> &p,
                                               const VerticalTile &vertical, unsigned row,
                                               unsigned col, VifHorizontalSums &sums,
                                               uint32_t &ref_reduction, uint32_t &dis_reduction)
{
    constexpr int half = vif_fwidth[SCALE] / 2;
    constexpr int reduction_width = vif_fwidth_rd[SCALE];
    unsigned const center = col + (unsigned)half;
    uint32_t const coeff = p.coeff[half];
    sums.mu1 += coeff * vif_fused_value<SCALE>(vertical, 0, row, center);
    sums.mu2 += coeff * vif_fused_value<SCALE>(vertical, 1, row, center);
    sums.ref += (uint64_t)coeff * vif_fused_value<SCALE>(vertical, 2, row, center);
    sums.dis += (uint64_t)coeff * vif_fused_value<SCALE>(vertical, 3, row, center);
    sums.ref_dis += (uint64_t)coeff * vif_fused_value<SCALE>(vertical, 4, row, center);
    if constexpr (reduction_width > 0) {
        uint32_t const reduction_coeff = p.reduction_coeff[reduction_width / 2];
        ref_reduction += reduction_coeff * vif_fused_value<SCALE>(vertical, 5, row, center);
        dis_reduction += reduction_coeff * vif_fused_value<SCALE>(vertical, 6, row, center);
    }
}

template <int SCALE, typename VerticalTile>
static inline void vif_fused_horizontal_pairs(const VifFusedParams<SCALE> &p,
                                              const VerticalTile &vertical, unsigned row,
                                              unsigned col, VifHorizontalSums &sums,
                                              uint32_t &ref_reduction, uint32_t &dis_reduction)
{
    constexpr int width = vif_fwidth[SCALE];
    constexpr int half = width / 2;
    constexpr int reduction_width = vif_fwidth_rd[SCALE];
    constexpr int reduction_start = (width - reduction_width) / 2;
#pragma unroll
    for (int tap = 0; tap < half; tap++) {
        unsigned const low = col + (unsigned)tap;
        unsigned const high = col + (unsigned)(width - 1 - tap);
        uint32_t const coeff = p.coeff[tap];
        sums.mu1 += coeff * (vif_fused_value<SCALE>(vertical, 0, row, low) +
                             vif_fused_value<SCALE>(vertical, 0, row, high));
        sums.mu2 += coeff * (vif_fused_value<SCALE>(vertical, 1, row, low) +
                             vif_fused_value<SCALE>(vertical, 1, row, high));
        sums.ref += (uint64_t)coeff * ((uint64_t)vif_fused_value<SCALE>(vertical, 2, row, low) +
                                       vif_fused_value<SCALE>(vertical, 2, row, high));
        sums.dis += (uint64_t)coeff * ((uint64_t)vif_fused_value<SCALE>(vertical, 3, row, low) +
                                       vif_fused_value<SCALE>(vertical, 3, row, high));
        sums.ref_dis += (uint64_t)coeff * ((uint64_t)vif_fused_value<SCALE>(vertical, 4, row, low) +
                                           vif_fused_value<SCALE>(vertical, 4, row, high));
        if constexpr (reduction_width > 0) {
            if (tap >= reduction_start && tap < reduction_start + reduction_width / 2) {
                uint32_t const reduction_coeff = p.reduction_coeff[tap - reduction_start];
                ref_reduction += reduction_coeff * (vif_fused_value<SCALE>(vertical, 5, row, low) +
                                                    vif_fused_value<SCALE>(vertical, 5, row, high));
                dis_reduction += reduction_coeff * (vif_fused_value<SCALE>(vertical, 6, row, low) +
                                                    vif_fused_value<SCALE>(vertical, 6, row, high));
            }
        }
    }
}

template <int SCALE, typename VerticalTile>
static inline VifHorizontalSums vif_fused_horizontal(const VifFusedParams<SCALE> &p,
                                                     const VerticalTile &vertical, unsigned row,
                                                     unsigned col)
{
    VifHorizontalSums sums = {};
    // DO_RD=false folds these writes away, but scale 0-2 mutate both accumulators. ADR-1266.
    uint32_t ref_reduction = 0;
    // The paired accumulator has the same scale-dependent mutation. ADR-1266.
    uint32_t dis_reduction = 0;
    vif_fused_horizontal_center(p, vertical, row, col, sums, ref_reduction, dis_reduction);
    vif_fused_horizontal_pairs(p, vertical, row, col, sums, ref_reduction, dis_reduction);
    sums.ref_reduction = ref_reduction;
    sums.dis_reduction = dis_reduction;
    return sums;
}

template <int SCALE>
static inline void vif_fused_downsample(const VifFusedParams<SCALE> &p,
                                        const VifHorizontalSums &sums, unsigned x, unsigned y)
{
    if constexpr (vif_fwidth_rd[SCALE] > 0) {
        if ((x % 2 == 0) && (y % 2 == 0)) {
            unsigned const stride = (p.width + 1U) / 2U;
            unsigned const index = (y / 2) * stride + x / 2;
            p.reduced_ref[index] = ((sums.ref_reduction + 32768) >> 16) & 0xFFFF;
            p.reduced_dis[index] = ((sums.dis_reduction + 32768) >> 16) & 0xFFFF;
        }
    }
}

template <int SCALE>
static VifFusedParams<SCALE>
vif_fused_params(const void *ref_data, const void *dis_data, unsigned width, unsigned height,
                 unsigned src_stride, unsigned bpc, float gain_limit, int64_t *accum,
                 uint32_t *reduced_ref, uint32_t *reduced_dis, const uint32_t *log2_lut)
{
    VifFusedParams<SCALE> p = {};
    p.ref_data = ref_data;
    p.dis_data = dis_data;
    p.width = width;
    p.height = height;
    p.src_stride = src_stride;
    p.bpc = bpc;
    p.shift = SCALE == 0 ? bpc : 16;
    p.round = SCALE == 0 ? 1u << (bpc - 1) : 32768;
    p.square_shift = SCALE == 0 ? (bpc - 8) * 2 : 16;
    p.square_round = p.square_shift == 0 ? 0 : 1u << (p.square_shift - 1);
    p.gain_limit = gain_limit;
    p.accum = accum;
    p.reduced_ref = reduced_ref;
    p.reduced_dis = reduced_dis;
    p.log2_lut = log2_lut;
    for (int tap = 0; tap < vif_fwidth[SCALE]; tap++)
        p.coeff[tap] = vif_filter1d_table[SCALE][tap];
    if constexpr (vif_fwidth_rd[SCALE] > 0) {
        for (int tap = 0; tap < vif_fwidth_rd[SCALE]; tap++)
            p.reduction_coeff[tap] = vif_filter1d_table[SCALE + 1][tap];
    }
    return p;
}

template <int SCALE>
static sycl::event
launch_vif_fused_impl(sycl::queue &q, const void *ref_data, const void *dis_data, unsigned width,
                      unsigned height, unsigned src_stride, unsigned bpc, float vif_enhn_gain_limit,
                      int64_t *accum, uint32_t *rd_ref, uint32_t *rd_dis, const uint32_t *log2_lut)
{
    constexpr int tile_height = 8 + vif_fwidth[SCALE] - 1;
    constexpr int tile_width = 16 + vif_fwidth[SCALE] - 1;
    constexpr int channel_count = vif_fwidth_rd[SCALE] > 0 ? 7 : 5;
    constexpr unsigned vertical_total = 8 * tile_width;
    VifFusedParams<SCALE> const p =
        vif_fused_params<SCALE>(ref_data, dis_data, width, height, src_stride, bpc,
                                vif_enhn_gain_limit, accum, rd_ref, rd_dis, log2_lut);
    sycl::range<2> const global(((height + 7) / 8) * 8, ((width + 15) / 16) * 16);
    sycl::range<2> const local(8, 16);
    return q.submit([&](sycl::handler &cgh) {
        sycl::local_accessor<uint32_t, 1> const ref_tile(sycl::range<1>(tile_height * tile_width),
                                                         cgh);
        sycl::local_accessor<uint32_t, 1> const dis_tile(sycl::range<1>(tile_height * tile_width),
                                                         cgh);
        sycl::local_accessor<uint32_t, 1> const vertical(
            sycl::range<1>(channel_count * vertical_total), cgh);
        sycl::local_accessor<int64_t, 1> const reduction(sycl::range<1>(ACCUM_FIELDS * 16), cgh);
        cgh.parallel_for(sycl::nd_range<2>(global, local),
                         [=](sycl::nd_item<2> item) VMAF_SYCL_REQD_SG_SIZE(16) {
                             vif_fused_load_tile(p, item, ref_tile, dis_tile);
                             item.barrier(sycl::access::fence_space::local_space);
                             vif_fused_vertical(p, item, ref_tile, dis_tile, vertical);
                             item.barrier(sycl::access::fence_space::local_space);
                             unsigned const x = item.get_global_id(1);
                             unsigned const y = item.get_global_id(0);
                             bool const valid = x < p.width && y < p.height;
                             VifHorizontalSums sums = {};
                             VifAccumTerms terms = {};
                             if (valid) {
                                 sums = vif_fused_horizontal(p, vertical, item.get_local_id(0),
                                                             item.get_local_id(1));
                                 terms = vif_terms_from_sums(sums, p.gain_limit, p.log2_lut);
                             }
                             vif_reduce_terms<16>(item, reduction, p.accum, terms);
                             if (valid)
                                 vif_fused_downsample(p, sums, x, y);
                         });
    });
}

static sycl::event launch_vif_fused(sycl::queue &q, const void *ref_data, const void *dis_data,
                                    int scale, unsigned width, unsigned height, unsigned src_stride,
                                    unsigned bpc, float vif_enhn_gain_limit, int64_t *accum,
                                    uint32_t *rd_ref, uint32_t *rd_dis, const uint32_t *log2_lut)
{
    switch (scale) {
    case 0:
        return launch_vif_fused_impl<0>(q, ref_data, dis_data, width, height, src_stride, bpc,
                                        vif_enhn_gain_limit, accum, rd_ref, rd_dis, log2_lut);
    case 1:
        return launch_vif_fused_impl<1>(q, ref_data, dis_data, width, height, src_stride, bpc,
                                        vif_enhn_gain_limit, accum, rd_ref, rd_dis, log2_lut);
    case 2:
        return launch_vif_fused_impl<2>(q, ref_data, dis_data, width, height, src_stride, bpc,
                                        vif_enhn_gain_limit, accum, rd_ref, rd_dis, log2_lut);
    default:
        return launch_vif_fused_impl<3>(q, ref_data, dis_data, width, height, src_stride, bpc,
                                        vif_enhn_gain_limit, accum, rd_ref, rd_dis, log2_lut);
    }
}

/* ------------------------------------------------------------------ */
/* Feature extractor callbacks                                         */
/* ------------------------------------------------------------------ */

// Forward declarations for combined graph callbacks (defined after enqueue_vif_work_impl)
static void enqueue_vif_work(void *queue_ptr, void *priv, void *shared_ref, void *shared_dis);
static void vif_pre_graph(void *queue_ptr, void *priv);
static void vif_post_graph(void *queue_ptr, void *priv);
static int close_fex_sycl(VmafFeatureExtractor *fex); /* forward decl for init error paths */

static int
close_fex_sycl(VmafFeatureExtractor *fex); /* forward decl for init-failure cleanup — SY-2a */

static int vif_init_runtime(VmafFeatureExtractor *fex, VifStateSycl *s, unsigned bpc, unsigned w,
                            unsigned h, VmafSyclState *&state, sycl::queue *&queue)
{
    s->width = w;
    s->height = h;
    s->bpc = bpc;
    s->has_pending = false;
    state = fex->sycl_state;
    if (!state) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "vif_sycl: no SYCL state\n");
        return -EINVAL;
    }
    queue = static_cast<sycl::queue *>(vmaf_sycl_get_queue_ptr(state));
    if (!queue)
        return -EINVAL;
    return vmaf_sycl_shared_frame_init(state, w, h, bpc);
}

static bool vif_tmp_buffers_ready(const VifStateSycl *s)
{
    return s->d_tmp_mu1 && s->d_tmp_mu2 && s->d_tmp_ref && s->d_tmp_dis && s->d_tmp_ref_dis &&
           s->d_tmp_ref_convol && s->d_tmp_dis_convol;
}

static int vif_allocate_buffers(VmafSyclState *state, VifStateSycl *s, unsigned w, unsigned h)
{
    size_t const tmp_size = (size_t)w * h * sizeof(uint32_t);
    if (!s->use_fused) {
        s->d_tmp_mu1 = static_cast<uint32_t *>(vmaf_sycl_malloc_device(state, tmp_size));
        s->d_tmp_mu2 = static_cast<uint32_t *>(vmaf_sycl_malloc_device(state, tmp_size));
        s->d_tmp_ref = static_cast<uint32_t *>(vmaf_sycl_malloc_device(state, tmp_size));
        s->d_tmp_dis = static_cast<uint32_t *>(vmaf_sycl_malloc_device(state, tmp_size));
        s->d_tmp_ref_dis = static_cast<uint32_t *>(vmaf_sycl_malloc_device(state, tmp_size));
        s->d_tmp_ref_convol = static_cast<uint32_t *>(vmaf_sycl_malloc_device(state, tmp_size));
        s->d_tmp_dis_convol = static_cast<uint32_t *>(vmaf_sycl_malloc_device(state, tmp_size));
    }
    size_t const rd_size = (size_t)((w + 1U) / 2U) * ((h + 1U) / 2U) * sizeof(uint32_t);
    s->d_rd_ref = static_cast<uint32_t *>(vmaf_sycl_malloc_device(state, rd_size));
    s->d_rd_dis = static_cast<uint32_t *>(vmaf_sycl_malloc_device(state, rd_size));
    size_t const accum_size = (ptrdiff_t)VIF_NUM_SCALES * ACCUM_FIELDS * sizeof(int64_t);
    s->d_accum = static_cast<int64_t *>(vmaf_sycl_malloc_device(state, accum_size));
    s->h_accum = static_cast<int64_t *>(vmaf_sycl_malloc_host(state, accum_size));
    size_t const lut_size = LOG2_LUT_SIZE * sizeof(uint32_t);
    s->d_log2_lut = static_cast<uint32_t *>(vmaf_sycl_malloc_device(state, lut_size));
    if (!s->use_fused && !vif_tmp_buffers_ready(s)) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "vif_sycl: tmp buffer allocation failed\n");
        return -ENOMEM;
    }
    if (!s->d_rd_ref || !s->d_rd_dis || !s->d_accum || !s->h_accum || !s->d_log2_lut) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "vif_sycl: device memory allocation failed\n");
        return -ENOMEM;
    }
    return 0;
}

static int vif_upload_log2_lut(VmafSyclState *state, VifStateSycl *s)
{
    size_t const lut_size = LOG2_LUT_SIZE * sizeof(uint32_t);
    uint32_t *lut_host = static_cast<uint32_t *>(std::malloc(lut_size));
    if (!lut_host)
        return -ENOMEM;
    for (int j = 0; j < LOG2_LUT_SIZE; j++)
        lut_host[j] = (uint32_t)std::roundf(std::log2f((float)(j + 32768)) * 2048.0f);
    int const err = vmaf_sycl_memcpy_h2d(state, s->d_log2_lut, lut_host, lut_size);
    std::free(lut_host);
    if (err)
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "vif_sycl: log2 LUT upload failed\n");
    return err;
}

static int vif_validate_subgroup(const sycl::queue &queue)
{
    auto const device = queue.get_device();
    auto const subgroup_sizes = device.get_info<sycl::info::device::sub_group_sizes>();
    if (std::find(subgroup_sizes.cbegin(), subgroup_sizes.cend(), size_t{16}) ==
        subgroup_sizes.cend()) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "vif_sycl: device does not support the required SIMD-16 subgroup size\n");
        return -ENOTSUP;
    }
    return 0;
}

static int vif_register_graph(VmafFeatureExtractor *fex, VmafSyclState *state, VifStateSycl *s)
{
    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (!s->feature_name_dict)
        return -ENOMEM;
    return vmaf_sycl_graph_register(state, enqueue_vif_work, vif_pre_graph, vif_post_graph, nullptr,
                                    s, "VIF");
}

static int init_fex_sycl(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                         unsigned w, unsigned h)
{
    (void)pix_fmt;
    auto *s = static_cast<VifStateSycl *>(fex->priv);
    VmafSyclState *state = nullptr;
    sycl::queue *queue = nullptr;
    int err = vif_init_runtime(fex, s, bpc, w, h, state, queue);
    if (err)
        return err;
    err = vif_allocate_buffers(state, s, w, h);
    if (!err)
        err = vif_upload_log2_lut(state, s);
    if (!err) {
        err = vif_validate_subgroup(*queue);
    }
    if (!err) {
        vmaf_log(VMAF_LOG_LEVEL_DEBUG, "vif_sycl: SIMD-16 kernel mode = %s\n",
                 s->use_fused ? "fused V+H (saves VRAM)" : "separate V+H");
        err = vif_register_graph(fex, state, s);
    }
    if (err)
        close_fex_sycl(fex);
    return err;
}

/* ------------------------------------------------------------------ */
/* Enqueue all VIF compute work (used for both recording and direct)    */
/* ------------------------------------------------------------------ */

struct VifScaleInput {
    const void *ref;
    const void *dis;
    unsigned stride;
};

static VifScaleInput vif_scale_input(const VifStateSycl *s, const void *shared_ref,
                                     const void *shared_dis, int scale, unsigned width)
{
    if (scale == 0) {
        unsigned const stride = s->bpc <= 8 ? width : width * 2;
        return {shared_ref, shared_dis, stride};
    }
    return {s->d_rd_ref, s->d_rd_dis, width};
}

static void vif_enqueue_separate(sycl::queue &q, VifStateSycl *s, const VifScaleInput &input,
                                 int scale, unsigned width, unsigned height, int64_t *accum)
{
    launch_vif_vert(q, input.ref, input.dis, scale, width, height, input.stride, s->bpc,
                    s->d_tmp_mu1, s->d_tmp_mu2, s->d_tmp_ref, s->d_tmp_dis, s->d_tmp_ref_dis,
                    s->d_tmp_ref_convol, s->d_tmp_dis_convol);
    launch_vif_hori(q, scale, width, height, (float)s->vif_enhn_gain_limit, s->d_tmp_mu1,
                    s->d_tmp_mu2, s->d_tmp_ref, s->d_tmp_dis, s->d_tmp_ref_dis,
                    s->d_tmp_ref_convol, s->d_tmp_dis_convol, accum, s->d_rd_ref, s->d_rd_dis,
                    s->d_log2_lut);
}

static void vif_enqueue_scale(sycl::queue &q, VifStateSycl *s, const VifScaleInput &input,
                              int scale, unsigned width, unsigned height)
{
    int64_t *const accum = s->d_accum + (ptrdiff_t)scale * ACCUM_FIELDS;
    if (s->use_fused) {
        launch_vif_fused(q, input.ref, input.dis, scale, width, height, input.stride, s->bpc,
                         (float)s->vif_enhn_gain_limit, accum, s->d_rd_ref, s->d_rd_dis,
                         s->d_log2_lut);
        return;
    }
    vif_enqueue_separate(q, s, input, scale, width, height, accum);
}

static void enqueue_vif_work_impl(sycl::queue &q, VifStateSycl *s, void *shared_ref,
                                  void *shared_dis)
{
    unsigned width = s->width;
    unsigned height = s->height;
    for (int scale = 0; scale < VIF_NUM_SCALES; scale++) {
        VifScaleInput const input = vif_scale_input(s, shared_ref, shared_dis, scale, width);
        vif_enqueue_scale(q, s, input, scale, width, height);
        width /= 2;
        height /= 2;
    }
}

/* ------------------------------------------------------------------ */
/* Command graph recording (one graph per double-buffer slot)          */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* C-compatible callbacks for combined command graph                   */
/* ------------------------------------------------------------------ */

// Pre-graph: zero accumulators (direct enqueue, outside graph)
static void vif_pre_graph(void *queue_ptr, void *priv)
{
    sycl::queue &q = *static_cast<sycl::queue *>(queue_ptr);
    auto *s = static_cast<VifStateSycl *>(priv);
    size_t accum_size = (ptrdiff_t)VIF_NUM_SCALES * ACCUM_FIELDS * sizeof(int64_t);
    q.memset(s->d_accum, 0, accum_size);
}

// Graph-recorded: compute kernels only
static void enqueue_vif_work(void *queue_ptr, void *priv, void *shared_ref, void *shared_dis)
{
    sycl::queue &q = *static_cast<sycl::queue *>(queue_ptr);
    auto *s = static_cast<VifStateSycl *>(priv);
    enqueue_vif_work_impl(q, s, shared_ref, shared_dis);
}

// Post-graph: D2H accumulator download (direct enqueue, outside graph)
static void vif_post_graph(void *queue_ptr, void *priv)
{
    sycl::queue &q = *static_cast<sycl::queue *>(queue_ptr);
    auto *s = static_cast<VifStateSycl *>(priv);
    size_t accum_size = (ptrdiff_t)VIF_NUM_SCALES * ACCUM_FIELDS * sizeof(int64_t);
    q.memcpy(s->h_accum, s->d_accum, accum_size);
}

/* ------------------------------------------------------------------ */
/* Submit / Collect / Extract                                          */
/* ------------------------------------------------------------------ */

static int submit_fex_sycl(VmafFeatureExtractor *fex, const VmafPicture *ref_pic, const VmafPicture *ref_pic_90,
                           const VmafPicture *dist_pic, const VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic;
    (void)ref_pic_90;
    (void)dist_pic;
    (void)dist_pic_90;

    auto *s = static_cast<VifStateSycl *>(fex->priv);
    VmafSyclState *state = fex->sycl_state;

    // Combined graph submit (idempotent per frame — first extractor wins)
    int const err = vmaf_sycl_graph_submit(state);
    if (err)
        return err;

    s->pending_index = index;
    s->has_pending = true;

    return 0;
}

struct VifScores {
    double numerator[VIF_NUM_SCALES];
    double denominator[VIF_NUM_SCALES];
    double total_numerator;
    double total_denominator;
};

static VifScores vif_compute_scores(const VifStateSycl *s, const vif_accums *accums)
{
    VifScores scores = {};
    for (int scale = 0; scale < VIF_NUM_SCALES; scale++) {
        double const numerator =
            accums[scale].num_log / 2048.0 + accums[scale].x2 +
            (accums[scale].den_non_log - (accums[scale].num_non_log / 16384.0) / 65025.0);
        double const denominator = accums[scale].den_log / 2048.0 -
                                   (accums[scale].x + accums[scale].num_x * 17) +
                                   accums[scale].den_non_log;
        scores.numerator[scale] = numerator;
        scores.denominator[scale] = denominator;
        if (!s->vif_skip_scale0 || scale > 0) {
            scores.total_numerator += numerator;
            scores.total_denominator += denominator;
        }
    }
    return scores;
}

static int vif_append_scale_scores(VmafFeatureCollector *collector, const VifStateSycl *s,
                                   const VifScores &scores, unsigned index)
{
    static const char *const key_names[] = {
        "VMAF_integer_feature_vif_scale0_score",
        "VMAF_integer_feature_vif_scale1_score",
        "VMAF_integer_feature_vif_scale2_score",
        "VMAF_integer_feature_vif_scale3_score",
    };
    for (int scale = 0; scale < VIF_NUM_SCALES; scale++) {
        double const score = (scale == 0 && s->vif_skip_scale0) ?
                                 0.0 :
                                 ((scores.denominator[scale] > 0.0) ?
                                      scores.numerator[scale] / scores.denominator[scale] :
                                      1.0);
        int const err = vmaf_feature_collector_append_with_dict(collector, s->feature_name_dict,
                                                                key_names[scale], score, index);
        if (err)
            return err;
    }
    return 0;
}

static void vif_append_debug_scale(VmafFeatureCollector *collector, const VifStateSycl *s,
                                   const VifScores &scores, int scale, unsigned index)
{
    char name[64];
    if (scale == 0 && s->vif_skip_scale0) {
        (void)std::snprintf(name, sizeof(name), "integer_vif_num_scale0");
        vmaf_feature_collector_append_with_dict(collector, s->feature_name_dict, name, 0.0, index);
        (void)std::snprintf(name, sizeof(name), "integer_vif_den_scale0");
        vmaf_feature_collector_append_with_dict(collector, s->feature_name_dict, name, -1.0, index);
        return;
    }
    (void)std::snprintf(name, sizeof(name), "integer_vif_num_scale%d", scale);
    vmaf_feature_collector_append_with_dict(collector, s->feature_name_dict, name,
                                            scores.numerator[scale], index);
    (void)std::snprintf(name, sizeof(name), "integer_vif_den_scale%d", scale);
    vmaf_feature_collector_append_with_dict(collector, s->feature_name_dict, name,
                                            scores.denominator[scale], index);
}

static void vif_append_debug_scores(VmafFeatureCollector *collector, const VifStateSycl *s,
                                    const VifScores &scores, unsigned index)
{
    double const vif =
        scores.total_denominator > 0.0 ? scores.total_numerator / scores.total_denominator : 1.0;
    vmaf_feature_collector_append_with_dict(collector, s->feature_name_dict, "integer_vif", vif,
                                            index);
    vmaf_feature_collector_append_with_dict(collector, s->feature_name_dict, "integer_vif_num",
                                            scores.total_numerator, index);
    vmaf_feature_collector_append_with_dict(collector, s->feature_name_dict, "integer_vif_den",
                                            scores.total_denominator, index);
    for (int scale = 0; scale < VIF_NUM_SCALES; scale++)
        vif_append_debug_scale(collector, s, scores, scale, index);
}

static int collect_fex_sycl(VmafFeatureExtractor *fex, unsigned index,
                            VmafFeatureCollector *feature_collector)
{
    auto *s = static_cast<VifStateSycl *>(fex->priv);
    vmaf_sycl_graph_wait(fex->sycl_state);
    vif_accums accums[VIF_NUM_SCALES];
    std::memcpy(accums, s->h_accum, sizeof(accums));
    VifScores const scores = vif_compute_scores(s, accums);
    int const err = vif_append_scale_scores(feature_collector, s, scores, index);
    if (err)
        return err;
    if (s->debug)
        vif_append_debug_scores(feature_collector, s, scores, index);
    s->has_pending = false;
    return 0;
}

static int extract_fex_sycl(VmafFeatureExtractor *fex, const VmafPicture *ref_pic,
                            const VmafPicture *ref_pic_90, const VmafPicture *dist_pic,
                            const VmafPicture *dist_pic_90, unsigned index,
                            VmafFeatureCollector *feature_collector)
{
    int const err = submit_fex_sycl(fex, ref_pic, ref_pic_90, dist_pic, dist_pic_90, index);
    if (err)
        return err;
    return collect_fex_sycl(fex, index, feature_collector);
}

static int flush_fex_sycl(VmafFeatureExtractor *fex, VmafFeatureCollector *feature_collector)
{
    (void)feature_collector;
    if (!fex)
        return -EINVAL;
    VmafSyclState *state = fex->sycl_state;
    if (state) {
        int wait_err = vmaf_sycl_queue_wait(state);
        if (wait_err)
            return wait_err;
    }
    return 1; // done — collect already consumed pending work
}

static int close_fex_sycl(VmafFeatureExtractor *fex)
{
    auto *s = static_cast<VifStateSycl *>(fex->priv);
    VmafSyclState *state = fex->sycl_state;

    if (state) {
        (void)vmaf_sycl_queue_wait(state);

        /* Unregister from the combined command graph before freeing priv.
         * Mirrors the fix in integer_motion_sycl.cpp (ADR-0989):
         * vmaf_sycl_graph_unregister() drains combined_queue and removes
         * this extractor's entry so a subsequent VmafContext sharing the
         * same sycl_state does not inherit a dangling priv pointer. */
        (void)vmaf_sycl_graph_unregister(state, s);

        if (s->d_tmp_mu1)
            vmaf_sycl_free(state, s->d_tmp_mu1);
        if (s->d_tmp_mu2)
            vmaf_sycl_free(state, s->d_tmp_mu2);
        if (s->d_tmp_ref)
            vmaf_sycl_free(state, s->d_tmp_ref);
        if (s->d_tmp_dis)
            vmaf_sycl_free(state, s->d_tmp_dis);
        if (s->d_tmp_ref_dis)
            vmaf_sycl_free(state, s->d_tmp_ref_dis);
        if (s->d_tmp_ref_convol)
            vmaf_sycl_free(state, s->d_tmp_ref_convol);
        if (s->d_tmp_dis_convol)
            vmaf_sycl_free(state, s->d_tmp_dis_convol);
        if (s->d_rd_ref)
            vmaf_sycl_free(state, s->d_rd_ref);
        if (s->d_rd_dis)
            vmaf_sycl_free(state, s->d_rd_dis);
        if (s->d_accum)
            vmaf_sycl_free(state, s->d_accum);
        if (s->h_accum)
            vmaf_sycl_free(state, s->h_accum);
        if (s->d_log2_lut)
            vmaf_sycl_free(state, s->d_log2_lut);
    }

    if (s->feature_name_dict)
        vmaf_dictionary_free(&s->feature_name_dict);

    return 0;
}

/* ------------------------------------------------------------------ */
/* Feature extractor definition                                        */
/* ------------------------------------------------------------------ */

static const char *provided_features[] = {"VMAF_integer_feature_vif_scale0_score",
                                          "VMAF_integer_feature_vif_scale1_score",
                                          "VMAF_integer_feature_vif_scale2_score",
                                          "VMAF_integer_feature_vif_scale3_score",
                                          "integer_vif",
                                          "integer_vif_num",
                                          "integer_vif_den",
                                          "integer_vif_num_scale0",
                                          "integer_vif_den_scale0",
                                          "integer_vif_num_scale1",
                                          "integer_vif_den_scale1",
                                          "integer_vif_num_scale2",
                                          "integer_vif_den_scale2",
                                          "integer_vif_num_scale3",
                                          "integer_vif_den_scale3",
                                          nullptr};

extern "C" VmafFeatureExtractor vmaf_fex_integer_vif_sycl = {
    .name = "vif_sycl",
    .init = init_fex_sycl,
    .extract = extract_fex_sycl,
    .flush = flush_fex_sycl,
    .close = close_fex_sycl,
    .submit = submit_fex_sycl,
    .collect = collect_fex_sycl,
    .options = options,
    .priv_size = sizeof(VifStateSycl),
    .flags = VMAF_FEATURE_EXTRACTOR_SYCL,
    .provided_features = provided_features,
};
