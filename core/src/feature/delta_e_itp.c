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
 * ΔE-ITP (Delta E ITP) — full-reference HDR/WCG colour-difference metric
 * per ITU-R BT.2124-0 "Objective metric for the assessment of the
 * potential visibility of colour differences in television".
 *
 * The per-pixel kernel converts reference and distorted YUV samples to
 * the scaled ICtCp ("ITP") colour space and reports the Euclidean
 * distance, scaled by 720 so that a value of ~1 corresponds to one
 * just-noticeable colour difference.  The frame score is the mean
 * per-pixel ΔE-ITP (sum in double / (w*h)).
 *
 * Pipeline (BT.2124-0 Annex 1):
 *   YUV -> non-linear R'G'B' (BT.2020/BT.709 inverse matrix, narrow/full)
 *       -> linear RGB (PQ EOTF) -> LMS (BT.2100 /4096 matrix)
 *       -> L'M'S' (PQ inverse EOTF) -> ICtCp (BT.2100 /4096 matrix)
 *       -> ITP (I, 0.5*Ct, Cp).
 *
 * RC scope: PQ (ST-2084) transfer ONLY.  The HLG and BT.1886/SDR
 * transfers defined in BT.2124-0 Annex 3 / Conversion 5 are deferred
 * follow-ups (their constants are single-sourced; see
 * docs/adr/1110-delta-e-itp-metric.md and docs/metrics/delta_e_itp.md).
 *
 * Mirrors ciede.c (the closest in-tree analog): chroma upsampling,
 * 8/16-bit plane reads, double-precision per-pixel math and frame sum.
 */

#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature/delta_e_itp_math.h"
#include "log.h"
#include "opt.h"
#include "picture.h"

#define DEFAULT_DEITP_TRANSFER ("pq")
#define DEFAULT_DEITP_MATRIX ("bt2020")
#define DEFAULT_DEITP_RANGE ("limited")

/* BT.2020 NCL luma coefficients (ITU-R BT.2100 / BT.2020). */
#define DEITP_KR_BT2020 (0.2627)
#define DEITP_KG_BT2020 (0.6780)
#define DEITP_KB_BT2020 (0.0593)

/* BT.709 luma coefficients (ITU-R BT.709) — SDR option. */
#define DEITP_KR_BT709 (0.2126)
#define DEITP_KG_BT709 (0.7152)
#define DEITP_KB_BT709 (0.0722)

typedef struct DeltaEItpState {
    VmafPicture ref;
    VmafPicture dist;
    void (*scale_chroma_planes)(VmafPicture *in, VmafPicture *out);
    char *transfer;
    char *matrix;
    char *range;
    /* Resolved from the string options at init time. */
    double kr;
    double kg;
    double kb;
    int full_range;
} DeltaEItpState;

static void scale_chroma_planes_hbd(VmafPicture *in, VmafPicture *out)
{
    const int ss_hor = in->pix_fmt != VMAF_PIX_FMT_YUV444P;
    const int ss_ver = in->pix_fmt == VMAF_PIX_FMT_YUV420P;

    for (unsigned p = 0; p < 3; p++) {
        uint16_t *in_buf = in->data[p];
        uint16_t *out_buf = out->data[p];
        for (unsigned i = 0; i < out->h[p]; i++) {
            for (unsigned j = 0; j < out->w[p]; j++) {
                out_buf[j] = in_buf[(j / ((p && ss_ver) ? 2 : 1))];
            }
            in_buf += (((p && ss_hor) ? i % 2 : 1) * in->stride[p]) / 2;
            out_buf += out->stride[p] / 2;
        }
    }
}

static void scale_chroma_planes(VmafPicture *in, VmafPicture *out)
{
    const int ss_hor = in->pix_fmt != VMAF_PIX_FMT_YUV444P;
    const int ss_ver = in->pix_fmt == VMAF_PIX_FMT_YUV420P;

    for (unsigned p = 0; p < 3; p++) {
        uint8_t *in_buf = in->data[p];
        uint8_t *out_buf = out->data[p];
        for (unsigned i = 0; i < out->h[p]; i++) {
            for (unsigned j = 0; j < out->w[p]; j++) {
                out_buf[j] = in_buf[(j / ((p && ss_ver) ? 2 : 1))];
            }
            in_buf += ((p && ss_hor) ? i % 2 : 1) * in->stride[p];
            out_buf += out->stride[p];
        }
    }
}

