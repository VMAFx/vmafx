/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * ADR-1378 — device-free replay of the HIP CAMBI kernels against cambi.c.
 *
 * integer_cambi/cambi_hip_device.h holds the arithmetic every cambi_score.hip
 * work-item runs. This test compiles the same header for the host and replays
 * each kernel of a frame work-item by work-item, in launch order, with the
 * kernels' own decomposition: the spatial mask tile by tile in its four
 * shared-memory phases, the run / change words 32 columns at a time, the
 * c-values with every (chunk, column) work-item in flight at once (begun
 * together and advanced row by row in lockstep, a canary behind the last
 * histogram), the pass-0 tally and the per-work-group sums, and the radix
 * select lane by lane with the same exclusive scan. The reference is cambi.c's own pipeline through the helpers
 * of cambi_internal.h (preprocessing, spatial mask, decimate, filter_mode,
 * calculate_c_values, spatial_pooling, weight_scores_per_scale) and, for the
 * default options, the registered CPU extractor. The parameter block is the
 * extractor's own: cambi_hip_plan() and cambi_hip_plan_bind_scales()
 * (integer_cambi_hip.h), with cambi.c's window, mask-index, resize-table and
 * contrast-weight helpers, so a planning change cannot pass here and fail on
 * the device.
 *
 * Also asserted: cambi_hip's init() accepts and rejects exactly the windows
 * cambi.c's reciprocal-table guard does, before any device work.
 *
 * Asserted per scale: the c-value planes are identical bit for bit and the
 * exact top-K sum gives the CPU's per-scale score bit for bit; per frame, the
 * weighted score equals the CPU's. The cases cover 8-, 9-, 10- and 12-bit
 * input, anti-dither on and off, resize to a smaller encode, the high-res
 * speed-up, several windows and contrast depths, and two row-chunk layouts of
 * the same frame (the result must not depend on them). A host replay cannot
 * see device scheduling; test_hip_cambi_parity covers that on an AMD device.
 */

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"

#include "feature/cambi_internal.h"
#include "feature/feature_extractor.h"
#include "feature/hip/integer_cambi_hip.h"
#include "libvmaf/feature.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/picture.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. ADR-1138. */

#define EMU_MAX_DIFFS 32u
#define EMU_TOPK 0.6
#define EMU_HIST_CANARY 4096u

typedef struct EmuCase {
    const char *name;
    unsigned w;
    unsigned h;
    unsigned bpc;
    unsigned enc_w; /* 0: the input size */
    unsigned enc_h;
    unsigned enc_bpc; /* 0: the input depth */
    int window_size;
    int max_log_contrast;
    int speedup; /* 1: cambi_high_res_speedup in effect */
    unsigned compute_units;
    unsigned salt;
    int clean; /* 1: one-level bands, rare dither (bands through the mask at any size);
                * 2: 8x8 flat blocks ramping through the whole sample range */
} EmuCase;

/* The resolved configuration, derived with cambi.c's own helpers. */
typedef struct EmuConfig {
    unsigned proc_w;
    unsigned proc_h;
    unsigned enc_bpc;
    unsigned num_diffs;
    uint16_t window;
    uint16_t vlt_luma;
    uint16_t v_band_base;
    uint16_t v_band_size;
    uint16_t tvi[EMU_MAX_DIFFS];
    int32_t weights[EMU_MAX_DIFFS];
    int cpu_weights[EMU_MAX_DIFFS];
    int all_diffs[2u * EMU_MAX_DIFFS + 1u];
    uint16_t diffs[EMU_MAX_DIFFS];
} EmuConfig;

static uint32_t emu_lcg(uint32_t x)
{
    return x * 1664525u + 1013904223u;
}

/* A banded ramp with a vertical component, a deterministic dither and an
 * inverted border ring (the stages a flat fixture cannot observe). */
static void emu_put(VmafPicture *pic, const EmuCase *c, unsigned row, unsigned col, unsigned v)
{
    if (c->bpc <= 8u)
        ((uint8_t *)pic->data[0])[row * pic->stride[0] + col] = (uint8_t)v;
    else
        ((uint16_t *)pic->data[0])[row * (pic->stride[0] / 2u) + col] = (uint16_t)v;
}

static void emu_fill_sample(VmafPicture *pic, const EmuCase *c, unsigned row, unsigned col)
{
    const unsigned max_val = (1u << c->bpc) - 1u;
    if (c->clean == 2) {
        /* One level per 8 columns, 80 levels per 8 rows, wrapping: flat
         * blocks at every sample value, so both edges of cambi's level band
         * and of the TVI table are crossed inside the mask. The bottom 16 rows
         * step one level per 8 columns from mid-range, so the first level
         * above the default band (564 at 10 bits) sits in the last row
         * chunk's final windows, right in front of the histogram canary. */
        const unsigned ramp = row + 16u >= c->h ? max_val / 2u + 8u + col / 8u :
                                                  (col / 8u) + (row / 8u) * 80u + c->salt;
        emu_put(pic, c, row, col, ramp % (max_val + 1u));
        return;
    }
    const unsigned step = c->bpc > 10u ? 1u << (c->bpc - 10u) : 1u;
    const uint32_t hash = emu_lcg(row * 8191u + col * 131u + c->salt);
    /* Above 10 bits, random low bits make the rounding shift of the 10-bit
     * conversion round some samples up and others down. */
    const unsigned col_step = c->clean ? 1u : 3u;
    const unsigned row_step = c->clean ? 1u : 2u;
    unsigned v = (col / 24u) * step * col_step + (row / 40u) * step * row_step + c->salt * step +
                 (hash >> 8) % step;
    /* The default fixture dithers 3 samples in 8; the clean one 1 in 256, so
     * the zero-derivative mask survives larger frames (a higher mask index)
     * and no anti-dither pass. */
    if (c->clean ? (hash >> 24) == 0u : (hash >> 29) < 3u)
        v += step;
    if (row == 0u || row + 1u == c->h || col == 0u || col + 1u == c->w)
        v += 17u * step;
    /* Clean bands sit darker, where cambi's TVI keeps one-level contrasts. */
    v = (v + max_val / (c->clean ? 12u : 5u)) % (max_val + 1u);
    emu_put(pic, c, row, col, v);
}

