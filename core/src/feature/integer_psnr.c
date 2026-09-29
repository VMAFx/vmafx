/**
 *
 *  Copyright 2016-2026 Netflix, Inc.
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

#include <errno.h>
#include <float.h>
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "opt.h"
#include "psnr_score.h"

#if ARCH_X86
#include "x86/psnr_avx2.h"
#if HAVE_AVX512
#include "x86/psnr_avx512.h"
#endif
#endif

#if ARCH_AARCH64
#include "arm64/psnr_neon.h"
#endif

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `NULL` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

typedef struct PsnrState {
    bool enable_chroma;
    bool enable_mse;
    bool enable_apsnr;
    bool reduced_hbd_peak;
    uint32_t peak;
    double psnr_max[3];
    double min_sse;
    /* ADR-1193 / T-UPSTREAM-1109: when true, `psnr_max[p]` keeps only its
     * `mse == 0` infinity-sentinel role and stops truncating genuinely
     * computed values above it. Default false keeps every shipped score
     * bit-identical. */
    bool uncapped;
    struct {
        uint64_t sse[3];
        uint64_t n_pixels[3];
    } apsnr;
    uint32_t (*sse_line_8)(const uint8_t *ref, const uint8_t *dis, unsigned w);
    uint64_t (*sse_line_16)(const uint16_t *ref, const uint16_t *dis, unsigned w);
} PsnrState;

static const VmafOption options[] = {
    {
        .name = "enable_chroma",
        .help = "enable calculation for chroma channels",
        .offset = offsetof(PsnrState, enable_chroma),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = true,
    },
    {
        .name = "enable_mse",
        .help = "enable MSE calculation",
        .offset = offsetof(PsnrState, enable_mse),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = false,
    },
    {
        .name = "enable_apsnr",
        .help = "enable APSNR calculation",
        .offset = offsetof(PsnrState, enable_apsnr),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = false,
    },
    {
        .name = "reduced_hbd_peak",
        .help = "reduce hbd peak value to align with scaled 8-bit content",
        .offset = offsetof(PsnrState, reduced_hbd_peak),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = false,
    },
    {
        .name = "min_sse",
        .help = "constrain the minimum possible sse",
        .offset = offsetof(PsnrState, min_sse),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = 0.0,
        .min = 0.0,
        .max = DBL_MAX,
    },
    {
        .name = "uncapped",
        .help = "report the true PSNR instead of truncating at the psnr_max ceiling "
                "(an all-zero SSE still reports psnr_max)",
        .offset = offsetof(PsnrState, uncapped),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = false,
    },
    {0}};

static uint32_t sse_line_8_c(const uint8_t *ref, const uint8_t *dis, unsigned w)
{
    uint32_t sse = 0;
    for (unsigned j = 0; j < w; j++) {
        const int16_t e = ref[j] - dis[j];
        sse += e * e;
    }
    return sse;
}

static uint64_t sse_line_16_c(const uint16_t *ref, const uint16_t *dis, unsigned w)
{
    uint64_t sse = 0;
    for (unsigned j = 0; j < w; j++) {
        const uint32_t e = abs(ref[j] - dis[j]);
        sse += (uint64_t)e * e;
    }
    return sse;
}

static int init(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc, unsigned w,
                unsigned h)
{
    PsnrState *s = fex->priv;
    s->peak = vmaf_psnr_peak(bpc, s->reduced_hbd_peak);

    if (pix_fmt == VMAF_PIX_FMT_YUV400P)
        s->enable_chroma = false;

    const int ss_hor = pix_fmt != VMAF_PIX_FMT_YUV444P;
    const int ss_ver = pix_fmt == VMAF_PIX_FMT_YUV420P;
    for (unsigned i = 0; i < 3; i++) {
        /* Ceiling division for chroma plane dimensions — mirrors picture.c
         * fix (Research-0094): odd luma → ceil(luma/2) chroma samples. */
        const unsigned pw = (i && ss_hor) ? (w + 1u) >> 1 : w;
        const unsigned ph = (i && ss_ver) ? (h + 1u) >> 1 : h;
        s->psnr_max[i] = vmaf_psnr_max(bpc, s->peak, s->min_sse, pw, ph);
    }

    s->sse_line_8 = sse_line_8_c;
    s->sse_line_16 = sse_line_16_c;

#if ARCH_X86
    unsigned flags = vmaf_get_cpu_flags();
    if (flags & VMAF_X86_CPU_FLAG_AVX2) {
        s->sse_line_8 = psnr_sse_line_8_avx2;
        s->sse_line_16 = psnr_sse_line_16_avx2;
    }
#if HAVE_AVX512
    if (flags & VMAF_X86_CPU_FLAG_AVX512) {
        s->sse_line_8 = psnr_sse_line_8_avx512;
        s->sse_line_16 = psnr_sse_line_16_avx512;
    }
#endif
#endif

#if ARCH_AARCH64
    unsigned flags = vmaf_get_cpu_flags();
    if (flags & VMAF_ARM_CPU_FLAG_NEON) {
        s->sse_line_8 = psnr_sse_line_8_neon;
        s->sse_line_16 = psnr_sse_line_16_neon;
    }
#endif

    return 0;
}

static char *mse_name[3] = {"mse_y", "mse_cb", "mse_cr"};
static char *psnr_name[3] = {"psnr_y", "psnr_cb", "psnr_cr"};

