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

/*
 * lint rationale: ADR-1266 keeps
 * file-local SYCL helpers static because Praetor misclassifies namespace scopes as functions.
 */

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

struct FadmDwtVertArgs {
    const void *ref_raw;
    const void *dis_raw;
    const float *parent_ref;
    const float *parent_dis;
    float *dwt_ref;
    float *dwt_dis;
    unsigned raw_stride;
    unsigned parent_stride;
    unsigned parent_w;
    unsigned parent_h;
    unsigned cur_w;
    unsigned cur_h;
    unsigned half_h;
    unsigned bpc;
    float scaler;
    float pixel_offset;
};

static float fadm_read_raw(const FadmDwtVertArgs &a, const void *plane, int y, int x)
{
    y = fadm_mirror_host(y, (int)a.cur_h);
    if (x < 0)
        x = 0;
    if (std::cmp_greater_equal(x, a.cur_w))
        x = (int)a.cur_w - 1;
    if (a.bpc <= 8u)
        return (float)static_cast<const uint8_t *>(plane)[y * a.raw_stride + x] + a.pixel_offset;
    const uint16_t v = reinterpret_cast<const uint16_t *>(static_cast<const uint8_t *>(plane) +
                                                          (size_t)y * a.raw_stride)[x];
    return (float)v / a.scaler + a.pixel_offset;
}

static float fadm_read_parent(const FadmDwtVertArgs &a, const float *band, int y, int x)
{
    y = fadm_mirror_host(y, (int)a.parent_h);
    if (x < 0)
        x = 0;
    if (std::cmp_greater_equal(x, a.parent_w))
        x = (int)a.parent_w - 1;
    return band[y * (int)a.parent_stride + x];
}

template <int SCALE>
static void fadm_dwt_vert_pixel(const FadmDwtVertArgs &a, sycl::nd_item<3> item)
{
    const int gx = (int)item.get_global_id(2);
    const int gy = (int)item.get_global_id(1);
    const int plane_is_dis = (int)item.get_global_id(0);
    if (std::cmp_greater_equal(gx, a.cur_w) || std::cmp_greater_equal(gy, a.half_h))
        return;

    const int row_start = 2 * gy - 1;
    float samples[4];
    for (int k = 0; k < 4; k++) {
        if constexpr (SCALE == 0) {
            const void *plane = (plane_is_dis == 0) ? a.ref_raw : a.dis_raw;
            samples[k] = fadm_read_raw(a, plane, row_start + k, gx);
        } else {
            const float *band = (plane_is_dis == 0) ? a.parent_ref : a.parent_dis;
            samples[k] = fadm_read_parent(a, band, row_start + k, gx);
        }
    }
    const float lo = FADM_LO0 * samples[0] + FADM_LO1 * samples[1] + FADM_LO2 * samples[2] +
                     FADM_LO3 * samples[3];
    const float hi = FADM_HI0 * samples[0] + FADM_HI1 * samples[1] + FADM_HI2 * samples[2] +
                     FADM_HI3 * samples[3];
    const int out_stride = (int)a.cur_w * 2;
    float *dst = (plane_is_dis == 0) ? a.dwt_ref : a.dwt_dis;
    dst[gy * out_stride + gx] = lo;
    dst[gy * out_stride + (int)a.cur_w + gx] = hi;
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
    const float *ref_band;
    const float *dis_band;
    float *csf_a;
    float *csf_f;
    unsigned half_w;
    unsigned half_h;
    unsigned buf_stride;
    float rfactor[3];
    float gain_limit;
};

static float fadm_retained_value(float original, float distorted, bool angle_flag, float gain_limit)
{
    float k = distorted / (original + FADM_EPS);
    k = sycl::fmax(0.0f, sycl::fmin(k, 1.0f));
    float retained = k * original;
    if (angle_flag && retained > 0.0f)
        retained = sycl::fmin(retained * gain_limit, distorted);
    else if (angle_flag && retained < 0.0f)
        retained = sycl::fmax(retained * gain_limit, distorted);
    return retained;
}

static bool fadm_angle_flag(const float original[3], const float distorted[3])
{
    const float dot = (original[0] * distorted[0]) + (original[1] * distorted[1]);
    const float original_mag = (original[0] * original[0]) + (original[1] * original[1]);
    const float distorted_mag = (distorted[0] * distorted[0]) + (distorted[1] * distorted[1]);
    const float lhs = dot * dot;
    const float rhs = FADM_COS_1DEG_SQ * (original_mag * distorted_mag);
    return (dot >= 0.0f) && (lhs >= rhs);
}

template <bool OUTPUT_ANOMALY>
static void fadm_decouple_pixel(const FadmDecoupleArgs &a, sycl::nd_item<2> item)
{
    const int x = (int)item.get_global_id(1);
    const int y = (int)item.get_global_id(0);
    if (std::cmp_greater_equal(x, a.half_w) || std::cmp_greater_equal(y, a.half_h))
        return;
    const int slice = (int)a.buf_stride * (int)a.half_h;
    float original[3];
    float distorted[3];
    for (int band = 0; band < FADM_NUM_BANDS; band++) {
        const int offset = (band + 1) * slice + y * (int)a.buf_stride + x;
        original[band] = a.ref_band[offset];
        distorted[band] = a.dis_band[offset];
    }
    const bool angle_flag = fadm_angle_flag(original, distorted);
    for (int band = 0; band < FADM_NUM_BANDS; band++) {
        const float retained =
            fadm_retained_value(original[band], distorted[band], angle_flag, a.gain_limit);
        const float component = OUTPUT_ANOMALY ? distorted[band] - retained : retained;
        const float value = a.rfactor[band] * component;
        const int offset = band * slice + y * (int)a.buf_stride + x;
        a.csf_a[offset] = value;
        a.csf_f[offset] = FADM_ONE_BY_30 * sycl::fabs(value);
    }
}

