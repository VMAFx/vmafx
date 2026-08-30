/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  speed_chroma feature extractor — CUDA backend with real on-device
 *  GPU kernels (ADR-0567).
 *
 *  Algorithm split (see ADR-0567 and speed_internal.h for rationale):
 *
 *  CPU (per channel, per frame):
 *    1. picture_copy  — convert raw chroma plane to float.
 *    2. speed_internal_filter_and_downscale — anti-alias Gaussian filter
 *       + 2^4 decimation + local mean subtraction (uses existing VIF CPU
 *       routines; result is a small operating-resolution float plane).
 *    3. Upload operating-resolution float plane to device (async H2D).
 *    4. Eigendecomposition (25×25 QR iteration, ~50µs) and QR factorize
 *       of cov matrix — after D2H of 625-float covariance matrix.
 *    5. Multiply Q^T × indterm (25×25 × 25×num_blocks, CPU).
 *    6. Aggregate per-tile scores to frame score (CPU).
 *
 *  GPU (per channel, per frame):
 *    K1. speed_means_kernel   — mean per (element, tile): [25 × num_blocks]
 *    K2. speed_cov_kernel     — covariance matrix: 625 blocks × 256 threads
 *    K3. speed_indterm_kernel — independent term: [25 × num_blocks]
 *    D2H: download cov_mat (625 × 4 bytes = 2.5 KB)
 *    K4. speed_solve_kernel   — backward substitution: num_blocks columns
 *        (after H2D upload of R [25×25] and Q^T×indterm [25×num_blocks])
 *    K5. speed_score_kernel   — per-tile entropy + score: num_blocks threads
 *    D2H: download ref/dis entropies, variances (4 × num_blocks × 4 bytes)
 *
 *  Numerical contract (ADR-0214 / ADR-0567):
 *    places=4 vs CPU reference (bit-exact CPU path differs only in
 *    double-precision accumulator for cov-matrix vs GPU's 64-bit shared mem).
 *    Full float32 path in score kernel.
 *
 *  Output features: Speed_chroma_feature_speed_chroma_{u,v,uv}_score.
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
#include "cuda/speed_chroma_cuda.h"

/* ------------------------------------------------------------------ */
/* Embedded PTX from speed_score.cu (generated at build time)          */
/* ------------------------------------------------------------------ */
extern const char speed_score_ptx[];

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

#define SC_BLOCK_SIZE (5u)
#define SC_ELEMENTS (SC_BLOCK_SIZE * SC_BLOCK_SIZE) /* 25 */
#define SC_COV_BLOCK (256u)
#define SC_MEANS_BLOCK (256u)
#define SC_INDTERM_BLOCK (256u)
#define SC_SCORE_BLOCK (256u)
#define SC_SOLVE_WARP (32u)

#define SC_DEFAULT_SIGMA_NN (0.29)
#define SC_DEFAULT_MAX_VAL (1000.0)
#define SC_DEFAULT_NN_FLOOR (0.0)
#define SC_DEFAULT_KERNELSCALE (1.0)
#define SC_DEFAULT_PRESCALE (1.0)
#define SC_DEFAULT_PRESCALE_METHOD ("nearest")

/* ------------------------------------------------------------------ */
/* Private extractor state                                             */
/* ------------------------------------------------------------------ */

typedef struct SpeedChromaCudaState {
    /* CUDA kernel function handles. */
    CUfunction func_means;
    CUfunction func_cov;
    CUfunction func_indterm;
    CUfunction func_solve;
    CUfunction func_score;

    /* CUDA stream (private to this extractor). */
    CUstream stream;

    /* SpEED algorithm dimensions (filled at init). */
    SpeedInternalDimensions dim;
    SpeedInternalOptions opt;
    size_t float_stride; /* operating plane stride in bytes */

    /* Device buffers (per channel — reallocated for U and V separately
     * since they have the same dimensions in 4:2:0). */
    CUdeviceptr d_plane;           /* downscaled float plane [op_h × stride_px] */
    CUdeviceptr d_means;           /* [25 × num_blocks] float */
    CUdeviceptr d_cov_mat;         /* [25 × 25] float */
    CUdeviceptr d_indterm_ref;     /* [25 × num_blocks] float */
    CUdeviceptr d_indterm_dis;     /* [25 × num_blocks] float */
    CUdeviceptr d_sol_ref;         /* [25 × num_blocks] float (Q^T×indterm) */
    CUdeviceptr d_sol_dis;         /* [25 × num_blocks] float */
    CUdeviceptr d_R;               /* [25 × 25] float (uploaded from CPU QR) */
    CUdeviceptr d_eigenvalues;     /* [25] float (uploaded from CPU eig — holds
                                  *  dis eigenvalues after the dis linalg pass) */
    CUdeviceptr d_eigenvalues_ref; /* [25] float (ref eigenvalues, copied aside
                                    *  before the dis linalg overwrites the
                                    *  shared d_eigenvalues buffer) */
    CUdeviceptr d_ref_entropies;   /* [num_blocks] float */
    CUdeviceptr d_ref_variances;   /* [num_blocks] float */
    CUdeviceptr d_dis_entropies;   /* [num_blocks] float */
    CUdeviceptr d_dis_variances;   /* [num_blocks] float */

    /* Pinned host staging for cov_mat D2H (avoids paged copy latency). */
    float *h_cov_mat; /* [625] float, cuMemHostAlloc (pinned) */

    /* Host-side CPU float plane buffer (for picture_copy → filter → H2D). */
    float *h_plane_ref; /* alloc_height × stride_px floats */
    float *h_plane_dis;

    /* CPU scratch for eigendecomp and QR factorization. */
    float *h_eigenvalues; /* [25] */
    float *h_eig_scratch; /* [25² + 3×25] = 700 floats */
    float *h_Q;           /* [25 × 25] */
    float *h_R;           /* [25 × 25] */
    float *h_qr_scratch;  /* [3 × 25²] = 1875 floats */
    float *h_indterm_ref; /* [25 × num_blocks] — for Qt multiply */
    float *h_indterm_dis;
    float *h_qt_scratch; /* [25 × max_num_blocks] */

    /* Result readback from device (pinned). */
    float *h_ref_entropies; /* [num_blocks] */
    float *h_ref_variances;
    float *h_dis_entropies;
    float *h_dis_variances;

    /* Per-channel scoring options. */
    double speed_chroma_kernelscale;
    double speed_chroma_prescale;
    char *speed_chroma_prescale_method;
    double speed_chroma_sigma_nn;
    double speed_chroma_nn_floor;
    double speed_chroma_max_val;
    int speed_weight_var_mode;

    /* PTX module backing the SpEED chroma kernels — owned here so
     * `close_fex_cuda` can unload it. Skipping the unload leaks
     * ~200-500 KB of GPU-resident PTX backing store per vmaf_close(). */
    CUmodule module;
    VmafDictionary *feature_name_dict;
} SpeedChromaCudaState;

/* ------------------------------------------------------------------ */
/* Option table                                                        */
/* ------------------------------------------------------------------ */

static const VmafOption options[] = {
    {
        .name = "speed_kernelscale",
        .help = "scaling factor for the Gaussian kernel",
        .offset = offsetof(SpeedChromaCudaState, speed_chroma_kernelscale),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = SC_DEFAULT_KERNELSCALE,
        .min = 0.1,
        .max = 4.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "ks",
    },
    {
        .name = "speed_prescale",
        .help = "scaling factor for the frame",
        .offset = offsetof(SpeedChromaCudaState, speed_chroma_prescale),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = SC_DEFAULT_PRESCALE,
        .min = 0.1,
        .max = 4.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "ps",
    },
    {
        .name = "speed_prescale_method",
        .help = "scaling method [nearest, bilinear, bicubic, lanczos4]",
        .offset = offsetof(SpeedChromaCudaState, speed_chroma_prescale_method),
        .type = VMAF_OPT_TYPE_STRING,
        .default_val.s = SC_DEFAULT_PRESCALE_METHOD,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "psm",
    },
    {
        .name = "speed_sigma_nn",
        .help = "standard deviation of neural noise",
        .offset = offsetof(SpeedChromaCudaState, speed_chroma_sigma_nn),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = SC_DEFAULT_SIGMA_NN,
        .min = 0.1,
        .max = 2.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "snn",
    },
    {
        .name = "speed_nn_floor",
        .help = "neural noise floor fraction",
        .offset = offsetof(SpeedChromaCudaState, speed_chroma_nn_floor),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = SC_DEFAULT_NN_FLOOR,
        .min = 0.0,
        .max = 1.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "nnf",
    },
    {
        .name = "speed_max_val",
        .help = "clip output to this maximum",
        .offset = offsetof(SpeedChromaCudaState, speed_chroma_max_val),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = SC_DEFAULT_MAX_VAL,
        .min = 0.0,
        .max = 1000.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "mxv",
    },
    {
        .name = "speed_weight_var_mode",
        .help = "variance weighting mode (0-6)",
        .offset = offsetof(SpeedChromaCudaState, speed_weight_var_mode),
        .type = VMAF_OPT_TYPE_INT,
        .default_val.d = 0,
        .min = 0,
        .max = 6,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "wvm",
    },
    {0},
};

/* ------------------------------------------------------------------ */
/* Free all device and pinned-host buffers                            */
/* ------------------------------------------------------------------ */

static void free_cuda_buffers(SpeedChromaCudaState *s, CudaFunctions *cu_f)
{
#define FREE_DPTR(p)                                                                               \
    do {                                                                                           \
        if ((p)) {                                                                                 \
            (void)cu_f->cuMemFree((p));                                                            \
            (p) = 0;                                                                               \
        }                                                                                          \
    } while (0)
#define FREE_HOST(p)                                                                               \
    do {                                                                                           \
        if ((p)) {                                                                                 \
            (void)cu_f->cuMemFreeHost((p));                                                        \
            (p) = NULL;                                                                            \
        }                                                                                          \
    } while (0)

    FREE_DPTR(s->d_plane);
    FREE_DPTR(s->d_means);
    FREE_DPTR(s->d_cov_mat);
    FREE_DPTR(s->d_indterm_ref);
    FREE_DPTR(s->d_indterm_dis);
    FREE_DPTR(s->d_sol_ref);
    FREE_DPTR(s->d_sol_dis);
    FREE_DPTR(s->d_R);
    FREE_DPTR(s->d_eigenvalues);
    FREE_DPTR(s->d_eigenvalues_ref);
    FREE_DPTR(s->d_ref_entropies);
    FREE_DPTR(s->d_ref_variances);
    FREE_DPTR(s->d_dis_entropies);
    FREE_DPTR(s->d_dis_variances);

    FREE_HOST(s->h_cov_mat);
    FREE_HOST(s->h_ref_entropies);
    FREE_HOST(s->h_ref_variances);
    FREE_HOST(s->h_dis_entropies);
    FREE_HOST(s->h_dis_variances);

#undef FREE_DPTR
#undef FREE_HOST

    if (s->h_plane_ref) {
        aligned_free(s->h_plane_ref);
        s->h_plane_ref = NULL;
    }
    if (s->h_plane_dis) {
        aligned_free(s->h_plane_dis);
        s->h_plane_dis = NULL;
    }
    if (s->h_eigenvalues) {
        aligned_free(s->h_eigenvalues);
        s->h_eigenvalues = NULL;
    }
    if (s->h_eig_scratch) {
        aligned_free(s->h_eig_scratch);
        s->h_eig_scratch = NULL;
    }
    if (s->h_Q) {
        aligned_free(s->h_Q);
        s->h_Q = NULL;
    }
    if (s->h_R) {
        aligned_free(s->h_R);
        s->h_R = NULL;
    }
    if (s->h_qr_scratch) {
        aligned_free(s->h_qr_scratch);
        s->h_qr_scratch = NULL;
    }
    if (s->h_indterm_ref) {
        aligned_free(s->h_indterm_ref);
        s->h_indterm_ref = NULL;
    }
    if (s->h_indterm_dis) {
        aligned_free(s->h_indterm_dis);
        s->h_indterm_dis = NULL;
    }
    if (s->h_qt_scratch) {
        aligned_free(s->h_qt_scratch);
        s->h_qt_scratch = NULL;
    }
}

/* ------------------------------------------------------------------ */
/* GPU kernel: run covariance + indterm pipeline for one plane         */
/* ------------------------------------------------------------------ */

static int run_gpu_pipeline(SpeedChromaCudaState *s, CudaFunctions *cu_f, CUdeviceptr d_plane,
                            CUdeviceptr d_indterm, float *h_plane, size_t plane_op_bytes)
{
    /* Upload operating-resolution plane to device (synchronous for simplicity;
     * the CPU eigendecomp below is the latency-hiding opportunity). */
    int _cuda_err = 0;
    CHECK_CUDA_GOTO(cu_f, cuMemcpyHtoDAsync(d_plane, h_plane, plane_op_bytes, s->stream), fail);

    const uint32_t num_blocks = (uint32_t)s->dim.num_blocks;
    const uint32_t num_blocks_h = (uint32_t)s->dim.num_blocks_horizontal;
    const uint32_t op_w = (uint32_t)s->dim.truncated_width;
    const uint32_t stride_px = (uint32_t)(s->float_stride / sizeof(float));
    const uint32_t submatrix_w = (uint32_t)s->dim.submatrix_width;
    const uint32_t submatrix_h = (uint32_t)s->dim.submatrix_height;

    /* --- Kernel 1: means ------------------------------------------ */
    {
        const uint32_t grid_x = (num_blocks + SC_MEANS_BLOCK - 1u) / SC_MEANS_BLOCK;
        void *args[] = {(void *)&d_plane,     (void *)&s->d_means,   (void *)&op_w,
                        (void *)&stride_px,   (void *)&num_blocks_h, (void *)&num_blocks,
                        (void *)&submatrix_w, (void *)&submatrix_h};
        CHECK_CUDA_GOTO(cu_f,
                        cuLaunchKernel(s->func_means, grid_x, 1u, 1u, SC_MEANS_BLOCK, 1u, 1u, 0u,
                                       s->stream, args, NULL),
                        fail);
    }

    /* --- Kernel 2: covariance matrix (625 blocks × COV_BLOCK threads) */
    {
        void *args[] = {(void *)&d_plane,     (void *)&s->d_means,   (void *)&s->d_cov_mat,
                        (void *)&stride_px,   (void *)&num_blocks_h, (void *)&num_blocks,
                        (void *)&submatrix_w, (void *)&submatrix_h};
        CHECK_CUDA_GOTO(cu_f,
                        cuLaunchKernel(s->func_cov, SC_ELEMENTS, SC_ELEMENTS, 1u, SC_COV_BLOCK, 1u,
                                       1u, 0u, s->stream, args, NULL),
                        fail);
    }

    /* --- Kernel 3: independent term -------------------------------- */
    {
        const uint32_t total = SC_ELEMENTS * num_blocks;
        const uint32_t grid_x = (total + SC_INDTERM_BLOCK - 1u) / SC_INDTERM_BLOCK;
        void *args[] = {(void *)&d_plane, (void *)&d_indterm, (void *)&stride_px,
                        (void *)&num_blocks_h, (void *)&num_blocks};
        CHECK_CUDA_GOTO(cu_f,
                        cuLaunchKernel(s->func_indterm, grid_x, 1u, 1u, SC_INDTERM_BLOCK, 1u, 1u,
                                       0u, s->stream, args, NULL),
                        fail);
    }

    /* Sync to get cov_mat back to CPU for eigendecomp. */
    CHECK_CUDA_GOTO(cu_f,
                    cuMemcpyDtoHAsync(s->h_cov_mat, s->d_cov_mat,
                                      SC_ELEMENTS * SC_ELEMENTS * sizeof(float), s->stream),
                    fail);
    CHECK_CUDA_GOTO(cu_f, cuStreamSynchronize(s->stream), fail);

    /* Download indterm (needed for Qt multiply on CPU). */
    const size_t indterm_bytes = (size_t)SC_ELEMENTS * num_blocks * sizeof(float);
    CHECK_CUDA_GOTO(
        cu_f,
        cuMemcpyDtoHAsync((void *)(uintptr_t)((d_indterm == s->d_indterm_ref) ? s->h_indterm_ref :
                                                                                s->h_indterm_dis),
                          d_indterm, indterm_bytes, s->stream),
        fail);
    CHECK_CUDA_GOTO(cu_f, cuStreamSynchronize(s->stream), fail);

    return 0;

fail:
    return _cuda_err;
}

/* ------------------------------------------------------------------ */
/* CPU eigendecomp + QR-factorize + Qt multiply for one plane         */
/* ------------------------------------------------------------------ */

static int run_cpu_linalg(SpeedChromaCudaState *s, CudaFunctions *cu_f, float *h_indterm,
                          CUdeviceptr d_sol, CUdeviceptr *d_indterm_ptr)
{
    const int sz = (int)SC_ELEMENTS;
    const int nb = (int)s->dim.num_blocks;
    int _cuda_err = 0;

    /* Eigendecomp of the 25×25 covariance matrix. */
    speed_internal_compute_eigenvalues(s->h_cov_mat, s->h_eigenvalues, sz, s->h_eig_scratch);

    /* Check regularity — if singular, zero out the solution. */
    bool regular = speed_internal_is_matrix_regular(s->h_eigenvalues, (size_t)sz);
    if (!regular) {
        vmaf_log(VMAF_LOG_LEVEL_WARNING,
                 "speed_chroma_cuda: covariance matrix singular, zeroing solution\n");
        (void)memset(h_indterm, 0, (size_t)sz * (size_t)nb * sizeof(float));
    } else {
        /* QR factorize cov_mat → Q, R. */
        (void)speed_internal_qr_factorize(s->h_cov_mat, sz, s->h_Q, s->h_R, s->h_qr_scratch);

        /* Q^T × indterm → solution (in-place in h_indterm). */
        speed_internal_qt_multiply(s->h_Q, h_indterm, sz, nb, s->h_qt_scratch);

        /* Upload R and Q^T×indterm to device for GPU backward substitution. */
        CHECK_CUDA_GOTO(
            cu_f,
            cuMemcpyHtoDAsync(s->d_R, s->h_R, (size_t)sz * (size_t)sz * sizeof(float), s->stream),
            fail);
        CHECK_CUDA_GOTO(
            cu_f,
            cuMemcpyHtoDAsync(d_sol, h_indterm, (size_t)sz * (size_t)nb * sizeof(float), s->stream),
            fail);

        /* GPU Kernel 4: backward substitution. */
        const uint32_t u_nb = (uint32_t)nb;
        const uint32_t warps_per_block = (u_nb + 7u) / 8u;
        const uint32_t threads = warps_per_block * SC_SOLVE_WARP;
        const uint32_t blocks = (u_nb + (threads / SC_SOLVE_WARP) - 1u) / (threads / SC_SOLVE_WARP);
        void *args[] = {(void *)&s->d_R, (void *)&d_sol, (void *)&u_nb};
        CHECK_CUDA_GOTO(cu_f,
                        cuLaunchKernel(s->func_solve, blocks, 1u, 1u, threads, 1u, 1u, 0u,
                                       s->stream, args, NULL),
                        fail);
        CHECK_CUDA_GOTO(cu_f, cuStreamSynchronize(s->stream), fail);
    }

    /* Upload eigenvalues to device for score kernel. */
    CHECK_CUDA_GOTO(cu_f,
                    cuMemcpyHtoDAsync(s->d_eigenvalues, s->h_eigenvalues,
                                      (size_t)sz * sizeof(float), s->stream),
                    fail);

    (void)d_indterm_ptr; /* unused but kept for symmetry */
    return 0;

fail:
    return _cuda_err;
}

/* ------------------------------------------------------------------ */
/* Run score kernel and aggregate to a single float score             */
/* ------------------------------------------------------------------ */

static int run_score_and_collect(SpeedChromaCudaState *s, CudaFunctions *cu_f, float *score_out)
{
    const uint32_t num_blocks = (uint32_t)s->dim.num_blocks;
    const float sigma_nn = (float)s->opt.speed_sigma_nn;
    int _cuda_err = 0;

    /* GPU Kernel 5: per-tile entropy + variance. */
    {
        const uint32_t grid = (num_blocks + SC_SCORE_BLOCK - 1u) / SC_SCORE_BLOCK;
        void *args[] = {
            (void *)&s->d_eigenvalues_ref, (void *)&s->d_eigenvalues,   (void *)&s->d_sol_ref,
            (void *)&s->d_sol_dis,         (void *)&s->d_indterm_ref,   (void *)&s->d_indterm_dis,
            (void *)&s->d_ref_entropies,   (void *)&s->d_ref_variances, (void *)&s->d_dis_entropies,
            (void *)&s->d_dis_variances,   (void *)&num_blocks,         (void *)&sigma_nn};
        CHECK_CUDA_GOTO(cu_f,
                        cuLaunchKernel(s->func_score, grid, 1u, 1u, SC_SCORE_BLOCK, 1u, 1u, 0u,
                                       s->stream, args, NULL),
                        fail);
    }

    /* D2H: four arrays of num_blocks floats. */
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

    /* CPU score aggregation (matches get_speed_score in speed.c). */
    const float base_entropy =
        (float)SC_ELEMENTS *
        (log2f((1.0f + (float)s->opt.speed_nn_floor) * (float)s->opt.speed_sigma_nn) +
         log2f(2.0f * 3.14159265358979323846f * 2.71828182845904523536f));

    float total = 0.0f;
    for (uint32_t i = 0; i < num_blocks; ++i) {
        float re = s->h_ref_entropies[i];
        float de = s->h_dis_entropies[i];
        if (re < base_entropy && de < base_entropy) {
            /* Both below noise floor — no visible difference. */
            continue;
        }
        float rv = s->h_ref_variances[i];
        float dv = s->h_dis_variances[i];
        float spatial_ref = 0.0f;
        float spatial_dis = 0.0f;
        const int wvm = s->opt.speed_weight_var_mode;
        if (wvm == 0) {
            spatial_ref = re * log2f(1.0f + rv);
            spatial_dis = de * log2f(1.0f + dv);
        } else if (wvm == 1) {
            spatial_ref = re * log2f(1.0f + rv);
            spatial_dis = de * log2f(1.0f + rv);
        } else if (wvm == 2) {
            spatial_ref = re * log2f(1.0f + dv);
            spatial_dis = de * log2f(1.0f + dv);
        } else if (wvm == 3) {
            float mv = (rv + dv) * 0.5f;
            spatial_ref = re * log2f(1.0f + mv);
            spatial_dis = de * log2f(1.0f + mv);
        } else if (wvm == 4) {
            spatial_ref = re * log2f(1.0f + rv);
            spatial_dis = de * log2f(1.0f + (rv + dv) * 0.5f);
        } else if (wvm == 5) {
            spatial_ref = re * log2f(1.0f + rv);
            spatial_dis = de * log2f(1.0f + 0.75f * rv + 0.25f * dv);
        } else if (wvm == 6) {
            spatial_ref = re * log2f(1.0f + rv);
            spatial_dis = de * log2f(1.0f + 0.25f * rv + 0.75f * dv);
        }
        total += fabsf(spatial_ref - spatial_dis);
    }

    *score_out = total / (float)num_blocks;
    return 0;

fail:
    return _cuda_err;
}

/* ------------------------------------------------------------------ */
/* Extract one channel (U or V) and return the score                   */
/* ------------------------------------------------------------------ */

static int extract_channel(SpeedChromaCudaState *s, CudaFunctions *cu_f, VmafPicture *ref_pic,
                           VmafPicture *dist_pic, int channel, float *score_out)
{
    /* Allocate a local tmp buffer for filter_and_downscale. */
    const size_t stride_px = s->float_stride / sizeof(float);
    const size_t tmp_size = 2u * s->dim.alloc_height * stride_px;
    float *tmp_filter = (float *)aligned_malloc(tmp_size * sizeof(float), 32);
    if (!tmp_filter)
        return -ENOMEM;

    int err = 0;

    /* The CUDA pipeline feeds DEVICE-resident pictures (ref_pic->data[] are
     * CUdeviceptr, as in adm/vif_cuda), but speed_chroma runs its Gaussian
     * filter on the CPU, so the picture_copy() calls below read HOST memory.
     * Download the chroma plane to host staging first — reading the device
     * pointer on the host SEGVs. CUDA tests don't run in CI (no GPU runner),
     * so this was never caught. */
    const size_t raw_ref_bytes = (size_t)ref_pic->h[channel] * ref_pic->stride[channel];
    const size_t raw_dis_bytes = (size_t)dist_pic->h[channel] * dist_pic->stride[channel];
    uint8_t *raw_ref = (uint8_t *)aligned_malloc(raw_ref_bytes, 32);
    uint8_t *raw_dis = (uint8_t *)aligned_malloc(raw_dis_bytes, 32);
    if (!raw_ref || !raw_dis) {
        aligned_free(raw_ref);
        aligned_free(raw_dis);
        aligned_free(tmp_filter);
        return -ENOMEM;
    }
    if (cu_f->cuMemcpyDtoH(raw_ref, (CUdeviceptr)ref_pic->data[channel], raw_ref_bytes) !=
            CUDA_SUCCESS ||
        cu_f->cuMemcpyDtoH(raw_dis, (CUdeviceptr)dist_pic->data[channel], raw_dis_bytes) !=
            CUDA_SUCCESS) {
        aligned_free(raw_ref);
        aligned_free(raw_dis);
        aligned_free(tmp_filter);
        return -EIO;
    }
    VmafPicture host_ref = *ref_pic;
    VmafPicture host_dis = *dist_pic;
    host_ref.data[channel] = raw_ref;
    host_dis.data[channel] = raw_dis;

    /* Reference plane: CPU copy + filter + downscale. */
    picture_copy(s->h_plane_ref, s->float_stride, &host_ref, -128, ref_pic->bpc, channel);
    speed_internal_filter_and_downscale(&s->dim, &s->opt, s->h_plane_ref, tmp_filter,
                                        s->float_stride);

    /* Distorted plane: CPU copy + filter + downscale. */
    picture_copy(s->h_plane_dis, s->float_stride, &host_dis, -128, dist_pic->bpc, channel);
    speed_internal_filter_and_downscale(&s->dim, &s->opt, s->h_plane_dis, tmp_filter,
                                        s->float_stride);

    aligned_free(raw_ref);
    aligned_free(raw_dis);
    aligned_free(tmp_filter);
    tmp_filter = NULL;

    /* Operating-resolution plane size in bytes (only the valid region). */
    const size_t plane_op_bytes = (size_t)s->dim.truncated_height * stride_px * sizeof(float);

    /* GPU pipeline: means → cov → indterm for reference. */
    err = run_gpu_pipeline(s, cu_f, s->d_plane, s->d_indterm_ref, s->h_plane_ref, plane_op_bytes);
    if (err)
        return err;

    /* CPU eigendecomp + QR for reference. Uploads ref eigenvalues into the
     * shared s->d_eigenvalues buffer. */
    err = run_cpu_linalg(s, cu_f, s->h_indterm_ref, s->d_sol_ref, NULL);
    if (err)
        return err;

    /* Stash the reference eigenvalues aside before the distorted linalg pass
     * overwrites s->d_eigenvalues. The CPU reference (est_params in speed.c)
     * computes SEPARATE ref and dis covariance + eigenvalues; the score kernel
     * needs both. A synchronous DtoD copy is ordered against the prior async
     * eigenvalue H2D on the same stream (run_cpu_linalg cuStreamSynchronize'd
     * before returning when regular; the singular-path skips the solve but
     * still uploads eigenvalues async — synchronize to be safe). */
    CHECK_CUDA_RETURN(cu_f, cuStreamSynchronize(s->stream));
    CHECK_CUDA_RETURN(
        cu_f, cuMemcpyDtoD(s->d_eigenvalues_ref, s->d_eigenvalues, SC_ELEMENTS * sizeof(float)));

    /* GPU pipeline: means → cov → indterm for distorted (keeps the DIS
     * covariance in h_cov_mat — no save/restore of the ref covariance). */
    err = run_gpu_pipeline(s, cu_f, s->d_plane, s->d_indterm_dis, s->h_plane_dis, plane_op_bytes);
    if (err)
        return err;

    /* CPU eigendecomp + QR for distorted (uses the DIS cov_mat). Uploads dis
     * eigenvalues into s->d_eigenvalues. */
    err = run_cpu_linalg(s, cu_f, s->h_indterm_dis, s->d_sol_dis, NULL);
    if (err)
        return err;

    /* GPU score kernel → aggregate → score_out. The kernel reads
     * d_eigenvalues_ref for the ref entropy and d_eigenvalues (now holding the
     * dis eigenvalues) for the dis entropy. */
    return run_score_and_collect(s, cu_f, score_out);
}

/* ------------------------------------------------------------------ */
/* Extractor lifecycle                                                 */
/* ------------------------------------------------------------------ */

static int init_fex_cuda(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                         unsigned w, unsigned h)
{
    (void)bpc;

    /* Derive chroma plane dimensions from luma dimensions + pixel format. */
    unsigned cw = w;
    unsigned ch = h;
    switch (pix_fmt) {
    case VMAF_PIX_FMT_UNKNOWN:
    case VMAF_PIX_FMT_YUV400P:
        return -EINVAL;
    case VMAF_PIX_FMT_YUV420P:
        cw /= 2u;
        ch /= 2u;
        break;
    case VMAF_PIX_FMT_YUV422P:
        cw /= 2u;
        break;
    case VMAF_PIX_FMT_YUV444P:
        break;
    }

    SpeedChromaCudaState *s = fex->priv;
    CudaFunctions *cu_f = fex->cu_state->f;
    int _cuda_err = 0;
    int err = 0;

    /* Fill options struct from extractor options (set by the option table). */
    s->opt = (SpeedInternalOptions){
        .speed_kernelscale = s->speed_chroma_kernelscale,
        .speed_prescale = s->speed_chroma_prescale,
        .speed_prescale_method = s->speed_chroma_prescale_method,
        .speed_sigma_nn = s->speed_chroma_sigma_nn,
        .speed_nn_floor = s->speed_chroma_nn_floor,
        .speed_weight_var_mode = s->speed_weight_var_mode,
    };

    /* Compute SpEED dimensions. */
    err = speed_internal_init_dimensions(&s->dim, (int)cw, (int)ch, s->opt.speed_prescale);
    if (err)
        return err;

    s->float_stride = speed_internal_float_stride(s->dim.alloc_width);

    const size_t stride_px = s->float_stride / sizeof(float);
    const size_t num_blocks = s->dim.num_blocks;
    const size_t plane_alloc = s->dim.alloc_height * stride_px * sizeof(float);
    const size_t indterm_bytes = SC_ELEMENTS * num_blocks * sizeof(float);
    const size_t cov_bytes = SC_ELEMENTS * SC_ELEMENTS * sizeof(float);

    /* Load PTX and get kernel function handles. */
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

    /* Allocate device buffers. */
    const size_t score_bytes = num_blocks * sizeof(float);
#define ALLOC_DPTR(field, sz)                                                                      \
    do {                                                                                           \
        CHECK_CUDA_GOTO(cu_f, cuMemAlloc(&(s->field), (sz)), fail_pop);                            \
    } while (0)

    ALLOC_DPTR(d_plane, plane_alloc);
    ALLOC_DPTR(d_means, indterm_bytes);
    ALLOC_DPTR(d_cov_mat, cov_bytes);
    ALLOC_DPTR(d_indterm_ref, indterm_bytes);
    ALLOC_DPTR(d_indterm_dis, indterm_bytes);
    ALLOC_DPTR(d_sol_ref, indterm_bytes);
    ALLOC_DPTR(d_sol_dis, indterm_bytes);
    ALLOC_DPTR(d_R, cov_bytes);
    ALLOC_DPTR(d_eigenvalues, SC_ELEMENTS * sizeof(float));
    ALLOC_DPTR(d_eigenvalues_ref, SC_ELEMENTS * sizeof(float));
    ALLOC_DPTR(d_ref_entropies, score_bytes);
    ALLOC_DPTR(d_ref_variances, score_bytes);
    ALLOC_DPTR(d_dis_entropies, score_bytes);
    ALLOC_DPTR(d_dis_variances, score_bytes);
#undef ALLOC_DPTR

    /* Allocate pinned host buffers for D2H. */
#define ALLOC_HOST(field, sz)                                                                      \
    do {                                                                                           \
        CHECK_CUDA_GOTO(cu_f, cuMemHostAlloc((void **)&(s->field), (sz), 0x01u), fail_pop);        \
    } while (0)

    ALLOC_HOST(h_cov_mat, cov_bytes);
    ALLOC_HOST(h_ref_entropies, score_bytes);
    ALLOC_HOST(h_ref_variances, score_bytes);
    ALLOC_HOST(h_dis_entropies, score_bytes);
    ALLOC_HOST(h_dis_variances, score_bytes);
#undef ALLOC_HOST

    CHECK_CUDA_GOTO(cu_f, cuCtxPopCurrent(NULL), fail_after_pop);

    /* Allocate CPU-side buffers (aligned for SIMD). */
    s->h_plane_ref = (float *)aligned_malloc(plane_alloc, 32);
    s->h_plane_dis = (float *)aligned_malloc(plane_alloc, 32);
    s->h_eigenvalues = (float *)aligned_malloc(SC_ELEMENTS * sizeof(float), 32);
    /* Eigendecomp scratch: size² + 3×size floats = 625 + 75 = 700 floats. */
    s->h_eig_scratch =
        (float *)aligned_malloc((SC_ELEMENTS * SC_ELEMENTS + 4u * SC_ELEMENTS) * sizeof(float), 32);
    s->h_Q = (float *)aligned_malloc(cov_bytes, 32);
    s->h_R = (float *)aligned_malloc(cov_bytes, 32);
    /* QR scratch: 3 × size² = 1875 floats (+ 1 for copy of A) = 4 × 625. */
    s->h_qr_scratch = (float *)aligned_malloc(4u * cov_bytes, 32);
    s->h_indterm_ref = (float *)aligned_malloc(indterm_bytes, 32);
    s->h_indterm_dis = (float *)aligned_malloc(indterm_bytes, 32);
    s->h_qt_scratch = (float *)aligned_malloc(indterm_bytes, 32);

    if (!s->h_plane_ref || !s->h_plane_dis || !s->h_eigenvalues || !s->h_eig_scratch || !s->h_Q ||
        !s->h_R || !s->h_qr_scratch || !s->h_indterm_ref || !s->h_indterm_dis || !s->h_qt_scratch) {
        err = -ENOMEM;
        goto free_cpu;
    }

    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (!s->feature_name_dict) {
        err = -ENOMEM;
        goto free_cpu;
    }

    return 0;

free_cpu:
    free_cuda_buffers(s, cu_f);
    return err;

fail_after_pop:
    (void)cu_f->cuCtxPopCurrent(NULL);
    free_cuda_buffers(s, cu_f);
    return _cuda_err;

fail_pop:
    (void)cu_f->cuCtxPopCurrent(NULL);
    free_cuda_buffers(s, cu_f);
    return _cuda_err;

fail:
    return _cuda_err;
}

static int extract_fex_cuda(VmafFeatureExtractor *fex, VmafPicture *ref_pic,
                            VmafPicture *ref_pic_90, VmafPicture *dist_pic,
                            VmafPicture *dist_pic_90, unsigned index,
                            VmafFeatureCollector *feature_collector)
{
    (void)ref_pic_90;
    (void)dist_pic_90;

    SpeedChromaCudaState *s = fex->priv;
    CudaFunctions *cu_f = fex->cu_state->f;
    int err = 0;
    int _cuda_err = 0;

    CHECK_CUDA_GOTO(cu_f, cuCtxPushCurrent(fex->cu_state->ctx), fail);

    float score_u = 0.0f;
    float score_v = 0.0f;
    int err_u = extract_channel(s, cu_f, ref_pic, dist_pic, 1, &score_u);
    int err_v = extract_channel(s, cu_f, ref_pic, dist_pic, 2, &score_v);

    CHECK_CUDA_GOTO(cu_f, cuCtxPopCurrent(NULL), fail_after_push);

    /* Singular-matrix imputation: if one channel is singular, use the other. */
    float score_uv;
    if (err_u && !err_v)
        score_uv = score_v;
    else if (err_v && !err_u)
        score_uv = score_u;
    else
        score_uv = (score_u + score_v) * 0.5f;

    const double mxv = s->speed_chroma_max_val;
#define CLIP(x) ((double)(x) < (mxv) ? (double)(x) : (mxv))

    err |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "Speed_chroma_feature_speed_chroma_u_score",
                                                   CLIP(score_u), index);
    err |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "Speed_chroma_feature_speed_chroma_v_score",
                                                   CLIP(score_v), index);
    err |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "Speed_chroma_feature_speed_chroma_uv_score",
                                                   CLIP(score_uv), index);
