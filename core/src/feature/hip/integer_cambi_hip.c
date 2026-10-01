/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  CAMBI banding-detection feature extractor on the HIP backend, fully
 *  device-resident since ADR-1378 (the HIP port of the SYCL design of
 *  ADR-1357).
 *
 *  submit() copies the distorted luma plane into pinned staging and enqueues
 *  on the extractor's stream, without waiting: one upload
 *  (vmaf_hip_picture_upload_staged()), a reset of the per-frame device state,
 *  input validation, preprocessing (10-bit conversion, resize, anti-dither),
 *  the spatial mask, and for each of the five scales decimation, the mode
 *  filter and level map, the run / change masks, the c-values and the
 *  radix-select top-K pooling; then one copy of the 88-byte CambiHipResults
 *  block back to pinned memory. collect() waits once and turns the five exact
 *  per-scale top-K sums into the score with cambi.c's own
 *  vmaf_cambi_fixed_topk_mean() and vmaf_cambi_weight_scores_per_scale(). No
 *  stage of cambi.c runs on the host and nothing waits on the device before
 *  collect().
 *
 *  The kernels are in integer_cambi/cambi_score.hip; their per-work-item
 *  arithmetic, and the parameter block laid out here once at init
 *  (cambi_hip_plan()), are in integer_cambi/cambi_hip_device.h. The window,
 *  mask index, resize tables, contrast weights and the reciprocal-table guard
 *  come from cambi.c's shared helpers (cambi_internal.h). Numerical contract:
 *  every stage is integer or float-for-float with cambi.c and the top-K sum
 *  is exact, so the score is bit-identical to the CPU extractor whenever
 *  cambi.c's own double sum is exact (ADR-1357).
 */

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature_name.h"
#include "log.h"
#include "mem.h"
#include "picture.h"

/* HIP common.h must come before <hip/hip_runtime_api.h>: common.h provides
 * the typedef stubs (hipError_t = int) used in the non-HAVE_HIPCC path.
 * Including the real HIP header first causes a type-redefinition error. */
#include "../../hip/common.h"
#include "../../hip/kernel_template.h"
#include "integer_cambi_hip.h"

#ifdef HAVE_HIPCC
#include <hip/hip_runtime_api.h>

#include "../../hip/hip_handle.h"
#include "../../hip/picture_hip.h"
#endif /* HAVE_HIPCC */

#include "feature/cambi_internal.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `NULL` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

/* --- Constants matching cambi.c --- */
/* CAMBI_MIN_WIDTH_HEIGHT and CAMBI_WINDOW_DIVISOR come from cambi_internal.h */
#define CAMBI_HIP_DEFAULT_MAX_VAL 1000.0
#define CAMBI_HIP_DEFAULT_WINDOW_SIZE 65
#define CAMBI_HIP_DEFAULT_TOPK 0.6
#define CAMBI_HIP_DEFAULT_TVI 0.019
#define CAMBI_HIP_DEFAULT_VLT 0.0
#define CAMBI_HIP_DEFAULT_MAX_LOG_CONTRAST 2
#define CAMBI_HIP_DEFAULT_EOTF "bt1886"
#define CAMBI_HIP_MAX_DIFFS 32u
/* c-values: the shortest row chunk worth re-priming a window for, and the
 * bound on the per-chunk column histograms (ADR-1357). */
#define CAMBI_HIP_MIN_CHUNK_ROWS 32u
#define CAMBI_HIP_HIST_BUDGET ((size_t)64u << 20u)
#define CAMBI_HIP_POOL_MAX_GROUPS 512u
#define CAMBI_HIP_POOL_ELEMS_PER_GROUP 4096u
#define CAMBI_HIP_ARENA_ALIGN ((size_t)256u)

/* The device fixed point is cambi.c's (vmaf_cambi_fixed_topk_mean()). */
_Static_assert(CAMBI_HIP_FIXED_SHIFT == VMAF_CAMBI_TOPK_FIXED_SHIFT,
               "device fixed point matches vmaf_cambi_fixed_topk_mean()");
_Static_assert(CAMBI_HIP_NUM_SCALES == VMAF_CAMBI_NUM_SCALES, "cambi.c's scale count");

/* Kernel entry points of cambi_score.hip, in launch order. */
enum CambiHipKernel {
    CAMBI_K_VALIDATE,
    CAMBI_K_PREPROCESS,
    CAMBI_K_MASK,
    CAMBI_K_DECIMATE,
    CAMBI_K_FILTER_H,
    CAMBI_K_FILTER_V,
    CAMBI_K_ROW_MASKS,
    CAMBI_K_CVALS,
    CAMBI_K_RADIX_HIST,
    CAMBI_K_RADIX_SCAN,
    CAMBI_K_TOPK_SUM,
    CAMBI_K_TOPK_FINAL,
    CAMBI_K_COUNT
};

#ifdef HAVE_HIPCC
static const char *const cambi_hip_kernel_names[CAMBI_K_COUNT] = {
    "cambi_hip_validate",   "cambi_hip_preprocess", "cambi_hip_spatial_mask",
    "cambi_hip_decimate",   "cambi_hip_filter_h",   "cambi_hip_filter_v",
    "cambi_hip_row_masks",  "cambi_hip_cvals",      "cambi_hip_radix_hist",
    "cambi_hip_radix_scan", "cambi_hip_topk_sum",   "cambi_hip_topk_final",
};
#endif /* HAVE_HIPCC */

