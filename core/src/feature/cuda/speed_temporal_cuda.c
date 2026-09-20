/**
 *  Copyright 2016-2025 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  speed_temporal feature extractor — CUDA backend with real on-device
 *  GPU kernels (ADR-0567).
 *
 *  Temporal design: two ping-pong host float buffers hold converted luma
 *  planes.  At each frame, the GPU runs the full SpEED pipeline on the
 *  temporal *difference* (prev − cur) rather than the raw plane — matching
 *  the CPU twin in speed.c.  Frame 0 emits score 0 (no previous frame).
 *
 *  Algorithm split: identical to speed_chroma_cuda.c.  See that file and
 *  ADR-0567 for the full GPU/CPU split rationale.
 *
 *  Output feature: Speed_temporal_feature_speed_temporal_score.
 */

#include "vmaf_nullptr.h"

#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "common.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature_name.h"
#include "log.h"
#include "mem.h"
#include "picture.h"
#include "picture_copy.h"
#include "picture_cuda.h"

#include "cuda_helper.cuh"
#include "cuda/kernel_template.h"

#include "feature/speed_internal.h"
#include "cuda/speed_temporal_cuda.h"

/* lint rationale: C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `VMAF_NULLPTR` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

extern const char speed_score_ptx[];

/* ------------------------------------------------------------------ */
/* Constants (mirrors speed_chroma_cuda.c)                            */
/* ------------------------------------------------------------------ */

#define ST_BLOCK_SIZE (5u)
#define ST_ELEMENTS (ST_BLOCK_SIZE * ST_BLOCK_SIZE)
#define ST_COV_BLOCK (256u)
#define ST_MEANS_BLOCK (256u)
#define ST_INDTERM_BLOCK (256u)
#define ST_SCORE_BLOCK (256u)
#define ST_SOLVE_WARP (32u)

#define ST_DEFAULT_SIGMA_NN (0.29)
#define ST_DEFAULT_MAX_VAL (1000.0)
#define ST_DEFAULT_NN_FLOOR (0.0)
#define ST_DEFAULT_KERNELSCALE (1.0)
#define ST_DEFAULT_PRESCALE (1.0)
#define ST_DEFAULT_PRESCALE_METHOD ("nearest")

/* ------------------------------------------------------------------ */
/* Private extractor state                                             */
/* ------------------------------------------------------------------ */

typedef struct SpeedTemporalCudaState {
    CUfunction func_means;
    CUfunction func_cov;
    CUfunction func_indterm;
    CUfunction func_solve;
    CUfunction func_score;
    CUstream stream;

    SpeedInternalDimensions dim;
    SpeedInternalOptions opt;
    size_t float_stride;

    /* Ping-pong host float buffers for ref/dis luma planes. */
    float *h_ref[2];
    float *h_dis[2];

    /* Device buffers (identical layout to speed_chroma_cuda). */
    CUdeviceptr d_plane;
    CUdeviceptr d_means;
    CUdeviceptr d_cov_mat;
    CUdeviceptr d_indterm_ref;
    CUdeviceptr d_indterm_dis;
    CUdeviceptr d_sol_ref;
    CUdeviceptr d_sol_dis;
    CUdeviceptr d_R;
    CUdeviceptr d_eigenvalues;     /* holds dis eigenvalues after the dis linalg */
    CUdeviceptr d_eigenvalues_ref; /* ref eigenvalues, stashed before dis linalg */
    CUdeviceptr d_ref_entropies;
    CUdeviceptr d_ref_variances;
    CUdeviceptr d_dis_entropies;
    CUdeviceptr d_dis_variances;

    float *h_cov_mat;
    float *h_ref_entropies;
    float *h_ref_variances;
    float *h_dis_entropies;
    float *h_dis_variances;

    float *h_eigenvalues;
    float *h_eig_scratch;
    float *h_Q;
    float *h_R;
    float *h_qr_scratch;
    float *h_indterm_ref;
    float *h_indterm_dis;
    float *h_qt_scratch;

    /* Frame bookkeeping. */
    unsigned index;

    double speed_temporal_kernelscale;
    double speed_temporal_prescale;
    char *speed_temporal_prescale_method;
    double speed_temporal_sigma_nn;
    double speed_temporal_nn_floor;
    double speed_temporal_max_val;
    bool speed_temporal_use_ref_diff;

    /* PTX module backing the SpEED temporal kernels — owned here so
     * `close_fex_st` can unload it. Skipping the unload leaks
     * ~200-500 KB of GPU-resident PTX backing store per vmaf_close(). */
    CUmodule module;
    VmafDictionary *feature_name_dict;
    /* Singular covariance matrices are counted, not logged per solve. */
    SpeedInternalSingularTally singular_tally;
} SpeedTemporalCudaState;