/* Convert one PQ R'G'B' triple in [0, 1] to the scaled ICtCp ("ITP")
 * colour space (ITU-R BT.2124-0 Annex 1, PQ path).  No clamping of
 * negative LMS / ICtCp values — out-of-gamut colours are measured as-is
 * (BT.2124 Annex 4 note).  Pure function (no extractor state) so the
 * unit test can call it directly against the normative oracle. */
static void rgb_pq_to_itp(double rp, double gp, double bp, double itp[3])
{
    /* PQ EOTF: non-linear R'G'B' -> linear, display-referred RGB (cd/m^2). */
    const double r = vmaf_pq_eotf(rp);
    const double g = vmaf_pq_eotf(gp);
    const double b = vmaf_pq_eotf(bp);

    /* Linear RGB -> LMS (BT.2100 integer matrix, each row /4096). */
    const double l = (1688.0 * r + 2146.0 * g + 262.0 * b) / 4096.0;
    const double m = (683.0 * r + 2951.0 * g + 462.0 * b) / 4096.0;
    const double s = (99.0 * r + 309.0 * g + 3688.0 * b) / 4096.0;

    /* LMS -> L'M'S' via the PQ inverse EOTF (ICtCp is always PQ-encoded). */
    const double lp = vmaf_pq_eotf_inv(l);
    const double mp = vmaf_pq_eotf_inv(m);
    const double sp = vmaf_pq_eotf_inv(s);

    /* L'M'S' -> ICtCp (BT.2100 integer matrix, each row /4096). */
    const double i = (2048.0 * lp + 2048.0 * mp) / 4096.0;
    const double ct = (6610.0 * lp - 13613.0 * mp + 7003.0 * sp) / 4096.0;
    const double cp = (17933.0 * lp - 17390.0 * mp - 543.0 * sp) / 4096.0;

    /* ITP scaling (BT.2124-0 §2 Step 4): I = I, T = 0.5*Ct, P = Cp. */
    itp[0] = i;
    itp[1] = 0.5 * ct;
    itp[2] = cp;
}

/* Per-pixel ΔE-ITP between two ITP triples (BT.2124-0 §2 Step 5). */
static double delta_e_itp_pair(const double a[3], const double b[3])
{
    const double di = a[0] - b[0];
    const double dt = a[1] - b[1];
    const double dp = a[2] - b[2];
    return 720.0 * sqrt(di * di + dt * dt + dp * dp);
}

/* Dequantize a single integer YUV sample triple to non-linear R'G'B'
 * in [0, 1] using the configured luma coefficients and quantization
 * range.  Narrow (limited) range: Y' = (Yd/2^(n-8) - 16)/219,
 * C = (Cd/2^(n-8) - 128)/224.  Full range: Y' = Yd/(2^n - 1),
 * C = (Cd - 2^(n-1))/(2^n - 1). */
static void yuv_to_rgb_prime(const DeltaEItpState *s, double y, double u, double v, unsigned bpc,
                             double rgb[3])
{
    if (s->full_range) {
        const double maxv = (double)((1U << bpc) - 1U);
        const double mid = (double)(1U << (bpc - 1));
        y = y / maxv;
        u = (u - mid) / maxv;
        v = (v - mid) / maxv;
    } else {
        const double scale = (double)(1U << (bpc - 8));
        y = (y - 16.0 * scale) / (219.0 * scale);
        u = (u - 128.0 * scale) / (224.0 * scale);
        v = (v - 128.0 * scale) / (224.0 * scale);
    }

    /* Standard NCL YCbCr -> R'G'B' inverse (U = Cb, V = Cr). */
    const double kr = s->kr;
    const double kg = s->kg;
    const double kb = s->kb;
    rgb[0] = y + 2.0 * (1.0 - kr) * v;
    rgb[2] = y + 2.0 * (1.0 - kb) * u;
    rgb[1] = y - (2.0 * kb * (1.0 - kb) / kg) * u - (2.0 * kr * (1.0 - kr) / kg) * v;
}

/* Read one integer YUV sample triple from a planar picture at (i, j),
 * returning it as doubles.  Handles 8-bit (uint8_t) and HBD (uint16_t)
 * plane layouts.  Returns 0 on success, -EINVAL on an unsupported bpc. */