/* ------------------------------------------------------------------ */
/* Private state                                                       */
/* ------------------------------------------------------------------ */
typedef struct CambiStateHip {
    /* HIP lifecycle (stream + events). */
    VmafHipKernelLifecycle lc;
    VmafHipContext *ctx;

#ifdef HAVE_HIPCC
    hipModule_t module;
    hipFunction_t kernels[CAMBI_K_COUNT];
#endif /* HAVE_HIPCC */

    /* One device allocation holds every buffer; params (host image in
     * `params`, device copy at d_params) points into it. */
    void *d_arena;
    CambiHipParams *d_params;
    CambiHipFrameState *d_frame;
    void *d_src;
    /* Pinned host memory: the staged luma plane and the results. */
    void *h_staging;
    CambiHipResults *h_results;
    size_t src_bytes;
    CambiHipParams params;

    /* Configuration options (mirrors cambi.c). */
    int enc_width;
    int enc_height;
    int enc_bitdepth;
    int max_log_contrast;
    int window_size;
    double topk;
    double cambi_topk;
    double tvi_threshold;
    double cambi_max_val;
    double cambi_vis_lum_threshold;
    char *eotf;
    char *cambi_eotf;
    int cambi_high_res_speedup;

    /* Resolved configuration. */
    CambiHipPlanInput plan;
    uint16_t tvi[CAMBI_HIP_MAX_DIFFS];

    VmafDictionary *feature_name_dict;
} CambiStateHip;

/* --- Options --- */
static const VmafOption options[] = {
    {
        .name = "cambi_max_val",
        .help = "maximum value allowed; larger values will be clipped",
        .offset = offsetof(CambiStateHip, cambi_max_val),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = CAMBI_HIP_DEFAULT_MAX_VAL,
        .min = 0.0,
        .max = 1000.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "cmxv",
    },
    {
        .name = "enc_width",
        .help = "Encoding width",
        .offset = offsetof(CambiStateHip, enc_width),
        .type = VMAF_OPT_TYPE_INT,
        .default_val.i = 0,
        .min = 180,
        .max = 7680,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "encw",
    },
    {
        .name = "enc_height",
        .help = "Encoding height",
        .offset = offsetof(CambiStateHip, enc_height),
        .type = VMAF_OPT_TYPE_INT,
        .default_val.i = 0,
        .min = 150,
        .max = 7680,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "ench",
    },
    {
        .name = "enc_bitdepth",
        .help = "Encoding bitdepth",
        .offset = offsetof(CambiStateHip, enc_bitdepth),
        .type = VMAF_OPT_TYPE_INT,
        .default_val.i = 0,
        .min = 6,
        .max = 16,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "encbd",
    },
    {
        .name = "window_size",
        .help = "Window size to compute CAMBI: 65 corresponds to ~1 degree at 4k",
        .offset = offsetof(CambiStateHip, window_size),
        .type = VMAF_OPT_TYPE_INT,
        .default_val.i = CAMBI_HIP_DEFAULT_WINDOW_SIZE,
        .min = 15,
        .max = 127,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "ws",
    },
    {
        .name = "topk",
        .help = "Ratio of pixels for the spatial pooling computation",
        .offset = offsetof(CambiStateHip, topk),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = CAMBI_HIP_DEFAULT_TOPK,
        .min = 0.0001,
        .max = 1.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "cambi_topk",
        .help = "Ratio of pixels for the spatial pooling computation",
        .offset = offsetof(CambiStateHip, cambi_topk),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = CAMBI_HIP_DEFAULT_TOPK,
        .min = 0.0001,
        .max = 1.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "ctpk",
    },
    {
        .name = "tvi_threshold",
        .help = "Visibility threshold delta-L < tvi_threshold * L_mean",
        .offset = offsetof(CambiStateHip, tvi_threshold),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = CAMBI_HIP_DEFAULT_TVI,
        .min = 0.0001,
        .max = 1.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "tvit",
    },
    {
        .name = "cambi_vis_lum_threshold",
        .help = "Luminance value below which banding is assumed invisible",
        .offset = offsetof(CambiStateHip, cambi_vis_lum_threshold),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = CAMBI_HIP_DEFAULT_VLT,
        .min = 0.0,
        .max = 300.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "vlt",
    },
    {
        .name = "max_log_contrast",
        .help = "Maximum log contrast (0 to 5, default 2)",
        .offset = offsetof(CambiStateHip, max_log_contrast),
        .type = VMAF_OPT_TYPE_INT,
        .default_val.i = CAMBI_HIP_DEFAULT_MAX_LOG_CONTRAST,
        .min = 0,
        .max = 5,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "mlc",
    },
    {
        .name = "eotf",
        .help = "EOTF for visibility-threshold conversion (bt1886 / pq)",
        .offset = offsetof(CambiStateHip, eotf),
        .type = VMAF_OPT_TYPE_STRING,
        .default_val.s = CAMBI_HIP_DEFAULT_EOTF,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "cambi_eotf",
        .help = "EOTF override for cambi (defaults to eotf)",
        .offset = offsetof(CambiStateHip, cambi_eotf),
        .type = VMAF_OPT_TYPE_STRING,
        .default_val.s = CAMBI_HIP_DEFAULT_EOTF,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "ceot",
    },
    {
        .name = "cambi_high_res_speedup",
        .help =
            "Speed up the processing by downsampling post spatial mask for resolutions >= 1080p",
        .offset = offsetof(CambiStateHip, cambi_high_res_speedup),
        .type = VMAF_OPT_TYPE_INT,
        .default_val.i = 0,
        .min = 0,
        .max = CAMBI_4K_HEIGHT,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "hrs",
    },
    {0},
};

