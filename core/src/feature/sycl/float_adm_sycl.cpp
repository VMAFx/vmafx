/**
 *  Copyright 2016-2020 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  float_adm feature kernel on the SYCL backend (T7-23 / batch 3
 *  part 6c — ADR-0192 / ADR-0202). SYCL twin of float_adm_vulkan
 *  (PR #154 / ADR-0199) and float_adm_cuda (this PR's `_cuda`
 *  sibling). Same four pipeline stages, same `-1` mirror form, same
 *  fused stage 3 with cross-band CM threshold.
 *
 *  Per-frame flow: 24 launches (6 stages × 4 scales). Self-contained
 *  submit/collect — does NOT use the shared_frame model (the
 *  multi-scale band/csf layout doesn't fit). Reduction across WGs
 *  runs on the host in double precision.
 */

#include <sycl/sycl.hpp>

#include "sycl_compat.h"

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <utility>

#include "config.h"
#include "feature/adm_options.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature_name.h"
#include "log.h"
#include "picture.h"
#include "sycl/common.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

constexpr int FADM_BX = 16;
constexpr int FADM_BY = 16;
constexpr int FADM_NUM_SCALES = 4;
constexpr int FADM_NUM_BANDS = 3;
/* ADR-0574: slots 0..5 = adm2 csf+cm per band; slots 6..8 = aim_cm per band. */
constexpr int FADM_ACCUM_SLOTS = 9;
constexpr double FADM_BORDER_FACTOR = 0.1;

constexpr float FADM_LO0 = 0.482962913144690f;
constexpr float FADM_LO1 = 0.836516303737469f;
constexpr float FADM_LO2 = 0.224143868041857f;
constexpr float FADM_LO3 = -0.129409522550921f;
constexpr float FADM_HI0 = -0.129409522550921f;
constexpr float FADM_HI1 = -0.224143868041857f;
constexpr float FADM_HI2 = 0.836516303737469f;
constexpr float FADM_HI3 = -0.482962913144690f;

constexpr float FADM_ONE_BY_30 = 0.0333333351f;
constexpr float FADM_ONE_BY_15 = 0.0666666701f;
constexpr float FADM_COS_1DEG_SQ = 0.99969541789740297f;
constexpr float FADM_EPS = 1e-30f;

struct FloatAdmStateSycl {
    bool debug;
    double adm_enhn_gain_limit;
    double adm_norm_view_dist;
    int adm_ref_display_height;
    int adm_csf_mode;
    double adm_csf_scale;
    double adm_csf_diag_scale;
    double adm_noise_weight;
    /* ADR-0574: AIM / ADM3 options. */
    int adm_adm3_apply_hm;
    double adm_p_norm;
    double adm_dlm_weight;
    double adm_min_val;

    unsigned width;
    unsigned height;
    unsigned bpc;
    unsigned buf_stride;
    float rfactor[12];

    VmafSyclState *sycl_state;

    void *h_ref_raw;
    void *h_dis_raw;
    void *d_ref_raw;
    void *d_dis_raw;
    float *d_dwt_tmp_ref;
    float *d_dwt_tmp_dis;
    float *d_ref_band[FADM_NUM_SCALES];
    float *d_dis_band[FADM_NUM_SCALES];
    float *d_csf_a;
    float *d_csf_f;
    float *d_csf_a_aim;
    float *d_csf_f_aim;
    float *d_accum[FADM_NUM_SCALES];
    float *h_accum[FADM_NUM_SCALES];

    unsigned wg_count[FADM_NUM_SCALES];
    unsigned scale_w[FADM_NUM_SCALES];
    unsigned scale_h[FADM_NUM_SCALES];
    unsigned scale_half_w[FADM_NUM_SCALES];
    unsigned scale_half_h[FADM_NUM_SCALES];

    bool has_pending;
    unsigned pending_index;

    VmafDictionary *feature_name_dict;
};

static const float fadm_dwt_basis_amp[6][4] = {
    {0.62171f, 0.67234f, 0.72709f, 0.67234f},     {0.34537f, 0.41317f, 0.49428f, 0.41317f},
    {0.18004f, 0.22727f, 0.28688f, 0.22727f},     {0.091401f, 0.11792f, 0.15214f, 0.11792f},
    {0.045943f, 0.059758f, 0.077727f, 0.059758f}, {0.023013f, 0.030018f, 0.039156f, 0.030018f},
};
constexpr float fadm_dwt_a_Y = 0.495f;
constexpr float fadm_dwt_k_Y = 0.466f;
constexpr float fadm_dwt_f0_Y = 0.401f;
static const float fadm_dwt_g_Y[4] = {1.501f, 1.0f, 0.534f, 1.0f};

static float fadm_dwt_quant_step(int lambda, int theta, double view_dist, int display_h)
{
    const float r = (float)(view_dist * (double)display_h * M_PI / 180.0);
    const float temp =
        (float)std::log10(std::pow(2.0, (double)(lambda + 1)) * (double)fadm_dwt_f0_Y *
                          (double)fadm_dwt_g_Y[theta] / (double)r);
    const float Q = (float)(2.0 * (double)fadm_dwt_a_Y *
                            std::pow(10.0, (double)fadm_dwt_k_Y * (double)temp * (double)temp) /
                            (double)fadm_dwt_basis_amp[lambda][theta]);
    return Q;
}

static inline int fadm_mirror_host(int idx, int sup)
{
    if (idx < 0)
        return -idx;
    if (idx >= sup)
        return 2 * sup - idx - 1;
    return idx;
}

template <typename T>
static void copy_y_plane(const VmafPicture *pic, void *dst, unsigned w, unsigned h)
{
    const T *src = static_cast<const T *>(pic->data[0]);
    T *out = static_cast<T *>(dst);
    const ptrdiff_t src_stride_t = pic->stride[0] / static_cast<ptrdiff_t>(sizeof(T));
    for (unsigned i = 0; i < h; i++) {
        for (unsigned j = 0; j < w; j++)
            out[j] = src[j];
        src += src_stride_t;
        out += w;
    }
}

/* ------------------------------------------------------------------ */
/* Stage 0 — DWT vertical.                                             */
/* ------------------------------------------------------------------ */
struct FadmDwtVertArgs {
    const void *ref_raw;
    const void *dis_raw;
    const float *parent_ref_band;
    const float *parent_dis_band;
    float *dwt_ref;
    float *dwt_dis;
    unsigned raw_stride;
    unsigned parent_buf_stride;
    unsigned parent_w;
    unsigned parent_h;
    unsigned cur_w;
    unsigned cur_h;
    unsigned half_h;
    unsigned bpc;
    float scaler;
    float pixel_offset;
};

static inline float fadm_read_raw(const FadmDwtVertArgs &args, const void *plane, int y, int x)
{
    y = fadm_mirror_host(y, (int)args.cur_h);
    x = sycl::clamp(x, 0, (int)args.cur_w - 1);
    if (args.bpc <= 8u)
        return (float)static_cast<const uint8_t *>(plane)[y * args.raw_stride + x] +
               args.pixel_offset;
    const auto *row = reinterpret_cast<const uint16_t *>(static_cast<const uint8_t *>(plane) +
                                                         (size_t)y * args.raw_stride);
    return (float)row[x] / args.scaler + args.pixel_offset;
}

