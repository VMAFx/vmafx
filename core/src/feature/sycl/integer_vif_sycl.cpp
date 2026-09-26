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
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

#include "config.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature_name.h"
#include "feature/nonfinite_score.h"
#include "sycl/common.h"
#include "log.h"

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

constexpr int VIF_NUM_SCALES = 4;
constexpr int VIF_FILTER_MAX_WIDTH = 17;
constexpr int VIF_FILTER_TABLE_PAD = 18;

// Filter coefficients per scale (sum to 65536 = 2^16)
constexpr uint32_t vif_filter1d_table[VIF_NUM_SCALES][VIF_FILTER_TABLE_PAD] = {
    {489, 935, 1640, 2640, 3896, 5274, 6547, 7455, 7784, 7455, 6547, 5274, 3896, 2640, 1640, 935,
     489, 0},
    {1244, 3663, 7925, 12590, 14692, 12590, 7925, 3663, 1244, 0},
    {3571, 16004, 26386, 16004, 3571, 0},
    {10904, 43728, 10904, 0},
};

constexpr int vif_fwidth[VIF_NUM_SCALES] = {17, 9, 5, 3};
constexpr int vif_fwidth_rd[VIF_NUM_SCALES] = {9, 5, 3, 0};

constexpr int64_t SIGMA_NSQ = 131072; // 2 * 65536

constexpr int LOG2_LUT_SIZE = 32768;

/* ------------------------------------------------------------------ */
/* Per-scale accumulator struct (7 x int64_t = 56 bytes)              */
/* ------------------------------------------------------------------ */

namespace
{
struct vif_accums {
    int64_t x;
    int64_t x2;
    int64_t num_x;
    int64_t num_log;
    int64_t den_log;
    int64_t num_non_log;
    int64_t den_non_log;
};
} // namespace
constexpr int ACCUM_FIELDS = 7;

/* ------------------------------------------------------------------ */
/* Extractor private state                                             */
/* ------------------------------------------------------------------ */

namespace
{
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

    // Subgroup size selection (auto-detected at init)
    bool use_simd16;

    // Fused V+H kernel mode: uses SLM intermediates, skips tmp buffers.
    // Saves ~70 MB VRAM at 4K but may be slower on some GPUs due to
    // SLM pressure and reduced occupancy.
    bool use_fused;

    // Deferred submit/collect state
    unsigned pending_index;
    bool has_pending;
};
} // namespace

/* ------------------------------------------------------------------ */
/* Options                                                             */
/* ------------------------------------------------------------------ */

const VmafOption options[] = {
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
    {.name = nullptr}};

/* ------------------------------------------------------------------ */
/* Device-side helpers (used inside kernels)                           */
/* ------------------------------------------------------------------ */

namespace
{
static inline int dev_mirror(int idx, int sup)
{
    if (idx < 0)
        return -idx;
    if (idx >= sup)
        return 2 * (sup - 1) - idx;
    return idx;
}
} // namespace

namespace
{
static inline uint32_t dev_get_best16_from32(uint32_t val, int &exp_out)
{
    if (val == 0) {
        exp_out = 0;
        return 0;
    }
    int msb = 31;
    while (msb >= 0 && !((val >> msb) & 1))
        msb--;
    int const k = (msb - 15 < 0) ? 0 : (msb - 15);
    val >>= k;
    exp_out = -k;
    return val & 0xFFFF;
}
} // namespace

namespace
{
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
} // namespace

namespace
{
template <int SCALE>
static inline uint32_t dev_read_pixel(const void *src, int y, int x, unsigned stride, unsigned bpc)
{
    if constexpr (SCALE == 0) {
        if (bpc <= 8) {
            return static_cast<const uint8_t *>(src)[y * stride + x];
        }
        return static_cast<const uint16_t *>(src)[y * (stride / 2) + x];
    } else {
        return static_cast<const uint32_t *>(src)[y * stride + x] & 0xFFFF;
    }
}
} // namespace

namespace
{
static inline uint32_t dev_quantize_sq(uint64_t acc, unsigned add_round, unsigned shift)
{
    if (shift > 0) {
        return static_cast<uint32_t>((acc + add_round) >> shift);
    }
    return static_cast<uint32_t>(acc);
}
} // namespace

/* ------------------------------------------------------------------ */
/* Profiling helper                                                   */
/* ------------------------------------------------------------------ */

namespace
{
static inline void sycl_profile_event(VmafSyclState *state, const char *name, sycl::event ev)
{
    if (vmaf_sycl_profiling_is_enabled(state)) {
        ev.wait();
        const uint64_t t0 = ev.get_profiling_info<sycl::info::event_profiling::command_start>();
        const uint64_t t1 = ev.get_profiling_info<sycl::info::event_profiling::command_end>();
        vmaf_sycl_profiling_record(state, name, t1 - t0);
    }
}
} // namespace

/* ------------------------------------------------------------------ */
/* SYCL Kernel: Vertical Pass Helpers                                */
/* ------------------------------------------------------------------ */

namespace
{
template <int SCALE, int TILE_H, int WG_X, int WG_Y>
static inline void dev_vert_load_tile(sycl::nd_item<2> item, const void *p_ref, const void *p_dis,
                                      unsigned width, unsigned height, unsigned src_stride,
                                      unsigned bpc, const sycl::local_accessor<uint32_t, 2> &s_ref,
                                      const sycl::local_accessor<uint32_t, 2> &s_dis)
{
    constexpr int FW = vif_fwidth[SCALE];
    constexpr int HALF_FW = FW / 2;
    constexpr unsigned tile_elems = TILE_H * WG_X;
    constexpr unsigned wg_size = WG_X * WG_Y;
    const unsigned lid = item.get_local_linear_id();

    const int tile_origin_y = (int)(item.get_group(0) * WG_Y) - HALF_FW;
    const int tile_col_x = (int)(item.get_group(1) * WG_X);

    const bool interior_wg = (tile_origin_y >= 0) && (tile_origin_y + TILE_H <= (int)height) &&
                             (tile_col_x + WG_X <= (int)width);

    if (interior_wg) {
        for (unsigned i = lid; i < tile_elems; i += wg_size) {
            const unsigned tr = i / WG_X;
            const unsigned tc = i % WG_X;
            const int px = tile_col_x + (int)tc;
            const int py = tile_origin_y + (int)tr;
            s_ref[tr][tc] = dev_read_pixel<SCALE>(p_ref, py, px, src_stride, bpc);
            s_dis[tr][tc] = dev_read_pixel<SCALE>(p_dis, py, px, src_stride, bpc);
        }
    } else {
        for (unsigned i = lid; i < tile_elems; i += wg_size) {
            const unsigned tr = i / WG_X;
            const unsigned tc = i % WG_X;
            int px = tile_col_x + (int)tc;
            const int py = dev_mirror(tile_origin_y + (int)tr, (int)height);
            if (std::cmp_less(px, width)) {
                px = dev_mirror(px, (int)width);
                s_ref[tr][tc] = dev_read_pixel<SCALE>(p_ref, py, px, src_stride, bpc);
                s_dis[tr][tc] = dev_read_pixel<SCALE>(p_dis, py, px, src_stride, bpc);
            } else {
                s_ref[tr][tc] = 0;
                s_dis[tr][tc] = 0;
            }
        }
    }
}
} // namespace

namespace
{
struct VifVertAccums {
    uint32_t mu1;
    uint32_t mu2;
    uint64_t ref;
    uint64_t dis;
    uint64_t ref_dis;
};
} // namespace

namespace
{
template <int FW>
static inline VifVertAccums dev_vert_accumulate(unsigned lx, unsigned ly, const uint32_t *fcoeff,
                                                const sycl::local_accessor<uint32_t, 2> &s_ref,
                                                const sycl::local_accessor<uint32_t, 2> &s_dis)
{
    VifVertAccums acc{};
    for (int fi = 0; fi < FW; fi++) {
        const uint32_t rv = s_ref[ly + fi][lx];
        const uint32_t dv = s_dis[ly + fi][lx];
        const uint32_t fc = fcoeff[fi];
        const uint32_t icr = fc * rv;
        const uint32_t icd = fc * dv;

        acc.mu1 += icr;
        acc.mu2 += icd;
        acc.ref += (uint64_t)icr * rv;
        acc.dis += (uint64_t)icd * dv;
        acc.ref_dis += (uint64_t)icr * dv;
    }
    return acc;
}
} // namespace