template <bool OUTPUT_ANOMALY>
static sycl::event launch_decouple(sycl::queue &q, const FadmDecoupleArgs &args)
{
    const size_t global_x = (size_t)((args.half_w + FADM_BX - 1u) / FADM_BX) * FADM_BX;
    const size_t global_y = (size_t)((args.half_h + FADM_BY - 1u) / FADM_BY) * FADM_BY;
    return q.submit([&](sycl::handler &cgh) {
        cgh.parallel_for(
            sycl::nd_range<2>(sycl::range<2>(global_y, global_x), sycl::range<2>(FADM_BY, FADM_BX)),
            [=](sycl::nd_item<2> item) { fadm_decouple_pixel<OUTPUT_ANOMALY>(args, item); });
    });
}

static sycl::event launch_decouple_csf(sycl::queue &q, const float *ref_band, const float *dis_band,
                                       float *csf_a, float *csf_f, unsigned half_w, unsigned half_h,
                                       unsigned buf_stride, float rfactor_h, float rfactor_v,
                                       float rfactor_d, float gain_limit)
{
    const FadmDecoupleArgs args{ref_band,  dis_band, csf_a,      csf_f,
                                half_w,    half_h,   buf_stride, {rfactor_h, rfactor_v, rfactor_d},
                                gain_limit};
    return launch_decouple<true>(q, args);
}

/* Stage 2b computes CSF of retained r rather than anomaly t-r (ADR-0574). */
static sycl::event launch_csf_r(sycl::queue &q, const float *ref_band, const float *dis_band,
                                float *csf_a_aim, float *csf_f_aim, unsigned half_w,
                                unsigned half_h, unsigned buf_stride, float rfactor_h,
                                float rfactor_v, float rfactor_d, float gain_limit)
{
    const FadmDecoupleArgs args{ref_band,  dis_band, csf_a_aim,  csf_f_aim,
                                half_w,    half_h,   buf_stride, {rfactor_h, rfactor_v, rfactor_d},
                                gain_limit};
    return launch_decouple<false>(q, args);
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

constexpr size_t FADM_WG_SIZE = (size_t)FADM_BX * FADM_BY;

struct FadmCmArgs {
    const float *ref_band;
    const float *dis_band;
    const float *csf_a;
    const float *csf_f;
    float *accum;
    unsigned half_w;
    unsigned half_h;
    unsigned buf_stride;
    int left;
    int top;
    int right;
    float rfactor[3];
    float gain_limit;
    float p_norm;
    unsigned active_h;
};

static float fadm_band_at(const float *data, const FadmCmArgs &a, int band, int y, int x)
{
    const int slice = (int)a.buf_stride * (int)a.half_h;
    return data[band * slice + y * (int)a.buf_stride + x];
}

static float fadm_csf_f_at(const FadmCmArgs &a, int band, int y, int x)
{
    if (x < 0)
        x = -x;
    if (std::cmp_greater_equal(x, a.half_w))
        x = (int)a.half_w - 1;
    if (y < 0)
        y = -y;
    if (std::cmp_greater_equal(y, a.half_h))
        y = (int)a.half_h - 1;
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    if (std::cmp_greater_equal(x, a.half_w))
        x = (int)a.half_w - 1;
    if (std::cmp_greater_equal(y, a.half_h))
        y = (int)a.half_h - 1;
    return fadm_band_at(a.csf_f, a, band, y, x);
}

static float fadm_csf_a_at(const FadmCmArgs &a, int band, int y, int x)
{
    if (x < 0)
        x = 0;
    if (std::cmp_greater_equal(x, a.half_w))
        x = (int)a.half_w - 1;
    if (y < 0)
        y = 0;
    if (std::cmp_greater_equal(y, a.half_h))
        y = (int)a.half_h - 1;
    return fadm_band_at(a.csf_a, a, band, y, x);
}

static float fadm_cm_threshold(const FadmCmArgs &a, int row, int col)
{
    float threshold = 0.0f;
    for (int band = 0; band < FADM_NUM_BANDS; band++) {
        for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                if (dx == 0 && dy == 0)
                    continue;
                threshold += fadm_csf_f_at(a, band, row + dy, col + dx);
            }
        }
    }
    threshold += FADM_ONE_BY_15 * sycl::fabs(fadm_csf_a_at(a, 0, row, col));
    threshold += FADM_ONE_BY_15 * sycl::fabs(fadm_csf_a_at(a, 1, row, col));
    threshold += FADM_ONE_BY_15 * sycl::fabs(fadm_csf_a_at(a, 2, row, col));
    return threshold;
}

static float fadm_retained_band(const FadmCmArgs &a, int row, int col, unsigned band)
{
    float original[3];
    float distorted[3];
    for (int index = 0; index < FADM_NUM_BANDS; index++) {
        original[index] = fadm_band_at(a.ref_band, a, index + 1, row, col);
        distorted[index] = fadm_band_at(a.dis_band, a, index + 1, row, col);
    }
    return fadm_retained_value(original[band], distorted[band],
                               fadm_angle_flag(original, distorted), a.gain_limit);
}

static float fadm_aim_row_sum(const FadmCmArgs &a, int row, unsigned band, unsigned lid)
{
    float sum = 0.0f;
    for (int col = a.left + (int)lid; col < a.right; col += (int)FADM_WG_SIZE) {
        const float distorted = fadm_band_at(a.dis_band, a, (int)band + 1, row, col);
        const float anomaly = distorted - fadm_retained_band(a, row, col, band);
        float value = sycl::fabs(a.rfactor[band] * anomaly) - fadm_cm_threshold(a, row, col);
        if (value < 0.0f)
            value = 0.0f;
        sum += fadm_pnorm_term(value, a.p_norm);
    }
    return sum;
}