static inline float fadm_read_parent(const FadmDwtVertArgs &args, const float *band, int y, int x)
{
    y = fadm_mirror_host(y, (int)args.parent_h);
    x = sycl::clamp(x, 0, (int)args.parent_w - 1);
    return band[y * (int)args.parent_buf_stride + x];
}

template <int SCALE>
static inline void fadm_dwt_vert_pixel(const FadmDwtVertArgs &args, sycl::nd_item<3> item)
{
    const int x = (int)item.get_global_id(2);
    const int y = (int)item.get_global_id(1);
    const bool distorted = item.get_global_id(0) != 0;
    if (std::cmp_greater_equal(x, args.cur_w) || std::cmp_greater_equal(y, args.half_h))
        return;
    const int row_start = 2 * y - 1;
    float samples[4];
    for (int tap = 0; tap < 4; tap++) {
        if constexpr (SCALE == 0) {
            const void *plane = distorted ? args.dis_raw : args.ref_raw;
            samples[tap] = fadm_read_raw(args, plane, row_start + tap, x);
        } else {
            const float *band = distorted ? args.parent_dis_band : args.parent_ref_band;
            samples[tap] = fadm_read_parent(args, band, row_start + tap, x);
        }
    }
    const float lo = FADM_LO0 * samples[0] + FADM_LO1 * samples[1] + FADM_LO2 * samples[2] +
                     FADM_LO3 * samples[3];
    const float hi = FADM_HI0 * samples[0] + FADM_HI1 * samples[1] + FADM_HI2 * samples[2] +
                     FADM_HI3 * samples[3];
    const int out_stride = (int)args.cur_w * 2;
    float *destination = distorted ? args.dwt_dis : args.dwt_ref;
    destination[y * out_stride + x] = lo;
    destination[y * out_stride + (int)args.cur_w + x] = hi;
}

template <int SCALE>
static sycl::event launch_dwt_vert(sycl::queue &q, const void *ref_raw, const void *dis_raw,
                                   unsigned raw_stride_bytes, const float *parent_ref_band,
                                   const float *parent_dis_band, unsigned parent_buf_stride,
                                   unsigned parent_w, unsigned parent_h, float *dwt_tmp_ref,
                                   float *dwt_tmp_dis, unsigned cur_w, unsigned cur_h,
                                   unsigned half_h, unsigned bpc, float scaler, float pixel_offset)
{
    const size_t global_x = (size_t)((cur_w + FADM_BX - 1u) / FADM_BX) * FADM_BX;
    const size_t global_y = (size_t)((half_h + FADM_BY - 1u) / FADM_BY) * FADM_BY;
    const FadmDwtVertArgs args{ref_raw,
                               dis_raw,
                               parent_ref_band,
                               parent_dis_band,
                               dwt_tmp_ref,
                               dwt_tmp_dis,
                               raw_stride_bytes,
                               parent_buf_stride,
                               parent_w,
                               parent_h,
                               cur_w,
                               cur_h,
                               half_h,
                               bpc,
                               scaler,
                               pixel_offset};
    return q.submit([&](sycl::handler &cgh) {
        cgh.parallel_for(sycl::nd_range<3>(sycl::range<3>(2, global_y, global_x),
                                           sycl::range<3>(1, FADM_BY, FADM_BX)),
                         [=](sycl::nd_item<3> item) { fadm_dwt_vert_pixel<SCALE>(args, item); });
    });
}

/* ------------------------------------------------------------------ */
/* Stage 1 — DWT horizontal.                                           */
/* ------------------------------------------------------------------ */
static sycl::event launch_dwt_hori(sycl::queue &q, const float *dwt_tmp_ref,
                                   const float *dwt_tmp_dis, float *ref_band, float *dis_band,
                                   unsigned cur_w, unsigned half_w, unsigned half_h,
                                   unsigned buf_stride)
{
    const size_t global_x = (size_t)((half_w + FADM_BX - 1u) / FADM_BX) * FADM_BX;
    const size_t global_y = (size_t)((half_h + FADM_BY - 1u) / FADM_BY) * FADM_BY;
    const unsigned e_cur_w = cur_w;
    const unsigned e_half_w = half_w;
    const unsigned e_half_h = half_h;
    const unsigned e_buf_stride = buf_stride;
    const float *e_ref = dwt_tmp_ref;
    const float *e_dis = dwt_tmp_dis;
    float *e_ref_band = ref_band;
    float *e_dis_band = dis_band;

    return q.submit([&](sycl::handler &cgh) {
        cgh.parallel_for(
            sycl::nd_range<3>(sycl::range<3>(2, global_y, global_x),
                              sycl::range<3>(1, FADM_BY, FADM_BX)),
            [=](sycl::nd_item<3> item) {
                const int gx = (int)item.get_global_id(2);
                const int gy = (int)item.get_global_id(1);
                const int plane_is_dis = (int)item.get_global_id(0);
                if (std::cmp_greater_equal(gx, e_half_w) || std::cmp_greater_equal(gy, e_half_h))
                    return;

                auto mirror = [](int idx, int sup) -> int {
                    if (idx < 0)
                        return -idx;
                    if (idx >= sup)
                        return 2 * sup - idx - 1;
                    return idx;
                };
                const float *src = (plane_is_dis == 0) ? e_ref : e_dis;
                float *dst = (plane_is_dis == 0) ? e_ref_band : e_dis_band;
                auto read_tmp = [&](int gy_l, int x_sub, int half_offset) -> float {
                    x_sub = mirror(x_sub, (int)e_cur_w);
                    const int stride = (int)e_cur_w * 2;
                    return src[gy_l * stride + half_offset + x_sub];
                };
                const int base_x = 2 * gx;
                const float l0 = read_tmp(gy, base_x - 1, 0);
                const float l1 = read_tmp(gy, base_x + 0, 0);
                const float l2 = read_tmp(gy, base_x + 1, 0);
                const float l3 = read_tmp(gy, base_x + 2, 0);
                const float a_val = FADM_LO0 * l0 + FADM_LO1 * l1 + FADM_LO2 * l2 + FADM_LO3 * l3;
                const float v_val = FADM_HI0 * l0 + FADM_HI1 * l1 + FADM_HI2 * l2 + FADM_HI3 * l3;
                const float h0 = read_tmp(gy, base_x - 1, (int)e_cur_w);
                const float h1 = read_tmp(gy, base_x + 0, (int)e_cur_w);
                const float h2 = read_tmp(gy, base_x + 1, (int)e_cur_w);
                const float h3 = read_tmp(gy, base_x + 2, (int)e_cur_w);
                const float h_val = FADM_LO0 * h0 + FADM_LO1 * h1 + FADM_LO2 * h2 + FADM_LO3 * h3;
                const float d_val = FADM_HI0 * h0 + FADM_HI1 * h1 + FADM_HI2 * h2 + FADM_HI3 * h3;
                const int slice = (int)e_buf_stride * (int)e_half_h;
                dst[0 * slice + gy * (int)e_buf_stride + gx] = a_val;
                dst[1 * slice + gy * (int)e_buf_stride + gx] = h_val;
                dst[2 * slice + gy * (int)e_buf_stride + gx] = v_val;
                dst[3 * slice + gy * (int)e_buf_stride + gx] = d_val;
            });
    });
}