namespace
{
template <int SCALE, int FW, int FW_RD>
static inline void dev_vert_convolve_and_store(
    sycl::nd_item<2> item, unsigned width, unsigned height, unsigned shift_vp,
    unsigned add_shift_round_vp, unsigned shift_vp_sq, unsigned add_shift_round_vp_sq,
    const uint32_t *fcoeff, const uint32_t *fcoeff_rd,
    const sycl::local_accessor<uint32_t, 2> &s_ref, const sycl::local_accessor<uint32_t, 2> &s_dis,
    uint32_t *tmp_mu1, uint32_t *tmp_mu2, uint32_t *tmp_ref, uint32_t *tmp_dis,
    uint32_t *tmp_ref_dis, uint32_t *tmp_ref_convol, uint32_t *tmp_dis_convol)
{
    const int gx = item.get_global_id(1);
    const int gy = item.get_global_id(0);
    if (std::cmp_greater_equal(gx, width) || std::cmp_greater_equal(gy, height))
        return;

    const unsigned lx = item.get_local_id(1);
    const unsigned ly = item.get_local_id(0);
    constexpr int RD_START = (FW - FW_RD) / 2;

    const VifVertAccums acc = dev_vert_accumulate<FW>(lx, ly, fcoeff, s_ref, s_dis);
    const unsigned idx = gy * width + gx;
    tmp_mu1[idx] = static_cast<uint32_t>((acc.mu1 + add_shift_round_vp) >> shift_vp);
    tmp_mu2[idx] = static_cast<uint32_t>((acc.mu2 + add_shift_round_vp) >> shift_vp);
    tmp_ref[idx] = dev_quantize_sq(acc.ref, add_shift_round_vp_sq, shift_vp_sq);
    tmp_dis[idx] = dev_quantize_sq(acc.dis, add_shift_round_vp_sq, shift_vp_sq);
    tmp_ref_dis[idx] = dev_quantize_sq(acc.ref_dis, add_shift_round_vp_sq, shift_vp_sq);

    if constexpr (FW_RD > 0) {
        uint32_t acc_ref_rd = 0;
        uint32_t acc_dis_rd = 0;
        for (int fi = RD_START; fi < RD_START + FW_RD; fi++) {
            const uint32_t fc_rd = fcoeff_rd[fi - RD_START];
            acc_ref_rd += fc_rd * s_ref[ly + fi][lx];
            acc_dis_rd += fc_rd * s_dis[ly + fi][lx];
        }
        tmp_ref_convol[idx] = static_cast<uint32_t>((acc_ref_rd + add_shift_round_vp) >> shift_vp);
        tmp_dis_convol[idx] = static_cast<uint32_t>((acc_dis_rd + add_shift_round_vp) >> shift_vp);
    } else {
        tmp_ref_convol[idx] = 0;
        tmp_dis_convol[idx] = 0;
    }
}
} // namespace

namespace
{
template <int SCALE>
static sycl::event launch_vif_vert_impl(sycl::queue &q, const void *ref_data, const void *dis_data,
                                        unsigned width, unsigned height, unsigned src_stride,
                                        unsigned bpc, uint32_t *tmp_mu1, uint32_t *tmp_mu2,
                                        uint32_t *tmp_ref, uint32_t *tmp_dis, uint32_t *tmp_ref_dis,
                                        uint32_t *tmp_ref_convol, uint32_t *tmp_dis_convol)
{
    constexpr int FW = vif_fwidth[SCALE];
    constexpr int FW_RD = vif_fwidth_rd[SCALE];
    constexpr int TILE_H = 16 + FW - 1;

    const unsigned shift_vp = (SCALE == 0) ? bpc : 16;
    const unsigned add_shift_round_vp = (SCALE == 0) ? (1u << (bpc - 1)) : 32768;
    const unsigned shift_vp_sq = (SCALE == 0) ? ((bpc - 8) * 2) : 16;
    const unsigned add_shift_round_vp_sq =
        (SCALE == 0) ? ((bpc == 8) ? 0 : (1u << (shift_vp_sq - 1))) : 32768;

    uint32_t fcoeff[VIF_FILTER_MAX_WIDTH];
    for (int i = 0; i < FW; i++)
        fcoeff[i] = vif_filter1d_table[SCALE][i];

    uint32_t fcoeff_rd[VIF_FILTER_MAX_WIDTH] = {};
    if constexpr (FW_RD > 0) {
        for (int i = 0; i < FW_RD; i++)
            fcoeff_rd[i] = vif_filter1d_table[SCALE + 1][i];
    }

    constexpr int WG_X = 16;
    constexpr int WG_Y = 16;
    const sycl::range<2> global(((static_cast<size_t>(height) + WG_Y - 1) / WG_Y) * WG_Y,
                                ((static_cast<size_t>(width) + WG_X - 1) / WG_X) * WG_X);
    const sycl::range<2> local(WG_Y, WG_X);

    return q.submit([&](sycl::handler &cgh) {
        sycl::local_accessor<uint32_t, 2> const s_ref(sycl::range<2>(TILE_H, WG_X), cgh);
        sycl::local_accessor<uint32_t, 2> const s_dis(sycl::range<2>(TILE_H, WG_X), cgh);

        cgh.parallel_for(sycl::nd_range<2>(global, local), [=](sycl::nd_item<2> item) {
            dev_vert_load_tile<SCALE, TILE_H, WG_X, WG_Y>(item, ref_data, dis_data, width, height,
                                                          src_stride, bpc, s_ref, s_dis);
            item.barrier(sycl::access::fence_space::local_space);
            dev_vert_convolve_and_store<SCALE, FW, FW_RD>(
                item, width, height, shift_vp, add_shift_round_vp, shift_vp_sq,
                add_shift_round_vp_sq, fcoeff, fcoeff_rd, s_ref, s_dis, tmp_mu1, tmp_mu2, tmp_ref,
                tmp_dis, tmp_ref_dis, tmp_ref_convol, tmp_dis_convol);
        });
    });
}
} // namespace

namespace
{
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
} // namespace

/* ------------------------------------------------------------------ */
/* SYCL Kernel: Horizontal Pass & Shared Statistics Helpers           */
/* ------------------------------------------------------------------ */

namespace
{
static inline void dev_vif_stats_log_domain(int32_t sigma1_sq, int32_t sigma2_sq, int32_t sigma12,
                                            float vif_enhn_gain_limit, const uint32_t *log2_lut,
                                            vif_accums &acc)
{
    float g = 0.0f;
    float sv_sq = 0.0f;
    float gg_sigma_f = 0.0f;

    if (sigma12 > 0 && sigma1_sq != 0 && sigma2_sq != 0) {
        g = static_cast<float>(sigma12) / static_cast<float>(sigma1_sq);
        sv_sq = static_cast<float>(sigma2_sq) - g * static_cast<float>(sigma12);
        if (sv_sq < 0.0f)
            sv_sq = 0.0f;
        g = sycl::fmin(g, vif_enhn_gain_limit);
        gg_sigma_f = g * g * static_cast<float>(sigma1_sq);
    }

    const uint32_t log_den_stage1 = static_cast<uint32_t>(SIGMA_NSQ + sigma1_sq);
    int x_exp = 0;
    const uint32_t log_den1 = dev_get_best16_from32(log_den_stage1, x_exp);

    acc.num_x = 1;
    acc.x = x_exp;

    const uint32_t den_val = log2_lut[log_den1 - 32768];

    if (sigma12 >= 0) {
        const uint32_t numer1 = static_cast<uint32_t>(sv_sq) + static_cast<uint32_t>(SIGMA_NSQ);
        const uint64_t numer1_tmp =
            static_cast<uint64_t>(static_cast<int64_t>(gg_sigma_f)) + static_cast<uint64_t>(numer1);

        int x1 = 0;
        int x2_val = 0;
        const uint32_t numlog = dev_get_best16_from64(numer1_tmp, x1);
        const uint32_t denlog = dev_get_best16_from64(static_cast<uint64_t>(numer1), x2_val);

        acc.x2 = x2_val - x1;

        const int32_t num_val = static_cast<int32_t>(log2_lut[numlog - 32768]) -
                                static_cast<int32_t>(log2_lut[denlog - 32768]);
        acc.num_log = static_cast<int64_t>(num_val);
    }

    acc.den_log = static_cast<int64_t>(den_val);
}
} // namespace

namespace
{
static inline vif_accums dev_compute_vif_stats(uint32_t h_mu1, uint32_t h_mu2, uint64_t h_ref,
                                               uint64_t h_dis, uint64_t h_ref_dis,
                                               float vif_enhn_gain_limit, const uint32_t *log2_lut)
{
    vif_accums acc = {};
    const uint32_t mu1_val = h_mu1;
    const uint32_t mu2_val = h_mu2;
    const uint32_t xx_filt = static_cast<uint32_t>((h_ref + 32768) >> 16);
    const uint32_t yy_filt = static_cast<uint32_t>((h_dis + 32768) >> 16);
    const uint32_t xy_filt = static_cast<uint32_t>((h_ref_dis + 32768) >> 16);

    const uint32_t mu1_sq =
        static_cast<uint32_t>(((uint64_t)mu1_val * mu1_val + 2147483648ULL) >> 32);
    const uint32_t mu2_sq =
        static_cast<uint32_t>(((uint64_t)mu2_val * mu2_val + 2147483648ULL) >> 32);
    const uint32_t mu1_mu2 =
        static_cast<uint32_t>(((uint64_t)mu1_val * mu2_val + 2147483648ULL) >> 32);

    int32_t sigma1_sq = static_cast<int32_t>(xx_filt - mu1_sq);
    int32_t sigma2_sq = static_cast<int32_t>(yy_filt - mu2_sq);
    const int32_t sigma12 = static_cast<int32_t>(xy_filt - mu1_mu2);
    if (sigma1_sq < 0)
        sigma1_sq = 0;
    if (sigma2_sq < 0)
        sigma2_sq = 0;

    if (sigma1_sq >= static_cast<int32_t>(SIGMA_NSQ)) {
        dev_vif_stats_log_domain(sigma1_sq, sigma2_sq, sigma12, vif_enhn_gain_limit, log2_lut, acc);
    } else {
        acc.num_non_log = sigma2_sq;
        acc.den_non_log = 1;
    }
    return acc;
}
} // namespace

