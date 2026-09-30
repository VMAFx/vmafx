/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  CAMBI banding-detection feature extractor on the CUDA backend
 *  (T3-15 / ADR-0360), fully device-resident since ADR-1379 (the CUDA port of
 *  the SYCL design of ADR-1357, core/src/feature/sycl/integer_cambi_sycl.cpp).
 *
 *  Per frame, submit() enqueues every stage of cambi.c on the distorted
 *  picture's stream, the stream its upload ran on:
 *
 *    reset         the per-scale selection state and the result block
 *                  (device memsets);
 *    validate      samples above the declared bit depth (only for bpc other
 *                  than 8 and 16, as cambi.c::validate_image);
 *    preprocess    10-bit conversion, resize to enc_width x enc_height through
 *                  init-time index tables, anti-dither when enc_bitdepth < 10;
 *    spatial mask  the ADR-0464 shared-memory tile;
 *    per scale     decimate (scale > 0 or high-res speed-up), the horizontal
 *                  and vertical mode filter (the vertical pass also writes the
 *                  level map), the per-row run/change bit masks, the c-values
 *                  (+ top-K radix pass 0) and the exact top-K pooling;
 *
 *  then one 88-byte device-to-host copy of the five per-scale top-K sums and
 *  the status word on the private stream. collect() waits once and weights
 *  the scales on the host with cambi.c's own helpers. No host code touches
 *  pixel, histogram or c-value data, and nothing waits mid-frame.
 *
 *  Precision contract (ADR-1357): the per-frame score is bit-identical to
 *  cambi.c whenever cambi.c's own sequential top-K double sum is exact;
 *  otherwise the device value is the exactly rounded mean and the CPU carries
 *  its accumulation rounding.
 *
 *  Out of scope, as since ADR-0360: full_ref (FR-CAMBI) and the heatmap dump.
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature_name.h"
#include "cuda/integer_cambi_cuda.h"
#include "cuda/kernel_template.h"
#include "cuda_helper.cuh"
#include "log.h"
#include "picture.h"
#include "picture_cuda.h"

#include "feature/cambi_internal.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `NULL` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

#if CAMBI_CUDA_NUM_SCALES != VMAF_CAMBI_NUM_SCALES
#error "CAMBI_CUDA_NUM_SCALES must equal VMAF_CAMBI_NUM_SCALES"
#endif
#if CAMBI_CUDA_FIXED_SHIFT != VMAF_CAMBI_TOPK_FIXED_SHIFT
#error "the device fixed point must match vmaf_cambi_fixed_topk_mean()"
#endif

#define CAMBI_CUDA_DEFAULT_MAX_VAL 1000.0
#define CAMBI_CUDA_DEFAULT_WINDOW_SIZE 65
#define CAMBI_CUDA_DEFAULT_TOPK 0.6
#define CAMBI_CUDA_DEFAULT_TVI 0.019
#define CAMBI_CUDA_DEFAULT_VLT 0.0
#define CAMBI_CUDA_DEFAULT_MAX_LOG_CONTRAST 2
#define CAMBI_CUDA_DEFAULT_EOTF "bt1886"
#define CAMBI_CUDA_MAX_DIFFS 32 /* 1 << max_log_contrast, max_log_contrast <= 5 */

/* c-values chunking (integer_cambi_sycl.cpp cvals_chunks()): enough column
 * threads to fill the device, row chunks of at least CAMBI_CUDA_MIN_CHUNK_ROWS
 * rows, and at most CAMBI_CUDA_HIST_BUDGET bytes of per-chunk histograms. */
#define CAMBI_CUDA_ITEMS_PER_SM 512u
#define CAMBI_CUDA_FALLBACK_SMS 32
#define CAMBI_CUDA_MIN_CHUNK_ROWS 32u
#define CAMBI_CUDA_HIST_BUDGET ((size_t)64u << 20u)
/* Pooling blocks: a block of a 7680 x 7680 frame holds under 2^17 elements,
 * so its fixed-point partial sum (each term < 2^38) stays below 2^55. */
#define CAMBI_CUDA_POOL_MAX_GROUPS 512u
#define CAMBI_CUDA_POOL_ELEMS_PER_GROUP 4096u

typedef enum {
    CAMBI_FN_VALIDATE,
    CAMBI_FN_PREPROCESS,
    CAMBI_FN_MASK,
    CAMBI_FN_DECIMATE,
    CAMBI_FN_FILTER_H,
    CAMBI_FN_FILTER_V,
    CAMBI_FN_ROW_MASKS,
    CAMBI_FN_CVALS,
    CAMBI_FN_RADIX_HIST,
    CAMBI_FN_RADIX_SCAN,
    CAMBI_FN_TOPK_PARTIALS,
    CAMBI_FN_TOPK_FINAL,
    CAMBI_FN_COUNT,
} CambiCudaKernel;

static const char *const cambi_kernel_names[CAMBI_FN_COUNT] = {
    "cambi_validate_kernel",   "cambi_preprocess_kernel",    "cambi_spatial_mask_kernel",
    "cambi_decimate_kernel",   "cambi_filter_mode_h_kernel", "cambi_filter_mode_v_levels_kernel",
    "cambi_row_masks_kernel",  "cambi_cvals_kernel",         "cambi_radix_hist_kernel",
    "cambi_radix_scan_kernel", "cambi_topk_partials_kernel", "cambi_topk_final_kernel",
};

