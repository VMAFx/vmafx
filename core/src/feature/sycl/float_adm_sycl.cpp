/**
 *  Copyright 2016-2020 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  float_adm feature kernel on the SYCL backend (T7-23 / batch 3
 *  part 6c — ADR-0192 / ADR-0202).
 *
 *  Per scale: the DWT (vertical, horizontal), the decouple with both CSF
 *  passes, the per-sample terms of the three reductions, and their row sums.
 *  Self-contained submit/collect — does NOT use the shared_frame model (the
 *  multi-scale band/csf layout doesn't fit).
 *
 *  Numerical contract (ADR-1434, after ADR-1420 for the CUDA twin). The twin
 *  returns the CPU extractor's values bit for bit:
 *   - the decouple, the CSF and the masking threshold are adm_tools.c
 *     operation for operation (sycl_float_adm_math.h), with the reference's
 *     three fp64 expressions evaluated without the fp64 type and its
 *     decouple quotient the IEEE fp32 one (ADR-1442);
 *   - adm_csf_den_scale_s() and adm_cm_s() add a row into one fp32
 *     accumulator per band and the rows into another. The terms kernel
 *     stores each sample's nine terms, a row kernel adds every row left to
 *     right, and the host adds the rows;
 *   - the CSF weights, the reduced region, the pooling of a scale and the
 *     angle threshold are the reference's own routines
 *     (adm_float_reference.h).
 *  adm_p_norm other than 1 or 3 raises every term with pow(), the device's on
 *  one side and glibc's on the other: that option is close to the CPU, not
 *  equal.
 *
 *  No kernel uses scratch memory (ADR-1395): band values are read into named
 *  scalars, never into an array indexed at run time.
 */

#include <sycl/sycl.hpp>

#include "sycl_compat.h"
#include "sycl_float_adm_math.h"

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <utility>

#include "config.h"
#include "feature/adm_csf_fixed_point.h"
#include "feature/adm_float_reference.h"
#include "feature/adm_options.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature_name.h"
#include "feature/adm_score.h"
#include "feature/nonfinite_score.h"
#include "log.h"
#include "picture.h"
#include "sycl/common.h"

namespace
{

constexpr int FADM_BX = 16;
constexpr int FADM_BY = 16;
constexpr int FADM_NUM_SCALES = 4;
constexpr int FADM_NUM_BANDS = 3;

constexpr float FADM_LO0 = 0.482962913144690f;
constexpr float FADM_LO1 = 0.836516303737469f;
constexpr float FADM_LO2 = 0.224143868041857f;
constexpr float FADM_LO3 = -0.129409522550921f;
constexpr float FADM_HI0 = -0.129409522550921f;
constexpr float FADM_HI1 = -0.224143868041857f;
constexpr float FADM_HI2 = 0.836516303737469f;
constexpr float FADM_HI3 = -0.482962913144690f;

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
    int adm_bypass_cm;
    int adm_adm3_apply_hm;
    double adm_p_norm;
    double adm_dlm_weight;
    double adm_min_val;
    /* Per-scale CSF weight overrides; negative keeps the model's value. */
    double adm_f1s0;
    double adm_f1s1;
    double adm_f1s2;
    double adm_f1s3;
    double adm_f2s0;
    double adm_f2s1;
    double adm_f2s2;
    double adm_f2s3;
    int adm_skip_aim_scale; /* -1 = no skip */
    bool adm_skip_scale0;

    unsigned width;
    unsigned height;
    unsigned bpc;
    unsigned buf_stride;

    /* What the reference derives per frame, from its own routines (init). */
    float rfactor[FADM_NUM_SCALES][FADM_NUM_BANDS]; /* adm_csf_rfactor_s() */
    AdmBorderS region[FADM_NUM_SCALES];             /* adm_border_s() */
    float cos_1deg_sq;                              /* adm_decouple_cos_1deg_sq_s() */
    vmaf_sycl_fadm::GainLimit gain_limit;           /* adm_enhn_gain_limit for the kernels */

    VmafSyclState *sycl_state;