static int read_yuv_sample(const VmafPicture *pic, unsigned i, unsigned j, double yuv[3])
{
    switch (pic->bpc) {
    case 8:
        yuv[0] = ((const uint8_t *)pic->data[0])[i * pic->stride[0] + j];
        yuv[1] = ((const uint8_t *)pic->data[1])[i * pic->stride[1] + j];
        yuv[2] = ((const uint8_t *)pic->data[2])[i * pic->stride[2] + j];
        return 0;
    case 10:
    case 12:
    case 16:
        // NOLINTBEGIN(bugprone-integer-division) — ADR-0141 / ADR-0278: `stride / 2` is the byte->element
        // step for the uint16_t plane index, not a value flowing into the double
        // `yuv[*]` destinations. The index arithmetic itself is exact integer math
        // (mirrors the ciede.c HBD read pattern).
        yuv[0] = ((const uint16_t *)pic->data[0])[i * (pic->stride[0] / 2) + j];
        yuv[1] = ((const uint16_t *)pic->data[1])[i * (pic->stride[1] / 2) + j];
        yuv[2] = ((const uint16_t *)pic->data[2])[i * (pic->stride[2] / 2) + j];
        // NOLINTEND(bugprone-integer-division)
        return 0;
    default:
        return -EINVAL;
    }
}

/* Resolve the matrix / range string options into the numeric state used
 * by the per-pixel kernel.  Returns 0 or -EINVAL on an unknown value. */
static int resolve_options(DeltaEItpState *s)
{
    /* RC scope: PQ transfer only.  HLG / BT.1886 are deferred follow-ups
     * (single-sourced constants — see ADR-1110). */
    if (!s->transfer || strcmp(s->transfer, "pq") != 0) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "delta_e_itp: transfer=\"%s\" is not supported; only \"pq\" (ST-2084) is "
                 "implemented in this release (HLG/BT.1886 are deferred)\n",
                 s->transfer ? s->transfer : "(null)");
        return -EINVAL;
    }

    if (s->matrix && strcmp(s->matrix, "bt709") == 0) {
        s->kr = DEITP_KR_BT709;
        s->kg = DEITP_KG_BT709;
        s->kb = DEITP_KB_BT709;
    } else if (!s->matrix || strcmp(s->matrix, "bt2020") == 0) {
        s->kr = DEITP_KR_BT2020;
        s->kg = DEITP_KG_BT2020;
        s->kb = DEITP_KB_BT2020;
    } else {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "delta_e_itp: matrix=\"%s\" is not supported; use \"bt2020\" or \"bt709\"\n",
                 s->matrix);
        return -EINVAL;
    }

    if (s->range && strcmp(s->range, "full") == 0) {
        s->full_range = 1;
    } else if (!s->range || strcmp(s->range, "limited") == 0) {
        s->full_range = 0;
    } else {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "delta_e_itp: range=\"%s\" is not supported; use \"limited\" or \"full\"\n",
                 s->range);
        return -EINVAL;
    }

    return 0;
}

static int init(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc, unsigned w,
                unsigned h)
{
    assert(fex && fex->priv && w > 0 && h > 0);
    DeltaEItpState *s = fex->priv;
    int err = 0;

    if (pix_fmt == VMAF_PIX_FMT_YUV400P) {
        vmaf_log(
            VMAF_LOG_LEVEL_ERROR,
            "delta_e_itp: YUV400 (no chroma) is not supported by a colour-difference metric\n");
        return -EINVAL;
    }

    err = resolve_options(s);
    if (err) {
        return err;
    }

    s->scale_chroma_planes = NULL;

    if (pix_fmt == VMAF_PIX_FMT_YUV444P) {
        return 0;
    }

    switch (bpc) {
    case 8:
        s->scale_chroma_planes = scale_chroma_planes;
        break;
    case 10:
    case 12:
    case 16:
        s->scale_chroma_planes = scale_chroma_planes_hbd;
        break;
    default:
        return -EINVAL;
    }

    err = vmaf_picture_alloc(&s->ref, VMAF_PIX_FMT_YUV444P, bpc, w, h);
    if (err) {
        return err;
    }
    err = vmaf_picture_alloc(&s->dist, VMAF_PIX_FMT_YUV444P, bpc, w, h);
    if (err) {
        (void)vmaf_picture_unref(&s->ref);
        return err;
    }
    return 0;
}

/* Convert one pixel of a YUV picture at (i, j) into its scaled ICtCp
 * ("ITP") triple.  Returns 0 or -EINVAL on an unsupported bpc. */