static int emu_fill(VmafPicture *pic, const EmuCase *c)
{
    const int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, c->bpc, c->w, c->h);
    if (err)
        return err;
    for (unsigned row = 0u; row < c->h; row++) {
        for (unsigned col = 0u; col < c->w; col++)
            emu_fill_sample(pic, c, row, col);
    }
    return 0;
}

static int emu_configure(const EmuCase *c, EmuConfig *cfg)
{
    cfg->proc_w = c->enc_w ? c->enc_w : c->w;
    cfg->proc_h = c->enc_h ? c->enc_h : c->h;
    cfg->enc_bpc = c->enc_bpc ? c->enc_bpc : c->bpc;
    cfg->num_diffs = 1u << (unsigned)c->max_log_contrast;
    cfg->window =
        vmaf_cambi_adjust_window(c->window_size, cfg->proc_w, cfg->proc_h, c->speedup != 0);
    const int *weights = vmaf_cambi_contrast_weights(NULL);
    for (unsigned d = 0u; d < cfg->num_diffs; d++) {
        cfg->diffs[d] = (uint16_t)(d + 1u);
        cfg->weights[d] = weights[d];
        cfg->cpu_weights[d] = weights[d];
    }
    for (int d = -(int)cfg->num_diffs; d <= (int)cfg->num_diffs; d++)
        cfg->all_diffs[d + (int)cfg->num_diffs] = d;
    return vmaf_cambi_init_tvi_and_vlt((int)cfg->num_diffs, cfg->diffs, 0.019, 0.0, "bt1886",
                                       "bt1886", cfg->tvi, &cfg->vlt_luma, &cfg->v_band_base,
                                       &cfg->v_band_size);
}

/* ------------------------------------------------------------------ */
/* The CPU reference: cambi.c::cambi_score() through cambi_internal.h. */
/* ------------------------------------------------------------------ */

typedef struct CpuRun {
    VmafPicture pics[2];
    float *cvals[CAMBI_HIP_NUM_SCALES];
    double scores[CAMBI_HIP_NUM_SCALES];
    double score;
} CpuRun;

static int cpu_prepare(const VmafPicture *dist, const EmuConfig *cfg, CpuRun *run)
{
    int err = vmaf_picture_alloc(&run->pics[0], VMAF_PIX_FMT_YUV400P, 10, cfg->proc_w, cfg->proc_h);
    if (!err)
        err = vmaf_picture_alloc(&run->pics[1], VMAF_PIX_FMT_YUV400P, 10, cfg->proc_w, cfg->proc_h);
    if (!err)
        err = vmaf_cambi_preprocessing(dist, &run->pics[0], (int)cfg->proc_w, (int)cfg->proc_h,
                                       (int)cfg->enc_bpc);
    if (err)
        return err;
    const unsigned dp_w = cfg->proc_w + 2u * 3u + 1u;
    uint32_t *dp = calloc((size_t)dp_w * (2u * 3u + 2u), sizeof(uint32_t));
    uint16_t *deriv = calloc(cfg->proc_w, sizeof(uint16_t));
    VmafCambiDerivativeCalculator derivative = NULL;
    vmaf_cambi_default_callbacks(NULL, NULL, &derivative);
    if (dp && deriv)
        vmaf_cambi_get_spatial_mask(&run->pics[0], &run->pics[1], dp, deriv, cfg->proc_w,
                                    cfg->proc_h, derivative);
    err = (dp && deriv) ? 0 : -ENOMEM;
    free(dp);
    free(deriv);
    return err;
}

/* One scale of cambi_score() on the pictures, keeping a copy of the c-values
 * before the quick-select pooling reorders them. */
static int cpu_scale(CpuRun *run, const EmuConfig *cfg, unsigned scale, unsigned w, unsigned h)
{
    uint16_t *filter = calloc(3u * (size_t)w, sizeof(uint16_t));
    uint16_t *hist = calloc((size_t)w * cfg->v_band_size, sizeof(uint16_t));
    float *pooled = calloc((size_t)w * h, sizeof(float));
    run->cvals[scale] = calloc((size_t)w * h, sizeof(float));
    VmafCambiRangeUpdater inc = NULL;
    VmafCambiRangeUpdater dec = NULL;
    vmaf_cambi_default_callbacks(&inc, &dec, NULL);
    const int ok = filter && hist && pooled && run->cvals[scale];
    if (ok) {
        vmaf_cambi_filter_mode(&run->pics[0], (int)w, (int)h, filter);
        vmaf_cambi_calculate_c_values(&run->pics[0], &run->pics[1], pooled, hist, cfg->window,
                                      (uint16_t)cfg->num_diffs, cfg->tvi, cfg->vlt_luma,
                                      cfg->cpu_weights, cfg->all_diffs, (int)w, (int)h, inc, dec);
        memcpy(run->cvals[scale], pooled, sizeof(float) * (size_t)w * h);
        run->scores[scale] = vmaf_cambi_spatial_pooling(pooled, EMU_TOPK, w, h);
    }
    free(filter);
    free(hist);
    free(pooled);
    return ok ? 0 : -ENOMEM;
}

