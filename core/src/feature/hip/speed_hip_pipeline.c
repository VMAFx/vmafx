/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Host side of the device-resident SpEED chain (ADR-1384): the parameter
 *  block the kernels read, and the per-frame enqueue of
 *  speed/speed_pipeline.hip. No SpEED stage runs on the host and nothing
 *  below waits on the device except the two collect-time entry points,
 *  speed_hip_pipeline_collect() and speed_hip_pipeline_wait().
 */

#include "speed_hip_pipeline.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* HIP common.h must come before <hip/hip_runtime_api.h>: it provides the
 * typedef stubs used without HAVE_HIPCC. */
#include "../../hip/common.h"
#include "../../hip/kernel_template.h"

#ifdef HAVE_HIPCC
#include <hip/hip_runtime_api.h>

#include "../../hip/hip_handle.h"
#include "../../hip/picture_hip.h"
#endif /* HAVE_HIPCC */

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `NULL` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

/* ------------------------------------------------------------------ */
/* The parameter block (host only, every build)                        */
/* ------------------------------------------------------------------ */

size_t speed_hip_plane_bytes(const SpeedGpuGeometry *geometry)
{
    return (size_t)geometry->src_w * geometry->src_h * geometry->bytes_per_sample;
}

void speed_hip_taps_fill(float *taps, const SpeedGpuFilters *filters)
{
    memcpy(taps, filters->antialias, sizeof(filters->antialias));
    memcpy(taps + SPEED_HIP_MAX_TAPS, filters->lowpass, sizeof(filters->lowpass));
}

void speed_hip_params_fill(SpeedHipParams *q, const SpeedHipConfig *c,
                           const SpeedHipBindingSets *bindings)
{
    const SpeedGpuGeometry *g = &c->shared.geometry;
    q->geometry = *g;
    q->scoring = c->shared.scoring;
    memcpy(q->bindings, bindings->set, sizeof(q->bindings));
    q->plane_bytes = (uint32_t)speed_hip_plane_bytes(g);
    q->channels = c->channels;
    q->antialias_width = c->shared.filters.antialias_width;
    q->lowpass_width = c->shared.filters.lowpass_width;
    q->cov_group = speed_hd_covariance_group_size(g->sub_w * g->sub_h);
    q->reserved = 0u;
}

void speed_hip_bindings_chroma(SpeedHipBindingSets *bindings)
{
    for (uint32_t set = 0u; set < SPEED_HIP_BINDING_SETS; set++) {
        for (uint32_t ch = 0u; ch < SPEED_HIP_MAX_CHANNELS; ch++) {
            bindings->set[set][ch].minuend = (int32_t)ch;
            bindings->set[set][ch].subtrahend = -1;
        }
    }
}

void speed_hip_bindings_temporal(int use_ref_diff, SpeedHipBindingSets *bindings)
{
    speed_hip_bindings_chroma(bindings);
    for (uint32_t set = 0u; set < SPEED_HIP_BINDING_SETS; set++) {
        const int32_t current = (int32_t)(2u * set);
        const int32_t previous = (int32_t)(2u * ((set + 1u) % SPEED_HIP_BINDING_SETS));
        bindings->set[set][0].minuend = previous;
        bindings->set[set][0].subtrahend = current;
        bindings->set[set][1].minuend = previous + 1;
        bindings->set[set][1].subtrahend = use_ref_diff ? current : current + 1;
    }
}

static bool speed_hip_shape_valid(const SpeedHipConfig *c)
{
    const SpeedGpuGeometry *g = &c->shared.geometry;
    const SpeedGpuFilters *f = &c->shared.filters;
    const bool channels = c->channels == 2u || c->channels == 4u;
    const bool planes = c->raw_planes >= 1u && c->raw_planes <= SPEED_HIP_MAX_RAW_PLANES &&
                        c->staged >= 1u && c->staged <= c->raw_planes;
    const bool taps = f->antialias_width >= 1u && f->antialias_width <= SPEED_HIP_MAX_TAPS &&
                      f->lowpass_width >= 1u && f->lowpass_width <= SPEED_HIP_MAX_TAPS;
    const bool dims = g->blocks > 0u && g->sub_w > 0u && g->sub_h > 0u && g->down_w >= g->trunc_w &&
                      g->down_h >= g->trunc_h;
    const bool samples = g->bytes_per_sample == 1u || g->bytes_per_sample == 2u;
    return channels && planes && taps && dims && samples;
}