namespace
{
template <int MAX_SUBGROUPS>
static inline void dev_reduce_and_accum(sycl::nd_item<2> item, const vif_accums &t_acc,
                                        const sycl::local_accessor<int64_t, 1> &lmem,
                                        int64_t *accum)
{
    const sycl::sub_group sg = item.get_sub_group();
    const uint32_t sg_id = sg.get_group_linear_id();
    const uint32_t sg_lid = sg.get_local_linear_id();
    const uint32_t n_subgroups = sg.get_group_linear_range();

    const int64_t sg_x = sycl::reduce_over_group(sg, t_acc.x, sycl::plus<int64_t>());
    const int64_t sg_x2 = sycl::reduce_over_group(sg, t_acc.x2, sycl::plus<int64_t>());
    const int64_t sg_num_x = sycl::reduce_over_group(sg, t_acc.num_x, sycl::plus<int64_t>());
    const int64_t sg_num_log = sycl::reduce_over_group(sg, t_acc.num_log, sycl::plus<int64_t>());
    const int64_t sg_den_log = sycl::reduce_over_group(sg, t_acc.den_log, sycl::plus<int64_t>());
    const int64_t sg_num_nlog =
        sycl::reduce_over_group(sg, t_acc.num_non_log, sycl::plus<int64_t>());
    const int64_t sg_den_nlog =
        sycl::reduce_over_group(sg, t_acc.den_non_log, sycl::plus<int64_t>());

    if (sg_lid == 0) {
        lmem[0 * MAX_SUBGROUPS + sg_id] = sg_x;
        lmem[1 * MAX_SUBGROUPS + sg_id] = sg_x2;
        lmem[2 * MAX_SUBGROUPS + sg_id] = sg_num_x;
        lmem[3 * MAX_SUBGROUPS + sg_id] = sg_num_log;
        lmem[4 * MAX_SUBGROUPS + sg_id] = sg_den_log;
        lmem[5 * MAX_SUBGROUPS + sg_id] = sg_num_nlog;
        lmem[6 * MAX_SUBGROUPS + sg_id] = sg_den_nlog;
    }

    item.barrier(sycl::access::fence_space::local_space);

    const int lid = item.get_local_linear_id();
    if (lid == 0) {
        int64_t final_vals[ACCUM_FIELDS] = {};
        for (uint32_t s = 0; s < n_subgroups; s++) {
            final_vals[0] += lmem[0 * MAX_SUBGROUPS + s];
            final_vals[1] += lmem[1 * MAX_SUBGROUPS + s];
            final_vals[2] += lmem[2 * MAX_SUBGROUPS + s];
            final_vals[3] += lmem[3 * MAX_SUBGROUPS + s];
            final_vals[4] += lmem[4 * MAX_SUBGROUPS + s];
            final_vals[5] += lmem[5 * MAX_SUBGROUPS + s];
            final_vals[6] += lmem[6 * MAX_SUBGROUPS + s];
        }
        for (int f = 0; f < ACCUM_FIELDS; f++) {
            const sycl::atomic_ref<int64_t, sycl::memory_order::relaxed, sycl::memory_scope::device,
                                   sycl::access::address_space::global_space>
                ref(accum[f]);
            ref.fetch_add(final_vals[f]);
        }
    }
}
} // namespace

namespace
{
template <int FW_RD>
static inline void dev_downsample_rd(int gx, int gy, bool valid, unsigned width, uint32_t h_ref_rd,
                                     uint32_t h_dis_rd, uint32_t *rd_ref, uint32_t *rd_dis)
{
    if constexpr (FW_RD > 0) {
        if (valid && (gx % 2 == 0) && (gy % 2 == 0)) {
            const uint32_t ref_rd_val = static_cast<uint32_t>((h_ref_rd + 32768) >> 16);
            const uint32_t dis_rd_val = static_cast<uint32_t>((h_dis_rd + 32768) >> 16);
            const unsigned rd_x = gx / 2;
            const unsigned rd_y = gy / 2;
            const unsigned rd_stride = (width + 1U) / 2U;
            rd_ref[rd_y * rd_stride + rd_x] = ref_rd_val & 0xFFFF;
            rd_dis[rd_y * rd_stride + rd_x] = dis_rd_val & 0xFFFF;
        }
    }
}
} // namespace

namespace
{
template <int FW, int FW_RD>
static inline void dev_hori_convolve_interior(
    unsigned buf_base, const uint32_t *fcoeff, const uint32_t *fcoeff_rd, const uint32_t *tmp_mu1,
    const uint32_t *tmp_mu2, const uint32_t *tmp_ref, const uint32_t *tmp_dis,
    const uint32_t *tmp_ref_dis, const uint32_t *tmp_ref_convol, const uint32_t *tmp_dis_convol,
    uint32_t &h_mu1, uint32_t &h_mu2, uint64_t &h_ref, uint64_t &h_dis, uint64_t &h_ref_dis,
    uint32_t &h_ref_rd, uint32_t &h_dis_rd)
{
    constexpr int HALF_FW = FW / 2;
    constexpr int RD_START = (FW - FW_RD) / 2;
    const unsigned ci = buf_base + (unsigned)HALF_FW;
    const uint32_t fcc = fcoeff[HALF_FW];

    h_mu1 += fcc * tmp_mu1[ci];
    h_mu2 += fcc * tmp_mu2[ci];
    h_ref += (uint64_t)fcc * tmp_ref[ci];
    h_dis += (uint64_t)fcc * tmp_dis[ci];
    h_ref_dis += (uint64_t)fcc * tmp_ref_dis[ci];
    if constexpr (FW_RD > 0) {
        const uint32_t fcc_rd = fcoeff_rd[FW_RD / 2];
        h_ref_rd += fcc_rd * tmp_ref_convol[ci];
        h_dis_rd += fcc_rd * tmp_dis_convol[ci];
    }

    for (int fj = 0; fj < HALF_FW; fj++) {
        const unsigned idx_lo = buf_base + (unsigned)fj;
        const unsigned idx_hi = buf_base + (unsigned)(FW - 1 - fj);
        const uint32_t fc = fcoeff[fj];

        h_mu1 += fc * (tmp_mu1[idx_lo] + tmp_mu1[idx_hi]);
        h_mu2 += fc * (tmp_mu2[idx_lo] + tmp_mu2[idx_hi]);
        h_ref += (uint64_t)fc * ((uint64_t)tmp_ref[idx_lo] + tmp_ref[idx_hi]);
        h_dis += (uint64_t)fc * ((uint64_t)tmp_dis[idx_lo] + tmp_dis[idx_hi]);
        h_ref_dis += (uint64_t)fc * ((uint64_t)tmp_ref_dis[idx_lo] + tmp_ref_dis[idx_hi]);

        if constexpr (FW_RD > 0) {
            if (fj >= RD_START && (int)fj < RD_START + FW_RD / 2) {
                const uint32_t fc_rd = fcoeff_rd[fj - RD_START];
                h_ref_rd += fc_rd * (tmp_ref_convol[idx_lo] + tmp_ref_convol[idx_hi]);
                h_dis_rd += fc_rd * (tmp_dis_convol[idx_lo] + tmp_dis_convol[idx_hi]);
            }
        }
    }
}
} // namespace

namespace
{
template <int FW, int FW_RD>
static inline void dev_hori_convolve_border(
    int gx, unsigned buf_row, unsigned width, const uint32_t *fcoeff, const uint32_t *fcoeff_rd,
    const uint32_t *tmp_mu1, const uint32_t *tmp_mu2, const uint32_t *tmp_ref,
    const uint32_t *tmp_dis, const uint32_t *tmp_ref_dis, const uint32_t *tmp_ref_convol,
    const uint32_t *tmp_dis_convol, uint32_t &h_mu1, uint32_t &h_mu2, uint64_t &h_ref,
    uint64_t &h_dis, uint64_t &h_ref_dis, uint32_t &h_ref_rd, uint32_t &h_dis_rd)
{
    constexpr int HALF_FW = FW / 2;
    constexpr int RD_START = (FW - FW_RD) / 2;

    for (int fi = 0; fi < FW; fi++) {
        const int sx = dev_mirror(gx - HALF_FW + fi, (int)width);
        const unsigned sidx = buf_row + sx;
        const uint32_t fc = fcoeff[fi];

        h_mu1 += fc * tmp_mu1[sidx];
        h_mu2 += fc * tmp_mu2[sidx];
        h_ref += (uint64_t)fc * tmp_ref[sidx];
        h_dis += (uint64_t)fc * tmp_dis[sidx];
        h_ref_dis += (uint64_t)fc * tmp_ref_dis[sidx];

        if constexpr (FW_RD > 0) {
            if (fi >= RD_START && fi < RD_START + FW_RD) {
                const uint32_t fc_rd = fcoeff_rd[fi - RD_START];
                h_ref_rd += fc_rd * tmp_ref_convol[sidx];
                h_dis_rd += fc_rd * tmp_dis_convol[sidx];
            }
        }
    }
}
} // namespace