static int cpu_run(const VmafPicture *dist, const EmuCase *c, const EmuConfig *cfg, CpuRun *run)
{
    int err = cpu_prepare(dist, cfg, run);
    unsigned w = cfg->proc_w;
    unsigned h = cfg->proc_h;
    for (unsigned scale = 0u; scale < CAMBI_HIP_NUM_SCALES && !err; scale++) {
        if (scale > 0u || c->speedup) {
            w = (w + 1u) >> 1;
            h = (h + 1u) >> 1;
            vmaf_cambi_decimate(&run->pics[0], w, h);
            vmaf_cambi_decimate(&run->pics[1], w, h);
        }
        err = cpu_scale(run, cfg, scale, w, h);
    }
    if (!err)
        run->score = vmaf_cambi_weight_scores_per_scale(
            run->scores, vmaf_cambi_get_pixels_in_window(cfg->window));
    return err;
}

static void cpu_free(CpuRun *run)
{
    for (unsigned scale = 0u; scale < CAMBI_HIP_NUM_SCALES; scale++)
        free(run->cvals[scale]);
    if (run->pics[0].ref)
        (void)vmaf_picture_unref(&run->pics[0]);
    if (run->pics[1].ref)
        (void)vmaf_picture_unref(&run->pics[1]);
}

/* ------------------------------------------------------------------ */
/* The device replay.                                                  */
/* ------------------------------------------------------------------ */

typedef struct Emu {
    CambiHipParams p;
    CambiHipFrameState frame;
    unsigned char *src;
    uint16_t *preproc;
    uint16_t *alt;
    uint16_t *filtered_h;
    uint16_t *mask;
    uint16_t *q;
    uint16_t *hist;
    size_t hist_cells;
    uint32_t *runs;
    uint32_t *change;
    uint32_t *ori_x;
    uint32_t *ori_y;
    float *cvals;
} Emu;

/* The extractor's plan for this case: what init() would upload. */
static void emu_plan(Emu *e, const EmuCase *c, const EmuConfig *cfg)
{
    const CambiHipPlanInput in = {
        .src_width = c->w,
        .src_height = c->h,
        .src_bpc = c->bpc,
        .proc_width = cfg->proc_w,
        .proc_height = cfg->proc_h,
        .enc_bpc = cfg->enc_bpc,
        .speedup = c->speedup ? 1u : 0u,
        .adjusted_window = cfg->window,
        .num_diffs = cfg->num_diffs,
        .levels = cfg->v_band_size,
        .vlt_luma = cfg->vlt_luma,
        .v_band_base = cfg->v_band_base,
        .topk = EMU_TOPK,
        .compute_units = c->compute_units,
    };
    cambi_hip_plan(&e->p, &in);
    unsigned lut_size = 0u;
    e->p.lut = vmaf_cambi_reciprocal_lut(&lut_size);
    e->p.tvi = cfg->tvi;
    e->p.weights = cfg->weights;
    e->p.frame = &e->frame;
}

/* Stage the distorted luma plane packed, as vmaf_hip_picture_upload_staged()
 * lands it on the device. */
static void emu_stage(Emu *e, const VmafPicture *dist, const EmuCase *c)
{
    const size_t row_bytes = (size_t)c->w * (c->bpc <= 8u ? 1u : 2u);
    const unsigned char *plane = dist->data[0];
    for (unsigned row = 0u; row < c->h; row++)
        memcpy(e->src + row * row_bytes, plane + (ptrdiff_t)row * dist->stride[0], row_bytes);
    e->p.src = e->src;
}

static int emu_alloc(Emu *e, const EmuCase *c, const EmuConfig *cfg)
{
    const size_t px = (size_t)cfg->proc_w * cfg->proc_h;
    const size_t words = (size_t)cfg->proc_h * ((cfg->proc_w + 31u) / 32u);
    e->src = calloc((size_t)c->w * c->h, 2u);
    e->preproc = calloc(px, sizeof(uint16_t));
    /* The device's sizes: the alternate image holds scale 1 (or the
     * speed-up's scale 0) at half resolution, rounded up. */
    const size_t half = (size_t)((cfg->proc_w + 1u) >> 1) * ((cfg->proc_h + 1u) >> 1);
    e->alt = calloc(half, sizeof(uint16_t));
    e->filtered_h = calloc(px, sizeof(uint16_t));
    e->mask = calloc(px, sizeof(uint16_t));
    e->q = calloc(px, sizeof(uint16_t));
    e->runs = calloc(words, sizeof(uint32_t));
    e->change = calloc(words, sizeof(uint32_t));
    e->cvals = calloc(px, sizeof(float));
    e->ori_x = calloc(cfg->proc_w, sizeof(uint32_t));
    e->ori_y = calloc(cfg->proc_h, sizeof(uint32_t));
    emu_plan(e, c, cfg);
    cambi_hip_plan_bind_scales(&e->p, e->preproc, e->alt, e->filtered_h, c->speedup ? 1u : 0u);
    /* The device's histogram size plus a canary tail: a work-item writing
     * past the last chunk's histograms is caught (emu_hist_intact()). */
    e->hist_cells = cambi_hip_plan_hist_cells(&e->p);
    e->hist = malloc((e->hist_cells + EMU_HIST_CANARY) * sizeof(uint16_t));
    if (e->hist) {
        memset(e->hist, 0, e->hist_cells * sizeof(uint16_t));
        for (size_t i = 0u; i < EMU_HIST_CANARY; i++)
            e->hist[e->hist_cells + i] = 0xA5A5u;
    }
    const int ok = e->src && e->preproc && e->alt && e->filtered_h && e->mask && e->q && e->runs &&
                   e->change && e->cvals && e->ori_x && e->ori_y && e->hist;
    return ok ? 0 : -ENOMEM;
}