static bool speed_hip_bindings_valid(const SpeedHipConfig *c, const SpeedHipBindingSets *bindings)
{
    for (uint32_t set = 0u; set < SPEED_HIP_BINDING_SETS; set++) {
        for (uint32_t ch = 0u; ch < c->channels; ch++) {
            const SpeedGpuChannelBinding b = bindings->set[set][ch];
            const bool minuend = b.minuend >= 0 && (uint32_t)b.minuend < c->raw_planes;
            const bool subtrahend = b.subtrahend < 0 || (uint32_t)b.subtrahend < c->raw_planes;
            if (!minuend || !subtrahend)
                return false;
        }
    }
    return true;
}

int speed_hip_config_valid(const SpeedHipConfig *config, const SpeedHipBindingSets *bindings)
{
    if (!config || !bindings)
        return 0;
    return speed_hip_shape_valid(config) && speed_hip_bindings_valid(config, bindings);
}

#ifndef HAVE_HIPCC

int speed_hip_pipeline_create(SpeedHipPipeline **out, const SpeedHipConfig *config,
                              const SpeedHipBindingSets *bindings)
{
    (void)config;
    (void)bindings;
    if (out)
        *out = NULL;
    return -ENOSYS;
}

void speed_hip_pipeline_destroy(SpeedHipPipeline **pipeline)
{
    if (pipeline)
        *pipeline = NULL;
}

int speed_hip_pipeline_upload(SpeedHipPipeline *pipeline, uint32_t first,
                              const SpeedHipPlane *planes, uint32_t count)
{
    (void)pipeline;
    (void)first;
    (void)planes;
    (void)count;
    return -ENOSYS;
}

int speed_hip_pipeline_submit(SpeedHipPipeline *pipeline, uint32_t set)
{
    (void)pipeline;
    (void)set;
    return -ENOSYS;
}

int speed_hip_pipeline_collect(SpeedHipPipeline *pipeline, SpeedGpuFrameResult *out)
{
    (void)pipeline;
    (void)out;
    return -ENOSYS;
}

int speed_hip_pipeline_wait(SpeedHipPipeline *pipeline)
{
    (void)pipeline;
    return -ENOSYS;
}

#else /* HAVE_HIPCC */

/* Embedded HSACO blob generated by meson / xxd from speed/speed_pipeline.hip. */
extern const unsigned char speed_pipeline_hsaco[];
extern const unsigned int speed_pipeline_hsaco_len;

/* Kernel entry points of speed_pipeline.hip, in launch order. */
enum SpeedHipKernel {
    SPEED_K_SCALE,
    SPEED_K_DECIMATE,
    SPEED_K_CENTRE,
    SPEED_K_MEANS,
    SPEED_K_COVARIANCE,
    SPEED_K_LINALG,
    SPEED_K_SOLVE,
    SPEED_K_SCORE,
    SPEED_K_COUNT
};

static const char *const speed_hip_kernel_names[SPEED_K_COUNT] = {
    "speed_hip_scale",      "speed_hip_decimate", "speed_hip_centre", "speed_hip_means",
    "speed_hip_covariance", "speed_hip_linalg",   "speed_hip_solve",  "speed_hip_score",
};

#define SPEED_HIP_ARENA_ALIGN ((size_t)256u)

struct SpeedHipPipeline {
    SpeedHipConfig config;
    VmafHipContext *ctx;
    VmafHipKernelLifecycle lc; /* the pipeline's stream and events */
    hipModule_t module;
    hipFunction_t kernels[SPEED_K_COUNT];
    void *d_arena;
    unsigned char *d_raw;
    SpeedHipParams *d_params;
    SpeedHipParams params;
    void *h_staging; /* vmaf_hip_picture_upload_staged() source */
    size_t staging_bytes;
    SpeedGpuFrameResult *h_result; /* pinned readback of the frame result */
    size_t plane_bytes;
};

static size_t speed_hip_take(size_t *cursor, size_t bytes)
{
    const size_t offset = *cursor;
    *cursor += (bytes + SPEED_HIP_ARENA_ALIGN - 1u) / SPEED_HIP_ARENA_ALIGN * SPEED_HIP_ARENA_ALIGN;
    return offset;
}

