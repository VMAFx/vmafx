/**
 *
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

/*
 * SpEED-QA feature extractor — ADR-0253.
 *
 * Reference: Bampis, Gupta, Soundararajan and Bovik,
 *   "SpEED-QA: Spatial Efficient Entropic Differencing for Image and Video
 *   Quality", IEEE SPL 24(9), 1333-1337, 2017.
 *   DOI 10.1109/LSP.2017.2726542
 *
 * Algorithm
 * ---------
 * Divide the distorted luma plane into non-overlapping B x B blocks (B=7).
 * For each block compute the Gaussian-windowed local mean (mu) and variance
 * (sigma^2) using a separable 7-tap Gaussian kernel (sigma_g = 1.166).
 * Per-block entropy:
 *
 *   H(block) = 0.5 * log2(2 * pi * e * (sigma^2 + epsilon))
 *
 * where epsilon = 1.0 pixel^2 prevents log(0) on flat blocks.
 *
 * Spatial score S:
 *   S = mean(H(block_i)) over all nb_x * nb_y blocks.
 *
 * Temporal score T (zero for frame 0; for frame n > 0):
 *   T = mean(H(delta_block_i)) where delta = dist[n] - dist[n-1].
 *
 * Output per frame: score = S + T.
 *
 * Implementation notes
 * --------------------
 * Self-contained: no dependency on speed.c (which is float-gated).
 * Integer luma pixels; double-precision accumulation.
 * Gaussian weights stored as Q16 fixed-point.
 * Previous distorted frame kept in a private aligned buffer.
 * VMAF_FEATURE_EXTRACTOR_TEMPORAL set for in-order frame delivery.
 */

#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES
#endif

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef M_E
#define M_E 2.71828182845904523536
#endif

#include "libvmaf/picture.h"

#include "feature_collector.h"
#include "feature_extractor.h"
#include "mem.h"

/* -------------------------------------------------------------------------
 * 1-D Gaussian kernel: 7 taps, sigma_g = 1.166 (matches VIF family).
 *
 * g(k) = exp(-k^2 / (2 * 1.166^2))  for k in {-3,..,+3}
 * Normalised to Q16 fixed-point (sum = 65535):
 *
 *   g(0)=23903  g(+/-1)=15754  g(+/-2)=4504  g(+/-3)=558
 *   Sum = 23903 + 2*(15754+4504+558) = 65535
 *
 * 2-D weight(i,j) = g_gauss_1d[i] * g_gauss_1d[j]  (separable).
 * Q16 * Q16 = Q32; divided by 65535^2 to get a weight in [0,1].
 * -------------------------------------------------------------------------*/
#define SPEED_QA_BLOCK_SIZE (7)

static const int32_t g_gauss_1d[SPEED_QA_BLOCK_SIZE] = {558, 4504, 15754, 23903, 15754, 4504, 558};

/* Noise floor (pixel^2): keeps log2(sigma^2 + epsilon) finite on flat blocks.
 * Pixels are in [0,255] for 8-bpc. */
#define SPEED_QA_EPSILON (1.0)

/* log2(2 * pi * e) — constant factor in the Gaussian differential entropy. */
static const double SPEED_QA_LOG2_2PIE = 2.0471975511965977;

/* -------------------------------------------------------------------------
 * Private extractor state.
 * -------------------------------------------------------------------------*/
typedef struct SpeedQaState {
    unsigned w;            /* luma width */
    unsigned h;            /* luma height */
    unsigned bpc;          /* bits per component */
    unsigned nb_x;         /* complete B x B blocks horizontally */
    unsigned nb_y;         /* complete B x B blocks vertically */
    uint8_t *prev_dist;    /* previous distorted luma buffer */
    ptrdiff_t prev_stride; /* bytes per row in prev_dist */
    int have_prev;         /* non-zero after first frame */
} SpeedQaState;

/* -------------------------------------------------------------------------
 * Gaussian-windowed mean and variance for a B x B block in the luma plane.
 *
 * data         : pointer to the start of the luma plane.
 * stride_bytes : bytes per row.
 * bpc          : bits per component (8 or 10/12 HBD).
 * r0, c0       : top-left pixel coordinates of the block.
 * out_mean     : output weighted mean.
 * out_var      : output weighted variance (>= 0).
 * -------------------------------------------------------------------------*/