/* Device buffers, one table so allocation and release walk the same list. */
typedef enum {
    CAMBI_BUF_IMAGE,
    CAMBI_BUF_MASK,
    CAMBI_BUF_TMP,
    CAMBI_BUF_Q,
    CAMBI_BUF_RUNS,
    CAMBI_BUF_CHANGE,
    CAMBI_BUF_CVALS,
    CAMBI_BUF_HIST,
    CAMBI_BUF_LUT,
    CAMBI_BUF_TVI,
    CAMBI_BUF_WEIGHTS,
    CAMBI_BUF_ORI_X,
    CAMBI_BUF_ORI_Y,
    CAMBI_BUF_SELECT,
    CAMBI_BUF_PARTIALS,
    CAMBI_BUF_RESULTS,
    CAMBI_BUF_COUNT,
} CambiCudaBufferId;

/* Init-time geometry of one scale. */
typedef struct CambiCudaScaleGeom {
    unsigned width;
    unsigned height;
    unsigned chunks;
    unsigned chunk_rows;
    unsigned cvals_groups;
    unsigned pool_groups;
    unsigned topk;
} CambiCudaScaleGeom;

typedef struct CambiStateCuda {
    VmafCudaState *cu_state;
    VmafCudaKernelLifecycle lc; /* private readback stream + fences */
    CUmodule module;
    CUfunction fn[CAMBI_FN_COUNT];
    VmafCudaBuffer *buf[CAMBI_BUF_COUNT];
    CambiCudaResults *h_results; /* pinned */

    /* Configuration options (mirrors CambiState). */
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
    /* Resolved at init against the encode pixel count, exactly as cambi.c
     * does: a value of 1080 / 1440 / 2160 below its own threshold is reset to
     * 0. Non-zero means "decimate once before scale 0 and halve the adjusted
     * window" (cambi.c::cambi_score / adjust_window_size). */
    int cambi_high_res_speedup;

    /* Resolved geometry and contrast tables. */
    unsigned src_width;
    unsigned src_height;
    unsigned src_bpc;
    unsigned proc_width;
    unsigned proc_height;
    unsigned num_diffs;
    unsigned levels;
    unsigned mask_index;
    uint16_t adjusted_window;
    uint16_t vlt_luma;
    uint16_t v_band_base;
    uint16_t v_band_size;
    uint16_t tvi_for_diff[CAMBI_CUDA_MAX_DIFFS];
    CambiCudaScaleGeom geom[CAMBI_CUDA_NUM_SCALES];
    size_t hist_elements;
    size_t partial_elements;

    VmafDictionary *feature_name_dict;
} CambiStateCuda;

/* --- Options --- */
static const VmafOption options[] = {
    {
        .name = "cambi_max_val",
        .help = "maximum value allowed; larger values will be clipped",
        .offset = offsetof(CambiStateCuda, cambi_max_val),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = CAMBI_CUDA_DEFAULT_MAX_VAL,
        .min = 0.0,
        .max = 1000.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "cmxv",
    },
    {
        .name = "enc_width",
        .help = "Encoding width",
        .offset = offsetof(CambiStateCuda, enc_width),
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
        .offset = offsetof(CambiStateCuda, enc_height),
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
        .offset = offsetof(CambiStateCuda, enc_bitdepth),
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
        .offset = offsetof(CambiStateCuda, window_size),
        .type = VMAF_OPT_TYPE_INT,
        .default_val.i = CAMBI_CUDA_DEFAULT_WINDOW_SIZE,
        .min = 15,
        .max = 127,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "ws",
    },
    {
        .name = "topk",
        .help = "Ratio of pixels for the spatial pooling computation",
        .offset = offsetof(CambiStateCuda, topk),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = CAMBI_CUDA_DEFAULT_TOPK,
        .min = 0.0001,
        .max = 1.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "cambi_topk",
        .help = "Ratio of pixels for the spatial pooling computation",
        .offset = offsetof(CambiStateCuda, cambi_topk),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = CAMBI_CUDA_DEFAULT_TOPK,
        .min = 0.0001,
        .max = 1.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "ctpk",
    },
    {
        .name = "tvi_threshold",
        .help = "Visibility threshold ΔL < tvi_threshold * L_mean",
        .offset = offsetof(CambiStateCuda, tvi_threshold),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = CAMBI_CUDA_DEFAULT_TVI,
        .min = 0.0001,
        .max = 1.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "tvit",
    },
    {
        .name = "cambi_vis_lum_threshold",
        .help = "Luminance value below which banding is assumed invisible",
        .offset = offsetof(CambiStateCuda, cambi_vis_lum_threshold),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = CAMBI_CUDA_DEFAULT_VLT,
        .min = 0.0,
        .max = 300.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "vlt",
    },
    {
        .name = "max_log_contrast",
        .help = "Maximum log contrast (0 to 5, default 2)",
        .offset = offsetof(CambiStateCuda, max_log_contrast),
        .type = VMAF_OPT_TYPE_INT,
        .default_val.i = CAMBI_CUDA_DEFAULT_MAX_LOG_CONTRAST,
        .min = 0,
        .max = 5,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "mlc",
    },
    {
        .name = "eotf",
        .help = "EOTF for visibility-threshold conversion (bt1886 / pq)",
        .offset = offsetof(CambiStateCuda, eotf),
        .type = VMAF_OPT_TYPE_STRING,
        .default_val.s = CAMBI_CUDA_DEFAULT_EOTF,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "cambi_eotf",
        .help = "EOTF override for cambi (defaults to eotf)",
        .offset = offsetof(CambiStateCuda, cambi_eotf),
        .type = VMAF_OPT_TYPE_STRING,
        .default_val.s = CAMBI_CUDA_DEFAULT_EOTF,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "ceot",
    },
    {
        .name = "cambi_high_res_speedup",
        .help =
            "Speed up the processing by downsampling post spatial mask for resolutions >= 1080p. "
            "Min speed-up resolution possible values: [1080, 1440, 2160, 0]. Default: 0 (not applied)",
        .offset = offsetof(CambiStateCuda, cambi_high_res_speedup),
        .type = VMAF_OPT_TYPE_INT,
        .default_val.i = 0,
        .min = 0,
        .max = 2160,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "hrs",
    },
    {0},
};