#ifdef HAVE_HIPCC
/* ------------------------------------------------------------------ */
/* Init-time configuration (cambi.c init), host only, before any       */
/* device work. The scaffold build reports -ENOSYS first (ADR-1264).   */
/* ------------------------------------------------------------------ */

/* cambi.c::validate_and_setup_dimensions: the speed-up only takes effect when
 * the encode is at least that resolution. */
static int cambi_hip_speedup_valid(int requested, int pixels)
{
    if (requested == 1080)
        return pixels >= CAMBI_HIGH_RES_SPEEDUP_THRESHOLD_1080p;
    if (requested == 1440)
        return pixels >= CAMBI_HIGH_RES_SPEEDUP_THRESHOLD_1440p;
    if (requested == 2160)
        return pixels >= CAMBI_HIGH_RES_SPEEDUP_THRESHOLD_2160p;
    return 0;
}

/* Resolves the encoded geometry exactly as cambi.c::init does. */
static int cambi_hip_resolve_geometry(CambiStateHip *s, unsigned bpc, unsigned w, unsigned h)
{
    if (s->enc_bitdepth == 0)
        s->enc_bitdepth = (int)bpc;
    if (s->enc_width == 0 || s->enc_height == 0 || (unsigned)s->enc_height > h ||
        (unsigned)s->enc_width > w) {
        s->enc_width = (int)w;
        s->enc_height = (int)h;
    }
    if (!cambi_validate_dimensions((unsigned)s->enc_width, (unsigned)s->enc_height)) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "cambi_hip: encoded resolution %dx%d below minimum %d.\n",
                 s->enc_width, s->enc_height, CAMBI_MIN_WIDTH_HEIGHT);
        return -EINVAL;
    }
    if (!cambi_hip_speedup_valid(s->cambi_high_res_speedup, s->enc_width * s->enc_height))
        s->cambi_high_res_speedup = 0;
    CambiHipPlanInput *in = &s->plan;
    in->src_width = w;
    in->src_height = h;
    in->src_bpc = bpc;
    in->proc_width = (unsigned)s->enc_width;
    in->proc_height = (unsigned)s->enc_height;
    in->enc_bpc = (unsigned)s->enc_bitdepth;
    in->speedup = s->cambi_high_res_speedup ? 1u : 0u;
    in->adjusted_window = vmaf_cambi_adjust_window(s->window_size, in->proc_width, in->proc_height,
                                                   in->speedup != 0u);
    in->num_diffs = 1u << (unsigned)s->max_log_contrast;
    /* The original topk setting when it is not the default, else cambi_topk
     * (cambi.c::extract). */
    in->topk = s->topk != CAMBI_HIP_DEFAULT_TOPK ? s->topk : s->cambi_topk;
    in->compute_units = 1u;
    return 0;
}

/* ADR-1219: the TVI table, vlt_luma and the level band come from the shared
 * CPU helper, never a re-derivation. */
static int cambi_hip_contrast_tables(CambiStateHip *s)
{
    uint16_t diffs[CAMBI_HIP_MAX_DIFFS];
    for (unsigned d = 0u; d < s->plan.num_diffs; d++)
        diffs[d] = (uint16_t)(d + 1u);
    uint16_t vlt_luma = 0u;
    uint16_t v_band_base = 0u;
    uint16_t v_band_size = 0u;
    const int err = vmaf_cambi_init_tvi_and_vlt((int)s->plan.num_diffs, diffs, s->tvi_threshold,
                                                s->cambi_vis_lum_threshold, s->cambi_eotf, s->eotf,
                                                s->tvi, &vlt_luma, &v_band_base, &v_band_size);
    s->plan.vlt_luma = vlt_luma;
    s->plan.v_band_base = v_band_base;
    s->plan.levels = v_band_size;
    return err;
}

/* cambi.c::setup_contrast_and_luminance()'s guard, at the same point of init
 * and through the same routine: the encode-resolution window and the
 * source-resolution window (the full input, as cambi.c's default src_width /
 * src_height), after the high-res speed-up, must satisfy window^2 < the
 * reciprocal table size, so the largest accepted window is 65 x 65. */
static int cambi_hip_check_window(const CambiStateHip *s)
{
    const CambiHipPlanInput *in = &s->plan;
    const uint16_t src_window =
        vmaf_cambi_adjust_window(s->window_size, in->src_width, in->src_height, in->speedup != 0u);
    return vmaf_cambi_check_window_fits_lut((uint16_t)in->adjusted_window, src_window);
}

/* Host-only part of init: geometry, contrast tables and the window guard,
 * all before the device is touched, so a rejected window costs no device. */
static int cambi_hip_configure(CambiStateHip *s, unsigned bpc, unsigned w, unsigned h)
{
    int err = cambi_hip_resolve_geometry(s, bpc, w, h);
    if (!err)
        err = cambi_hip_contrast_tables(s);
    if (!err)
        err = cambi_hip_check_window(s);
    return err;
}
#endif /* HAVE_HIPCC */

/* ------------------------------------------------------------------ */
/* The parameter block (integer_cambi_hip.h), host only, every build.  */
/* ------------------------------------------------------------------ */

/* Row chunks for the c-values pass (ADR-1357): one work-item walks one column
 * of one chunk, so enough chunks keep the device busy, at least
 * CAMBI_HIP_MIN_CHUNK_ROWS rows each keep the per-chunk window priming a minor
 * cost, and the column histograms stay within CAMBI_HIP_HIST_BUDGET. */