namespace
{
template <int SCALE, int FW, int FW_RD>
static inline void
dev_hori_convolve_tmp(int gx, int gy, unsigned width, const uint32_t *fcoeff,
                      const uint32_t *fcoeff_rd, const uint32_t *tmp_mu1, const uint32_t *tmp_mu2,
                      const uint32_t *tmp_ref, const uint32_t *tmp_dis, const uint32_t *tmp_ref_dis,
                      const uint32_t *tmp_ref_convol, const uint32_t *tmp_dis_convol,
                      uint32_t &h_mu1, uint32_t &h_mu2, uint64_t &h_ref, uint64_t &h_dis,
                      uint64_t &h_ref_dis, uint32_t &h_ref_rd, uint32_t &h_dis_rd)
{
    constexpr int HALF_FW = FW / 2;
    const unsigned buf_row = gy * width;

    if (gx >= HALF_FW && gx < (int)width - HALF_FW) {
        const unsigned buf_base = buf_row + (unsigned)(gx - HALF_FW);
        dev_hori_convolve_interior<FW, FW_RD>(buf_base, fcoeff, fcoeff_rd, tmp_mu1, tmp_mu2,
                                              tmp_ref, tmp_dis, tmp_ref_dis, tmp_ref_convol,
                                              tmp_dis_convol, h_mu1, h_mu2, h_ref, h_dis, h_ref_dis,
                                              h_ref_rd, h_dis_rd);
    } else {
        dev_hori_convolve_border<FW, FW_RD>(gx, buf_row, width, fcoeff, fcoeff_rd, tmp_mu1, tmp_mu2,
                                            tmp_ref, tmp_dis, tmp_ref_dis, tmp_ref_convol,
                                            tmp_dis_convol, h_mu1, h_mu2, h_ref, h_dis, h_ref_dis,
                                            h_ref_rd, h_dis_rd);
    }
}
} // namespace

namespace
{
struct VifHoriLaunchParams {
    unsigned width;
    unsigned height;
    float vif_enhn_gain_limit;
    const uint32_t *tmp_mu1;
    const uint32_t *tmp_mu2;
    const uint32_t *tmp_ref;
    const uint32_t *tmp_dis;
    const uint32_t *tmp_ref_dis;
    const uint32_t *tmp_ref_convol;
    const uint32_t *tmp_dis_convol;
    int64_t *accum;
    uint32_t *rd_ref;
    uint32_t *rd_dis;
    const uint32_t *log2_lut;
};
} // namespace

namespace
{
template <int SCALE, int FW, int FW_RD, int MAX_SUBGROUPS>
static inline void dev_hori_item_step(sycl::nd_item<2> item, const VifHoriLaunchParams &p,
                                      const uint32_t *fcoeff, const uint32_t *fcoeff_rd,
                                      const sycl::local_accessor<int64_t, 1> &lmem)
{
    const int gx = item.get_global_id(1);
    const int gy = item.get_global_id(0);
    const bool valid = (std::cmp_less(gx, p.width) && std::cmp_less(gy, p.height));
    vif_accums t_acc = {};
    uint32_t h_ref_rd = 0;
    uint32_t h_dis_rd = 0;

    if (valid) {
        uint32_t h_mu1 = 0;
        uint32_t h_mu2 = 0;
        uint64_t h_ref = 0;
        uint64_t h_dis = 0;
        uint64_t h_ref_dis = 0;
        dev_hori_convolve_tmp<SCALE, FW, FW_RD>(gx, gy, p.width, fcoeff, fcoeff_rd, p.tmp_mu1,
                                                p.tmp_mu2, p.tmp_ref, p.tmp_dis, p.tmp_ref_dis,
                                                p.tmp_ref_convol, p.tmp_dis_convol, h_mu1, h_mu2,
                                                h_ref, h_dis, h_ref_dis, h_ref_rd, h_dis_rd);
        t_acc = dev_compute_vif_stats(h_mu1, h_mu2, h_ref, h_dis, h_ref_dis, p.vif_enhn_gain_limit,
                                      p.log2_lut);
    }

    dev_reduce_and_accum<MAX_SUBGROUPS>(item, t_acc, lmem, p.accum);
    dev_downsample_rd<FW_RD>(gx, gy, valid, p.width, h_ref_rd, h_dis_rd, p.rd_ref, p.rd_dis);
}
} // namespace

namespace
{
template <int SCALE, int SG_SIZE>
static sycl::event
launch_vif_hori_impl(sycl::queue &q, unsigned width, unsigned height, float vif_enhn_gain_limit,
                     const uint32_t *tmp_mu1, const uint32_t *tmp_mu2, const uint32_t *tmp_ref,
                     const uint32_t *tmp_dis, const uint32_t *tmp_ref_dis,
                     const uint32_t *tmp_ref_convol, const uint32_t *tmp_dis_convol, int64_t *accum,
                     uint32_t *rd_ref, uint32_t *rd_dis, const uint32_t *log2_lut)
{
    constexpr int FW = vif_fwidth[SCALE];
    constexpr int FW_RD = vif_fwidth_rd[SCALE];
    uint32_t fcoeff[VIF_FILTER_MAX_WIDTH];
    for (int i = 0; i < FW; i++)
        fcoeff[i] = vif_filter1d_table[SCALE][i];

    uint32_t fcoeff_rd[VIF_FILTER_MAX_WIDTH] = {};
    if constexpr (FW_RD > 0) {
        for (int i = 0; i < FW_RD; i++)
            fcoeff_rd[i] = vif_filter1d_table[SCALE + 1][i];
    }

    constexpr int WG_X = 16;
    constexpr int WG_Y = 16;
    constexpr int MAX_SUBGROUPS = 32;
    const sycl::range<2> global(((static_cast<size_t>(height) + WG_Y - 1) / WG_Y) * WG_Y,
                                ((static_cast<size_t>(width) + WG_X - 1) / WG_X) * WG_X);
    const sycl::range<2> local(WG_Y, WG_X);
    const VifHoriLaunchParams p = {
        .width = width,
        .height = height,
        .vif_enhn_gain_limit = vif_enhn_gain_limit,
        .tmp_mu1 = tmp_mu1,
        .tmp_mu2 = tmp_mu2,
        .tmp_ref = tmp_ref,
        .tmp_dis = tmp_dis,
        .tmp_ref_dis = tmp_ref_dis,
        .tmp_ref_convol = tmp_ref_convol,
        .tmp_dis_convol = tmp_dis_convol,
        .accum = accum,
        .rd_ref = rd_ref,
        .rd_dis = rd_dis,
        .log2_lut = log2_lut,
    };

    return q.submit([&](sycl::handler &cgh) {
        sycl::local_accessor<int64_t, 1> const lmem(
            sycl::range<1>(static_cast<size_t>(ACCUM_FIELDS) * MAX_SUBGROUPS), cgh);

        cgh.parallel_for(sycl::nd_range<2>(global, local),
                         [=](sycl::nd_item<2> item) VMAF_SYCL_REQD_SG_SIZE(SG_SIZE) {
                             dev_hori_item_step<SCALE, FW, FW_RD, MAX_SUBGROUPS>(item, p, fcoeff,
                                                                                 fcoeff_rd, lmem);
                         });
    });
}
} // namespace

namespace
{
static sycl::event launch_vif_hori_v2(sycl::queue &q, int scale, unsigned width, unsigned height,
                                      float vif_enhn_gain_limit, uint32_t *tmp_mu1,
                                      uint32_t *tmp_mu2, uint32_t *tmp_ref, uint32_t *tmp_dis,
                                      uint32_t *tmp_ref_dis, uint32_t *tmp_ref_convol,
                                      uint32_t *tmp_dis_convol, int64_t *accum, uint32_t *rd_ref,
                                      uint32_t *rd_dis, const uint32_t *log2_lut)
{
    switch (scale) {
    case 0:
        return launch_vif_hori_impl<0, 32>(q, width, height, vif_enhn_gain_limit, tmp_mu1, tmp_mu2,
                                           tmp_ref, tmp_dis, tmp_ref_dis, tmp_ref_convol,
                                           tmp_dis_convol, accum, rd_ref, rd_dis, log2_lut);
    case 1:
        return launch_vif_hori_impl<1, 32>(q, width, height, vif_enhn_gain_limit, tmp_mu1, tmp_mu2,
                                           tmp_ref, tmp_dis, tmp_ref_dis, tmp_ref_convol,
                                           tmp_dis_convol, accum, rd_ref, rd_dis, log2_lut);
    case 2:
        return launch_vif_hori_impl<2, 32>(q, width, height, vif_enhn_gain_limit, tmp_mu1, tmp_mu2,
                                           tmp_ref, tmp_dis, tmp_ref_dis, tmp_ref_convol,
                                           tmp_dis_convol, accum, rd_ref, rd_dis, log2_lut);
    default:
        return launch_vif_hori_impl<3, 32>(q, width, height, vif_enhn_gain_limit, tmp_mu1, tmp_mu2,
                                           tmp_ref, tmp_dis, tmp_ref_dis, tmp_ref_convol,
                                           tmp_dis_convol, accum, rd_ref, rd_dis, log2_lut);
    }
}
} // namespace