/* ------------------------------------------------------------------ */
/* Configuration (cambi.c::init)                                       */
/* ------------------------------------------------------------------ */

static bool cambi_speedup_is_valid(int requested, int pixels)
{
    if (requested == 1080)
        return pixels >= CAMBI_HIGH_RES_SPEEDUP_THRESHOLD_1080p;
    if (requested == 1440)
        return pixels >= CAMBI_HIGH_RES_SPEEDUP_THRESHOLD_1440p;
    if (requested == 2160)
        return pixels >= CAMBI_HIGH_RES_SPEEDUP_THRESHOLD_2160p;
    return false;
}

/* Encoded geometry, high-res speed-up, window and mask index, resolved as
 * cambi.c::validate_and_setup_dimensions() and init() resolve them. */
static int cambi_resolve_geometry(CambiStateCuda *s, unsigned bpc, unsigned w, unsigned h)
{
    if (s->enc_bitdepth == 0)
        s->enc_bitdepth = (int)bpc;
    if (s->enc_width == 0 || s->enc_height == 0 || (unsigned)s->enc_height > h ||
        (unsigned)s->enc_width > w) {
        s->enc_width = (int)w;
        s->enc_height = (int)h;
    }
    if (!cambi_validate_dimensions((unsigned)s->enc_width, (unsigned)s->enc_height)) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "cambi_cuda: encoded resolution %dx%d below minimum %d×%d.\n", s->enc_width,
                 s->enc_height, CAMBI_MIN_WIDTH_HEIGHT, CAMBI_MIN_WIDTH_HEIGHT);
        return -EINVAL;
    }
    if (!cambi_speedup_is_valid(s->cambi_high_res_speedup, s->enc_width * s->enc_height))
        s->cambi_high_res_speedup = 0;
    s->src_width = w;
    s->src_height = h;
    s->src_bpc = bpc;
    s->proc_width = (unsigned)s->enc_width;
    s->proc_height = (unsigned)s->enc_height;
    s->adjusted_window = vmaf_cambi_adjust_window(s->window_size, s->proc_width, s->proc_height,
                                                  (bool)s->cambi_high_res_speedup);
    s->mask_index = vmaf_cambi_mask_index(s->proc_width, s->proc_height);
    s->num_diffs = 1u << (unsigned)s->max_log_contrast;
    return 0;
}

/* tvi_for_diff / vlt_luma / v_band through cambi.c's own routine, then
 * cambi.c::setup_contrast_and_luminance()'s window guard at the same point
 * (after the TVI tables), both windows, same code and message. */
static int cambi_init_contrast(CambiStateCuda *s)
{
    uint16_t diffs[CAMBI_CUDA_MAX_DIFFS];
    for (unsigned d = 0u; d < s->num_diffs; d++)
        diffs[d] = (uint16_t)(d + 1u);
    const int err = vmaf_cambi_init_tvi_and_vlt(
        (int)s->num_diffs, diffs, s->tvi_threshold, s->cambi_vis_lum_threshold, s->cambi_eotf,
        s->eotf, s->tvi_for_diff, &s->vlt_luma, &s->v_band_base, &s->v_band_size);
    if (err)
        return err;
    s->levels = s->v_band_size;
    const uint16_t src_window = vmaf_cambi_adjust_window(
        s->window_size, s->src_width, s->src_height, (bool)s->cambi_high_res_speedup);
    return vmaf_cambi_check_window_fits_lut(s->adjusted_window, src_window);
}

/* Row chunks for the c-values pass. One thread walks one column of one
 * chunk, row after row, so a chunk is a latency chain: enough of them keep
 * the device busy, and at least CAMBI_CUDA_MIN_CHUNK_ROWS rows each keep the
 * per-chunk window priming a minor cost. */
static unsigned cambi_cvals_chunks(unsigned width, unsigned height, unsigned levels, int sms)
{
    const unsigned target_items = (unsigned)sms * CAMBI_CUDA_ITEMS_PER_SM;
    unsigned chunks = (target_items + width - 1u) / width;
    const size_t chunk_bytes = (size_t)width * levels * sizeof(uint16_t);
    size_t max_chunks = height / CAMBI_CUDA_MIN_CHUNK_ROWS;
    const size_t by_memory = CAMBI_CUDA_HIST_BUDGET / chunk_bytes;
    if (max_chunks > by_memory)
        max_chunks = by_memory;
    if (max_chunks < 1u)
        max_chunks = 1u;
    if (chunks > max_chunks)
        chunks = (unsigned)max_chunks;
    return chunks < 1u ? 1u : chunks;
}