    void *h_ref_raw;
    void *h_dis_raw;
    void *d_ref_raw;
    void *d_dis_raw;
    float *d_dwt_tmp_ref;
    float *d_dwt_tmp_dis;
    float *d_ref_band[FADM_NUM_SCALES];
    float *d_dis_band[FADM_NUM_SCALES];
    /* The four CSF buffers of the scale being computed, three sub-bands
     * each: rfactor * decouple_a, its filtered magnitude, rfactor *
     * decouple_r, its filtered magnitude. */
    float *d_csf_a;
    float *d_csf_fa;
    float *d_csf_r;
    float *d_csf_fr;
    /* Per-sample terms of the scale being computed (scale-0 size; the queue
     * is in order, so the scales share them), see vmaf_sycl_fadm::term_index(). */
    float *d_terms;
    /* Row sums of every scale, nine slots per scale at row_offset[scale];
     * one copy to the host per frame. */
    float *d_rows;
    float *h_rows;
    size_t row_offset[FADM_NUM_SCALES];
    size_t row_floats;
    unsigned scale_w[FADM_NUM_SCALES];
    unsigned scale_h[FADM_NUM_SCALES];
    unsigned scale_half_w[FADM_NUM_SCALES];
    unsigned scale_half_h[FADM_NUM_SCALES];

    bool has_pending;
    unsigned pending_index;

    VmafDictionary *feature_name_dict;
};

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
/* Stages 2 to 4. What a work-item does is sycl_float_adm_math.h's      */
/* decouple_sample(), terms_sample() and row_item(); these launch them. */
/* ------------------------------------------------------------------ */

/* Stage 2 — decouple and both CSF passes over the whole band. */
static sycl::event launch_decouple_csf(sycl::queue &q, const vmaf_sycl_fadm::DecoupleArgs &args)
{
    const size_t global_x =
        (size_t)(((unsigned)args.bands.half_w + FADM_BX - 1u) / FADM_BX) * FADM_BX;
    const size_t global_y =
        (size_t)(((unsigned)args.bands.half_h + FADM_BY - 1u) / FADM_BY) * FADM_BY;
    return q.submit([&](sycl::handler &cgh) {
        cgh.parallel_for(
            sycl::nd_range<2>(sycl::range<2>(global_y, global_x), sycl::range<2>(FADM_BY, FADM_BX)),
            [=](sycl::nd_item<2> item) {
                const int x = (int)item.get_global_id(1);
                const int y = (int)item.get_global_id(0);
                if (x < args.bands.half_w && y < args.bands.half_h)
                    vmaf_sycl_fadm::decouple_sample(args, y, x);
            });
    });
}

/* Stage 3 — the nine terms of every sample of the reduced region, in the
 * shape sycl_float_adm_math.h gives the term kernel. */
class FadmTermsKernel
    : public VmafSyclKernelShape<vmaf_sycl_fadm::kTermsSubGroup, vmaf_sycl_fadm::kTermsGrf>
{
  public:
    explicit FadmTermsKernel(const vmaf_sycl_fadm::TermArgs &args) : args_(args)
    {
    }

    void operator()(sycl::id<2> region) const
    {
        vmaf_sycl_fadm::terms_sample(args_, (unsigned)region[0], (unsigned)region[1]);
    }

  private:
    vmaf_sycl_fadm::TermArgs args_;
};

static sycl::event launch_terms(sycl::queue &q, const vmaf_sycl_fadm::TermArgs &args)
{
    return q.submit([&](sycl::handler &cgh) {
        cgh.parallel_for(sycl::range<2>(args.region_h, args.region_w), FadmTermsKernel(args));
    });
}

/* Stage 4 — one work-item per (slot, row) adds that row's terms left to right
 * in fp32, the reference's per-row accumulator. Sub-group size 16: the lanes
 * of a hardware thread are rows, the pass is bound by reading the terms, and
 * 16 is the narrowest size every AOT target accepts (ADR-1468). */
static sycl::event launch_row_sums(sycl::queue &q, const vmaf_sycl_fadm::RowArgs &args)
{
    const size_t count = (size_t)vmaf_sycl_fadm::kTermSlots * args.region_h;
    return q.submit([&](sycl::handler &cgh) {
        cgh.parallel_for(sycl::range<1>(count), [=](sycl::id<1> id) VMAF_SYCL_REQD_SG_SIZE(16) {
            vmaf_sycl_fadm::row_item(args, id[0]);
        });
    });
}

