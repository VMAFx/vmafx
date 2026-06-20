/**
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
#include <string.h>

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
    CUdeviceptr d_eigenvalues;
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
    for (int i = 0; i < h; i++)
        for (int j = 0; j < w; j++)
            a[(size_t)i * stride_px + (size_t)j] -= b[(size_t)i * stride_px + (size_t)j];
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
            (p) = NULL;                                                                            \
        }                                                                                          \
    } while (0)
#define FREE_A(p)                                                                                  \
    do {                                                                                           \
        if ((p)) {                                                                                 \
            aligned_free((p));                                                                     \
            (p) = NULL;                                                                            \
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
    return -EIO;
}

static int run_cpu_linalg_st(SpeedTemporalCudaState *s, CudaFunctions *cu_f, float *h_indterm,
                             CUdeviceptr d_sol)
{
    int _cuda_err = 0;
    const int sz = (int)ST_ELEMENTS;
    const int nb = (int)s->dim.num_blocks;

    speed_internal_compute_eigenvalues(s->h_cov_mat, s->h_eigenvalues, sz, s->h_eig_scratch);
    bool regular = speed_internal_is_matrix_regular(s->h_eigenvalues, (size_t)sz);

    if (!regular) {
        vmaf_log(VMAF_LOG_LEVEL_WARNING,
                 "speed_temporal_cuda: covariance matrix singular, zeroing solution\n");
        (void)memset(h_indterm, 0, (size_t)sz * (size_t)nb * sizeof(float));
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
    return -EIO;
}

static int run_score_st(SpeedTemporalCudaState *s, CudaFunctions *cu_f, float *score_out)
{
    int _cuda_err = 0;
    const uint32_t num_blocks = (uint32_t)s->dim.num_blocks;
    const float sigma_nn = (float)s->opt.speed_sigma_nn;

    {
        const uint32_t grid = (num_blocks + ST_SCORE_BLOCK - 1u) / ST_SCORE_BLOCK;
        void *args[] = {(void *)&s->d_eigenvalues,
                        (void *)&s->d_sol_ref,
                        (void *)&s->d_sol_dis,
                        (void *)&s->d_indterm_ref,
                        (void *)&s->d_indterm_dis,
                        (void *)&s->d_ref_entropies,
                        (void *)&s->d_ref_variances,
                        (void *)&s->d_dis_entropies,
                        (void *)&s->d_dis_variances,
                        (void *)&num_blocks,
                        (void *)&sigma_nn};
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
    return -EIO;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

static int init_fex_st(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                       unsigned w, unsigned h)
{
    (void)pix_fmt;
    (void)bpc;

    SpeedTemporalCudaState *s = fex->priv;
    CudaFunctions *cu_f = fex->cu_state->f;
    int _cuda_err = 0;
    int err = 0;

    s->opt = (SpeedInternalOptions){
        .speed_kernelscale = s->speed_temporal_kernelscale,
        .speed_prescale = s->speed_temporal_prescale,
        .speed_prescale_method = s->speed_temporal_prescale_method,
        .speed_sigma_nn = s->speed_temporal_sigma_nn,
        .speed_nn_floor = s->speed_temporal_nn_floor,
        .speed_weight_var_mode = 0,
    };

    err = speed_internal_init_dimensions(&s->dim, (int)w, (int)h, s->opt.speed_prescale);
    if (err)
        return err;

    s->float_stride = speed_internal_float_stride(s->dim.alloc_width);

    const size_t stride_px = s->float_stride / sizeof(float);
    const size_t num_blocks = s->dim.num_blocks;
    const size_t plane_alloc = s->dim.alloc_height * stride_px * sizeof(float);
    const size_t indterm_bytes = ST_ELEMENTS * num_blocks * sizeof(float);
    const size_t cov_bytes = ST_ELEMENTS * ST_ELEMENTS * sizeof(float);
    const size_t score_bytes = num_blocks * sizeof(float);

    CHECK_CUDA_GOTO(cu_f, cuCtxPushCurrent(fex->cu_state->ctx), fail);
    CHECK_CUDA_GOTO(cu_f, cuModuleLoadData(&s->module, speed_score_ptx), fail_pop);
    CHECK_CUDA_GOTO(cu_f, cuModuleGetFunction(&s->func_means, s->module, "speed_means_kernel"),
                    fail_pop);
    CHECK_CUDA_GOTO(cu_f, cuModuleGetFunction(&s->func_cov, s->module, "speed_cov_kernel"),
                    fail_pop);
    CHECK_CUDA_GOTO(cu_f, cuModuleGetFunction(&s->func_indterm, s->module, "speed_indterm_kernel"),
                    fail_pop);
    CHECK_CUDA_GOTO(cu_f, cuModuleGetFunction(&s->func_solve, s->module, "speed_solve_kernel"),
                    fail_pop);
    CHECK_CUDA_GOTO(cu_f, cuModuleGetFunction(&s->func_score, s->module, "speed_score_kernel"),
                    fail_pop);
    CHECK_CUDA_GOTO(cu_f, cuStreamCreate(&s->stream, CU_STREAM_NON_BLOCKING), fail_pop);

#define ALLOC_D(field, sz) CHECK_CUDA_GOTO(cu_f, cuMemAlloc(&(s->field), (sz)), fail_pop)
    ALLOC_D(d_plane, plane_alloc);
    ALLOC_D(d_means, indterm_bytes);
    ALLOC_D(d_cov_mat, cov_bytes);
    ALLOC_D(d_indterm_ref, indterm_bytes);
    ALLOC_D(d_indterm_dis, indterm_bytes);
    ALLOC_D(d_sol_ref, indterm_bytes);
    ALLOC_D(d_sol_dis, indterm_bytes);
    ALLOC_D(d_R, cov_bytes);
    ALLOC_D(d_eigenvalues, ST_ELEMENTS * sizeof(float));
    ALLOC_D(d_ref_entropies, score_bytes);
    ALLOC_D(d_ref_variances, score_bytes);
    ALLOC_D(d_dis_entropies, score_bytes);
    ALLOC_D(d_dis_variances, score_bytes);
#undef ALLOC_D

#define ALLOC_H(field, sz)                                                                         \
    CHECK_CUDA_GOTO(cu_f, cuMemHostAlloc((void **)&(s->field), (sz), 0x01u), fail_pop)
    ALLOC_H(h_cov_mat, cov_bytes);
    ALLOC_H(h_ref_entropies, score_bytes);
    ALLOC_H(h_ref_variances, score_bytes);
    ALLOC_H(h_dis_entropies, score_bytes);
    ALLOC_H(h_dis_variances, score_bytes);
#undef ALLOC_H

    CHECK_CUDA_GOTO(cu_f, cuCtxPopCurrent(NULL), fail_pop);

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
        err = -ENOMEM;
        goto free_all;
    }

    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (!s->feature_name_dict) {
        err = -ENOMEM;
        goto free_all;
    }

    s->index = 0;
    return 0;

free_all:
    free_cuda_buffers_st(s, cu_f);
    return err;
fail_pop:
    (void)cu_f->cuCtxPopCurrent(NULL);
    return -EIO;
fail:
    return -EIO;
}

static int extract_fex_st(VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                          VmafPicture *dist_pic, VmafPicture *dist_pic_90, unsigned index,
                          VmafFeatureCollector *feature_collector)
{
    (void)ref_pic_90;
    (void)dist_pic_90;

    SpeedTemporalCudaState *s = fex->priv;
    CudaFunctions *cu_f = fex->cu_state->f;
    int _cuda_err = 0;
    int err = 0;

    const int cyclic = (int)(index % 2u);
    const int other = (int)((index + 1u) % 2u);

    /* Copy current frame luma planes to ping-pong buffers. */
    picture_copy(s->h_ref[cyclic], s->float_stride, ref_pic, -128, ref_pic->bpc, 0);
    picture_copy(s->h_dis[cyclic], s->float_stride, dist_pic, -128, ref_pic->bpc, 0);

    /* Frame 0: emit score 0 (no previous frame to diff against). */
    if (index == 0) {
        return vmaf_feature_collector_append_with_dict(
            feature_collector, s->feature_name_dict, "Speed_temporal_feature_speed_temporal_score",
            0.0, index);
    }

    /* Temporal difference: other_index = previous frame. */
    const int w = (int)s->dim.original_width;
    const int h = (int)s->dim.original_height;
    subtract_plane(s->h_ref[other], s->h_ref[cyclic], w, h, s->float_stride);
    if (s->speed_temporal_use_ref_diff)
        subtract_plane(s->h_dis[other], s->h_ref[cyclic], w, h, s->float_stride);
    else
        subtract_plane(s->h_dis[other], s->h_dis[cyclic], w, h, s->float_stride);

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

    const size_t plane_op_bytes = s->dim.truncated_height * stride_px * sizeof(float);

    CHECK_CUDA_GOTO(cu_f, cuCtxPushCurrent(fex->cu_state->ctx), fail);

    /* GPU pipeline for reference diff. */
    err = run_gpu_pipeline_st(s, cu_f, s->h_ref[other], s->d_indterm_ref, plane_op_bytes);
    if (err)
        goto pop_ctx;

    float saved_cov[ST_ELEMENTS * ST_ELEMENTS];
    (void)memcpy(saved_cov, s->h_cov_mat, sizeof(saved_cov));

    err = run_cpu_linalg_st(s, cu_f, s->h_indterm_ref, s->d_sol_ref);
    if (err)
        goto pop_ctx;

    /* GPU pipeline for distorted diff. */
    err = run_gpu_pipeline_st(s, cu_f, s->h_dis[other], s->d_indterm_dis, plane_op_bytes);
    if (err)
        goto pop_ctx;

    /* Restore reference cov (SpEED uses reference basis for both). */
    (void)memcpy(s->h_cov_mat, saved_cov, sizeof(saved_cov));

    err = run_cpu_linalg_st(s, cu_f, s->h_indterm_dis, s->d_sol_dis);
    if (err)
        goto pop_ctx;

    float score = 0.0f;
    err = run_score_st(s, cu_f, &score);
    if (err)
        goto pop_ctx;

    CHECK_CUDA_GOTO(cu_f, cuCtxPopCurrent(NULL), fail);

    const double clipped =
        (double)score < s->speed_temporal_max_val ? (double)score : s->speed_temporal_max_val;

    err = vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                  "Speed_temporal_feature_speed_temporal_score",
                                                  clipped, index);
    return err;

pop_ctx:
    (void)cu_f->cuCtxPopCurrent(NULL);
    return err;
fail:
    return -EIO;
}

static int close_fex_st(VmafFeatureExtractor *fex)
{
    SpeedTemporalCudaState *s = fex->priv;
    if (fex->cu_state && fex->cu_state->f) {
        free_cuda_buffers_st(s, fex->cu_state->f);
        if (s->stream)
            (void)fex->cu_state->f->cuStreamDestroy(s->stream);
    }
    if (s->feature_name_dict)
        vmaf_dictionary_free(&s->feature_name_dict);
    if (fex->cu_state && fex->cu_state->f && s->module)
        (void)fex->cu_state->f->cuModuleUnload(s->module);
    return 0;
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