static unsigned cambi_hip_cvals_chunks(unsigned width, unsigned height, unsigned levels,
                                       unsigned compute_units)
{
    const unsigned target_items = compute_units * 512u;
    unsigned chunks = (target_items + width - 1u) / width;
    const size_t chunk_bytes = (size_t)width * levels * sizeof(uint16_t);
    size_t by_memory = CAMBI_HIP_HIST_BUDGET / chunk_bytes;
    if (by_memory < 1u)
        by_memory = 1u;
    unsigned max_chunks = height / CAMBI_HIP_MIN_CHUNK_ROWS;
    if (max_chunks < 1u)
        max_chunks = 1u;
    if ((size_t)max_chunks > by_memory)
        max_chunks = (unsigned)by_memory;
    if (chunks < 1u)
        chunks = 1u;
    return chunks > max_chunks ? max_chunks : chunks;
}

/* Every scalar the kernels read. */
static void cambi_hip_plan_scalars(CambiHipParams *p, const CambiHipPlanInput *in)
{
    p->src_width = in->src_width;
    p->src_height = in->src_height;
    p->src_bpc = in->src_bpc;
    p->src_max = (1u << in->src_bpc) - 1u;
    /* cambi.c::validate_image checks every depth but 8 and 16. */
    p->validate = (in->src_bpc != 8u && in->src_bpc != 16u) ? 1u : 0u;
    p->same_size = (in->proc_width == in->src_width && in->proc_height == in->src_height) ? 1u : 0u;
    p->anti_dither = in->enc_bpc < 10u ? 1u : 0u;
    p->proc_width = in->proc_width;
    p->proc_height = in->proc_height;
    p->mask_index = vmaf_cambi_mask_index(in->proc_width, in->proc_height);
    p->pad = in->adjusted_window >> 1;
    p->levels = in->levels;
    p->num_diffs = in->num_diffs;
    p->vlt_luma = in->vlt_luma;
    p->v_band_base = in->v_band_base;
    p->reserved = 0u;
}

/* Per-scale dimensions (cambi_score's scaled_width / scaled_height walk),
 * c-values chunking and top-K counts (spatial_pooling's clip()). */
void cambi_hip_plan(CambiHipParams *p, const CambiHipPlanInput *in)
{
    assert(p != NULL);
    assert(in != NULL);
    assert(in->proc_width > 0u);
    assert(in->proc_height > 0u);
    cambi_hip_plan_scalars(p, in);
    unsigned width = in->proc_width;
    unsigned height = in->proc_height;
    for (unsigned scale = 0u; scale < CAMBI_HIP_NUM_SCALES; ++scale) {
        CambiHipScale *g = &p->scale[scale];
        g->decimate_src_width = width;
        g->decimate = (scale > 0u || in->speedup) ? 1u : 0u;
        if (g->decimate) {
            width = (width + 1u) >> 1;
            height = (height + 1u) >> 1;
        }
        g->width = width;
        g->height = height;
        g->mask_shift = scale + in->speedup;
        g->words = (width + 31u) / 32u;
        g->chunks = cambi_hip_cvals_chunks(width, height, in->levels, in->compute_units);
        g->chunk_rows = (height + g->chunks - 1u) / g->chunks;
        g->chunks = (height + g->chunk_rows - 1u) / g->chunk_rows;
        const unsigned n = width * height;
        const int raw = (int)(in->topk * (double)(int)n);
        g->topk = raw < 1 ? 1u : ((unsigned)raw > n ? n : (unsigned)raw);
        const unsigned groups =
            (n + CAMBI_HIP_POOL_ELEMS_PER_GROUP - 1u) / CAMBI_HIP_POOL_ELEMS_PER_GROUP;
        g->pool_groups = groups > CAMBI_HIP_POOL_MAX_GROUPS ? CAMBI_HIP_POOL_MAX_GROUPS : groups;
    }
}

void cambi_hip_plan_bind_scales(CambiHipParams *p, uint16_t *preproc, uint16_t *alt,
                                uint16_t *filtered_h, unsigned speedup)
{
    assert(p != NULL);
    assert(preproc != NULL);
    const uint16_t *previous = preproc;
    for (unsigned scale = 0u; scale < CAMBI_HIP_NUM_SCALES; ++scale) {
        CambiHipScale *g = &p->scale[scale];
        g->image = ((scale + speedup) % 2u == 0u) ? preproc : alt;
        g->filtered_h = filtered_h;
        g->decimate_src = previous;
        previous = g->image;
    }
}

size_t cambi_hip_plan_hist_cells(const CambiHipParams *p)
{
    size_t most = 0u;
    for (unsigned scale = 0u; scale < CAMBI_HIP_NUM_SCALES; ++scale) {
        const CambiHipScale *g = &p->scale[scale];
        const size_t cells = (size_t)g->chunks * g->width;
        most = cells > most ? cells : most;
    }
    return most * p->levels;
}

#ifdef HAVE_HIPCC

/* ------------------------------------------------------------------ */
/* Device arena: one allocation, carved at init.                       */
/* ------------------------------------------------------------------ */

/* Byte offsets of every buffer inside the arena. */
typedef struct CambiHipArena {
    size_t params;
    size_t frame;
    size_t src;
    size_t preproc;
    size_t alt;
    size_t filtered_h;
    size_t mask;
    size_t q;
    size_t runs;
    size_t change;
    size_t cvals;
    size_t hist;
    size_t lut;
    size_t tvi;
    size_t weights;
    size_t ori_x;
    size_t ori_y;
    size_t total;
} CambiHipArena;

