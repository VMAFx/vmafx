/**
 *  Copyright 2016-2020 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2 AND BSD-2-Clause-Patent
 *
 *  The host side of integer_adm_metal that does not touch the Metal API
 *  (integer_adm_metal_host.h). Every fixed-point term and every score formula
 *  comes from integer_adm.c's own contexts and helpers in
 *  integer_adm_kernels.h, adm_score.h and nonfinite_score.h
 *  (T-METAL-INTEGER-ADM-TWIN-DEFECTS-2026-10-05; integer_adm_cuda.c does the
 *  same, ADR-1416).
 */

#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "integer_adm_metal_host.h"

#include "../adm_gain_limit.h"
#include "../adm_score.h"
#include "../integer_adm_kernels.h"
#include "../nonfinite_score.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy proposes `nullptr`, but the required MSVC C build does
 * not provide that keyword. Preserve the portable C spelling. ADR-1138. */

#define IADM_METAL_TILE 16u
#define IADM_METAL_ROW_THREADS 256u

static const char *const IADM_METAL_KERNEL_NAMES[IADM_METAL_KERNEL_COUNT] = {
    "integer_adm_dwt_vert_8bpc",   "integer_adm_dwt_vert_16bpc",    "integer_adm_dwt_vert_s1",
    "integer_adm_dwt_vert_s123",   "integer_adm_dwt_hori_s0",       "integer_adm_dwt_hori_s123",
    "integer_adm_decouple_csf_s0", "integer_adm_decouple_csf_s123", "integer_adm_csf_cm_s0",
    "integer_adm_csf_cm_s123",     "integer_adm_aim_cm_s0",         "integer_adm_aim_cm_s123",
};

const char *iadm_metal_kernel_name(IadmMetalKernel entry)
{
    if ((unsigned)entry >= (unsigned)IADM_METAL_KERNEL_COUNT) {
        return NULL;
    }
    return IADM_METAL_KERNEL_NAMES[entry];
}

void iadm_metal_geometry(IadmMetalGeometry *g, unsigned w, unsigned h, unsigned bpc)
{
    assert(g != NULL);
    assert(w > 0u && h > 0u);
    (void)memset(g, 0, sizeof(*g));
    g->w = w;
    g->h = h;
    g->bpc = bpc;
    unsigned cur_w = w;
    unsigned cur_h = h;
    for (int scale = 0; scale < IADM_METAL_NUM_SCALES; ++scale) {
        const unsigned half_w = (cur_w + 1u) / 2u;
        const unsigned half_h = (cur_h + 1u) / 2u;
        g->scale_w[scale] = cur_w;
        g->scale_h[scale] = cur_h;
        g->half_w[scale] = half_w;
        g->half_h[scale] = half_h;
        const AdmBorder b = adm_border((int)half_w, (int)half_h);
        const int rows = b.bottom - b.top;
        g->wg_count[scale] = 3u * (rows > 0 ? (unsigned)rows : 1u);
        cur_w = half_w;
        cur_h = half_h;
    }
    g->buf_stride = (g->half_w[0] + 3u) & ~3u;
}

size_t iadm_metal_buffer_bytes(const IadmMetalGeometry *g, IadmMetalBuffer buffer, int scale)
{
    assert(g != NULL);
    assert(g->buf_stride >= g->half_w[0]);
    if (scale < 0 || scale >= IADM_METAL_NUM_SCALES) {
        return 0u;
    }
    switch (buffer) {
    case IADM_METAL_BUF_SOURCE:
        return (size_t)g->w * g->h * (g->bpc <= 8u ? 1u : 2u);
    case IADM_METAL_BUF_DWT_TMP:
        return (size_t)g->w * 2u * g->half_h[0] * sizeof(int32_t);
    case IADM_METAL_BUF_BAND:
        /* int16 bands at scale 0, int32 at scales 1-3, as integer_adm.c. */
        return (size_t)4u * g->buf_stride * g->half_h[scale] *
               (scale == 0 ? sizeof(int16_t) : sizeof(int32_t));
    case IADM_METAL_BUF_CSF:
        return (size_t)3u * g->buf_stride * g->half_h[0] * sizeof(int32_t);
    case IADM_METAL_BUF_ACCUM:
        return (size_t)vmaf_mtl_iadm_accum_word(g->wg_count[scale], 0u, 0u) * sizeof(uint32_t);
    }
    return 0u;
}