static int emu_setup(Emu *e, const VmafPicture *dist, const EmuCase *c, const EmuConfig *cfg)
{
    const int err = emu_alloc(e, c, cfg);
    if (err)
        return err;
    vmaf_cambi_resize_source_indices(c->w, cfg->proc_w, e->ori_x);
    vmaf_cambi_resize_source_indices(c->h, cfg->proc_h, e->ori_y);
    CambiHipParams *p = &e->p;
    p->preproc = e->preproc;
    p->mask = e->mask;
    p->q = e->q;
    p->runs = e->runs;
    p->change = e->change;
    p->cvals = e->cvals;
    p->hist = e->hist;
    p->ori_x = p->same_size ? NULL : e->ori_x;
    p->ori_y = p->same_size ? NULL : e->ori_y;
    emu_stage(e, dist, c);
    return 0;
}

static void emu_free(Emu *e)
{
    free(e->src);
    free(e->preproc);
    free(e->alt);
    free(e->filtered_h);
    free(e->mask);
    free(e->q);
    free(e->runs);
    free(e->change);
    free(e->cvals);
    free(e->ori_x);
    free(e->ori_y);
    free(e->hist);
}

/* cambi_hip_validate + cambi_hip_preprocess. */
static void emu_preprocess(Emu *e)
{
    const CambiHipParams *p = &e->p;
    for (unsigned y = 0u; y < p->src_height && p->validate; y++) {
        for (unsigned x = 0u; x < p->src_width; x++) {
            if (cambi_hd_sample_invalid(p, x, y))
                e->frame.results.status |= CAMBI_HIP_STATUS_INVALID_INPUT;
        }
    }
    for (unsigned y = 0u; y < p->proc_height; y++) {
        for (unsigned x = 0u; x < p->proc_width; x++)
            p->preproc[y * p->proc_width + x] = cambi_hd_preproc_pixel(p, y, x);
    }
}

/* cambi_hip_spatial_mask on one tile, phase by phase. */
static void emu_mask_tile(const CambiHipParams *p, unsigned ty, unsigned tx)
{
    uint32_t pixels[CAMBI_HIP_MASK_PIXELS];
    uint8_t flags[CAMBI_HIP_MASK_FLAGS];
    uint8_t row_sums[CAMBI_HIP_MASK_ROW_SUMS];
    const int y0 = (int)(ty * CAMBI_HIP_TILE) - (int)CAMBI_HIP_MASK_HALO;
    const int x0 = (int)(tx * CAMBI_HIP_TILE) - (int)CAMBI_HIP_MASK_HALO;
    for (uint32_t i = 0u; i < CAMBI_HIP_MASK_PIXELS; i++)
        pixels[i] = cambi_hd_mask_tile_pixel(p, y0, x0, i);
    for (uint32_t i = 0u; i < CAMBI_HIP_MASK_FLAGS; i++)
        flags[i] = cambi_hd_mask_tile_flag(pixels, i);
    for (uint32_t i = 0u; i < CAMBI_HIP_MASK_ROW_SUMS; i++)
        row_sums[i] = cambi_hd_mask_tile_row_sum(flags, i);
    for (uint32_t ly = 0u; ly < CAMBI_HIP_TILE; ly++) {
        for (uint32_t lx = 0u; lx < CAMBI_HIP_TILE; lx++) {
            const uint32_t y = ty * CAMBI_HIP_TILE + ly;
            const uint32_t x = tx * CAMBI_HIP_TILE + lx;
            if (x < p->proc_width && y < p->proc_height)
                p->mask[y * p->proc_width + x] =
                    cambi_hd_mask_tile_out(row_sums, ly, lx, p->mask_index);
        }
    }
}

static void emu_mask(const CambiHipParams *p)
{
    const unsigned tiles_x = (p->proc_width + CAMBI_HIP_TILE - 1u) / CAMBI_HIP_TILE;
    const unsigned tiles_y = (p->proc_height + CAMBI_HIP_TILE - 1u) / CAMBI_HIP_TILE;
    for (unsigned ty = 0u; ty < tiles_y; ty++) {
        for (unsigned tx = 0u; tx < tiles_x; tx++)
            emu_mask_tile(p, ty, tx);
    }
}

/* cambi_hip_decimate, _filter_h, _filter_v: one kernel after the other. */
static void emu_scale_image(const CambiHipParams *p, unsigned scale)
{
    const CambiHipScale *sc = &p->scale[scale];
    for (uint32_t y = 0u; y < sc->height && sc->decimate; y++) {
        for (uint32_t x = 0u; x < sc->width; x++)
            cambi_hd_decimate_pixel(sc, x, y);
    }
    for (uint32_t y = 0u; y < sc->height; y++) {
        for (uint32_t x = 0u; x < sc->width; x++)
            cambi_hd_filter_h_pixel(sc, x, y);
    }
    for (uint32_t y = 0u; y < sc->height; y++) {
        for (uint32_t x = 0u; x < sc->width; x++)
            cambi_hd_filter_v_pixel(p, sc, x, y);
    }
}

