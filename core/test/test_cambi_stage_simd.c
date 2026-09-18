/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 *  Bit-exact parity of every CAMBI per-stage SIMD kernel against the
 *  production scalar stage in cambi.c, on every ISA that has a twin.
 *
 *  Subjects, per ISA (AVX2, AVX-512, NEON): decimate, anti_dithering_filter,
 *  filter_mode, get_derivative_data_for_row, the histogram range updaters,
 *  calculate_c_values_row and the frame-level calculate_c_values driver.
 *  The spatial-mask dp / mask rows have their own test
 *  (test_cambi_spatial_mask_simd).
 *
 *  The reference is the shipped scalar code: this TU includes cambi.c, the way
 *  test_cambi.c does, so it calls the same static functions the extractor
 *  falls back to and cannot drift from them the way a private copy can
 *  (ADR-1207). The CPU-flag mask is cleared first, so the one scalar stage that
 *  dispatches internally (anti_dithering_filter) takes its scalar body. SIMD
 *  kernels are called directly, gated on CPUID.
 *
 *  Every kernel is integer-only except the c-value multiply, which is a single
 *  int-to-float conversion times a LUT entry with no fused add, so SIMD and
 *  scalar agree byte for byte; SIMD_BITEXACT_ASSERT_MEMCMP is the assertion,
 *  with no tolerance (ADR-0138 / ADR-0139).
 *
 *  Guard bands: every buffer is allocated as the compared region plus a
 *  sentinel-filled tail, pictures get a stride wider than the processed width
 *  and extra rows below it, and the whole allocation is compared. A SIMD tail
 *  that writes one element past the region the scalar writes shows up as a
 *  divergence in the padding or the guard.
 *
 *  Widths cover every tail residue of the 8-, 16- and 32-lane loops, sizes
 *  below and just above one vector, and odd widths. Value sets cover 8-bit
 *  input as CAMBI converts it (x << 2), 10-bit, full-range uint16, and
 *  low-entropy content that makes the mode filter and derivative take their
 *  tie branches.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test.h"
/* clang-format off — test.h has no header guard; must precede harness. */
#include "simd_bitexact_test.h"
/* clang-format on */

/* The reference must be the shipped file-static scalar stages, not a copy
 * (ADR-1207), so the TU is included, as test_cambi.c does. */
// NOLINTNEXTLINE(bugprone-suspicious-include) — ADR-0141 / ADR-1207: white-box reference to the static scalar stages.
#include "feature/cambi.c"
#include "feature/cambi_c_values_frame.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe. ADR-1138. */

typedef struct {
    const char *name;
    VmafDecimate decimate;
    VmafDecimate anti_dithering;
    VmafFilterMode filter_mode;
    VmafDerivativeCalculator derivative;
    VmafRangeUpdater inc;
    VmafRangeUpdater dec;
    CambiCValuesRow c_values_row;
    VmafCalcCValues c_values;
} StageKernels;

static const StageKernels g_scalar = {
    "scalar",
    decimate,
    anti_dithering_filter,
    filter_mode,
    get_derivative_data_for_row,
    increment_range,
    decrement_range,
    calculate_c_values_row,
    calculate_c_values,
};

/* Sentinel elements after every compared region. */
#define GUARD 67u
#define SENTINEL16 0xA5C3u

/* Every tail residue of the 8-lane (AVX2 c-values row, NEON), 16-lane and
 * 32-lane loops, widths below and just above one vector, odd widths. */
static const int g_widths[] = {1,  2,  3,   4,   5,   6,   7,   8,   9,   10,  11, 12,
                               13, 14, 15,  16,  17,  18,  23,  24,  25,  31,  32, 33,
                               34, 35, 47,  48,  49,  63,  64,  65,  66,  67,  95, 96,
                               97, 99, 127, 128, 129, 130, 173, 255, 256, 257, 577};
#define NUM_WIDTHS (sizeof(g_widths) / sizeof(g_widths[0]))

static const int g_heights[] = {1, 2, 3, 4, 7};
#define NUM_HEIGHTS (sizeof(g_heights) / sizeof(g_heights[0]))

enum Fill { FILL_8BIT, FILL_10BIT, FILL_16BIT, FILL_FEW, FILL_FLAT, FILL_RAMP, NUM_FILLS };

static char g_label[192];

/* ---- helpers --------------------------------------------------------- */