/* Byte offsets of every buffer inside the device arena. */
typedef struct SpeedHipArena {
    size_t params;
    size_t raw;
    size_t taps;
    size_t scaled;
    size_t down;
    size_t centered;
    size_t indterm;
    size_t means;
    size_t cov;
    size_t eig;
    size_t qmat;
    size_t rmat;
    size_t var;
    size_t ent;
    size_t contrib;
    size_t status;
    size_t result;
    size_t total;
} SpeedHipArena;

static SpeedHipArena speed_hip_arena_layout(const SpeedHipPipeline *p)
{
    const SpeedGpuGeometry *g = &p->config.shared.geometry;
    const size_t ch = p->config.channels;
    const size_t f = sizeof(float);
    const size_t scaled = g->prescale != 0 ? ch * g->scaled_w * g->scaled_h * f : 0u;
    size_t cursor = 0u;
    SpeedHipArena a;
    a.params = speed_hip_take(&cursor, sizeof(SpeedHipParams));
    a.raw = speed_hip_take(&cursor, p->plane_bytes * p->config.raw_planes);
    a.taps = speed_hip_take(&cursor, 2u * SPEED_HIP_MAX_TAPS * f);
    a.scaled = speed_hip_take(&cursor, scaled);
    a.down = speed_hip_take(&cursor, ch * g->down_w * g->down_h * f);
    a.centered = speed_hip_take(&cursor, ch * g->trunc_w * g->trunc_h * f);
    a.indterm = speed_hip_take(&cursor, ch * SPEED_HIP_N * g->blocks * f);
    a.means = speed_hip_take(&cursor, ch * SPEED_HIP_N * f);
    a.cov = speed_hip_take(&cursor, ch * SPEED_HIP_MATRIX * f);
    a.eig = speed_hip_take(&cursor, ch * SPEED_HIP_N * f);
    a.qmat = speed_hip_take(&cursor, ch * SPEED_HIP_MATRIX * f);
    a.rmat = speed_hip_take(&cursor, ch * SPEED_HIP_MATRIX * f);
    a.var = speed_hip_take(&cursor, ch * g->blocks * f);
    a.ent = speed_hip_take(&cursor, ch * g->blocks * f);
    a.contrib = speed_hip_take(&cursor, (ch / 2u) * g->blocks * f);
    a.status = speed_hip_take(&cursor, ch * 2u * sizeof(int32_t));
    a.result = speed_hip_take(&cursor, sizeof(SpeedGpuFrameResult));
    a.total = cursor;
    return a;
}

/* Point the parameter block at the arena at device address `base`. */
static void speed_hip_bind(SpeedHipPipeline *p, unsigned char *base, const SpeedHipArena *a)
{
    SpeedHipParams *q = &p->params;
    p->d_raw = base + a->raw;
    q->raw = p->d_raw;
    q->taps = (float *)(void *)(base + a->taps);
    q->scaled =
        p->config.shared.geometry.prescale != 0 ? (float *)(void *)(base + a->scaled) : NULL;
    q->down = (float *)(void *)(base + a->down);
    q->centered = (float *)(void *)(base + a->centered);
    q->indterm = (float *)(void *)(base + a->indterm);
    q->means = (float *)(void *)(base + a->means);
    q->cov = (float *)(void *)(base + a->cov);
    q->eig = (float *)(void *)(base + a->eig);
    q->qmat = (float *)(void *)(base + a->qmat);
    q->rmat = (float *)(void *)(base + a->rmat);
    q->var = (float *)(void *)(base + a->var);
    q->ent = (float *)(void *)(base + a->ent);
    q->contrib = (float *)(void *)(base + a->contrib);
    q->status = (int32_t *)(void *)(base + a->status);
    q->result = (SpeedGpuFrameResult *)(void *)(base + a->result);
    p->d_params = (SpeedHipParams *)(void *)(base + a->params);
}

static int speed_hip_load_module(SpeedHipPipeline *p)
{
    hipError_t rc = hipModuleLoadData(&p->module, speed_pipeline_hsaco);
    if (rc != hipSuccess) {
        p->module = NULL;
        return vmaf_hip_rc_to_errno(rc);
    }
    for (int k = 0; k < SPEED_K_COUNT && rc == hipSuccess; ++k)
        rc = hipModuleGetFunction(&p->kernels[k], p->module, speed_hip_kernel_names[k]);
    return vmaf_hip_rc_to_errno(rc);
}

/* The device arena, the pinned staging and result blocks, and the one-time
 * upload of the taps and the parameter block (init only). */