#define SPEED_TEMPORAL_DOUBLE_OPTION(NAME, HELP, FIELD, DEFAULT, MINIMUM, MAXIMUM, ALIAS)          \
    {                                                                                              \
        .name = (NAME),                                                                            \
        .help = (HELP),                                                                            \
        .offset = offsetof(SpeedTemporalCudaState, FIELD),                                         \
        .type = VMAF_OPT_TYPE_DOUBLE,                                                              \
        .default_val.d = (DEFAULT),                                                                \
        .min = (MINIMUM),                                                                          \
        .max = (MAXIMUM),                                                                          \
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,                                                      \
        .alias = (ALIAS),                                                                          \
    }
#define SPEED_TEMPORAL_STRING_OPTION(NAME, HELP, FIELD, DEFAULT, ALIAS)                            \
    {                                                                                              \
        .name = (NAME),                                                                            \
        .help = (HELP),                                                                            \
        .offset = offsetof(SpeedTemporalCudaState, FIELD),                                         \
        .type = VMAF_OPT_TYPE_STRING,                                                              \
        .default_val.s = (DEFAULT),                                                                \
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,                                                      \
        .alias = (ALIAS),                                                                          \
    }
#define SPEED_TEMPORAL_BOOL_OPTION(NAME, HELP, FIELD, DEFAULT, ALIAS)                              \
    {                                                                                              \
        .name = (NAME),                                                                            \
        .help = (HELP),                                                                            \
        .offset = offsetof(SpeedTemporalCudaState, FIELD),                                         \
        .type = VMAF_OPT_TYPE_BOOL,                                                                \
        .default_val.b = (DEFAULT),                                                                \
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,                                                      \
        .alias = (ALIAS),                                                                          \
    }

static const VmafOption options[] = {
    SPEED_TEMPORAL_DOUBLE_OPTION("speed_kernelscale", "scaling factor for the Gaussian kernel",
                                 speed_temporal_kernelscale, ST_DEFAULT_KERNELSCALE, 0.1, 4.0,
                                 "ks"),
    SPEED_TEMPORAL_DOUBLE_OPTION("speed_prescale", "scaling factor for the frame",
                                 speed_temporal_prescale, ST_DEFAULT_PRESCALE, 0.1, 4.0, "ps"),
    SPEED_TEMPORAL_STRING_OPTION("speed_prescale_method",
                                 "scaling method [nearest, bilinear, bicubic, lanczos4]",
                                 speed_temporal_prescale_method, ST_DEFAULT_PRESCALE_METHOD, "psm"),
    SPEED_TEMPORAL_DOUBLE_OPTION("speed_sigma_nn", "standard deviation of neural noise",
                                 speed_temporal_sigma_nn, ST_DEFAULT_SIGMA_NN, 0.1, 2.0, "snn"),
    SPEED_TEMPORAL_DOUBLE_OPTION("speed_nn_floor", "neural noise floor fraction",
                                 speed_temporal_nn_floor, ST_DEFAULT_NN_FLOOR, 0.0, 1.0, "nnf"),
    SPEED_TEMPORAL_DOUBLE_OPTION("speed_max_val", "clip output to this maximum",
                                 speed_temporal_max_val, ST_DEFAULT_MAX_VAL, 0.0, 1000.0, "mxv"),
    SPEED_TEMPORAL_BOOL_OPTION("speed_use_ref_diff",
                               "use reference frame difference instead of distorted",
                               speed_temporal_use_ref_diff, false, "urd"),
    {0},
};

#undef SPEED_TEMPORAL_BOOL_OPTION
#undef SPEED_TEMPORAL_STRING_OPTION
#undef SPEED_TEMPORAL_DOUBLE_OPTION

/* ------------------------------------------------------------------ */
/* Helpers (shared with speed_chroma_cuda.c, copied for TU isolation) */
/* ------------------------------------------------------------------ */

static void subtract_plane(float *a, const float *b, int w, int h, size_t stride_bytes)
{
    const size_t stride_px = stride_bytes / sizeof(float);
    for (int i = 0; i < h; i++) {
        for (int j = 0; j < w; j++)
            a[(size_t)i * stride_px + (size_t)j] -= b[(size_t)i * stride_px + (size_t)j];
    }
}