/* ------------------------------------------------------------------ */
/* Stage 2 — Decouple + CSF.                                           */
/* ------------------------------------------------------------------ */
struct FadmDecoupleArgs {
    const float *reference;
    const float *distorted;
    float *csf_a;
    float *csf_f;
    unsigned width;
    unsigned height;
    unsigned stride;
    float rfactor[3];
    float gain_limit;
};

static inline float fadm_restored(float original, float test, bool angle_flag, float gain_limit)
{
    float ratio = test / (original + FADM_EPS);
    ratio = sycl::fmax(0.0f, sycl::fmin(ratio, 1.0f));
    float restored = ratio * original;
    if (angle_flag && restored > 0.0f)
        restored = sycl::fmin(restored * gain_limit, test);
    else if (angle_flag && restored < 0.0f)
        restored = sycl::fmax(restored * gain_limit, test);
    return restored;
}

template <bool USE_RESTORED>
static inline void fadm_decouple_pixel(const FadmDecoupleArgs &args, sycl::nd_item<2> item)
{
    const int x = (int)item.get_global_id(1);
    const int y = (int)item.get_global_id(0);
    if (std::cmp_greater_equal(x, args.width) || std::cmp_greater_equal(y, args.height))
        return;
    const int slice = (int)args.stride * (int)args.height;
    const int offset = y * (int)args.stride + x;
    float original[3];
    float test[3];
    for (int band = 0; band < FADM_NUM_BANDS; band++) {
        original[band] = args.reference[(band + 1) * slice + offset];
        test[band] = args.distorted[(band + 1) * slice + offset];
    }
    const float dot = (original[0] * test[0]) + (original[1] * test[1]);
    const float original_magnitude = (original[0] * original[0]) + (original[1] * original[1]);
    const float test_magnitude = (test[0] * test[0]) + (test[1] * test[1]);
    const bool angle_flag =
        (dot >= 0.0f) && (dot * dot >= FADM_COS_1DEG_SQ * (original_magnitude * test_magnitude));
    for (int band = 0; band < FADM_NUM_BANDS; band++) {
        const float restored =
            fadm_restored(original[band], test[band], angle_flag, args.gain_limit);
        const float selected = USE_RESTORED ? restored : test[band] - restored;
        const float csf_a = args.rfactor[band] * selected;
        args.csf_a[band * slice + offset] = csf_a;
        args.csf_f[band * slice + offset] = FADM_ONE_BY_30 * sycl::fabs(csf_a);
    }
}

template <bool USE_RESTORED>
static sycl::event
launch_decouple_impl(sycl::queue &q, const float *ref_band, const float *dis_band, float *csf_a,
                     float *csf_f, unsigned half_w, unsigned half_h, unsigned buf_stride,
                     float rfactor_h, float rfactor_v, float rfactor_d, float gain_limit)
{
    const size_t global_x = (size_t)((half_w + FADM_BX - 1u) / FADM_BX) * FADM_BX;
    const size_t global_y = (size_t)((half_h + FADM_BY - 1u) / FADM_BY) * FADM_BY;
    const FadmDecoupleArgs args{ref_band,  dis_band, csf_a,      csf_f,
                                half_w,    half_h,   buf_stride, {rfactor_h, rfactor_v, rfactor_d},
                                gain_limit};
    return q.submit([&](sycl::handler &cgh) {
        cgh.parallel_for(
            sycl::nd_range<2>(sycl::range<2>(global_y, global_x), sycl::range<2>(FADM_BY, FADM_BX)),
            [=](sycl::nd_item<2> item) { fadm_decouple_pixel<USE_RESTORED>(args, item); });
    });
}

static sycl::event launch_decouple_csf(sycl::queue &q, const float *ref_band, const float *dis_band,
                                       float *csf_a, float *csf_f, unsigned half_w, unsigned half_h,
                                       unsigned buf_stride, float rfactor_h, float rfactor_v,
                                       float rfactor_d, float gain_limit)
{
    return launch_decouple_impl<false>(q, ref_band, dis_band, csf_a, csf_f, half_w, half_h,
                                       buf_stride, rfactor_h, rfactor_v, rfactor_d, gain_limit);
}

static sycl::event launch_csf_r(sycl::queue &q, const float *ref_band, const float *dis_band,
                                float *csf_a_aim, float *csf_f_aim, unsigned half_w,
                                unsigned half_h, unsigned buf_stride, float rfactor_h,
                                float rfactor_v, float rfactor_d, float gain_limit)
{
    return launch_decouple_impl<true>(q, ref_band, dis_band, csf_a_aim, csf_f_aim, half_w, half_h,
                                      buf_stride, rfactor_h, rfactor_v, rfactor_d, gain_limit);
}

/* ------------------------------------------------------------------ */
/* Stage 3b — AIM CM numerator (noise_weight = 0). ADR-0574.           */
/* Mirrors stage 3 but uses csf_a_aim/csf_f_aim (from decouple_r)     */
/* and accumulates decouple_a into slots 6..8.                         */
/* ------------------------------------------------------------------ */
/* p-norm accumulation, mirroring adm_tools.c exactly: the CPU special-cases
 * p == 3 to a literal cube and only falls back to pow() otherwise, so the
 * default path stays bit-identical. ADR-1220. */
static inline float fadm_pnorm_term(float x, float p_norm)
{
    return (p_norm == 3.0f) ? (x * x * x) : sycl::pow(x, p_norm);
}

struct FadmAccumArgs {
    const float *reference;
    const float *distorted;
    const float *csf_a;
    const float *csf_f;
    float *output;
    unsigned width;
    unsigned height;
    unsigned stride;
    unsigned active_height;
    int left;
    int top;
    int right;
    float rfactor[3];
    float gain_limit;
    float p_norm;
};

struct FadmAccumPair {
    float csf;
    float cm;
};

static inline float fadm_read_band(const float *bands, const FadmAccumArgs &args, int band, int y,
                                   int x)
{
    const int slice = (int)args.stride * (int)args.height;
    return bands[band * slice + y * (int)args.stride + x];
}

static inline float fadm_read_threshold_f(const FadmAccumArgs &args, int band, int y, int x)
{
    if (x < 0)
        x = -x;
    if (std::cmp_greater_equal(x, args.width))
        x = (int)args.width - 1;
    if (y < 0)
        y = -y;
    if (std::cmp_greater_equal(y, args.height))
        y = (int)args.height - 1;
    x = sycl::clamp(x, 0, (int)args.width - 1);
    y = sycl::clamp(y, 0, (int)args.height - 1);
    return fadm_read_band(args.csf_f, args, band, y, x);
}

static inline float fadm_read_threshold_a(const FadmAccumArgs &args, int band, int y, int x)
{
    x = sycl::clamp(x, 0, (int)args.width - 1);
    y = sycl::clamp(y, 0, (int)args.height - 1);
    return fadm_read_band(args.csf_a, args, band, y, x);
}