#undef CLIP

    return err;

fail_after_push:
    /* The context was pushed but the pop failed: attempt the pop once more so
     * the CUDA context stack is not left unbalanced (a per-frame leak that
     * eventually exhausts the stack), then propagate the original error.
     * Mirrors the fail_after_pop pattern in init_fex_cuda. */
    (void)cu_f->cuCtxPopCurrent(NULL);
fail:
    return _cuda_err;
}

static int close_fex_cuda(VmafFeatureExtractor *fex)
{
    SpeedChromaCudaState *s = fex->priv;
    if (fex->cu_state && fex->cu_state->f)
        free_cuda_buffers(s, fex->cu_state->f);
    if (s->feature_name_dict)
        vmaf_dictionary_free(&s->feature_name_dict);
    if (s->stream && fex->cu_state && fex->cu_state->f)
        (void)fex->cu_state->f->cuStreamDestroy(s->stream);
    if (fex->cu_state && fex->cu_state->f && s->module)
        (void)fex->cu_state->f->cuModuleUnload(s->module);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Feature extractor registration                                      */
/* ------------------------------------------------------------------ */

static const char *provided_features[] = {
    "Speed_chroma_feature_speed_chroma_u_score",
    "Speed_chroma_feature_speed_chroma_v_score",
    "Speed_chroma_feature_speed_chroma_uv_score",
    NULL,
};

/* ADR-0567: VMAF_FEATURE_EXTRACTOR_CUDA flag ensures this extractor is
 * selected over the CPU twin on CUDA-backend sessions.  Real GPU kernels:
 * means, covariance, indterm, backward-substitution, score.  The 25×25
 * eigendecomp runs on CPU (unavoidable serial constraint). */
VmafFeatureExtractor vmaf_fex_speed_chroma_cuda = {
    .name = "speed_chroma_cuda",
    .init = init_fex_cuda,
    .extract = extract_fex_cuda,
    .close = close_fex_cuda,
    .options = options,
    .priv_size = sizeof(SpeedChromaCudaState),
    .provided_features = provided_features,
    .flags = VMAF_FEATURE_EXTRACTOR_CUDA,
};