static uint16_t fill_value(enum Fill fill, int col, uint32_t *state)
{
    const uint32_t r = simd_test_xorshift32(state);
    switch (fill) {
    case FILL_8BIT:
        return (uint16_t)((r & 0xFFu) << 2);
    case FILL_10BIT:
        return (uint16_t)(r & 0x3FFu);
    case FILL_16BIT:
        return (uint16_t)r;
    case FILL_FEW:
        return (uint16_t)(500u + r % 3u);
    case FILL_FLAT:
        /* Mostly one value, so derivatives are mostly 1 and a misread edge shows. */
        return (uint16_t)((r & 15u) == 0u ? (r >> 8) & 0x3FFu : 512u);
    default:
        /* Banding-like ramp: steps of 4 across the row, with rare 1-LSB noise. */
        return (uint16_t)(300u + ((unsigned)col / 9u) * 4u + ((r & 63u) == 0u ? 1u : 0u));
    }
}

static void fill_guard16(uint16_t *buf, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        buf[i] = SENTINEL16;
    }
}

static char *compare_bytes(const void *scalar_buf, const void *simd_buf, size_t n_bytes)
{
    SIMD_BITEXACT_ASSERT_MEMCMP(scalar_buf, simd_buf, n_bytes, g_label);
    return NULL;
}

/* A plane with `rows` rows of `stride` elements plus a GUARD tail. The picture
 * view carries only what the stage kernels read: data[0] and stride[0]. */
typedef struct {
    uint16_t *buf;
    size_t n;
    int stride;
    int rows;
    VmafPicture pic;
} Plane;

static bool plane_alloc(Plane *p, int stride, int rows)
{
    p->stride = stride;
    p->rows = rows;
    p->n = (size_t)stride * (size_t)rows + GUARD;
    p->buf = simd_test_aligned_malloc(p->n * sizeof(uint16_t), 64);
    memset(&p->pic, 0, sizeof(p->pic));
    p->pic.data[0] = p->buf;
    p->pic.stride[0] = (ptrdiff_t)stride * (ptrdiff_t)sizeof(uint16_t);
    p->pic.w[0] = (unsigned)stride;
    p->pic.h[0] = (unsigned)rows;
    p->pic.bpc = 10;
    return p->buf != NULL;
}

/* Fills the data_w x data_h region with `fill` and everything else (stride
 * padding, rows below, the guard tail) with SENTINEL16, which no 10-bit sample
 * equals: a kernel that reads past the region it may read changes its output. */
static void fill_plane(Plane *p, int data_w, int data_h, enum Fill fill, uint32_t seed)
{
    uint32_t state = seed;
    fill_guard16(p->buf, p->n);
    for (int i = 0; i < data_h; i++) {
        uint16_t *row = &p->buf[(size_t)i * (size_t)p->stride];
        for (int j = 0; j < data_w; j++) {
            row[j] = fill_value(fill, j, &state);
        }
    }
}

static void plane_free(Plane *p)
{
    simd_test_aligned_free(p->buf);
    p->buf = NULL;
}

/* Two identical planes: one for the scalar stage, one for the SIMD stage. */
typedef struct {
    Plane s;
    Plane v;
} PlanePair;

static bool pair_alloc(PlanePair *pp, int stride, int rows, int data_w, int data_h, enum Fill fill,
                       uint32_t seed)
{
    /* Allocate both before testing either, so pair_free() always sees both. */
    const bool ok_s = plane_alloc(&pp->s, stride, rows);
    const bool ok_v = plane_alloc(&pp->v, stride, rows);
    const bool ok = ok_s && ok_v;
    if (ok) {
        fill_plane(&pp->s, data_w, data_h, fill, seed);
        memcpy(pp->v.buf, pp->s.buf, pp->s.n * sizeof(uint16_t));
    }
    return ok;
}

static void pair_free(PlanePair *pp)
{
    plane_free(&pp->s);
    plane_free(&pp->v);
}

static char *compare_pair(const PlanePair *pp)
{
    return compare_bytes(pp->s.buf, pp->v.buf, pp->s.n * sizeof(uint16_t));
}

/* ---- decimate, anti-dithering, mode filter --------------------------- */

typedef enum { STAGE_DECIMATE, STAGE_ANTI_DITHER, STAGE_FILTER_MODE } PictureStage;

