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

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `NULL` and
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

static const VmafOption options[] = {
    {
        .name = "speed_kernelscale",
        .help = "scaling factor for the Gaussian kernel",
        .offset = offsetof(SpeedTemporalCudaState, speed_temporal_kernelscale),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = ST_DEFAULT_KERNELSCALE,
        .min = 0.1,
        .max = 4.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "ks",
    },
    {
        .name = "speed_prescale",
        .help = "scaling factor for the frame",
        .offset = offsetof(SpeedTemporalCudaState, speed_temporal_prescale),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = ST_DEFAULT_PRESCALE,
        .min = 0.1,
        .max = 4.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "ps",
    },
    {
        .name = "speed_prescale_method",
        .help = "scaling method [nearest, bilinear, bicubic, lanczos4]",
        .offset = offsetof(SpeedTemporalCudaState, speed_temporal_prescale_method),
        .type = VMAF_OPT_TYPE_STRING,
        .default_val.s = ST_DEFAULT_PRESCALE_METHOD,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "psm",
    },
    {
        .name = "speed_sigma_nn",
        .help = "standard deviation of neural noise",
        .offset = offsetof(SpeedTemporalCudaState, speed_temporal_sigma_nn),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = ST_DEFAULT_SIGMA_NN,
        .min = 0.1,
        .max = 2.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "snn",
    },
    {
        .name = "speed_nn_floor",
        .help = "neural noise floor fraction",
        .offset = offsetof(SpeedTemporalCudaState, speed_temporal_nn_floor),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = ST_DEFAULT_NN_FLOOR,
        .min = 0.0,
        .max = 1.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "nnf",
    },
    {
        .name = "speed_max_val",
        .help = "clip output to this maximum",
        .offset = offsetof(SpeedTemporalCudaState, speed_temporal_max_val),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = ST_DEFAULT_MAX_VAL,
        .min = 0.0,
        .max = 1000.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "mxv",
    },
    {
        .name = "speed_use_ref_diff",
        .help = "use reference frame difference instead of distorted",
        .offset = offsetof(SpeedTemporalCudaState, speed_temporal_use_ref_diff),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = false,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "urd",
    },
    {0},
};

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

static void st_free_host_aligned(SpeedTemporalCudaState *s)
{
#define FREE_A(p)                                                                                  \
    do {                                                                                           \
        if ((p)) {                                                                                 \
            aligned_free((p));                                                                     \
            (p) = NULL;                                                                            \
        }                                                                                          \
    } while (0)

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

#undef FREE_A
}