namespace
{
static sycl::event launch_vif_hori_v2_sg16(sycl::queue &q, int scale, unsigned width,
                                           unsigned height, float vif_enhn_gain_limit,
                                           uint32_t *tmp_mu1, uint32_t *tmp_mu2, uint32_t *tmp_ref,
                                           uint32_t *tmp_dis, uint32_t *tmp_ref_dis,
                                           uint32_t *tmp_ref_convol, uint32_t *tmp_dis_convol,
                                           int64_t *accum, uint32_t *rd_ref, uint32_t *rd_dis,
                                           const uint32_t *log2_lut)
{
    switch (scale) {
    case 0:
        return launch_vif_hori_impl<0, 16>(q, width, height, vif_enhn_gain_limit, tmp_mu1, tmp_mu2,
                                           tmp_ref, tmp_dis, tmp_ref_dis, tmp_ref_convol,
                                           tmp_dis_convol, accum, rd_ref, rd_dis, log2_lut);
    case 1:
        return launch_vif_hori_impl<1, 16>(q, width, height, vif_enhn_gain_limit, tmp_mu1, tmp_mu2,
                                           tmp_ref, tmp_dis, tmp_ref_dis, tmp_ref_convol,
                                           tmp_dis_convol, accum, rd_ref, rd_dis, log2_lut);
    case 2:
        return launch_vif_hori_impl<2, 16>(q, width, height, vif_enhn_gain_limit, tmp_mu1, tmp_mu2,
                                           tmp_ref, tmp_dis, tmp_ref_dis, tmp_ref_convol,
                                           tmp_dis_convol, accum, rd_ref, rd_dis, log2_lut);
    default:
        return launch_vif_hori_impl<3, 16>(q, width, height, vif_enhn_gain_limit, tmp_mu1, tmp_mu2,
                                           tmp_ref, tmp_dis, tmp_ref_dis, tmp_ref_convol,
                                           tmp_dis_convol, accum, rd_ref, rd_dis, log2_lut);
    }
}
} // namespace

/* ------------------------------------------------------------------ */
/* SYCL Kernel: Fused Vertical + Horizontal Helpers                   */
/* ------------------------------------------------------------------ */

namespace
{
template <int SCALE, int TILE_H, int TILE_W, int WG_SIZE>
static inline void dev_fused_load_tile(sycl::nd_item<2> item, const void *p_ref, const void *p_dis,
                                       unsigned width, unsigned height, unsigned src_stride,
                                       unsigned bpc, const sycl::local_accessor<uint32_t, 1> &s_ref,
                                       const sycl::local_accessor<uint32_t, 1> &s_dis)
{
    constexpr int FW_V = vif_fwidth[SCALE];
    constexpr int HALF_FW_V = FW_V / 2;
    constexpr int FW_H = FW_V;
    constexpr int HALF_FW_H = FW_H / 2;
    constexpr int WG_X = 16;
    constexpr int WG_Y = 8;
    constexpr unsigned tile_elems = TILE_H * TILE_W;
    const unsigned lid = item.get_local_linear_id();

    const int tile_origin_y = (int)(item.get_group(0) * WG_Y) - HALF_FW_V;
    const int tile_origin_x = (int)(item.get_group(1) * WG_X) - HALF_FW_H;

    const bool interior_wg = (tile_origin_y >= 0) && (tile_origin_y + TILE_H <= (int)height) &&
                             (tile_origin_x >= 0) && (tile_origin_x + TILE_W <= (int)width);

    if (interior_wg) {
        for (unsigned i = lid; i < tile_elems; i += WG_SIZE) {
            const unsigned tr = i / TILE_W;
            const unsigned tc = i % TILE_W;
            const int py = tile_origin_y + (int)tr;
            const int px = tile_origin_x + (int)tc;
            s_ref[tr * TILE_W + tc] = dev_read_pixel<SCALE>(p_ref, py, px, src_stride, bpc);
            s_dis[tr * TILE_W + tc] = dev_read_pixel<SCALE>(p_dis, py, px, src_stride, bpc);
        }
    } else {
        for (unsigned i = lid; i < tile_elems; i += WG_SIZE) {
            const unsigned tr = i / TILE_W;
            const unsigned tc = i % TILE_W;
            const int py = dev_mirror(tile_origin_y + (int)tr, (int)height);
            const int px = dev_mirror(tile_origin_x + (int)tc, (int)width);
            s_ref[tr * TILE_W + tc] = dev_read_pixel<SCALE>(p_ref, py, px, src_stride, bpc);
            s_dis[tr * TILE_W + tc] = dev_read_pixel<SCALE>(p_dis, py, px, src_stride, bpc);
        }
    }
}
} // namespace

namespace
{
template <int FW_V, int FW_RD, int TILE_W>
static inline void dev_fused_vert_accum_pixel(unsigned r, unsigned c, const uint32_t *fcoeff,
                                              const uint32_t *fcoeff_rd,
                                              const sycl::local_accessor<uint32_t, 1> &s_ref,
                                              const sycl::local_accessor<uint32_t, 1> &s_dis,
                                              uint32_t &a_mu1, uint32_t &a_mu2, uint64_t &a_ref,
                                              uint64_t &a_dis, uint64_t &a_ref_dis,
                                              uint32_t &a_ref_rd, uint32_t &a_dis_rd)
{
    constexpr int RD_START = (FW_V - FW_RD) / 2;

    for (int fi = 0; fi < FW_V; fi++) {
        const unsigned sidx = (r + (unsigned)fi) * TILE_W + c;
        const uint32_t rv = s_ref[sidx];
        const uint32_t dv = s_dis[sidx];
        const uint32_t fc = fcoeff[fi];
        const uint32_t icr = fc * rv;
        const uint32_t icd = fc * dv;

        a_mu1 += icr;
        a_mu2 += icd;
        a_ref += (uint64_t)icr * rv;
        a_dis += (uint64_t)icd * dv;
        a_ref_dis += (uint64_t)icr * dv;

        if constexpr (FW_RD > 0) {
            if (fi >= RD_START && fi < RD_START + FW_RD) {
                const uint32_t fc_rd = fcoeff_rd[fi - RD_START];
                a_ref_rd += fc_rd * rv;
                a_dis_rd += fc_rd * dv;
            }
        }
    }
}
} // namespace

namespace
{
template <int SCALE, int FW_V, int FW_RD, int TILE_W, unsigned VERT_TOTAL, int WG_SIZE>
static inline void
dev_fused_vert_conv(sycl::nd_item<2> item, unsigned shift_vp, unsigned add_shift_round_vp,
                    unsigned shift_vp_sq, unsigned add_shift_round_vp_sq, const uint32_t *fcoeff,
                    const uint32_t *fcoeff_rd, const sycl::local_accessor<uint32_t, 1> &s_ref,
                    const sycl::local_accessor<uint32_t, 1> &s_dis,
                    const sycl::local_accessor<uint32_t, 1> &s_vert)
{
    const unsigned lid = item.get_local_linear_id();

    for (unsigned i = lid; i < VERT_TOTAL; i += WG_SIZE) {
        const unsigned r = i / TILE_W;
        const unsigned c = i % TILE_W;

        uint32_t a_mu1 = 0;
        uint32_t a_mu2 = 0;
        uint64_t a_ref = 0;
        uint64_t a_dis = 0;
        uint64_t a_ref_dis = 0;
        uint32_t a_ref_rd = 0;
        uint32_t a_dis_rd = 0;

        dev_fused_vert_accum_pixel<FW_V, FW_RD, TILE_W>(r, c, fcoeff, fcoeff_rd, s_ref, s_dis,
                                                        a_mu1, a_mu2, a_ref, a_dis, a_ref_dis,
                                                        a_ref_rd, a_dis_rd);

        const unsigned base = r * TILE_W + c;
        s_vert[0 * VERT_TOTAL + base] =
            static_cast<uint32_t>((a_mu1 + add_shift_round_vp) >> shift_vp);
        s_vert[1 * VERT_TOTAL + base] =
            static_cast<uint32_t>((a_mu2 + add_shift_round_vp) >> shift_vp);
        s_vert[2 * VERT_TOTAL + base] = dev_quantize_sq(a_ref, add_shift_round_vp_sq, shift_vp_sq);
        s_vert[3 * VERT_TOTAL + base] = dev_quantize_sq(a_dis, add_shift_round_vp_sq, shift_vp_sq);
        s_vert[4 * VERT_TOTAL + base] =
            dev_quantize_sq(a_ref_dis, add_shift_round_vp_sq, shift_vp_sq);

        if constexpr (FW_RD > 0) {
            s_vert[5 * VERT_TOTAL + base] =
                static_cast<uint32_t>((a_ref_rd + add_shift_round_vp) >> shift_vp);
            s_vert[6 * VERT_TOTAL + base] =
                static_cast<uint32_t>((a_dis_rd + add_shift_round_vp) >> shift_vp);
        }
    }
}
} // namespace