static size_t cambi_hip_arena_take(size_t *cursor, size_t bytes)
{
    const size_t offset = *cursor;
    *cursor += (bytes + CAMBI_HIP_ARENA_ALIGN - 1u) / CAMBI_HIP_ARENA_ALIGN * CAMBI_HIP_ARENA_ALIGN;
    return offset;
}

static CambiHipArena cambi_hip_arena_layout(const CambiStateHip *s)
{
    const CambiHipPlanInput *in = &s->plan;
    const size_t pixels = (size_t)in->proc_width * in->proc_height;
    const size_t half = (size_t)((in->proc_width + 1u) >> 1) * ((in->proc_height + 1u) >> 1);
    const size_t mask_words = (size_t)in->proc_height * ((in->proc_width + 31u) / 32u);
    const size_t hist_cells = cambi_hip_plan_hist_cells(&s->params);
    unsigned lut_size = 0u;
    (void)vmaf_cambi_reciprocal_lut(&lut_size);
    size_t cursor = 0u;
    CambiHipArena a;
    a.params = cambi_hip_arena_take(&cursor, sizeof(CambiHipParams));
    a.frame = cambi_hip_arena_take(&cursor, sizeof(CambiHipFrameState));
    a.src = cambi_hip_arena_take(&cursor, s->src_bytes);
    a.preproc = cambi_hip_arena_take(&cursor, pixels * sizeof(uint16_t));
    a.alt = cambi_hip_arena_take(&cursor, half * sizeof(uint16_t));
    a.filtered_h = cambi_hip_arena_take(&cursor, pixels * sizeof(uint16_t));
    a.mask = cambi_hip_arena_take(&cursor, pixels * sizeof(uint16_t));
    a.q = cambi_hip_arena_take(&cursor, pixels * sizeof(uint16_t));
    a.runs = cambi_hip_arena_take(&cursor, mask_words * sizeof(uint32_t));
    a.change = cambi_hip_arena_take(&cursor, mask_words * sizeof(uint32_t));
    a.cvals = cambi_hip_arena_take(&cursor, pixels * sizeof(float));
    a.hist = cambi_hip_arena_take(&cursor, hist_cells * sizeof(uint16_t));
    a.lut = cambi_hip_arena_take(&cursor, (size_t)lut_size * sizeof(float));
    a.tvi = cambi_hip_arena_take(&cursor, sizeof(s->tvi));
    a.weights = cambi_hip_arena_take(&cursor, CAMBI_HIP_MAX_DIFFS * sizeof(int32_t));
    a.ori_x = cambi_hip_arena_take(&cursor, (size_t)in->proc_width * sizeof(uint32_t));
    a.ori_y = cambi_hip_arena_take(&cursor, (size_t)in->proc_height * sizeof(uint32_t));
    a.total = cursor;
    return a;
}

/* Point params at the arena at device address `base`. */
static void cambi_hip_bind_params(CambiStateHip *s, unsigned char *base, const CambiHipArena *a)
{
    CambiHipParams *p = &s->params;
    p->src = base + a->src;
    p->preproc = (uint16_t *)(void *)(base + a->preproc);
    p->mask = (uint16_t *)(void *)(base + a->mask);
    p->q = (uint16_t *)(void *)(base + a->q);
    p->runs = (uint32_t *)(void *)(base + a->runs);
    p->change = (uint32_t *)(void *)(base + a->change);
    p->cvals = (float *)(void *)(base + a->cvals);
    p->hist = (uint16_t *)(void *)(base + a->hist);
    p->lut = (const float *)(const void *)(base + a->lut);
    p->tvi = (const uint16_t *)(const void *)(base + a->tvi);
    p->weights = (const int32_t *)(const void *)(base + a->weights);
    p->ori_x = p->same_size ? NULL : (const uint32_t *)(const void *)(base + a->ori_x);
    p->ori_y = p->same_size ? NULL : (const uint32_t *)(const void *)(base + a->ori_y);
    p->frame = (CambiHipFrameState *)(void *)(base + a->frame);
    cambi_hip_plan_bind_scales(p, p->preproc, (uint16_t *)(void *)(base + a->alt),
                               (uint16_t *)(void *)(base + a->filtered_h), s->plan.speedup);
    s->d_params = (CambiHipParams *)(void *)(base + a->params);
    s->d_frame = p->frame;
    s->d_src = base + a->src;
}

/* ------------------------------------------------------------------ */
/* Device setup and teardown.                                          */
/* ------------------------------------------------------------------ */

static int cambi_hip_module_load(CambiStateHip *s)
{
    hipError_t rc = hipModuleLoadData(&s->module, cambi_score_hsaco);
    if (rc != hipSuccess) {
        s->module = NULL;
        return vmaf_hip_rc_to_errno(rc);
    }
    for (int k = 0; k < CAMBI_K_COUNT && rc == hipSuccess; ++k)
        rc = hipModuleGetFunction(&s->kernels[k], s->module, cambi_hip_kernel_names[k]);
    return vmaf_hip_rc_to_errno(rc);
}

static int cambi_hip_compute_units(unsigned *out)
{
    int device = 0;
    int units = 0;
    hipError_t rc = hipGetDevice(&device);
    if (rc == hipSuccess)
        rc = hipDeviceGetAttribute(&units, hipDeviceAttributeMultiprocessorCount, device);
    *out = (rc == hipSuccess && units > 0) ? (unsigned)units : 1u;
    return vmaf_hip_rc_to_errno(rc);
}