static inline float fadm_threshold(const FadmAccumArgs &args, int row, int column)
{
    float threshold = 0.0f;
    for (int band = 0; band < FADM_NUM_BANDS; band++) {
        for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                if (dx != 0 || dy != 0)
                    threshold += fadm_read_threshold_f(args, band, row + dy, column + dx);
            }
        }
    }
    threshold += FADM_ONE_BY_15 * sycl::fabs(fadm_read_threshold_a(args, 0, row, column));
    threshold += FADM_ONE_BY_15 * sycl::fabs(fadm_read_threshold_a(args, 1, row, column));
    threshold += FADM_ONE_BY_15 * sycl::fabs(fadm_read_threshold_a(args, 2, row, column));
    return threshold;
}

template <bool AIM_MODE>
static inline FadmAccumPair fadm_accum_column(const FadmAccumArgs &args, unsigned band, int row,
                                              int column)
{
    float original[3];
    float test[3];
    for (int component = 0; component < FADM_NUM_BANDS; component++) {
        original[component] = fadm_read_band(args.reference, args, component + 1, row, column);
        test[component] = fadm_read_band(args.distorted, args, component + 1, row, column);
    }
    const float dot = (original[0] * test[0]) + (original[1] * test[1]);
    const float original_magnitude = (original[0] * original[0]) + (original[1] * original[1]);
    const float test_magnitude = (test[0] * test[0]) + (test[1] * test[1]);
    const bool angle_flag =
        (dot >= 0.0f) && (dot * dot >= FADM_COS_1DEG_SQ * (original_magnitude * test_magnitude));
    const float restored = fadm_restored(original[band], test[band], angle_flag, args.gain_limit);
    const float selected = AIM_MODE ? test[band] - restored : restored;
    float magnitude = sycl::fabs(args.rfactor[band] * selected) - fadm_threshold(args, row, column);
    if (magnitude < 0.0f)
        magnitude = 0.0f;
    const float cm = fadm_pnorm_term(magnitude, args.p_norm);
    if constexpr (AIM_MODE)
        return {0.0f, cm};
    const float source = sycl::fabs(
        args.rfactor[band] * fadm_read_band(args.reference, args, (int)band + 1, row, column));
    return {fadm_pnorm_term(source, args.p_norm), cm};
}

template <bool AIM_MODE>
static inline void fadm_accum_group(const FadmAccumArgs &args, sycl::nd_item<1> item,
                                    const sycl::local_accessor<float, 1> &local_csf,
                                    const sycl::local_accessor<float, 1> &local_cm)
{
    const unsigned group = (unsigned)item.get_group(0);
    const unsigned local_id = (unsigned)item.get_local_id(0);
    const unsigned band = group / args.active_height;
    const unsigned row_index = group - band * args.active_height;
    const int row = args.top + (int)row_index;
    FadmAccumPair local{0.0f, 0.0f};
    constexpr int work_group_size = FADM_BX * FADM_BY;
    for (int column = args.left + (int)local_id; column < args.right; column += work_group_size) {
        const FadmAccumPair value = fadm_accum_column<AIM_MODE>(args, band, row, column);
        local.csf += value.csf;
        local.cm += value.cm;
    }
    const sycl::sub_group subgroup = item.get_sub_group();
    const float subgroup_csf = sycl::reduce_over_group(subgroup, local.csf, sycl::plus<float>{});
    const float subgroup_cm = sycl::reduce_over_group(subgroup, local.cm, sycl::plus<float>{});
    const uint32_t subgroup_id = subgroup.get_group_linear_id();
    if (subgroup.get_local_linear_id() == 0) {
        local_csf[subgroup_id] = subgroup_csf;
        local_cm[subgroup_id] = subgroup_cm;
    }
    item.barrier(sycl::access::fence_space::local_space);
    if (local_id != 0)
        return;
    float total_csf = 0.0f;
    float total_cm = 0.0f;
    for (uint32_t index = 0; index < subgroup.get_group_linear_range(); index++) {
        total_csf += local_csf[index];
        total_cm += local_cm[index];
    }
    const unsigned slot_base = group * FADM_ACCUM_SLOTS;
    if constexpr (AIM_MODE)
        args.output[slot_base + 6u + band] = total_cm;
    else {
        args.output[slot_base + band] = total_csf;
        args.output[slot_base + 3u + band] = total_cm;
    }
}

template <bool AIM_MODE>
static sycl::event launch_accum_impl(sycl::queue &q, const FadmAccumArgs &args)
{
    if (args.active_height == 0u || args.right <= args.left)
        return sycl::event{};
    constexpr size_t work_group_size = (size_t)FADM_BX * FADM_BY;
    const size_t global_size = 3u * args.active_height * work_group_size;
    return q.submit([&](sycl::handler &cgh) {
        const sycl::local_accessor<float, 1> local_csf(sycl::range<1>(work_group_size / 32), cgh);
        const sycl::local_accessor<float, 1> local_cm(sycl::range<1>(work_group_size / 32), cgh);
        cgh.parallel_for(
            sycl::nd_range<1>(sycl::range<1>(global_size), sycl::range<1>(work_group_size)),
            [=](sycl::nd_item<1> item) VMAF_SYCL_REQD_SG_SIZE(32) {
                fadm_accum_group<AIM_MODE>(args, item, local_csf, local_cm);
            });
    });
}

static FadmAccumArgs fadm_accum_args(const float *ref_band, const float *dis_band,
                                     const float *csf_a, const float *csf_f, float *accum_out,
                                     unsigned half_w, unsigned half_h, unsigned buf_stride,
                                     int active_left, int active_top, int active_right,
                                     int active_bottom, float rfactor_h, float rfactor_v,
                                     float rfactor_d, float gain_limit, float p_norm)
{
    const int active_height = active_bottom - active_top;
    return FadmAccumArgs{ref_band,
                         dis_band,
                         csf_a,
                         csf_f,
                         accum_out,
                         half_w,
                         half_h,
                         buf_stride,
                         active_height > 0 ? (unsigned)active_height : 0u,
                         active_left,
                         active_top,
                         active_right,
                         {rfactor_h, rfactor_v, rfactor_d},
                         gain_limit,
                         p_norm};
}

static sycl::event launch_aim_cm(sycl::queue &q, const float *ref_band, const float *dis_band,
                                 const float *csf_a_aim, const float *csf_f_aim, float *accum_out,
                                 unsigned half_w, unsigned half_h, unsigned buf_stride,
                                 int active_left, int active_top, int active_right,
                                 int active_bottom, float rfactor_h, float rfactor_v,
                                 float rfactor_d, float gain_limit, float p_norm)
{
    const FadmAccumArgs args =
        fadm_accum_args(ref_band, dis_band, csf_a_aim, csf_f_aim, accum_out, half_w, half_h,
                        buf_stride, active_left, active_top, active_right, active_bottom, rfactor_h,
                        rfactor_v, rfactor_d, gain_limit, p_norm);
    return launch_accum_impl<true>(q, args);
}