namespace
{
template <int FW_H, int FW_RD, int TILE_W, unsigned VERT_TOTAL>
static inline void
dev_fused_hori_conv(unsigned lx, unsigned ly, const uint32_t *fcoeff, const uint32_t *fcoeff_rd,
                    const sycl::local_accessor<uint32_t, 1> &s_vert, uint32_t &h_mu1,
                    uint32_t &h_mu2, uint64_t &h_ref, uint64_t &h_dis, uint64_t &h_ref_dis,
                    uint32_t &h_ref_rd, uint32_t &h_dis_rd)
{
    constexpr int HALF_FW_H = FW_H / 2;
    constexpr int RD_START = (FW_H - FW_RD) / 2;
    constexpr bool DO_RD = (FW_RD > 0);

    auto sv = [&](int ch, unsigned r, unsigned c) -> uint32_t {
        return s_vert[ch * VERT_TOTAL + r * TILE_W + c];
    };

    const uint32_t fcc = fcoeff[HALF_FW_H];
    const unsigned cc = lx + (unsigned)HALF_FW_H;
    h_mu1 += fcc * sv(0, ly, cc);
    h_mu2 += fcc * sv(1, ly, cc);
    h_ref += (uint64_t)fcc * sv(2, ly, cc);
    h_dis += (uint64_t)fcc * sv(3, ly, cc);
    h_ref_dis += (uint64_t)fcc * sv(4, ly, cc);
    if constexpr (DO_RD) {
        constexpr int RD_HALF = FW_RD / 2;
        h_ref_rd += fcoeff_rd[RD_HALF] * sv(5, ly, cc);
        h_dis_rd += fcoeff_rd[RD_HALF] * sv(6, ly, cc);
    }

    for (int fj = 0; fj < HALF_FW_H; fj++) {
        const uint32_t fc = fcoeff[fj];
        const unsigned lo_c = lx + (unsigned)fj;
        const unsigned hi_c = lx + (unsigned)(FW_H - 1 - fj);

        h_mu1 += fc * (sv(0, ly, lo_c) + sv(0, ly, hi_c));
        h_mu2 += fc * (sv(1, ly, lo_c) + sv(1, ly, hi_c));
        h_ref += (uint64_t)fc * ((uint64_t)sv(2, ly, lo_c) + sv(2, ly, hi_c));
        h_dis += (uint64_t)fc * ((uint64_t)sv(3, ly, lo_c) + sv(3, ly, hi_c));
        h_ref_dis += (uint64_t)fc * ((uint64_t)sv(4, ly, lo_c) + sv(4, ly, hi_c));

        if constexpr (DO_RD) {
            if (fj >= RD_START && (int)fj < RD_START + FW_RD / 2) {
                const uint32_t fc_rd = fcoeff_rd[fj - RD_START];
                h_ref_rd += fc_rd * (sv(5, ly, lo_c) + sv(5, ly, hi_c));
                h_dis_rd += fc_rd * (sv(6, ly, lo_c) + sv(6, ly, hi_c));
            }
        }
    }
}
} // namespace

namespace
{
struct VifFusedLaunchParams {
    unsigned width;
    unsigned height;
    unsigned src_stride;
    unsigned bpc;
    float vif_enhn_gain_limit;
    int64_t *accum;
    uint32_t *rd_ref;
    uint32_t *rd_dis;
    const uint32_t *log2_lut;
};

struct VifFusedConstants {
    uint32_t fcoeff[VIF_FILTER_MAX_WIDTH];
    uint32_t fcoeff_rd[VIF_FILTER_MAX_WIDTH];
    unsigned shift_vp;
    unsigned add_shift_round_vp;
    unsigned shift_vp_sq;
    unsigned add_shift_round_vp_sq;
};
} // namespace

namespace
{
template <int SCALE> static VifFusedConstants make_vif_fused_constants(unsigned bpc)
{
    constexpr int FW_V = vif_fwidth[SCALE];
    constexpr int FW_RD = vif_fwidth_rd[SCALE];
    VifFusedConstants c{};
    for (int i = 0; i < FW_V; i++)
        c.fcoeff[i] = vif_filter1d_table[SCALE][i];
    if constexpr (FW_RD > 0) {
        for (int i = 0; i < FW_RD; i++)
            c.fcoeff_rd[i] = vif_filter1d_table[SCALE + 1][i];
    }
    c.shift_vp = (SCALE == 0) ? bpc : 16;
    c.add_shift_round_vp = (SCALE == 0) ? (1u << (bpc - 1)) : 32768;
    c.shift_vp_sq = (SCALE == 0) ? ((bpc - 8) * 2) : 16;
    c.add_shift_round_vp_sq = (SCALE == 0) ? ((bpc == 8) ? 0 : (1u << (c.shift_vp_sq - 1))) : 32768;
    return c;
}
} // namespace

namespace
{
template <int SCALE, int FW_V, int FW_H, int FW_RD, int TILE_H, int TILE_W, int VERT_TOTAL,
          int WG_SIZE, int MAX_SUBGROUPS>
static void dev_fused_item_step(sycl::nd_item<2> item, const void *ref_data, const void *dis_data,
                                const VifFusedLaunchParams &p, const VifFusedConstants &c,
                                const sycl::local_accessor<uint32_t, 1> &s_ref,
                                const sycl::local_accessor<uint32_t, 1> &s_dis,
                                const sycl::local_accessor<uint32_t, 1> &s_vert,
                                const sycl::local_accessor<int64_t, 1> &lmem)
{
    const int gx = item.get_global_id(1);
    const int gy = item.get_global_id(0);
    const unsigned lx = item.get_local_id(1);
    const unsigned ly = item.get_local_id(0);
    const bool valid = (std::cmp_less(gx, p.width) && std::cmp_less(gy, p.height));

    dev_fused_load_tile<SCALE, TILE_H, TILE_W, WG_SIZE>(item, ref_data, dis_data, p.width, p.height,
                                                        p.src_stride, p.bpc, s_ref, s_dis);
    item.barrier(sycl::access::fence_space::local_space);

    dev_fused_vert_conv<SCALE, FW_V, FW_RD, TILE_W, VERT_TOTAL, WG_SIZE>(
        item, c.shift_vp, c.add_shift_round_vp, c.shift_vp_sq, c.add_shift_round_vp_sq, c.fcoeff,
        c.fcoeff_rd, s_ref, s_dis, s_vert);
    item.barrier(sycl::access::fence_space::local_space);

    vif_accums t_acc = {};
    uint32_t h_ref_rd = 0;
    uint32_t h_dis_rd = 0;
    if (valid) {
        uint32_t h_mu1 = 0;
        uint32_t h_mu2 = 0;
        uint64_t h_ref = 0;
        uint64_t h_dis = 0;
        uint64_t h_ref_dis = 0;
        dev_fused_hori_conv<FW_H, FW_RD, TILE_W, VERT_TOTAL>(lx, ly, c.fcoeff, c.fcoeff_rd, s_vert,
                                                             h_mu1, h_mu2, h_ref, h_dis, h_ref_dis,
                                                             h_ref_rd, h_dis_rd);
        t_acc = dev_compute_vif_stats(h_mu1, h_mu2, h_ref, h_dis, h_ref_dis, p.vif_enhn_gain_limit,
                                      p.log2_lut);
    }

    dev_reduce_and_accum<MAX_SUBGROUPS>(item, t_acc, lmem, p.accum);
    dev_downsample_rd<FW_RD>(gx, gy, valid, p.width, h_ref_rd, h_dis_rd, p.rd_ref, p.rd_dis);
}
} // namespace

namespace
{
template <int SCALE, int SG_SIZE>
static sycl::event launch_vif_fused_impl(sycl::queue &q, const void *ref_data, const void *dis_data,
                                         const VifFusedLaunchParams &p)
{
    constexpr int FW_V = vif_fwidth[SCALE];
    constexpr int FW_H = FW_V;
    constexpr int FW_RD = vif_fwidth_rd[SCALE];
    constexpr int WG_X = 16;
    constexpr int WG_Y = 8;
    constexpr int WG_SIZE = WG_X * WG_Y;
    constexpr int MAX_SUBGROUPS = 16;
    constexpr int TILE_H = WG_Y + FW_V - 1;
    constexpr int TILE_W = WG_X + FW_H - 1;
    constexpr unsigned VERT_TOTAL = WG_Y * TILE_W;
    constexpr int N_CH = (FW_RD > 0) ? 7 : 5;

    const VifFusedConstants c = make_vif_fused_constants<SCALE>(p.bpc);

    const sycl::range<2> global(((static_cast<size_t>(p.height) + WG_Y - 1) / WG_Y) * WG_Y,
                                ((static_cast<size_t>(p.width) + WG_X - 1) / WG_X) * WG_X);
    const sycl::range<2> local(WG_Y, WG_X);

    return q.submit([&](sycl::handler &cgh) {
        sycl::local_accessor<uint32_t, 1> const s_ref(sycl::range<1>(TILE_H * TILE_W), cgh);
        sycl::local_accessor<uint32_t, 1> const s_dis(sycl::range<1>(TILE_H * TILE_W), cgh);
        sycl::local_accessor<uint32_t, 1> const s_vert(sycl::range<1>(N_CH * VERT_TOTAL), cgh);
        sycl::local_accessor<int64_t, 1> const lmem(
            sycl::range<1>(static_cast<size_t>(ACCUM_FIELDS) * MAX_SUBGROUPS), cgh);

        cgh.parallel_for(sycl::nd_range<2>(global, local),
                         [=](sycl::nd_item<2> item) VMAF_SYCL_REQD_SG_SIZE(SG_SIZE) {
                             dev_fused_item_step<SCALE, FW_V, FW_H, FW_RD, TILE_H, TILE_W,
                                                 VERT_TOTAL, WG_SIZE, MAX_SUBGROUPS>(
                                 item, ref_data, dis_data, p, c, s_ref, s_dis, s_vert, lmem);
                         });
    });
}
} // namespace