static const char *const g_stage_names[] = {"decimate", "anti_dithering", "filter_mode"};

/* Runs one picture stage on both planes. Decimate works on a (2w, 2h) source
 * as the extractor does; the other two work in place on (w, h). */
static void run_picture_stage(const StageKernels *k, PictureStage stage, Plane *p, int width,
                              int height, uint16_t *mode_buf)
{
    switch (stage) {
    case STAGE_DECIMATE:
        k->decimate(&p->pic, (unsigned)width, (unsigned)height);
        break;
    case STAGE_ANTI_DITHER:
        k->anti_dithering(&p->pic, (unsigned)width, (unsigned)height);
        break;
    default:
        k->filter_mode(&p->pic, width, height, mode_buf);
        break;
    }
}

static char *check_picture_stage(const StageKernels *simd, PictureStage stage, int width,
                                 int height, enum Fill fill, uint32_t seed)
{
    /* Decimate halves a (2w - 1 or 2w, 2h - 1 or 2h) source, the two parities
     * (w + 1) / 2 comes from; it may read source column 2w - 1 but must not use
     * it when the source is 2w - 1 wide. The other stages work in place. */
    const int odd = (int)(seed & 1u);
    const int data_w = stage == STAGE_DECIMATE ? 2 * width - odd : width;
    const int data_h = stage == STAGE_DECIMATE ? 2 * height - odd : height;
    const int stride = 2 * width + 1 + (int)(seed % 13u);
    PlanePair pp;
    uint16_t *mode_buf = simd_test_aligned_malloc((size_t)3 * (size_t)width * sizeof(uint16_t), 64);
    const bool ok = pair_alloc(&pp, stride, data_h + 2, data_w, data_h, fill, seed) && mode_buf;
    char *err = NULL;
    if (ok) {
        run_picture_stage(&g_scalar, stage, &pp.s, width, height, mode_buf);
        run_picture_stage(simd, stage, &pp.v, width, height, mode_buf);
        (void)snprintf(g_label, sizeof(g_label), "%s %s w=%d h=%d fill=%d", simd->name,
                       g_stage_names[stage], width, height, (int)fill);
        err = compare_pair(&pp);
    }
    pair_free(&pp);
    simd_test_aligned_free(mode_buf);
    mu_assert("picture stage allocation failed", ok);
    return err;
}

static char *sweep_picture_stage(const StageKernels *simd, PictureStage stage)
{
    uint32_t state = 0x0DDBA11u + (uint32_t)stage;
    for (size_t w = 0; w < NUM_WIDTHS; w++) {
        for (size_t h = 0; h < NUM_HEIGHTS; h++) {
            for (int f = 0; f < NUM_FILLS; f++) {
                char *err = check_picture_stage(simd, stage, g_widths[w], g_heights[h],
                                                (enum Fill)f, simd_test_xorshift32(&state));
                if (err)
                    return err;
            }
        }
    }
    return NULL;
}

/* ---- derivative row -------------------------------------------------- */

static char *check_derivative(const StageKernels *simd, int width, int height, int row,
                              enum Fill fill, uint32_t seed)
{
    const int stride = width + 1 + (int)(seed % 7u);
    Plane img;
    const size_t n_out = (size_t)width + GUARD;
    uint16_t *out_s = simd_test_aligned_malloc(n_out * sizeof(uint16_t), 64);
    uint16_t *out_v = simd_test_aligned_malloc(n_out * sizeof(uint16_t), 64);
    const bool ok = plane_alloc(&img, stride, height) && out_s && out_v;
    char *err = NULL;
    if (ok) {
        fill_plane(&img, width, height, fill, seed);
        fill_guard16(out_s, n_out);
        fill_guard16(out_v, n_out);
        g_scalar.derivative(img.buf, out_s, width, height, row, stride);
        simd->derivative(img.buf, out_v, width, height, row, stride);
        (void)snprintf(g_label, sizeof(g_label), "%s derivative w=%d h=%d row=%d fill=%d",
                       simd->name, width, height, row, (int)fill);
        err = compare_bytes(out_s, out_v, n_out * sizeof(uint16_t));
    }
    plane_free(&img);
    simd_test_aligned_free(out_s);
    simd_test_aligned_free(out_v);
    mu_assert("derivative allocation failed", ok);
    return err;
}

