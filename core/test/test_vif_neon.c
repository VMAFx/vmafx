/**
 * Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 * NEON-vs-scalar bit-exactness for the integer VIF kernels.
 *
 * VIF is one of the two heavyweight VMAF features (the other is ADM), and
 * `core/src/feature/arm64/vif_neon.c` replaces *all four* scalar entry points
 * unconditionally: `integer_vif.c`'s dispatcher installs the NEON variants
 * whenever `VMAF_ARM_CPU_FLAG_NEON` is set, with no width guard at all.  Every
 * kernel therefore owns its own tail handling for arbitrary `w`, and nothing in
 * the suite checked that before this file.
 *
 * The tests here run each NEON kernel against the scalar reference over a set of
 * geometries chosen to straddle the vector strides in use (16 columns for the
 * `subsample_rd` kernels, 8 for the `vif_statistic` kernels), plus data patterns
 * that drive both arms of the `sigma1_sq >= sigma_nsq` branch:
 *
 *   - `PATTERN_NOISE` keeps `sigma1_sq` far above `sigma_nsq`, so every pixel
 *     takes the log branch.
 *   - `PATTERN_FLAT_STEP` holds the reference constant (`sigma1_sq == 0`), so
 *     every pixel takes the *non-log* branch, where the scalar clamps
 *     `sigma2_sq` to zero before accumulating it.
 *
 * Two real divergences were found in `vif_statistic_8_neon` and fixed alongside
 * this file; both are re-checked here:
 *
 *   1. The horizontal pass stepped 8 columns while stopping at `w - 7` and had
 *      no tail, so the last `w % 8` columns of every row contributed nothing to
 *      num/den (and for `w <= 7`, *no* column did).  `vif_statistic_16_neon`,
 *      `vif_statistic_8_avx2` and `vif_statistic_8_avx512` all close that gap
 *      with `vif_compute_line_residuals()`; the NEON 8-bit kernel did not.
 *   2. The per-lane epilogue read `sigma2_sq` straight out of the subtraction
 *      without the scalar's `MAX(sigma2_sq, 0)`, so a negative `sigma2_sq` was
 *      accumulated verbatim into `accum_num_non_log` instead of as zero.  Again
 *      the 16-bit NEON kernel (`vmaxq_s32`) and AVX2 (`_mm256_max_epi32`) get
 *      this right.
 */

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "test.h"

#include "mem.h"

#include "feature/integer_vif.h"

#if ARCH_AARCH64
#include "feature/arm64/vif_neon.h"

/* ---------------------------------------------------------------------- */
/* Buffer plumbing — a byte-for-byte replica of the allocation `init()` in
 * integer_vif.c performs, because the NEON kernels rely on the exact same
 * negative-index padding slack in front of every `buf.tmp.*` cursor. */
/* ---------------------------------------------------------------------- */

typedef struct VifTestCtx {
    VifPublicState *s;
    uint8_t *data;
} VifTestCtx;

static size_t vif_ctx_data_size(const VifBuffer *buf, unsigned h)
{
    const size_t frame_size = (size_t)buf->stride * h;
    const size_t pad_size = (size_t)buf->stride * 8;
    return 2 * (pad_size + frame_size + pad_size) + 2 * ((size_t)h * (size_t)buf->stride_16) +
           5 * (size_t)buf->stride_32 + 7 * (size_t)buf->stride_tmp;
}

static void vif_ctx_free(VifTestCtx *ctx)
{
    free(ctx->data);
    free(ctx->s);
    ctx->data = NULL;
    ctx->s = NULL;
}

/* Mirrors log_generate() in integer_vif.c (static there).  The `(double)` cast
 * makes the promotion `round()` performs explicit; the value is unchanged. */
static void ref_log_generate(uint16_t *log2_table)
{
    for (unsigned i = 0; i < VIF_LOG2_TABLE_SIZE; ++i) {
        log2_table[i] = (uint16_t)round((double)(log2f((float)(VIF_LOG2_TABLE_OFFSET + i)) * 2048));
    }
}