static int speed_hip_allocate(SpeedHipPipeline *p, const SpeedHipBindingSets *bindings)
{
    const SpeedHipArena a = speed_hip_arena_layout(p);
    hipError_t rc = hipMalloc(&p->d_arena, a.total);
    if (rc != hipSuccess) {
        p->d_arena = NULL;
        return vmaf_hip_rc_to_errno(rc);
    }
    p->staging_bytes = p->plane_bytes * p->config.staged;
    const int err = vmaf_hip_picture_staging_alloc(&p->h_staging, p->staging_bytes);
    if (err) {
        p->h_staging = NULL;
        return err;
    }
    rc = hipHostMalloc((void **)&p->h_result, sizeof(SpeedGpuFrameResult), hipHostMallocDefault);
    if (rc != hipSuccess) {
        p->h_result = NULL;
        return vmaf_hip_rc_to_errno(rc);
    }
    speed_hip_bind(p, (unsigned char *)p->d_arena, &a);
    speed_hip_params_fill(&p->params, &p->config, bindings);
    float taps[2u * SPEED_HIP_MAX_TAPS];
    speed_hip_taps_fill(taps, &p->config.shared.filters);
    rc = hipMemcpy(p->params.taps, taps, sizeof(taps), hipMemcpyHostToDevice);
    if (rc == hipSuccess)
        rc = hipMemcpy(p->d_params, &p->params, sizeof(p->params), hipMemcpyHostToDevice);
    return vmaf_hip_rc_to_errno(rc);
}

/* Release in the safe order: drain and destroy the stream first, so no
 * queued copy or kernel still uses the memory freed after it. */
static void speed_hip_release(SpeedHipPipeline *p)
{
    (void)vmaf_hip_kernel_lifecycle_close(&p->lc, p->ctx);
    vmaf_hip_picture_staging_free(p->h_staging);
    p->h_staging = NULL;
    if (p->h_result) {
        (void)hipHostFree(p->h_result);
        p->h_result = NULL;
    }
    if (p->d_arena) {
        (void)hipFree(p->d_arena);
        p->d_arena = NULL;
    }
    if (p->module) {
        (void)hipModuleUnload(p->module);
        p->module = NULL;
    }
    if (p->ctx) {
        vmaf_hip_context_destroy(p->ctx);
        p->ctx = NULL;
    }
}

int speed_hip_pipeline_create(SpeedHipPipeline **out, const SpeedHipConfig *config,
                              const SpeedHipBindingSets *bindings)
{
    if (!out || !speed_hip_config_valid(config, bindings))
        return -EINVAL;
    SpeedHipPipeline *p = calloc(1u, sizeof(*p));
    if (!p)
        return -ENOMEM;
    p->config = *config;
    p->plane_bytes = speed_hip_plane_bytes(&config->shared.geometry);
    int err = vmaf_hip_context_new(&p->ctx, 0);
    if (!err)
        err = vmaf_hip_kernel_lifecycle_init(&p->lc, p->ctx);
    if (!err)
        err = speed_hip_load_module(p);
    if (!err)
        err = speed_hip_allocate(p, bindings);
    if (err) {
        speed_hip_release(p);
        free(p);
        return err;
    }
    *out = p;
    return 0;
}

void speed_hip_pipeline_destroy(SpeedHipPipeline **pipeline)
{
    if (!pipeline || !*pipeline)
        return;
    speed_hip_release(*pipeline);
    free(*pipeline);
    *pipeline = NULL;
}

int speed_hip_pipeline_upload(SpeedHipPipeline *p, uint32_t first, const SpeedHipPlane *planes,
                              uint32_t count)
{
    if (!p || !planes || count == 0u || count > p->config.staged ||
        first + count > p->config.raw_planes)
        return -EINVAL;
    const SpeedGpuGeometry *g = &p->config.shared.geometry;
    const size_t row_bytes = (size_t)g->src_w * g->bytes_per_sample;
    VmafHipPlaneUpload uploads[SPEED_HIP_MAX_RAW_PLANES];
    for (uint32_t i = 0u; i < count; i++) {
        const VmafPicture *pic = planes[i].pic;
        const unsigned plane = planes[i].plane;
        if (!pic || plane > 2u || pic->w[plane] < g->src_w || pic->h[plane] < g->src_h)
            return -EINVAL;
        uploads[i].dst = p->d_raw + (size_t)(first + i) * p->plane_bytes;
        uploads[i].dst_pitch = row_bytes;
        uploads[i].pic = pic;
        uploads[i].plane = plane;
        uploads[i].row_bytes = row_bytes;
        uploads[i].rows = g->src_h;
    }
    return vmaf_hip_picture_upload_staged(uploads, count, p->h_staging, p->staging_bytes,
                                          p->lc.str);
}