/* cambi_hip_row_masks: every word ORs the bits of its 32 columns. */
static void emu_row_word(const CambiHipParams *p, const CambiHipScale *sc, uint32_t y, uint32_t w)
{
    uint32_t run_word = 0u;
    uint32_t change_word = 0u;
    for (uint32_t b = 0u; b < 32u && w * 32u + b < sc->width; b++) {
        uint32_t run = 0u;
        uint32_t change = 0u;
        cambi_hd_row_mask_bits(p->q, sc->width, sc->height, p->pad, w * 32u + b, y, &run, &change);
        run_word |= run << b;
        change_word |= change << b;
    }
    p->runs[y * sc->words + w] = run_word;
    p->change[y * sc->words + w] = change_word;
}

static void emu_row_masks(const CambiHipParams *p, unsigned scale)
{
    const CambiHipScale *sc = &p->scale[scale];
    for (uint32_t y = 0u; y < sc->height; y++) {
        for (uint32_t w = 0u; w < sc->words; w++)
            emu_row_word(p, sc, y, w);
    }
}

/* cambi_hip_cvals: every (chunk, column) work-item in flight at once, the way
 * the device runs them: all columns of all chunks are begun, then advanced
 * one row at a time in lockstep, so a work-item that wrote outside its own
 * histogram column would corrupt a neighbour still in use. Then one group
 * sum per block. */
static int emu_cvals(const CambiHipParams *p, unsigned scale)
{
    const CambiHipScale *sc = &p->scale[scale];
    const CambiHdCvals a = cambi_hd_cvals_args(p, (int)scale);
    CambiHipSelect *sel = &p->frame->select[scale];
    const unsigned blocks = (sc->width + CAMBI_HIP_CVALS_BLOCK - 1u) / CAMBI_HIP_CVALS_BLOCK;
    const size_t items = (size_t)sc->chunks * blocks * CAMBI_HIP_CVALS_BLOCK;
    CambiHdColumn *cols = calloc(items, sizeof(*cols));
    CambiHdTally *tallies = calloc(items, sizeof(*tallies));
    unsigned char *live = calloc(items, 1u);
    if (!cols || !tallies || !live) {
        free(cols);
        free(tallies);
        free(live);
        return -ENOMEM;
    }
    for (size_t i = 0u; i < items; i++) {
        const uint32_t chunk = (uint32_t)(i / ((size_t)blocks * CAMBI_HIP_CVALS_BLOCK));
        const uint32_t col = (uint32_t)(i % ((size_t)blocks * CAMBI_HIP_CVALS_BLOCK));
        live[i] = (unsigned char)cambi_hd_cvals_begin(&a, chunk, col, &cols[i]);
    }
    for (uint32_t r = 0u; r < sc->chunk_rows; r++) {
        for (size_t i = 0u; i < items; i++) {
            if (live[i] && cols[i].y0 + r < cols[i].y1)
                cambi_hd_cvals_row(&a, &cols[i], cols[i].y0 + r, &tallies[i]);
        }
    }
    for (size_t group = 0u; group < (size_t)sc->chunks * blocks; group++) {
        uint64_t sum = 0u;
        for (uint32_t lane = 0u; lane < CAMBI_HIP_CVALS_BLOCK; lane++) {
            CambiHdTally *tally = &tallies[group * CAMBI_HIP_CVALS_BLOCK + lane];
            cambi_hd_tally_flush(a.radix_hist, tally);
            sum += tally->sum;
        }
        cambi_hd_add_split(&sel->all_lo, &sel->all_hi, sum);
    }
    free(cols);
    free(tallies);
    free(live);
    return 0;
}

/* cambi_hip_radix_scan: all lanes count, one exclusive scan, the owning lane
 * picks, then every lane clears its bins. */
static void emu_radix_scan(const CambiHipParams *p, unsigned scale, int pass)
{
    CambiHipSelect *sel = &p->frame->select[scale];
    if (sel->resolved != 0u)
        return;
    const uint32_t k = cambi_hd_pass_rank(&p->scale[scale], sel, pass);
    uint32_t mine[CAMBI_HIP_POOL_BLOCK];
    for (uint32_t lane = 0u; lane < CAMBI_HIP_POOL_BLOCK; lane++) {
        const uint32_t top = CAMBI_HIP_RADIX_BINS - 1u - lane * CAMBI_HIP_BINS_PER_LANE;
        mine[lane] = 0u;
        for (uint32_t j = 0u; j < CAMBI_HIP_BINS_PER_LANE; j++)
            mine[lane] += sel->hist[top - j];
    }
    uint32_t before = 0u;
    for (uint32_t lane = 0u; lane < CAMBI_HIP_POOL_BLOCK; lane++) {
        const uint32_t top = CAMBI_HIP_RADIX_BINS - 1u - lane * CAMBI_HIP_BINS_PER_LANE;
        if (before < k && k <= before + mine[lane])
            cambi_hd_scan_pick(sel, top, CAMBI_HIP_BINS_PER_LANE, k, before, pass);
        before += mine[lane];
    }
    memset(sel->hist, 0, sizeof(sel->hist));
}

/* cambi_hip_radix_hist (pass 1 or 2) or, for pass 0, cambi_hip_topk_sum,
 * one pooling group at a time. */