static void free_cuda_buffers_st(SpeedTemporalCudaState *s, CudaFunctions *cu_f)
{
#define FREE_D(p)                                                                                  \
    do {                                                                                           \
        if ((p)) {                                                                                 \
            (void)cu_f->cuMemFree((p));                                                            \
            (p) = 0;                                                                               \
        }                                                                                          \
    } while (0)
#define FREE_H(p)                                                                                  \
    do {                                                                                           \
        if ((p)) {                                                                                 \
            (void)cu_f->cuMemFreeHost((p));                                                        \
            (p) = VMAF_NULLPTR;                                                                    \
        }                                                                                          \
    } while (0)
#define FREE_A(p)                                                                                  \
    do {                                                                                           \
        if ((p)) {                                                                                 \
            aligned_free((p));                                                                     \
            (p) = VMAF_NULLPTR;                                                                    \
        }                                                                                          \
    } while (0)

    FREE_D(s->d_plane);
    FREE_D(s->d_means);
    FREE_D(s->d_cov_mat);
    FREE_D(s->d_indterm_ref);
    FREE_D(s->d_indterm_dis);
    FREE_D(s->d_sol_ref);
    FREE_D(s->d_sol_dis);
    FREE_D(s->d_R);
    FREE_D(s->d_eigenvalues);
    FREE_D(s->d_eigenvalues_ref);
    FREE_D(s->d_ref_entropies);
    FREE_D(s->d_ref_variances);
    FREE_D(s->d_dis_entropies);
    FREE_D(s->d_dis_variances);
    FREE_H(s->h_cov_mat);
    FREE_H(s->h_ref_entropies);
    FREE_H(s->h_ref_variances);
    FREE_H(s->h_dis_entropies);
    FREE_H(s->h_dis_variances);
    FREE_A(s->h_ref[0]);
    FREE_A(s->h_ref[1]);
    FREE_A(s->h_dis[0]);
    FREE_A(s->h_dis[1]);
    FREE_A(s->h_eigenvalues);
    FREE_A(s->h_eig_scratch);
    FREE_A(s->h_Q);
    FREE_A(s->h_R);
    FREE_A(s->h_qr_scratch);
    FREE_A(s->h_indterm_ref);
    FREE_A(s->h_indterm_dis);
    FREE_A(s->h_qt_scratch);

#undef FREE_D
#undef FREE_H
#undef FREE_A
}

/* ------------------------------------------------------------------ */
/* GPU pipeline helpers (inline mirrors of speed_chroma_cuda.c)       */
/* ------------------------------------------------------------------ */

static int run_gpu_pipeline_st(SpeedTemporalCudaState *s, CudaFunctions *cu_f, float *h_plane,
                               CUdeviceptr d_indterm, size_t plane_op_bytes)
{
    int _cuda_err;
    const uint32_t num_blocks = (uint32_t)s->dim.num_blocks;
    const uint32_t num_blocks_h = (uint32_t)s->dim.num_blocks_horizontal;
    const uint32_t op_w = (uint32_t)s->dim.truncated_width;
    const uint32_t stride_px = (uint32_t)(s->float_stride / sizeof(float));
    const uint32_t submatrix_w = (uint32_t)s->dim.submatrix_width;
    const uint32_t submatrix_h = (uint32_t)s->dim.submatrix_height;

    CHECK_CUDA_GOTO(cu_f, cuMemcpyHtoDAsync(s->d_plane, h_plane, plane_op_bytes, s->stream), fail);

    /* K1: means */
    {
        const uint32_t grid_x = (num_blocks + ST_MEANS_BLOCK - 1u) / ST_MEANS_BLOCK;
        void *args[] = {(void *)&s->d_plane,  (void *)&s->d_means,   (void *)&op_w,
                        (void *)&stride_px,   (void *)&num_blocks_h, (void *)&num_blocks,
                        (void *)&submatrix_w, (void *)&submatrix_h};
        CHECK_CUDA_GOTO(cu_f,
                        cuLaunchKernel(s->func_means, grid_x, 1u, 1u, ST_MEANS_BLOCK, 1u, 1u, 0u,
                                       s->stream, args, VMAF_NULLPTR),
                        fail);
    }
    /* K2: cov */
    {
        void *args[] = {(void *)&s->d_plane,  (void *)&s->d_means,   (void *)&s->d_cov_mat,
                        (void *)&stride_px,   (void *)&num_blocks_h, (void *)&num_blocks,
                        (void *)&submatrix_w, (void *)&submatrix_h};
        CHECK_CUDA_GOTO(cu_f,
                        cuLaunchKernel(s->func_cov, ST_ELEMENTS, ST_ELEMENTS, 1u, ST_COV_BLOCK, 1u,
                                       1u, 0u, s->stream, args, VMAF_NULLPTR),
                        fail);
    }
    /* K3: indterm */
    {
        const uint32_t total = ST_ELEMENTS * num_blocks;
        const uint32_t grid_x = (total + ST_INDTERM_BLOCK - 1u) / ST_INDTERM_BLOCK;
        void *args[] = {(void *)&s->d_plane, (void *)&d_indterm, (void *)&stride_px,
                        (void *)&num_blocks_h, (void *)&num_blocks};
        CHECK_CUDA_GOTO(cu_f,
                        cuLaunchKernel(s->func_indterm, grid_x, 1u, 1u, ST_INDTERM_BLOCK, 1u, 1u,
                                       0u, s->stream, args, VMAF_NULLPTR),
                        fail);
    }
    CHECK_CUDA_GOTO(cu_f,
                    cuMemcpyDtoHAsync(s->h_cov_mat, s->d_cov_mat,
                                      (size_t)ST_ELEMENTS * (size_t)ST_ELEMENTS * sizeof(float),
                                      s->stream),
                    fail);
    const size_t indterm_bytes = (size_t)ST_ELEMENTS * num_blocks * sizeof(float);
    float *h_ind = (d_indterm == s->d_indterm_ref) ? s->h_indterm_ref : s->h_indterm_dis;
    CHECK_CUDA_GOTO(cu_f, cuMemcpyDtoHAsync(h_ind, d_indterm, indterm_bytes, s->stream), fail);
    CHECK_CUDA_GOTO(cu_f, cuStreamSynchronize(s->stream), fail);
    return 0;
fail:
    return _cuda_err;
}