/* Hands each `VifBuffer` cursor its slice of the single allocation, in the same
 * order and at the same byte offsets as integer_vif.c's `init()`. */
static void vif_ctx_carve(VifBuffer *buf, uint8_t *base, unsigned h)
{
    const size_t frame_size = (size_t)buf->stride * h;
    const size_t pad_size = (size_t)buf->stride * 8;
    uint8_t *data = base;

    buf->data = data;
    data += pad_size;
    buf->ref = data;
    data += frame_size + pad_size + pad_size;
    buf->dis = data;
    data += frame_size + pad_size;
    buf->mu1 = (uint16_t *)data;
    data += (size_t)h * (size_t)buf->stride_16;
    buf->mu2 = (uint16_t *)data;
    data += (size_t)h * (size_t)buf->stride_16;
    buf->mu1_32 = (uint32_t *)data;
    data += buf->stride_32;
    buf->mu2_32 = (uint32_t *)data;
    data += buf->stride_32;
    buf->ref_sq = (uint32_t *)data;
    data += buf->stride_32;
    buf->dis_sq = (uint32_t *)data;
    data += buf->stride_32;
    buf->ref_dis = (uint32_t *)data;
    data += buf->stride_32;
    buf->tmp.mu1 = (uint32_t *)data;
    data += buf->stride_tmp;
    buf->tmp.mu2 = (uint32_t *)data;
    data += buf->stride_tmp;
    buf->tmp.ref = (uint32_t *)data;
    data += buf->stride_tmp;
    buf->tmp.dis = (uint32_t *)data;
    data += buf->stride_tmp;
    buf->tmp.ref_dis = (uint32_t *)data;
    data += buf->stride_tmp;
    buf->tmp.ref_convol = (uint32_t *)data;
    data += buf->stride_tmp;
    buf->tmp.dis_convol = (uint32_t *)data;
}

static bool vif_ctx_alloc(VifTestCtx *ctx, unsigned w, unsigned h, bool hbd)
{
    ctx->s = calloc(1, sizeof(*ctx->s));
    if (!ctx->s) {
        return false;
    }

    VifBuffer *buf = &ctx->s->buf;
    buf->stride = ALIGN_CEIL(w << (hbd ? 1 : 0));
    buf->stride_16 = ALIGN_CEIL(w * (int)sizeof(uint16_t));
    buf->stride_32 = ALIGN_CEIL(w * (int)sizeof(uint32_t));
    buf->stride_tmp = ALIGN_CEIL((MAX_ALIGN + (int)w + MAX_ALIGN) * (int)sizeof(uint32_t));

    ctx->data = calloc(vif_ctx_data_size(buf, h), 1);
    if (!ctx->data) {
        free(ctx->s);
        ctx->s = NULL;
        return false;
    }

    vif_ctx_carve(buf, ctx->data, h);
    ctx->s->vif_enhn_gain_limit = DEFAULT_VIF_ENHN_GAIN_LIMIT;
    ref_log_generate(ctx->s->log2_table);
    return true;
}

/* Bit-exactness is the contract, so compare the float object representations
 * rather than the values: `num` and `den` are both derived from int64
 * accumulators, so equal inputs must produce identical bit patterns. */
static bool float_bits_equal(float a, float b)
{
    uint32_t abits;
    uint32_t bbits;
    memcpy(&abits, &a, sizeof(abits));
    memcpy(&bbits, &b, sizeof(bbits));
    return abits == bbits;
}