struct FadmCsfCmSums {
    float csf;
    float cm;
};

static FadmCsfCmSums fadm_csf_cm_row_sums(const FadmCmArgs &a, int row, unsigned band, unsigned lid)
{
    FadmCsfCmSums sums{0.0f, 0.0f};
    for (int col = a.left + (int)lid; col < a.right; col += (int)FADM_WG_SIZE) {
        const float original = fadm_band_at(a.ref_band, a, (int)band + 1, row, col);
        const float csf_o = sycl::fabs(a.rfactor[band] * original);
        sums.csf += fadm_pnorm_term(csf_o, a.p_norm);
        const float retained = fadm_retained_band(a, row, col, band);
        float value = sycl::fabs(a.rfactor[band] * retained) - fadm_cm_threshold(a, row, col);
        if (value < 0.0f)
            value = 0.0f;
        sums.cm += fadm_pnorm_term(value, a.p_norm);
    }
    return sums;
}

static sycl::event launch_aim_cm(sycl::queue &q, const float *ref_band, const float *dis_band,
                                 const float *csf_a_aim, const float *csf_f_aim, float *accum_out,
                                 unsigned half_w, unsigned half_h, unsigned buf_stride,
                                 int active_left, int active_top, int active_right,
                                 int active_bottom, float rfactor_h, float rfactor_v,
                                 float rfactor_d, float gain_limit, float p_norm)
{
    const int active_h = active_bottom - active_top;
    const int active_w = active_right - active_left;
    if (active_h <= 0 || active_w <= 0)
        return sycl::event{};
    const size_t num_groups = (size_t)3 * active_h;
    const size_t global_x = num_groups * FADM_WG_SIZE;
    const FadmCmArgs args{ref_band,   dis_band,     csf_a_aim,
                          csf_f_aim,  accum_out,    half_w,
                          half_h,     buf_stride,   active_left,
                          active_top, active_right, {rfactor_h, rfactor_v, rfactor_d},
                          gain_limit, p_norm,       (unsigned)active_h};

    return q.submit([&](sycl::handler &cgh) {
        sycl::local_accessor<float, 1> const s_aim(sycl::range<1>(FADM_WG_SIZE / 32), cgh);
        cgh.parallel_for(sycl::nd_range<1>(sycl::range<1>(global_x), sycl::range<1>(FADM_WG_SIZE)),
                         [=](sycl::nd_item<1> item) VMAF_SYCL_REQD_SG_SIZE(32) {
                             const unsigned wg_id = (unsigned)item.get_group(0);
                             const unsigned lid = (unsigned)item.get_local_id(0);
                             const unsigned band_idx = wg_id / args.active_h;
                             const unsigned row_idx = wg_id - band_idx * args.active_h;
                             const int row = args.top + (int)row_idx;
                             const float local_aim_cm = fadm_aim_row_sum(args, row, band_idx, lid);
                             sycl::sub_group const sg = item.get_sub_group();
                             const float wn_aim =
                                 sycl::reduce_over_group(sg, local_aim_cm, sycl::plus<float>{});
                             const uint32_t sg_id = sg.get_group_linear_id();
                             const uint32_t sg_lid = sg.get_local_linear_id();
                             const uint32_t n_sg = sg.get_group_linear_range();
                             if (sg_lid == 0)
                                 s_aim[sg_id] = wn_aim;
                             item.barrier(sycl::access::fence_space::local_space);
                             if (lid == 0) {
                                 float total_aim = 0.0f;
                                 for (uint32_t i = 0; i < n_sg; i++)
                                     total_aim += s_aim[i];
                                 const unsigned slot_base = wg_id * FADM_ACCUM_SLOTS;
                                 args.accum[slot_base + 6u + band_idx] = total_aim;
                             }
                         });
    });
}

/* ------------------------------------------------------------------ */
/* Stage 3 — CSF denominator + CM fused.                               */
/* ------------------------------------------------------------------ */
static sycl::event launch_csf_cm(sycl::queue &q, const float *ref_band, const float *dis_band,
                                 const float *csf_a, const float *csf_f, float *accum_out,
                                 unsigned half_w, unsigned half_h, unsigned buf_stride,
                                 int active_left, int active_top, int active_right,
                                 int active_bottom, float rfactor_h, float rfactor_v,
                                 float rfactor_d, float gain_limit, float p_norm)
{
    const int active_h = active_bottom - active_top;
    const int active_w = active_right - active_left;
    if (active_h <= 0 || active_w <= 0)
        return sycl::event{};
    const size_t num_groups = (size_t)3 * active_h;
    const size_t global_x = num_groups * FADM_WG_SIZE;
    const FadmCmArgs args{ref_band,   dis_band,     csf_a,
                          csf_f,      accum_out,    half_w,
                          half_h,     buf_stride,   active_left,
                          active_top, active_right, {rfactor_h, rfactor_v, rfactor_d},
                          gain_limit, p_norm,       (unsigned)active_h};

    return q.submit([&](sycl::handler &cgh) {
        sycl::local_accessor<float, 1> const s_csf(sycl::range<1>(FADM_WG_SIZE / 32), cgh);
        sycl::local_accessor<float, 1> const s_cm(sycl::range<1>(FADM_WG_SIZE / 32), cgh);
        cgh.parallel_for(
            sycl::nd_range<1>(sycl::range<1>(global_x), sycl::range<1>(FADM_WG_SIZE)),
            [=](sycl::nd_item<1> item) VMAF_SYCL_REQD_SG_SIZE(32) {
                const unsigned wg_id = (unsigned)item.get_group(0);
                const unsigned lid = (unsigned)item.get_local_id(0);
                const unsigned band_idx = wg_id / args.active_h;
                const unsigned row_idx = wg_id - band_idx * args.active_h;
                const int row = args.top + (int)row_idx;
                const FadmCsfCmSums sums = fadm_csf_cm_row_sums(args, row, band_idx, lid);
                sycl::sub_group const sg = item.get_sub_group();
                const float wn_csf = sycl::reduce_over_group(sg, sums.csf, sycl::plus<float>{});
                const float wn_cm = sycl::reduce_over_group(sg, sums.cm, sycl::plus<float>{});
                const uint32_t sg_id = sg.get_group_linear_id();
                const uint32_t sg_lid = sg.get_local_linear_id();
                const uint32_t n_sg = sg.get_group_linear_range();
                if (sg_lid == 0) {
                    s_csf[sg_id] = wn_csf;
                    s_cm[sg_id] = wn_cm;
                }
                item.barrier(sycl::access::fence_space::local_space);
                if (lid == 0) {
                    float total_csf = 0.0f;
                    float total_cm = 0.0f;
                    for (uint32_t i = 0; i < n_sg; i++) {
                        total_csf += s_csf[i];
                        total_cm += s_cm[i];
                    }
                    const unsigned slot_base = wg_id * FADM_ACCUM_SLOTS;
                    args.accum[slot_base + band_idx] = total_csf;
                    args.accum[slot_base + 3u + band_idx] = total_cm;
                }
            });
    });
}