/* Upload cambi.c's resize index tables (only when the encode is resized). */
static hipError_t cambi_hip_upload_resize(const CambiStateHip *s)
{
    const CambiHipPlanInput *in = &s->plan;
    uint32_t *ori_x = malloc(sizeof(uint32_t) * in->proc_width);
    uint32_t *ori_y = malloc(sizeof(uint32_t) * in->proc_height);
    hipError_t rc = (ori_x && ori_y) ? hipSuccess : hipErrorOutOfMemory;
    if (rc == hipSuccess) {
        vmaf_cambi_resize_source_indices(in->src_width, in->proc_width, ori_x);
        vmaf_cambi_resize_source_indices(in->src_height, in->proc_height, ori_y);
        rc = hipMemcpy((void *)s->params.ori_x, ori_x, sizeof(uint32_t) * in->proc_width,
                       hipMemcpyHostToDevice);
    }
    if (rc == hipSuccess)
        rc = hipMemcpy((void *)s->params.ori_y, ori_y, sizeof(uint32_t) * in->proc_height,
                       hipMemcpyHostToDevice);
    free(ori_x);
    free(ori_y);
    return rc;
}

/* The constant tables and the parameter block, once, at init. */
static int cambi_hip_upload_tables(CambiStateHip *s)
{
    unsigned lut_size = 0u;
    const float *lut = vmaf_cambi_reciprocal_lut(&lut_size);
    const CambiHipParams *p = &s->params;
    hipError_t rc = hipMemcpy((void *)p->lut, lut, sizeof(float) * lut_size, hipMemcpyHostToDevice);
    if (rc == hipSuccess)
        rc = hipMemcpy((void *)p->tvi, s->tvi, sizeof(s->tvi), hipMemcpyHostToDevice);
    if (rc == hipSuccess)
        rc = hipMemcpy((void *)p->weights, vmaf_cambi_contrast_weights(NULL),
                       sizeof(int32_t) * s->plan.num_diffs, hipMemcpyHostToDevice);
    if (rc == hipSuccess && !p->same_size)
        rc = cambi_hip_upload_resize(s);
    if (rc == hipSuccess)
        rc = hipMemcpy(s->d_params, p, sizeof(*p), hipMemcpyHostToDevice);
    return vmaf_hip_rc_to_errno(rc);
}

/* The device arena and the pinned staging and results blocks. */
static int cambi_hip_alloc(CambiStateHip *s)
{
    const CambiHipPlanInput *in = &s->plan;
    s->src_bytes = (size_t)in->src_width * in->src_height * (in->src_bpc <= 8u ? 1u : 2u);
    const CambiHipArena a = cambi_hip_arena_layout(s);
    hipError_t rc = hipMalloc(&s->d_arena, a.total);
    if (rc != hipSuccess) {
        s->d_arena = NULL;
        return vmaf_hip_rc_to_errno(rc);
    }
    const int err = vmaf_hip_picture_staging_alloc(&s->h_staging, s->src_bytes);
    if (err) {
        s->h_staging = NULL;
        return err;
    }
    rc = hipHostMalloc((void **)&s->h_results, sizeof(CambiHipResults), hipHostMallocDefault);
    if (rc != hipSuccess) {
        s->h_results = NULL;
        return vmaf_hip_rc_to_errno(rc);
    }
    cambi_hip_bind_params(s, (unsigned char *)s->d_arena, &a);
    return cambi_hip_upload_tables(s);
}

/* Release every device resource init acquired, whichever step failed: the
 * stream is drained and destroyed first, so no queued copy or kernel still
 * uses the memory freed after it. Safe on a partial init and on close. */
static int cambi_hip_release(CambiStateHip *s)
{
    int rc = vmaf_hip_kernel_lifecycle_close(&s->lc, s->ctx);
    vmaf_hip_picture_staging_free(s->h_staging);
    s->h_staging = NULL;
    if (s->h_results) {
        (void)hipHostFree(s->h_results);
        s->h_results = NULL;
    }
    if (s->d_arena) {
        (void)hipFree(s->d_arena);
        s->d_arena = NULL;
    }
    if (s->module) {
        const int err = vmaf_hip_rc_to_errno(hipModuleUnload(s->module));
        s->module = NULL;
        rc = rc ? rc : err;
    }
    if (s->ctx) {
        vmaf_hip_context_destroy(s->ctx);
        s->ctx = NULL;
    }
    return rc;
}

static int cambi_hip_setup_device(CambiStateHip *s)
{
    int err = vmaf_hip_context_new(&s->ctx, 0);
    if (!err)
        err = vmaf_hip_kernel_lifecycle_init(&s->lc, s->ctx);
    if (!err)
        err = cambi_hip_module_load(s);
    if (!err)
        err = cambi_hip_compute_units(&s->plan.compute_units);
    if (!err) {
        cambi_hip_plan(&s->params, &s->plan);
        err = cambi_hip_alloc(s);
    }
    return err;
}

/* ------------------------------------------------------------------ */
/* Per-frame enqueue: no host wait anywhere below.                     */
/* ------------------------------------------------------------------ */

