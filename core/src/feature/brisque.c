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
 * BRISQUE — Blind/Referenceless Image Spatial Quality Evaluator
 * (Mittal, Moorthy, Bovik, IEEE TIP 21(12):4695-4708, 2012).
 *
 * No-reference (NR), opinion-aware spatial IQA. Scores the DISTORTED picture's
 * luma plane only (ref_pic / *_90 are (void)-discarded, mirroring CAMBI/NIQE).
 * Lower score = better quality (~0 pristine, rising toward ~100 for heavy
 * distortion); the reference prediction path does NOT clamp, and neither do we
 * (the OpenCV variant clamps to [0,100] and is a different model — ADR-1115).
 *
 * Pipeline (all double precision), replicating the gregfreeman MATLAB pipeline
 * that TRAINED the bundled allmodel (model/other_models/brisque_live.model):
 *   distorted luma (plane 0) -> double in [0,255] (any bpc scaled to /maxval*255)
 *   for scale in {1, 2}:
 *     mu     = filter2(gauss7x7, img)                                  (Eq.2)
 *     sigma  = sqrt(|filter2(gauss7x7, img^2) - mu^2|)                 (Eq.3)
 *     mscn   = (img - mu) / (sigma + 1)                                (Eq.1, C=1)
 *     GGD-fit(mscn)            -> push {alpha, sigma^2}                (2 features)
 *     for shift in {(0,1),(1,0),(1,1),(-1,1)}:
 *       pair = mscn * roll(mscn, shift)
 *       AGGD-fit(pair)         -> push {alpha, eta, lstd^2, rstd^2}   (16 features)
 *     img = imresize(img, 0.5)  (MATLAB antialiased bicubic)
 *   feature vector = [scale1 f1..f18, scale2 f1..f18]  (36-D)
 *   range-scale to [-1,1] via inline min_[36]/max_[36]
 *   score = svm_predict(allmodel, x)
 *
 * Numeric kernels live in brisque_math.h. The SVR model is embedded at BUILD
 * time (xxd custom_target, the same path libvmaf's JSON models take) and
 * declared in brisque_model.h as src_brisque_live_model[] / _len — loaded once
 * at init() via svm_parse_model_from_buffer(). The `model_path` option lets a
 * caller override that with an on-disk libsvm model (svm_load_model), which is
 * also the only loader when built_in_models is disabled. See
 * docs/metrics/brisque.md and docs/adr/1115-brisque-nr-metric.md.
 */

#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "libvmaf/picture.h"

#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature/brisque_math.h"
#include "feature/brisque_model.h"
#include "log.h"
#include "mem.h"
#include "svm.h"

#define BRISQUE_FEATPERSCALE 18
#define BRISQUE_SCALENUM 2
#define BRISQUE_FEAT_DIM (BRISQUE_FEATPERSCALE * BRISQUE_SCALENUM) /* 36 */
#define BRISQUE_NUM_SHIFTS 4

/* libsvm range-scale arrays (min_[36] / max_[36]). SOURCE OF TRUTH: the INLINE
 * arrays from the reference computescore.cpp prediction path — the array the
 * trained allmodel expects. These are NOT the separate C++/allrange file, which
 * is a different, prediction-inconsistent set the reference code never reads;
 * substituting it would corrupt every score (ADR-1115, model card). */
static const double brisque_min[BRISQUE_FEAT_DIM] = {
    0.336999, 0.019667, 0.230000,  -0.125959, 0.000167, 0.000616, 0.231000,  -0.125873, 0.000165,
    0.000600, 0.241000, -0.128814, 0.000179,  0.000386, 0.243000, -0.133080, 0.000182,  0.000421,
    0.436998, 0.016929, 0.247000,  -0.200231, 0.000104, 0.000834, 0.257000,  -0.200017, 0.000112,
    0.000876, 0.257000, -0.155072, 0.000112,  0.000356, 0.258000, -0.154374, 0.000117,  0.000351};

static const double brisque_max[BRISQUE_FEAT_DIM] = {
    9.999411, 0.807472, 1.644021, 0.202917, 0.712384, 0.468672, 1.644021, 0.169548, 0.713132,
    0.467896, 1.553016, 0.101368, 0.687324, 0.533087, 1.554016, 0.101000, 0.689177, 0.533133,
    3.639918, 0.800955, 1.096995, 0.175286, 0.755547, 0.399270, 1.095995, 0.155928, 0.751488,
    0.402398, 1.041992, 0.093209, 0.623516, 0.532925, 1.042992, 0.093714, 0.621958, 0.534484};

/* Paired-product shifts {(dy,dx)} = H, V, D1, D2 (brisque_feature.m). */
static const int brisque_shifts[BRISQUE_NUM_SHIFTS][2] = {{0, 1}, {1, 0}, {1, 1}, {-1, 1}};

typedef struct BrisqueState {
    /* Option: override path to an on-disk libsvm model. NULL (the default)
     * uses the build-time-embedded allmodel; required when built_in_models is
     * disabled. Owned by the options machinery — do not free here. */
    char *model_path;

    unsigned w;
    unsigned h;
    unsigned bpc;
    int hdr_warned; /* one-shot PQ/HLG warning latch */

    /* Half-resolution dimensions (ceil(w/2), ceil(h/2)). */
    unsigned w2;
    unsigned h2;

    double window[BRISQUE_WIN_LEN]; /* separable 7-tap Gaussian */

    /* Gamma ratio tables (GGD and AGGD), BRISQUE_GAMMA_COUNT each. */
    double *ggd_table;
    double *aggd_table;

    /* Full-res working buffers (double). */
    double *img;  /* w*h    luma (then downsampled in place per scale) */
    double *tmp;  /* w*h    convolution scratch */
    double *mu;   /* w*h    local mean */
    double *mscn; /* w*h    MSCN field */
    double *pair; /* w*h    paired product scratch */

    /* Downsample scratch + half-res destination. */
    double *resize_tmp; /* w2*h  intermediate after the vertical pass */
    double *img2;       /* w2*h2 downsampled luma */

    /* Resize coefficient tables (vertical h->h2, horizontal w->w2). */
    int *vidx;
    double *vwts;
    int *hidx;
    double *hwts;

    struct svm_model *model;
    struct svm_node x[BRISQUE_FEAT_DIM + 1]; /* 36 features + terminator */
    double feat[BRISQUE_FEAT_DIM];
} BrisqueState;

/* ------------------------------------------------------------------ */
/* Separable correlate with ZERO boundary (MATLAB filter2 'same').     */
/* The 7-tap window is symmetric, so correlation == convolution.       */
/* `src` and `dst` must not alias.                                     */
/* ------------------------------------------------------------------ */
static void brisque_filter2(const double *src, double *dst, double *scratch, unsigned w, unsigned h,
                            const double window[BRISQUE_WIN_LEN])
{
    const int lw = BRISQUE_WIN_LW;
    /* Vertical (rows) pass: src -> scratch. */
    for (unsigned y = 0; y < h; y++) {
        for (unsigned x = 0; x < w; x++) {
            double acc = 0.0;
            for (int k = -lw; k <= lw; k++) {
                const long yy = (long)y + k;
                if (yy < 0 || yy >= (long)h)
                    continue; /* zero boundary */
                acc += src[(size_t)yy * w + x] * window[k + lw];
            }
            scratch[(size_t)y * w + x] = acc;
        }
    }
    /* Horizontal (cols) pass: scratch -> dst. */
    for (unsigned y = 0; y < h; y++) {
        for (unsigned x = 0; x < w; x++) {
            double acc = 0.0;
            for (int k = -lw; k <= lw; k++) {
                const long xx = (long)x + k;
                if (xx < 0 || xx >= (long)w)
                    continue; /* zero boundary */
                acc += scratch[(size_t)y * w + (size_t)xx] * window[k + lw];
            }
            dst[(size_t)y * w + x] = acc;
        }
    }
}

/* Compute the MSCN field of `img` (w*h) into `mscn`. Uses `tmp` and `mu` as
 * scratch (all w*h). mscn = (img - mu) / (sigma + 1), sigma = sqrt(|E[img^2] -
 * mu^2|). `tmp` is reused for both the squared image and the convolution
 * scratch. */
static void brisque_compute_mscn(const double *img, double *tmp, double *mu, double *sq_scratch,
                                 double *mscn, unsigned w, unsigned h,
                                 const double window[BRISQUE_WIN_LEN])
{
    const size_t n = (size_t)w * h;
    /* mu = filter2(img). tmp is the convolution scratch. */
    brisque_filter2(img, mu, tmp, w, h, window);

    /* sq_scratch = img^2, then filter2 into mscn (reused as E[img^2]), tmp
     * scratch. */
    for (size_t i = 0; i < n; i++)
        sq_scratch[i] = img[i] * img[i];
    brisque_filter2(sq_scratch, mscn, tmp, w, h, window);

    for (size_t i = 0; i < n; i++) {
        const double var = mscn[i] - mu[i] * mu[i];
        const double sigma = sqrt(fabs(var));
        mscn[i] = (img[i] - mu[i]) / (sigma + 1.0);
    }
}

/* Form the paired product of the MSCN field for one shift index into `pair`:
 *   pair[y,x] = mscn[y,x] * mscn[(y-dy) mod h, (x-dx) mod w]
 * matching np.roll wraparound (brisque_feature.m circshift). */
static void brisque_paired_product(const double *mscn, double *pair, unsigned w, unsigned h, int dy,
                                   int dx)
{
    for (unsigned y = 0; y < h; y++) {
        /* (y - dy) mod h, branch-free for |dy| <= 1. */
        long sy = (long)y - dy;
        if (sy < 0) {
            sy += (long)h;
        } else if (sy >= (long)h) {
            sy -= (long)h;
        }
        for (unsigned x = 0; x < w; x++) {
            long sx = (long)x - dx;
            if (sx < 0) {
                sx += (long)w;
            } else if (sx >= (long)w) {
                sx -= (long)w;
            }
            pair[(size_t)y * w + x] = mscn[(size_t)y * w + x] * mscn[(size_t)sy * w + (size_t)sx];
        }
    }
}

/* Build the 18 features of one scale from its MSCN field into out[0..17]. */
static void brisque_scale_features(const BrisqueState *s, const double *mscn, unsigned w,
                                   unsigned h, double *pair, double *out)
{
    const size_t n = (size_t)w * h;

    /* f1, f2: GGD fit of the MSCN field. */
    const BrisqueGgd g = brisque_fit_ggd(mscn, n, s->ggd_table);
    out[0] = g.alpha;
    out[1] = g.sigma_sq;

    /* f3..f18: AGGD fit of each paired product. */
    for (int sft = 0; sft < BRISQUE_NUM_SHIFTS; sft++) {
        brisque_paired_product(mscn, pair, w, h, brisque_shifts[sft][0], brisque_shifts[sft][1]);
        const BrisqueAggd a = brisque_fit_aggd(pair, n, s->aggd_table);
        double *p = out + 2 + (size_t)sft * 4;
        p[0] = a.alpha;
        p[1] = a.eta;
        p[2] = a.left_sq;
        p[3] = a.right_sq;
    }
}

/* MATLAB imresize 0.5x downscale of `src` (w*h) into `dst` (w2*h2). Vertical
 * (rows) pass into `vtmp` (w*h2 region, stride w), then horizontal (cols) pass
 * into `dst`. Coefficient tables are precomputed in init(). */
static void brisque_downsample(const BrisqueState *s, const double *src, double *vtmp, double *dst)
{
    const unsigned w = s->w;
    const unsigned h2 = s->h2;
    const unsigned w2 = s->w2;
    /* Vertical pass: h -> h2, full width w. Output rows stored with stride w. */
    for (unsigned oy = 0; oy < h2; oy++) {
        const int *iy = s->vidx + (size_t)oy * BRISQUE_RESIZE_TAPS;
        const double *wy = s->vwts + (size_t)oy * BRISQUE_RESIZE_TAPS;
        for (unsigned x = 0; x < w; x++) {
            double acc = 0.0;
            for (int t = 0; t < BRISQUE_RESIZE_TAPS; t++)
                acc += src[(size_t)iy[t] * w + x] * wy[t];
            vtmp[(size_t)oy * w + x] = acc;
        }
    }
    /* Horizontal pass: w -> w2, on the h2 rows of vtmp (stride w). */
    for (unsigned ox = 0; ox < w2; ox++) {
        const int *ix = s->hidx + (size_t)ox * BRISQUE_RESIZE_TAPS;
        const double *wx = s->hwts + (size_t)ox * BRISQUE_RESIZE_TAPS;
        for (unsigned y = 0; y < h2; y++) {
            double acc = 0.0;
            for (int t = 0; t < BRISQUE_RESIZE_TAPS; t++)
                acc += vtmp[(size_t)y * w + (size_t)ix[t]] * wx[t];
            dst[(size_t)y * w2 + ox] = acc;
        }
    }
}

/* Read the distorted luma plane into `out` (w*h) as double scaled to [0,255].
 * SDR-luma trained model: any bit depth is scaled to the [0,255] working range
 * (value * 255 / (2^bpc - 1)). */
static void brisque_read_luma(const VmafPicture *pic, double *out, unsigned w, unsigned h)
{
    if (pic->bpc <= 8) {
        const uint8_t *base = pic->data[0];
        const ptrdiff_t stride = pic->stride[0];
        for (unsigned y = 0; y < h; y++) {
            const uint8_t *row = base + (size_t)y * (size_t)stride;
            for (unsigned x = 0; x < w; x++)
                out[(size_t)y * w + x] = (double)row[x];
        }
    } else {
        const uint8_t *base = pic->data[0];
        const ptrdiff_t stride = pic->stride[0];
        const double scale = 255.0 / (double)((1u << pic->bpc) - 1u);
        for (unsigned y = 0; y < h; y++) {
            const uint16_t *row = (const uint16_t *)(base + (size_t)y * (size_t)stride);
            for (unsigned x = 0; x < w; x++)
                out[(size_t)y * w + x] = (double)row[x] * scale;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Lifecycle: init / extract / close.                                  */
/* ------------------------------------------------------------------ */

/* Allocate every working buffer + coefficient table into `s`. Returns 0 on
 * success, -ENOMEM on the first failed allocation. close() frees whatever was
 * acquired (aligned_free(NULL) is a no-op), so a partial failure unwinds
 * cleanly. Split out of init() to stay under the function-size threshold. */
static int brisque_alloc_buffers(BrisqueState *s)
{
    const size_t plane = (size_t)s->w * s->h;
    const size_t plane2 = (size_t)s->w2 * s->h2;
    const size_t vtmp_n = (size_t)s->w * s->h2; /* vertical-pass intermediate */
    const size_t d = sizeof(double);
    const size_t taps = BRISQUE_RESIZE_TAPS;
    assert(plane > 0 && plane2 > 0);

    s->ggd_table = aligned_malloc(BRISQUE_GAMMA_COUNT * d, 32);
    s->aggd_table = aligned_malloc(BRISQUE_GAMMA_COUNT * d, 32);
    s->img = aligned_malloc(plane * d, 32);
    s->tmp = aligned_malloc(plane * d, 32);
    s->mu = aligned_malloc(plane * d, 32);
    s->mscn = aligned_malloc(plane * d, 32);
    s->pair = aligned_malloc(plane * d, 32);
    s->resize_tmp = aligned_malloc(vtmp_n * d, 32);
    s->img2 = aligned_malloc(plane2 * d, 32);
    s->vidx = aligned_malloc((size_t)s->h2 * taps * sizeof(int), 32);
    s->vwts = aligned_malloc((size_t)s->h2 * taps * d, 32);
    s->hidx = aligned_malloc((size_t)s->w2 * taps * sizeof(int), 32);
    s->hwts = aligned_malloc((size_t)s->w2 * taps * d, 32);

    if (!s->ggd_table || !s->aggd_table || !s->img || !s->tmp || !s->mu || !s->mscn || !s->pair ||
        !s->resize_tmp || !s->img2 || !s->vidx || !s->vwts || !s->hidx || !s->hwts) {
        return -ENOMEM;
    }
    return 0;
}

/* Free all aligned buffers (NULL-guarded; aligned_free(NULL) is a no-op).
 * Used by close() AND by the init() failure paths — the framework does NOT
 * call close() after a failed init(), so init() must free its own partials. */
static void brisque_free_buffers(BrisqueState *s)
{
    aligned_free(s->ggd_table);
    aligned_free(s->aggd_table);
    aligned_free(s->img);
    aligned_free(s->tmp);
    aligned_free(s->mu);
    aligned_free(s->mscn);
    aligned_free(s->pair);
    aligned_free(s->resize_tmp);
    aligned_free(s->img2);
    aligned_free(s->vidx);
    aligned_free(s->vwts);
    aligned_free(s->hidx);
    aligned_free(s->hwts);
}

/* Load the EPSILON_SVR model into s->model. Resolution order:
 *   1. s->model_path set (the `model_path` option) -> svm_load_model(path).
 *      Used to override the bundled model or when built_in_models is off.
 *   2. otherwise -> the build-time-embedded allmodel buffer
 *      (svm_parse_model_from_buffer), present iff VMAF_BRISQUE_BUILT_IN_MODEL.
 * Returns 0 on success; -EINVAL when no model can be loaded (with a clear log).
 * Split out of init() to keep that function under the size threshold. */
static int brisque_load_model(BrisqueState *s)
{
    if (s->model_path && s->model_path[0] != '\0') {
        s->model = svm_load_model(s->model_path);
        if (!s->model) {
            vmaf_log(VMAF_LOG_LEVEL_ERROR,
                     "brisque: could not load libsvm model from model_path=\"%s\" "
                     "(missing file or not a valid libsvm text model). "
                     "See docs/metrics/brisque.md.\n",
                     s->model_path);
            return -EINVAL;
        }
        return 0;
    }

#if VMAF_BRISQUE_BUILT_IN_MODEL
    s->model = svm_parse_model_from_buffer((const char *)src_brisque_live_model,
                                           src_brisque_live_model_len);
    if (!s->model) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "brisque: failed to parse the built-in allmodel\n");
        return -EINVAL;
    }
    return 0;
#else
    vmaf_log(VMAF_LOG_LEVEL_ERROR, "brisque: no model available — this build was configured with "
                                   "built_in_models disabled (or without xxd -n support), so the "
                                   "allmodel is not embedded. Pass the model explicitly, e.g. "
                                   "--feature brisque=model_path=/path/to/brisque_live.model. "
                                   "See docs/metrics/brisque.md.\n");
    return -EINVAL;
#endif /* VMAF_BRISQUE_BUILT_IN_MODEL */
}

static int init(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc, unsigned w,
                unsigned h)
{
    (void)pix_fmt;
    assert(fex != NULL);
    assert(fex->priv != NULL);
    assert(w > 0 && h > 0);
    BrisqueState *s = fex->priv;

    /* The 7x7 window must fit, and a 0.5x downscale must leave a non-empty
     * second scale. w,h >= 7 guarantees both (ceil(7/2)=4 >= 7-tap? no — but
     * the filter zero-pads, so any >=1 scale-2 plane is valid). Require >= 7
     * so scale-1 has a meaningful neighbourhood. */
    if (w < BRISQUE_WIN_LEN || h < BRISQUE_WIN_LEN)
        return -EINVAL;

    s->w = w;
    s->h = h;
    s->bpc = bpc;
    s->hdr_warned = 0;
    s->w2 = (unsigned)brisque_resize_out_len((int)w); /* ceil(w/2) */
    s->h2 = (unsigned)brisque_resize_out_len((int)h); /* ceil(h/2) */

    brisque_build_window(s->window);

    const int rc = brisque_alloc_buffers(s);
    if (rc != 0) {
        brisque_free_buffers(s);
        return rc;
    }

    brisque_build_ggd_table(s->ggd_table);
    brisque_build_aggd_table(s->aggd_table);
    brisque_resize_coeffs((int)h, (int)s->h2, s->vidx, s->vwts);
    brisque_resize_coeffs((int)w, (int)s->w2, s->hidx, s->hwts);

    const int model_rc = brisque_load_model(s);
    if (model_rc != 0) {
        brisque_free_buffers(s);
        return model_rc;
    }

    return 0;
}

/* One-shot warning for HDR transfer characteristics: BRISQUE is SDR-luma
 * trained, no HDR model exists. We still produce a (non-meaningful) score so
 * the pipeline does not error, but warn once per extractor instance. */
static void brisque_maybe_warn_hdr(BrisqueState *s, const VmafPicture *pic)
{
    if (s->hdr_warned)
        return;
    if (pic->bpc > 8) {
        /* Heuristic: >8bpc is the necessary (not sufficient) condition for PQ/
         * HLG. VmafPicture carries no transfer-characteristic field, so we
         * cannot distinguish SDR-10bit from PQ/HLG here; warn conservatively
         * that HDR is unsupported and the score assumes SDR. */
        vmaf_log(VMAF_LOG_LEVEL_WARNING,
                 "brisque: >8-bit input scored as SDR; PQ/HLG HDR is out of scope "
                 "(no HDR-trained BRISQUE model). Score is not meaningful for HDR. "
                 "See docs/metrics/brisque.md.\n");
        s->hdr_warned = 1;
    }
}

static int extract(VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                   VmafPicture *dist_pic, VmafPicture *dist_pic_90, unsigned index,
                   VmafFeatureCollector *feature_collector)
{
    (void)ref_pic;
    (void)ref_pic_90;
    (void)dist_pic_90;
    assert(fex != NULL);
    assert(fex->priv != NULL);
    assert(dist_pic != NULL);
    assert(feature_collector != NULL);
    BrisqueState *s = fex->priv;

    brisque_maybe_warn_hdr(s, dist_pic);

    /* Stage 1: distorted luma -> double in [0,255]. */
    brisque_read_luma(dist_pic, s->img, s->w, s->h);

    /* Stage 2 (scale 1): MSCN + 18 features over the full-res field. */
    brisque_compute_mscn(s->img, s->tmp, s->mu, s->pair, s->mscn, s->w, s->h, s->window);
    brisque_scale_features(s, s->mscn, s->w, s->h, s->pair, s->feat);

    /* Stage 3 (scale 2): downsample 0.5x, MSCN + 18 features over half-res. */
    brisque_downsample(s, s->img, s->resize_tmp, s->img2);
    brisque_compute_mscn(s->img2, s->tmp, s->mu, s->pair, s->mscn, s->w2, s->h2, s->window);
    brisque_scale_features(s, s->mscn, s->w2, s->h2, s->pair, s->feat + BRISQUE_FEATPERSCALE);

    /* Stage 4: range-scale to [-1,1] (no clamp) and build the svm_node vector. */
    for (size_t i = 0; i < (size_t)BRISQUE_FEAT_DIM; i++) {
        s->x[i].index = (int)i + 1;
        s->x[i].value = brisque_range_scale(s->feat[i], brisque_min[i], brisque_max[i]);
    }
    s->x[(size_t)BRISQUE_FEAT_DIM].index = -1; /* terminator */

    /* Stage 5: EPSILON_SVR predict (== svm_predict_probability for EPS_SVR). */
    const double score = svm_predict(s->model, s->x);
    assert(isfinite(score));

    return vmaf_feature_collector_append(feature_collector, "brisque", score, index);
}

static int close(VmafFeatureExtractor *fex)
{
    assert(fex != NULL);
    BrisqueState *s = fex->priv;
    if (!s)
        return 0;
    if (s->model)
        svm_free_and_destroy_model(&s->model);
    brisque_free_buffers(s);
    return 0;
}

static const char *provided_features[] = {"brisque", NULL};

static const VmafOption options[] = {
    {
        .name = "model_path",
        .help = "Path to an on-disk libsvm EPSILON_SVR model overriding the "
                "build-time-embedded LIVE allmodel. Required when libvmaf was "
                "built with built_in_models disabled.",
        .offset = offsetof(BrisqueState, model_path),
        .type = VMAF_OPT_TYPE_STRING,
        .default_val.s = NULL,
    },
    {0},
};

/* External linkage is required — the extractor registry iterates over
 * `vmaf_fex_*` externs in core/src/feature/feature_extractor.cpp. */
// NOLINTNEXTLINE(misc-use-internal-linkage,cppcoreguidelines-avoid-non-const-global-variables)
VmafFeatureExtractor vmaf_fex_brisque = {
    .name = "brisque",
    .init = init,
    .extract = extract,
    .close = close,
    .options = options,
    .priv_size = sizeof(BrisqueState),
    .provided_features = provided_features,
};