static sycl::event launch_csf_cm(sycl::queue &q, const float *ref_band, const float *dis_band,
                                 const float *csf_a, const float *csf_f, float *accum_out,
                                 unsigned half_w, unsigned half_h, unsigned buf_stride,
                                 int active_left, int active_top, int active_right,
                                 int active_bottom, float rfactor_h, float rfactor_v,
                                 float rfactor_d, float gain_limit, float p_norm)
{
    const FadmAccumArgs args =
        fadm_accum_args(ref_band, dis_band, csf_a, csf_f, accum_out, half_w, half_h, buf_stride,
                        active_left, active_top, active_right, active_bottom, rfactor_h, rfactor_v,
                        rfactor_d, gain_limit, p_norm);
    return launch_accum_impl<false>(q, args);
}

#define FADM_BOOL_OPTION(name_, help_, alias_, member_, default_, flags_)                          \
    {.name = name_,                                                                                \
     .help = help_,                                                                                \
     .alias = alias_,                                                                              \
     .offset = offsetof(FloatAdmStateSycl, member_),                                               \
     .type = VMAF_OPT_TYPE_BOOL,                                                                   \
     .default_val = {.b = default_},                                                               \
     .min = 0.0,                                                                                   \
     .max = 0.0,                                                                                   \
     .flags = flags_}
#define FADM_DOUBLE_OPTION(name_, help_, alias_, member_, default_, min_, max_)                    \
    {.name = name_,                                                                                \
     .help = help_,                                                                                \
     .alias = alias_,                                                                              \
     .offset = offsetof(FloatAdmStateSycl, member_),                                               \
     .type = VMAF_OPT_TYPE_DOUBLE,                                                                 \
     .default_val = {.d = default_},                                                               \
     .min = min_,                                                                                  \
     .max = max_,                                                                                  \
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM}
#define FADM_INT_OPTION(name_, help_, alias_, member_, default_, min_, max_)                       \
    {.name = name_,                                                                                \
     .help = help_,                                                                                \
     .alias = alias_,                                                                              \
     .offset = offsetof(FloatAdmStateSycl, member_),                                               \
     .type = VMAF_OPT_TYPE_INT,                                                                    \
     .default_val = {.i = default_},                                                               \
     .min = min_,                                                                                  \
     .max = max_,                                                                                  \
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM}

static const VmafOption options_float_adm_sycl[] = {
    FADM_BOOL_OPTION("debug", "debug mode", nullptr, debug, false, 0),
    FADM_DOUBLE_OPTION("adm_enhn_gain_limit", "enhancement gain (>=1.0)", "egl",
                       adm_enhn_gain_limit, 100.0, 1.0, 100.0),
    FADM_DOUBLE_OPTION("adm_norm_view_dist", "normalized viewing distance", "nvd",
                       adm_norm_view_dist, 3.0, 0.75, 24.0),
    FADM_INT_OPTION("adm_ref_display_height", "reference display height in pixels", "rdf",
                    adm_ref_display_height, 1080, 1, 4320),
    FADM_INT_OPTION("adm_csf_mode", "contrast sensitivity function (mode 0 only on SYCL v1)", "csf",
                    adm_csf_mode, 0, 0, 9),
    FADM_DOUBLE_OPTION("adm_csf_scale",
                       "CSF band-scale multiplier for h/v bands (default 1.0 = no scaling)", "scf",
                       adm_csf_scale, DEFAULT_ADM_CSF_SCALE, 0.0, 100.0),
    FADM_DOUBLE_OPTION("adm_csf_diag_scale",
                       "CSF band-scale multiplier for diagonal bands (default 1.0 = no scaling)",
                       "scfd", adm_csf_diag_scale, DEFAULT_ADM_CSF_DIAG_SCALE, 0.0, 100.0),
    FADM_DOUBLE_OPTION("adm_noise_weight",
                       "noise floor weight for CM numerator (default 0.03125 = 1/32)", "nw",
                       adm_noise_weight, DEFAULT_ADM_NOISE_WEIGHT, 0.0, 100.0),
    FADM_BOOL_OPTION("adm_adm3_apply_hm",
                     "apply harmonic mean for adm3 score (false = linear blend)", "aah",
                     adm_adm3_apply_hm, false, VMAF_OPT_FLAG_FEATURE_PARAM),
    FADM_DOUBLE_OPTION("adm_p_norm", "p-norm exponent for AIM/ADM3 score (default 3.0)", "apn",
                       adm_p_norm, 3.0, 1.0, 20.0),
    FADM_DOUBLE_OPTION("adm_dlm_weight", "DLM weight for linear-blend adm3 score (default 0.5)",
                       "dlmw", adm_dlm_weight, 0.5, 0.0, 1.0),
    FADM_DOUBLE_OPTION("adm_min_val", "minimum clamp for adm3 score (default 0.0)", "min",
                       adm_min_val, DEFAULT_ADM_MIN_VAL, 0.0, 1.0),
    {.name = nullptr,
     .help = nullptr,
     .alias = nullptr,
     .offset = 0,
     .type = VMAF_OPT_TYPE_BOOL,
     .default_val = {.b = false},
     .min = 0.0,
     .max = 0.0,
     .flags = 0}};

#undef FADM_BOOL_OPTION
#undef FADM_DOUBLE_OPTION
#undef FADM_INT_OPTION

template <typename T> static void fadm_free_buffer(FloatAdmStateSycl *s, T *&buffer)
{
    if (!buffer)
        return;
    vmaf_sycl_free(s->sycl_state, static_cast<void *>(buffer));
    buffer = nullptr;
}

static void fadm_release_buffers(FloatAdmStateSycl *s)
{
    if (!s->sycl_state)
        return;
    fadm_free_buffer(s, s->h_ref_raw);
    fadm_free_buffer(s, s->h_dis_raw);
    fadm_free_buffer(s, s->d_ref_raw);
    fadm_free_buffer(s, s->d_dis_raw);
    fadm_free_buffer(s, s->d_dwt_tmp_ref);
    fadm_free_buffer(s, s->d_dwt_tmp_dis);
    for (int scale = 0; scale < FADM_NUM_SCALES; scale++) {
        fadm_free_buffer(s, s->d_ref_band[scale]);
        fadm_free_buffer(s, s->d_dis_band[scale]);
        fadm_free_buffer(s, s->d_accum[scale]);
        fadm_free_buffer(s, s->h_accum[scale]);
    }
    fadm_free_buffer(s, s->d_csf_a);
    fadm_free_buffer(s, s->d_csf_f);
    fadm_free_buffer(s, s->d_csf_a_aim);
    fadm_free_buffer(s, s->d_csf_f_aim);
}

static int fadm_configure(FloatAdmStateSycl *s, VmafSyclState *sycl_state, unsigned bpc, unsigned w,
                          unsigned h)
{
    if (s->adm_csf_mode != 0)
        return -EINVAL;
    if (!sycl_state)
        return -EINVAL;
    s->width = w;
    s->height = h;
    s->bpc = bpc;
    s->has_pending = false;
    s->sycl_state = sycl_state;
    unsigned width = w;
    unsigned height = h;
    for (int scale = 0; scale < FADM_NUM_SCALES; scale++) {
        s->scale_w[scale] = width;
        s->scale_h[scale] = height;
        s->scale_half_w[scale] = (width + 1u) / 2u;
        s->scale_half_h[scale] = (height + 1u) / 2u;
        width = s->scale_half_w[scale];
        height = s->scale_half_h[scale];
    }
    s->buf_stride = (s->scale_half_w[0] + 3u) & ~3u;
    for (int scale = 0; scale < FADM_NUM_SCALES; scale++) {
        const float f1 =
            fadm_dwt_quant_step(scale, 1, s->adm_norm_view_dist, s->adm_ref_display_height);
        const float f2 =
            fadm_dwt_quant_step(scale, 2, s->adm_norm_view_dist, s->adm_ref_display_height);
        s->rfactor[scale * 3 + 0] = 1.0f / f1;
        s->rfactor[scale * 3 + 1] = 1.0f / f1;
        s->rfactor[scale * 3 + 2] = 1.0f / f2;
    }
    return 0;
}