static int speed_hip_launch(SpeedHipPipeline *p, int kernel, unsigned gx, unsigned gy, unsigned gz,
                            unsigned block_x, unsigned block_y, uint32_t set)
{
    void *args[] = {&p->d_params, &set};
    const hipError_t rc = hipModuleLaunchKernel(p->kernels[kernel], gx, gy, gz, block_x, block_y,
                                                1u, 0u, vmaf_hip_stream_of(p->lc.str), args, NULL);
    return vmaf_hip_rc_to_errno(rc);
}

/* A SPEED_HIP_IMAGE_TILE^2 work-group per tile of a w x h plane, per channel. */
static int speed_hip_launch_planes(SpeedHipPipeline *p, int kernel, unsigned w, unsigned h,
                                   uint32_t set)
{
    const unsigned t = SPEED_HIP_IMAGE_TILE;
    return speed_hip_launch(p, kernel, (w + t - 1u) / t, (h + t - 1u) / t, p->config.channels, t, t,
                            set);
}

/* The device part of one frame, from the raw planes to the result block. */
static int speed_hip_enqueue_chain(SpeedHipPipeline *p, uint32_t set)
{
    const SpeedGpuGeometry *g = &p->config.shared.geometry;
    const unsigned items = SPEED_HIP_ITEMS_BLOCK;
    const unsigned ch = p->config.channels;
    int err = 0;
    if (g->prescale != 0)
        err = speed_hip_launch_planes(p, SPEED_K_SCALE, g->scaled_w, g->scaled_h, set);
    if (!err)
        err = speed_hip_launch_planes(p, SPEED_K_DECIMATE, g->down_w, g->down_h, set);
    if (!err)
        err = speed_hip_launch_planes(p, SPEED_K_CENTRE, g->trunc_w, g->trunc_h, set);
    if (!err)
        err = speed_hip_launch(p, SPEED_K_MEANS, (ch * SPEED_HIP_N + items - 1u) / items, 1u, 1u,
                               items, 1u, set);
    if (!err)
        err = speed_hip_launch(p, SPEED_K_COVARIANCE, ch * SPEED_HIP_TRIANGLE, 1u, 1u,
                               p->params.cov_group, 1u, set);
    if (!err)
        err = speed_hip_launch(p, SPEED_K_LINALG, ch, 1u, 1u, SPEED_HIP_LINALG_GROUP, 1u, set);
    if (!err)
        err = speed_hip_launch(p, SPEED_K_SOLVE, (g->blocks + items - 1u) / items, ch, 1u, items,
                               1u, set);
    if (!err)
        err = speed_hip_launch(p, SPEED_K_SCORE, ch / 2u, 1u, 1u, SPEED_HIP_GROUP, 1u, set);
    return err;
}

int speed_hip_pipeline_submit(SpeedHipPipeline *p, uint32_t set)
{
    if (!p || set >= SPEED_HIP_BINDING_SETS)
        return -EINVAL;
    int err = speed_hip_enqueue_chain(p, set);
    if (!err)
        err = vmaf_hip_rc_to_errno(
            hipMemcpyAsync(p->h_result, p->params.result, sizeof(SpeedGpuFrameResult),
                           hipMemcpyDeviceToHost, vmaf_hip_stream_of(p->lc.str)));
    if (!err)
        err = vmaf_hip_kernel_submit_post_record(&p->lc, p->ctx);
    return err;
}

int speed_hip_pipeline_wait(SpeedHipPipeline *p)
{
    if (!p)
        return -EINVAL;
    return vmaf_hip_kernel_collect_wait(&p->lc, p->ctx);
}

int speed_hip_pipeline_collect(SpeedHipPipeline *p, SpeedGpuFrameResult *out)
{
    if (!out)
        return -EINVAL;
    const int err = speed_hip_pipeline_wait(p);
    if (!err)
        *out = *p->h_result;
    return err;
}

#endif /* HAVE_HIPCC */

/* NOLINTEND(modernize-use-nullptr) */