static int cambi_hip_launch(CambiStateHip *s, int kernel, unsigned gx, unsigned gy, unsigned bx,
                            unsigned by, int scale, int pass)
{
    void *args[] = {&s->d_params, &scale, &pass};
    const hipError_t rc = hipModuleLaunchKernel(s->kernels[kernel], gx, gy, 1u, bx, by, 1u, 0u,
                                                vmaf_hip_stream_of(s->lc.str), args, NULL);
    return vmaf_hip_rc_to_errno(rc);
}

/* A CAMBI_HIP_TILE x CAMBI_HIP_TILE work-group per tile of a w x h image. */
static int cambi_hip_launch_image(CambiStateHip *s, int kernel, unsigned w, unsigned h, int scale)
{
    const unsigned gx = (w + CAMBI_HIP_TILE - 1u) / CAMBI_HIP_TILE;
    const unsigned gy = (h + CAMBI_HIP_TILE - 1u) / CAMBI_HIP_TILE;
    return cambi_hip_launch(s, kernel, gx, gy, CAMBI_HIP_TILE, CAMBI_HIP_TILE, scale, 0);
}

/* Radix scan 0 (pass 0 was counted by the c-values kernel), passes 1 and 2,
 * the partial sums above the threshold and the exact per-scale total. The
 * kernels return at once on the device when the threshold resolved to 0. */
static int cambi_hip_enqueue_pool(CambiStateHip *s, int scale)
{
    const unsigned groups = s->params.scale[scale].pool_groups;
    int err = cambi_hip_launch(s, CAMBI_K_RADIX_SCAN, 1u, 1u, CAMBI_HIP_POOL_BLOCK, 1u, scale, 0);
    for (int pass = 1; pass < CAMBI_HIP_RADIX_PASSES && !err; ++pass) {
        err = cambi_hip_launch(s, CAMBI_K_RADIX_HIST, groups, 1u, CAMBI_HIP_POOL_BLOCK, 1u, scale,
                               pass);
        if (!err)
            err = cambi_hip_launch(s, CAMBI_K_RADIX_SCAN, 1u, 1u, CAMBI_HIP_POOL_BLOCK, 1u, scale,
                                   pass);
    }
    if (!err)
        err = cambi_hip_launch(s, CAMBI_K_TOPK_SUM, groups, 1u, CAMBI_HIP_POOL_BLOCK, 1u, scale, 0);
    if (!err)
        err = cambi_hip_launch(s, CAMBI_K_TOPK_FINAL, 1u, 1u, 1u, 1u, scale, 0);
    return err;
}

/* cambi_score's per-scale body: decimate, filter_mode, calculate_c_values,
 * spatial_pooling. */
static int cambi_hip_enqueue_scale(CambiStateHip *s, int scale)
{
    const CambiHipScale *g = &s->params.scale[scale];
    int err = 0;
    if (g->decimate)
        err = cambi_hip_launch_image(s, CAMBI_K_DECIMATE, g->width, g->height, scale);
    if (!err)
        err = cambi_hip_launch_image(s, CAMBI_K_FILTER_H, g->width, g->height, scale);
    if (!err)
        err = cambi_hip_launch_image(s, CAMBI_K_FILTER_V, g->width, g->height, scale);
    if (!err) {
        const unsigned rows = (g->height + CAMBI_HIP_ROWMASK_ROWS - 1u) / CAMBI_HIP_ROWMASK_ROWS;
        err = cambi_hip_launch(s, CAMBI_K_ROW_MASKS, g->words, rows, 32u, CAMBI_HIP_ROWMASK_ROWS,
                               scale, 0);
    }
    if (!err) {
        const unsigned columns = (g->width + CAMBI_HIP_CVALS_BLOCK - 1u) / CAMBI_HIP_CVALS_BLOCK;
        err = cambi_hip_launch(s, CAMBI_K_CVALS, columns, g->chunks, CAMBI_HIP_CVALS_BLOCK, 1u,
                               scale, 0);
    }
    if (!err)
        err = cambi_hip_enqueue_pool(s, scale);
    return err;
}

/* The whole frame: the staged upload, reset, preprocessing, mask, five
 * scales, one readback. The host copy into staging has read the picture
 * before this returns, so the caller may refill it at once
 * (core/src/feature/hip/AGENTS.md, "Picture uploads"). */
static int cambi_hip_enqueue_frame(CambiStateHip *s, const VmafPicture *dist)
{
    const CambiHipPlanInput *in = &s->plan;
    if (!dist || dist->w[0] < in->src_width || dist->h[0] < in->src_height)
        return -EINVAL;
    const size_t row_bytes = (size_t)in->src_width * (in->src_bpc <= 8u ? 1u : 2u);
    const VmafHipPlaneUpload plane = {.dst = s->d_src,
                                      .dst_pitch = row_bytes,
                                      .pic = dist,
                                      .plane = 0u,
                                      .row_bytes = row_bytes,
                                      .rows = in->src_height};
    const hipStream_t stream = vmaf_hip_stream_of(s->lc.str);
    int err = vmaf_hip_picture_upload_staged(&plane, 1u, s->h_staging, s->src_bytes, s->lc.str);
    if (!err)
        err =
            vmaf_hip_rc_to_errno(hipMemsetAsync(s->d_frame, 0, sizeof(CambiHipFrameState), stream));
    if (!err && s->params.validate)
        err = cambi_hip_launch_image(s, CAMBI_K_VALIDATE, in->src_width, in->src_height, 0);
    if (!err)
        err = cambi_hip_launch_image(s, CAMBI_K_PREPROCESS, in->proc_width, in->proc_height, 0);
    if (!err)
        err = cambi_hip_launch_image(s, CAMBI_K_MASK, in->proc_width, in->proc_height, 0);
    for (int scale = 0; scale < CAMBI_HIP_NUM_SCALES && !err; ++scale)
        err = cambi_hip_enqueue_scale(s, scale);
    if (!err)
        err = vmaf_hip_rc_to_errno(hipMemcpyAsync(s->h_results, &s->d_frame->results,
                                                  sizeof(CambiHipResults), hipMemcpyDeviceToHost,
                                                  stream));
    return err;
}