int iadm_metal_check_options(const IadmMetalOptions *o)
{
    assert(o != NULL);
    const int geom_err =
        adm_viewing_geometry_check("adm_metal", o->adm_norm_view_dist, o->adm_ref_display_height);
    if (geom_err) {
        return geom_err;
    }

    for (int scale = 0; scale < IADM_METAL_NUM_SCALES; ++scale) {
        const AdmCsfFactors f =
            adm_csf_factors(scale, o->adm_norm_view_dist, o->adm_ref_display_height,
                            o->adm_csf_mode, o->adm_csf_scale, o->adm_csf_diag_scale);
        const float rfactor1[3] = {f.factor1, f.factor1, f.factor2};
        const int err = adm_csf_check_scale(scale, rfactor1, o->adm_norm_view_dist,
                                            o->adm_ref_display_height, o->adm_csf_mode);
        if (err) {
            return err;
        }
    }
    return 0;
}

static void iadm_uniform_dims(const IadmMetalGeometry *g, int scale, IadmDims *d)
{
    (void)memset(d, 0, sizeof(*d));
    d->scale = scale;
    d->cur_w = (int)g->scale_w[scale];
    d->cur_h = (int)g->scale_h[scale];
    d->half_w = (int)g->half_w[scale];
    d->half_h = (int)g->half_h[scale];
    d->buf_stride = (int)g->buf_stride;
    d->parent_buf_stride = (int)g->buf_stride;
    d->bpc = g->bpc;
}

/* The DWT rounding: adm_dwt2_vpass_8() / adm_dwt2_vpass_16() and
 * adm_dwt2_hpass() at scale 0, i4_dwt2_round() at scales 1-3. */
static void iadm_uniform_dwt(const IadmMetalGeometry *g, int scale, IadmCsf *c)
{
    const int inp_bits = (g->bpc <= 8u) ? 8 : (int)g->bpc;
    c->v_shift = inp_bits;
    c->v_add_shift = 1 << (inp_bits - 1);
    c->h_shift = 16;
    c->h_add_shift = 32768;
    if (scale > 0) {
        const I4Dwt2Round r = i4_dwt2_round(scale);
        c->s123_vert_add = r.add_vp;
        c->s123_vert_shift = r.shift_vp;
        c->s123_hori_add = r.add_hp;
        c->s123_hori_shift = r.shift_hp;
    }
}

static void iadm_uniform_cm_band(IadmCsf *c, int band, const AdmCmBand *p)
{
    c->cm_shift_sub[band] = p->shift_sub;
    c->cm_add_shift_sq[band] = p->add_shift_sq;
    c->cm_shift_sq[band] = p->shift_sq;
    c->cm_add_shift_cub[band] = p->add_shift_cub;
    c->cm_shift_cub[band] = p->shift_cub;
}

/* Scale 0: adm_cm_ctx_init() and adm_csf_den_ctx_init(). */
static void iadm_uniform_scale0(const IadmMetalOptions *o, int w, int h, IadmCsf *c)
{
    AdmBuffer no_planes;
    (void)memset(&no_planes, 0, sizeof(no_planes));
    AdmCmCtx cm;
    adm_cm_ctx_init(&cm, &no_planes, w, h, 0, 0, o->adm_norm_view_dist, o->adm_ref_display_height,
                    o->adm_csf_mode, o->adm_csf_scale, o->adm_csf_diag_scale, false);
    c->i_rfactor_h = cm.i_rfactor[0];
    c->i_rfactor_v = cm.i_rfactor[1];
    c->i_rfactor_d = cm.i_rfactor[2];
    for (int band = 0; band < 3; ++band) {
        iadm_uniform_cm_band(c, band, &cm.band[band]);
    }
    c->cm_shift_inner = cm.shift_inner_accum;
    c->cm_add_shift_inner = cm.add_shift_inner_accum;

    AdmDenCtx den;
    adm_csf_den_ctx_init(&den, w, h, o->adm_norm_view_dist, o->adm_ref_display_height,
                         o->adm_csf_mode, o->adm_csf_scale, o->adm_csf_diag_scale);
    c->den_shift_accum = (uint32_t)den.shift_accum;
    c->den_add_shift_accum = (uint32_t)den.add_shift_accum;
}