static unsigned cambi_clamp_unsigned(unsigned value, unsigned low, unsigned high)
{
    return value < low ? low : (value > high ? high : value);
}

/* Per-scale dimensions (cambi_score's scaled_width / scaled_height walk),
 * c-values chunking and top-K counts (spatial_pooling's clip()). */
static void cambi_scale_geometry(CambiStateCuda *s, double topk, int sms)
{
    unsigned width = s->proc_width;
    unsigned height = s->proc_height;
    s->hist_elements = 0u;
    s->partial_elements = CAMBI_CUDA_POOL_MAX_GROUPS;
    for (int scale = 0; scale < CAMBI_CUDA_NUM_SCALES; scale++) {
        if (scale > 0 || s->cambi_high_res_speedup) {
            width = (width + 1u) >> 1;
            height = (height + 1u) >> 1;
        }
        CambiCudaScaleGeom *g = &s->geom[scale];
        g->width = width;
        g->height = height;
        g->chunks = cambi_cvals_chunks(width, height, s->levels, sms);
        g->chunk_rows = (height + g->chunks - 1u) / g->chunks;
        g->chunks = (height + g->chunk_rows - 1u) / g->chunk_rows;
        g->cvals_groups =
            g->chunks * ((width + CAMBI_CUDA_CVALS_BLOCK - 1u) / CAMBI_CUDA_CVALS_BLOCK);
        const unsigned n = width * height;
        const int raw = (int)(topk * (int)n);
        g->topk = cambi_clamp_unsigned(raw < 1 ? 1u : (unsigned)raw, 1u, n);
        g->pool_groups = cambi_clamp_unsigned((n + CAMBI_CUDA_POOL_ELEMS_PER_GROUP - 1u) /
                                                  CAMBI_CUDA_POOL_ELEMS_PER_GROUP,
                                              1u, CAMBI_CUDA_POOL_MAX_GROUPS);
        const size_t hist = (size_t)g->chunks * width * s->levels;
        if (hist > s->hist_elements)
            s->hist_elements = hist;
        if (g->cvals_groups > s->partial_elements)
            s->partial_elements = g->cvals_groups;
    }
}

/* ------------------------------------------------------------------ */
/* Device state                                                        */
/* ------------------------------------------------------------------ */

static bool cambi_resizes(const CambiStateCuda *s)
{
    return s->proc_width != s->src_width || s->proc_height != s->src_height;
}

static size_t cambi_buffer_bytes(const CambiStateCuda *s, CambiCudaBufferId id)
{
    const size_t pixels = (size_t)s->proc_width * s->proc_height;
    const size_t words = (size_t)s->proc_height * ((s->proc_width + 31u) / 32u);
    unsigned lut_size = 0u;
    switch (id) {
    case CAMBI_BUF_IMAGE:
    case CAMBI_BUF_MASK:
    case CAMBI_BUF_TMP:
    case CAMBI_BUF_Q:
        return pixels * sizeof(uint16_t);
    case CAMBI_BUF_RUNS:
    case CAMBI_BUF_CHANGE:
        return words * sizeof(uint32_t);
    case CAMBI_BUF_CVALS:
        return pixels * sizeof(float);
    case CAMBI_BUF_HIST:
        return s->hist_elements * sizeof(uint16_t);
    case CAMBI_BUF_LUT:
        (void)vmaf_cambi_reciprocal_lut(&lut_size);
        return (size_t)lut_size * sizeof(float);
    case CAMBI_BUF_TVI:
        return s->num_diffs * sizeof(uint16_t);
    case CAMBI_BUF_WEIGHTS:
        return s->num_diffs * sizeof(int);
    case CAMBI_BUF_ORI_X:
        return cambi_resizes(s) ? s->proc_width * sizeof(uint32_t) : 0u;
    case CAMBI_BUF_ORI_Y:
        return cambi_resizes(s) ? s->proc_height * sizeof(uint32_t) : 0u;
    case CAMBI_BUF_SELECT:
        return CAMBI_CUDA_NUM_SCALES * sizeof(CambiCudaSelect);
    case CAMBI_BUF_PARTIALS:
        return s->partial_elements * sizeof(uint64_t);
    case CAMBI_BUF_RESULTS:
        return sizeof(CambiCudaResults);
    default:
        return 0u;
    }
}

static CUdeviceptr cambi_dptr(const CambiStateCuda *s, CambiCudaBufferId id)
{
    return s->buf[id] ? s->buf[id]->data : (CUdeviceptr)0;
}

static int cambi_preserve(int rc, int err)
{
    return rc ? rc : err;
}

/* The single teardown path of init and close: quiesce the private stream,
 * then release every buffer, the pinned block, the module and the name
 * dictionary. A handle whose release fails is kept for a retry. */
static int cambi_release(CambiStateCuda *s, int rc)
{
    if (s->cu_state) {
        const int lc_rc = vmaf_cuda_kernel_lifecycle_close(&s->lc, s->cu_state);
        if (lc_rc)
            return cambi_preserve(rc, lc_rc);
        for (int id = 0; id < CAMBI_BUF_COUNT; id++)
            rc = cambi_preserve(rc, vmaf_cuda_buffer_free_owned(s->cu_state, &s->buf[id]));
        rc = cambi_preserve(rc,
                            vmaf_cuda_buffer_host_free_owned(s->cu_state, (void **)&s->h_results));
        rc = cambi_preserve(rc, vmaf_cuda_module_unload(s->cu_state, &s->module));
    }
    if (s->feature_name_dict)
        rc = cambi_preserve(rc, vmaf_dictionary_free(&s->feature_name_dict));
    return rc;
}