static void emu_pool_groups(const CambiHipParams *p, unsigned scale, int pass)
{
    const CambiHipScale *sc = &p->scale[scale];
    CambiHipSelect *sel = &p->frame->select[scale];
    if (sel->resolved != 0u)
        return;
    for (uint32_t group = 0u; group < sc->pool_groups; group++) {
        uint32_t begin = 0u;
        uint32_t end = 0u;
        cambi_hd_pool_block(sc->width * sc->height, sc->pool_groups, group, &begin, &end);
        uint64_t sum = 0u;
        for (uint32_t i = begin; i < end; i++) {
            const uint32_t bits = cambi_hd_float_bits(p->cvals[i]);
            uint32_t bin = 0u;
            if (pass > 0 && cambi_hd_radix_match(bits, sel->prefix, pass, &bin))
                cambi_hd_add_u32(&sel->hist[bin], 1u);
            if (pass == 0 && bits > sel->prefix)
                sum += cambi_hd_fixed(p->cvals[i]);
        }
        if (pass == 0)
            cambi_hd_add_split(&sel->gt_lo, &sel->gt_hi, sum);
    }
}

/* Radix scan 0, passes 1 and 2, the sum above the threshold, the total. */
static void emu_pool(const CambiHipParams *p, unsigned scale)
{
    emu_radix_scan(p, scale, 0);
    for (int pass = 1; pass < CAMBI_HIP_RADIX_PASSES; pass++) {
        emu_pool_groups(p, scale, pass);
        emu_radix_scan(p, scale, pass);
    }
    emu_pool_groups(p, scale, 0);
    const CambiHdU128 total = cambi_hd_topk_total(&p->frame->select[scale]);
    p->frame->results.sum_lo[scale] = total.lo;
    p->frame->results.sum_hi[scale] = total.hi;
}

/* ------------------------------------------------------------------ */
/* Comparison.                                                         */
/* ------------------------------------------------------------------ */

static int emu_compare_scale(const Emu *e, const CpuRun *cpu, const EmuCase *c, unsigned scale,
                             double *score)
{
    const CambiHipScale *sc = &e->p.scale[scale];
    const size_t n = (size_t)sc->width * sc->height;
    if (memcmp(e->cvals, cpu->cvals[scale], n * sizeof(float)) != 0) {
        (void)fprintf(stderr, "\n%s: scale %u c-values differ from cambi.c\n", c->name, scale);
        return 1;
    }
    const CambiHipResults *r = &e->frame.results;
    *score = vmaf_cambi_fixed_topk_mean(r->sum_hi[scale], r->sum_lo[scale], sc->topk);
    if (*score != cpu->scores[scale]) {
        (void)fprintf(stderr, "\n%s: scale %u score %.17g, cambi.c %.17g\n", c->name, scale, *score,
                      cpu->scores[scale]);
        return 1;
    }
    return 0;
}

static int emu_hist_intact(const Emu *e, const EmuCase *c, unsigned scale)
{
    for (size_t i = 0u; i < EMU_HIST_CANARY; i++) {
        if (e->hist[e->hist_cells + i] != 0xA5A5u) {
            (void)fprintf(stderr, "\n%s: scale %u wrote past the column histograms\n", c->name,
                          scale);
            return 0;
        }
    }
    return 1;
}

static int emu_replay(Emu *e, const CpuRun *cpu, const EmuCase *c, double *scores)
{
    emu_preprocess(e);
    emu_mask(&e->p);
    int fail = 0;
    for (unsigned scale = 0u; scale < CAMBI_HIP_NUM_SCALES && !fail; scale++) {
        emu_scale_image(&e->p, scale);
        emu_row_masks(&e->p, scale);
        fail = emu_cvals(&e->p, scale) != 0 || !emu_hist_intact(e, c, scale);
        if (!fail) {
            emu_pool(&e->p, scale);
            fail = emu_compare_scale(e, cpu, c, scale, &scores[scale]);
        }
    }
    return fail || e->frame.results.status != 0u;
}

/* Replay one frame of the device pipeline and compare it scale by scale.
 * Returns the frame score through `score` (NAN when it failed). */
static int emu_run_case(const EmuCase *c, double *score)
{
    VmafPicture dist;
    EmuConfig cfg;
    CpuRun cpu;
    Emu e;
    memset(&dist, 0, sizeof(dist));
    memset(&cpu, 0, sizeof(cpu));
    memset(&e, 0, sizeof(e));
    double scores[CAMBI_HIP_NUM_SCALES] = {0.0};
    int fail = emu_fill(&dist, c) || emu_configure(c, &cfg) || cpu_run(&dist, c, &cfg, &cpu) ||
               emu_setup(&e, &dist, c, &cfg);
    fail = fail || emu_replay(&e, &cpu, c, scores);
    *score = NAN;
    if (!fail) {
        *score =
            vmaf_cambi_weight_scores_per_scale(scores, vmaf_cambi_get_pixels_in_window(cfg.window));
        fail = *score != cpu.score;
    }
    if (fail)
        (void)fprintf(stderr, "\n%s: device replay %.17g, cambi.c %.17g\n", c->name, *score,
                      cpu.score);
    emu_free(&e);
    cpu_free(&cpu);
    if (dist.ref)
        (void)vmaf_picture_unref(&dist);
    return fail;
}

/* ------------------------------------------------------------------ */
/* Tests.                                                              */
/* ------------------------------------------------------------------ */