/* Scales 1-3: i4_adm_cm_ctx_init() and i4_adm_csf_den_ctx_init(). */
static void iadm_uniform_s123(const IadmMetalOptions *o, int scale, int w, int h, IadmCsf *c)
{
    AdmBuffer no_planes;
    (void)memset(&no_planes, 0, sizeof(no_planes));
    I4AdmCmCtx cm;
    i4_adm_cm_ctx_init(&cm, &no_planes, w, h, 0, 0, scale, o->adm_norm_view_dist,
                       o->adm_ref_display_height, o->adm_csf_mode, o->adm_csf_scale,
                       o->adm_csf_diag_scale, false);
    c->i_rfactor_h = cm.rfactor[0];
    c->i_rfactor_v = cm.rfactor[1];
    c->i_rfactor_d = cm.rfactor[2];
    for (int band = 0; band < 3; ++band) {
        iadm_uniform_cm_band(c, band, &cm.band);
    }
    c->cm_shift_inner = cm.shift_inner_accum;
    c->cm_add_shift_inner = cm.add_shift_inner_accum;
    c->i4_add_shift_dst = cm.add_bef_shift_dst;
    c->i4_shift_dst = cm.shift_dst;
    c->i4_add_shift_flt = cm.add_bef_shift_flt;
    c->i4_shift_flt = cm.shift_flt;

    I4AdmDenCtx den;
    i4_adm_csf_den_ctx_init(&den, scale, w, h, o->adm_norm_view_dist, o->adm_ref_display_height,
                            o->adm_csf_mode, o->adm_csf_scale, o->adm_csf_diag_scale);
    c->den_shift_sq = den.shift_sq;
    c->den_add_shift_sq = den.add_shift_sq;
    c->den_shift_cub = den.shift_cub;
    c->den_add_shift_cub = den.add_shift_cub;
    c->den_shift_accum = den.shift_accum;
    c->den_add_shift_accum = den.add_shift_accum;
}

void iadm_metal_uniforms(const IadmMetalOptions *o, const IadmMetalGeometry *g, int scale,
                         IadmDims *d, IadmCsf *c)
{
    iadm_uniform_dims(g, scale, d);
    (void)memset(c, 0, sizeof(*c));
    const int w = (int)g->half_w[scale];
    const int h = (int)g->half_h[scale];
    const AdmBorder b = adm_border(w, h);
    c->active_left = b.left;
    c->active_top = b.top;
    c->active_right = b.right;
    c->active_bottom = b.bottom;
    const struct AdmGainLimit gain = adm_gain_limit_split(o->adm_enhn_gain_limit);
    c->gain_m_hi = gain.m_hi;
    c->gain_m_lo = gain.m_lo;
    c->gain_frac_bits = gain.frac_bits;
    iadm_uniform_dwt(g, scale, c);
    if (scale == 0) {
        iadm_uniform_scale0(o, w, h, c);
    } else {
        iadm_uniform_s123(o, scale, w, h, c);
    }
}

static IadmMetalStage iadm_stage_tiles(IadmMetalKernel entry, unsigned w, unsigned h,
                                       unsigned planes)
{
    const IadmMetalStage s = {
        .entry = entry,
        .groups = {(w + IADM_METAL_TILE - 1u) / IADM_METAL_TILE,
                   (h + IADM_METAL_TILE - 1u) / IADM_METAL_TILE, planes},
        .threads = {IADM_METAL_TILE, IADM_METAL_TILE, 1u},
    };
    return s;
}

static IadmMetalStage iadm_stage_rows(IadmMetalKernel entry, unsigned wg_count)
{
    const IadmMetalStage s = {
        .entry = entry,
        .groups = {wg_count, 1u, 1u},
        .threads = {IADM_METAL_ROW_THREADS, 1u, 1u},
    };
    return s;
}

static IadmMetalKernel iadm_vertical_kernel(const IadmMetalGeometry *g, int scale)
{
    if (scale == 0) {
        return g->bpc <= 8u ? IADM_METAL_DWT_VERT_8BPC : IADM_METAL_DWT_VERT_16BPC;
    }
    /* Scale 1 reads the int16 band a of scale 0 (i16_to_i32()). */
    return scale == 1 ? IADM_METAL_DWT_VERT_S1 : IADM_METAL_DWT_VERT_S123;
}