static void fadm_allocate_shared_buffers(FloatAdmStateSycl *s)
{
    const size_t bpp = (s->bpc <= 8u) ? 1u : 2u;
    const size_t raw_bytes = (size_t)s->width * s->height * bpp;
    s->h_ref_raw = vmaf_sycl_malloc_host(s->sycl_state, raw_bytes);
    s->h_dis_raw = vmaf_sycl_malloc_host(s->sycl_state, raw_bytes);
    s->d_ref_raw = vmaf_sycl_malloc_device(s->sycl_state, raw_bytes);
    s->d_dis_raw = vmaf_sycl_malloc_device(s->sycl_state, raw_bytes);
    const size_t dwt_bytes = (size_t)s->width * 2u * s->scale_half_h[0] * sizeof(float);
    s->d_dwt_tmp_ref = static_cast<float *>(vmaf_sycl_malloc_device(s->sycl_state, dwt_bytes));
    s->d_dwt_tmp_dis = static_cast<float *>(vmaf_sycl_malloc_device(s->sycl_state, dwt_bytes));
    const size_t csf_bytes =
        (size_t)FADM_NUM_BANDS * s->buf_stride * s->scale_half_h[0] * sizeof(float);
    s->d_csf_a = static_cast<float *>(vmaf_sycl_malloc_device(s->sycl_state, csf_bytes));
    s->d_csf_f = static_cast<float *>(vmaf_sycl_malloc_device(s->sycl_state, csf_bytes));
    s->d_csf_a_aim = static_cast<float *>(vmaf_sycl_malloc_device(s->sycl_state, csf_bytes));
    s->d_csf_f_aim = static_cast<float *>(vmaf_sycl_malloc_device(s->sycl_state, csf_bytes));
}

static void fadm_allocate_scale_buffers(FloatAdmStateSycl *s)
{
    for (int scale = 0; scale < FADM_NUM_SCALES; scale++) {
        const size_t band_bytes =
            (size_t)4u * s->buf_stride * s->scale_half_h[scale] * sizeof(float);
        s->d_ref_band[scale] =
            static_cast<float *>(vmaf_sycl_malloc_device(s->sycl_state, band_bytes));
        s->d_dis_band[scale] =
            static_cast<float *>(vmaf_sycl_malloc_device(s->sycl_state, band_bytes));
        const int hh = (int)s->scale_half_h[scale];
        int top = (int)((double)hh * FADM_BORDER_FACTOR - 0.5);
        if (top < 0)
            top = 0;
        const int bottom = hh - top;
        const unsigned num_rows = (bottom > top) ? (unsigned)(bottom - top) : 1u;
        s->wg_count[scale] = 3u * num_rows;
        const size_t accum_bytes = (size_t)s->wg_count[scale] * FADM_ACCUM_SLOTS * sizeof(float);
        s->d_accum[scale] =
            static_cast<float *>(vmaf_sycl_malloc_device(s->sycl_state, accum_bytes));
        s->h_accum[scale] = static_cast<float *>(vmaf_sycl_malloc_host(s->sycl_state, accum_bytes));
    }
}

static bool fadm_buffers_ready(const FloatAdmStateSycl *s)
{
    if (!s->h_ref_raw || !s->h_dis_raw || !s->d_ref_raw || !s->d_dis_raw || !s->d_dwt_tmp_ref ||
        !s->d_dwt_tmp_dis || !s->d_csf_a || !s->d_csf_f || !s->d_csf_a_aim || !s->d_csf_f_aim)
        return false;
    for (int scale = 0; scale < FADM_NUM_SCALES; scale++) {
        if (!s->d_ref_band[scale] || !s->d_dis_band[scale] || !s->d_accum[scale] ||
            !s->h_accum[scale])
            return false;
    }
    return true;
}

static int init_fex_sycl(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                         unsigned w, unsigned h)
{
    (void)pix_fmt;
    auto *s = static_cast<FloatAdmStateSycl *>(fex->priv);
    int err = fadm_configure(s, fex->sycl_state, bpc, w, h);
    if (err)
        return err;
    fadm_allocate_shared_buffers(s);
    fadm_allocate_scale_buffers(s);
    if (!fadm_buffers_ready(s)) {
        fadm_release_buffers(s);
        return -ENOMEM;
    }
    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (!s->feature_name_dict) {
        fadm_release_buffers(s);
        return -ENOMEM;
    }
    return 0;
}

static void fadm_upload_frames(FloatAdmStateSycl *s, sycl::queue &queue, const VmafPicture *ref_pic,
                               const VmafPicture *dist_pic)
{
    const size_t bpp = (s->bpc <= 8u) ? 1u : 2u;
    if (s->bpc <= 8u) {
        copy_y_plane<uint8_t>(ref_pic, s->h_ref_raw, s->width, s->height);
        copy_y_plane<uint8_t>(dist_pic, s->h_dis_raw, s->width, s->height);
    } else {
        copy_y_plane<uint16_t>(ref_pic, s->h_ref_raw, s->width, s->height);
        copy_y_plane<uint16_t>(dist_pic, s->h_dis_raw, s->width, s->height);
    }
    const size_t raw_bytes = (size_t)s->width * s->height * bpp;
    queue.memcpy(s->d_ref_raw, s->h_ref_raw, raw_bytes);
    queue.memcpy(s->d_dis_raw, s->h_dis_raw, raw_bytes);
}

static void fadm_reset_accumulators(FloatAdmStateSycl *s, sycl::queue &queue)
{
    for (int scale = 0; scale < FADM_NUM_SCALES; scale++) {
        queue.memset(s->d_accum[scale], 0,
                     (size_t)s->wg_count[scale] * FADM_ACCUM_SLOTS * sizeof(float));
    }
}

static float fadm_sample_scaler(unsigned bpc)
{
    if (bpc == 10u)
        return 4.0f;
    if (bpc == 12u)
        return 16.0f;
    if (bpc == 16u)
        return 256.0f;
    return 1.0f;
}

struct FadmScaleGeometry {
    unsigned width;
    unsigned height;
    unsigned half_width;
    unsigned half_height;
    int left;
    int top;
    int right;
    int bottom;
};