static int st_free_device(SpeedTemporalCudaState *s, VmafCudaState *cu_state)
{
    int rc = 0;
#define FREE_D(p)                                                                                  \
    do {                                                                                           \
        const int free_err = vmaf_cuda_deviceptr_free_owned(cu_state, &(p));                       \
        if (free_err != 0 && rc == 0)                                                              \
            rc = free_err;                                                                         \
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
#undef FREE_D
    return rc;
}

static int st_free_pinned(SpeedTemporalCudaState *s, VmafCudaState *cu_state)
{
    int rc = vmaf_cuda_buffer_host_free_owned(cu_state, (void **)&s->h_cov_mat);
    int e = vmaf_cuda_buffer_host_free_owned(cu_state, (void **)&s->h_ref_entropies);
    if (e && !rc)
        rc = e;
    e = vmaf_cuda_buffer_host_free_owned(cu_state, (void **)&s->h_ref_variances);
    if (e && !rc)
        rc = e;
    e = vmaf_cuda_buffer_host_free_owned(cu_state, (void **)&s->h_dis_entropies);
    if (e && !rc)
        rc = e;
    e = vmaf_cuda_buffer_host_free_owned(cu_state, (void **)&s->h_dis_variances);
    if (e && !rc)
        rc = e;
    return rc;
}

static int free_cuda_buffers_st(SpeedTemporalCudaState *s, VmafCudaState *cu_state)
{
    if (!s)
        return -EINVAL;
    if (!cu_state || !cu_state->f || !cu_state->ctx) {
        st_free_host_aligned(s);
        return -EINVAL;
    }

    int rc = st_free_device(s, cu_state);
    const int pinned_rc = st_free_pinned(s, cu_state);
    if (!rc)
        rc = pinned_rc;
    st_free_host_aligned(s);
    return rc;
}

/* ------------------------------------------------------------------ */
/* GPU pipeline helpers (inline mirrors of speed_chroma_cuda.c)       */
/* ------------------------------------------------------------------ */

static int run_gpu_pipeline_st(SpeedTemporalCudaState *s, CudaFunctions *cu_f, float *h_plane,
                               CUdeviceptr d_indterm, size_t plane_op_bytes)
{
    int _cuda_err = 0;
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
                                       s->stream, args, NULL),
                        fail);
    }
    /* K2: cov */
    {
        void *args[] = {(void *)&s->d_plane,  (void *)&s->d_means,   (void *)&s->d_cov_mat,
                        (void *)&stride_px,   (void *)&num_blocks_h, (void *)&num_blocks,
                        (void *)&submatrix_w, (void *)&submatrix_h};
        CHECK_CUDA_GOTO(cu_f,
                        cuLaunchKernel(s->func_cov, ST_ELEMENTS, ST_ELEMENTS, 1u, ST_COV_BLOCK, 1u,
                                       1u, 0u, s->stream, args, NULL),
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
                                       0u, s->stream, args, NULL),
                        fail);
    }
    CHECK_CUDA_GOTO(cu_f,
                    cuMemcpyDtoHAsync(s->h_cov_mat, s->d_cov_mat,
                                      ST_ELEMENTS * ST_ELEMENTS * sizeof(float), s->stream),
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
    int _cuda_err = 0;
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
                                       s->stream, args, NULL),
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
    int _cuda_err = 0;
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
                                       s->stream, args, NULL),
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

/* speed_temporal_init_unwind - the single teardown path for init_fex_cuda.
 *
 * The stream must quiesce before any memory it may reference is released.
 * Buffer failures retain their owner fields for retry, and the module is
 * unloaded last. The original init error remains authoritative.
 */
static int speed_temporal_init_unwind(SpeedTemporalCudaState *s, VmafCudaState *cu_state, int err)
{
    int rc = err;
    const int phase_rc = vmaf_cuda_stream_destroy(cu_state, &s->stream, true);
    if (phase_rc)
        return rc ? rc : phase_rc;

    int e = free_cuda_buffers_st(s, cu_state);
    if (e && !rc)
        rc = e;
    e = vmaf_dictionary_free(&s->feature_name_dict);
    if (e && !rc)
        rc = e;
    e = vmaf_cuda_module_unload(cu_state, &s->module);
    if (e && !rc)
        rc = e;
    return rc;
}

/* st_init_unwind_pop - the body the former `fail_pop` label ran, verbatim and
 * in the same order.
 */
static int st_init_unwind_pop(SpeedTemporalCudaState *s, VmafCudaState *cu_state, int cuda_err)
{
    CudaFunctions *const cu_f = cu_state->f;
    const CUresult pop_res = cu_f->cuCtxPopCurrent(NULL);
    if (pop_res != CUDA_SUCCESS)
        (void)cu_f->cuCtxPopCurrent(NULL);
    return speed_temporal_init_unwind(s, cu_state, cuda_err);
}

/* st_get_kernels - load the PTX module, resolve the kernels, create the
 * stream.
 *
 * HISS-04: lifted verbatim out of init_fex_st. Inside a helper the macro is
 * CHECK_CUDA_RETURN rather than CHECK_CUDA_GOTO; the caller routes a non-zero
 * return into st_init_unwind_pop(), which is the body `fail_pop` ran.
 */