unsigned iadm_metal_stages(const IadmMetalOptions *o, const IadmMetalGeometry *g, int scale,
                           IadmMetalStage stages[IADM_METAL_MAX_STAGES])
{
    const bool s0 = (scale == 0);
    const unsigned half_w = g->half_w[scale];
    const unsigned half_h = g->half_h[scale];
    unsigned n = 0u;
    stages[n++] = iadm_stage_tiles(iadm_vertical_kernel(g, scale), g->scale_w[scale], half_h, 2u);
    stages[n++] = iadm_stage_tiles(s0 ? IADM_METAL_DWT_HORI_S0 : IADM_METAL_DWT_HORI_S123, half_w,
                                   half_h, 2u);
    /* adm_skip_scale0: the CPU runs only the DWT at scale 0. */
    if (s0 && o->adm_skip_scale0) {
        return n;
    }
    stages[n++] = iadm_stage_tiles(s0 ? IADM_METAL_DECOUPLE_CSF_S0 : IADM_METAL_DECOUPLE_CSF_S123,
                                   half_w, half_h, 1u);
    stages[n++] =
        iadm_stage_rows(s0 ? IADM_METAL_CSF_CM_S0 : IADM_METAL_CSF_CM_S123, g->wg_count[scale]);
    if (!o->adm_skip_aim) {
        stages[n++] =
            iadm_stage_rows(s0 ? IADM_METAL_AIM_CM_S0 : IADM_METAL_AIM_CM_S123, g->wg_count[scale]);
    }
    return n;
}

static bool iadm_is_dwt(IadmMetalKernel entry)
{
    return entry == IADM_METAL_DWT_VERT_8BPC || entry == IADM_METAL_DWT_VERT_16BPC ||
           entry == IADM_METAL_DWT_VERT_S1 || entry == IADM_METAL_DWT_VERT_S123 ||
           entry == IADM_METAL_DWT_HORI_S0 || entry == IADM_METAL_DWT_HORI_S123;
}

unsigned iadm_metal_view_stages(const IadmMetalOptions *o, const IadmMetalGeometry *g, int scale,
                                unsigned view, IadmMetalStage stages[IADM_METAL_MAX_STAGES])
{
    IadmMetalStage all[IADM_METAL_MAX_STAGES];
    const unsigned count = iadm_metal_stages(o, g, scale, all);
    unsigned n = 0u;
    for (unsigned i = 0u; i < count; ++i) {
        if (view == 0u || !iadm_is_dwt(all[i].entry)) {
            stages[n++] = all[i];
        }
    }
    return n;
}

static uint64_t iadm_slot(const uint32_t *accum, unsigned wg, unsigned slot)
{
    return ((uint64_t)accum[vmaf_mtl_iadm_accum_word(wg, slot, 1u)] << 32) |
           (uint64_t)accum[vmaf_mtl_iadm_accum_word(wg, slot, 0u)];
}

/* The three bands' sums of one scale over its reduction rows. */
typedef struct IadmScaleSums {
    uint64_t den[3];
    uint64_t cm[3]; /* every term is non-negative; a scale-0 sum can pass INT64_MAX */
    uint64_t aim[3];
} IadmScaleSums;

static IadmScaleSums iadm_scale_sums(const uint32_t *accum, unsigned wg_count)
{
    IadmScaleSums t;
    (void)memset(&t, 0, sizeof(t));
    for (unsigned wg = 0u; wg < wg_count; ++wg) {
        for (unsigned band = 0u; band < 3u; ++band) {
            t.den[band] += iadm_slot(accum, wg, VMAF_MTL_IADM_SLOT_DEN + band);
            t.cm[band] += iadm_slot(accum, wg, VMAF_MTL_IADM_SLOT_CM + band);
            t.aim[band] += iadm_slot(accum, wg, VMAF_MTL_IADM_SLOT_AIM + band);
        }
    }
    return t;
}

/* adm_cm_result() / i4_adm_cm_result() on the CPU's context of the scale. */
static float iadm_cm_result(const IadmMetalOptions *o, int scale, int w, int h,
                            const uint64_t accum[3], double noise_weight)
{
    AdmBuffer no_planes;
    (void)memset(&no_planes, 0, sizeof(no_planes));
    const AdmCmBounds bd = adm_cm_bounds(w, h);
    if (scale == 0) {
        AdmCmCtx c;
        adm_cm_ctx_init(&c, &no_planes, w, h, 0, 0, o->adm_norm_view_dist,
                        o->adm_ref_display_height, o->adm_csf_mode, o->adm_csf_scale,
                        o->adm_csf_diag_scale, false);
        return adm_cm_result(&c, &bd, accum, noise_weight, o->adm_p_norm);
    }
    I4AdmCmCtx c;
    i4_adm_cm_ctx_init(&c, &no_planes, w, h, 0, 0, scale, o->adm_norm_view_dist,
                       o->adm_ref_display_height, o->adm_csf_mode, o->adm_csf_scale,
                       o->adm_csf_diag_scale, false);
    /* Scales 1-3 stay below 2^63 (accumulator bounds). */
    const int64_t sums[3] = {(int64_t)accum[0], (int64_t)accum[1], (int64_t)accum[2]};
    return i4_adm_cm_result(&c, &bd, sums, noise_weight, o->adm_p_norm);
}

