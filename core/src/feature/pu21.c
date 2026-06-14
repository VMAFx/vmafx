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
 * PU21 (Perceptually Uniform encoding, 2021) HDR feature extractor.
 *
 * Provides two features:
 *   - "pu21_psnr": PSNR of the PU21-encoded luma planes (peak = 256, no cap)
 *   - "pu21_ssim": single-scale Gaussian SSIM of the PU21-encoded luma planes
 *                  at data range L = 256
 *
 * PU21 is a "transform-then-score" HDR adapter (cf. ciede / ssimulacra2): it
 * maps absolute display luminance (cd/m^2) through the PU21 transfer function
 * so the existing SDR metrics become perceptually meaningful on HDR content.
 *
 * Scope (RC):
 *   - Encodes the LUMA (Y) plane only (matches SSIM's luminance path).
 *   - Input transfer = PQ (SMPTE ST.2084) ONLY. The PQ-coded Y is decoded to
 *     absolute cd/m^2 via the ST.2084 EOTF x 10000, then PU21-encoded. HLG and
 *     SDR-gamma paths are deferred (rejected with -EINVAL), consistent with the
 *     ΔE-ITP HDR-scope decision.
 *   - All per-pixel math is double precision.
 *
 * See docs/metrics/pu21.md and docs/adr/<NNNN>-pu21-hdr-metric.md.
 */

#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "feature_collector.h"
#include "feature_extractor.h"
#include "mem.h"
#include "opt.h"

#include "pu21_math.h"
#include "pu21_ssim.h"

/* PU-PSNR downstream constants (pu21_metric.m). The encoder is designed so
 * 100 nit maps to ~256, mimicking an 8-bit SDR input to the downstream metric;
 * hence peak = 256 and SSIM data range L = 256. */
#define PU21_PSNR_PEAK 256.0
#define PU21_SSIM_L 256.0

/* MSE floor for the PU-PSNR dB conversion: identical to compute_psnr()'s eps,
 * so MSE == 0 (identical frames) yields a large finite dB rather than +inf. */
#define PU21_MSE_EPS 1e-10

enum Pu21Transfer {
    PU21_TRANSFER_PQ = 0,
};

typedef struct Pu21State {
    char *variant;  /**< coefficient-table variant name (option) */
    char *transfer; /**< input transfer characteristic name (option) */
    enum Pu21Variant variant_id;
    enum Pu21Transfer transfer_id;
    const double *p; /**< selected 7-coefficient row */
    /* Encoded planes are stored in double (per the maintainer constraint
     * "all per-pixel math in double precision"). This is also required for
     * PU-PSNR correctness: PU-PSNR differences two large nearly-equal encoded
     * values (~256), so float storage would lose ~7 significant figures and
     * push the dB score off the fp64 oracle by ~1e-4 dB. */
    double *ref_enc;  /**< w*h PU21-encoded reference luma */
    double *dist_enc; /**< w*h PU21-encoded distorted luma */
    unsigned w;
    unsigned h;
} Pu21State;

static const VmafOption options[] = {
    {
        .name = "variant",
        .help = "PU21 coefficient variant: banding, banding_glare (default), peaks, peaks_glare",
        .offset = offsetof(Pu21State, variant),
        .type = VMAF_OPT_TYPE_STRING,
        .default_val.s = "banding_glare",
    },
    {
        .name = "transfer",
        .help = "input transfer characteristic (RC supports pq only): pq",
        .offset = offsetof(Pu21State, transfer),
        .type = VMAF_OPT_TYPE_STRING,
        .default_val.s = "pq",
    },
    {0}};

static int pu21_select_variant(const char *name, enum Pu21Variant *out)
{
    if (!name)
        return -EINVAL;
    if (!strcmp(name, "banding")) {
        *out = PU21_VARIANT_BANDING;
    } else if (!strcmp(name, "banding_glare")) {
        *out = PU21_VARIANT_BANDING_GLARE;
    } else if (!strcmp(name, "peaks")) {
        *out = PU21_VARIANT_PEAKS;
    } else if (!strcmp(name, "peaks_glare")) {
        *out = PU21_VARIANT_PEAKS_GLARE;
    } else {
        return -EINVAL;
    }
    return 0;
}

static int pu21_select_transfer(const char *name, enum Pu21Transfer *out)
{
    if (!name)
        return -EINVAL;
    /* RC ships PQ input only; HLG / SDR-gamma are deferred. */
    if (!strcmp(name, "pq")) {
        *out = PU21_TRANSFER_PQ;
        return 0;
    }
    return -EINVAL;
}

static int init(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc, unsigned w,
                unsigned h)
{
    (void)pix_fmt; /* luma-only: every planar YUV (incl. YUV400P) is accepted */
    Pu21State *s = fex->priv;

    if (bpc != 8 && bpc != 10 && bpc != 12 && bpc != 16)
        return -EINVAL;

    int err = pu21_select_variant(s->variant, &s->variant_id);
    if (err)
        return err;
    err = pu21_select_transfer(s->transfer, &s->transfer_id);
    if (err)
        return err;
    s->p = pu21_params[s->variant_id];

    s->w = w;
    s->h = h;
    s->ref_enc = aligned_malloc((size_t)w * (size_t)h * sizeof(double), 32);
    if (!s->ref_enc)
        return -ENOMEM;
    s->dist_enc = aligned_malloc((size_t)w * (size_t)h * sizeof(double), 32);
    if (!s->dist_enc) {
        aligned_free(s->ref_enc);
        s->ref_enc = NULL;
        return -ENOMEM;
    }

    return 0;
}

/* Read a luma sample (bpc-aware) and return the raw integer code value. */
static inline double pu21_read_luma(const VmafPicture *pic, unsigned i, unsigned j)
{
    if (pic->bpc > 8) {
        const uint16_t *row =
            (const uint16_t *)((const uint8_t *)pic->data[0] + (size_t)i * pic->stride[0]);
        return (double)row[j];
    }
    const uint8_t *row = (const uint8_t *)pic->data[0] + (size_t)i * pic->stride[0];
    return (double)row[j];
}

/* PQ code value -> absolute luminance (cd/m^2). Decodes the integer code value
 * to a normalised [0,1] PQ signal (full-range normalisation by the bpc max),
 * then applies the ST.2084 EOTF x 10000. */
static inline double pu21_codeval_to_luminance(double codeval, unsigned bpc,
                                               enum Pu21Transfer transfer)
{
    /* Only PQ is reachable here (init() rejects all other transfers). */
    (void)transfer;
    const double maxv = (double)((1u << bpc) - 1u);
    const double ep = codeval / maxv;
    return pu21_pq_eotf_nits(ep);
}

/* Encode one luma plane into a tightly-packed (stride == w) double buffer. */
static void pu21_encode_plane(const Pu21State *s, const VmafPicture *pic, double *out)
{
    for (unsigned i = 0; i < s->h; i++) {
        for (unsigned j = 0; j < s->w; j++) {
            const double cv = pu21_read_luma(pic, i, j);
            double lum = pu21_codeval_to_luminance(cv, pic->bpc, s->transfer_id);
            lum = pu21_clamp_luminance(lum);
            out[(size_t)i * s->w + j] = pu21_encode(lum, s->p);
        }
    }
}

/* PU-PSNR (dB) over the encoded planes. Cannot fail: peak = 256, NO artificial
 * SDR cap (the 60/72/84/108 dB table is wrong for the ~0..600 PU-encoded
 * domain). MSE == 0 (identical frames) yields a large finite dB via the eps
 * floor rather than +inf. */
static double pu21_compute_psnr(const double *ref, const double *dist, unsigned w, unsigned h)
{
    double mse = 0.0;
    const size_t n = (size_t)w * (size_t)h;
    for (size_t i = 0; i < n; i++) {
        const double diff = ref[i] - dist[i];
        mse += diff * diff;
    }
    mse /= (double)n;

    const double denom = mse > PU21_MSE_EPS ? mse : PU21_MSE_EPS;
    return 10.0 * log10(PU21_PSNR_PEAK * PU21_PSNR_PEAK / denom);
}

static int extract(VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                   VmafPicture *dist_pic, VmafPicture *dist_pic_90, unsigned index,
                   VmafFeatureCollector *feature_collector)
{
    Pu21State *s = fex->priv;
    (void)ref_pic_90;
    (void)dist_pic_90;

    pu21_encode_plane(s, ref_pic, s->ref_enc);
    pu21_encode_plane(s, dist_pic, s->dist_enc);

    const double psnr_score = pu21_compute_psnr(s->ref_enc, s->dist_enc, s->w, s->h);
    int err = vmaf_feature_collector_append(feature_collector, "pu21_psnr", psnr_score, index);
    if (err)
        return err;

    double ssim_score = 0.0;
    err =
        pu21_compute_ssim(s->ref_enc, s->dist_enc, (int)s->w, (int)s->h, PU21_SSIM_L, &ssim_score);
    if (err)
        return err;
    return vmaf_feature_collector_append(feature_collector, "pu21_ssim", ssim_score, index);
}

static int close(VmafFeatureExtractor *fex)
{
    Pu21State *s = fex->priv;
    if (s->ref_enc)
        aligned_free(s->ref_enc);
    if (s->dist_enc)
        aligned_free(s->dist_enc);
    return 0;
}

static const char *provided_features[] = {"pu21_psnr", "pu21_ssim", NULL};

// NOLINTNEXTLINE(misc-use-internal-linkage): cross-TU registry pattern — external linkage required (ADR-0278).
VmafFeatureExtractor vmaf_fex_pu21 = {
    .name = "pu21",
    .init = init,
    .extract = extract,
    .options = options,
    .close = close,
    .priv_size = sizeof(Pu21State),
    .provided_features = provided_features,
    /* Per-pixel PU21 encode (double pow) + a double-precision PSNR reduction
     * and a single-scale Gaussian SSIM per frame; the kernel cost scales with
     * frame size, mirroring ciede's dispatch profile (AUTO + 720p area,
     * ADR-0181 / ADR-0182). */
    .chars =
        {
            .n_dispatches_per_frame = 1,
            .is_reduction_only = false,
            .min_useful_frame_area = 1280U * 720U,
            .dispatch_hint = VMAF_FEATURE_DISPATCH_AUTO,
        },
};