static int st_get_kernels(SpeedTemporalCudaState *s, CudaFunctions *cu_f)
{
    CHECK_CUDA_RETURN(cu_f, cuModuleLoadData(&s->module, speed_score_ptx));
    CHECK_CUDA_RETURN(cu_f, cuModuleGetFunction(&s->func_means, s->module, "speed_means_kernel"));
    CHECK_CUDA_RETURN(cu_f, cuModuleGetFunction(&s->func_cov, s->module, "speed_cov_kernel"));
    CHECK_CUDA_RETURN(cu_f,
                      cuModuleGetFunction(&s->func_indterm, s->module, "speed_indterm_kernel"));
    CHECK_CUDA_RETURN(cu_f, cuModuleGetFunction(&s->func_solve, s->module, "speed_solve_kernel"));
    CHECK_CUDA_RETURN(cu_f, cuModuleGetFunction(&s->func_score, s->module, "speed_score_kernel"));
    CHECK_CUDA_RETURN(cu_f, cuStreamCreate(&s->stream, CU_STREAM_NON_BLOCKING));
    return 0;
}

/* st_alloc_buffers - every device allocation plus the pinned host staging.
 *
 * HISS-04: lifted verbatim out of init_fex_st; same set, same order.
 */
static int st_alloc_buffers(SpeedTemporalCudaState *s, CudaFunctions *cu_f, size_t plane_alloc,
                            size_t indterm_bytes, size_t cov_bytes, size_t score_bytes)
{
#define ALLOC_D(field, sz) CHECK_CUDA_RETURN(cu_f, cuMemAlloc(&(s->field), (sz)))
    ALLOC_D(d_plane, plane_alloc);
    ALLOC_D(d_means, indterm_bytes);
    ALLOC_D(d_cov_mat, cov_bytes);
    ALLOC_D(d_indterm_ref, indterm_bytes);
    ALLOC_D(d_indterm_dis, indterm_bytes);
    ALLOC_D(d_sol_ref, indterm_bytes);
    ALLOC_D(d_sol_dis, indterm_bytes);
    ALLOC_D(d_R, cov_bytes);
    ALLOC_D(d_eigenvalues, ST_ELEMENTS * sizeof(float));
    ALLOC_D(d_eigenvalues_ref, ST_ELEMENTS * sizeof(float));
    ALLOC_D(d_ref_entropies, score_bytes);
    ALLOC_D(d_ref_variances, score_bytes);
    ALLOC_D(d_dis_entropies, score_bytes);
    ALLOC_D(d_dis_variances, score_bytes);
#undef ALLOC_D

#define ALLOC_H(field, sz)                                                                         \
    CHECK_CUDA_RETURN(cu_f, cuMemHostAlloc((void **)&(s->field), (sz), 0x01u))
    ALLOC_H(h_cov_mat, cov_bytes);
    ALLOC_H(h_ref_entropies, score_bytes);
    ALLOC_H(h_ref_variances, score_bytes);
    ALLOC_H(h_dis_entropies, score_bytes);
    ALLOC_H(h_dis_variances, score_bytes);
#undef ALLOC_H
    return 0;
}

/* st_init_cuda - push the context, set the device side up, pop it again.
 *
 * HISS-01 / HISS-04: lifted out of init_fex_st. The `fail` and `fail_pop`
 * CHECK_CUDA_GOTO sites keep their labels; the `fail_pop` body became
 * st_init_unwind_pop(), reached on exactly the failures that jumped to it.
 */
static int st_init_cuda(VmafFeatureExtractor *fex, SpeedTemporalCudaState *s, CudaFunctions *cu_f,
                        size_t plane_alloc, size_t indterm_bytes, size_t cov_bytes,
                        size_t score_bytes)
{
    int _cuda_err = 0;
    CHECK_CUDA_GOTO(cu_f, cuCtxPushCurrent(fex->cu_state->ctx), fail);

    int err = st_get_kernels(s, cu_f);
    if (err)
        return st_init_unwind_pop(s, fex->cu_state, err);

    err = st_alloc_buffers(s, cu_f, plane_alloc, indterm_bytes, cov_bytes, score_bytes);
    if (err)
        return st_init_unwind_pop(s, fex->cu_state, err);

    CHECK_CUDA_GOTO(cu_f, cuCtxPopCurrent(NULL), fail_pop);
    return 0;

fail_pop:
    return st_init_unwind_pop(s, fex->cu_state, _cuda_err);
fail:
    return _cuda_err;
}