// clang-format off
static const VmafOption options_float_adm_sycl[] = {
    {.name = "debug", .help = "debug mode", .offset = offsetof(FloatAdmStateSycl, debug),
     .type = VMAF_OPT_TYPE_BOOL, .default_val = {.b = false}},
    {.name = "adm_enhn_gain_limit", .help = "enhancement gain (>=1.0)", .alias = "egl",
     .offset = offsetof(FloatAdmStateSycl, adm_enhn_gain_limit),
     .type = VMAF_OPT_TYPE_DOUBLE, .default_val = {.d = 100.0}, .min = 1.0, .max = 100.0,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_norm_view_dist", .help = "normalized viewing distance", .alias = "nvd",
     .offset = offsetof(FloatAdmStateSycl, adm_norm_view_dist),
     .type = VMAF_OPT_TYPE_DOUBLE, .default_val = {.d = 3.0}, .min = 0.75, .max = 24.0,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_ref_display_height", .help = "reference display height in pixels", .alias = "rdf",
     .offset = offsetof(FloatAdmStateSycl, adm_ref_display_height),
     .type = VMAF_OPT_TYPE_INT, .default_val = {.i = 1080}, .min = 1, .max = 4320,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_csf_mode", .help = "contrast sensitivity function (mode 0 only on SYCL v1)",
     .alias = "csf",
     .offset = offsetof(FloatAdmStateSycl, adm_csf_mode),
     .type = VMAF_OPT_TYPE_INT, .default_val = {.i = 0}, .min = 0, .max = 9,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_csf_scale",
     .help = "CSF band-scale multiplier for h/v bands (default 1.0 = no scaling)", .alias = "scf",
     .offset = offsetof(FloatAdmStateSycl, adm_csf_scale),
     .type = VMAF_OPT_TYPE_DOUBLE, .default_val = {.d = DEFAULT_ADM_CSF_SCALE},
     .min = 0.0, .max = 100.0, .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_csf_diag_scale",
     .help = "CSF band-scale multiplier for diagonal bands (default 1.0 = no scaling)",
     .alias = "scfd",
     .offset = offsetof(FloatAdmStateSycl, adm_csf_diag_scale),
     .type = VMAF_OPT_TYPE_DOUBLE, .default_val = {.d = DEFAULT_ADM_CSF_DIAG_SCALE},
     .min = 0.0, .max = 100.0, .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_noise_weight",
     .help = "noise floor weight for CM numerator (default 0.03125 = 1/32)", .alias = "nw",
     .offset = offsetof(FloatAdmStateSycl, adm_noise_weight),
     .type = VMAF_OPT_TYPE_DOUBLE, .default_val = {.d = DEFAULT_ADM_NOISE_WEIGHT},
     .min = 0.0, .max = 100.0, .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_adm3_apply_hm",
     .help = "apply harmonic mean for adm3 score (false = linear blend)", .alias = "aah",
     .offset = offsetof(FloatAdmStateSycl, adm_adm3_apply_hm),
     .type = VMAF_OPT_TYPE_BOOL, .default_val = {.b = false},
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_p_norm", .help = "p-norm exponent for AIM/ADM3 score (default 3.0)",
     .alias = "apn",
     .offset = offsetof(FloatAdmStateSycl, adm_p_norm),
     .type = VMAF_OPT_TYPE_DOUBLE, .default_val = {.d = 3.0}, .min = 1.0, .max = 20.0,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_dlm_weight", .help = "DLM weight for linear-blend adm3 score (default 0.5)",
     .alias = "dlmw",
     .offset = offsetof(FloatAdmStateSycl, adm_dlm_weight),
     .type = VMAF_OPT_TYPE_DOUBLE, .default_val = {.d = 0.5}, .min = 0.0, .max = 1.0,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_min_val", .help = "minimum clamp for adm3 score (default 0.0)", .alias = "min",
     .offset = offsetof(FloatAdmStateSycl, adm_min_val),
     .type = VMAF_OPT_TYPE_DOUBLE, .default_val = {.d = DEFAULT_ADM_MIN_VAL},
     .min = 0.0, .max = 1.0, .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = nullptr}};
// clang-format on