static void fadm_configure_dimensions(FloatAdmStateSycl *s, unsigned width, unsigned height)
{
    unsigned current_width = width;
    unsigned current_height = height;
    s->row_floats = 0u;
    for (int scale = 0; scale < FADM_NUM_SCALES; scale++) {
        s->scale_w[scale] = current_width;
        s->scale_h[scale] = current_height;
        s->scale_half_w[scale] = (current_width + 1u) / 2u;
        s->scale_half_h[scale] = (current_height + 1u) / 2u;
        current_width = s->scale_half_w[scale];
        current_height = s->scale_half_h[scale];
        s->region[scale] = adm_border_s((int)s->scale_half_w[scale], (int)s->scale_half_h[scale],
                                        ADM_BORDER_FACTOR);
        s->row_offset[scale] = s->row_floats;
        s->row_floats += (size_t)vmaf_sycl_fadm::kTermSlots *
                         (size_t)(s->region[scale].bottom - s->region[scale].top);
    }
    s->buf_stride = (s->scale_half_w[0] + 3u) & ~3u;
}

/* The constants the reference derives per frame, taken from its own routines
 * so they cannot drift from it (ADR-1420, ADR-1434).
 *
 * The CSF weights come from adm_csf_rfactor_s() with the options float_adm.c
 * passes, the per-scale overrides included, and the reference's luminance
 * level. In the Watson-97 mode this twin supports the weights ignore
 * adm_csf_scale / adm_csf_diag_scale, as on the CPU (ADR-1214). */
static void fadm_init_reference(FloatAdmStateSycl *s)
{
    for (int scale = 0; scale < FADM_NUM_SCALES; scale++) {
        adm_csf_rfactor_s(scale, s->adm_norm_view_dist, s->adm_ref_display_height, s->adm_csf_mode,
                          DEFAULT_ADM_CSF_LUMINANCE_LEVEL, s->adm_csf_scale, s->adm_csf_diag_scale,
                          s->adm_f1s0, s->adm_f1s1, s->adm_f1s2, s->adm_f1s3, s->adm_f2s0,
                          s->adm_f2s1, s->adm_f2s2, s->adm_f2s3, s->rfactor[scale]);
    }
    s->cos_1deg_sq = adm_decouple_cos_1deg_sq_s();
    s->gain_limit = vmaf_sycl_fadm::make_gain_limit(s->adm_enhn_gain_limit);
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
    s->d_csf_fa = static_cast<float *>(vmaf_sycl_malloc_device(s->sycl_state, csf_bytes));
    s->d_csf_r = static_cast<float *>(vmaf_sycl_malloc_device(s->sycl_state, csf_bytes));
    s->d_csf_fr = static_cast<float *>(vmaf_sycl_malloc_device(s->sycl_state, csf_bytes));
}

/* The terms of the largest scale and the row sums of every scale. */
static void fadm_allocate_sums(FloatAdmStateSycl *s)
{
    const AdmBorderS &r = s->region[0];
    const size_t term_bytes = (size_t)vmaf_sycl_fadm::kTermSlots * (size_t)(r.right - r.left) *
                              (size_t)(r.bottom - r.top) * sizeof(float);
    s->d_terms = static_cast<float *>(vmaf_sycl_malloc_device(s->sycl_state, term_bytes));
    const size_t row_bytes = s->row_floats * sizeof(float);
    s->d_rows = static_cast<float *>(vmaf_sycl_malloc_device(s->sycl_state, row_bytes));
    s->h_rows = static_cast<float *>(vmaf_sycl_malloc_host(s->sycl_state, row_bytes));
}

static bool fadm_allocations_complete(const FloatAdmStateSycl *s)
{
    if (!s->h_ref_raw || !s->h_dis_raw || !s->d_ref_raw || !s->d_dis_raw || !s->d_dwt_tmp_ref ||
        !s->d_dwt_tmp_dis || !s->d_csf_a || !s->d_csf_fa || !s->d_csf_r || !s->d_csf_fr ||
        !s->d_terms || !s->d_rows || !s->h_rows)
        return false;
    for (int scale = 0; scale < FADM_NUM_SCALES; scale++) {
        if (!s->d_ref_band[scale] || !s->d_dis_band[scale])
            return false;
    }
    return true;
}