static int pixel_to_itp(const DeltaEItpState *s, const VmafPicture *pic, unsigned i, unsigned j,
                        double itp[3])
{
    assert(s && pic && itp);
    double yuv[3] = {0.0, 0.0, 0.0};
    int err = read_yuv_sample(pic, i, j, yuv);
    if (err) {
        return err;
    }
    double rgb[3];
    yuv_to_rgb_prime(s, yuv[0], yuv[1], yuv[2], pic->bpc, rgb);
    rgb_pq_to_itp(rgb[0], rgb[1], rgb[2], itp);
    return 0;
}

static int extract(VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                   VmafPicture *dist_pic, VmafPicture *dist_pic_90, unsigned index,
                   VmafFeatureCollector *feature_collector)
{
    assert(fex && fex->priv && ref_pic && dist_pic);
    DeltaEItpState *s = fex->priv;
    (void)ref_pic_90;
    (void)dist_pic_90;

    VmafPicture *ref;
    VmafPicture *dist;

    if (ref_pic->pix_fmt == VMAF_PIX_FMT_YUV444P) {
        ref = ref_pic;
        dist = dist_pic;
    } else {
        ref = &s->ref;
        dist = &s->dist;
        s->scale_chroma_planes(ref_pic, ref);
        s->scale_chroma_planes(dist_pic, dist);
    }

    double de_sum = 0.0;

    for (unsigned i = 0; i < ref->h[0]; i++) {
        for (unsigned j = 0; j < ref->w[0]; j++) {
            double ref_itp[3];
            double dist_itp[3];
            int err = pixel_to_itp(s, ref, i, j, ref_itp);
            if (err) {
                return err;
            }
            err = pixel_to_itp(s, dist, i, j, dist_itp);
            if (err) {
                return err;
            }
            de_sum += delta_e_itp_pair(ref_itp, dist_itp);
        }
    }

    const double score = de_sum / ((double)ref_pic->w[0] * (double)ref_pic->h[0]);
    return vmaf_feature_collector_append(feature_collector, "delta_e_itp", score, index);
}

static int close(VmafFeatureExtractor *fex)
{
    assert(fex && fex->priv);
    DeltaEItpState *s = fex->priv;
    if (s->ref.ref != NULL) {
        (void)vmaf_picture_unref(&s->ref);
    }
    if (s->dist.ref != NULL) {
        (void)vmaf_picture_unref(&s->dist);
    }
    return 0;
}

static const VmafOption options[] = {
    {
        .name = "transfer",
        .help = "Electro-optical transfer function assumed for the input. Only 'pq' (SMPTE "
                "ST-2084) is supported in this release; 'hlg' and 'bt1886' are deferred. Default: "
                "'pq'.",
        .offset = offsetof(DeltaEItpState, transfer),
        .type = VMAF_OPT_TYPE_STRING,
        .default_val.s = DEFAULT_DEITP_TRANSFER,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "matrix",
        .help = "YUV->R'G'B' colour matrix. Possible values: ['bt2020', 'bt709']. Default: "
                "'bt2020' (BT.2100 / HDR norm).",
        .offset = offsetof(DeltaEItpState, matrix),
        .type = VMAF_OPT_TYPE_STRING,
        .default_val.s = DEFAULT_DEITP_MATRIX,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "range",
        .help = "YUV quantization range. Possible values: ['limited', 'full']. Default: 'limited' "
                "(narrow/video range).",
        .offset = offsetof(DeltaEItpState, range),
        .type = VMAF_OPT_TYPE_STRING,
        .default_val.s = DEFAULT_DEITP_RANGE,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {0},
};

static const char *provided_features[] = {"delta_e_itp", NULL};

// NOLINTNEXTLINE(misc-use-internal-linkage): cross-TU registry pattern — external linkage required; referenced as `extern VmafFeatureExtractor vmaf_fex_delta_e_itp` by feature_extractor.cpp's feature_extractor_list[] (ADR-0278).
VmafFeatureExtractor vmaf_fex_delta_e_itp = {
    .name = "delta_e_itp",
    .init = init,
    .extract = extract,
    .close = close,
    .options = options,
    .priv_size = sizeof(DeltaEItpState),
    .provided_features = provided_features,
    /* Single per-pixel ITP-conversion kernel + double-precision sum
     * reduction per frame (same shape as ciede.c). The pow-heavy PQ
     * EOTFs make the kernel cost scale with frame size; AUTO + 720p
     * area (see ADR-0181 / ADR-0182 and the ciede.c rationale). */
    .chars =
        {
            .n_dispatches_per_frame = 1,
            .is_reduction_only = false,
            .min_useful_frame_area = 1280U * 720U,
            .dispatch_hint = VMAF_FEATURE_DISPATCH_AUTO,
        },
};