static void fadm_init_dimensions(FloatAdmStateSycl *s, unsigned w, unsigned h)
{
    unsigned current_w = w;
    unsigned current_h = h;
    for (int scale = 0; scale < FADM_NUM_SCALES; scale++) {
        s->scale_w[scale] = current_w;
        s->scale_h[scale] = current_h;
        s->scale_half_w[scale] = (current_w + 1u) / 2u;
        s->scale_half_h[scale] = (current_h + 1u) / 2u;
        current_w = s->scale_half_w[scale];
        current_h = s->scale_half_h[scale];
    }
    s->buf_stride = (s->scale_half_w[0] + 3u) & ~3u;
}

/* Watson-97 mode matches adm_tools.c: its rfactor ignores the Barten-only
 * adm_csf_scale and adm_csf_diag_scale options (ADR-1214). */
static void fadm_init_rfactors(FloatAdmStateSycl *s)
{
    for (int scale = 0; scale < FADM_NUM_SCALES; scale++) {
        const float f1 =
            fadm_dwt_quant_step(scale, 1, s->adm_norm_view_dist, s->adm_ref_display_height);
        const float f2 =
            fadm_dwt_quant_step(scale, 2, s->adm_norm_view_dist, s->adm_ref_display_height);
        s->rfactor[scale * 3 + 0] = 1.0f / f1;
        s->rfactor[scale * 3 + 1] = 1.0f / f1;
        s->rfactor[scale * 3 + 2] = 1.0f / f2;
    }
}

static void fadm_allocate_planes(FloatAdmStateSycl *s)
{
    const size_t bytes_per_pixel = (s->bpc <= 8u) ? 1u : 2u;
    const size_t raw_bytes = (size_t)s->width * s->height * bytes_per_pixel;
    s->h_ref_raw = vmaf_sycl_malloc_host(s->sycl_state, raw_bytes);
    s->h_dis_raw = vmaf_sycl_malloc_host(s->sycl_state, raw_bytes);
    s->d_ref_raw = vmaf_sycl_malloc_device(s->sycl_state, raw_bytes);
    s->d_dis_raw = vmaf_sycl_malloc_device(s->sycl_state, raw_bytes);

    const size_t dwt_bytes = (size_t)s->width * 2u * s->scale_half_h[0] * sizeof(float);
    s->d_dwt_tmp_ref = static_cast<float *>(vmaf_sycl_malloc_device(s->sycl_state, dwt_bytes));
    s->d_dwt_tmp_dis = static_cast<float *>(vmaf_sycl_malloc_device(s->sycl_state, dwt_bytes));
    for (int scale = 0; scale < FADM_NUM_SCALES; scale++) {
        const size_t band_bytes =
            (size_t)4u * s->buf_stride * s->scale_half_h[scale] * sizeof(float);
        s->d_ref_band[scale] =
            static_cast<float *>(vmaf_sycl_malloc_device(s->sycl_state, band_bytes));
        s->d_dis_band[scale] =
            static_cast<float *>(vmaf_sycl_malloc_device(s->sycl_state, band_bytes));
    }
    const size_t csf_bytes =
        (size_t)FADM_NUM_BANDS * s->buf_stride * s->scale_half_h[0] * sizeof(float);
    s->d_csf_a = static_cast<float *>(vmaf_sycl_malloc_device(s->sycl_state, csf_bytes));
    s->d_csf_f = static_cast<float *>(vmaf_sycl_malloc_device(s->sycl_state, csf_bytes));
    s->d_csf_a_aim = static_cast<float *>(vmaf_sycl_malloc_device(s->sycl_state, csf_bytes));
    s->d_csf_f_aim = static_cast<float *>(vmaf_sycl_malloc_device(s->sycl_state, csf_bytes));
}

static void fadm_allocate_accumulators(FloatAdmStateSycl *s)
{
    for (int scale = 0; scale < FADM_NUM_SCALES; scale++) {
        const int half_height = (int)s->scale_half_h[scale];
        int top = (int)((double)half_height * FADM_BORDER_FACTOR - 0.5);
        if (top < 0)
            top = 0;
        const int bottom = half_height - top;
        const unsigned rows = (bottom > top) ? (unsigned)(bottom - top) : 1u;
        s->wg_count[scale] = 3u * rows;
        const size_t bytes = (size_t)s->wg_count[scale] * FADM_ACCUM_SLOTS * sizeof(float);
        s->d_accum[scale] = static_cast<float *>(vmaf_sycl_malloc_device(s->sycl_state, bytes));
        s->h_accum[scale] = static_cast<float *>(vmaf_sycl_malloc_host(s->sycl_state, bytes));
    }
}

static bool fadm_buffers_valid(const FloatAdmStateSycl *s)
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

    if (s->adm_csf_mode != 0)
        return -EINVAL;

    s->width = w;
    s->height = h;
    s->bpc = bpc;
    s->has_pending = false;

    fadm_init_dimensions(s, w, h);
    fadm_init_rfactors(s);

    if (!fex->sycl_state)
        return -EINVAL;
    s->sycl_state = fex->sycl_state;

    fadm_allocate_planes(s);
    fadm_allocate_accumulators(s);
    if (!fadm_buffers_valid(s))
        return -ENOMEM;

    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (!s->feature_name_dict)
        return -ENOMEM;
    return 0;
}

static float fadm_pixel_scaler(unsigned bpc)
{
    if (bpc == 10u)
        return 4.0f;
    if (bpc == 12u)
        return 16.0f;
    if (bpc == 16u)
        return 256.0f;
    return 1.0f;
}