/* Both luma planes into d_ref_raw / d_dis_raw, packed at the returned
 * stride: packed on the host and uploaded, or, for frames of the VMAFx API
 * on this device, copied on the device (ADR-2091; the frame's planes are
 * never read on the host). 0 when a device copy could not be enqueued. */
static unsigned fadm_upload_planes(sycl::queue &q, const FloatAdmStateSycl *s,
                                   const VmafPicture *ref_pic, const VmafPicture *dist_pic)
{
    const size_t bytes_per_pixel = (s->bpc <= 8u) ? 1u : 2u;
    const size_t row = (size_t)s->width * bytes_per_pixel;
    if (vmaf_sycl_picture_on_device(ref_pic) || vmaf_sycl_picture_on_device(dist_pic)) {
        const bool copied = vmaf_sycl_picture_read_plane(ref_pic, 0, &q, s->d_ref_raw, row, row,
                                                         s->height, nullptr) == 0 &&
                            vmaf_sycl_picture_read_plane(dist_pic, 0, &q, s->d_dis_raw, row, row,
                                                         s->height, nullptr) == 0;
        return copied ? (unsigned)row : 0u;
    }
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

static vmaf_sycl_fadm::Bands fadm_scale_bands(const FloatAdmStateSycl *s, int scale)
{
    return {.ref_band = s->d_ref_band[scale],
            .dis_band = s->d_dis_band[scale],
            .csf_a = s->d_csf_a,
            .csf_fa = s->d_csf_fa,
            .csf_r = s->d_csf_r,
            .csf_fr = s->d_csf_fr,
            .half_w = (int)s->scale_half_w[scale],
            .half_h = (int)s->scale_half_h[scale],
            .buf_stride = (int)s->buf_stride,
            .rfactor_h = s->rfactor[scale][0],
            .rfactor_v = s->rfactor[scale][1],
            .rfactor_d = s->rfactor[scale][2]};
}

static void fadm_launch_scale(sycl::queue &q, FloatAdmStateSycl *s, int scale, unsigned raw_stride,
                              float scaler)
{
    const unsigned current_width = s->scale_w[scale];
    const unsigned half_width = s->scale_half_w[scale];
    const unsigned half_height = s->scale_half_h[scale];
    fadm_launch_dwt_scale(q, s, scale, raw_stride, scaler);
    launch_dwt_hori(q, s->d_dwt_tmp_ref, s->d_dwt_tmp_dis, s->d_ref_band[scale],
                    s->d_dis_band[scale], current_width, half_width, half_height, s->buf_stride);
    /* adm_skip_scale0: the reference computes scale 0's approximation band,
     * which scale 1 reads, and nothing else of it. */
    if (scale == 0 && s->adm_skip_scale0)
        return;
    const vmaf_sycl_fadm::Bands bands = fadm_scale_bands(s, scale);
    launch_decouple_csf(q, {.bands = bands, .limit = s->gain_limit, .cos_1deg_sq = s->cos_1deg_sq});
    const AdmBorderS &r = s->region[scale];
    const int region_w = r.right - r.left;
    const int region_h = r.bottom - r.top;
    if (region_w <= 0 || region_h <= 0)
        return;
    launch_terms(q, {.bands = bands,
                     .terms = s->d_terms,
                     .left = r.left,
                     .top = r.top,
                     .region_w = (unsigned)region_w,
                     .region_h = (unsigned)region_h,
                     .p_norm = (float)s->adm_p_norm,
                     .is_cube = s->adm_p_norm == 3.0,
                     .bypass_cm = s->adm_bypass_cm != 0});
    launch_row_sums(q, {.terms = s->d_terms,
                        .rows = s->d_rows + s->row_offset[scale],
                        .region_w = (unsigned)region_w,
                        .region_h = (unsigned)region_h});
}

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

struct FadmScaleSums {
    float numerator;
    float denominator;
    float aim_numerator;
};

/* One scale of compute_adm() past the kernels. The frame accumulators are the
 * reference's: one fp32 value per band that the row sums are added to top to
 * bottom. The scale is then concluded by the reference's own
 * adm_pool_bands_s(), with the noise weight for the denominator and the adm2
 * numerator and with none for the AIM numerator. */
static FadmScaleSums fadm_pool_scale(const FloatAdmStateSycl *s, int scale)
{
    if (scale == 0 && s->adm_skip_scale0) {
        /* compute_adm(): `den_scale = 1e-10; // avoid divide by zero`. */
        return {.numerator = 0.0f, .denominator = (float)1e-10, .aim_numerator = 0.0f};
    }
    const AdmBorderS &r = s->region[scale];
    const int region_w = r.right - r.left;
    const int region_h = r.bottom - r.top;
    const float *rows = s->h_rows + s->row_offset[scale];
    float accum[vmaf_sycl_fadm::kTermSlots] = {};
    if (region_w > 0 && region_h > 0) {
        for (unsigned slot = 0u; slot < vmaf_sycl_fadm::kTermSlots; slot++) {
            accum[slot] = vmaf_sycl_fadm::fold_rows(rows + (size_t)slot * (size_t)region_h,
                                                    (unsigned)region_h);
        }
    }
    return {.numerator = adm_pool_bands_s(accum + vmaf_sycl_fadm::kSlotCm, region_w, region_h,
                                          s->adm_noise_weight, s->adm_p_norm),
            .denominator = adm_pool_bands_s(accum + vmaf_sycl_fadm::kSlotDen, region_w, region_h,
                                            s->adm_noise_weight, s->adm_p_norm),
            .aim_numerator = adm_pool_bands_s(accum + vmaf_sycl_fadm::kSlotAim, region_w, region_h,
                                              0.0, s->adm_p_norm)};
}

/* compute_adm()'s sums over the scales. */
static FadmPooledScores fadm_pool_scores(const FloatAdmStateSycl *s)
{
    FadmPooledScores pooled = {};
    for (int scale = 0; scale < FADM_NUM_SCALES; scale++) {
        const FadmScaleSums sums = fadm_pool_scale(s, scale);
        pooled.numerator += sums.numerator;
        pooled.denominator += sums.denominator;
        if (s->adm_skip_aim_scale != scale) {
            pooled.aim_denominator += sums.denominator;
            pooled.aim_numerator += sums.aim_numerator;
        }
        pooled.scales[2 * scale + 0] = sums.numerator;
        pooled.scales[2 * scale + 1] = sums.denominator;
    }
    return pooled;
}

static int fadm_finalize_scores(const FloatAdmStateSycl *s, unsigned index,
                                FadmPooledScores *pooled, FadmFinalScores *final)
{
    /* compute_adm()'s numden_limit, in its expression. */
    const int w = (int)s->width;
    const int h = (int)s->height;
    const double floor = 1e-10 * (w * h) / (1920.0 * 1080.0);
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
    values[1] = {.name = "VMAF_feature_adm_scale0_score",
                 .value = s->adm_skip_scale0 ? 0.0 : final.scales[0]};
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
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM | VMAF_OPT_FLAG_DEFAULT_ONLY},
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
    {.name = "adm_bypass_cm",
     .help = "bypass contrast masking (CM)",
     .alias = "bcm",
     .offset = offsetof(FloatAdmStateSycl, adm_bypass_cm),
     .type = VMAF_OPT_TYPE_INT,
     .default_val = {.i = 0},
     .min = 0,
     .max = 1,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
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
    {.name = "adm_f1s0",
     .help = "factor1 scale0",
     .alias = "f1s0",
     .offset = offsetof(FloatAdmStateSycl, adm_f1s0),
     .type = VMAF_OPT_TYPE_DOUBLE,
     .default_val = {.d = -1.0},
     .min = -1.0,
     .max = 10.0,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_f1s1",
     .help = "factor1 scale1",
     .alias = "f1s1",
     .offset = offsetof(FloatAdmStateSycl, adm_f1s1),
     .type = VMAF_OPT_TYPE_DOUBLE,
     .default_val = {.d = -1.0},
     .min = -1.0,
     .max = 10.0,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_f1s2",
     .help = "factor1 scale2",
     .alias = "f1s2",
     .offset = offsetof(FloatAdmStateSycl, adm_f1s2),
     .type = VMAF_OPT_TYPE_DOUBLE,
     .default_val = {.d = -1.0},
     .min = -1.0,
     .max = 10.0,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_f1s3",
     .help = "factor1 scale3",
     .alias = "f1s3",
     .offset = offsetof(FloatAdmStateSycl, adm_f1s3),
     .type = VMAF_OPT_TYPE_DOUBLE,
     .default_val = {.d = -1.0},
     .min = -1.0,
     .max = 10.0,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_f2s0",
     .help = "factor2 scale0",
     .alias = "f2s0",
     .offset = offsetof(FloatAdmStateSycl, adm_f2s0),
     .type = VMAF_OPT_TYPE_DOUBLE,
     .default_val = {.d = -1.0},
     .min = -1.0,
     .max = 10.0,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_f2s1",
     .help = "factor2 scale1",
     .alias = "f2s1",
     .offset = offsetof(FloatAdmStateSycl, adm_f2s1),
     .type = VMAF_OPT_TYPE_DOUBLE,
     .default_val = {.d = -1.0},
     .min = -1.0,
     .max = 10.0,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_f2s2",
     .help = "factor2 scale2",
     .alias = "f2s2",
     .offset = offsetof(FloatAdmStateSycl, adm_f2s2),
     .type = VMAF_OPT_TYPE_DOUBLE,
     .default_val = {.d = -1.0},
     .min = -1.0,
     .max = 10.0,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_f2s3",
     .help = "factor2 scale3",
     .alias = "f2s3",
     .offset = offsetof(FloatAdmStateSycl, adm_f2s3),
     .type = VMAF_OPT_TYPE_DOUBLE,
     .default_val = {.d = -1.0},
     .min = -1.0,
     .max = 10.0,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_skip_aim_scale",
     .help = "when set, skip AIM calculations for that scale",
     .alias = "sasc",
     .offset = offsetof(FloatAdmStateSycl, adm_skip_aim_scale),
     .type = VMAF_OPT_TYPE_INT,
     .default_val = {.i = -1},
     .min = 0,
     .max = 3,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_skip_scale0",
     .help = "skip the calculation of scale 0",
     .alias = "ssz",
     .offset = offsetof(FloatAdmStateSycl, adm_skip_scale0),
     .type = VMAF_OPT_TYPE_BOOL,
     .default_val = {.b = false},
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

    /* Below 17x17 the scale-3 bands have one sample; the CPU float_adm
     * refuses such frames and so does the twin. Before any device resource. */
    const int size_error = adm_frame_size_check("float_adm_sycl", w, h);
    if (size_error)
        return size_error;
    if (s->adm_csf_mode != 0)
        return -EINVAL;

    s->width = w;
    s->height = h;
    s->bpc = bpc;
    s->has_pending = false;
    fadm_configure_dimensions(s, w, h);
    fadm_init_reference(s);

    if (!fex->sycl_state)
        return -EINVAL;
    s->sycl_state = fex->sycl_state;
    fadm_allocate_raw_and_dwt(s);
    fadm_allocate_bands_and_csf(s);
    fadm_allocate_sums(s);
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
    if (!raw_stride)
        return -EIO;
    const float scaler = fadm_pixel_scaler(s->bpc);

    for (int scale = 0; scale < FADM_NUM_SCALES; scale++) {
        fadm_launch_scale(q, s, scale, raw_stride, scaler);
    }

    /* The only device-to-host copy of the frame; collect() waits on it. */
    q.memcpy(s->h_rows, s->d_rows, s->row_floats * sizeof(float));

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
    FadmPooledScores pooled = fadm_pool_scores(s);
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
        }
        void *const sums[] = {s->d_csf_a, s->d_csf_fa, s->d_csf_r, s->d_csf_fr,
                              s->d_terms, s->d_rows,   s->h_rows};
        for (void *buffer : sums) {
            if (buffer)
                vmaf_sycl_free(s->sycl_state, buffer);
        }
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
                                                         "adm_scale0",
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
    .unsuffixed_debug_key = "adm",
};

} /* extern "C" */