static char *sweep_derivative(const StageKernels *simd)
{
    static const int heights[] = {1, 2, 3, 9};
    uint32_t state = 0xDE21BA7Eu;
    for (size_t w = 0; w < NUM_WIDTHS; w++) {
        for (size_t h = 0; h < sizeof(heights) / sizeof(heights[0]); h++) {
            const int rows[] = {0, heights[h] / 2, heights[h] - 1};
            for (size_t r = 0; r < (size_t)3 * (size_t)NUM_FILLS; r++) {
                char *err = check_derivative(simd, g_widths[w], heights[h], rows[r % 3],
                                             (enum Fill)(r / 3), simd_test_xorshift32(&state));
                if (err)
                    return err;
            }
        }
    }
    return NULL;
}

/* ---- histogram range updaters ---------------------------------------- */

#define RANGE_ARRAY 160

static char *check_range(const StageKernels *simd, int left, int right, uint32_t seed)
{
    uint16_t arr_s[RANGE_ARRAY + GUARD];
    uint16_t arr_v[RANGE_ARRAY + GUARD];
    uint32_t state = seed;
    for (size_t i = 0; i < RANGE_ARRAY; i++) {
        /* Include both wrap points of the modular uint16 update. */
        const uint32_t r = simd_test_xorshift32(&state);
        arr_s[i] = (r & 7u) == 0u ? 0u : ((r & 7u) == 1u ? 0xFFFFu : (uint16_t)(r >> 8));
    }
    fill_guard16(&arr_s[RANGE_ARRAY], GUARD);
    memcpy(arr_v, arr_s, sizeof(arr_s));

    g_scalar.inc(arr_s, left, right);
    simd->inc(arr_v, left, right);
    (void)snprintf(g_label, sizeof(g_label), "%s increment_range [%d, %d)", simd->name, left,
                   right);
    char *err = compare_bytes(arr_s, arr_v, sizeof(arr_s));
    if (err)
        return err;

    g_scalar.dec(arr_s, left, right);
    g_scalar.dec(arr_s, left, right);
    simd->dec(arr_v, left, right);
    simd->dec(arr_v, left, right);
    (void)snprintf(g_label, sizeof(g_label), "%s decrement_range [%d, %d)", simd->name, left,
                   right);
    return compare_bytes(arr_s, arr_v, sizeof(arr_s));
}

static char *sweep_ranges(const StageKernels *simd)
{
    static const int lefts[] = {0, 1, 3, 7, 8, 15, 31, 33, 60};
    uint32_t state = 0x4A7E5EEDu;
    for (size_t l = 0; l < sizeof(lefts) / sizeof(lefts[0]); l++) {
        /* Lengths 0 .. 99 cover every window CAMBI uses (3 .. 65) and every
         * residue of the 32-lane step. */
        for (int len = 0; len < 100 && lefts[l] + len <= RANGE_ARRAY; len++) {
            char *err = check_range(simd, lefts[l], lefts[l] + len, simd_test_xorshift32(&state));
            if (err)
                return err;
        }
    }
    return NULL;
}

/* ---- c-values fixtures ----------------------------------------------- */

/* One CAMBI contrast / visibility configuration, built with the production
 * helpers exactly as init() builds it. */
typedef struct {
    uint16_t num_diffs;
    uint16_t *diffs_to_consider;
    int *diff_weights;
    int *all_diffs;
    uint16_t tvi_for_diff[32];
    uint16_t vlt_luma;
    uint16_t v_band_base;
    uint16_t v_band_size;
} CValuesConfig;