/* `singular_out` reports a singular covariance matrix, which is NOT a failure:
 * the CPU reference zeroes the solution and reports it separately so the caller
 * can apply the one-sided-zero rule in speed_extract_score(). The return value
 * stays reserved for hard CUDA failures. Mirrors the chroma twin (ADR-1202) and
 * ADR-1218. */
static int run_cpu_linalg_st(SpeedTemporalCudaState *s, CudaFunctions *cu_f, float *h_indterm,
                             CUdeviceptr d_sol, bool *singular_out)
{
    int _cuda_err;
    const int sz = (int)ST_ELEMENTS;
    const int nb = (int)s->dim.num_blocks;

    speed_internal_compute_eigenvalues(s->h_cov_mat, s->h_eigenvalues, sz, s->h_eig_scratch);
    bool regular = speed_internal_is_matrix_regular(s->h_eigenvalues, (size_t)sz);
    *singular_out = !regular;
    speed_internal_tally_solve(&s->singular_tally, !regular, "speed_temporal_cuda");

    if (!regular) {
        /* Zero the DEVICE solution, not the host staging buffer. The score
         * kernel reads `d_sol`; the host `h_indterm` is re-downloaded from
         * `d_indterm` at the top of every pipeline run, so zeroing it changed
         * nothing. Without this, a singular frame scored against the previous
         * frame's solution — or, on the first frame, against whatever the
         * device allocator handed back. ADR-1218. */
        CHECK_CUDA_GOTO(
            cu_f, cuMemsetD8Async(d_sol, 0, (size_t)sz * (size_t)nb * sizeof(float), s->stream),
            fail);
    } else {
        (void)speed_internal_qr_factorize(s->h_cov_mat, sz, s->h_Q, s->h_R, s->h_qr_scratch);
        speed_internal_qt_multiply(s->h_Q, h_indterm, sz, nb, s->h_qt_scratch);
        CHECK_CUDA_GOTO(
            cu_f,
            cuMemcpyHtoDAsync(s->d_R, s->h_R, (size_t)sz * (size_t)sz * sizeof(float), s->stream),
            fail);
        CHECK_CUDA_GOTO(
            cu_f,
            cuMemcpyHtoDAsync(d_sol, h_indterm, (size_t)sz * (size_t)nb * sizeof(float), s->stream),
            fail);
        const uint32_t u_nb = (uint32_t)nb;
        const uint32_t threads = ((u_nb + 7u) / 8u) * ST_SOLVE_WARP;
        const uint32_t blocks = (u_nb + (threads / ST_SOLVE_WARP) - 1u) / (threads / ST_SOLVE_WARP);
        void *args[] = {(void *)&s->d_R, (void *)&d_sol, (void *)&u_nb};
        CHECK_CUDA_GOTO(cu_f,
                        cuLaunchKernel(s->func_solve, blocks, 1u, 1u, threads, 1u, 1u, 0u,
                                       s->stream, args, VMAF_NULLPTR),
                        fail);
        CHECK_CUDA_GOTO(cu_f, cuStreamSynchronize(s->stream), fail);
    }
    CHECK_CUDA_GOTO(cu_f,
                    cuMemcpyHtoDAsync(s->d_eigenvalues, s->h_eigenvalues,
                                      (size_t)sz * sizeof(float), s->stream),
                    fail);
    return 0;
fail:
    return _cuda_err;
}