/* Run `body` with the extractor's context current and pop it on every path;
 * the body's error wins over a failed pop. */
static int cambi_in_context(CambiStateCuda *s, int (*body)(CambiStateCuda *))
{
    CudaFunctions *cu_f = s->cu_state->f;
    CHECK_CUDA_RETURN(cu_f, cuCtxPushCurrent(s->cu_state->ctx));
    const int err = body(s);
    const CUresult pop = cu_f->cuCtxPopCurrent(NULL);
    return err ? err : vmaf_cuda_result_to_errno((int)pop);
}

static int cambi_load_kernels(CambiStateCuda *s)
{
    CudaFunctions *cu_f = s->cu_state->f;
    CHECK_CUDA_RETURN(cu_f, cuModuleLoadData(&s->module, cambi_score_ptx));
    for (int k = 0; k < CAMBI_FN_COUNT; k++)
        CHECK_CUDA_RETURN(cu_f, cuModuleGetFunction(&s->fn[k], s->module, cambi_kernel_names[k]));
    return 0;
}

static int cambi_allocate(CambiStateCuda *s)
{
    for (int id = 0; id < CAMBI_BUF_COUNT; id++) {
        const size_t bytes = cambi_buffer_bytes(s, (CambiCudaBufferId)id);
        if (bytes == 0u)
            continue; /* the resize tables of a same-size run */
        const int err = vmaf_cuda_buffer_alloc(s->cu_state, &s->buf[id], bytes);
        if (err)
            return err;
    }
    return vmaf_cuda_buffer_host_alloc(s->cu_state, (void **)&s->h_results,
                                       sizeof(CambiCudaResults));
}

static int cambi_upload(CambiStateCuda *s, CambiCudaBufferId id, const void *host)
{
    CHECK_CUDA_RETURN(s->cu_state->f,
                      cuMemcpyHtoD(cambi_dptr(s, id), host, cambi_buffer_bytes(s, id)));
    return 0;
}

/* The constant tables, once at init (the copies may be synchronous), all
 * from cambi.c itself: the reciprocal table c_value_pixel() multiplies by,
 * the TVI table, the contrast weights and, when resizing, the source-index
 * walk of decimate_generic_*_and_convert_to_10b. */
static int cambi_upload_tables(CambiStateCuda *s)
{
    int err = cambi_upload(s, CAMBI_BUF_LUT, vmaf_cambi_reciprocal_lut(NULL));
    if (!err)
        err = cambi_upload(s, CAMBI_BUF_TVI, s->tvi_for_diff);
    if (!err)
        err = cambi_upload(s, CAMBI_BUF_WEIGHTS, vmaf_cambi_contrast_weights(NULL));
    if (err || !cambi_resizes(s))
        return err;
    uint32_t *ori_x = malloc(s->proc_width * sizeof(uint32_t));
    uint32_t *ori_y = malloc(s->proc_height * sizeof(uint32_t));
    err = (ori_x && ori_y) ? 0 : -ENOMEM;
    if (!err) {
        vmaf_cambi_resize_source_indices(s->src_width, s->proc_width, ori_x);
        vmaf_cambi_resize_source_indices(s->src_height, s->proc_height, ori_y);
        err = cambi_upload(s, CAMBI_BUF_ORI_X, ori_x);
    }
    if (!err)
        err = cambi_upload(s, CAMBI_BUF_ORI_Y, ori_y);
    free(ori_x);
    free(ori_y);
    return err;
}

static int cambi_init_device(VmafFeatureExtractor *fex, CambiStateCuda *s)
{
    int sms = 0;
    if (fex->cu_state->f->cuDeviceGetAttribute(&sms, CU_DEVICE_ATTRIBUTE_MULTIPROCESSOR_COUNT,
                                               fex->cu_state->dev) != CUDA_SUCCESS ||
        sms < 1)
        sms = CAMBI_CUDA_FALLBACK_SMS; /* chunking only: any count is correct */
    const double topk = (s->topk != CAMBI_CUDA_DEFAULT_TOPK) ? s->topk : s->cambi_topk;
    cambi_scale_geometry(s, topk, sms);
    s->cu_state = fex->cu_state;
    int err = vmaf_cuda_kernel_lifecycle_init(&s->lc, fex->cu_state);
    if (!err)
        err = cambi_in_context(s, cambi_load_kernels);
    if (!err)
        err = cambi_allocate(s);
    if (!err)
        err = cambi_in_context(s, cambi_upload_tables);
    return err;
}