static int config_init(CValuesConfig *c, int max_log_contrast, const char *eotf_name,
                       double vis_lum_threshold)
{
    memset(c, 0, sizeof(*c));
    /* The option's range, 0 .. 5, gives 1 .. 32 contrast steps (tvi_for_diff). */
    if (max_log_contrast < 0 || max_log_contrast > 5)
        return -EINVAL;
    const uint16_t num_diffs = (uint16_t)(1u << (unsigned)max_log_contrast);
    if (num_diffs == 0u || num_diffs > sizeof(c->tvi_for_diff) / sizeof(c->tvi_for_diff[0]))
        return -EINVAL;
    c->num_diffs = num_diffs;
    int err =
        set_contrast_arrays(num_diffs, &c->diffs_to_consider, &c->diff_weights, &c->all_diffs);
    if (err)
        return err;
    VmafLumaRange luma_range;
    VmafEOTF eotf;
    err = vmaf_luminance_init_luma_range(&luma_range, 10, VMAF_PIXEL_RANGE_LIMITED);
    err |= vmaf_luminance_init_eotf(&eotf, eotf_name);
    if (err)
        return err;
    for (int d = 0; d < num_diffs; d++) {
        c->tvi_for_diff[d] = (uint16_t)(get_tvi_for_diff(c->diffs_to_consider[d], DEFAULT_CAMBI_TVI,
                                                         10, luma_range, eotf) +
                                        num_diffs);
    }
    c->vlt_luma = (uint16_t)get_vlt_luma(vis_lum_threshold, luma_range, eotf);
    const int v_lo = (int)c->vlt_luma - 3 * (int)num_diffs + 1;
    c->v_band_base = v_lo > 0 ? (uint16_t)v_lo : 0;
    c->v_band_size = (uint16_t)(c->tvi_for_diff[num_diffs - 1] + 1 - c->v_band_base);
    return 0;
}

static void config_free(CValuesConfig *c)
{
    aligned_free(c->diffs_to_consider);
    aligned_free(c->diff_weights);
    aligned_free(c->all_diffs);
}

/* Values concentrated on the scored band (plus some outside it), so most
 * pixels reach the per-diff loop. */
static uint16_t band_value(const CValuesConfig *c, uint32_t r)
{
    if ((r & 15u) == 0u)
        return (uint16_t)((r >> 4) & 0x3FFu);
    const uint32_t lo = c->v_band_base;
    return (uint16_t)(lo + (r >> 4) % (uint32_t)(c->v_band_size + 8u));
}

/* ---- c-values row ---------------------------------------------------- */

typedef struct {
    uint16_t *image;
    uint16_t *mask;
    uint16_t *hist;
    float *out_s;
    float *out_v;
} RowBuffers;

static bool row_buffers_alloc(RowBuffers *b, int width, uint16_t band)
{
    const size_t w = (size_t)width;
    b->image = simd_test_aligned_malloc(w * sizeof(uint16_t), 64);
    b->mask = simd_test_aligned_malloc(w * sizeof(uint16_t), 64);
    b->hist = simd_test_aligned_malloc(w * band * sizeof(uint16_t), 64);
    b->out_s = simd_test_aligned_malloc((w + GUARD) * sizeof(float), 64);
    b->out_v = simd_test_aligned_malloc((w + GUARD) * sizeof(float), 64);
    return b->image && b->mask && b->hist && b->out_s && b->out_v;
}

static void row_buffers_free(RowBuffers *b)
{
    simd_test_aligned_free(b->image);
    simd_test_aligned_free(b->mask);
    simd_test_aligned_free(b->hist);
    simd_test_aligned_free(b->out_s);
    simd_test_aligned_free(b->out_v);
}

/* Histogram counts stay below 2113 so p_0 + p_max indexes the 4226-entry
 * reciprocal LUT, as a real window (at most 65 x 65 pixels) guarantees. */
static void row_fixture(RowBuffers *b, const CValuesConfig *c, int width, uint32_t mask_bits,
                        uint32_t *state)
{
    const size_t w = (size_t)width;
    for (size_t j = 0; j < w; j++) {
        const uint32_t r = simd_test_xorshift32(state);
        b->image[j] = band_value(c, r);
        b->mask[j] = (uint16_t)((r >> 27) < mask_bits ? 1u : 0u);
    }
    for (size_t i = 0; i < w * c->v_band_size; i++) {
        const uint32_t r = simd_test_xorshift32(state);
        b->hist[i] = (uint16_t)((r & 3u) == 0u ? 0u : (r >> 8) % 2113u);
    }
    for (size_t j = 0; j < w + GUARD; j++) {
        /* The frame driver zeroes c_values before the rows run. */
        b->out_s[j] = j < w ? 0.0f : -1.0f;
        b->out_v[j] = b->out_s[j];
    }
}

