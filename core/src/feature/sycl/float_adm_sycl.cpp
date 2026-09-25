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
#include "feature/adm_score.h"
#include "feature/nonfinite_score.h"
#include "log.h"
#include "picture.h"
#include "sycl/common.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace
{

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

struct FadmDwtVertParams {
    const void *ref_raw;
    const void *dis_raw;
    const float *parent_ref_band;
    const float *parent_dis_band;
    float *dwt_tmp_ref;
    float *dwt_tmp_dis;
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

static inline float fadm_read_raw(const FadmDwtVertParams &p, const void *plane, int y, int x)
{
    y = fadm_mirror_host(y, (int)p.cur_h);
    if (x < 0)
        x = 0;
    if (std::cmp_greater_equal(x, p.cur_w))
        x = (int)p.cur_w - 1;
    if (p.bpc <= 8u)
        return (float)static_cast<const uint8_t *>(plane)[y * p.raw_stride + x] + p.pixel_offset;
    const uint16_t v = reinterpret_cast<const uint16_t *>(static_cast<const uint8_t *>(plane) +
                                                          (size_t)y * p.raw_stride)[x];
    return (float)v / p.scaler + p.pixel_offset;
}

static inline float fadm_read_parent(const FadmDwtVertParams &p, const float *band, int y, int x)
{
    y = fadm_mirror_host(y, (int)p.parent_h);
    if (x < 0)
        x = 0;
    if (std::cmp_greater_equal(x, p.parent_w))
        x = (int)p.parent_w - 1;
    return band[y * (int)p.parent_buf_stride + x];
}

template <int SCALE>
static inline void fadm_dwt_vert_item(const FadmDwtVertParams &p, sycl::nd_item<3> item)
{
    const int gx = (int)item.get_global_id(2);
    const int gy = (int)item.get_global_id(1);
    const int plane_is_dis = (int)item.get_global_id(0);
    if (std::cmp_greater_equal(gx, p.cur_w) || std::cmp_greater_equal(gy, p.half_h))
        return;

    const int row_start = 2 * gy - 1;
    float samples[4];
    for (int k = 0; k < 4; k++) {
        if constexpr (SCALE == 0) {
            const void *plane = (plane_is_dis == 0) ? p.ref_raw : p.dis_raw;
            samples[k] = fadm_read_raw(p, plane, row_start + k, gx);
        } else {
            const float *band = (plane_is_dis == 0) ? p.parent_ref_band : p.parent_dis_band;
            samples[k] = fadm_read_parent(p, band, row_start + k, gx);
        }
    }
    const float lo = FADM_LO0 * samples[0] + FADM_LO1 * samples[1] + FADM_LO2 * samples[2] +
                     FADM_LO3 * samples[3];
    const float hi = FADM_HI0 * samples[0] + FADM_HI1 * samples[1] + FADM_HI2 * samples[2] +
                     FADM_HI3 * samples[3];
    const int out_stride = (int)p.cur_w * 2;
    float *dst = (plane_is_dis == 0) ? p.dwt_tmp_ref : p.dwt_tmp_dis;
    dst[gy * out_stride + gx] = lo;
    dst[gy * out_stride + (int)p.cur_w + gx] = hi;
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
    const FadmDwtVertParams params = {
        .ref_raw = ref_raw,
        .dis_raw = dis_raw,
        .parent_ref_band = parent_ref_band,
        .parent_dis_band = parent_dis_band,
        .dwt_tmp_ref = dwt_tmp_ref,
        .dwt_tmp_dis = dwt_tmp_dis,
        .raw_stride = raw_stride_bytes,
        .parent_buf_stride = parent_buf_stride,
        .parent_w = parent_w,
        .parent_h = parent_h,
        .cur_w = cur_w,
        .cur_h = cur_h,
        .half_h = half_h,
        .bpc = bpc,
        .scaler = scaler,
        .pixel_offset = pixel_offset,
    };

    return q.submit([&](sycl::handler &cgh) {
        cgh.parallel_for(sycl::nd_range<3>(sycl::range<3>(2, global_y, global_x),
                                           sycl::range<3>(1, FADM_BY, FADM_BX)),
                         [=](sycl::nd_item<3> item) { fadm_dwt_vert_item<SCALE>(params, item); });
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
struct FadmDecoupleParams {
    const float *ref_band;
    const float *dis_band;
    float *csf_a;
    float *csf_f;
    unsigned half_w;
    unsigned half_h;
    unsigned buf_stride;
    float rfactor_h;
    float rfactor_v;
    float rfactor_d;
    float gain_limit;
};

struct FadmDecouplePixel {
    float original[FADM_NUM_BANDS];
    float transformed[FADM_NUM_BANDS];
    bool angle_flag;
};

static inline float fadm_band_value(const float *bands, int band, int y, int x, int slice,
                                    unsigned stride)
{
    return bands[band * slice + y * (int)stride + x];
}

static inline FadmDecouplePixel fadm_load_decouple_pixel(const FadmDecoupleParams &p, int y, int x)
{
    const int slice = (int)p.buf_stride * (int)p.half_h;
    const float oh = fadm_band_value(p.ref_band, 1, y, x, slice, p.buf_stride);
    const float ov = fadm_band_value(p.ref_band, 2, y, x, slice, p.buf_stride);
    const float od = fadm_band_value(p.ref_band, 3, y, x, slice, p.buf_stride);
    const float th = fadm_band_value(p.dis_band, 1, y, x, slice, p.buf_stride);
    const float tv = fadm_band_value(p.dis_band, 2, y, x, slice, p.buf_stride);
    const float td = fadm_band_value(p.dis_band, 3, y, x, slice, p.buf_stride);
    const float ot_dp = (oh * th) + (ov * tv);
    const float o_mag = (oh * oh) + (ov * ov);
    const float t_mag = (th * th) + (tv * tv);
    const float lhs = ot_dp * ot_dp;
    const float rhs = FADM_COS_1DEG_SQ * (o_mag * t_mag);
    return {.original = {oh, ov, od},
            .transformed = {th, tv, td},
            .angle_flag = (ot_dp >= 0.0f) && (lhs >= rhs)};
}

static inline float fadm_restore(float original, float transformed, bool angle_flag,
                                 float gain_limit)
{
    float k = transformed / (original + FADM_EPS);
    k = sycl::fmax(0.0f, sycl::fmin(k, 1.0f));
    float restored = k * original;
    if (angle_flag && restored > 0.0f) {
        restored = sycl::fmin(restored * gain_limit, transformed);
    } else if (angle_flag && restored < 0.0f) {
        restored = sycl::fmax(restored * gain_limit, transformed);
    }
    return restored;
}

template <bool USE_ANOMALY>
static inline void fadm_decouple_item(const FadmDecoupleParams &p, sycl::nd_item<2> item)
{
    const int gx = (int)item.get_global_id(1);
    const int gy = (int)item.get_global_id(0);
    if (std::cmp_greater_equal(gx, p.half_w) || std::cmp_greater_equal(gy, p.half_h))
        return;
    const int slice = (int)p.buf_stride * (int)p.half_h;
    const FadmDecouplePixel pixel = fadm_load_decouple_pixel(p, gy, gx);
    float const rfactor[3] = {p.rfactor_h, p.rfactor_v, p.rfactor_d};
    for (int b = 0; b < FADM_NUM_BANDS; b++) {
        const float restored =
            fadm_restore(pixel.original[b], pixel.transformed[b], pixel.angle_flag, p.gain_limit);
        const float value = USE_ANOMALY ? pixel.transformed[b] - restored : restored;
        const float csf_a_val = rfactor[b] * value;
        p.csf_a[b * slice + gy * (int)p.buf_stride + gx] = csf_a_val;
        p.csf_f[b * slice + gy * (int)p.buf_stride + gx] = FADM_ONE_BY_30 * sycl::fabs(csf_a_val);
    }
}

template <bool USE_ANOMALY>
static sycl::event submit_decouple(sycl::queue &q, const FadmDecoupleParams &params)
{
    const size_t global_x = (size_t)((params.half_w + FADM_BX - 1u) / FADM_BX) * FADM_BX;
    const size_t global_y = (size_t)((params.half_h + FADM_BY - 1u) / FADM_BY) * FADM_BY;
    return q.submit([&](sycl::handler &cgh) {
        cgh.parallel_for(
            sycl::nd_range<2>(sycl::range<2>(global_y, global_x), sycl::range<2>(FADM_BY, FADM_BX)),
            [=](sycl::nd_item<2> item) { fadm_decouple_item<USE_ANOMALY>(params, item); });
    });
}

static sycl::event launch_decouple_csf(sycl::queue &q, const float *ref_band, const float *dis_band,
                                       float *csf_a, float *csf_f, unsigned half_w, unsigned half_h,
                                       unsigned buf_stride, float rfactor_h, float rfactor_v,
                                       float rfactor_d, float gain_limit)
{
    const FadmDecoupleParams params = {.ref_band = ref_band,
                                       .dis_band = dis_band,
                                       .csf_a = csf_a,
                                       .csf_f = csf_f,
                                       .half_w = half_w,
                                       .half_h = half_h,
                                       .buf_stride = buf_stride,
                                       .rfactor_h = rfactor_h,
                                       .rfactor_v = rfactor_v,
                                       .rfactor_d = rfactor_d,
                                       .gain_limit = gain_limit};
    return submit_decouple<true>(q, params);
}

/* ------------------------------------------------------------------ */
/* Stage 2b — CSF on decouple_r (writes csf_a_aim + csf_f_aim).       */
/* ADR-0574: mirrors stage 2 but computes CSF of r_val = k*o rather   */
/* than the anomaly a_val = t - r.                                     */
/* ------------------------------------------------------------------ */
static sycl::event launch_csf_r(sycl::queue &q, const float *ref_band, const float *dis_band,
                                float *csf_a_aim, float *csf_f_aim, unsigned half_w,
                                unsigned half_h, unsigned buf_stride, float rfactor_h,
                                float rfactor_v, float rfactor_d, float gain_limit)
{
    const FadmDecoupleParams params = {.ref_band = ref_band,
                                       .dis_band = dis_band,
                                       .csf_a = csf_a_aim,
                                       .csf_f = csf_f_aim,
                                       .half_w = half_w,
                                       .half_h = half_h,
                                       .buf_stride = buf_stride,
                                       .rfactor_h = rfactor_h,
                                       .rfactor_v = rfactor_v,
                                       .rfactor_d = rfactor_d,
                                       .gain_limit = gain_limit};
    return submit_decouple<false>(q, params);
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

struct FadmCmParams {
    const float *ref_band;
    const float *dis_band;
    const float *csf_a;
    const float *csf_f;
    float *accum;
    unsigned half_w;
    unsigned half_h;
    unsigned buf_stride;
    unsigned active_h;
    int active_left;
    int active_top;
    int active_right;
    float rfactor_h;
    float rfactor_v;
    float rfactor_d;
    float gain_limit;
    float p_norm;
};

struct FadmCsfCmTerms {
    float csf;
    float cm;
};

static inline float fadm_cm_rfactor(const FadmCmParams &p, unsigned band)
{
    return (band == 0u) ? p.rfactor_h : (band == 1u) ? p.rfactor_v : p.rfactor_d;
}

static inline FadmDecouplePixel fadm_load_cm_pixel(const FadmCmParams &p, int y, int x)
{
    const int slice = (int)p.buf_stride * (int)p.half_h;
    const float oh = fadm_band_value(p.ref_band, 1, y, x, slice, p.buf_stride);
    const float ov = fadm_band_value(p.ref_band, 2, y, x, slice, p.buf_stride);
    const float od = fadm_band_value(p.ref_band, 3, y, x, slice, p.buf_stride);
    const float th = fadm_band_value(p.dis_band, 1, y, x, slice, p.buf_stride);
    const float tv = fadm_band_value(p.dis_band, 2, y, x, slice, p.buf_stride);
    const float td = fadm_band_value(p.dis_band, 3, y, x, slice, p.buf_stride);
    const float ot_dp = (oh * th) + (ov * tv);
    const float o_mag = (oh * oh) + (ov * ov);
    const float t_mag = (th * th) + (tv * tv);
    const float lhs = ot_dp * ot_dp;
    const float rhs = FADM_COS_1DEG_SQ * (o_mag * t_mag);
    return {.original = {oh, ov, od},
            .transformed = {th, tv, td},
            .angle_flag = (ot_dp >= 0.0f) && (lhs >= rhs)};
}

static inline float fadm_read_csf_f(const FadmCmParams &p, int band, int y, int x)
{
    if (x < 0)
        x = -x;
    if (std::cmp_greater_equal(x, p.half_w))
        x = (int)p.half_w - 1;
    if (y < 0)
        y = -y;
    if (std::cmp_greater_equal(y, p.half_h))
        y = (int)p.half_h - 1;
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    if (std::cmp_greater_equal(x, p.half_w))
        x = (int)p.half_w - 1;
    if (std::cmp_greater_equal(y, p.half_h))
        y = (int)p.half_h - 1;
    const int slice = (int)p.buf_stride * (int)p.half_h;
    return p.csf_f[band * slice + y * (int)p.buf_stride + x];
}

static inline float fadm_read_csf_a(const FadmCmParams &p, int band, int y, int x)
{
    if (x < 0)
        x = 0;
    if (std::cmp_greater_equal(x, p.half_w))
        x = (int)p.half_w - 1;
    if (y < 0)
        y = 0;
    if (std::cmp_greater_equal(y, p.half_h))
        y = (int)p.half_h - 1;
    const int slice = (int)p.buf_stride * (int)p.half_h;
    return p.csf_a[band * slice + y * (int)p.buf_stride + x];
}

static inline float fadm_cm_threshold(const FadmCmParams &p, int row, int col)
{
    float threshold = 0.0f;
    for (int band = 0; band < FADM_NUM_BANDS; band++) {
        for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                if (dx == 0 && dy == 0)
                    continue;
                threshold += fadm_read_csf_f(p, band, row + dy, col + dx);
            }
        }
    }
    const float own_h = fadm_read_csf_a(p, 0, row, col);
    const float own_v = fadm_read_csf_a(p, 1, row, col);
    const float own_d = fadm_read_csf_a(p, 2, row, col);
    threshold += FADM_ONE_BY_15 * sycl::fabs(own_h);
    threshold += FADM_ONE_BY_15 * sycl::fabs(own_v);
    threshold += FADM_ONE_BY_15 * sycl::fabs(own_d);
    return threshold;
}

static inline float fadm_aim_cm_term(const FadmCmParams &p, unsigned band, int row, int col,
                                     float rfactor)
{
    const FadmDecouplePixel pixel = fadm_load_cm_pixel(p, row, col);
    const float restored =
        fadm_restore(pixel.original[band], pixel.transformed[band], pixel.angle_flag, p.gain_limit);
    const float anomaly = pixel.transformed[band] - restored;
    const float threshold = fadm_cm_threshold(p, row, col);
    const float x_val = rfactor * anomaly;
    float xa = sycl::fabs(x_val) - threshold;
    if (xa < 0.0f)
        xa = 0.0f;
    return fadm_pnorm_term(xa, p.p_norm);
}

static inline FadmCsfCmTerms fadm_csf_cm_terms(const FadmCmParams &p, unsigned band, int row,
                                               int col, float rfactor)
{
    const int slice = (int)p.buf_stride * (int)p.half_h;
    const float src_ref = fadm_band_value(p.ref_band, (int)band + 1, row, col, slice, p.buf_stride);
    const float csf_o = sycl::fabs(rfactor * src_ref);
    const float csf = fadm_pnorm_term(csf_o, p.p_norm);
    const FadmDecouplePixel pixel = fadm_load_cm_pixel(p, row, col);
    const float restored =
        fadm_restore(pixel.original[band], pixel.transformed[band], pixel.angle_flag, p.gain_limit);
    const float threshold = fadm_cm_threshold(p, row, col);
    const float x_val = rfactor * restored;
    float xa = sycl::fabs(x_val) - threshold;
    if (xa < 0.0f)
        xa = 0.0f;
    return {.csf = csf, .cm = fadm_pnorm_term(xa, p.p_norm)};
}

static inline void fadm_reduce_aim(sycl::nd_item<1> item, float local_aim,
                                   const sycl::local_accessor<float, 1> &subgroup_aim,
                                   const FadmCmParams &p, unsigned workgroup, unsigned band)
{
    const sycl::sub_group sg = item.get_sub_group();
    const float workgroup_aim = sycl::reduce_over_group(sg, local_aim, sycl::plus<float>{});
    const uint32_t subgroup = sg.get_group_linear_id();
    const uint32_t lane = sg.get_local_linear_id();
    const uint32_t subgroup_count = sg.get_group_linear_range();
    if (lane == 0)
        subgroup_aim[subgroup] = workgroup_aim;
    item.barrier(sycl::access::fence_space::local_space);
    if (item.get_local_id(0) == 0) {
        float total_aim = 0.0f;
        for (uint32_t i = 0; i < subgroup_count; i++)
            total_aim += subgroup_aim[i];
        const unsigned slot_base = workgroup * FADM_ACCUM_SLOTS;
        p.accum[slot_base + 6u + band] = total_aim;
    }
}

static inline void fadm_reduce_csf_cm(sycl::nd_item<1> item, const FadmCsfCmTerms &local,
                                      const sycl::local_accessor<float, 1> &subgroup_csf,
                                      const sycl::local_accessor<float, 1> &subgroup_cm,
                                      const FadmCmParams &p, unsigned workgroup, unsigned band)
{
    const sycl::sub_group sg = item.get_sub_group();
    const float workgroup_csf = sycl::reduce_over_group(sg, local.csf, sycl::plus<float>{});
    const float workgroup_cm = sycl::reduce_over_group(sg, local.cm, sycl::plus<float>{});
    const uint32_t subgroup = sg.get_group_linear_id();
    const uint32_t lane = sg.get_local_linear_id();
    const uint32_t subgroup_count = sg.get_group_linear_range();
    if (lane == 0) {
        subgroup_csf[subgroup] = workgroup_csf;
        subgroup_cm[subgroup] = workgroup_cm;
    }
    item.barrier(sycl::access::fence_space::local_space);
    if (item.get_local_id(0) == 0) {
        float total_csf = 0.0f;
        float total_cm = 0.0f;
        for (uint32_t i = 0; i < subgroup_count; i++) {
            total_csf += subgroup_csf[i];
            total_cm += subgroup_cm[i];
        }
        const unsigned slot_base = workgroup * FADM_ACCUM_SLOTS;
        p.accum[slot_base + band] = total_csf;
        p.accum[slot_base + 3u + band] = total_cm;
    }
}

static inline void fadm_aim_cm_item(sycl::nd_item<1> item,
                                    const sycl::local_accessor<float, 1> &subgroup_aim,
                                    const FadmCmParams &p)
{
    const unsigned workgroup = (unsigned)item.get_group(0);
    const unsigned local_id = (unsigned)item.get_local_id(0);
    const unsigned band = workgroup / p.active_h;
    const unsigned row_index = workgroup - band * p.active_h;
    const int row = p.active_top + (int)row_index;
    const float rfactor = fadm_cm_rfactor(p, band);
    const int workgroup_size = FADM_BX * FADM_BY;
    float local_aim = 0.0f;
    for (int col = p.active_left + (int)local_id; col < p.active_right; col += workgroup_size)
        local_aim += fadm_aim_cm_term(p, band, row, col, rfactor);
    fadm_reduce_aim(item, local_aim, subgroup_aim, p, workgroup, band);
}

static inline void fadm_csf_cm_item(sycl::nd_item<1> item,
                                    const sycl::local_accessor<float, 1> &subgroup_csf,
                                    const sycl::local_accessor<float, 1> &subgroup_cm,
                                    const FadmCmParams &p)
{
    const unsigned workgroup = (unsigned)item.get_group(0);
    const unsigned local_id = (unsigned)item.get_local_id(0);
    const unsigned band = workgroup / p.active_h;
    const unsigned row_index = workgroup - band * p.active_h;
    const int row = p.active_top + (int)row_index;
    const float rfactor = fadm_cm_rfactor(p, band);
    const int workgroup_size = FADM_BX * FADM_BY;
    FadmCsfCmTerms local = {};
    for (int col = p.active_left + (int)local_id; col < p.active_right; col += workgroup_size) {
        const FadmCsfCmTerms terms = fadm_csf_cm_terms(p, band, row, col, rfactor);
        local.csf += terms.csf;
        local.cm += terms.cm;
    }
    fadm_reduce_csf_cm(item, local, subgroup_csf, subgroup_cm, p, workgroup, band);
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
    const size_t workgroup_size = (size_t)FADM_BX * FADM_BY;
    const size_t global_x = num_groups * workgroup_size;
    const FadmCmParams params = {.ref_band = ref_band,
                                 .dis_band = dis_band,
                                 .csf_a = csf_a_aim,
                                 .csf_f = csf_f_aim,
                                 .accum = accum_out,
                                 .half_w = half_w,
                                 .half_h = half_h,
                                 .buf_stride = buf_stride,
                                 .active_h = (unsigned)active_h,
                                 .active_left = active_left,
                                 .active_top = active_top,
                                 .active_right = active_right,
                                 .rfactor_h = rfactor_h,
                                 .rfactor_v = rfactor_v,
                                 .rfactor_d = rfactor_d,
                                 .gain_limit = gain_limit,
                                 .p_norm = p_norm};

    return q.submit([&](sycl::handler &cgh) {
        sycl::local_accessor<float, 1> const subgroup_aim(sycl::range<1>(workgroup_size / 32), cgh);
        cgh.parallel_for(
            sycl::nd_range<1>(sycl::range<1>(global_x), sycl::range<1>(workgroup_size)),
            [=](sycl::nd_item<1> item)
                VMAF_SYCL_REQD_SG_SIZE(32) { fadm_aim_cm_item(item, subgroup_aim, params); });
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
    const size_t workgroup_size = (size_t)FADM_BX * FADM_BY;
    const size_t global_x = num_groups * workgroup_size;
    const FadmCmParams params = {.ref_band = ref_band,
                                 .dis_band = dis_band,
                                 .csf_a = csf_a,
                                 .csf_f = csf_f,
                                 .accum = accum_out,
                                 .half_w = half_w,
                                 .half_h = half_h,
                                 .buf_stride = buf_stride,
                                 .active_h = (unsigned)active_h,
                                 .active_left = active_left,
                                 .active_top = active_top,
                                 .active_right = active_right,
                                 .rfactor_h = rfactor_h,
                                 .rfactor_v = rfactor_v,
                                 .rfactor_d = rfactor_d,
                                 .gain_limit = gain_limit,
                                 .p_norm = p_norm};

    return q.submit([&](sycl::handler &cgh) {
        sycl::local_accessor<float, 1> const subgroup_csf(sycl::range<1>(workgroup_size / 32), cgh);
        sycl::local_accessor<float, 1> const subgroup_cm(sycl::range<1>(workgroup_size / 32), cgh);
        cgh.parallel_for(
            sycl::nd_range<1>(sycl::range<1>(global_x), sycl::range<1>(workgroup_size)),
            [=](sycl::nd_item<1> item) VMAF_SYCL_REQD_SG_SIZE(32) {
                fadm_csf_cm_item(item, subgroup_csf, subgroup_cm, params);
            });
    });
}

static void fadm_configure_dimensions(FloatAdmStateSycl *s, unsigned width, unsigned height)
{
    unsigned current_width = width;
    unsigned current_height = height;
    for (int scale = 0; scale < FADM_NUM_SCALES; scale++) {
        s->scale_w[scale] = current_width;
        s->scale_h[scale] = current_height;
        s->scale_half_w[scale] = (current_width + 1u) / 2u;
        s->scale_half_h[scale] = (current_height + 1u) / 2u;
        current_width = s->scale_half_w[scale];
        current_height = s->scale_half_h[scale];
    }
    s->buf_stride = (s->scale_half_w[0] + 3u) & ~3u;
}

static void fadm_configure_rfactors(FloatAdmStateSycl *s)
{
    for (int scale = 0; scale < FADM_NUM_SCALES; scale++) {
        const float f1 =
            fadm_dwt_quant_step(scale, 1, s->adm_norm_view_dist, s->adm_ref_display_height);
        const float f2 =
            fadm_dwt_quant_step(scale, 2, s->adm_norm_view_dist, s->adm_ref_display_height);
        /* ADR-1214: Watson-97 mode matches adm_csf_rfactor_s and does not
         * consult the Barten-only adm_csf_scale options. */
        s->rfactor[scale * 3 + 0] = 1.0f / f1;
        s->rfactor[scale * 3 + 1] = 1.0f / f1;
        s->rfactor[scale * 3 + 2] = 1.0f / f2;
    }
}

static void fadm_allocate_raw_and_dwt(FloatAdmStateSycl *s)
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
}

static void fadm_allocate_bands_and_csf(FloatAdmStateSycl *s)
{
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

static bool fadm_allocations_complete(const FloatAdmStateSycl *s)
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

struct FadmScaleBounds {
    int left;
    int top;
    int right;
    int bottom;
};

static FadmScaleBounds fadm_scale_bounds(unsigned half_width, unsigned half_height)
{
    int top = (int)((double)half_height * FADM_BORDER_FACTOR - 0.5);
    int left = (int)((double)half_width * FADM_BORDER_FACTOR - 0.5);
    if (top < 0)
        top = 0;
    if (left < 0)
        left = 0;
    return {.left = left,
            .top = top,
            .right = (int)half_width - left,
            .bottom = (int)half_height - top};
}

static unsigned fadm_upload_planes(sycl::queue &q, const FloatAdmStateSycl *s,
                                   const VmafPicture *ref_pic, const VmafPicture *dist_pic)
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
    return (unsigned)(s->width * bytes_per_pixel);
}

static void fadm_reset_accumulators(sycl::queue &q, const FloatAdmStateSycl *s)
{
    for (int scale = 0; scale < FADM_NUM_SCALES; scale++) {
        q.memset(s->d_accum[scale], 0,
                 (size_t)s->wg_count[scale] * FADM_ACCUM_SLOTS * sizeof(float));
    }
}

static float fadm_pixel_scaler(unsigned bits_per_component)
{
    if (bits_per_component == 10u)
        return 4.0f;
    if (bits_per_component == 12u)
        return 16.0f;
    if (bits_per_component == 16u)
        return 256.0f;
    return 1.0f;
}

template <int SCALE>
static void fadm_launch_dwt_case(sycl::queue &q, FloatAdmStateSycl *s, unsigned raw_stride,
                                 float scaler)
{
    constexpr int parent_band_by_scale[FADM_NUM_SCALES] = {0, 0, 1, 2};
    const unsigned current_width = s->scale_w[SCALE];
    const unsigned current_height = s->scale_h[SCALE];
    const unsigned half_height = s->scale_half_h[SCALE];
    const unsigned parent_width = SCALE > 0 ? s->scale_w[SCALE] : 0u;
    const unsigned parent_height = SCALE > 0 ? s->scale_h[SCALE] : 0u;
    const float *const parent_ref =
        SCALE > 0 ? s->d_ref_band[parent_band_by_scale[SCALE]] : nullptr;
    const float *const parent_dis =
        SCALE > 0 ? s->d_dis_band[parent_band_by_scale[SCALE]] : nullptr;
    const float pixel_offset = -128.0f;
    launch_dwt_vert<SCALE>(q, s->d_ref_raw, s->d_dis_raw, raw_stride, parent_ref, parent_dis,
                           s->buf_stride, parent_width, parent_height, s->d_dwt_tmp_ref,
                           s->d_dwt_tmp_dis, current_width, current_height, half_height, s->bpc,
                           scaler, pixel_offset);
}

static void fadm_launch_dwt_scale(sycl::queue &q, FloatAdmStateSycl *s, int scale,
                                  unsigned raw_stride, float scaler)
{
    if (scale == 0) {
        fadm_launch_dwt_case<0>(q, s, raw_stride, scaler);
    } else if (scale == 1) {
        fadm_launch_dwt_case<1>(q, s, raw_stride, scaler);
    } else if (scale == 2) {
        fadm_launch_dwt_case<2>(q, s, raw_stride, scaler);
    } else {
        fadm_launch_dwt_case<3>(q, s, raw_stride, scaler);
    }
}

static void fadm_launch_scale(sycl::queue &q, FloatAdmStateSycl *s, int scale, unsigned raw_stride,
                              float scaler)
{
    const unsigned current_width = s->scale_w[scale];
    const unsigned half_width = s->scale_half_w[scale];
    const unsigned half_height = s->scale_half_h[scale];
    const FadmScaleBounds bounds = fadm_scale_bounds(half_width, half_height);
    fadm_launch_dwt_scale(q, s, scale, raw_stride, scaler);
    launch_dwt_hori(q, s->d_dwt_tmp_ref, s->d_dwt_tmp_dis, s->d_ref_band[scale],
                    s->d_dis_band[scale], current_width, half_width, half_height, s->buf_stride);
    launch_decouple_csf(q, s->d_ref_band[scale], s->d_dis_band[scale], s->d_csf_a, s->d_csf_f,
                        half_width, half_height, s->buf_stride, s->rfactor[scale * 3 + 0],
                        s->rfactor[scale * 3 + 1], s->rfactor[scale * 3 + 2],
                        (float)s->adm_enhn_gain_limit);
    launch_csf_cm(q, s->d_ref_band[scale], s->d_dis_band[scale], s->d_csf_a, s->d_csf_f,
                  s->d_accum[scale], half_width, half_height, s->buf_stride, bounds.left,
                  bounds.top, bounds.right, bounds.bottom, s->rfactor[scale * 3 + 0],
                  s->rfactor[scale * 3 + 1], s->rfactor[scale * 3 + 2],
                  (float)s->adm_enhn_gain_limit, (float)s->adm_p_norm);
    launch_csf_r(q, s->d_ref_band[scale], s->d_dis_band[scale], s->d_csf_a_aim, s->d_csf_f_aim,
                 half_width, half_height, s->buf_stride, s->rfactor[scale * 3 + 0],
                 s->rfactor[scale * 3 + 1], s->rfactor[scale * 3 + 2],
                 (float)s->adm_enhn_gain_limit);
    launch_aim_cm(q, s->d_ref_band[scale], s->d_dis_band[scale], s->d_csf_a_aim, s->d_csf_f_aim,
                  s->d_accum[scale], half_width, half_height, s->buf_stride, bounds.left,
                  bounds.top, bounds.right, bounds.bottom, s->rfactor[scale * 3 + 0],
                  s->rfactor[scale * 3 + 1], s->rfactor[scale * 3 + 2],
                  (float)s->adm_enhn_gain_limit, (float)s->adm_p_norm);
}

static void fadm_download_accumulators(sycl::queue &q, const FloatAdmStateSycl *s)
{
    for (int scale = 0; scale < FADM_NUM_SCALES; scale++) {
        q.memcpy(s->h_accum[scale], s->d_accum[scale],
                 (size_t)s->wg_count[scale] * FADM_ACCUM_SLOTS * sizeof(float));
    }
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
    double scales[FADM_NUM_SCALES * 2];
};

struct FadmFinalScores {
    double adm;
    double aim;
    double adm3;
    double scales[FADM_NUM_SCALES];
};

static FadmTotals fadm_accumulate_totals(const FloatAdmStateSycl *s)
{
    FadmTotals totals = {};
    for (int scale = 0; scale < FADM_NUM_SCALES; scale++) {
        const float *slots = s->h_accum[scale];
        const unsigned workgroup_count = s->wg_count[scale];
        for (unsigned workgroup = 0u; workgroup < workgroup_count; workgroup++) {
            const float *values = slots + (size_t)workgroup * FADM_ACCUM_SLOTS;
            for (int band = 0; band < FADM_NUM_BANDS; band++) {
                totals.csf[scale][band] += (double)values[band];
                totals.cm[scale][band] += (double)values[3 + band];
                totals.aim_cm[scale][band] += (double)values[6 + band];
            }
        }
    }
    return totals;
}

static void fadm_pool_scale(const FloatAdmStateSycl *s, const FadmTotals &totals, int scale,
                            FadmPooledScores *pooled)
{
    const int half_width = (int)s->scale_half_w[scale];
    const int half_height = (int)s->scale_half_h[scale];
    const FadmScaleBounds bounds = fadm_scale_bounds((unsigned)half_width, (unsigned)half_height);
    const float inverse_p = 1.0f / (float)s->adm_p_norm;
    const float area_root =
        std::pow((float)((bounds.bottom - bounds.top) * (bounds.right - bounds.left)) *
                     (float)s->adm_noise_weight,
                 inverse_p);
    float scale_numerator = 0.0f;
    float scale_denominator = 0.0f;
    for (int band = 0; band < FADM_NUM_BANDS; band++) {
        scale_numerator += std::pow((float)totals.cm[scale][band], inverse_p) + area_root;
        scale_denominator += std::pow((float)totals.csf[scale][band], inverse_p) + area_root;
    }
    pooled->scales[2 * scale + 0] = scale_numerator;
    pooled->scales[2 * scale + 1] = scale_denominator;
    pooled->numerator += scale_numerator;
    pooled->denominator += scale_denominator;
    float aim_scale_numerator = 0.0f;
    for (int band = 0; band < FADM_NUM_BANDS; band++)
        aim_scale_numerator += std::pow((float)totals.aim_cm[scale][band], inverse_p);
    pooled->aim_denominator += scale_denominator;
    pooled->aim_numerator += aim_scale_numerator;
}

static FadmPooledScores fadm_pool_scores(const FloatAdmStateSycl *s, const FadmTotals &totals)
{
    FadmPooledScores pooled = {};
    for (int scale = 0; scale < FADM_NUM_SCALES; scale++)
        fadm_pool_scale(s, totals, scale, &pooled);
    return pooled;
}

static int fadm_finalize_scores(const FloatAdmStateSycl *s, unsigned index,
                                FadmPooledScores *pooled, FadmFinalScores *final)
{
    const int width = (int)s->scale_w[0];
    const int height = (int)s->scale_h[0];
    const double floor = 1e-2 * (double)(width * height) / (1920.0 * 1080.0);
    int err =
        vmaf_adm_floor_pair_named("float_adm_sycl", index, pooled->numerator, pooled->denominator,
                                  floor, &pooled->numerator, &pooled->denominator);
    if (err)
        return err;
    err = vmaf_adm_finalize_scores_named("float_adm_sycl", index, pooled->numerator,
                                         pooled->denominator, pooled->aim_numerator,
                                         pooled->aim_denominator, &final->adm, &final->aim);
    if (err)
        return err;
    err =
        vmaf_adm3_score_named("float_adm_sycl", index, final->adm, final->aim, s->adm_adm3_apply_hm,
                              s->adm_dlm_weight, s->adm_min_val, &final->adm3);
    if (err)
        return err;
    return vmaf_adm_scale_ratios_named("float_adm_sycl", index, pooled->scales, FADM_NUM_SCALES,
                                       final->scales);
}

static size_t fadm_make_named_scores(const FloatAdmStateSycl *s, const FadmPooledScores &pooled,
                                     const FadmFinalScores &final, VmafNamedScore *values)
{
    values[0] = {.name = "VMAF_feature_adm2_score", .value = final.adm};
    values[1] = {.name = "VMAF_feature_adm_scale0_score", .value = final.scales[0]};
    values[2] = {.name = "VMAF_feature_adm_scale1_score", .value = final.scales[1]};
    values[3] = {.name = "VMAF_feature_adm_scale2_score", .value = final.scales[2]};
    values[4] = {.name = "VMAF_feature_adm_scale3_score", .value = final.scales[3]};
    values[5] = {.name = "VMAF_feature_aim_score", .value = final.aim};
    values[6] = {.name = "VMAF_feature_adm3_score", .value = final.adm3};
    size_t count = 7u;
    if (!s->debug)
        return count;
    static const char *const debug_names[8] = {"adm_num_scale0", "adm_den_scale0", "adm_num_scale1",
                                               "adm_den_scale1", "adm_num_scale2", "adm_den_scale2",
                                               "adm_num_scale3", "adm_den_scale3"};
    values[count++] = VmafNamedScore{.name = "adm", .value = final.adm};
    values[count++] = VmafNamedScore{.name = "adm_num", .value = pooled.numerator};
    values[count++] = VmafNamedScore{.name = "adm_den", .value = pooled.denominator};
    for (size_t i = 0u; i < 8u; ++i)
        values[count++] = VmafNamedScore{.name = debug_names[i], .value = pooled.scales[i]};
    return count;
}

} /* anonymous namespace */

extern "C" {

static const VmafOption options_float_adm_sycl[] = {
    {.name = "debug",
     .help = "debug mode",
     .offset = offsetof(FloatAdmStateSycl, debug),
     .type = VMAF_OPT_TYPE_BOOL,
     .default_val = {.b = false}},
    {.name = "adm_enhn_gain_limit",
     .help = "enhancement gain (>=1.0)",
     .alias = "egl",
     .offset = offsetof(FloatAdmStateSycl, adm_enhn_gain_limit),
     .type = VMAF_OPT_TYPE_DOUBLE,
     .default_val = {.d = 100.0},
     .min = 1.0,
     .max = 100.0,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_norm_view_dist",
     .help = "normalized viewing distance",
     .alias = "nvd",
     .offset = offsetof(FloatAdmStateSycl, adm_norm_view_dist),
     .type = VMAF_OPT_TYPE_DOUBLE,
     .default_val = {.d = 3.0},
     .min = 0.75,
     .max = 24.0,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_ref_display_height",
     .help = "reference display height in pixels",
     .alias = "rdf",
     .offset = offsetof(FloatAdmStateSycl, adm_ref_display_height),
     .type = VMAF_OPT_TYPE_INT,
     .default_val = {.i = 1080},
     .min = 1,
     .max = 4320,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_csf_mode",
     .help = "contrast sensitivity function (mode 0 only on SYCL v1)",
     .alias = "csf",
     .offset = offsetof(FloatAdmStateSycl, adm_csf_mode),
     .type = VMAF_OPT_TYPE_INT,
     .default_val = {.i = 0},
     .min = 0,
     .max = 9,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_csf_scale",
     .help = "CSF band-scale multiplier for h/v bands (default 1.0 = no scaling)",
     .alias = "scf",
     .offset = offsetof(FloatAdmStateSycl, adm_csf_scale),
     .type = VMAF_OPT_TYPE_DOUBLE,
     .default_val = {.d = DEFAULT_ADM_CSF_SCALE},
     .min = 0.0,
     .max = 100.0,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_csf_diag_scale",
     .help = "CSF band-scale multiplier for diagonal bands (default 1.0 = no scaling)",
     .alias = "scfd",
     .offset = offsetof(FloatAdmStateSycl, adm_csf_diag_scale),
     .type = VMAF_OPT_TYPE_DOUBLE,
     .default_val = {.d = DEFAULT_ADM_CSF_DIAG_SCALE},
     .min = 0.0,
     .max = 100.0,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_noise_weight",
     .help = "noise floor weight for CM numerator (default 0.03125 = 1/32)",
     .alias = "nw",
     .offset = offsetof(FloatAdmStateSycl, adm_noise_weight),
     .type = VMAF_OPT_TYPE_DOUBLE,
     .default_val = {.d = DEFAULT_ADM_NOISE_WEIGHT},
     .min = 0.0,
     .max = 100.0,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    /* ADR-0574: AIM / ADM3 options — mirrors CUDA twin. */
    {.name = "adm_adm3_apply_hm",
     .help = "apply harmonic mean for adm3 score (false = linear blend)",
     .alias = "aah",
     .offset = offsetof(FloatAdmStateSycl, adm_adm3_apply_hm),
     .type = VMAF_OPT_TYPE_BOOL,
     .default_val = {.b = false},
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_p_norm",
     .help = "p-norm exponent for AIM/ADM3 score (default 3.0)",
     .alias = "apn",
     .offset = offsetof(FloatAdmStateSycl, adm_p_norm),
     .type = VMAF_OPT_TYPE_DOUBLE,
     .default_val = {.d = 3.0},
     .min = 1.0,
     .max = 20.0,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_dlm_weight",
     .help = "DLM weight for linear-blend adm3 score (default 0.5)",
     .alias = "dlmw",
     .offset = offsetof(FloatAdmStateSycl, adm_dlm_weight),
     .type = VMAF_OPT_TYPE_DOUBLE,
     .default_val = {.d = 0.5},
     .min = 0.0,
     .max = 1.0,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_min_val",
     .help = "minimum clamp for adm3 score (default 0.0)",
     .alias = "min",
     .offset = offsetof(FloatAdmStateSycl, adm_min_val),
     .type = VMAF_OPT_TYPE_DOUBLE,
     .default_val = {.d = DEFAULT_ADM_MIN_VAL},
     .min = 0.0,
     .max = 1.0,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = nullptr}};

// NOLINTBEGIN(misc-use-anonymous-namespace, misc-use-internal-linkage) — ADR-0141 §2 load-bearing invariant: the
// `init_fex_sycl` / `submit_fex_sycl` / `collect_fex_sycl` / `close_fex_sycl`
// entry points use C-style `static` rather than an anonymous namespace because
// their addresses are stored in the `extern "C" VmafFeatureExtractor` struct at
// the bottom of this file, which the C ABI consumes through the
// function-pointer types in `feature_extractor.h`. A namespace cannot appear
// inside this linkage specification at all. Same band, same reason, as
// integer_motion_sycl.cpp and integer_adm_sycl.cpp. Per CLAUDE.md §12 r12 these
// are load-bearing invariants of the SYCL <-> libvmaf C-API ABI.

static int close_fex_sycl(VmafFeatureExtractor *fex);

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
    fadm_configure_dimensions(s, w, h);
    fadm_configure_rfactors(s);

    if (!fex->sycl_state)
        return -EINVAL;
    s->sycl_state = fex->sycl_state;
    fadm_allocate_raw_and_dwt(s);
    fadm_allocate_bands_and_csf(s);
    fadm_allocate_accumulators(s);
    if (!fadm_allocations_complete(s)) {
        (void)close_fex_sycl(fex);
        return -ENOMEM;
    }

    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (!s->feature_name_dict) {
        (void)close_fex_sycl(fex);
        return -ENOMEM;
    }
    return 0;
}

static int submit_fex_sycl(VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                           VmafPicture *dist_pic, VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic_90;
    (void)dist_pic_90;
    auto *s = static_cast<FloatAdmStateSycl *>(fex->priv);
    auto *qptr = static_cast<sycl::queue *>(vmaf_sycl_get_queue_ptr(s->sycl_state));
    if (!qptr)
        return -EINVAL;
    sycl::queue &q = *qptr;
    const unsigned raw_stride = fadm_upload_planes(q, s, ref_pic, dist_pic);
    fadm_reset_accumulators(q, s);
    const float scaler = fadm_pixel_scaler(s->bpc);

    for (int scale = 0; scale < FADM_NUM_SCALES; scale++) {
        fadm_launch_scale(q, s, scale, raw_stride, scaler);
    }

    fadm_download_accumulators(q, s);

    s->pending_index = index;
    s->has_pending = true;
    return 0;
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
    FadmFinalScores final = {};
    const int err = fadm_finalize_scores(s, index, &pooled, &final);
    if (err)
        return err;
    VmafNamedScore values[18] = {};
    const size_t value_count = fadm_make_named_scores(s, pooled, final, values);
    return vmaf_feature_emit_finite_scores(fc, s->feature_name_dict, "float_adm_sycl", values,
                                           value_count, index);
}

// NOLINTNEXTLINE(readability-function-size): SYCL kernel-launch / lifecycle entry — body is dominated by accessor declarations + a single `parallel_for` lambda. Splitting either inlines via macro (no readability win) or introduces a free function the compiler cannot inline back into the device kernel. Keeping it large is the pattern shared across every SYCL TU in this fork (ADR-0141 §2 load-bearing invariant; T7-5 sweep closeout — ADR-0278).
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

// NOLINTEND(misc-use-anonymous-namespace, misc-use-internal-linkage)

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

} /* extern "C" */