namespace
{
static sycl::event launch_vif_fused(sycl::queue &q, const void *ref_data, const void *dis_data,
                                    int scale, unsigned width, unsigned height, unsigned src_stride,
                                    unsigned bpc, bool use_simd16, float vif_enhn_gain_limit,
                                    int64_t *accum, uint32_t *rd_ref, uint32_t *rd_dis,
                                    const uint32_t *log2_lut)
{
    const VifFusedLaunchParams p = {
        .width = width,
        .height = height,
        .src_stride = src_stride,
        .bpc = bpc,
        .vif_enhn_gain_limit = vif_enhn_gain_limit,
        .accum = accum,
        .rd_ref = rd_ref,
        .rd_dis = rd_dis,
        .log2_lut = log2_lut,
    };
    if (use_simd16) {
        switch (scale) {
        case 0:
            return launch_vif_fused_impl<0, 16>(q, ref_data, dis_data, p);
        case 1:
            return launch_vif_fused_impl<1, 16>(q, ref_data, dis_data, p);
        case 2:
            return launch_vif_fused_impl<2, 16>(q, ref_data, dis_data, p);
        default:
            return launch_vif_fused_impl<3, 16>(q, ref_data, dis_data, p);
        }
    } else {
        switch (scale) {
        case 0:
            return launch_vif_fused_impl<0, 32>(q, ref_data, dis_data, p);
        case 1:
            return launch_vif_fused_impl<1, 32>(q, ref_data, dis_data, p);
        case 2:
            return launch_vif_fused_impl<2, 32>(q, ref_data, dis_data, p);
        default:
            return launch_vif_fused_impl<3, 32>(q, ref_data, dis_data, p);
        }
    }
}
} // namespace

/* ------------------------------------------------------------------ */
/* Feature extractor lifecycle helpers                                */
/* ------------------------------------------------------------------ */

namespace
{
static int close_fex_sycl(VmafFeatureExtractor *fex); /* forward decl for init error paths */
} // namespace

namespace
{
static inline int vif_alloc_buffers(VmafSyclState *state, VifStateSycl *s, unsigned w, unsigned h)
{
    const size_t tmp_size = (size_t)w * h * sizeof(uint32_t);
    if (!s->use_fused) {
        s->d_tmp_mu1 = static_cast<uint32_t *>(vmaf_sycl_malloc_device(state, tmp_size));
        s->d_tmp_mu2 = static_cast<uint32_t *>(vmaf_sycl_malloc_device(state, tmp_size));
        s->d_tmp_ref = static_cast<uint32_t *>(vmaf_sycl_malloc_device(state, tmp_size));
        s->d_tmp_dis = static_cast<uint32_t *>(vmaf_sycl_malloc_device(state, tmp_size));
        s->d_tmp_ref_dis = static_cast<uint32_t *>(vmaf_sycl_malloc_device(state, tmp_size));
        s->d_tmp_ref_convol = static_cast<uint32_t *>(vmaf_sycl_malloc_device(state, tmp_size));
        s->d_tmp_dis_convol = static_cast<uint32_t *>(vmaf_sycl_malloc_device(state, tmp_size));
        if (!s->d_tmp_mu1 || !s->d_tmp_mu2 || !s->d_tmp_ref || !s->d_tmp_dis || !s->d_tmp_ref_dis ||
            !s->d_tmp_ref_convol || !s->d_tmp_dis_convol) {
            vmaf_log(VMAF_LOG_LEVEL_ERROR, "vif_sycl: tmp buffer allocation failed\n");
            return -ENOMEM;
        }
    }

    const size_t rd_size = (size_t)((w + 1U) / 2U) * ((h + 1U) / 2U) * sizeof(uint32_t);
    s->d_rd_ref = static_cast<uint32_t *>(vmaf_sycl_malloc_device(state, rd_size));
    s->d_rd_dis = static_cast<uint32_t *>(vmaf_sycl_malloc_device(state, rd_size));

    const size_t accum_size = (ptrdiff_t)VIF_NUM_SCALES * ACCUM_FIELDS * sizeof(int64_t);
    s->d_accum = static_cast<int64_t *>(vmaf_sycl_malloc_device(state, accum_size));
    s->h_accum = static_cast<int64_t *>(vmaf_sycl_malloc_host(state, accum_size));

    const size_t lut_size = LOG2_LUT_SIZE * sizeof(uint32_t);
    s->d_log2_lut = static_cast<uint32_t *>(vmaf_sycl_malloc_device(state, lut_size));

    if (!s->d_rd_ref || !s->d_rd_dis || !s->d_accum || !s->h_accum || !s->d_log2_lut) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "vif_sycl: device memory allocation failed\n");
        return -ENOMEM;
    }
    return 0;
}
} // namespace

namespace
{
static inline int vif_init_log2_lut(VmafSyclState *state, uint32_t *d_log2_lut)
{
    const size_t lut_size = LOG2_LUT_SIZE * sizeof(uint32_t);
    auto *lut_host = static_cast<uint32_t *>(std::malloc(lut_size));
    if (!lut_host)
        return -ENOMEM;

    for (int j = 0; j < LOG2_LUT_SIZE; j++) {
        lut_host[j] = static_cast<uint32_t>(std::roundf(std::log2f((float)(j + 32768)) * 2048.0f));
    }
    const int cpy_err = vmaf_sycl_memcpy_h2d(state, d_log2_lut, lut_host, lut_size);
    std::free(lut_host);
    if (cpy_err) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "vif_sycl: log2 LUT upload failed\n");
        return cpy_err;
    }
    return 0;
}
} // namespace

// Forward declarations for combined graph callbacks (defined after enqueue_vif_work_impl)
namespace
{
static void enqueue_vif_work(void *queue_ptr, void *priv, void *shared_ref, void *shared_dis);
} // namespace
namespace
{
static void vif_pre_graph(void *queue_ptr, void *priv);
} // namespace
namespace
{
static void vif_post_graph(void *queue_ptr, void *priv);
} // namespace

namespace
{
static int vif_init_resources(VmafFeatureExtractor *fex, VmafSyclState *state, VifStateSycl *s,
                              unsigned w, unsigned h, unsigned bpc)
{
    int err = vmaf_sycl_shared_frame_init(state, w, h, bpc);
    if (err)
        return err;

    err = vif_alloc_buffers(state, s, w, h);
    if (err) {
        close_fex_sycl(fex);
        return err;
    }
    err = vif_init_log2_lut(state, s->d_log2_lut);
    if (err) {
        close_fex_sycl(fex);
        return err;
    }
    return 0;
}
} // namespace

namespace
{
static void vif_configure_device(const sycl::queue &q, VifStateSycl *s)
{
    const auto dev = q.get_device();
    const auto sg_sizes = dev.get_info<sycl::info::device::sub_group_sizes>();
    s->use_simd16 = std::ranges::any_of(sg_sizes, [](size_t sz) { return sz == 16; });
    vmaf_log(VMAF_LOG_LEVEL_DEBUG, "vif_sycl: auto-selected SIMD-%d subgroup size\n",
             s->use_simd16 ? 16 : 32);
    vmaf_log(VMAF_LOG_LEVEL_DEBUG, "vif_sycl: kernel mode = %s\n",
             s->use_fused ? "fused V+H (saves VRAM)" : "separate V+H");
}
} // namespace

namespace
{
static int vif_register_graph(VmafFeatureExtractor *fex, VmafSyclState *state, VifStateSycl *s)
{
    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (!s->feature_name_dict) {
        close_fex_sycl(fex);
        return -ENOMEM;
    }

    const int err = vmaf_sycl_graph_register(state, enqueue_vif_work, vif_pre_graph, vif_post_graph,
                                             nullptr, s, "VIF");
    if (err)
        close_fex_sycl(fex);
    return err;
}
} // namespace

namespace
{
static int init_fex_sycl(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                         unsigned w, unsigned h)
{
    (void)pix_fmt;
    auto *s = static_cast<VifStateSycl *>(fex->priv);
    s->width = w;
    s->height = h;
    s->bpc = bpc;
    s->has_pending = false;

    if (!fex->sycl_state) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "vif_sycl: no SYCL state\n");
        return -EINVAL;
    }
    VmafSyclState *state = fex->sycl_state;
    auto *q_ptr = static_cast<sycl::queue *>(vmaf_sycl_get_queue_ptr(state));
    if (!q_ptr)
        return -EINVAL;
    const sycl::queue &q = *q_ptr;

    const int err = vif_init_resources(fex, state, s, w, h, bpc);
    if (err)
        return err;

    vif_configure_device(q, s);
    return vif_register_graph(fex, state, s);
}
} // namespace