static char *check_c_values_row(const StageKernels *simd, const CValuesConfig *c, int width,
                                uint32_t mask_bits, uint32_t seed)
{
    RowBuffers b;
    const bool ok = row_buffers_alloc(&b, width, c->v_band_size);
    char *err = NULL;
    if (ok) {
        uint32_t state = seed;
        row_fixture(&b, c, width, mask_bits, &state);
        g_scalar.c_values_row(b.out_s, b.hist, b.image, b.mask, 0, width, width, c->num_diffs,
                              c->tvi_for_diff, c->vlt_luma, c->diff_weights, c->all_diffs,
                              reciprocal_lut);
        simd->c_values_row(b.out_v, b.hist, b.image, b.mask, 0, width, width, c->num_diffs,
                           c->tvi_for_diff, c->vlt_luma, c->diff_weights, c->all_diffs,
                           reciprocal_lut);
        (void)snprintf(g_label, sizeof(g_label), "%s c_values_row w=%d diffs=%u vlt=%u mask=%u/32",
                       simd->name, width, (unsigned)c->num_diffs, (unsigned)c->vlt_luma,
                       (unsigned)mask_bits);
        err = compare_bytes(b.out_s, b.out_v, ((size_t)width + GUARD) * sizeof(float));
    }
    row_buffers_free(&b);
    mu_assert("c-values row allocation failed", ok);
    return err;
}

/* max_log_contrast 0..5 (the option's range), BT.1886 with and without a
 * visibility threshold, and PQ. */
typedef struct {
    int max_log_contrast;
    const char *eotf;
    double vis_lum_threshold;
} ConfigSpec;

static const ConfigSpec g_configs[] = {
    {2, "bt1886", 0.0}, {0, "bt1886", 0.0}, {1, "bt1886", 0.0}, {3, "bt1886", 0.06},
    {4, "pq", 0.0},     {5, "bt1886", 0.0}, {2, "pq", 0.1},
};
#define NUM_CONFIGS (sizeof(g_configs) / sizeof(g_configs[0]))

static char *sweep_c_values_rows_for(const StageKernels *simd, const CValuesConfig *c,
                                     uint32_t *state)
{
    static const uint32_t mask_bits[] = {0u, 10u, 32u};
    for (size_t w = 0; w < NUM_WIDTHS; w++) {
        for (size_t m = 0; m < 3; m++) {
            char *err =
                check_c_values_row(simd, c, g_widths[w], mask_bits[m], simd_test_xorshift32(state));
            if (err)
                return err;
        }
    }
    return NULL;
}

static char *sweep_c_values_rows(const StageKernels *simd)
{
    uint32_t state = 0xC0FFEE42u;
    for (size_t i = 0; i < NUM_CONFIGS; i++) {
        CValuesConfig c;
        const int init_err = config_init(&c, g_configs[i].max_log_contrast, g_configs[i].eotf,
                                         g_configs[i].vis_lum_threshold);
        char *err =
            init_err ? "c-values config init failed" : sweep_c_values_rows_for(simd, &c, &state);
        config_free(&c);
        if (err)
            return err;
    }
    return NULL;
}

/* ---- frame-level c-values -------------------------------------------- */

typedef struct {
    int window;
    int width;
    int height;
} FrameShape;

typedef struct {
    PlanePair image;
    PlanePair mask;
    uint16_t *hist_s;
    uint16_t *hist_v;
    float *c_s;
    float *c_v;
    size_t n_hist;
    size_t n_c;
} FrameBuffers;

static bool frame_buffers_alloc(FrameBuffers *b, const FrameShape *s, const CValuesConfig *c,
                                int stride, uint32_t seed)
{
    b->n_hist = (size_t)s->width * c->v_band_size + GUARD;
    b->n_c = (size_t)s->width * (size_t)s->height + GUARD;
    b->hist_s = simd_test_aligned_malloc(b->n_hist * sizeof(uint16_t), 64);
    b->hist_v = simd_test_aligned_malloc(b->n_hist * sizeof(uint16_t), 64);
    b->c_s = simd_test_aligned_malloc(b->n_c * sizeof(float), 64);
    b->c_v = simd_test_aligned_malloc(b->n_c * sizeof(float), 64);
    const bool ok_image =
        pair_alloc(&b->image, stride, s->height, s->width, s->height, FILL_10BIT, seed);
    const bool ok_mask =
        pair_alloc(&b->mask, stride, s->height, s->width, s->height, FILL_10BIT, seed + 1u);
    return ok_image && ok_mask && b->hist_s && b->hist_v && b->c_s && b->c_v;
}