/*  name, w, h, bpc, enc_w, enc_h, enc_bpc, window, mlc, speedup, units, salt, clean */
static const EmuCase emu_cases[] = {
    {"8-bit 320x240", 320u, 240u, 8u, 0u, 0u, 0u, 65, 2, 0, 1u, 0u, 0},
    {"8-bit 320x240, 7 row chunks", 320u, 240u, 8u, 0u, 0u, 0u, 65, 2, 0, 64u, 0u, 0},
    {"10-bit 640x360, window 127, mlc 3", 640u, 360u, 10u, 0u, 0u, 0u, 127, 3, 0, 4u, 1u, 0},
    {"12-bit 480x270, mlc 1", 480u, 270u, 12u, 0u, 0u, 0u, 65, 1, 0, 2u, 2u, 0},
    {"9-bit 320x240, mlc 5", 320u, 240u, 9u, 0u, 0u, 0u, 65, 5, 0, 1u, 3u, 0},
    {"8-bit 640x480 encoded at 400x300", 640u, 480u, 8u, 400u, 300u, 0u, 65, 2, 0, 3u, 4u, 1},
    {"8-bit 320x240 at enc_bitdepth 10", 320u, 240u, 8u, 0u, 0u, 10u, 65, 2, 0, 1u, 5u, 1},
    {"10-bit 320x240, mlc 0", 320u, 240u, 10u, 0u, 0u, 0u, 65, 0, 0, 1u, 8u, 1},
    {"8-bit 1920x1080, high-res speed-up", 1920u, 1080u, 8u, 0u, 0u, 0u, 65, 2, 1, 8u, 6u, 1},
    {"10-bit 1280x720, one-level bands", 1280u, 720u, 10u, 0u, 0u, 0u, 65, 2, 0, 16u, 7u, 1},
    {"10-bit 640x360, full-range ramp", 640u, 360u, 10u, 0u, 0u, 0u, 65, 2, 0, 4u, 9u, 2},
    {"8-bit 640x360, full-range ramp, mlc 4", 640u, 360u, 8u, 0u, 0u, 0u, 65, 4, 0, 4u, 3u, 2},
};

static char *test_device_replay_matches_cambi_c(void)
{
    const size_t n_cases = sizeof(emu_cases) / sizeof(emu_cases[0]);
    int nonzero = 0;
    for (size_t i = 0u; i < n_cases; i++) {
        double score = NAN;
        mu_assert("HIP CAMBI device replay differs from cambi.c",
                  !emu_run_case(&emu_cases[i], &score));
        nonzero += score > 0.0;
    }
    /* A replay of zeros proves nothing: every fixture must band. */
    mu_assert("a CAMBI replay fixture is degenerate (score 0)", nonzero == (int)n_cases);
    return NULL;
}

/* The per-chunk column histograms are a work decomposition, not a numeric
 * choice: one chunk per scale and seven give the same score. */
static char *test_row_chunks_do_not_change_the_score(void)
{
    double one = NAN;
    double many = NAN;
    mu_assert("replay (1 chunk)", !emu_run_case(&emu_cases[0], &one));
    mu_assert("replay (7 chunks)", !emu_run_case(&emu_cases[1], &many));
    mu_assert("row-chunk layout changed the CAMBI score", one == many);
    return NULL;
}

/* cambi.c::validate_image: a 10-bit sample above 1023 sets the status bit
 * collect() turns into -EINVAL, as the CPU extractor fails the frame. */
static char *test_out_of_range_sample_sets_status(void)
{
    const EmuCase c = {
        "10-bit, one sample at 1024", 320u, 240u, 10u, 0u, 0u, 0u, 65, 2, 0, 1u, 0u, 0};
    VmafPicture dist;
    EmuConfig cfg;
    Emu e;
    memset(&e, 0, sizeof(e));
    mu_assert("fixture", !emu_fill(&dist, &c));
    ((uint16_t *)dist.data[0])[5u * (dist.stride[0] / 2u) + 7u] = 1024u;
    mu_assert("configure", !emu_configure(&c, &cfg));
    VmafPicture pre;
    mu_assert("cpu picture", !vmaf_picture_alloc(&pre, VMAF_PIX_FMT_YUV400P, 10, c.w, c.h));
    const int cpu_err = vmaf_cambi_preprocessing(&dist, &pre, (int)c.w, (int)c.h, 10);
    mu_assert("replay setup", !emu_setup(&e, &dist, &c, &cfg));
    emu_preprocess(&e);
    const uint32_t status = e.frame.results.status;
    emu_free(&e);
    (void)vmaf_picture_unref(&pre);
    (void)vmaf_picture_unref(&dist);
    mu_assert("cambi.c accepted an out-of-range sample", cpu_err == -EINVAL);
    mu_assert("device replay missed the out-of-range sample",
              status == CAMBI_HIP_STATUS_INVALID_INPUT);
    return NULL;
}

/* The reference above is cambi.c's pipeline assembled from its helpers; pin it
 * to the registered extractor so the replay compares against what `cambi`
 * reports, not against a copy that could drift. */