static int init_fex_cuda(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                         unsigned w, unsigned h)
{
    (void)pix_fmt;
    CambiStateCuda *s = fex->priv;
    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (!s->feature_name_dict)
        return -ENOMEM;
    int err = cambi_resolve_geometry(s, bpc, w, h);
    if (!err)
        err = cambi_init_contrast(s);
    if (!err)
        err = cambi_init_device(fex, s);
    if (err)
        return cambi_release(s, err);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Per frame: enqueue only                                             */
/* ------------------------------------------------------------------ */

typedef struct CambiLaunch {
    unsigned grid_x;
    unsigned grid_y;
    unsigned block_x;
    unsigned block_y;
} CambiLaunch;

static CambiLaunch cambi_image_launch(unsigned width, unsigned height)
{
    const CambiLaunch l = {
        .grid_x = (width + CAMBI_CUDA_IMAGE_BLOCK_X - 1u) / CAMBI_CUDA_IMAGE_BLOCK_X,
        .grid_y = (height + CAMBI_CUDA_IMAGE_BLOCK_Y - 1u) / CAMBI_CUDA_IMAGE_BLOCK_Y,
        .block_x = CAMBI_CUDA_IMAGE_BLOCK_X,
        .block_y = CAMBI_CUDA_IMAGE_BLOCK_Y,
    };
    return l;
}

static CambiLaunch cambi_linear_launch(unsigned grid, unsigned threads)
{
    const CambiLaunch l = {.grid_x = grid, .grid_y = 1u, .block_x = threads, .block_y = 1u};
    return l;
}

/* Every kernel takes its one argument block by value. */
static int cambi_launch(const CambiStateCuda *s, CambiCudaKernel k, CambiLaunch l, CUstream stream,
                        void *args)
{
    void *params[] = {args};
    CHECK_CUDA_RETURN(s->cu_state->f, cuLaunchKernel(s->fn[k], l.grid_x, l.grid_y, 1u, l.block_x,
                                                     l.block_y, 1u, 0u, stream, params, NULL));
    return 0;
}

/* The per-scale selection state and the result block start every frame at
 * zero (device memsets on the frame's stream, no host wait). */
static int cambi_enqueue_reset(const CambiStateCuda *s, CUstream stream)
{
    CudaFunctions *cu_f = s->cu_state->f;
    CHECK_CUDA_RETURN(cu_f, cuMemsetD8Async(cambi_dptr(s, CAMBI_BUF_SELECT), 0,
                                            cambi_buffer_bytes(s, CAMBI_BUF_SELECT), stream));
    CHECK_CUDA_RETURN(cu_f, cuMemsetD8Async(cambi_dptr(s, CAMBI_BUF_RESULTS), 0,
                                            cambi_buffer_bytes(s, CAMBI_BUF_RESULTS), stream));
    return 0;
}

/* validate (bpc other than 8 and 16), preprocess, spatial mask at scale 0. */
static int cambi_enqueue_preprocess(const CambiStateCuda *s, CUstream stream, VmafPicture *dist)
{
    CambiCudaPreprocArgs pre = {
        .src = (uint64_t)(uintptr_t)dist->data[0],
        .dst = cambi_dptr(s, CAMBI_BUF_IMAGE),
        .ori_x = cambi_dptr(s, CAMBI_BUF_ORI_X),
        .ori_y = cambi_dptr(s, CAMBI_BUF_ORI_Y),
        .results = cambi_dptr(s, CAMBI_BUF_RESULTS),
        .src_pitch = (uint32_t)dist->stride[0],
        .src_width = s->src_width,
        .src_height = s->src_height,
        .out_width = s->proc_width,
        .out_height = s->proc_height,
        .bpc = s->src_bpc,
        .same_size = cambi_resizes(s) ? 0u : 1u,
        .anti_dither = s->enc_bitdepth < 10 ? 1u : 0u,
    };
    int err = 0;
    if (s->src_bpc != 8u && s->src_bpc != 16u) {
        err = cambi_launch(s, CAMBI_FN_VALIDATE, cambi_image_launch(s->src_width, s->src_height),
                           stream, &pre);
    }
    if (!err) {
        err = cambi_launch(s, CAMBI_FN_PREPROCESS,
                           cambi_image_launch(s->proc_width, s->proc_height), stream, &pre);
    }
    CambiCudaMaskArgs mask = {
        .image = cambi_dptr(s, CAMBI_BUF_IMAGE),
        .mask = cambi_dptr(s, CAMBI_BUF_MASK),
        .width = s->proc_width,
        .height = s->proc_height,
        .mask_index = s->mask_index,
    };
    if (!err) {
        err = cambi_launch(s, CAMBI_FN_MASK, cambi_image_launch(s->proc_width, s->proc_height),
                           stream, &mask);
    }
    return err;
}

/* The three rotating image buffers of cambi_score's per-scale walk. */
typedef struct CambiScaleBuffers {
    CUdeviceptr image;
    CUdeviceptr mask;
    CUdeviceptr scratch;
    unsigned width;
} CambiScaleBuffers;

static int cambi_enqueue_decimate(const CambiStateCuda *s, CUstream stream, CambiScaleBuffers *b,
                                  const CambiCudaScaleGeom *g)
{
    CambiCudaDecimateArgs dec = {
        .src = b->image,
        .dst = b->scratch,
        .out_width = g->width,
        .out_height = g->height,
        .src_stride = b->width,
    };
    const CambiLaunch l = cambi_image_launch(g->width, g->height);
    int err = cambi_launch(s, CAMBI_FN_DECIMATE, l, stream, &dec);
    CUdeviceptr swap = b->image;
    b->image = b->scratch;
    b->scratch = swap;
    dec.src = b->mask;
    dec.dst = b->scratch;
    if (!err)
        err = cambi_launch(s, CAMBI_FN_DECIMATE, l, stream, &dec);
    swap = b->mask;
    b->mask = b->scratch;
    b->scratch = swap;
    b->width = g->width;
    return err;
}

/* decimate (scale > 0 or high-res speed-up), then filter_mode H and V; the
 * vertical pass also writes the level map. */
static int cambi_enqueue_scale_image(const CambiStateCuda *s, CUstream stream, CambiScaleBuffers *b,
                                     int scale)
{
    const CambiCudaScaleGeom *g = &s->geom[scale];
    int err = 0;
    if (scale > 0 || s->cambi_high_res_speedup)
        err = cambi_enqueue_decimate(s, stream, b, g);
    CambiCudaFilterArgs filter = {
        .image = b->image,
        .filtered_h = b->scratch,
        .mask = b->mask,
        .q = cambi_dptr(s, CAMBI_BUF_Q),
        .width = g->width,
        .height = g->height,
        .v_band_base = s->v_band_base,
        .v_band_size = s->v_band_size,
    };
    const CambiLaunch l = cambi_image_launch(g->width, g->height);
    if (!err)
        err = cambi_launch(s, CAMBI_FN_FILTER_H, l, stream, &filter);
    if (!err)
        err = cambi_launch(s, CAMBI_FN_FILTER_V, l, stream, &filter);
    return err;
}

static CambiCudaCvalsArgs cambi_cvals_args(const CambiStateCuda *s, int scale)
{
    const CambiCudaScaleGeom *g = &s->geom[scale];
    const CambiCudaCvalsArgs a = {
        .q = cambi_dptr(s, CAMBI_BUF_Q),
        .runs = cambi_dptr(s, CAMBI_BUF_RUNS),
        .change = cambi_dptr(s, CAMBI_BUF_CHANGE),
        .hist = cambi_dptr(s, CAMBI_BUF_HIST),
        .cvals = cambi_dptr(s, CAMBI_BUF_CVALS),
        .select = cambi_dptr(s, CAMBI_BUF_SELECT) + (CUdeviceptr)(scale * sizeof(CambiCudaSelect)),
        .partials = cambi_dptr(s, CAMBI_BUF_PARTIALS),
        .lut = cambi_dptr(s, CAMBI_BUF_LUT),
        .tvi = cambi_dptr(s, CAMBI_BUF_TVI),
        .weights = cambi_dptr(s, CAMBI_BUF_WEIGHTS),
        .width = g->width,
        .height = g->height,
        .words = (g->width + 31u) / 32u,
        .pad = (unsigned)s->adjusted_window >> 1,
        .chunk_rows = g->chunk_rows,
        .levels = s->levels,
        .num_diffs = s->num_diffs,
        .vlt_luma = s->vlt_luma,
        .v_band_base = s->v_band_base,
    };
    return a;
}

/* Scan 0 (pass 0 was counted by the c-values kernel), passes 1 and 2 (the
 * device skips them once resolved), the partial sums and the final sum. */
static int cambi_enqueue_pooling(const CambiStateCuda *s, CUstream stream, int scale)
{
    const CambiCudaScaleGeom *g = &s->geom[scale];
    CambiCudaPoolArgs pool = {
        .cvals = cambi_dptr(s, CAMBI_BUF_CVALS),
        .select = cambi_dptr(s, CAMBI_BUF_SELECT) + (CUdeviceptr)(scale * sizeof(CambiCudaSelect)),
        .partials = cambi_dptr(s, CAMBI_BUF_PARTIALS),
        .results = cambi_dptr(s, CAMBI_BUF_RESULTS),
        .n = g->width * g->height,
        .groups = g->pool_groups,
        .cvals_groups = g->cvals_groups,
        .topk = g->topk,
        .pass = 0,
        .scale = scale,
    };
    const CambiLaunch one = cambi_linear_launch(1u, CAMBI_CUDA_POOL_BLOCK);
    const CambiLaunch all = cambi_linear_launch(g->pool_groups, CAMBI_CUDA_POOL_BLOCK);
    int err = cambi_launch(s, CAMBI_FN_RADIX_SCAN, one, stream, &pool);
    for (int pass = 1; pass < CAMBI_CUDA_RADIX_PASSES && !err; pass++) {
        pool.pass = pass;
        err = cambi_launch(s, CAMBI_FN_RADIX_HIST, all, stream, &pool);
        if (!err)
            err = cambi_launch(s, CAMBI_FN_RADIX_SCAN, one, stream, &pool);
    }
    if (!err)
        err = cambi_launch(s, CAMBI_FN_TOPK_PARTIALS, all, stream, &pool);
    if (!err)
        err = cambi_launch(s, CAMBI_FN_TOPK_FINAL, one, stream, &pool);
    return err;
}

/* Row masks, c-values (+ radix pass 0) and the top-K pooling of one scale. */
static int cambi_enqueue_scale_score(const CambiStateCuda *s, CUstream stream, int scale)
{
    const CambiCudaScaleGeom *g = &s->geom[scale];
    CambiCudaCvalsArgs cvals = cambi_cvals_args(s, scale);
    const CambiLaunch masks = {
        .grid_x = cvals.words,
        .grid_y = (g->height + CAMBI_CUDA_ROWMASK_ROWS - 1u) / CAMBI_CUDA_ROWMASK_ROWS,
        .block_x = 32u,
        .block_y = CAMBI_CUDA_ROWMASK_ROWS,
    };
    int err = cambi_launch(s, CAMBI_FN_ROW_MASKS, masks, stream, &cvals);
    const CambiLaunch columns = {
        .grid_x = (g->width + CAMBI_CUDA_CVALS_BLOCK - 1u) / CAMBI_CUDA_CVALS_BLOCK,
        .grid_y = g->chunks,
        .block_x = CAMBI_CUDA_CVALS_BLOCK,
        .block_y = 1u,
    };
    if (!err)
        err = cambi_launch(s, CAMBI_FN_CVALS, columns, stream, &cvals);
    if (!err)
        err = cambi_enqueue_pooling(s, stream, scale);
    return err;
}

/* The whole frame on the distorted picture's stream, then the one readback
 * on the private stream behind an event. Never waits. */
static int cambi_enqueue_frame(CambiStateCuda *s, VmafPicture *dist)
{
    CudaFunctions *cu_f = s->cu_state->f;
    CUstream stream = vmaf_cuda_picture_get_stream(dist);
    int err = cambi_enqueue_reset(s, stream);
    if (!err)
        err = cambi_enqueue_preprocess(s, stream, dist);
    CambiScaleBuffers b = {
        .image = cambi_dptr(s, CAMBI_BUF_IMAGE),
        .mask = cambi_dptr(s, CAMBI_BUF_MASK),
        .scratch = cambi_dptr(s, CAMBI_BUF_TMP),
        .width = s->proc_width,
    };
    for (int scale = 0; scale < CAMBI_CUDA_NUM_SCALES && !err; scale++) {
        err = cambi_enqueue_scale_image(s, stream, &b, scale);
        if (!err)
            err = cambi_enqueue_scale_score(s, stream, scale);
    }
    if (err)
        return err;
    CHECK_CUDA_RETURN(cu_f, cuEventRecord(s->lc.submit, stream));
    CHECK_CUDA_RETURN(cu_f, cuStreamWaitEvent(s->lc.str, s->lc.submit, CU_EVENT_WAIT_DEFAULT));
    CHECK_CUDA_RETURN(cu_f, cuMemcpyDtoHAsync(s->h_results, cambi_dptr(s, CAMBI_BUF_RESULTS),
                                              sizeof(CambiCudaResults), s->lc.str));
    return vmaf_cuda_kernel_submit_post_record(&s->lc, s->cu_state);
}

static int submit_fex_cuda(VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                           VmafPicture *dist_pic, VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic;
    (void)ref_pic_90;
    (void)dist_pic_90;
    (void)index;
    CambiStateCuda *s = fex->priv;
    CudaFunctions *cu_f = fex->cu_state->f;
    CHECK_CUDA_RETURN(cu_f, cuCtxPushCurrent(fex->cu_state->ctx));
    const int err = cambi_enqueue_frame(s, dist_pic);
    const CUresult pop = cu_f->cuCtxPopCurrent(NULL);
    return err ? err : vmaf_cuda_result_to_errno((int)pop);
}

/* ------------------------------------------------------------------ */
/* collect: the frame's one wait, then host arithmetic on 88 bytes     */
/* ------------------------------------------------------------------ */

static int collect_fex_cuda(VmafFeatureExtractor *fex, unsigned index,
                            VmafFeatureCollector *feature_collector)
{
    CambiStateCuda *s = fex->priv;
    const int err = vmaf_cuda_kernel_collect_wait(&s->lc, fex->cu_state);
    if (err)
        return err;
    const CambiCudaResults *r = s->h_results;
    if (r->status & CAMBI_CUDA_STATUS_INVALID_INPUT) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "cambi_cuda: frame %u holds samples above the %u-bit maximum\n", index,
                 s->src_bpc);
        return -EINVAL;
    }
    double scores[CAMBI_CUDA_NUM_SCALES];
    for (int scale = 0; scale < CAMBI_CUDA_NUM_SCALES; scale++) {
        scores[scale] =
            vmaf_cambi_fixed_topk_mean(r->sum_hi[scale], r->sum_lo[scale], s->geom[scale].topk);
    }
    const uint16_t pixels = vmaf_cambi_get_pixels_in_window(s->adjusted_window);
    double score = vmaf_cambi_weight_scores_per_scale(scores, pixels);
    /* cambi.c emits MIN(score, cambi_max_val) and nothing else. */
    if (score > s->cambi_max_val)
        score = s->cambi_max_val;
    return vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "Cambi_feature_cambi_score", score, index);
}

static int close_fex_cuda(VmafFeatureExtractor *fex)
{
    return cambi_release(fex->priv, 0);
}

static const char *provided_features[] = {"Cambi_feature_cambi_score", NULL};

VmafFeatureExtractor vmaf_fex_cambi_cuda = {
    .name = "cambi_cuda",
    .init = init_fex_cuda,
    .submit = submit_fex_cuda,
    .collect = collect_fex_cuda,
    .close = close_fex_cuda,
    .options = options,
    .priv_size = sizeof(CambiStateCuda),
    .provided_features = provided_features,
    .flags = VMAF_FEATURE_EXTRACTOR_CUDA,
    /* Device-resident (ADR-1379): about 68 launches per frame — validate,
     * preprocess, mask, and per scale two decimates, two filter passes, row
     * masks, c-values and seven top-K kernels — and one 88-byte readback. No
     * host stage remains, so frames pipeline like any submit/collect twin. */
    .chars =
        {
            .n_dispatches_per_frame = 68,
            .is_reduction_only = false,
            .min_useful_frame_area = 1920U * 1080U,
            .dispatch_hint = VMAF_FEATURE_DISPATCH_AUTO,
        },
};

/* NOLINTEND(modernize-use-nullptr) */