static void fadm_upload_pictures(FloatAdmStateSycl *s, sycl::queue &q, const VmafPicture *ref_pic,
                                 const VmafPicture *dist_pic)
{
    const size_t bytes_per_pixel = (s->bpc <= 8u) ? 1u : 2u;
    if (s->bpc <= 8u) {
        copy_y_plane<uint8_t>(ref_pic, s->h_ref_raw, s->width, s->height);
        copy_y_plane<uint8_t>(dist_pic, s->h_dis_raw, s->width, s->height);
    } else {
        copy_y_plane<uint16_t>(ref_pic, s->h_ref_raw, s->width, s->height);
        copy_y_plane<uint16_t>(dist_pic, s->h_dis_raw, s->width, s->height);
    }
    const size_t raw_bytes = (size_t)s->width * s->height * bytes_per_pixel;
    q.memcpy(s->d_ref_raw, s->h_ref_raw, raw_bytes);
    q.memcpy(s->d_dis_raw, s->h_dis_raw, raw_bytes);
}

static void fadm_reset_accumulators(FloatAdmStateSycl *s, sycl::queue &q)
{
    for (int scale = 0; scale < FADM_NUM_SCALES; scale++) {
        q.memset(s->d_accum[scale], 0,
                 (size_t)s->wg_count[scale] * FADM_ACCUM_SLOTS * sizeof(float));
    }
}

struct FadmActiveBounds {
    int left;
    int top;
    int right;
    int bottom;
};

static FadmActiveBounds fadm_active_bounds(unsigned half_w, unsigned half_h)
{
    int top = (int)((double)half_h * FADM_BORDER_FACTOR - 0.5);
    int left = (int)((double)half_w * FADM_BORDER_FACTOR - 0.5);
    if (top < 0)
        top = 0;
    if (left < 0)
        left = 0;
    return {left, top, (int)half_w - left, (int)half_h - top};
}

template <int SCALE>
static void fadm_launch_dwt_scale(FloatAdmStateSycl *s, sycl::queue &q, int scale,
                                  unsigned raw_stride, float scaler)
{
    const unsigned parent_w = (scale > 0) ? s->scale_w[scale] : 0u;
    const unsigned parent_h = (scale > 0) ? s->scale_h[scale] : 0u;
    const float *parent_ref = (scale > 0) ? s->d_ref_band[scale - 1] : nullptr;
    const float *parent_dis = (scale > 0) ? s->d_dis_band[scale - 1] : nullptr;
    launch_dwt_vert<SCALE>(q, s->d_ref_raw, s->d_dis_raw, raw_stride, parent_ref, parent_dis,
                           s->buf_stride, parent_w, parent_h, s->d_dwt_tmp_ref, s->d_dwt_tmp_dis,
                           s->scale_w[scale], s->scale_h[scale], s->scale_half_h[scale], s->bpc,
                           scaler, -128.0f);
}

static void fadm_dispatch_dwt(FloatAdmStateSycl *s, sycl::queue &q, int scale, unsigned raw_stride,
                              float scaler)
{
    if (scale == 0)
        fadm_launch_dwt_scale<0>(s, q, scale, raw_stride, scaler);
    else if (scale == 1)
        fadm_launch_dwt_scale<1>(s, q, scale, raw_stride, scaler);
    else if (scale == 2)
        fadm_launch_dwt_scale<2>(s, q, scale, raw_stride, scaler);
    else
        fadm_launch_dwt_scale<3>(s, q, scale, raw_stride, scaler);
}

static void fadm_launch_scale(FloatAdmStateSycl *s, sycl::queue &q, int scale, unsigned raw_stride,
                              float scaler)
{
    const unsigned width = s->scale_w[scale];
    const unsigned half_w = s->scale_half_w[scale];
    const unsigned half_h = s->scale_half_h[scale];
    const FadmActiveBounds bounds = fadm_active_bounds(half_w, half_h);
    fadm_dispatch_dwt(s, q, scale, raw_stride, scaler);
    launch_dwt_hori(q, s->d_dwt_tmp_ref, s->d_dwt_tmp_dis, s->d_ref_band[scale],
                    s->d_dis_band[scale], width, half_w, half_h, s->buf_stride);
    launch_decouple_csf(q, s->d_ref_band[scale], s->d_dis_band[scale], s->d_csf_a, s->d_csf_f,
                        half_w, half_h, s->buf_stride, s->rfactor[scale * 3],
                        s->rfactor[scale * 3 + 1], s->rfactor[scale * 3 + 2],
                        (float)s->adm_enhn_gain_limit);
    launch_csf_cm(q, s->d_ref_band[scale], s->d_dis_band[scale], s->d_csf_a, s->d_csf_f,
                  s->d_accum[scale], half_w, half_h, s->buf_stride, bounds.left, bounds.top,
                  bounds.right, bounds.bottom, s->rfactor[scale * 3], s->rfactor[scale * 3 + 1],
                  s->rfactor[scale * 3 + 2], (float)s->adm_enhn_gain_limit, (float)s->adm_p_norm);
    launch_csf_r(q, s->d_ref_band[scale], s->d_dis_band[scale], s->d_csf_a_aim, s->d_csf_f_aim,
                 half_w, half_h, s->buf_stride, s->rfactor[scale * 3], s->rfactor[scale * 3 + 1],
                 s->rfactor[scale * 3 + 2], (float)s->adm_enhn_gain_limit);
    launch_aim_cm(q, s->d_ref_band[scale], s->d_dis_band[scale], s->d_csf_a_aim, s->d_csf_f_aim,
                  s->d_accum[scale], half_w, half_h, s->buf_stride, bounds.left, bounds.top,
                  bounds.right, bounds.bottom, s->rfactor[scale * 3], s->rfactor[scale * 3 + 1],
                  s->rfactor[scale * 3 + 2], (float)s->adm_enhn_gain_limit, (float)s->adm_p_norm);
}