static int run_score_st(SpeedTemporalCudaState *s, CudaFunctions *cu_f, float *score_out)
{
    int _cuda_err;
    const uint32_t num_blocks = (uint32_t)s->dim.num_blocks;
    const float sigma_nn = (float)s->opt.speed_sigma_nn;

    {
        const uint32_t grid = (num_blocks + ST_SCORE_BLOCK - 1u) / ST_SCORE_BLOCK;
        void *args[] = {
            (void *)&s->d_eigenvalues_ref, (void *)&s->d_eigenvalues,   (void *)&s->d_sol_ref,
            (void *)&s->d_sol_dis,         (void *)&s->d_indterm_ref,   (void *)&s->d_indterm_dis,
            (void *)&s->d_ref_entropies,   (void *)&s->d_ref_variances, (void *)&s->d_dis_entropies,
            (void *)&s->d_dis_variances,   (void *)&num_blocks,         (void *)&sigma_nn};
        CHECK_CUDA_GOTO(cu_f,
                        cuLaunchKernel(s->func_score, grid, 1u, 1u, ST_SCORE_BLOCK, 1u, 1u, 0u,
                                       s->stream, args, VMAF_NULLPTR),
                        fail);
    }
    const size_t ab = (size_t)num_blocks * sizeof(float);
    CHECK_CUDA_GOTO(cu_f, cuMemcpyDtoHAsync(s->h_ref_entropies, s->d_ref_entropies, ab, s->stream),
                    fail);
    CHECK_CUDA_GOTO(cu_f, cuMemcpyDtoHAsync(s->h_ref_variances, s->d_ref_variances, ab, s->stream),
                    fail);
    CHECK_CUDA_GOTO(cu_f, cuMemcpyDtoHAsync(s->h_dis_entropies, s->d_dis_entropies, ab, s->stream),
                    fail);
    CHECK_CUDA_GOTO(cu_f, cuMemcpyDtoHAsync(s->h_dis_variances, s->d_dis_variances, ab, s->stream),
                    fail);
    CHECK_CUDA_GOTO(cu_f, cuStreamSynchronize(s->stream), fail);

    const float base_entropy =
        (float)ST_ELEMENTS *
        (log2f((1.0f + (float)s->opt.speed_nn_floor) * (float)s->opt.speed_sigma_nn) +
         log2f(2.0f * 3.14159265358979323846f * 2.71828182845904523536f));

    float total = 0.0f;
    for (uint32_t i = 0; i < num_blocks; ++i) {
        float re = s->h_ref_entropies[i];
        float de = s->h_dis_entropies[i];
        if (re < base_entropy && de < base_entropy)
            continue;
        float rv = s->h_ref_variances[i];
        float dv = s->h_dis_variances[i];
        /* speed_temporal uses weight_var_mode = 0 (no option). */
        float spatial_ref = re * log2f(1.0f + rv);
        float spatial_dis = de * log2f(1.0f + dv);
        total += fabsf(spatial_ref - spatial_dis);
    }
    *score_out = total / (float)num_blocks;
    return 0;
fail:
    return _cuda_err;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

static void release_cuda_module_and_stream_st(SpeedTemporalCudaState *s, CudaFunctions *cu_f)
{
    if (!s || !cu_f)
        return;
    if (s->stream) {
        (void)cu_f->cuStreamDestroy(s->stream);
        s->stream = 0;
    }
    if (s->module) {
        (void)cu_f->cuModuleUnload(s->module);
        s->module = VMAF_NULLPTR;
    }
}

static void speed_temporal_set_options(SpeedTemporalCudaState *s)
{
    s->opt = (SpeedInternalOptions){
        .speed_kernelscale = s->speed_temporal_kernelscale,
        .speed_prescale = s->speed_temporal_prescale,
        .speed_prescale_method = s->speed_temporal_prescale_method,
        .speed_sigma_nn = s->speed_temporal_sigma_nn,
        .speed_nn_floor = s->speed_temporal_nn_floor,
        .speed_weight_var_mode = 0,
    };
}

static int speed_temporal_load_module(SpeedTemporalCudaState *s, CudaFunctions *cu_f)
{
    CHECK_CUDA_RETURN(cu_f, cuModuleLoadData(&s->module, speed_score_ptx));
    struct KernelSlot {
        CUfunction *function;
        const char *name;
    } kernels[] = {
        {&s->func_means, "speed_means_kernel"},     {&s->func_cov, "speed_cov_kernel"},
        {&s->func_indterm, "speed_indterm_kernel"}, {&s->func_solve, "speed_solve_kernel"},
        {&s->func_score, "speed_score_kernel"},
    };
    for (size_t i = 0; i < sizeof(kernels) / sizeof(kernels[0]); i++) {
        CHECK_CUDA_RETURN(cu_f,
                          cuModuleGetFunction(kernels[i].function, s->module, kernels[i].name));
    }
    return 0;
}

static int speed_temporal_alloc_device(SpeedTemporalCudaState *s, CudaFunctions *cu_f)
{
    const size_t stride = s->float_stride / sizeof(float);
    const size_t plane = s->dim.alloc_height * stride * sizeof(float);
    const size_t indterm = (size_t)ST_ELEMENTS * s->dim.num_blocks * sizeof(float);
    const size_t covariance = (size_t)ST_ELEMENTS * (size_t)ST_ELEMENTS * sizeof(float);
    const size_t score = s->dim.num_blocks * sizeof(float);
    CUdeviceptr *buffers[] = {
        &s->d_plane,         &s->d_means,           &s->d_cov_mat,       &s->d_indterm_ref,
        &s->d_indterm_dis,   &s->d_sol_ref,         &s->d_sol_dis,       &s->d_R,
        &s->d_eigenvalues,   &s->d_eigenvalues_ref, &s->d_ref_entropies, &s->d_ref_variances,
        &s->d_dis_entropies, &s->d_dis_variances,
    };
    const size_t sizes[] = {plane,
                            indterm,
                            covariance,
                            indterm,
                            indterm,
                            indterm,
                            indterm,
                            covariance,
                            (size_t)ST_ELEMENTS * sizeof(float),
                            (size_t)ST_ELEMENTS * sizeof(float),
                            score,
                            score,
                            score,
                            score};
    for (size_t i = 0; i < sizeof(buffers) / sizeof(buffers[0]); i++)
        CHECK_CUDA_RETURN(cu_f, cuMemAlloc(buffers[i], sizes[i]));
    return 0;
}

static int speed_temporal_alloc_pinned(SpeedTemporalCudaState *s, CudaFunctions *cu_f)
{
    const size_t covariance = (size_t)ST_ELEMENTS * (size_t)ST_ELEMENTS * sizeof(float);
    const size_t score = s->dim.num_blocks * sizeof(float);
    float **buffers[] = {&s->h_cov_mat, &s->h_ref_entropies, &s->h_ref_variances,
                         &s->h_dis_entropies, &s->h_dis_variances};
    const size_t sizes[] = {covariance, score, score, score, score};
    for (size_t i = 0; i < sizeof(buffers) / sizeof(buffers[0]); i++)
        CHECK_CUDA_RETURN(cu_f, cuMemHostAlloc((void **)buffers[i], sizes[i], 0x01u));
    return 0;
}

static int speed_temporal_create_stream(SpeedTemporalCudaState *s, CudaFunctions *cu_f)
{
    CHECK_CUDA_RETURN(cu_f, cuStreamCreate(&s->stream, CU_STREAM_NON_BLOCKING));
    return 0;
}

static int speed_temporal_init_device(VmafFeatureExtractor *fex, SpeedTemporalCudaState *s)
{
    CudaFunctions *cu_f = fex->cu_state->f;
    CUresult push = cu_f->cuCtxPushCurrent(fex->cu_state->ctx);
    if (push != CUDA_SUCCESS)
        return vmaf_cuda_result_to_errno((int)push);
    int ret = speed_temporal_load_module(s, cu_f);
    if (!ret)
        ret = speed_temporal_create_stream(s, cu_f);
    if (!ret)
        ret = speed_temporal_alloc_device(s, cu_f);
    if (!ret)
        ret = speed_temporal_alloc_pinned(s, cu_f);
    const CUresult pop = cu_f->cuCtxPopCurrent(VMAF_NULLPTR);
    if (!ret && pop != CUDA_SUCCESS)
        ret = vmaf_cuda_result_to_errno((int)pop);
    return ret;
}

static int speed_temporal_alloc_cpu(SpeedTemporalCudaState *s)
{
    const size_t stride = s->float_stride / sizeof(float);
    const size_t plane = s->dim.alloc_height * stride * sizeof(float);
    const size_t indterm = (size_t)ST_ELEMENTS * s->dim.num_blocks * sizeof(float);
    const size_t covariance = (size_t)ST_ELEMENTS * (size_t)ST_ELEMENTS * sizeof(float);
    float **buffers[] = {&s->h_ref[0],      &s->h_ref[1],      &s->h_dis[0],      &s->h_dis[1],
                         &s->h_eigenvalues, &s->h_eig_scratch, &s->h_Q,           &s->h_R,
                         &s->h_qr_scratch,  &s->h_indterm_ref, &s->h_indterm_dis, &s->h_qt_scratch};
    const size_t sizes[] = {
        plane,
        plane,
        plane,
        plane,
        (size_t)ST_ELEMENTS * sizeof(float),
        ((size_t)ST_ELEMENTS * (size_t)ST_ELEMENTS + (size_t)4U * ST_ELEMENTS) * sizeof(float),
        covariance,
        covariance,
        4u * covariance,
        indterm,
        indterm,
        indterm,
    };
    for (size_t i = 0; i < sizeof(buffers) / sizeof(buffers[0]); i++) {
        *buffers[i] = (float *)aligned_malloc(sizes[i], 32);
        if (!*buffers[i])
            return -ENOMEM;
    }
    return 0;
}

static int init_fex_st(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                       unsigned w, unsigned h)
{
    (void)pix_fmt;
    (void)bpc;

    SpeedTemporalCudaState *s = fex->priv;
    CudaFunctions *cu_f = fex->cu_state->f;
    speed_temporal_set_options(s);
    int err = speed_internal_init_dimensions(&s->dim, (int)w, (int)h, s->opt.speed_prescale);
    if (err)
        return err;
    s->float_stride = speed_internal_float_stride(s->dim.alloc_width);
    err = speed_temporal_alloc_cpu(s);
    if (!err)
        err = speed_temporal_init_device(fex, s);
    if (!err) {
        s->feature_name_dict =
            vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
        if (!s->feature_name_dict)
            err = -ENOMEM;
    }
    if (err) {
        free_cuda_buffers_st(s, cu_f);
        release_cuda_module_and_stream_st(s, cu_f);
        return err;
    }
    s->index = 0;
    return 0;
}

static int speed_temporal_copy_input(VmafFeatureExtractor *fex, SpeedTemporalCudaState *s,
                                     const VmafPicture *ref, const VmafPicture *dist, int cyclic)
{
    CudaFunctions *cu_f = fex->cu_state->f;
    const size_t ref_bytes = (size_t)ref->h[0] * ref->stride[0];
    const size_t dist_bytes = (size_t)dist->h[0] * dist->stride[0];
    uint8_t *raw_ref = (uint8_t *)aligned_malloc(ref_bytes, 32);
    uint8_t *raw_dist = (uint8_t *)aligned_malloc(dist_bytes, 32);
    if (!raw_ref || !raw_dist) {
        aligned_free(raw_ref);
        aligned_free(raw_dist);
        return -ENOMEM;
    }
    const CUresult push = cu_f->cuCtxPushCurrent(fex->cu_state->ctx);
    if (push != CUDA_SUCCESS) {
        aligned_free(raw_ref);
        aligned_free(raw_dist);
        return vmaf_cuda_result_to_errno((int)push);
    }
    const bool copy_failed =
        cu_f->cuMemcpyDtoH(raw_ref, (CUdeviceptr)ref->data[0], ref_bytes) != CUDA_SUCCESS ||
        cu_f->cuMemcpyDtoH(raw_dist, (CUdeviceptr)dist->data[0], dist_bytes) != CUDA_SUCCESS;
    (void)cu_f->cuCtxPopCurrent(VMAF_NULLPTR);
    if (!copy_failed) {
        VmafPicture host_ref = *ref;
        VmafPicture host_dist = *dist;
        host_ref.data[0] = raw_ref;
        host_dist.data[0] = raw_dist;
        picture_copy(s->h_ref[cyclic], s->float_stride, &host_ref, -128, ref->bpc, 0);
        picture_copy(s->h_dis[cyclic], s->float_stride, &host_dist, -128, ref->bpc, 0);
    }
    aligned_free(raw_ref);
    aligned_free(raw_dist);
    return copy_failed ? -EIO : 0;
}

static void speed_temporal_prepare_diff(SpeedTemporalCudaState *s, int current, int previous)
{
    const int width = (int)s->dim.original_width;
    const int height = (int)s->dim.original_height;
    subtract_plane(s->h_ref[previous], s->h_ref[current], width, height, s->float_stride);
    if (s->speed_temporal_use_ref_diff) {
        subtract_plane(s->h_dis[previous], s->h_ref[current], width, height, s->float_stride);
    } else {
        subtract_plane(s->h_dis[previous], s->h_dis[current], width, height, s->float_stride);
    }
}

static int speed_temporal_filter_diff(SpeedTemporalCudaState *s, int previous)
{
    const size_t stride = s->float_stride / sizeof(float);
    const size_t elements = 2u * s->dim.alloc_height * stride;
    float *scratch = (float *)aligned_malloc(elements * sizeof(float), 32);
    if (!scratch)
        return -ENOMEM;
    speed_internal_filter_and_downscale(&s->dim, &s->opt, s->h_ref[previous], scratch,
                                        s->float_stride);
    speed_internal_filter_and_downscale(&s->dim, &s->opt, s->h_dis[previous], scratch,
                                        s->float_stride);
    aligned_free(scratch);
    return 0;
}

static int speed_temporal_stash_eigenvalues(SpeedTemporalCudaState *s, CudaFunctions *cu_f)
{
    CHECK_CUDA_RETURN(cu_f, cuStreamSynchronize(s->stream));
    CHECK_CUDA_RETURN(cu_f, cuMemcpyDtoD(s->d_eigenvalues_ref, s->d_eigenvalues,
                                         (size_t)ST_ELEMENTS * sizeof(float)));
    return 0;
}

static int speed_temporal_score_diff(VmafFeatureExtractor *fex, SpeedTemporalCudaState *s,
                                     int previous, float *score)
{
    CudaFunctions *cu_f = fex->cu_state->f;
    const size_t plane_bytes =
        s->dim.truncated_height * (s->float_stride / sizeof(float)) * sizeof(float);
    const CUresult push = cu_f->cuCtxPushCurrent(fex->cu_state->ctx);
    if (push != CUDA_SUCCESS)
        return vmaf_cuda_result_to_errno((int)push);
    bool singular_ref = false;
    int ret = run_gpu_pipeline_st(s, cu_f, s->h_ref[previous], s->d_indterm_ref, plane_bytes);
    if (!ret)
        ret = run_cpu_linalg_st(s, cu_f, s->h_indterm_ref, s->d_sol_ref, &singular_ref);
    if (!ret)
        ret = speed_temporal_stash_eigenvalues(s, cu_f);
    bool singular_dis = false;
    if (!ret)
        ret = run_gpu_pipeline_st(s, cu_f, s->h_dis[previous], s->d_indterm_dis, plane_bytes);
    if (!ret)
        ret = run_cpu_linalg_st(s, cu_f, s->h_indterm_dis, s->d_sol_dis, &singular_dis);
    *score = 0.0f;
    if (!ret && singular_ref == singular_dis)
        ret = run_score_st(s, cu_f, score);
    const CUresult pop = cu_f->cuCtxPopCurrent(VMAF_NULLPTR);
    if (!ret && pop != CUDA_SUCCESS)
        ret = vmaf_cuda_result_to_errno((int)pop);
    return ret;
}

static int extract_fex_st(VmafFeatureExtractor *fex, const VmafPicture *ref_pic,
                          const VmafPicture *ref_pic_90, const VmafPicture *dist_pic,
                          const VmafPicture *dist_pic_90, unsigned index,
                          VmafFeatureCollector *feature_collector)
{
    (void)ref_pic_90;
    (void)dist_pic_90;

    SpeedTemporalCudaState *s = fex->priv;
    const int cyclic = (int)(index % 2u);
    const int other = (int)((index + 1u) % 2u);
    int err = speed_temporal_copy_input(fex, s, ref_pic, dist_pic, cyclic);
    if (err)
        return err;
    if (index == 0) {
        return vmaf_feature_collector_append_with_dict(
            feature_collector, s->feature_name_dict, "Speed_temporal_feature_speed_temporal_score",
            0.0, index);
    }
    speed_temporal_prepare_diff(s, cyclic, other);
    err = speed_temporal_filter_diff(s, other);
    if (err)
        return err;
    float score = 0.0f;
    err = speed_temporal_score_diff(fex, s, other, &score);
    if (err)
        return err;
    const double clipped =
        (double)score < s->speed_temporal_max_val ? (double)score : s->speed_temporal_max_val;
    return vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "Speed_temporal_feature_speed_temporal_score",
                                                   clipped, index);
}