/* ------------------------------------------------------------------ */
/* Enqueue all VIF compute work (used for both recording and direct)    */
/* ------------------------------------------------------------------ */

namespace
{
static inline void vif_dispatch_scale(sycl::queue &q, VifStateSycl *s, int scale,
                                      const void *ref_src, const void *dis_src, unsigned cur_w,
                                      unsigned cur_h, unsigned src_stride, int64_t *scale_accum)
{
    if (s->use_fused) {
        launch_vif_fused(q, ref_src, dis_src, scale, cur_w, cur_h, src_stride, s->bpc,
                         s->use_simd16, static_cast<float>(s->vif_enhn_gain_limit), scale_accum,
                         s->d_rd_ref, s->d_rd_dis, s->d_log2_lut);
    } else {
        launch_vif_vert(q, ref_src, dis_src, scale, cur_w, cur_h, src_stride, s->bpc, s->d_tmp_mu1,
                        s->d_tmp_mu2, s->d_tmp_ref, s->d_tmp_dis, s->d_tmp_ref_dis,
                        s->d_tmp_ref_convol, s->d_tmp_dis_convol);
        if (s->use_simd16) {
            launch_vif_hori_v2_sg16(
                q, scale, cur_w, cur_h, static_cast<float>(s->vif_enhn_gain_limit), s->d_tmp_mu1,
                s->d_tmp_mu2, s->d_tmp_ref, s->d_tmp_dis, s->d_tmp_ref_dis, s->d_tmp_ref_convol,
                s->d_tmp_dis_convol, scale_accum, s->d_rd_ref, s->d_rd_dis, s->d_log2_lut);
        } else {
            launch_vif_hori_v2(q, scale, cur_w, cur_h, static_cast<float>(s->vif_enhn_gain_limit),
                               s->d_tmp_mu1, s->d_tmp_mu2, s->d_tmp_ref, s->d_tmp_dis,
                               s->d_tmp_ref_dis, s->d_tmp_ref_convol, s->d_tmp_dis_convol,
                               scale_accum, s->d_rd_ref, s->d_rd_dis, s->d_log2_lut);
        }
    }
}
} // namespace

namespace
{
static inline void enqueue_vif_work_impl(sycl::queue &q, VifStateSycl *s, void *shared_ref,
                                         void *shared_dis)
{
    unsigned cur_w = s->width;
    unsigned cur_h = s->height;

    for (int scale = 0; scale < VIF_NUM_SCALES; scale++) {
        const void *ref_src = (scale == 0) ? shared_ref : s->d_rd_ref;
        const void *dis_src = (scale == 0) ? shared_dis : s->d_rd_dis;
        const unsigned src_stride = (scale == 0 && s->bpc > 8) ? (cur_w * 2) : cur_w;
        int64_t *const scale_accum = s->d_accum + (ptrdiff_t)scale * ACCUM_FIELDS;

        vif_dispatch_scale(q, s, scale, ref_src, dis_src, cur_w, cur_h, src_stride, scale_accum);

        cur_w /= 2;
        cur_h /= 2;
    }
}
} // namespace

/* ------------------------------------------------------------------ */
/* C-compatible callbacks for combined command graph                   */
/* ------------------------------------------------------------------ */

namespace
{
static void vif_pre_graph(void *queue_ptr, void *priv)
{
    sycl::queue &q = *static_cast<sycl::queue *>(queue_ptr);
    auto *s = static_cast<VifStateSycl *>(priv);
    const size_t accum_size = (ptrdiff_t)VIF_NUM_SCALES * ACCUM_FIELDS * sizeof(int64_t);
    q.memset(s->d_accum, 0, accum_size);
}
} // namespace

namespace
{
static void enqueue_vif_work(void *queue_ptr, void *priv, void *shared_ref, void *shared_dis)
{
    sycl::queue &q = *static_cast<sycl::queue *>(queue_ptr);
    auto *s = static_cast<VifStateSycl *>(priv);
    enqueue_vif_work_impl(q, s, shared_ref, shared_dis);
}
} // namespace

namespace
{
static void vif_post_graph(void *queue_ptr, void *priv)
{
    sycl::queue &q = *static_cast<sycl::queue *>(queue_ptr);
    auto *s = static_cast<VifStateSycl *>(priv);
    const size_t accum_size = (ptrdiff_t)VIF_NUM_SCALES * ACCUM_FIELDS * sizeof(int64_t);
    q.memcpy(s->h_accum, s->d_accum, accum_size);
}
} // namespace

/* ------------------------------------------------------------------ */
/* Submit / Collect / Extract                                          */
/* ------------------------------------------------------------------ */

namespace
{
static int submit_fex_sycl(VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                           VmafPicture *dist_pic, VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic;
    (void)ref_pic_90;
    (void)dist_pic;
    (void)dist_pic_90;

    auto *s = static_cast<VifStateSycl *>(fex->priv);
    VmafSyclState *state = fex->sycl_state;

    int const err = vmaf_sycl_graph_submit(state);
    if (err)
        return err;

    s->pending_index = index;
    s->has_pending = true;

    return 0;
}
} // namespace

namespace
{
static inline void vif_compute_scores(const struct vif_accums *accums, bool vif_skip_scale0,
                                      double &score_num, double &score_den, double *vif_scale_num,
                                      double *vif_scale_den)
{
    score_num = 0.0;
    score_den = 0.0;
    for (int scale = 0; scale < VIF_NUM_SCALES; scale++) {
        const double num =
            accums[scale].num_log / 2048.0 + accums[scale].x2 +
            (accums[scale].den_non_log - (accums[scale].num_non_log / 16384.0) / 65025.0);

        const double den = accums[scale].den_log / 2048.0 -
                           (accums[scale].x + accums[scale].num_x * 17) + accums[scale].den_non_log;

        vif_scale_num[scale] = num;
        vif_scale_den[scale] = den;
        if (!vif_skip_scale0 || scale > 0) {
            score_num += num;
            score_den += den;
        }
    }
}
} // namespace

namespace
{
static int collect_fex_sycl(VmafFeatureExtractor *fex, unsigned index,
                            VmafFeatureCollector *feature_collector)
{
    auto *s = static_cast<VifStateSycl *>(fex->priv);
    VmafSyclState *state = fex->sycl_state;

    vmaf_sycl_graph_wait(state);

    struct vif_accums accums[VIF_NUM_SCALES];
    std::memcpy(accums, s->h_accum, sizeof(accums));

    double score_num = 0.0;
    double score_den = 0.0;
    double vif_scale_num[VIF_NUM_SCALES];
    double vif_scale_den[VIF_NUM_SCALES];

    vif_compute_scores(accums, s->vif_skip_scale0, score_num, score_den, vif_scale_num,
                       vif_scale_den);

    VmafVifScoreSet output = {
        .score = score_den > 0.0 ? score_num / score_den : NAN,
        .score_num = score_num,
        .score_den = score_den,
        .skip_scale0 = s->vif_skip_scale0,
        .debug = s->debug,
    };
    for (int scale = 0; scale < VIF_NUM_SCALES; ++scale) {
        const size_t offset = (size_t)scale * 2u;
        output.scale[offset] = vif_scale_num[scale];
        output.scale[offset + 1u] = vif_scale_den[scale];
    }
    const int err =
        vmaf_vif_emit_scores(feature_collector, s->feature_name_dict, "integer_vif_sycl", &output,
                             VMAF_VIF_INTEGER_NAMES, index);
    if (err)
        return err;
    s->has_pending = false;
    return 0;
}
} // namespace

namespace
{
static int extract_fex_sycl(VmafFeatureExtractor *fex, VmafPicture *ref_pic,
                            VmafPicture *ref_pic_90, VmafPicture *dist_pic,
                            VmafPicture *dist_pic_90, unsigned index,
                            VmafFeatureCollector *feature_collector)
{
    const int err = submit_fex_sycl(fex, ref_pic, ref_pic_90, dist_pic, dist_pic_90, index);
    if (err)
        return err;
    return collect_fex_sycl(fex, index, feature_collector);
}
} // namespace

namespace
{
static int flush_fex_sycl(VmafFeatureExtractor *fex, VmafFeatureCollector *feature_collector)
{
    (void)feature_collector;
    if (!fex)
        return -EINVAL;
    VmafSyclState *state = fex->sycl_state;
    if (state) {
        const int wait_err = vmaf_sycl_queue_wait(state);
        if (wait_err)
            return wait_err;
    }
    return 1;
}
} // namespace

namespace
{
static int close_fex_sycl(VmafFeatureExtractor *fex)
{
    auto *s = static_cast<VifStateSycl *>(fex->priv);
    VmafSyclState *state = fex->sycl_state;

    if (state) {
        (void)vmaf_sycl_queue_wait(state);
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
} // namespace

namespace
{
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
} // namespace

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