/* Mirrors pad_top_and_bottom() in integer_vif.c (static there). */
static void ref_pad_top_and_bottom(const VifBuffer *buf, unsigned h, int fwidth)
{
    const unsigned fwidth_half = (unsigned)fwidth / 2;
    uint8_t *ref = buf->ref;
    uint8_t *dis = buf->dis;
    for (unsigned i = 1; i <= fwidth_half; ++i) {
        const size_t offset = (size_t)buf->stride * i;
        memcpy(ref - offset, ref + offset, (size_t)buf->stride);
        memcpy(dis - offset, dis + offset, (size_t)buf->stride);
        memcpy(ref + (size_t)buf->stride * (h - 1) + offset,
               ref + (size_t)buf->stride * (h - 1) - offset, (size_t)buf->stride);
        memcpy(dis + (size_t)buf->stride * (h - 1) + offset,
               dis + (size_t)buf->stride * (h - 1) - offset, (size_t)buf->stride);
    }
}

/* ---------------------------------------------------------------------- */
/* Input patterns                                                          */
/* ---------------------------------------------------------------------- */

enum {
    PATTERN_NOISE = 0,       /* wide-band: every pixel takes the log branch */
    PATTERN_FLAT_STEP = 1,   /* ref constant => sigma1_sq == 0 => non-log branch */
    PATTERN_LOW_CONTRAST = 2 /* small-amplitude noise near the branch boundary */
};