/* st_alloc_host_scratch - the aligned CPU-side scratch buffers.
 *
 * HISS-04: lifted verbatim out of init_fex_st; the sizes, the order and the
 * single combined NULL check are unchanged.
 */
static int st_alloc_host_scratch(SpeedTemporalCudaState *s, VmafCudaState *cu_state,
                                 size_t plane_alloc, size_t cov_bytes, size_t indterm_bytes)
{
    /* CPU buffers. */
    s->h_ref[0] = (float *)aligned_malloc(plane_alloc, 32);
    s->h_ref[1] = (float *)aligned_malloc(plane_alloc, 32);
    s->h_dis[0] = (float *)aligned_malloc(plane_alloc, 32);
    s->h_dis[1] = (float *)aligned_malloc(plane_alloc, 32);
    s->h_eigenvalues = (float *)aligned_malloc(ST_ELEMENTS * sizeof(float), 32);
    s->h_eig_scratch =
        (float *)aligned_malloc((ST_ELEMENTS * ST_ELEMENTS + 4u * ST_ELEMENTS) * sizeof(float), 32);
    s->h_Q = (float *)aligned_malloc(cov_bytes, 32);
    s->h_R = (float *)aligned_malloc(cov_bytes, 32);
    s->h_qr_scratch = (float *)aligned_malloc(4u * cov_bytes, 32);
    s->h_indterm_ref = (float *)aligned_malloc(indterm_bytes, 32);
    s->h_indterm_dis = (float *)aligned_malloc(indterm_bytes, 32);
    s->h_qt_scratch = (float *)aligned_malloc(indterm_bytes, 32);

    if (!s->h_ref[0] || !s->h_ref[1] || !s->h_dis[0] || !s->h_dis[1] || !s->h_eigenvalues ||
        !s->h_eig_scratch || !s->h_Q || !s->h_R || !s->h_qr_scratch || !s->h_indterm_ref ||
        !s->h_indterm_dis || !s->h_qt_scratch) {
        return speed_temporal_init_unwind(s, cu_state, -ENOMEM);
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

    s->opt = (SpeedInternalOptions){
        .speed_kernelscale = s->speed_temporal_kernelscale,
        .speed_prescale = s->speed_temporal_prescale,
        .speed_prescale_method = s->speed_temporal_prescale_method,
        .speed_sigma_nn = s->speed_temporal_sigma_nn,
        .speed_nn_floor = s->speed_temporal_nn_floor,
        .speed_weight_var_mode = 0,
    };

    int err = speed_internal_init_dimensions(&s->dim, (int)w, (int)h, s->opt.speed_prescale);
    if (err)
        return err;

    s->float_stride = speed_internal_float_stride(s->dim.alloc_width);

    const size_t stride_px = s->float_stride / sizeof(float);
    const size_t num_blocks = s->dim.num_blocks;
    const size_t plane_alloc = s->dim.alloc_height * stride_px * sizeof(float);
    const size_t indterm_bytes = ST_ELEMENTS * num_blocks * sizeof(float);
    const size_t cov_bytes = ST_ELEMENTS * ST_ELEMENTS * sizeof(float);
    const size_t score_bytes = num_blocks * sizeof(float);

    err = st_init_cuda(fex, s, cu_f, plane_alloc, indterm_bytes, cov_bytes, score_bytes);
    if (err)
        return err;

    err = st_alloc_host_scratch(s, fex->cu_state, plane_alloc, cov_bytes, indterm_bytes);
    if (err)
        return err;

    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (!s->feature_name_dict)
        return speed_temporal_init_unwind(s, fex->cu_state, -ENOMEM);

    s->index = 0;
    return 0;
}

/* speed_temporal_pop_ctx - the single teardown path for extract_fex_cuda.
 *
 * HISS-01: the body of the former `pop_ctx` label, unchanged. `pop_ctx` is
 * still a CHECK_CUDA_GOTO target, so the label stays and now defers here;
 * every path pops the context once and returns the same code.
 */
static int speed_temporal_pop_ctx(CudaFunctions *cu_f, int err)
{
    (void)cu_f->cuCtxPopCurrent(NULL);
    return err;
}

/* st_pop_done - pop the context and mark the caller's early-return path.
 *
 * HISS-04: `*done` reproduces the fact that every
 * `return speed_temporal_pop_ctx(...)` inside extract_fex_st returned from
 * the extractor immediately, without emitting a feature — including the
 * former `pop_ctx` label, which deliberately returned `err` (still 0 at both
 * CHECK_CUDA_GOTO sites that target it) rather than `_cuda_err`.
 */
static int st_pop_done(CudaFunctions *cu_f, int err, bool *done)
{
    *done = true;
    return speed_temporal_pop_ctx(cu_f, err);
}

/* st_stage_luma_planes - download both luma planes to host staging, then CPU
 * copy them into the ping-pong buffers.
 *
 * HISS-04: lifted verbatim out of extract_fex_st.
 *
 * The CUDA pipeline feeds DEVICE-resident pictures (ref_pic->data[] are
 * CUdeviceptr), but picture_copy reads HOST memory — download the luma
 * plane first (same device-pointer SEGV as speed_chroma_cuda). The DtoH
 * copy needs the CUDA context active (the GPU pipeline later pushes it
 * again, so push/pop locally here).
 */
static int st_stage_luma_planes(VmafFeatureExtractor *fex, SpeedTemporalCudaState *s,
                                CudaFunctions *cu_f, VmafPicture *ref_pic, VmafPicture *dist_pic,
                                int cyclic)
{
    int _cuda_err = 0;
    const size_t raw_ref_bytes = (size_t)ref_pic->h[0] * ref_pic->stride[0];
    const size_t raw_dis_bytes = (size_t)dist_pic->h[0] * dist_pic->stride[0];
    uint8_t *raw_ref = (uint8_t *)aligned_malloc(raw_ref_bytes, 32);
    uint8_t *raw_dis = (uint8_t *)aligned_malloc(raw_dis_bytes, 32);
    if (!raw_ref || !raw_dis) {
        aligned_free(raw_ref);
        aligned_free(raw_dis);
        return -ENOMEM;
    }
    if (cu_f->cuCtxPushCurrent(fex->cu_state->ctx) != CUDA_SUCCESS) {
        aligned_free(raw_ref);
        aligned_free(raw_dis);
        return -EIO;
    }
    _cuda_err = (cu_f->cuMemcpyDtoH(raw_ref, (CUdeviceptr)ref_pic->data[0], raw_ref_bytes) !=
                     CUDA_SUCCESS ||
                 cu_f->cuMemcpyDtoH(raw_dis, (CUdeviceptr)dist_pic->data[0], raw_dis_bytes) !=
                     CUDA_SUCCESS);
    (void)cu_f->cuCtxPopCurrent(NULL);
    if (_cuda_err) {
        aligned_free(raw_ref);
        aligned_free(raw_dis);
        return -EIO;
    }
    VmafPicture host_ref = *ref_pic;
    VmafPicture host_dis = *dist_pic;
    host_ref.data[0] = raw_ref;
    host_dis.data[0] = raw_dis;

    /* Copy current frame luma planes to ping-pong buffers. */
    picture_copy(s->h_ref[cyclic], s->float_stride, &host_ref, -128, ref_pic->bpc, 0);
    picture_copy(s->h_dis[cyclic], s->float_stride, &host_dis, -128, ref_pic->bpc, 0);
    aligned_free(raw_ref);
    aligned_free(raw_dis);
    return 0;
}

/* st_prepare_diff - the temporal difference planes, filtered and downscaled.
 *
 * HISS-04: lifted verbatim out of extract_fex_st.
 */
static int st_prepare_diff(SpeedTemporalCudaState *s, int cyclic, int other, size_t *plane_op_bytes)
{
    /* Temporal difference: other_index = previous frame. */
    const int w = (int)s->dim.original_width;
    const int h = (int)s->dim.original_height;
    subtract_plane(s->h_ref[other], s->h_ref[cyclic], w, h, s->float_stride);
    if (s->speed_temporal_use_ref_diff) {
        subtract_plane(s->h_dis[other], s->h_ref[cyclic], w, h, s->float_stride);
    } else {
        subtract_plane(s->h_dis[other], s->h_dis[cyclic], w, h, s->float_stride);
    }

    /* Allocate filter tmp buffer. */
    const size_t stride_px = s->float_stride / sizeof(float);
    const size_t tmp_size = 2u * s->dim.alloc_height * stride_px;
    float *tmp_filter = (float *)aligned_malloc(tmp_size * sizeof(float), 32);
    if (!tmp_filter)
        return -ENOMEM;

    /* Filter+downscale the temporal diff planes. */
    speed_internal_filter_and_downscale(&s->dim, &s->opt, s->h_ref[other], tmp_filter,
                                        s->float_stride);
    speed_internal_filter_and_downscale(&s->dim, &s->opt, s->h_dis[other], tmp_filter,
                                        s->float_stride);
    aligned_free(tmp_filter);

    *plane_op_bytes = s->dim.truncated_height * stride_px * sizeof(float);
    return 0;
}

/* st_score_or_zero - the singular-side rule.
 *
 * HISS-04: lifted verbatim out of extract_fex_st.
 *
 * Exactly one side numerically unstable: report 0 rather than the inflated
 * score a zeroed solution on one side produces. Verbatim the CPU rule in
 * speed_extract_score() (speed.c), which this twin has to match. When BOTH
 * sides are singular the CPU still scores, from two zeroed solutions — so do
 * we, which is why the singular branch in run_cpu_linalg_st zeroes `d_sol` on
 * the device. ADR-1218.
 */
static int st_score_or_zero(SpeedTemporalCudaState *s, CudaFunctions *cu_f, bool singular_ref,
                            bool singular_dis, float *score_out)
{
    *score_out = 0.0f;
    if (singular_ref != singular_dis)
        return 0;
    return run_score_st(s, cu_f, score_out);
}

/* st_gpu_score - push the context, run both diff passes and the score kernel,
 * then pop the context again.
 *
 * HISS-01 / HISS-04: lifted verbatim out of extract_fex_st, labels included.
 *
 * The eigenvalue stash: the reference eigenvalues are copied aside before the
 * distorted linalg pass overwrites s->d_eigenvalues. The CPU reference
 * (est_params in speed.c) computes SEPARATE ref and dis covariance +
 * eigenvalues; the score kernel needs both. The stream is synchronized first
 * so the async eigenvalue H2D from run_cpu_linalg_st is complete before the
 * DtoD copy.
 *
 * `fail_pop`: the pop itself failed after a successful push, so it is retried
 * to keep the CUDA context stack balanced (otherwise a per-frame leak), and
 * the original CUDA error is propagated rather than the success path's err.
 */
static int st_gpu_score(VmafFeatureExtractor *fex, SpeedTemporalCudaState *s, CudaFunctions *cu_f,
                        int other, size_t plane_op_bytes, float *score_out, bool *done)
{
    int _cuda_err = 0;
    int err = 0;
    *done = false;
    CHECK_CUDA_GOTO(cu_f, cuCtxPushCurrent(fex->cu_state->ctx), fail);

    /* GPU pipeline for reference diff. */
    err = run_gpu_pipeline_st(s, cu_f, s->h_ref[other], s->d_indterm_ref, plane_op_bytes);
    if (err)
        return st_pop_done(cu_f, err, done);

    /* CPU eigendecomp + QR for the reference diff. Uploads ref eigenvalues
     * into the shared s->d_eigenvalues buffer. */
    bool singular_ref = false;
    err = run_cpu_linalg_st(s, cu_f, s->h_indterm_ref, s->d_sol_ref, &singular_ref);
    if (err)
        return st_pop_done(cu_f, err, done);

    CHECK_CUDA_GOTO(cu_f, cuStreamSynchronize(s->stream), pop_ctx);
    CHECK_CUDA_GOTO(
        cu_f, cuMemcpyDtoD(s->d_eigenvalues_ref, s->d_eigenvalues, ST_ELEMENTS * sizeof(float)),
        pop_ctx);

    /* GPU pipeline for distorted diff (keeps the DIS covariance in h_cov_mat —
     * no save/restore of the ref covariance). */
    err = run_gpu_pipeline_st(s, cu_f, s->h_dis[other], s->d_indterm_dis, plane_op_bytes);
    if (err)
        return st_pop_done(cu_f, err, done);

    /* CPU eigendecomp + QR for the distorted diff (uses the DIS cov_mat).
     * Uploads dis eigenvalues into s->d_eigenvalues. */
    bool singular_dis = false;
    err = run_cpu_linalg_st(s, cu_f, s->h_indterm_dis, s->d_sol_dis, &singular_dis);
    if (err)
        return st_pop_done(cu_f, err, done);

    err = st_score_or_zero(s, cu_f, singular_ref, singular_dis, score_out);
    if (err)
        return st_pop_done(cu_f, err, done);

    CHECK_CUDA_GOTO(cu_f, cuCtxPopCurrent(NULL), fail_pop);
    return 0;

pop_ctx:
    return st_pop_done(cu_f, err, done);
fail_pop:
    (void)cu_f->cuCtxPopCurrent(NULL);
    *done = true;
    return _cuda_err;
fail:
    *done = true;
    return _cuda_err;
}

static int extract_fex_st(VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                          VmafPicture *dist_pic, VmafPicture *dist_pic_90, unsigned index,
                          VmafFeatureCollector *feature_collector)
{
    (void)ref_pic_90;
    (void)dist_pic_90;

    SpeedTemporalCudaState *s = fex->priv;
    CudaFunctions *cu_f = fex->cu_state->f;

    const int cyclic = (int)(index % 2u);
    const int other = (int)((index + 1u) % 2u);

    int err = st_stage_luma_planes(fex, s, cu_f, ref_pic, dist_pic, cyclic);
    if (err)
        return err;

    /* Frame 0: emit score 0 (no previous frame to diff against). */
    if (index == 0) {
        return vmaf_feature_collector_append_with_dict(
            feature_collector, s->feature_name_dict, "Speed_temporal_feature_speed_temporal_score",
            0.0, index);
    }

    size_t plane_op_bytes = 0;
    err = st_prepare_diff(s, cyclic, other, &plane_op_bytes);
    if (err)
        return err;

    float score = 0.0f;
    bool done = false;
    err = st_gpu_score(fex, s, cu_f, other, plane_op_bytes, &score, &done);
    if (done || err)
        return err;

    /* Every clamp here was a less-than comparison, and every comparison
     * against NaN is false, so a non-finite score was published as
     * speed_temporal_max_val -- a finite, plausible 1000.0 standing in for a
     * computation that produced no number, and invisible to the parity
     * harness's own isfinite() assertion. speed_internal_clamp_score() checks
     * finiteness first and fails the frame, matching the CPU reference and
     * the brisque.c / y_funque_plus.c convention. Finite scores clamp exactly
     * as before. */
    double clipped = 0.0;
    err = speed_internal_clamp_score(score, s->speed_temporal_max_val, index, "speed_temporal_cuda",
                                     "speed_temporal", &clipped);
    if (err)
        return err;

    return vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "Speed_temporal_feature_speed_temporal_score",
                                                   clipped, index);
}

static int close_fex_st(VmafFeatureExtractor *fex)
{
    SpeedTemporalCudaState *s = fex->priv;
    speed_internal_report_singular(&s->singular_tally, "speed_temporal_cuda");
    return speed_temporal_init_unwind(s, fex->cu_state, 0);
}

/* ------------------------------------------------------------------ */
/* Registration                                                        */
/* ------------------------------------------------------------------ */

static const char *provided_features[] = {"Speed_temporal_feature_speed_temporal_score", NULL};

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

/* NOLINTEND(modernize-use-nullptr) */