static void frame_buffers_free(FrameBuffers *b)
{
    pair_free(&b->image);
    pair_free(&b->mask);
    simd_test_aligned_free(b->hist_s);
    simd_test_aligned_free(b->hist_v);
    simd_test_aligned_free(b->c_s);
    simd_test_aligned_free(b->c_v);
}

/* Image: a banded ramp with some off-band pixels. Mask: 4x4 blocks, mostly
 * set, as a flat-region spatial mask looks. Scalar and SIMD sides start from
 * identical buffers, including garbage in the histogram and c-values. */
static void frame_fixture(FrameBuffers *b, const FrameShape *s, const CValuesConfig *c, int stride,
                          uint32_t *state)
{
    for (int i = 0; i < s->height; i++) {
        for (int j = 0; j < s->width; j++) {
            const uint32_t r = simd_test_xorshift32(state);
            const size_t at = (size_t)i * (size_t)stride + (size_t)j;
            const uint32_t ramp =
                c->v_band_base + (uint32_t)(j / 5 + i / 3) % (c->v_band_size + 4u);
            b->image.s.buf[at] = (r & 31u) == 0u ? band_value(c, r) : (uint16_t)ramp;
            const uint32_t block = (uint32_t)(i / 4) * 131u + (uint32_t)(j / 4) * 71u;
            b->mask.s.buf[at] = (uint16_t)(((block ^ (block >> 3)) & 7u) != 0u);
        }
    }
    memcpy(b->image.v.buf, b->image.s.buf, b->image.s.n * sizeof(uint16_t));
    memcpy(b->mask.v.buf, b->mask.s.buf, b->mask.s.n * sizeof(uint16_t));
    fill_guard16(b->hist_s, b->n_hist);
    fill_guard16(b->hist_v, b->n_hist);
    memset(b->c_s, 0xA5, b->n_c * sizeof(float));
    memset(b->c_v, 0xA5, b->n_c * sizeof(float));
}

static char *check_c_values_frame(const StageKernels *simd, const CValuesConfig *c,
                                  const FrameShape *s, uint32_t seed)
{
    const int stride = s->width + 3 + (int)(seed % 11u);
    FrameBuffers b;
    const bool ok = frame_buffers_alloc(&b, s, c, stride, seed);
    char *err = NULL;
    if (ok) {
        uint32_t state = seed;
        frame_fixture(&b, s, c, stride, &state);
        g_scalar.c_values(&b.image.s.pic, &b.mask.s.pic, b.c_s, b.hist_s, (uint16_t)s->window,
                          c->num_diffs, c->tvi_for_diff, c->vlt_luma, c->diff_weights, c->all_diffs,
                          s->width, s->height);
        simd->c_values(&b.image.v.pic, &b.mask.v.pic, b.c_v, b.hist_v, (uint16_t)s->window,
                       c->num_diffs, c->tvi_for_diff, c->vlt_luma, c->diff_weights, c->all_diffs,
                       s->width, s->height);
        (void)snprintf(g_label, sizeof(g_label), "%s c_values window=%d w=%d h=%d diffs=%u",
                       simd->name, s->window, s->width, s->height, (unsigned)c->num_diffs);
        err = compare_bytes(b.c_s, b.c_v, b.n_c * sizeof(float));
        if (!err)
            err = compare_bytes(b.hist_s, b.hist_v, b.n_hist * sizeof(uint16_t));
    }
    frame_buffers_free(&b);
    mu_assert("c-values frame allocation failed", ok);
    return err;
}

/* Windows 3 .. 65 (576x324 uses 9, 1080p 33, 2160p 65). Heights from the
 * smallest the walk supports (pad + 1) through the top / bottom edge overlap
 * to a full middle slide; widths around every vector boundary. */
static char *sweep_frames_for(const StageKernels *simd, const CValuesConfig *c, bool full,
                              uint32_t *state)
{
    static const int windows[] = {3, 5, 9, 17, 21, 33, 65};
    static const int widths[] = {1, 7, 16, 17, 31, 33, 40, 64, 65, 97, 130, 257};
    for (size_t wi = 0; wi < sizeof(windows) / sizeof(windows[0]); wi++) {
        const int pad = windows[wi] >> 1;
        const int heights[] = {pad + 1, 2 * pad + 1, 2 * pad + 9};
        for (size_t x = 0; x < sizeof(widths) / sizeof(widths[0]); x++) {
            for (size_t y = 0; y < (full ? 3u : 1u); y++) {
                const FrameShape s = {windows[wi], MAX(widths[x], pad + 1), heights[y]};
                char *err = check_c_values_frame(simd, c, &s, simd_test_xorshift32(state));
                if (err)
                    return err;
            }
        }
    }
    return NULL;
}