static FadmScaleGeometry fadm_scale_geometry(const FloatAdmStateSycl *s, int scale)
{
    const unsigned half_width = s->scale_half_w[scale];
    const unsigned half_height = s->scale_half_h[scale];
    int top = (int)((double)half_height * FADM_BORDER_FACTOR - 0.5);
    int left = (int)((double)half_width * FADM_BORDER_FACTOR - 0.5);
    if (top < 0)
        top = 0;
    if (left < 0)
        left = 0;
    return FadmScaleGeometry{
        s->scale_w[scale],      s->scale_h[scale],     half_width, half_height, left, top,
        (int)half_width - left, (int)half_height - top};
}

template <int SCALE>
static sycl::event fadm_launch_vertical(FloatAdmStateSycl *s, sycl::queue &queue,
                                        const FadmScaleGeometry &geometry, unsigned raw_stride,
                                        float scaler)
{
    const float *parent_ref = SCALE > 0 ? s->d_ref_band[SCALE - 1] : nullptr;
    const float *parent_dis = SCALE > 0 ? s->d_dis_band[SCALE - 1] : nullptr;
    const unsigned parent_width = SCALE > 0 ? geometry.width : 0u;
    const unsigned parent_height = SCALE > 0 ? geometry.height : 0u;
    return launch_dwt_vert<SCALE>(queue, s->d_ref_raw, s->d_dis_raw, raw_stride, parent_ref,
                                  parent_dis, s->buf_stride, parent_width, parent_height,
                                  s->d_dwt_tmp_ref, s->d_dwt_tmp_dis, geometry.width,
                                  geometry.height, geometry.half_height, s->bpc, scaler, -128.0f);
}

static sycl::event fadm_dispatch_vertical(FloatAdmStateSycl *s, sycl::queue &queue, int scale,
                                          const FadmScaleGeometry &geometry, unsigned raw_stride,
                                          float scaler)
{
    switch (scale) {
    case 0:
        return fadm_launch_vertical<0>(s, queue, geometry, raw_stride, scaler);
    case 1:
        return fadm_launch_vertical<1>(s, queue, geometry, raw_stride, scaler);
    case 2:
        return fadm_launch_vertical<2>(s, queue, geometry, raw_stride, scaler);
    default:
        return fadm_launch_vertical<3>(s, queue, geometry, raw_stride, scaler);
    }
}

static void fadm_submit_scale(FloatAdmStateSycl *s, sycl::queue &queue, int scale,
                              unsigned raw_stride, float scaler)
{
    const FadmScaleGeometry g = fadm_scale_geometry(s, scale);
    fadm_dispatch_vertical(s, queue, scale, g, raw_stride, scaler);
    launch_dwt_hori(queue, s->d_dwt_tmp_ref, s->d_dwt_tmp_dis, s->d_ref_band[scale],
                    s->d_dis_band[scale], g.width, g.half_width, g.half_height, s->buf_stride);
    const float *rfactor = s->rfactor + scale * 3;
    launch_decouple_csf(queue, s->d_ref_band[scale], s->d_dis_band[scale], s->d_csf_a, s->d_csf_f,
                        g.half_width, g.half_height, s->buf_stride, rfactor[0], rfactor[1],
                        rfactor[2], (float)s->adm_enhn_gain_limit);
    launch_csf_cm(queue, s->d_ref_band[scale], s->d_dis_band[scale], s->d_csf_a, s->d_csf_f,
                  s->d_accum[scale], g.half_width, g.half_height, s->buf_stride, g.left, g.top,
                  g.right, g.bottom, rfactor[0], rfactor[1], rfactor[2],
                  (float)s->adm_enhn_gain_limit, (float)s->adm_p_norm);
    launch_csf_r(queue, s->d_ref_band[scale], s->d_dis_band[scale], s->d_csf_a_aim, s->d_csf_f_aim,
                 g.half_width, g.half_height, s->buf_stride, rfactor[0], rfactor[1], rfactor[2],
                 (float)s->adm_enhn_gain_limit);
    launch_aim_cm(queue, s->d_ref_band[scale], s->d_dis_band[scale], s->d_csf_a_aim, s->d_csf_f_aim,
                  s->d_accum[scale], g.half_width, g.half_height, s->buf_stride, g.left, g.top,
                  g.right, g.bottom, rfactor[0], rfactor[1], rfactor[2],
                  (float)s->adm_enhn_gain_limit, (float)s->adm_p_norm);
}

static void fadm_download_accumulators(FloatAdmStateSycl *s, sycl::queue &queue)
{
    for (int scale = 0; scale < FADM_NUM_SCALES; scale++) {
        queue.memcpy(s->h_accum[scale], s->d_accum[scale],
                     (size_t)s->wg_count[scale] * FADM_ACCUM_SLOTS * sizeof(float));
    }
}

static int submit_fex_sycl(VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                           VmafPicture *dist_pic, VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic_90;
    (void)dist_pic_90;
    auto *s = static_cast<FloatAdmStateSycl *>(fex->priv);
    auto *queue = static_cast<sycl::queue *>(vmaf_sycl_get_queue_ptr(s->sycl_state));
    if (!queue)
        return -EINVAL;
    try {
        fadm_upload_frames(s, *queue, ref_pic, dist_pic);
        fadm_reset_accumulators(s, *queue);
        const unsigned raw_stride = s->width * ((s->bpc <= 8u) ? 1u : 2u);
        const float scaler = fadm_sample_scaler(s->bpc);
        for (int scale = 0; scale < FADM_NUM_SCALES; scale++)
            fadm_submit_scale(s, *queue, scale, raw_stride, scaler);
        fadm_download_accumulators(s, *queue);
        s->pending_index = index;
        s->has_pending = true;
        return 0;
    } catch (const sycl::exception &exception) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "float_adm_sycl: submission failed: %s\n", exception.what());
        return -EIO;
    }
}

struct FadmTotals {
    double cm[FADM_NUM_SCALES][FADM_NUM_BANDS];
    double csf[FADM_NUM_SCALES][FADM_NUM_BANDS];
    double aim[FADM_NUM_SCALES][FADM_NUM_BANDS];
};

static FadmTotals fadm_gather_totals(const FloatAdmStateSycl *s)
{
    FadmTotals totals{};
    for (int scale = 0; scale < FADM_NUM_SCALES; scale++) {
        const float *slots = s->h_accum[scale];
        for (unsigned group = 0u; group < s->wg_count[scale]; group++) {
            const float *values = slots + (size_t)group * FADM_ACCUM_SLOTS;
            for (int band = 0; band < FADM_NUM_BANDS; band++) {
                totals.csf[scale][band] += (double)values[band];
                totals.cm[scale][band] += (double)values[3 + band];
                totals.aim[scale][band] += (double)values[6 + band];
            }
        }
    }
    return totals;
}

struct FadmPooled {
    double numerator;
    double denominator;
    double aim_numerator;
    double aim_denominator;
    double scales[8];
};