static void fadm_download_accumulators(FloatAdmStateSycl *s, sycl::queue &q)
{
    for (int scale = 0; scale < FADM_NUM_SCALES; scale++) {
        q.memcpy(s->h_accum[scale], s->d_accum[scale],
                 (size_t)s->wg_count[scale] * FADM_ACCUM_SLOTS * sizeof(float));
    }
}

static int submit_fex_sycl(VmafFeatureExtractor *fex, const VmafPicture *ref_pic,
                           const VmafPicture *ref_pic_90, const VmafPicture *dist_pic,
                           const VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic_90;
    (void)dist_pic_90;
    auto *s = static_cast<FloatAdmStateSycl *>(fex->priv);
    auto *qptr = static_cast<sycl::queue *>(vmaf_sycl_get_queue_ptr(s->sycl_state));
    if (!qptr)
        return -EINVAL;
    sycl::queue &q = *qptr;

    const size_t bytes_per_pixel = (s->bpc <= 8u) ? 1u : 2u;
    const unsigned raw_stride = (unsigned)(s->width * bytes_per_pixel);
    fadm_upload_pictures(s, q, ref_pic, dist_pic);
    fadm_reset_accumulators(s, q);
    const float scaler = fadm_pixel_scaler(s->bpc);
    for (int scale = 0; scale < FADM_NUM_SCALES; scale++)
        fadm_launch_scale(s, q, scale, raw_stride, scaler);
    fadm_download_accumulators(s, q);

    s->pending_index = index;
    s->has_pending = true;
    return 0;
}

struct FadmTotals {
    double cm[FADM_NUM_SCALES][FADM_NUM_BANDS];
    double csf[FADM_NUM_SCALES][FADM_NUM_BANDS];
    double aim_cm[FADM_NUM_SCALES][FADM_NUM_BANDS];
};

struct FadmPooledScores {
    double numerator;
    double denominator;
    double aim_numerator;
    double aim_denominator;
    double scales[8];
};

struct FadmFinalScores {
    double adm2;
    double aim;
    double adm3;
};

static FadmTotals fadm_accumulate_totals(const FloatAdmStateSycl *s)
{
    FadmTotals totals{};
    for (int scale = 0; scale < FADM_NUM_SCALES; scale++) {
        const float *slots = s->h_accum[scale];
        for (unsigned wg = 0u; wg < s->wg_count[scale]; wg++) {
            const float *values = slots + (size_t)wg * FADM_ACCUM_SLOTS;
            for (int band = 0; band < FADM_NUM_BANDS; band++) {
                totals.csf[scale][band] += (double)values[band];
                totals.cm[scale][band] += (double)values[3 + band];
                totals.aim_cm[scale][band] += (double)values[6 + band];
            }
        }
    }
    return totals;
}

static void fadm_pool_scale(const FloatAdmStateSycl *s, const FadmTotals &totals,
                            FadmPooledScores *pooled, int scale)
{
    const unsigned half_w = s->scale_half_w[scale];
    const unsigned half_h = s->scale_half_h[scale];
    const FadmActiveBounds bounds = fadm_active_bounds(half_w, half_h);
    const float inverse_p = 1.0f / (float)s->adm_p_norm;
    const float noise =
        std::pow((float)((bounds.bottom - bounds.top) * (bounds.right - bounds.left)) *
                     (float)s->adm_noise_weight,
                 inverse_p);
    float numerator = 0.0f;
    float denominator = 0.0f;
    for (int band = 0; band < FADM_NUM_BANDS; band++) {
        numerator += std::pow((float)totals.cm[scale][band], inverse_p) + noise;
        denominator += std::pow((float)totals.csf[scale][band], inverse_p) + noise;
    }
    pooled->scales[2 * scale] = numerator;
    pooled->scales[2 * scale + 1] = denominator;
    pooled->numerator += numerator;
    pooled->denominator += denominator;

    float aim_numerator = 0.0f;
    for (int band = 0; band < FADM_NUM_BANDS; band++)
        aim_numerator += std::pow((float)totals.aim_cm[scale][band], inverse_p);
    pooled->aim_denominator += denominator;
    pooled->aim_numerator += aim_numerator;
}

static FadmPooledScores fadm_pool_scores(const FloatAdmStateSycl *s, const FadmTotals &totals)
{
    FadmPooledScores pooled{};
    for (int scale = 0; scale < FADM_NUM_SCALES; scale++)
        fadm_pool_scale(s, totals, &pooled, scale);
    return pooled;
}

static FadmFinalScores fadm_finalize_scores(const FloatAdmStateSycl *s, FadmPooledScores *pooled)
{
    const int width = (int)s->scale_w[0];
    const int height = (int)s->scale_h[0];
    const double limit = 1e-2 * (double)(width * height) / (1920.0 * 1080.0);
    if (pooled->numerator < limit)
        pooled->numerator = 0.0;
    if (pooled->denominator < limit)
        pooled->denominator = 0.0;
    const double adm2 =
        (pooled->denominator == 0.0) ? 1.0 : pooled->numerator / pooled->denominator;
    const double aim = (pooled->aim_denominator == 0.0) ?
                           1.0 :
                           std::fmin(pooled->aim_numerator / pooled->aim_denominator, 1.0);
    double adm3 = 0.0;
    if (s->adm_adm3_apply_hm) {
        const double denominator = adm2 + aim;
        adm3 = (denominator > 0.0) ? (2.0 * adm2 * aim / denominator) : 0.0;
    } else {
        adm3 = adm2 * s->adm_dlm_weight + (1.0 - aim) * (1.0 - s->adm_dlm_weight);
    }
    if (adm3 < s->adm_min_val)
        adm3 = s->adm_min_val;
    return {adm2, aim, adm3};
}