static int close_fex_st(VmafFeatureExtractor *fex)
{
    SpeedTemporalCudaState *s = fex->priv;
    speed_internal_report_singular(&s->singular_tally, "speed_temporal_cuda");
    if (fex->cu_state && fex->cu_state->f) {
        free_cuda_buffers_st(s, fex->cu_state->f);
        release_cuda_module_and_stream_st(s, fex->cu_state->f);
    }
    if (s->feature_name_dict)
        vmaf_dictionary_free(&s->feature_name_dict);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Registration                                                        */
/* ------------------------------------------------------------------ */

static const char *provided_features[] = {"Speed_temporal_feature_speed_temporal_score",
                                          VMAF_NULLPTR};

/* ADR-0567: Real GPU kernels — means, covariance, indterm,
 * backward-substitution, score.  TEMPORAL flag guarantees sequential
 * submission (frame ordering required for ping-pong diff). */
VmafFeatureExtractor vmaf_fex_speed_temporal_cuda = {
    .name = "speed_temporal_cuda",
    .init = init_fex_st,
    .extract = extract_fex_st,
    .close = close_fex_st,
    .options = options,
    .priv_size = sizeof(SpeedTemporalCudaState),
    .provided_features = provided_features,
    .flags = VMAF_FEATURE_EXTRACTOR_TEMPORAL | VMAF_FEATURE_EXTRACTOR_CUDA,
};