static FadmPooled fadm_pool_totals(const FloatAdmStateSycl *s, const FadmTotals &totals)
{
    FadmPooled pooled{};
    for (int scale = 0; scale < FADM_NUM_SCALES; scale++) {
        const FadmScaleGeometry geometry = fadm_scale_geometry(s, scale);
        const float inv_p = 1.0f / (float)s->adm_p_norm;
        const int active_area = (geometry.bottom - geometry.top) * (geometry.right - geometry.left);
        const float area_root = std::pow((float)active_area * (float)s->adm_noise_weight, inv_p);
        float num_scale = 0.0f;
        float den_scale = 0.0f;
        for (int band = 0; band < FADM_NUM_BANDS; band++) {
            num_scale += std::pow((float)totals.cm[scale][band], inv_p) + area_root;
            den_scale += std::pow((float)totals.csf[scale][band], inv_p) + area_root;
        }
        pooled.scales[2 * scale + 0] = num_scale;
        pooled.scales[2 * scale + 1] = den_scale;
        pooled.numerator += num_scale;
        pooled.denominator += den_scale;
        float aim_num_scale = 0.0f;
        for (int band = 0; band < FADM_NUM_BANDS; band++)
            aim_num_scale += std::pow((float)totals.aim[scale][band], inv_p);
        pooled.aim_denominator += den_scale;
        pooled.aim_numerator += aim_num_scale;
    }
    return pooled;
}

struct FadmScores {
    double adm;
    double aim;
    double adm3;
};

static FadmScores fadm_finalize_scores(const FloatAdmStateSycl *s, FadmPooled *pooled)
{
    const int w = (int)s->scale_w[0];
    const int h = (int)s->scale_h[0];
    const double numden_limit = 1e-2 * (double)(w * h) / (1920.0 * 1080.0);
    if (pooled->numerator < numden_limit)
        pooled->numerator = 0.0;
    if (pooled->denominator < numden_limit)
        pooled->denominator = 0.0;
    const double adm = pooled->denominator == 0.0 ? 1.0 : pooled->numerator / pooled->denominator;
    const double aim = pooled->aim_denominator == 0.0 ?
                           1.0 :
                           std::fmin(pooled->aim_numerator / pooled->aim_denominator, 1.0);
    double adm3;
    if (s->adm_adm3_apply_hm) {
        const double hm_denom = adm + aim;
        adm3 = hm_denom > 0.0 ? 2.0 * adm * aim / hm_denom : 0.0;
    } else {
        adm3 = adm * s->adm_dlm_weight + (1.0 - aim) * (1.0 - s->adm_dlm_weight);
    }
    if (adm3 < s->adm_min_val)
        adm3 = s->adm_min_val;
    return {adm, aim, adm3};
}

static int fadm_append(VmafFeatureCollector *collector, VmafDictionary *dictionary,
                       const char *name, double score, unsigned index)
{
    return vmaf_feature_collector_append_with_dict(collector, dictionary, name, score, index);
}

static int fadm_append_scores(const FloatAdmStateSycl *s, VmafFeatureCollector *collector,
                              const FadmPooled &pooled, const FadmScores &scores, unsigned index)
{
    static const char *const scale_names[] = {
        "VMAF_feature_adm_scale0_score", "VMAF_feature_adm_scale1_score",
        "VMAF_feature_adm_scale2_score", "VMAF_feature_adm_scale3_score"};
    int err =
        fadm_append(collector, s->feature_name_dict, "VMAF_feature_adm2_score", scores.adm, index);
    for (int scale = 0; scale < FADM_NUM_SCALES && !err; scale++) {
        err = fadm_append(collector, s->feature_name_dict, scale_names[scale],
                          pooled.scales[2 * scale] / pooled.scales[2 * scale + 1], index);
    }
    if (!err)
        err = fadm_append(collector, s->feature_name_dict, "VMAF_feature_aim_score", scores.aim,
                          index);
    if (!err)
        err = fadm_append(collector, s->feature_name_dict, "VMAF_feature_adm3_score", scores.adm3,
                          index);
    return err;
}

static int fadm_append_debug(const FloatAdmStateSycl *s, VmafFeatureCollector *collector,
                             const FadmPooled &pooled, const FadmScores &scores, unsigned index)
{
    if (!s->debug)
        return 0;
    int err = fadm_append(collector, s->feature_name_dict, "adm", scores.adm, index);
    if (!err)
        err = fadm_append(collector, s->feature_name_dict, "adm_num", pooled.numerator, index);
    if (!err)
        err = fadm_append(collector, s->feature_name_dict, "adm_den", pooled.denominator, index);
    static const char *const names[8] = {"adm_num_scale0", "adm_den_scale0", "adm_num_scale1",
                                         "adm_den_scale1", "adm_num_scale2", "adm_den_scale2",
                                         "adm_num_scale3", "adm_den_scale3"};
    for (int value = 0; value < 8 && !err; value++)
        err =
            fadm_append(collector, s->feature_name_dict, names[value], pooled.scales[value], index);
    return err;
}

static int collect_fex_sycl(VmafFeatureExtractor *fex, unsigned index, VmafFeatureCollector *fc)
{
    auto *s = static_cast<FloatAdmStateSycl *>(fex->priv);
    auto *queue = static_cast<sycl::queue *>(vmaf_sycl_get_queue_ptr(s->sycl_state));
    if (!queue)
        return -EINVAL;
    try {
        queue->wait_and_throw();
    } catch (const sycl::exception &exception) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "float_adm_sycl: execution failed: %s\n", exception.what());
        return -EIO;
    }
    const FadmTotals totals = fadm_gather_totals(s);
    FadmPooled pooled = fadm_pool_totals(s, totals);
    const FadmScores scores = fadm_finalize_scores(s, &pooled);
    int err = fadm_append_scores(s, fc, pooled, scores, index);
    if (!err)
        err = fadm_append_debug(s, fc, pooled, scores, index);
    return err;
}

static int close_fex_sycl(VmafFeatureExtractor *fex)
{
    auto *s = static_cast<FloatAdmStateSycl *>(fex->priv);
    fadm_release_buffers(s);
    return s->feature_name_dict ? vmaf_dictionary_free(&s->feature_name_dict) : 0;
}

static const char *provided_features_float_adm_sycl[] = {"VMAF_feature_adm2_score",
                                                         "VMAF_feature_adm_scale0_score",
                                                         "VMAF_feature_adm_scale1_score",
                                                         "VMAF_feature_adm_scale2_score",
                                                         "VMAF_feature_adm_scale3_score",
                                                         "VMAF_feature_aim_score",
                                                         "VMAF_feature_adm3_score",
                                                         "adm",
                                                         "adm_num",
                                                         "adm_den",
                                                         "adm_num_scale0",
                                                         "adm_den_scale0",
                                                         "adm_num_scale1",
                                                         "adm_den_scale1",
                                                         "adm_num_scale2",
                                                         "adm_den_scale2",
                                                         "adm_num_scale3",
                                                         "adm_den_scale3",
                                                         nullptr};

extern "C" VmafFeatureExtractor vmaf_fex_float_adm_sycl = {
    .name = "float_adm_sycl",
    .init = init_fex_sycl,
    .extract = nullptr,
    .flush = nullptr,
    .close = close_fex_sycl,
    .submit = submit_fex_sycl,
    .collect = collect_fex_sycl,
    .options = options_float_adm_sycl,
    .priv = nullptr,
    .priv_size = sizeof(FloatAdmStateSycl),
    .flags = VMAF_FEATURE_EXTRACTOR_SYCL,
    .provided_features = provided_features_float_adm_sycl,
    .sycl_state = nullptr,
    .framesync = nullptr,
    .prev_ref = {},
    .chars = {},
};