static int psnr(VmafPicture *ref_pic, VmafPicture *dist_pic, unsigned index,
                VmafFeatureCollector *feature_collector, PsnrState *s)
{
    const uint8_t peak = 255;
    const unsigned n = s->enable_chroma ? 3 : 1;

    int err = 0;

    for (unsigned p = 0; p < n; p++) {
        uint8_t *ref = ref_pic->data[p];
        uint8_t *dis = dist_pic->data[p];

        uint64_t sse = 0;
        uint32_t (*sse_fn)(const uint8_t *, const uint8_t *, unsigned) =
            s->sse_line_8 ? s->sse_line_8 : sse_line_8_c;
        for (unsigned i = 0; i < ref_pic->h[p]; i++) {
            sse += sse_fn(ref, dis, ref_pic->w[p]);
            ref += ref_pic->stride[p];
            dis += dist_pic->stride[p];
        }

        if (s->enable_apsnr) {
            s->apsnr.sse[p] += sse;
            s->apsnr.n_pixels[p] += (uint64_t)ref_pic->h[p] * ref_pic->w[p];
        }

        const double mse = ((double)sse) / (ref_pic->w[p] * ref_pic->h[p]);
        const double psnr =
            vmaf_psnr_from_mse(mse, (double)peak * peak, s->psnr_max[p], s->uncapped);

        err |= vmaf_feature_collector_append(feature_collector, psnr_name[p], psnr, index);
        if (s->enable_mse) {
            err |= vmaf_feature_collector_append(feature_collector, mse_name[p], mse, index);
        }
    }

    return err;
}

static int psnr_hbd(VmafPicture *ref_pic, VmafPicture *dist_pic, unsigned index,
                    VmafFeatureCollector *feature_collector, PsnrState *s)
{
    const unsigned n = s->enable_chroma ? 3 : 1;

    int err = 0;

    for (unsigned p = 0; p < n; p++) {
        uint16_t *ref = ref_pic->data[p];
        uint16_t *dis = dist_pic->data[p];

        uint64_t sse = 0;
        uint64_t (*sse_fn)(const uint16_t *, const uint16_t *, unsigned) =
            s->sse_line_16 ? s->sse_line_16 : sse_line_16_c;
        for (unsigned i = 0; i < ref_pic->h[p]; i++) {
            sse += sse_fn(ref, dis, ref_pic->w[p]);
            ref += ref_pic->stride[p] / 2;
            dis += dist_pic->stride[p] / 2;
        }

        if (s->enable_apsnr) {
            s->apsnr.sse[p] += sse;
            s->apsnr.n_pixels[p] += (uint64_t)ref_pic->h[p] * ref_pic->w[p];
        }

        const double mse = ((double)sse) / (ref_pic->w[p] * ref_pic->h[p]);
        const double psnr =
            vmaf_psnr_from_mse(mse, (double)s->peak * s->peak, s->psnr_max[p], s->uncapped);

        err |= vmaf_feature_collector_append(feature_collector, psnr_name[p], psnr, index);
        if (s->enable_mse) {
            err |= vmaf_feature_collector_append(feature_collector, mse_name[p], mse, index);
        }
    }

    return err;
}

static int extract(VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                   VmafPicture *dist_pic, VmafPicture *dist_pic_90, unsigned index,
                   VmafFeatureCollector *feature_collector)
{
    PsnrState *s = fex->priv;

    (void)ref_pic_90;
    (void)dist_pic_90;

    switch (ref_pic->bpc) {
    case 8:
        return psnr(ref_pic, dist_pic, index, feature_collector, s);
    case 10:
    case 12:
    case 16:
        return psnr_hbd(ref_pic, dist_pic, index, feature_collector, s);
    default:
        return -EINVAL;
    }
}

static int flush(VmafFeatureExtractor *fex, VmafFeatureCollector *feature_collector)
{
    PsnrState *s = fex->priv;
    const char *apsnr_name[3] = {"apsnr_y", "apsnr_cb", "apsnr_cr"};

    int err = 0;
    if (s->enable_apsnr) {
        /* When chroma is disabled only luma (i=0) is accumulated; iterating
         * over disabled planes would invoke log10(0) yielding -inf / NaN.   */
        const unsigned n_planes = s->enable_chroma ? 3u : 1u;
        for (unsigned i = 0; i < n_planes; i++) {
            /* An all-zero SSE (identical clip) reports psnr_max instead of
             * log10(0) = -inf; the cap is 10*log10(peak^2 * n_pixels) — the
             * original "* 2" factor inflated it by one PSNR unit. */
            const double apsnr =
                vmaf_psnr_aggregate(s->peak, s->apsnr.sse[i], s->apsnr.n_pixels[i], s->psnr_max[i]);
            err |= vmaf_feature_collector_set_aggregate(feature_collector, apsnr_name[i], apsnr);
        }
    }

    return (err < 0) ? err : !err;
}

static const char *provided_features[] = {"psnr_y", "psnr_cb", "psnr_cr", NULL};

// NOLINTNEXTLINE(misc-use-internal-linkage): cross-TU registry pattern — external linkage required (ADR-0278).
VmafFeatureExtractor vmaf_fex_psnr = {
    .name = "psnr",
    .options = options,
    .init = init,
    .extract = extract,
    .flush = flush,
    .priv_size = sizeof(PsnrState),
    .provided_features = provided_features,
    .flags = VMAF_FEATURE_EXTRACTOR_TEMPORAL,
    /* Single per-pixel squared-error reduction per frame (per
     * channel; up to 3 if chroma is enabled). Reduction-dominated
     * — benefits least from graph replay. AUTO + 1080p area
     * matches motion's profile (see ADR-0181 / ADR-0182). */
    .chars =
        {
            .n_dispatches_per_frame = 1,
            .is_reduction_only = true,
            .min_useful_frame_area = 1920U * 1080U,
            .dispatch_hint = VMAF_FEATURE_DISPATCH_AUTO,
        },
};

/* NOLINTEND(modernize-use-nullptr) */