static uint32_t xs32(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static void fill_8(const VifBuffer *buf, unsigned w, unsigned h, int pattern, uint32_t seed)
{
    uint8_t *ref = buf->ref;
    uint8_t *dis = buf->dis;
    uint32_t st = seed | 1u;

    for (unsigned i = 0; i < h; ++i) {
        for (unsigned j = 0; j < w; ++j) {
            uint8_t r;
            uint8_t d;
            switch (pattern) {
            case PATTERN_FLAT_STEP:
                /* A flat reference pins sigma1_sq to exactly 0, so every output
                 * pixel lands in the `else` arm that accumulates sigma2_sq.  The
                 * sparse +1 rows in `dis` bias the mu2 rounding upward hard
                 * enough that yy_filt - mu2_sq goes negative — precisely the
                 * case the scalar clamps with MAX(sigma2_sq, 0). */
                r = 200;
                d = (uint8_t)(200 + (((i % 5u) == 0u) ? 1 : 0));
                break;
            case PATTERN_LOW_CONTRAST:
                r = (uint8_t)(100 + (xs32(&st) & 3u));
                d = (uint8_t)(100 + (xs32(&st) & 3u));
                break;
            default:
                r = (uint8_t)(xs32(&st) & 0xFFu);
                d = (uint8_t)(xs32(&st) & 0xFFu);
                break;
            }
            ref[i * (size_t)buf->stride + j] = r;
            dis[i * (size_t)buf->stride + j] = d;
        }
    }
    ref_pad_top_and_bottom(buf, h, vif_filter1d_width[0]);
}

static void fill_16(const VifBuffer *buf, unsigned w, unsigned h, int pattern, int bpc,
                    uint32_t seed)
{
    uint16_t *ref = buf->ref;
    uint16_t *dis = buf->dis;
    const ptrdiff_t stride = buf->stride / (ptrdiff_t)sizeof(uint16_t);
    const uint32_t mask = (bpc >= 16) ? 0xFFFFu : ((1u << bpc) - 1u);
    const uint32_t mid = (mask + 1u) * 3u / 4u;
    uint32_t st = seed | 1u;

    for (unsigned i = 0; i < h; ++i) {
        for (unsigned j = 0; j < w; ++j) {
            uint16_t r;
            uint16_t d;
            switch (pattern) {
            case PATTERN_FLAT_STEP:
                r = (uint16_t)mid;
                d = (uint16_t)(mid + (((i % 5u) == 0u) ? 1u : 0u));
                break;
            case PATTERN_LOW_CONTRAST:
                r = (uint16_t)(mid + (xs32(&st) & 3u));
                d = (uint16_t)(mid + (xs32(&st) & 3u));
                break;
            default:
                r = (uint16_t)(xs32(&st) & mask);
                d = (uint16_t)(xs32(&st) & mask);
                break;
            }
            ref[i * stride + j] = r;
            dis[i * stride + j] = d;
        }
    }
    ref_pad_top_and_bottom(buf, h, vif_filter1d_width[0]);
}

/* ---------------------------------------------------------------------- */
/* Scalar references for the two `subsample_rd` kernels, which are static in
 * integer_vif.c and therefore unlinkable.  Transcribed verbatim.          */
/* ---------------------------------------------------------------------- */

static void ref_decimate_and_pad(const VifBuffer *buf, unsigned w, unsigned h, int scale)
{
    uint16_t *ref = buf->ref;
    uint16_t *dis = buf->dis;
    const ptrdiff_t stride = buf->stride / (ptrdiff_t)sizeof(uint16_t);
    const ptrdiff_t mu_stride = buf->stride_16 / (ptrdiff_t)sizeof(uint16_t);

    for (unsigned i = 0; i < h / 2; ++i) {
        for (unsigned j = 0; j < w / 2; ++j) {
            const ptrdiff_t src = (ptrdiff_t)(i * 2) * mu_stride + (ptrdiff_t)(j * 2);
            ref[i * stride + j] = buf->mu1[src];
            dis[i * stride + j] = buf->mu2[src];
        }
    }
    ref_pad_top_and_bottom(buf, h / 2, vif_filter1d_width[scale]);
}

static void ref_subsample_rd_8(const VifBuffer *buf, unsigned w, unsigned h)
{
    const unsigned fwidth = (unsigned)vif_filter1d_width[1];
    const uint16_t *vif_filt_s1 = vif_filter1d_table[1];

    for (unsigned i = 0; i < h; ++i) {
        for (unsigned j = 0; j < w; ++j) {
            uint32_t accum_ref = 0;
            uint32_t accum_dis = 0;
            for (unsigned fi = 0; fi < fwidth; ++fi) {
                const int ii = (int)i - (int)fwidth / 2;
                const int ii_check = ii + (int)fi;
                const uint16_t fcoeff = vif_filt_s1[fi];
                const uint8_t *ref = (const uint8_t *)buf->ref;
                const uint8_t *dis = (const uint8_t *)buf->dis;
                accum_ref += fcoeff * (uint32_t)ref[ii_check * buf->stride + j];
                accum_dis += fcoeff * (uint32_t)dis[ii_check * buf->stride + j];
            }
            buf->tmp.ref_convol[j] = (accum_ref + 128) >> 8;
            buf->tmp.dis_convol[j] = (accum_dis + 128) >> 8;
        }

        PADDING_SQ_DATA_2(buf, (int)w, fwidth / 2);

        for (unsigned j = 0; j < w; ++j) {
            uint32_t accum_ref = 0;
            uint32_t accum_dis = 0;
            for (unsigned fj = 0; fj < fwidth; ++fj) {
                const int jj = (int)j - (int)fwidth / 2;
                const int jj_check = jj + (int)fj;
                const uint16_t fcoeff = vif_filt_s1[fj];
                accum_ref += fcoeff * buf->tmp.ref_convol[jj_check];
                accum_dis += fcoeff * buf->tmp.dis_convol[jj_check];
            }
            const ptrdiff_t stride = buf->stride_16 / (ptrdiff_t)sizeof(uint16_t);
            buf->mu1[i * stride + j] = (uint16_t)((accum_ref + 32768) >> 16);
            buf->mu2[i * stride + j] = (uint16_t)((accum_dis + 32768) >> 16);
        }
    }
    ref_decimate_and_pad(buf, w, h, 0);
}

static void ref_subsample_rd_16(const VifBuffer *buf, unsigned w, unsigned h, int scale, int bpc)
{
    const unsigned fwidth = (unsigned)vif_filter1d_width[scale + 1];
    const uint16_t *vif_filt = vif_filter1d_table[scale + 1];
    int32_t add_shift_round_VP;
    int32_t shift_VP;

    if (scale == 0) {
        add_shift_round_VP = 1 << (bpc - 1);
        shift_VP = bpc;
    } else {
        add_shift_round_VP = 32768;
        shift_VP = 16;
    }

    for (unsigned i = 0; i < h; ++i) {
        for (unsigned j = 0; j < w; ++j) {
            uint32_t accum_ref = 0;
            uint32_t accum_dis = 0;
            for (unsigned fi = 0; fi < fwidth; ++fi) {
                const int ii = (int)i - (int)fwidth / 2;
                const int ii_check = ii + (int)fi;
                const uint16_t fcoeff = vif_filt[fi];
                const ptrdiff_t stride = buf->stride / (ptrdiff_t)sizeof(uint16_t);
                const uint16_t *ref = buf->ref;
                const uint16_t *dis = buf->dis;
                accum_ref += fcoeff * ((uint32_t)ref[ii_check * stride + j]);
                accum_dis += fcoeff * ((uint32_t)dis[ii_check * stride + j]);
            }
            buf->tmp.ref_convol[j] = (uint16_t)((accum_ref + add_shift_round_VP) >> shift_VP);
            buf->tmp.dis_convol[j] = (uint16_t)((accum_dis + add_shift_round_VP) >> shift_VP);
        }

        PADDING_SQ_DATA_2(buf, (int)w, fwidth / 2);

        for (unsigned j = 0; j < w; ++j) {
            uint32_t accum_ref = 0;
            uint32_t accum_dis = 0;
            for (unsigned fj = 0; fj < fwidth; ++fj) {
                const int jj = (int)j - (int)fwidth / 2;
                const int jj_check = jj + (int)fj;
                const uint16_t fcoeff = vif_filt[fj];
                accum_ref += fcoeff * ((uint32_t)buf->tmp.ref_convol[jj_check]);
                accum_dis += fcoeff * ((uint32_t)buf->tmp.dis_convol[jj_check]);
            }
            const ptrdiff_t stride = buf->stride_16 / (ptrdiff_t)sizeof(uint16_t);
            buf->mu1[i * stride + j] = (uint16_t)((accum_ref + 32768) >> 16);
            buf->mu2[i * stride + j] = (uint16_t)((accum_dis + 32768) >> 16);
        }
    }
    ref_decimate_and_pad(buf, w, h, scale);
}

/* ---------------------------------------------------------------------- */
/* Comparators                                                             */
/* ---------------------------------------------------------------------- */

/* Prints the first few mismatches only; `bad` is the running count. */
static void report_pair(const char *tag, const char *what, int i, unsigned j, int bad, unsigned sa,
                        unsigned sb, unsigned na, unsigned nb)
{
    if (bad < 6) {
        (void)fprintf(stderr, "    %s %s[%d][%u]: scalar %u/%u != neon %u/%u\n", tag, what, i, j,
                      sa, sb, na, nb);
    }
}

static int cmp_mu_planes(const VifBuffer *ba, const VifBuffer *bb, unsigned w, unsigned h,
                         const char *tag)
{
    const ptrdiff_t mu_stride = ba->stride_16 / (ptrdiff_t)sizeof(uint16_t);
    int bad = 0;

    for (unsigned i = 0; i < h; ++i) {
        for (unsigned j = 0; j < w; ++j) {
            const ptrdiff_t k = i * mu_stride + j;
            const bool differs = ba->mu1[k] != bb->mu1[k] || ba->mu2[k] != bb->mu2[k];
            if (differs) {
                report_pair(tag, "mu", (int)i, j, bad, ba->mu1[k], ba->mu2[k], bb->mu1[k],
                            bb->mu2[k]);
                ++bad;
            }
        }
    }
    return bad;
}

/* The decimated half-resolution image `decimate_and_pad()` leaves behind in
 * `ref`/`dis`, plus the mirrored rows it pads above and below. */
static int cmp_decimated(const VifBuffer *ba, const VifBuffer *bb, unsigned w, unsigned h,
                         int scale, const char *tag)
{
    const ptrdiff_t v_stride = ba->stride / (ptrdiff_t)sizeof(uint16_t);
    const int fwh = vif_filter1d_width[scale] / 2;
    const uint16_t *ra = ba->ref;
    const uint16_t *rb = bb->ref;
    const uint16_t *da = ba->dis;
    const uint16_t *db = bb->dis;
    int bad = 0;

    for (int i = -fwh; i < (int)(h / 2) + fwh; ++i) {
        for (unsigned j = 0; j < w / 2; ++j) {
            const ptrdiff_t k = i * v_stride + j;
            const bool differs = ra[k] != rb[k] || da[k] != db[k];
            if (differs) {
                report_pair(tag, "decimated", i, j, bad, ra[k], da[k], rb[k], db[k]);
                ++bad;
            }
        }
    }
    return bad;
}

/* ---------------------------------------------------------------------- */
/* Per-case drivers.  Each returns the mismatch count, or -1 if the case could
 * not be set up.                                                          */
/* ---------------------------------------------------------------------- */

static int run_subsample_8_case(unsigned w, unsigned h, int pattern)
{
    VifTestCtx sc = {0};
    VifTestCtx nc = {0};

    if (!vif_ctx_alloc(&sc, w, h, false)) {
        return -1;
    }
    if (!vif_ctx_alloc(&nc, w, h, false)) {
        vif_ctx_free(&sc);
        return -1;
    }

    const uint32_t seed = 0x5eed0000u ^ (uint32_t)(w * 131u + h * 7u + (unsigned)pattern);
    fill_8(&sc.s->buf, w, h, pattern, seed);
    fill_8(&nc.s->buf, w, h, pattern, seed);

    ref_subsample_rd_8(&sc.s->buf, w, h);
    vif_subsample_rd_8_neon(&nc.s->buf, w, h);

    int bad = cmp_mu_planes(&sc.s->buf, &nc.s->buf, w, h, "subsample_rd_8");
    bad += cmp_decimated(&sc.s->buf, &nc.s->buf, w, h, 0, "subsample_rd_8");
    if (bad) {
        (void)fprintf(stderr, "  %ux%u pattern %d: %d mismatches\n", w, h, pattern, bad);
    }

    vif_ctx_free(&sc);
    vif_ctx_free(&nc);
    return bad;
}

static int run_subsample_16_case(unsigned w, unsigned h, int scale, int bpc, int pattern)
{
    VifTestCtx sc = {0};
    VifTestCtx nc = {0};

    if (!vif_ctx_alloc(&sc, w, h, true)) {
        return -1;
    }
    if (!vif_ctx_alloc(&nc, w, h, true)) {
        vif_ctx_free(&sc);
        return -1;
    }

    const uint32_t seed = 0xb16b00b5u ^ (uint32_t)(w * 131u + h * 7u + (unsigned)scale * 17u +
                                                   (unsigned)bpc * 3u + (unsigned)pattern);
    fill_16(&sc.s->buf, w, h, pattern, bpc, seed);
    fill_16(&nc.s->buf, w, h, pattern, bpc, seed);

    ref_subsample_rd_16(&sc.s->buf, w, h, scale, bpc);
    vif_subsample_rd_16_neon(&nc.s->buf, w, h, scale, bpc);

    int bad = cmp_mu_planes(&sc.s->buf, &nc.s->buf, w, h, "subsample_rd_16");
    bad += cmp_decimated(&sc.s->buf, &nc.s->buf, w, h, scale, "subsample_rd_16");
    if (bad) {
        (void)fprintf(stderr, "  %ux%u scale %d bpc %d pattern %d: %d mismatches\n", w, h, scale,
                      bpc, pattern, bad);
    }

    vif_ctx_free(&sc);
    vif_ctx_free(&nc);
    return bad;
}

/* `vif_statistic_*` only read ref/dis, so one context serves both kernels. */
static int run_statistic_8_case(unsigned w, unsigned h, int pattern)
{
    VifTestCtx ctx = {0};

    if (!vif_ctx_alloc(&ctx, w, h, false)) {
        return -1;
    }
    fill_8(&ctx.s->buf, w, h, pattern,
           0x1234abcdu ^ (uint32_t)(w * 131u + h * 7u + (unsigned)pattern));

    float num_s = 0.f;
    float den_s = 0.f;
    float num_n = 0.f;
    float den_n = 0.f;
    vif_statistic_8(ctx.s, &num_s, &den_s, w, h);
    vif_statistic_8_neon(ctx.s, &num_n, &den_n, w, h);

    const bool ok = float_bits_equal(num_s, num_n) && float_bits_equal(den_s, den_n);
    if (!ok) {
        (void)fprintf(stderr,
                      "  %ux%u (w%%8=%u) pattern %d: scalar num/den %.9g/%.9g != "
                      "neon %.9g/%.9g\n",
                      w, h, w % 8u, pattern, (double)num_s, (double)den_s, (double)num_n,
                      (double)den_n);
    }
    vif_ctx_free(&ctx);
    return ok ? 0 : 1;
}

static int run_statistic_16_case(unsigned w, unsigned h, int scale, int bpc, int pattern)
{
    VifTestCtx ctx = {0};

    if (!vif_ctx_alloc(&ctx, w, h, true)) {
        return -1;
    }
    fill_16(&ctx.s->buf, w, h, pattern, bpc,
            0xfeed7a11u ^ (uint32_t)(w * 131u + h * 7u + (unsigned)scale * 17u +
                                     (unsigned)bpc * 3u + (unsigned)pattern));

    float num_s = 0.f;
    float den_s = 0.f;
    float num_n = 0.f;
    float den_n = 0.f;
    vif_statistic_16(ctx.s, &num_s, &den_s, w, h, bpc, scale);
    vif_statistic_16_neon(ctx.s, &num_n, &den_n, w, h, bpc, scale);

    const bool ok = float_bits_equal(num_s, num_n) && float_bits_equal(den_s, den_n);
    if (!ok) {
        (void)fprintf(stderr,
                      "  %ux%u scale %d bpc %d pattern %d: scalar num/den %.9g/%.9g != "
                      "neon %.9g/%.9g\n",
                      w, h, scale, bpc, pattern, (double)num_s, (double)den_s, (double)num_n,
                      (double)den_n);
    }
    vif_ctx_free(&ctx);
    return ok ? 0 : 1;
}

/* ---------------------------------------------------------------------- */
/* Geometries.  The NEON dispatcher installs these kernels for every width, so
 * both sides of each vector stride are in scope: the `subsample_rd` kernels
 * step 16 columns, the `vif_statistic` kernels step 8.                     */
/* ---------------------------------------------------------------------- */

typedef struct {
    unsigned w;
    unsigned h;
} Geom;

static const Geom geoms[] = {
    {32, 24}, /* w % 16 == 0             */
    {40, 24}, /* w % 16 == 8, w % 8 == 0 */
    {36, 22}, /* w % 8  == 4             */
    {33, 20}, /* w % 8  == 1             */
    {47, 21}, /* w % 8  == 7, odd height */
    {64, 32}, /* both strides divide     */
};

static const int bpcs_subsample[] = {8, 10, 12};
static const int bpcs_statistic[] = {10, 12};
#endif /* ARCH_AARCH64 */

static char *test_vif_subsample_rd_8_neon(void)
{
#if !ARCH_AARCH64
    return NULL; /* NEON kernels are aarch64-only. */
#else
    for (size_t t = 0; t < sizeof(geoms) / sizeof(geoms[0]); ++t) {
        for (int pattern = 0; pattern <= PATTERN_LOW_CONTRAST; ++pattern) {
            const int bad = run_subsample_8_case(geoms[t].w, geoms[t].h, pattern);
            mu_assert("allocation failed", bad >= 0);
            mu_assert("vif_subsample_rd_8_neon diverges from the scalar reference", bad == 0);
        }
    }
    return NULL;
#endif
}

#if ARCH_AARCH64
/* Sweeps bpc x pattern for one (geometry, scale); returns NULL when all pass. */
static char *check_subsample_16_geom(const Geom *g, int scale)
{
    for (size_t b = 0; b < sizeof(bpcs_subsample) / sizeof(bpcs_subsample[0]); ++b) {
        for (int pattern = 0; pattern <= PATTERN_LOW_CONTRAST; ++pattern) {
            const int bad = run_subsample_16_case(g->w, g->h, scale, bpcs_subsample[b], pattern);
            mu_assert("allocation failed", bad >= 0);
            mu_assert("vif_subsample_rd_16_neon diverges from the scalar reference", bad == 0);
        }
    }
    return NULL;
}
#endif

static char *test_vif_subsample_rd_16_neon(void)
{
#if !ARCH_AARCH64
    return NULL;
#else
    for (size_t t = 0; t < sizeof(geoms) / sizeof(geoms[0]); ++t) {
        for (int scale = 0; scale < 3; ++scale) {
            char *msg = check_subsample_16_geom(&geoms[t], scale);
            if (msg) {
                return msg;
            }
        }
    }
    return NULL;
#endif
}

static char *test_vif_statistic_8_neon(void)
{
#if !ARCH_AARCH64
    return NULL;
#else
    for (size_t t = 0; t < sizeof(geoms) / sizeof(geoms[0]); ++t) {
        for (int pattern = 0; pattern <= PATTERN_LOW_CONTRAST; ++pattern) {
            const int bad = run_statistic_8_case(geoms[t].w, geoms[t].h, pattern);
            mu_assert("allocation failed", bad >= 0);
            mu_assert("vif_statistic_8_neon diverges from the scalar reference", bad == 0);
        }
    }
    return NULL;
#endif
}

#if ARCH_AARCH64
/* Sweeps bpc x pattern for one (geometry, scale); returns NULL when all pass. */
static char *check_statistic_16_geom(const Geom *g, int scale)
{
    for (size_t b = 0; b < sizeof(bpcs_statistic) / sizeof(bpcs_statistic[0]); ++b) {
        for (int pattern = 0; pattern <= PATTERN_LOW_CONTRAST; ++pattern) {
            const int bad = run_statistic_16_case(g->w, g->h, scale, bpcs_statistic[b], pattern);
            mu_assert("allocation failed", bad >= 0);
            mu_assert("vif_statistic_16_neon diverges from the scalar reference", bad == 0);
        }
    }
    return NULL;
}
#endif

static char *test_vif_statistic_16_neon(void)
{
#if !ARCH_AARCH64
    return NULL;
#else
    for (size_t t = 0; t < sizeof(geoms) / sizeof(geoms[0]); ++t) {
        for (int scale = 0; scale < 4; ++scale) {
            char *msg = check_statistic_16_geom(&geoms[t], scale);
            if (msg) {
                return msg;
            }
        }
    }
    return NULL;
#endif
}

char *run_tests(void)
{
#if ARCH_AARCH64
    mu_run_test(test_vif_subsample_rd_8_neon);
    mu_run_test(test_vif_subsample_rd_16_neon);
    mu_run_test(test_vif_statistic_8_neon);
    mu_run_test(test_vif_statistic_16_neon);
#else
    (void)fprintf(stderr, "skipping: non-aarch64 arch\n");
    (void)test_vif_subsample_rd_8_neon;
    (void)test_vif_subsample_rd_16_neon;
    (void)test_vif_statistic_8_neon;
    (void)test_vif_statistic_16_neon;
#endif
    return NULL;
}