static void block_mean_var(const uint8_t *data, ptrdiff_t stride_bytes, unsigned bpc, unsigned r0,
                           unsigned c0, double *out_mean, double *out_var)
{
    const double w2d_norm = (double)65535 * (double)65535;
    double wsum = 0.0;
    double wpix = 0.0;
    double wpix2 = 0.0;

    for (int dr = 0; dr < SPEED_QA_BLOCK_SIZE; dr++) {
        unsigned row = r0 + (unsigned)dr;
        const int32_t wr = g_gauss_1d[dr];

        if (bpc <= 8) {
            const uint8_t *rowptr = data + row * stride_bytes + c0;
            for (int dc = 0; dc < SPEED_QA_BLOCK_SIZE; dc++) {
                double w2 = (double)(wr * g_gauss_1d[dc]) / w2d_norm;
                double p = (double)rowptr[dc];
                wsum += w2;
                wpix += w2 * p;
                wpix2 += w2 * p * p;
            }
        } else {
            /* HBD: pixels stored as uint16_t; normalise to [0,255]. */
            const uint16_t *rowptr =
                (const uint16_t *)((const uint8_t *)data + row * stride_bytes) + c0;
            double scale = 255.0 / (double)((1u << bpc) - 1u);
            for (int dc = 0; dc < SPEED_QA_BLOCK_SIZE; dc++) {
                double w2 = (double)(wr * g_gauss_1d[dc]) / w2d_norm;
                double p = (double)rowptr[dc] * scale;
                wsum += w2;
                wpix += w2 * p;
                wpix2 += w2 * p * p;
            }
        }
    }

    double mean = (wsum > 0.0) ? wpix / wsum : 0.0;
    double var = (wsum > 0.0) ? (wpix2 / wsum) - mean * mean : 0.0;
    if (var < 0.0)
        var = 0.0; /* clamp floating-point rounding artefacts */

    *out_mean = mean;
    *out_var = var;
}

/* -------------------------------------------------------------------------
 * Gaussian-windowed mean and variance for a B x B block of a signed
 * frame-difference image (int16_t, pixels normalised to 8-bpc scale).
 * -------------------------------------------------------------------------*/
static void diff_block_mean_var(const int16_t *diff, ptrdiff_t stride_elems, unsigned r0,
                                unsigned c0, double *out_mean, double *out_var)
{
    const double w2d_norm = (double)65535 * (double)65535;
    double wsum = 0.0;
    double wpix = 0.0;
    double wpix2 = 0.0;

    for (int dr = 0; dr < SPEED_QA_BLOCK_SIZE; dr++) {
        const int16_t *rowptr = diff + (r0 + (unsigned)dr) * stride_elems + c0;
        const int32_t wr = g_gauss_1d[dr];
        for (int dc = 0; dc < SPEED_QA_BLOCK_SIZE; dc++) {
            double w2 = (double)(wr * g_gauss_1d[dc]) / w2d_norm;
            double p = (double)rowptr[dc];
            wsum += w2;
            wpix += w2 * p;
            wpix2 += w2 * p * p;
        }
    }

    double mean = (wsum > 0.0) ? wpix / wsum : 0.0;
    double var = (wsum > 0.0) ? (wpix2 / wsum) - mean * mean : 0.0;
    if (var < 0.0)
        var = 0.0;

    *out_mean = mean;
    *out_var = var;
}

/* -------------------------------------------------------------------------
 * Spatial entropy: mean per-block H over the distorted luma plane.
 * -------------------------------------------------------------------------*/
static double compute_spatial_entropy(const SpeedQaState *s, const VmafPicture *pic)
{
    double entropy_sum = 0.0;
    const uint8_t *luma = pic->data[0];
    ptrdiff_t stride_bytes = pic->stride[0];

    for (unsigned by = 0; by < s->nb_y; by++) {
        for (unsigned bx = 0; bx < s->nb_x; bx++) {
            unsigned r0 = by * SPEED_QA_BLOCK_SIZE;
            unsigned c0 = bx * SPEED_QA_BLOCK_SIZE;
            double mean;
            double var;
            block_mean_var(luma, stride_bytes, s->bpc, r0, c0, &mean, &var);
            double h = 0.5 * (SPEED_QA_LOG2_2PIE + log2(var + SPEED_QA_EPSILON));
            entropy_sum += h;
        }
    }

    unsigned n_blocks = s->nb_x * s->nb_y;
    return (n_blocks > 0u) ? entropy_sum / (double)n_blocks : 0.0;
}

/* -------------------------------------------------------------------------
 * Temporal entropy: mean per-block H over the frame-difference image.
 * -------------------------------------------------------------------------*/
