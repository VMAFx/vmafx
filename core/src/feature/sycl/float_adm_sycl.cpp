/**
 *  Copyright 2026 Lusoris and Claude (Anthropic)
 *  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
 *
 *  float_adm feature kernel on the SYCL backend (T7-23 / batch 3
 *  part 6c — ADR-0192 / ADR-0202). SYCL twin of float_adm_vulkan
 *  (PR #154 / ADR-0199) and float_adm_cuda (this PR's `_cuda`
 *  sibling). Same four pipeline stages, same `-1` mirror form, same
 *  fused stage 3 with cross-band CM threshold.
 *
 *  Per-frame flow: 16 launches (4 stages × 4 scales). Self-contained
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

extern "C" {
#include "config.h"
#include "feature/adm_options.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature_name.h"
#include "log.h"
#include "picture.h"
#include "sycl/common.h"
}

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace
{

constexpr int FADM_BX = 16;
constexpr int FADM_BY = 16;
constexpr int FADM_NUM_SCALES = 4;
constexpr int FADM_NUM_BANDS = 3;
constexpr int FADM_ACCUM_SLOTS = 6;
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
template <int SCALE>
static sycl::event launch_dwt_vert(sycl::queue &q, const void *ref_raw, const void *dis_raw,
                                   unsigned raw_stride_bytes, const float *parent_ref_band,
                                   const float *parent_dis_band, unsigned parent_buf_stride,
                                   unsigned parent_w, unsigned parent_h, float *dwt_tmp_ref,
                                   float *dwt_tmp_dis, unsigned cur_w, unsigned cur_h,
                                   unsigned half_h, unsigned bpc, float scaler, float pixel_offset)
{
    const size_t global_x = ((cur_w + FADM_BX - 1u) / FADM_BX) * FADM_BX;
    const size_t global_y = ((half_h + FADM_BY - 1u) / FADM_BY) * FADM_BY;
    const unsigned e_cur_w = cur_w;
    const unsigned e_cur_h = cur_h;
    const unsigned e_half_h = half_h;
    const unsigned e_bpc = bpc;
    const unsigned e_raw_stride = raw_stride_bytes;
    const unsigned e_parent_w = parent_w;
    const unsigned e_parent_h = parent_h;
    const unsigned e_parent_buf_stride = parent_buf_stride;
    const float e_scaler = scaler;
    const float e_pixel_offset = pixel_offset;
    const void *e_ref_raw = ref_raw;
    const void *e_dis_raw = dis_raw;
    const float *e_p_ref_band = parent_ref_band;
    const float *e_p_dis_band = parent_dis_band;
    float *e_dwt_ref = dwt_tmp_ref;
    float *e_dwt_dis = dwt_tmp_dis;

    return q.submit([&](sycl::handler &cgh) {
        cgh.parallel_for(
            sycl::nd_range<3>(sycl::range<3>(2, global_y, global_x),
                              sycl::range<3>(1, FADM_BY, FADM_BX)),
            [=](sycl::nd_item<3> item) {
                const int gx = (int)item.get_global_id(2);
                const int gy = (int)item.get_global_id(1);
                const int plane_is_dis = (int)item.get_global_id(0);
                if (gx >= (int)e_cur_w || gy >= (int)e_half_h)
                    return;

                auto mirror = [](int idx, int sup) -> int {
                    if (idx < 0)
                        return -idx;
                    if (idx >= sup)
                        return 2 * sup - idx - 1;
                    return idx;
                };
                auto read_src = [&](const void *plane, int y, int x) -> float {
                    y = mirror(y, (int)e_cur_h);
                    if (x < 0)
                        x = 0;
                    if (x >= (int)e_cur_w)
                        x = (int)e_cur_w - 1;
                    if (e_bpc <= 8u) {
                        return (float)static_cast<const uint8_t *>(plane)[y * e_raw_stride + x] +
                               e_pixel_offset;
                    }
                    const uint16_t v = reinterpret_cast<const uint16_t *>(
                        static_cast<const uint8_t *>(plane) + y * e_raw_stride)[x];
                    return (float)v / e_scaler + e_pixel_offset;
                };
                auto read_parent = [&](const float *band, int y, int x) -> float {
                    y = mirror(y, (int)e_parent_h);
                    if (x < 0)
                        x = 0;
                    if (x >= (int)e_parent_w)
                        x = (int)e_parent_w - 1;
                    return band[y * (int)e_parent_buf_stride + x];
                };

                const int row_start = 2 * gy - 1;
                float s[4];
                for (int k = 0; k < 4; k++) {
                    if constexpr (SCALE == 0) {
                        const void *plane = (plane_is_dis == 0) ? e_ref_raw : e_dis_raw;
                        s[k] = read_src(plane, row_start + k, gx);
                    } else {
                        const float *band = (plane_is_dis == 0) ? e_p_ref_band : e_p_dis_band;
                        s[k] = read_parent(band, row_start + k, gx);
                    }
                }
                const float lo =
                    FADM_LO0 * s[0] + FADM_LO1 * s[1] + FADM_LO2 * s[2] + FADM_LO3 * s[3];
                const float hi =
                    FADM_HI0 * s[0] + FADM_HI1 * s[1] + FADM_HI2 * s[2] + FADM_HI3 * s[3];
                const int out_stride = (int)e_cur_w * 2;
                float *dst = (plane_is_dis == 0) ? e_dwt_ref : e_dwt_dis;
                dst[gy * out_stride + gx] = lo;
                dst[gy * out_stride + (int)e_cur_w + gx] = hi;
            });
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
    const size_t global_x = ((half_w + FADM_BX - 1u) / FADM_BX) * FADM_BX;
    const size_t global_y = ((half_h + FADM_BY - 1u) / FADM_BY) * FADM_BY;
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
                if (gx >= (int)e_half_w || gy >= (int)e_half_h)
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
static sycl::event launch_decouple_csf(sycl::queue &q, const float *ref_band, const float *dis_band,
                                       float *csf_a, float *csf_f, unsigned half_w, unsigned half_h,
                                       unsigned buf_stride, float rfactor_h, float rfactor_v,
                                       float rfactor_d, float gain_limit)
{
    const size_t global_x = ((half_w + FADM_BX - 1u) / FADM_BX) * FADM_BX;
    const size_t global_y = ((half_h + FADM_BY - 1u) / FADM_BY) * FADM_BY;
    const unsigned e_half_w = half_w;
    const unsigned e_half_h = half_h;
    const unsigned e_buf_stride = buf_stride;
    const float e_rfh = rfactor_h;
    const float e_rfv = rfactor_v;
    const float e_rfd = rfactor_d;
    const float e_gl = gain_limit;
    const float *e_ref = ref_band;
    const float *e_dis = dis_band;
    float *e_csf_a = csf_a;
    float *e_csf_f = csf_f;

    return q.submit([&](sycl::handler &cgh) {
        cgh.parallel_for(
            sycl::nd_range<2>(sycl::range<2>(global_y, global_x), sycl::range<2>(FADM_BY, FADM_BX)),
            [=](sycl::nd_item<2> item) {
                const int gx = (int)item.get_global_id(1);
                const int gy = (int)item.get_global_id(0);
                if (gx >= (int)e_half_w || gy >= (int)e_half_h)
                    return;
                const int slice = (int)e_buf_stride * (int)e_half_h;
                auto rb = [&](int band, int y, int x) -> float {
                    return e_ref[band * slice + y * (int)e_buf_stride + x];
                };
                auto db = [&](int band, int y, int x) -> float {
                    return e_dis[band * slice + y * (int)e_buf_stride + x];
                };
                const float oh = rb(1, gy, gx);
                const float ov = rb(2, gy, gx);
                const float od = rb(3, gy, gx);
                const float th = db(1, gy, gx);
                const float tv = db(2, gy, gx);
                const float td = db(3, gy, gx);
                const float ot_dp = (oh * th) + (ov * tv);
                const float o_mag = (oh * oh) + (ov * ov);
                const float t_mag = (th * th) + (tv * tv);
                const float lhs = ot_dp * ot_dp;
                const float rhs = FADM_COS_1DEG_SQ * (o_mag * t_mag);
                const bool angle_flag = (ot_dp >= 0.0f) && (lhs >= rhs);

                float oarr[3] = {oh, ov, od};
                float tarr[3] = {th, tv, td};
                float rfac[3] = {e_rfh, e_rfv, e_rfd};
                for (int b = 0; b < FADM_NUM_BANDS; b++) {
                    float k = tarr[b] / (oarr[b] + FADM_EPS);
                    k = sycl::fmax(0.0f, sycl::fmin(k, 1.0f));
                    float rst = k * oarr[b];
                    if (angle_flag && rst > 0.0f)
                        rst = sycl::fmin(rst * e_gl, tarr[b]);
                    else if (angle_flag && rst < 0.0f)
                        rst = sycl::fmax(rst * e_gl, tarr[b]);
                    const float a_val = tarr[b] - rst;
                    const float csf_a_val = rfac[b] * a_val;
                    e_csf_a[b * slice + gy * (int)e_buf_stride + gx] = csf_a_val;
                    e_csf_f[b * slice + gy * (int)e_buf_stride + gx] =
                        FADM_ONE_BY_30 * sycl::fabs(csf_a_val);
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
                                 float rfactor_d, float gain_limit)
{
    const int active_h = active_bottom - active_top;
    const int active_w = active_right - active_left;
    if (active_h <= 0 || active_w <= 0)
        return sycl::event{};
    const size_t num_groups = (size_t)(3 * active_h);
    const size_t WG_SIZE = FADM_BX * FADM_BY;
    const size_t global_x = num_groups * WG_SIZE;

    const unsigned e_half_w = half_w;
    const unsigned e_half_h = half_h;
    const unsigned e_buf_stride = buf_stride;
    const int e_left = active_left;
    const int e_top = active_top;
    const int e_right = active_right;
    const float e_rfh = rfactor_h;
    const float e_rfv = rfactor_v;
    const float e_rfd = rfactor_d;
    const float e_gl = gain_limit;
    const unsigned e_active_h = (unsigned)active_h;
    const float *e_ref = ref_band;
    const float *e_dis = dis_band;
    const float *e_csf_a = csf_a;
    const float *e_csf_f = csf_f;
    float *e_accum = accum_out;

    return q.submit([&](sycl::handler &cgh) {
        sycl::local_accessor<float, 1> s_csf(sycl::range<1>(WG_SIZE / 32), cgh);
        sycl::local_accessor<float, 1> s_cm(sycl::range<1>(WG_SIZE / 32), cgh);
        cgh.parallel_for(sycl::nd_range<1>(sycl::range<1>(global_x), sycl::range<1>(WG_SIZE)),
                         [=](sycl::nd_item<1> item) VMAF_SYCL_REQD_SG_SIZE(32) {
                             const unsigned wg_id = (unsigned)item.get_group(0);
                             const unsigned lid = (unsigned)item.get_local_id(0);
                             const unsigned band_idx = wg_id / e_active_h;
                             const unsigned row_idx = wg_id - band_idx * e_active_h;
                             const int row = e_top + (int)row_idx;
                             const int slice = (int)e_buf_stride * (int)e_half_h;
                             const float rfactor_band = (band_idx == 0u) ? e_rfh :
                                                        (band_idx == 1u) ? e_rfv :
                                                                           e_rfd;

                             auto rb = [&](int band, int y, int x) -> float {
                                 return e_ref[band * slice + y * (int)e_buf_stride + x];
                             };
                             auto db_ = [&](int band, int y, int x) -> float {
                                 return e_dis[band * slice + y * (int)e_buf_stride + x];
                             };
                             auto read_csf_f = [&](int band, int y, int x) -> float {
                                 if (x < 0)
                                     x = -x;
                                 if (x >= (int)e_half_w)
                                     x = 2 * (int)e_half_w - x - 2;
                                 if (y < 0)
                                     y = -y;
                                 if (y >= (int)e_half_h)
                                     y = 2 * (int)e_half_h - y - 2;
                                 if (x < 0)
                                     x = 0;
                                 if (y < 0)
                                     y = 0;
                                 if (x >= (int)e_half_w)
                                     x = (int)e_half_w - 1;
                                 if (y >= (int)e_half_h)
                                     y = (int)e_half_h - 1;
                                 return e_csf_f[band * slice + y * (int)e_buf_stride + x];
                             };
                             auto read_csf_a = [&](int band, int y, int x) -> float {
                                 if (x < 0)
                                     x = 0;
                                 if (x >= (int)e_half_w)
                                     x = (int)e_half_w - 1;
                                 if (y < 0)
                                     y = 0;
                                 if (y >= (int)e_half_h)
                                     y = (int)e_half_h - 1;
                                 return e_csf_a[band * slice + y * (int)e_buf_stride + x];
                             };

                             float local_csf_sum = 0.0f;
                             float local_cm_sum = 0.0f;
                             for (int col = e_left + (int)lid; col < e_right; col += (int)WG_SIZE) {
                                 const float src_ref = rb((int)band_idx + 1, row, col);
                                 const float csf_o = sycl::fabs(rfactor_band * src_ref);
                                 local_csf_sum += csf_o * csf_o * csf_o;

                                 const float oh = rb(1, row, col);
                                 const float ov = rb(2, row, col);
                                 const float od = rb(3, row, col);
                                 const float th = db_(1, row, col);
                                 const float tv = db_(2, row, col);
                                 const float td = db_(3, row, col);
                                 (void)od;
                                 (void)td;
                                 const float ot_dp = (oh * th) + (ov * tv);
                                 const float o_mag = (oh * oh) + (ov * ov);
                                 const float t_mag = (th * th) + (tv * tv);
                                 const float lhs = ot_dp * ot_dp;
                                 const float rhs = FADM_COS_1DEG_SQ * (o_mag * t_mag);
                                 const bool angle_flag = (ot_dp >= 0.0f) && (lhs >= rhs);

                                 float oarr[3] = {oh, ov, od};
                                 float tarr[3] = {th, tv, td};
                                 float k = tarr[band_idx] / (oarr[band_idx] + FADM_EPS);
                                 k = sycl::fmax(0.0f, sycl::fmin(k, 1.0f));
                                 float r_val = k * oarr[band_idx];
                                 if (angle_flag && r_val > 0.0f)
                                     r_val = sycl::fmin(r_val * e_gl, tarr[band_idx]);
                                 else if (angle_flag && r_val < 0.0f)
                                     r_val = sycl::fmax(r_val * e_gl, tarr[band_idx]);

                                 float thr = 0.0f;
                                 for (int b = 0; b < FADM_NUM_BANDS; b++) {
                                     for (int dy = -1; dy <= 1; dy++) {
                                         for (int dx = -1; dx <= 1; dx++) {
                                             if (dx == 0 && dy == 0)
                                                 continue;
                                             thr += read_csf_f(b, row + dy, col + dx);
                                         }
                                     }
                                 }
                                 const float own_h = read_csf_a(0, row, col);
                                 const float own_v = read_csf_a(1, row, col);
                                 const float own_d = read_csf_a(2, row, col);
                                 thr += FADM_ONE_BY_15 * sycl::fabs(own_h);
                                 thr += FADM_ONE_BY_15 * sycl::fabs(own_v);
                                 thr += FADM_ONE_BY_15 * sycl::fabs(own_d);

                                 const float x_val = rfactor_band * r_val;
                                 float xa = sycl::fabs(x_val) - thr;
                                 if (xa < 0.0f)
                                     xa = 0.0f;
                                 local_cm_sum += xa * xa * xa;
                             }

                             sycl::sub_group sg = item.get_sub_group();
                             const float wn_csf =
                                 sycl::reduce_over_group(sg, local_csf_sum, sycl::plus<float>{});
                             const float wn_cm =
                                 sycl::reduce_over_group(sg, local_cm_sum, sycl::plus<float>{});
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
                                 e_accum[slot_base + band_idx] = total_csf;
                                 e_accum[slot_base + 3u + band_idx] = total_cm;
                             }
                         });
    });
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
     .alias = "egl",
     .help = "enhancement gain (>=1.0)",
     .offset = offsetof(FloatAdmStateSycl, adm_enhn_gain_limit),
     .type = VMAF_OPT_TYPE_DOUBLE,
     .default_val = {.d = 100.0},
     .min = 1.0,
     .max = 100.0,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_norm_view_dist",
     .alias = "nvd",
     .help = "normalized viewing distance",
     .offset = offsetof(FloatAdmStateSycl, adm_norm_view_dist),
     .type = VMAF_OPT_TYPE_DOUBLE,
     .default_val = {.d = 3.0},
     .min = 0.75,
     .max = 24.0,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_ref_display_height",
     .alias = "rdf",
     .help = "reference display height in pixels",
     .offset = offsetof(FloatAdmStateSycl, adm_ref_display_height),
     .type = VMAF_OPT_TYPE_INT,
     .default_val = {.i = 1080},
     .min = 1,
     .max = 4320,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_csf_mode",
     .alias = "csf",
     .help = "contrast sensitivity function (mode 0 only on SYCL v1)",
     .offset = offsetof(FloatAdmStateSycl, adm_csf_mode),
     .type = VMAF_OPT_TYPE_INT,
     .default_val = {.i = 0},
     .min = 0,
     .max = 9,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_csf_scale",
     .alias = "cs",
     .help = "CSF band-scale multiplier for h/v bands (default 1.0 = no scaling)",
     .offset = offsetof(FloatAdmStateSycl, adm_csf_scale),
     .type = VMAF_OPT_TYPE_DOUBLE,
     .default_val = {.d = DEFAULT_ADM_CSF_SCALE},
     .min = 0.0,
     .max = 100.0,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_csf_diag_scale",
     .alias = "cds",
     .help = "CSF band-scale multiplier for diagonal bands (default 1.0 = no scaling)",
     .offset = offsetof(FloatAdmStateSycl, adm_csf_diag_scale),
     .type = VMAF_OPT_TYPE_DOUBLE,
     .default_val = {.d = DEFAULT_ADM_CSF_DIAG_SCALE},
     .min = 0.0,
     .max = 100.0,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_noise_weight",
     .alias = "nw",
     .help = "noise floor weight for CM numerator (default 0.03125 = 1/32)",
     .offset = offsetof(FloatAdmStateSycl, adm_noise_weight),
     .type = VMAF_OPT_TYPE_DOUBLE,
     .default_val = {.d = DEFAULT_ADM_NOISE_WEIGHT},
     .min = 0.0,
     .max = 100.0,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {0}};

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

    /* Per-scale dims. */
    unsigned cw = w;
    unsigned ch = h;
    for (int scale = 0; scale < FADM_NUM_SCALES; scale++) {
        s->scale_w[scale] = cw;
        s->scale_h[scale] = ch;
        s->scale_half_w[scale] = (cw + 1u) / 2u;
        s->scale_half_h[scale] = (ch + 1u) / 2u;
        cw = s->scale_half_w[scale];
        ch = s->scale_half_h[scale];
    }
    s->buf_stride = (s->scale_half_w[0] + 3u) & ~3u;

    for (int scale = 0; scale < FADM_NUM_SCALES; scale++) {
        const float f1 =
            fadm_dwt_quant_step(scale, 1, s->adm_norm_view_dist, s->adm_ref_display_height);
        const float f2 =
            fadm_dwt_quant_step(scale, 2, s->adm_norm_view_dist, s->adm_ref_display_height);
        /* adm_csf_scale / adm_csf_diag_scale multiply the CSF sensitivity
         * (matches the CPU Watson-mode path: rfactor = scale * (1/quant_step)).
         * Default 1.0 → identical rfactors to the pre-PR-731 behaviour. */
        s->rfactor[scale * 3 + 0] = (float)s->adm_csf_scale / f1;
        s->rfactor[scale * 3 + 1] = (float)s->adm_csf_scale / f1;
        s->rfactor[scale * 3 + 2] = (float)s->adm_csf_diag_scale / f2;
    }

    if (!fex->sycl_state)
        return -EINVAL;
    s->sycl_state = fex->sycl_state;

    const size_t bpp = (bpc <= 8u) ? 1u : 2u;
    const size_t raw_bytes = (size_t)w * h * bpp;
    s->h_ref_raw = vmaf_sycl_malloc_host(s->sycl_state, raw_bytes);
    s->h_dis_raw = vmaf_sycl_malloc_host(s->sycl_state, raw_bytes);
    s->d_ref_raw = vmaf_sycl_malloc_device(s->sycl_state, raw_bytes);
    s->d_dis_raw = vmaf_sycl_malloc_device(s->sycl_state, raw_bytes);

    const size_t dwt_bytes = (size_t)w * 2u * s->scale_half_h[0] * sizeof(float);
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

    for (int scale = 0; scale < FADM_NUM_SCALES; scale++) {
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

    if (!s->h_ref_raw || !s->h_dis_raw || !s->d_ref_raw || !s->d_dis_raw || !s->d_dwt_tmp_ref ||
        !s->d_dwt_tmp_dis || !s->d_csf_a || !s->d_csf_f)
        return -ENOMEM;
    for (int scale = 0; scale < FADM_NUM_SCALES; scale++) {
        if (!s->d_ref_band[scale] || !s->d_dis_band[scale] || !s->d_accum[scale] ||
            !s->h_accum[scale])
            return -ENOMEM;
    }

    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (!s->feature_name_dict)
        return -ENOMEM;
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

    const size_t bpp = (s->bpc <= 8u) ? 1u : 2u;
    const unsigned raw_stride = (unsigned)(s->width * bpp);
    if (s->bpc <= 8u) {
        copy_y_plane<uint8_t>(ref_pic, s->h_ref_raw, s->width, s->height);
        copy_y_plane<uint8_t>(dist_pic, s->h_dis_raw, s->width, s->height);
    } else {
        copy_y_plane<uint16_t>(ref_pic, s->h_ref_raw, s->width, s->height);
        copy_y_plane<uint16_t>(dist_pic, s->h_dis_raw, s->width, s->height);
    }
    const size_t raw_bytes = (size_t)s->width * s->height * bpp;
    q.memcpy(s->d_ref_raw, s->h_ref_raw, raw_bytes);
    q.memcpy(s->d_dis_raw, s->h_dis_raw, raw_bytes);

    /* Reset accumulator buffers. */
    for (int scale = 0; scale < FADM_NUM_SCALES; scale++) {
        q.memset(s->d_accum[scale], 0,
                 (size_t)s->wg_count[scale] * FADM_ACCUM_SLOTS * sizeof(float));
    }

    float scaler = 1.0f;
    if (s->bpc == 10u)
        scaler = 4.0f;
    else if (s->bpc == 12u)
        scaler = 16.0f;
    else if (s->bpc == 16u)
        scaler = 256.0f;
    const float pixel_offset = -128.0f;

    for (int scale = 0; scale < FADM_NUM_SCALES; scale++) {
        const unsigned cur_w = s->scale_w[scale];
        const unsigned cur_h = s->scale_h[scale];
        const unsigned half_w = s->scale_half_w[scale];
        const unsigned half_h = s->scale_half_h[scale];
        /* Parent LL band dimensions = scale_w/h[scale] (input dim at
         * this scale = parent's LL output dim). See float_adm_cuda.c
         * for the long version of this comment. */
        const unsigned parent_w = (scale > 0) ? s->scale_w[scale] : 0u;
        const unsigned parent_h = (scale > 0) ? s->scale_h[scale] : 0u;
        const float *parent_ref = (scale > 0) ? s->d_ref_band[scale - 1] : nullptr;
        const float *parent_dis = (scale > 0) ? s->d_dis_band[scale - 1] : nullptr;

        int top = (int)((double)half_h * FADM_BORDER_FACTOR - 0.5);
        int left = (int)((double)half_w * FADM_BORDER_FACTOR - 0.5);
        if (top < 0)
            top = 0;
        if (left < 0)
            left = 0;
        const int bottom = (int)half_h - top;
        const int right = (int)half_w - left;

        if (scale == 0)
            launch_dwt_vert<0>(q, s->d_ref_raw, s->d_dis_raw, raw_stride, parent_ref, parent_dis,
                               s->buf_stride, parent_w, parent_h, s->d_dwt_tmp_ref,
                               s->d_dwt_tmp_dis, cur_w, cur_h, half_h, s->bpc, scaler,
                               pixel_offset);
        else if (scale == 1)
            launch_dwt_vert<1>(q, s->d_ref_raw, s->d_dis_raw, raw_stride, parent_ref, parent_dis,
                               s->buf_stride, parent_w, parent_h, s->d_dwt_tmp_ref,
                               s->d_dwt_tmp_dis, cur_w, cur_h, half_h, s->bpc, scaler,
                               pixel_offset);
        else if (scale == 2)
            launch_dwt_vert<2>(q, s->d_ref_raw, s->d_dis_raw, raw_stride, parent_ref, parent_dis,
                               s->buf_stride, parent_w, parent_h, s->d_dwt_tmp_ref,
                               s->d_dwt_tmp_dis, cur_w, cur_h, half_h, s->bpc, scaler,
                               pixel_offset);
        else
            launch_dwt_vert<3>(q, s->d_ref_raw, s->d_dis_raw, raw_stride, parent_ref, parent_dis,
                               s->buf_stride, parent_w, parent_h, s->d_dwt_tmp_ref,
                               s->d_dwt_tmp_dis, cur_w, cur_h, half_h, s->bpc, scaler,
                               pixel_offset);

        launch_dwt_hori(q, s->d_dwt_tmp_ref, s->d_dwt_tmp_dis, s->d_ref_band[scale],
                        s->d_dis_band[scale], cur_w, half_w, half_h, s->buf_stride);
        launch_decouple_csf(q, s->d_ref_band[scale], s->d_dis_band[scale], s->d_csf_a, s->d_csf_f,
                            half_w, half_h, s->buf_stride, s->rfactor[scale * 3 + 0],
                            s->rfactor[scale * 3 + 1], s->rfactor[scale * 3 + 2],
                            (float)s->adm_enhn_gain_limit);
        launch_csf_cm(q, s->d_ref_band[scale], s->d_dis_band[scale], s->d_csf_a, s->d_csf_f,
                      s->d_accum[scale], half_w, half_h, s->buf_stride, left, top, right, bottom,
                      s->rfactor[scale * 3 + 0], s->rfactor[scale * 3 + 1],
                      s->rfactor[scale * 3 + 2], (float)s->adm_enhn_gain_limit);
    }

    for (int scale = 0; scale < FADM_NUM_SCALES; scale++) {
        q.memcpy(s->h_accum[scale], s->d_accum[scale],
                 (size_t)s->wg_count[scale] * FADM_ACCUM_SLOTS * sizeof(float));
    }

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

    double cm_totals[FADM_NUM_SCALES][FADM_NUM_BANDS] = {{0.0}};
    double csf_totals[FADM_NUM_SCALES][FADM_NUM_BANDS] = {{0.0}};
    for (int scale = 0; scale < FADM_NUM_SCALES; scale++) {
        const float *slots = s->h_accum[scale];
        const unsigned wg_count = s->wg_count[scale];
        for (unsigned wg = 0u; wg < wg_count; wg++) {
            const float *p = slots + (size_t)wg * FADM_ACCUM_SLOTS;
            for (int b = 0; b < FADM_NUM_BANDS; b++) {
                csf_totals[scale][b] += (double)p[b];
                cm_totals[scale][b] += (double)p[3 + b];
            }
        }
    }

    double score_num = 0.0;
    double score_den = 0.0;
    double scores[8];
    for (int scale = 0; scale < FADM_NUM_SCALES; scale++) {
        const int hw = (int)s->scale_half_w[scale];
        const int hh = (int)s->scale_half_h[scale];
        int left = (int)((double)hw * FADM_BORDER_FACTOR - 0.5);
        int top = (int)((double)hh * FADM_BORDER_FACTOR - 0.5);
        if (left < 0)
            left = 0;
        if (top < 0)
            top = 0;
        const int right = hw - left;
        const int bottom = hh - top;
        const float area_cbrt = std::pow(
            (float)((bottom - top) * (right - left)) * (float)s->adm_noise_weight, 1.0f / 3.0f);
        float num_scale = 0.0f;
        float den_scale = 0.0f;
        for (int b = 0; b < FADM_NUM_BANDS; b++) {
            num_scale += std::pow((float)cm_totals[scale][b], 1.0f / 3.0f) + area_cbrt;
            den_scale += std::pow((float)csf_totals[scale][b], 1.0f / 3.0f) + area_cbrt;
        }
        scores[2 * scale + 0] = num_scale;
        scores[2 * scale + 1] = den_scale;
        score_num += num_scale;
        score_den += den_scale;
    }

    const int w = (int)s->scale_w[0];
    const int h = (int)s->scale_h[0];
    const double numden_limit = 1e-2 * (double)(w * h) / (1920.0 * 1080.0);
    if (score_num < numden_limit)
        score_num = 0.0;
    if (score_den < numden_limit)
        score_den = 0.0;
    const double score = (score_den == 0.0) ? 1.0 : score_num / score_den;

    int err = 0;
    err |= vmaf_feature_collector_append_with_dict(fc, s->feature_name_dict,
                                                   "VMAF_feature_adm2_score", score, index);
    err |= vmaf_feature_collector_append_with_dict(
        fc, s->feature_name_dict, "VMAF_feature_adm_scale0_score", scores[0] / scores[1], index);
    err |= vmaf_feature_collector_append_with_dict(
        fc, s->feature_name_dict, "VMAF_feature_adm_scale1_score", scores[2] / scores[3], index);
    err |= vmaf_feature_collector_append_with_dict(
        fc, s->feature_name_dict, "VMAF_feature_adm_scale2_score", scores[4] / scores[5], index);
    err |= vmaf_feature_collector_append_with_dict(
        fc, s->feature_name_dict, "VMAF_feature_adm_scale3_score", scores[6] / scores[7], index);

    if (s->debug && !err) {
        err |=
            vmaf_feature_collector_append_with_dict(fc, s->feature_name_dict, "adm", score, index);
        err |= vmaf_feature_collector_append_with_dict(fc, s->feature_name_dict, "adm_num",
                                                       score_num, index);
        err |= vmaf_feature_collector_append_with_dict(fc, s->feature_name_dict, "adm_den",
                                                       score_den, index);
        const char *names[8] = {"adm_num_scale0", "adm_den_scale0", "adm_num_scale1",
                                "adm_den_scale1", "adm_num_scale2", "adm_den_scale2",
                                "adm_num_scale3", "adm_den_scale3"};
        for (int i = 0; i < 8 && !err; i++) {
            err |= vmaf_feature_collector_append_with_dict(fc, s->feature_name_dict, names[i],
                                                           scores[i], index);
        }
    }
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
                                                         NULL};

extern "C" VmafFeatureExtractor vmaf_fex_float_adm_sycl = {
    .name = "float_adm_sycl",
    .init = init_fex_sycl,
    .extract = NULL,
    .flush = NULL,
    .close = close_fex_sycl,
    .submit = submit_fex_sycl,
    .collect = collect_fex_sycl,
    .options = options_float_adm_sycl,
    .priv_size = sizeof(FloatAdmStateSycl),
    .flags = VMAF_FEATURE_EXTRACTOR_SYCL,
    .provided_features = provided_features_float_adm_sycl,
};

} /* extern "C" */