#endif /* HAVE_HIPCC */

/* ------------------------------------------------------------------ */
/* Extractor callbacks.                                                */
/* ------------------------------------------------------------------ */

static int init_fex_hip(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                        unsigned w, unsigned h)
{
    (void)pix_fmt;
#ifndef HAVE_HIPCC
    /* Scaffold posture: -ENOSYS and nothing else (ADR-1264). */
    (void)fex;
    (void)bpc;
    (void)w;
    (void)h;
    return -ENOSYS;
#else
    CambiStateHip *s = fex->priv;
    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (!s->feature_name_dict)
        return -ENOMEM;

    int err = cambi_hip_configure(s, bpc, w, h);
    if (!err)
        err = cambi_hip_setup_device(s);
    if (err) {
        (void)cambi_hip_release(s);
        (void)vmaf_dictionary_free(&s->feature_name_dict);
    }
    return err;
#endif /* HAVE_HIPCC */
}

static int submit_fex_hip(VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                          VmafPicture *dist_pic, VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic;
    (void)ref_pic_90;
    (void)dist_pic_90;
    (void)index;
#ifndef HAVE_HIPCC
    (void)fex;
    (void)dist_pic;
    return -ENOSYS;
#else
    CambiStateHip *s = fex->priv;
    int err = cambi_hip_enqueue_frame(s, dist_pic);
    if (!err)
        err = vmaf_hip_kernel_submit_post_record(&s->lc, s->ctx);
    return err;
#endif /* HAVE_HIPCC */
}

/* The one wait of the frame, then cambi.c's pooling mean and scale
 * weighting on five exact sums. */
static int collect_fex_hip(VmafFeatureExtractor *fex, unsigned index,
                           VmafFeatureCollector *feature_collector)
{
#ifndef HAVE_HIPCC
    (void)fex;
    (void)index;
    (void)feature_collector;
    return -ENOSYS;
#else
    CambiStateHip *s = fex->priv;
    const int err = vmaf_hip_kernel_collect_wait(&s->lc, s->ctx);
    if (err)
        return err;
    const CambiHipResults *r = s->h_results;
    if (r->status & CAMBI_HIP_STATUS_INVALID_INPUT) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "cambi_hip: frame %u holds samples above the %u-bit maximum\n", index,
                 s->plan.src_bpc);
        return -EINVAL;
    }
    double scores[CAMBI_HIP_NUM_SCALES];
    for (unsigned scale = 0u; scale < CAMBI_HIP_NUM_SCALES; ++scale)
        scores[scale] = vmaf_cambi_fixed_topk_mean(r->sum_hi[scale], r->sum_lo[scale],
                                                   s->params.scale[scale].topk);
    const uint16_t pixels = vmaf_cambi_get_pixels_in_window((uint16_t)s->plan.adjusted_window);
    double score = vmaf_cambi_weight_scores_per_scale(scores, pixels);
    if (score > s->cambi_max_val)
        score = s->cambi_max_val;
    if (score < 0.0)
        score = 0.0;
    return vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "Cambi_feature_cambi_score", score, index);
#endif /* HAVE_HIPCC */
}

static int close_fex_hip(VmafFeatureExtractor *fex)
{
    CambiStateHip *s = fex->priv;
    int rc = 0;
#ifdef HAVE_HIPCC
    rc = cambi_hip_release(s);
#endif /* HAVE_HIPCC */
    if (s->feature_name_dict) {
        const int e = vmaf_dictionary_free(&s->feature_name_dict);
        rc = rc ? rc : e;
    }
    return rc;
}

static const char *provided_features[] = {"Cambi_feature_cambi_score", NULL};

/* Load-bearing: declared `extern` in feature_extractor.c's
 * `feature_extractor_list[]` under `#if HAVE_HIP`. Making this static
 * would unlink the extractor from the registry. Same pattern as every
 * HIP extractor (see vmaf_fex_float_psnr_hip). */
// NOLINTNEXTLINE(misc-use-internal-linkage) -- ADR-0254: registry linkage invariant
VmafFeatureExtractor vmaf_fex_cambi_hip = {
    .name = "cambi_hip",
    .init = init_fex_hip,
    .submit = submit_fex_hip,
    .collect = collect_fex_hip,
    .close = close_fex_hip,
    .options = options,
    .priv_size = sizeof(CambiStateHip),
    .provided_features = provided_features,
    .flags = VMAF_FEATURE_EXTRACTOR_HIP,
    /* Device-resident (ADR-1378): about 64 launches per frame on one stream
     * -- upload, reset, validate, preprocess, mask, and per scale decimate,
     * filter, masks, c-values and seven top-K pooling kernels -- and one
     * readback; no host stage remains, so frames overlap through the normal
     * submit / collect double buffering. */
    .chars =
        {
            .n_dispatches_per_frame = 64,
            .is_reduction_only = false,
            .min_useful_frame_area = 1920U * 1080U,
            .dispatch_hint = VMAF_FEATURE_DISPATCH_AUTO,
        },
};

/* NOLINTEND(modernize-use-nullptr) */