static char *test_reference_matches_registered_extractor(void)
{
    const EmuCase *c = &emu_cases[0];
    VmafPicture ref;
    VmafPicture dist;
    EmuConfig cfg;
    CpuRun cpu;
    memset(&cpu, 0, sizeof(cpu));
    mu_assert("fixtures", !emu_fill(&ref, c) && !emu_fill(&dist, c));
    mu_assert("configure", !emu_configure(c, &cfg));
    mu_assert("reference", !cpu_run(&dist, c, &cfg, &cpu));
    const double reference = cpu.score > 1000.0 ? 1000.0 : cpu.score;
    cpu_free(&cpu);
    VmafConfiguration vcfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    mu_assert("vmaf_init", !vmaf_init(&vmaf, vcfg));
    mu_assert("use cambi", !vmaf_use_feature(vmaf, "cambi", NULL));
    mu_assert("read", !vmaf_read_pictures(vmaf, &ref, &dist, 0u));
    mu_assert("flush", !vmaf_read_pictures(vmaf, NULL, NULL, 0u));
    double extractor = NAN;
    const int err = vmaf_feature_score_at_index(vmaf, "Cambi_feature_cambi_score", &extractor, 0u);
    (void)vmaf_close(vmaf);
    mu_assert("cambi score", !err);
    if (extractor != reference)
        (void)fprintf(stderr, "\nextractor %.17g, reference %.17g\n", extractor, reference);
    mu_assert("helper pipeline differs from the registered cambi extractor",
              extractor == reference);
    return NULL;
}

/* Window-size guard (cambi.c::setup_contrast_and_luminance(), ADR-1357):
 * cambi_hip must accept and reject exactly the windows cambi.c does. A hipcc
 * build rejects them in init() before any device work, so this needs no AMD
 * device; a scaffold build reports -ENOSYS before any check (ADR-1264), so
 * there only cambi.c's half runs. At 3840x2160 the window_size option adjusts to itself (rounded up
 * to odd); the reciprocal table (4226 entries) takes windows up to 65:
 *   65 -> 65 accepted (boundary); 66 -> 67 rejected;
 *   66 at enc 1920x1080 -> 33 encode / 67 source, rejected;
 *   127 with the 2160 speed-up -> 65 accepted; 127 -> 127 rejected. */
typedef struct EmuWindowCase {
    const char *options[4]; /* "key=value", NULL-terminated */
    int rejected;
} EmuWindowCase;

static const EmuWindowCase emu_window_cases[] = {
    {{"window_size=65", NULL, NULL, NULL}, 0},
    {{"window_size=66", NULL, NULL, NULL}, 1},
    {{"window_size=66", "enc_width=1920", "enc_height=1080", NULL}, 1},
    {{"window_size=127", "cambi_high_res_speedup=2160", NULL, NULL}, 0},
    {{"window_size=127", NULL, NULL, NULL}, 1},
};

/* init() of `name` at 3840x2160 8-bit with the case's options; the context
 * is torn down either way. */
static int emu_window_init_rc(const char *name, const EmuWindowCase *c, int *rc)
{
    const VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name(name);
    VmafFeatureDictionary *opts = NULL;
    int err = fex ? 0 : -EINVAL;
    for (unsigned i = 0u; !err && c->options[i]; i++) {
        char key[32];
        const char *eq = strchr(c->options[i], '=');
        const size_t len = (size_t)(eq - c->options[i]);
        memcpy(key, c->options[i], len);
        key[len] = '\0';
        err = vmaf_feature_dictionary_set(&opts, key, eq + 1);
    }
    VmafFeatureExtractorContext *ctx = NULL;
    if (!err)
        err = vmaf_feature_extractor_context_create(&ctx, fex, (VmafDictionary *)opts);
    if (err)
        return err;
    *rc = vmaf_feature_extractor_context_init(ctx, VMAF_PIX_FMT_YUV420P, 8u, 3840u, 2160u);
    if (*rc == 0)
        err = vmaf_feature_extractor_context_close(ctx);
    const int destroy = vmaf_feature_extractor_context_destroy(ctx);
    return err ? err : destroy;
}

static char *test_window_guard_matches_cambi_c(void)
{
    const size_t n = sizeof(emu_window_cases) / sizeof(emu_window_cases[0]);
    unsigned scaffold_cases = 0u;
    for (size_t i = 0u; i < n; i++) {
        const EmuWindowCase *c = &emu_window_cases[i];
        int cpu_rc = 1;
        int hip_rc = 1;
        mu_assert("cambi init harness", !emu_window_init_rc("cambi", c, &cpu_rc));
        mu_assert("cambi_hip init harness", !emu_window_init_rc("cambi_hip", c, &hip_rc));
        if ((cpu_rc == -EINVAL) != c->rejected || (hip_rc == -EINVAL) != c->rejected)
            (void)fprintf(stderr, "\nwindow case %zu (%s): cambi %d, cambi_hip %d, rejected %d\n",
                          i, c->options[0], cpu_rc, hip_rc, c->rejected);
        mu_assert("cambi.c's window guard moved", (cpu_rc == -EINVAL) == c->rejected);
        if (hip_rc == -ENOSYS && c->rejected) {
            /* Only a scaffold build reports -ENOSYS for a window the guard
             * rejects: it gives up before any check (ADR-1264). */
            scaffold_cases++;
            continue;
        }
        /* Accepted: 0 with an AMD device, the missing device's code without
         * one; never the guard's -EINVAL. */
        mu_assert("cambi_hip's window guard differs from cambi.c",
                  (hip_rc == -EINVAL) == c->rejected);
    }
    if (scaffold_cases)
        (void)fprintf(stderr, "[cambi_hip window guard unchecked: scaffold build (-ENOSYS)] ");
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_window_guard_matches_cambi_c);
    mu_run_test(test_reference_matches_registered_extractor);
    mu_run_test(test_device_replay_matches_cambi_c);
    mu_run_test(test_row_chunks_do_not_change_the_score);
    mu_run_test(test_out_of_range_sample_sets_status);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