static double compute_temporal_entropy(const SpeedQaState *s, const VmafPicture *cur)
{
    unsigned nb = s->nb_x * s->nb_y;
    if (nb == 0u)
        return 0.0;

    size_t n_elems = (size_t)s->w * (size_t)s->h;
    int16_t *diff = aligned_malloc(n_elems * sizeof(int16_t), 32);
    if (!diff)
        return 0.0;

    const uint8_t *cur_data = cur->data[0];
    const uint8_t *prev_data = s->prev_dist;
    ptrdiff_t cur_stride = cur->stride[0];
    ptrdiff_t prev_stride = s->prev_stride;

    if (s->bpc <= 8) {
        for (unsigned r = 0; r < s->h; r++) {
            const uint8_t *cr = cur_data + r * cur_stride;
            const uint8_t *pr = prev_data + r * prev_stride;
            int16_t *dr = diff + r * (ptrdiff_t)s->w;
            for (unsigned c = 0; c < s->w; c++)
                dr[c] = (int16_t)cr[c] - (int16_t)pr[c];
        }
    } else {
        double scale = 255.0 / (double)((1u << s->bpc) - 1u);
        for (unsigned r = 0; r < s->h; r++) {
            const uint16_t *cr = (const uint16_t *)(cur_data + r * cur_stride);
            const uint16_t *pr = (const uint16_t *)(prev_data + r * prev_stride);
            int16_t *dr = diff + r * (ptrdiff_t)s->w;
            for (unsigned c = 0; c < s->w; c++) {
                double d = ((double)cr[c] - (double)pr[c]) * scale;
                /* Normalised diff is in [-255, 255]; clamp to int16 range. */
                if (d < -32768.0)
                    d = -32768.0;
                if (d > 32767.0)
                    d = 32767.0;
                dr[c] = (int16_t)d;
            }
        }
    }

    double entropy_sum = 0.0;
    ptrdiff_t diff_stride = (ptrdiff_t)s->w;

    for (unsigned by = 0; by < s->nb_y; by++) {
        for (unsigned bx = 0; bx < s->nb_x; bx++) {
            unsigned r0 = by * SPEED_QA_BLOCK_SIZE;
            unsigned c0 = bx * SPEED_QA_BLOCK_SIZE;
            double mean;
            double var;
            diff_block_mean_var(diff, diff_stride, r0, c0, &mean, &var);
            double h = 0.5 * (SPEED_QA_LOG2_2PIE + log2(var + SPEED_QA_EPSILON));
            entropy_sum += h;
        }
    }

    aligned_free(diff);
    return entropy_sum / (double)nb;
}

/* -------------------------------------------------------------------------
 * Copy distorted luma into prev_dist for use on the next frame.
 * -------------------------------------------------------------------------*/
static void update_prev_dist(SpeedQaState *s, const VmafPicture *dist)
{
    const uint8_t *src = dist->data[0];
    ptrdiff_t src_stride = dist->stride[0];
    ptrdiff_t dst_stride = s->prev_stride;
    unsigned bytes_per_px = (s->bpc <= 8) ? 1u : 2u;
    unsigned row_bytes = s->w * bytes_per_px;

    for (unsigned r = 0; r < s->h; r++) {
        memcpy(s->prev_dist + r * dst_stride, src + r * src_stride, row_bytes);
    }
    s->have_prev = 1;
}

/* -------------------------------------------------------------------------
 * VmafFeatureExtractor callbacks.
 * -------------------------------------------------------------------------*/

static int init(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc, unsigned w,
                unsigned h)
{
    (void)pix_fmt;

    if (w < (unsigned)SPEED_QA_BLOCK_SIZE || h < (unsigned)SPEED_QA_BLOCK_SIZE)
        return -22; /* EINVAL: frame too small for one block */

    SpeedQaState *s = fex->priv;
    s->w = w;
    s->h = h;
    s->bpc = bpc;
    s->nb_x = w / SPEED_QA_BLOCK_SIZE;
    s->nb_y = h / SPEED_QA_BLOCK_SIZE;
    s->have_prev = 0;

    unsigned bytes_per_px = (bpc <= 8) ? 1u : 2u;
    size_t row_bytes = (size_t)w * bytes_per_px;
    s->prev_stride = (ptrdiff_t)ALIGN_CEIL(row_bytes);
    size_t buf_size = (size_t)s->prev_stride * (size_t)h;

    s->prev_dist = aligned_malloc(buf_size, 32);
    if (!s->prev_dist)
        return -12; /* ENOMEM */

    return 0;
}

static int extract(VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                   VmafPicture *dist_pic, VmafPicture *dist_pic_90, unsigned index,
                   VmafFeatureCollector *feature_collector)
{
    (void)ref_pic;
    (void)ref_pic_90;
    (void)dist_pic_90;

    SpeedQaState *s = fex->priv;

    double spatial = compute_spatial_entropy(s, dist_pic);
    double temporal = 0.0;

    if (s->have_prev)
        temporal = compute_temporal_entropy(s, dist_pic);

    update_prev_dist(s, dist_pic);

    double score = spatial + temporal;
    return vmaf_feature_collector_append(feature_collector, "speed_qa", score, index);
}

static int close_fex(VmafFeatureExtractor *fex)
{
    SpeedQaState *s = fex->priv;
    if (s->prev_dist) {
        aligned_free(s->prev_dist);
        s->prev_dist = NULL;
    }
    return 0;
}

static const char *provided_features[] = {"speed_qa", NULL};

/* NOLINTNEXTLINE(misc-use-internal-linkage) — external linkage required:
 * vmaf_fex_speed_qa is declared extern in feature_extractor.c and appears
 * in feature_extractor_list[]. Making it static would break linking.
 * ADR-0253 (real-impl follow-up). */
VmafFeatureExtractor vmaf_fex_speed_qa = {
    .name = "speed_qa",
    .init = init,
    .extract = extract,
    .close = close_fex,
    .priv_size = sizeof(SpeedQaState),
    .provided_features = provided_features,
    /* TEMPORAL: in-order delivery; we maintain our own prev_dist buffer
     * rather than fex->prev_ref (which carries reference, not dist, frames). */
    .flags = VMAF_FEATURE_EXTRACTOR_TEMPORAL,
    .chars = {0},
};