static int fadm_append_scores(const FloatAdmStateSycl *s, VmafFeatureCollector *fc, unsigned index,
                              const FadmPooledScores &pooled, const FadmFinalScores &scores)
{
    int err = 0;
    err |= vmaf_feature_collector_append_with_dict(fc, s->feature_name_dict,
                                                   "VMAF_feature_adm2_score", scores.adm2, index);
    const char *const names[4] = {"VMAF_feature_adm_scale0_score", "VMAF_feature_adm_scale1_score",
                                  "VMAF_feature_adm_scale2_score", "VMAF_feature_adm_scale3_score"};
    for (int scale = 0; scale < FADM_NUM_SCALES; scale++) {
        err |= vmaf_feature_collector_append_with_dict(
            fc, s->feature_name_dict, names[scale],
            pooled.scales[2 * scale] / pooled.scales[2 * scale + 1], index);
    }
    err |= vmaf_feature_collector_append_with_dict(fc, s->feature_name_dict,
                                                   "VMAF_feature_aim_score", scores.aim, index);
    err |= vmaf_feature_collector_append_with_dict(fc, s->feature_name_dict,
                                                   "VMAF_feature_adm3_score", scores.adm3, index);
    return err;
}

static int fadm_append_debug(const FloatAdmStateSycl *s, VmafFeatureCollector *fc, unsigned index,
                             const FadmPooledScores &pooled, double adm2)
{
    int err = 0;
    err |= vmaf_feature_collector_append_with_dict(fc, s->feature_name_dict, "adm", adm2, index);
    err |= vmaf_feature_collector_append_with_dict(fc, s->feature_name_dict, "adm_num",
                                                   pooled.numerator, index);
    err |= vmaf_feature_collector_append_with_dict(fc, s->feature_name_dict, "adm_den",
                                                   pooled.denominator, index);
    const char *const names[8] = {"adm_num_scale0", "adm_den_scale0", "adm_num_scale1",
                                  "adm_den_scale1", "adm_num_scale2", "adm_den_scale2",
                                  "adm_num_scale3", "adm_den_scale3"};
    for (int i = 0; i < 8 && !err; i++) {
        err |= vmaf_feature_collector_append_with_dict(fc, s->feature_name_dict, names[i],
                                                       pooled.scales[i], index);
    }
    return err;
}

static int collect_fex_sycl(VmafFeatureExtractor *fex, unsigned index, VmafFeatureCollector *fc)
{
    auto *s = static_cast<FloatAdmStateSycl *>(fex->priv);
    auto *qptr = static_cast<sycl::queue *>(vmaf_sycl_get_queue_ptr(s->sycl_state));
    if (!qptr)
        return -EINVAL;
    qptr->wait();

    const FadmTotals totals = fadm_accumulate_totals(s);
    FadmPooledScores pooled = fadm_pool_scores(s, totals);
    const FadmFinalScores scores = fadm_finalize_scores(s, &pooled);
    int err = fadm_append_scores(s, fc, index, pooled, scores);
    if (s->debug && !err)
        err = fadm_append_debug(s, fc, index, pooled, scores.adm2);
    return err;
}

static int close_fex_sycl(VmafFeatureExtractor *fex)
{
    auto *s = static_cast<FloatAdmStateSycl *>(fex->priv);
    if (s->sycl_state) {
        if (s->h_ref_raw)
            vmaf_sycl_free(s->sycl_state, s->h_ref_raw);
        if (s->h_dis_raw)
            vmaf_sycl_free(s->sycl_state, s->h_dis_raw);
        if (s->d_ref_raw)
            vmaf_sycl_free(s->sycl_state, s->d_ref_raw);
        if (s->d_dis_raw)
            vmaf_sycl_free(s->sycl_state, s->d_dis_raw);
        if (s->d_dwt_tmp_ref)
            vmaf_sycl_free(s->sycl_state, s->d_dwt_tmp_ref);
        if (s->d_dwt_tmp_dis)
            vmaf_sycl_free(s->sycl_state, s->d_dwt_tmp_dis);
        for (int scale = 0; scale < FADM_NUM_SCALES; scale++) {
            if (s->d_ref_band[scale])
                vmaf_sycl_free(s->sycl_state, s->d_ref_band[scale]);
            if (s->d_dis_band[scale])
                vmaf_sycl_free(s->sycl_state, s->d_dis_band[scale]);
            if (s->d_accum[scale])
                vmaf_sycl_free(s->sycl_state, s->d_accum[scale]);
            if (s->h_accum[scale])
                vmaf_sycl_free(s->sycl_state, s->h_accum[scale]);
        }
        if (s->d_csf_a)
            vmaf_sycl_free(s->sycl_state, s->d_csf_a);
        if (s->d_csf_f)
            vmaf_sycl_free(s->sycl_state, s->d_csf_f);
        /* ADR-0574: AIM CSF buffers. */
        if (s->d_csf_a_aim)
            vmaf_sycl_free(s->sycl_state, s->d_csf_a_aim);
        if (s->d_csf_f_aim)
            vmaf_sycl_free(s->sycl_state, s->d_csf_f_aim);
    }
    if (s->feature_name_dict)
        vmaf_dictionary_free(&s->feature_name_dict);
    return 0;
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

/*
 * lint rationale: ADR-1266 ends the
 * file-local helper band before the exported C-linkage descriptor.
 */

extern "C" VmafFeatureExtractor vmaf_fex_float_adm_sycl = {
    .name = "float_adm_sycl",
    .init = init_fex_sycl,
    .extract = nullptr,
    .flush = nullptr,
    .close = close_fex_sycl,
    .submit = submit_fex_sycl,
    .collect = collect_fex_sycl,
    .options = options_float_adm_sycl,
    .priv_size = sizeof(FloatAdmStateSycl),
    .flags = VMAF_FEATURE_EXTRACTOR_SYCL,
    .provided_features = provided_features_float_adm_sycl,
};