static char *sweep_c_values_frames(const StageKernels *simd)
{
    uint32_t state = 0xF4A3E5EDu;
    for (size_t i = 0; i < NUM_CONFIGS; i++) {
        CValuesConfig c;
        const int init_err = config_init(&c, g_configs[i].max_log_contrast, g_configs[i].eotf,
                                         g_configs[i].vis_lum_threshold);
        /* The production configuration gets every shape; the others one height each. */
        char *err =
            init_err ? "c-values config init failed" : sweep_frames_for(simd, &c, i == 0, &state);
        config_free(&c);
        if (err)
            return err;
    }
    return NULL;
}

/* ---- per-ISA entry points -------------------------------------------- */

static const StageKernels *g_current;

static char *test_decimate_parity(void)
{
    return sweep_picture_stage(g_current, STAGE_DECIMATE);
}

static char *test_anti_dithering_parity(void)
{
    return sweep_picture_stage(g_current, STAGE_ANTI_DITHER);
}

static char *test_filter_mode_parity(void)
{
    return sweep_picture_stage(g_current, STAGE_FILTER_MODE);
}

static char *test_derivative_parity(void)
{
    return sweep_derivative(g_current);
}

static char *test_range_parity(void)
{
    return sweep_ranges(g_current);
}

static char *test_c_values_row_parity(void)
{
    return sweep_c_values_rows(g_current);
}

static char *test_c_values_frame_parity(void)
{
    return sweep_c_values_frames(g_current);
}

static char *run_isa(const StageKernels *k)
{
    (void)fprintf(stderr, "[%s]\n", k->name);
    g_current = k;
    mu_run_test(test_decimate_parity);
    mu_run_test(test_anti_dithering_parity);
    mu_run_test(test_filter_mode_parity);
    mu_run_test(test_derivative_parity);
    if (k->inc)
        mu_run_test(test_range_parity);
    mu_run_test(test_c_values_row_parity);
    mu_run_test(test_c_values_frame_parity);
    return NULL;
}

#if ARCH_X86
static const StageKernels g_avx2 = {
    "avx2",
    decimate_avx2,
    anti_dithering_filter_avx2,
    filter_mode_avx2,
    get_derivative_data_for_row_avx2,
    cambi_increment_range_avx2,
    cambi_decrement_range_avx2,
    calculate_c_values_row_avx2,
    calculate_c_values_avx2,
};
#if HAVE_AVX512
static const StageKernels g_avx512 = {
    "avx512",
    decimate_avx512,
    anti_dithering_filter_avx512,
    filter_mode_avx512,
    get_derivative_data_for_row_avx512,
    cambi_increment_range_avx512,
    cambi_decrement_range_avx512,
    calculate_c_values_row_avx512,
    calculate_c_values_avx512,
};
#endif
#endif

#if ARCH_AARCH64
static const StageKernels g_neon = {
    "neon",
    decimate_neon,
    anti_dithering_filter_neon,
    filter_mode_neon,
    get_derivative_data_for_row_neon,
    /* No NEON range updaters: calculate_c_values_neon uses plain C loops that
     * the compilers vectorize (Research-2065). */
    NULL,
    NULL,
    calculate_c_values_row_neon,
    calculate_c_values_neon,
};
#endif

char *run_tests(void)
{
    /* Scalar reference: clear every CPU flag so anti_dithering_filter, the
     * one stage that dispatches internally, runs its scalar body. */
    vmaf_set_cpu_flags_mask(0u);
    char *err = NULL;
#if ARCH_X86
    if (simd_test_have_avx2())
        err = run_isa(&g_avx2);
#if HAVE_AVX512
    if (!err && simd_test_have_avx512())
        err = run_isa(&g_avx512);
#else
    (void)fprintf(stderr, "skipping AVX-512: built without AVX-512\n");
#endif
#elif ARCH_AARCH64
    err = run_isa(&g_neon);
#else
    (void)fprintf(stderr, "skipping: arch lacks CAMBI stage SIMD\n");
#endif
    return err;
}

/* NOLINTEND(modernize-use-nullptr) */