/* adm_csf_den_result() / i4_adm_csf_den_result(). */
static float iadm_den_result(const IadmMetalOptions *o, int scale, int w, int h,
                             const uint64_t accum[3])
{
    if (scale == 0) {
        AdmDenCtx c;
        adm_csf_den_ctx_init(&c, w, h, o->adm_norm_view_dist, o->adm_ref_display_height,
                             o->adm_csf_mode, o->adm_csf_scale, o->adm_csf_diag_scale);
        return adm_csf_den_result(&c, accum, o->adm_noise_weight);
    }
    I4AdmDenCtx c;
    i4_adm_csf_den_ctx_init(&c, scale, w, h, o->adm_norm_view_dist, o->adm_ref_display_height,
                            o->adm_csf_mode, o->adm_csf_scale, o->adm_csf_diag_scale);
    return i4_adm_csf_den_result(&c, accum, o->adm_noise_weight);
}

/* integer_adm_scale0() / integer_adm_scale_s123(): numerator, denominator
 * and AIM numerator of one scale, each the float the CPU keeps. */
static void iadm_scale_scores(const IadmMetalOptions *o, const IadmMetalGeometry *g, int scale,
                              const uint32_t *accum, float out[3])
{
    out[0] = 0.0f;
    out[1] = 0.0f;
    out[2] = 0.0f;
    if (scale == 0 && o->adm_skip_scale0) {
        out[1] = 1e-10f; /* the CPU's den = 1e-10, no numerator, no AIM */
        return;
    }
    const int w = (int)g->half_w[scale];
    const int h = (int)g->half_h[scale];
    const IadmScaleSums t = iadm_scale_sums(accum, g->wg_count[scale]);
    out[0] = iadm_cm_result(o, scale, w, h, t.cm, o->adm_noise_weight);
    out[1] = iadm_den_result(o, scale, w, h, t.den);
    if (!o->adm_skip_aim) {
        out[2] = iadm_cm_result(o, scale, w, h, t.aim, 0.0);
    }
}

/* adm_result_finalise() and the tail of extract() in integer_adm.c. */
static int iadm_finalise(const IadmMetalOptions *o, const IadmMetalGeometry *g, double num,
                         double den, double aim_num, unsigned index, IadmMetalScores *out)
{
    const double numden_limit = 1e-10 * (int)(g->w * g->h) / (1920.0 * 1080.0);
    int err =
        vmaf_adm_floor_pair_named("integer_adm_metal", index, num, den, numden_limit, &num, &den);
    if (err) {
        return err;
    }
    const double pairs[4] = {num, den, aim_num, den};
    double ratios[2];
    err = vmaf_adm_scale_ratios(pairs, 2u, ratios);
    if (err) {
        vmaf_log(VMAF_LOG_LEVEL_WARNING,
                 "integer_adm_metal: undefined or non-finite aggregate at frame %u "
                 "(num=%g den=%g aim_num=%g)\n",
                 index, num, den, aim_num);
        return err;
    }
    out->score = ratios[0];
    out->score_aim = ratios[1];
    out->score_num = num;
    out->score_den = den;
    err = vmaf_adm3_score_named("integer_adm_metal", index, out->score, out->score_aim, 0,
                                o->adm_dlm_weight, o->adm_min_val, &out->score_adm3);
    if (err) {
        return err;
    }
    return vmaf_adm_scale_ratios_named("integer_adm_metal", index, out->scores,
                                       IADM_METAL_NUM_SCALES, out->scale_scores);
}

int iadm_metal_scores(const IadmMetalOptions *o, const IadmMetalGeometry *g,
                      const uint32_t *const accum[IADM_METAL_NUM_SCALES], unsigned index,
                      IadmMetalScores *out)
{
    (void)memset(out, 0, sizeof(*out));
    double num = 0.0;
    double den = 0.0;
    double aim_num = 0.0;
    for (int scale = 0; scale < IADM_METAL_NUM_SCALES; ++scale) {
        float s[3];
        iadm_scale_scores(o, g, scale, accum[scale], s);
        num += s[0];
        den += s[1];
        aim_num += s[2];
        const size_t at = 2u * (size_t)scale;
        out->scores[at] = s[0];
        out->scores[at + 1u] = s[1];
    }
    return iadm_finalise(o, g, num, den, aim_num, index, out);
}

/* NOLINTEND(modernize-use-nullptr) */
