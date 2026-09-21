/**
 *  Copyright 2016-2025 Netflix, Inc.
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

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `NULL` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

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
/* Warps per backward-substitution block. 8 x 32 = 256 threads, comfortably
 * inside CUDA's 1024-thread block limit at every picture size. */
#define SC_SOLVE_WARPS_PER_BLOCK (8u)

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
    /* Singular covariance matrices are counted, not logged per solve. */
    SpeedInternalSingularTally singular_tally;
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

static void free_device_buffer(CUdeviceptr *buffer, CudaFunctions *cu_f)
{
    if (*buffer) {
        (void)cu_f->cuMemFree(*buffer);
        *buffer = 0;
    }
}

static void free_pinned_buffer(float **buffer, CudaFunctions *cu_f)
{
    if (*buffer) {
        (void)cu_f->cuMemFreeHost(*buffer);
        *buffer = NULL;
    }
}

static void free_aligned_buffer(float **buffer)
{
    if (*buffer) {
        aligned_free(*buffer);
        *buffer = NULL;
    }
}

static void free_cuda_buffers(SpeedChromaCudaState *s, CudaFunctions *cu_f)
{
    free_device_buffer(&s->d_plane, cu_f);
    free_device_buffer(&s->d_means, cu_f);
    free_device_buffer(&s->d_cov_mat, cu_f);
    free_device_buffer(&s->d_indterm_ref, cu_f);
    free_device_buffer(&s->d_indterm_dis, cu_f);
    free_device_buffer(&s->d_sol_ref, cu_f);
    free_device_buffer(&s->d_sol_dis, cu_f);
    free_device_buffer(&s->d_R, cu_f);
    free_device_buffer(&s->d_eigenvalues, cu_f);
    free_device_buffer(&s->d_eigenvalues_ref, cu_f);
    free_device_buffer(&s->d_ref_entropies, cu_f);
    free_device_buffer(&s->d_ref_variances, cu_f);
    free_device_buffer(&s->d_dis_entropies, cu_f);
    free_device_buffer(&s->d_dis_variances, cu_f);

    free_pinned_buffer(&s->h_cov_mat, cu_f);
    free_pinned_buffer(&s->h_ref_entropies, cu_f);
    free_pinned_buffer(&s->h_ref_variances, cu_f);
    free_pinned_buffer(&s->h_dis_entropies, cu_f);
    free_pinned_buffer(&s->h_dis_variances, cu_f);

    free_aligned_buffer(&s->h_plane_ref);
    free_aligned_buffer(&s->h_plane_dis);
    free_aligned_buffer(&s->h_eigenvalues);
    free_aligned_buffer(&s->h_eig_scratch);
    free_aligned_buffer(&s->h_Q);
    free_aligned_buffer(&s->h_R);
    free_aligned_buffer(&s->h_qr_scratch);
    free_aligned_buffer(&s->h_indterm_ref);
    free_aligned_buffer(&s->h_indterm_dis);
    free_aligned_buffer(&s->h_qt_scratch);
}

/* ------------------------------------------------------------------ */
/* GPU kernel: run covariance + indterm pipeline for one plane         */
/* ------------------------------------------------------------------ */

typedef struct SpeedCudaLaunchDimensions {
    uint32_t num_blocks;
    uint32_t num_blocks_horizontal;
    uint32_t operating_width;
    uint32_t stride_pixels;
    uint32_t submatrix_width;
    uint32_t submatrix_height;
} SpeedCudaLaunchDimensions;

static SpeedCudaLaunchDimensions get_launch_dimensions(const SpeedChromaCudaState *s)
{
    return (SpeedCudaLaunchDimensions){
        .num_blocks = (uint32_t)s->dim.num_blocks,
        .num_blocks_horizontal = (uint32_t)s->dim.num_blocks_horizontal,
        .operating_width = (uint32_t)s->dim.truncated_width,
        .stride_pixels = (uint32_t)(s->float_stride / sizeof(float)),
        .submatrix_width = (uint32_t)s->dim.submatrix_width,
        .submatrix_height = (uint32_t)s->dim.submatrix_height,
    };
}

static int launch_means_kernel(SpeedChromaCudaState *s, CudaFunctions *cu_f, CUdeviceptr d_plane,
                               const SpeedCudaLaunchDimensions *dim)
{
    const uint32_t grid_x = (dim->num_blocks + SC_MEANS_BLOCK - 1u) / SC_MEANS_BLOCK;
    void *args[] = {(void *)&d_plane,
                    (void *)&s->d_means,
                    (void *)&dim->operating_width,
                    (void *)&dim->stride_pixels,
                    (void *)&dim->num_blocks_horizontal,
                    (void *)&dim->num_blocks,
                    (void *)&dim->submatrix_width,
                    (void *)&dim->submatrix_height};

    CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(s->func_means, grid_x, 1u, 1u, SC_MEANS_BLOCK, 1u, 1u,
                                           0u, s->stream, args, NULL));
    return 0;
}

static int launch_covariance_kernel(SpeedChromaCudaState *s, CudaFunctions *cu_f,
                                    CUdeviceptr d_plane, const SpeedCudaLaunchDimensions *dim)
{
    void *args[] = {(void *)&d_plane,
                    (void *)&s->d_means,
                    (void *)&s->d_cov_mat,
                    (void *)&dim->stride_pixels,
                    (void *)&dim->num_blocks_horizontal,
                    (void *)&dim->num_blocks,
                    (void *)&dim->submatrix_width,
                    (void *)&dim->submatrix_height};

    CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(s->func_cov, SC_ELEMENTS, SC_ELEMENTS, 1u, SC_COV_BLOCK,
                                           1u, 1u, 0u, s->stream, args, NULL));
    return 0;
}

static int launch_indterm_kernel(SpeedChromaCudaState *s, CudaFunctions *cu_f, CUdeviceptr d_plane,
                                 CUdeviceptr d_indterm, const SpeedCudaLaunchDimensions *dim)
{
    const uint32_t total = SC_ELEMENTS * dim->num_blocks;
    const uint32_t grid_x = (total + SC_INDTERM_BLOCK - 1u) / SC_INDTERM_BLOCK;
    void *args[] = {(void *)&d_plane, (void *)&d_indterm, (void *)&dim->stride_pixels,
                    (void *)&dim->num_blocks_horizontal, (void *)&dim->num_blocks};

    CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(s->func_indterm, grid_x, 1u, 1u, SC_INDTERM_BLOCK, 1u,
                                           1u, 0u, s->stream, args, NULL));
    return 0;
}

static int download_pipeline_inputs(SpeedChromaCudaState *s, CudaFunctions *cu_f,
                                    CUdeviceptr d_indterm, uint32_t num_blocks)
{
    const size_t covariance_bytes = (size_t)SC_ELEMENTS * (size_t)SC_ELEMENTS * sizeof(float);
    const size_t indterm_bytes = (size_t)SC_ELEMENTS * (size_t)num_blocks * sizeof(float);
    float *const h_indterm = (d_indterm == s->d_indterm_ref) ? s->h_indterm_ref : s->h_indterm_dis;

    CHECK_CUDA_RETURN(cu_f,
                      cuMemcpyDtoHAsync(s->h_cov_mat, s->d_cov_mat, covariance_bytes, s->stream));
    CHECK_CUDA_RETURN(cu_f, cuMemcpyDtoHAsync(h_indterm, d_indterm, indterm_bytes, s->stream));
    CHECK_CUDA_RETURN(cu_f, cuStreamSynchronize(s->stream));
    return 0;
}

static int run_gpu_pipeline(SpeedChromaCudaState *s, CudaFunctions *cu_f, CUdeviceptr d_plane,
                            CUdeviceptr d_indterm, float *h_plane, size_t plane_op_bytes)
{
    const SpeedCudaLaunchDimensions dim = get_launch_dimensions(s);
    CHECK_CUDA_RETURN(cu_f, cuMemcpyHtoDAsync(d_plane, h_plane, plane_op_bytes, s->stream));

    int err = launch_means_kernel(s, cu_f, d_plane, &dim);
    if (err)
        return err;
    err = launch_covariance_kernel(s, cu_f, d_plane, &dim);
    if (err)
        return err;
    err = launch_indterm_kernel(s, cu_f, d_plane, d_indterm, &dim);
    if (err)
        return err;

    /* The copies are ordered on the same stream and share one synchronization. */
    return download_pipeline_inputs(s, cu_f, d_indterm, dim.num_blocks);
}

/* GPU Kernel 4: backward substitution, one warp per linear system and
 * SC_SOLVE_WARPS_PER_BLOCK warps per block.
 *
 * The BLOCK COUNT scales with the picture; the block size is fixed. Deriving
 * the block size from the system count instead pushes any launch above 256
 * systems past CUDA's 1024-thread block limit -- every 4K frame -- and the
 * kernel then never runs. See ADR-1202. */
static int launch_backward_substitution(SpeedChromaCudaState *s, CudaFunctions *cu_f,
                                        CUdeviceptr d_sol, uint32_t u_nb)
{
    const uint32_t warps_per_block =
        (u_nb < SC_SOLVE_WARPS_PER_BLOCK) ? (u_nb ? u_nb : 1u) : SC_SOLVE_WARPS_PER_BLOCK;
    const uint32_t threads = warps_per_block * SC_SOLVE_WARP;
    const uint32_t blocks = (u_nb + warps_per_block - 1u) / warps_per_block;
    void *args[] = {(void *)&s->d_R, (void *)&d_sol, (void *)&u_nb};

    CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(s->func_solve, blocks, 1u, 1u, threads, 1u, 1u, 0u,
                                           s->stream, args, NULL));
    /* No stall: the solution stays on the device and its only consumer, the
     * score kernel, is enqueued on this same stream, which orders it. */
    return 0;
}

/* uv from u and v, imputing across a singular channel exactly as extract_fex()
 * in speed.c does. */
static float combine_chroma_uv(float score_u, float score_v, bool singular_u, bool singular_v)
{
    if (singular_u && !singular_v)
        return score_v;
    if (singular_v && !singular_u)
        return score_u;
    return (score_u + score_v) * 0.5f;
}

/* ------------------------------------------------------------------ */
/* CPU eigendecomp + QR-factorize + Qt multiply for one plane         */
/* ------------------------------------------------------------------ */

static int run_cpu_linalg(SpeedChromaCudaState *s, CudaFunctions *cu_f, float *h_indterm,
                          CUdeviceptr d_sol, bool *singular_out)
{
    const int sz = (int)SC_ELEMENTS;
    const int nb = (int)s->dim.num_blocks;

    /* Eigendecomp of the 25×25 covariance matrix. */
    speed_internal_compute_eigenvalues(s->h_cov_mat, s->h_eigenvalues, sz, s->h_eig_scratch);

    /* Check regularity — if singular, zero out the solution. */
    bool regular = speed_internal_is_matrix_regular(s->h_eigenvalues, (size_t)sz);
    /* A singular covariance matrix is NOT a failure: the CPU reference zeroes
     * the solution and reports it separately so the caller can impute. The
     * return value stays reserved for hard failures. See ADR-1202. */
    *singular_out = !regular;
    speed_internal_tally_solve(&s->singular_tally, !regular, "speed_chroma_cuda");
    if (!regular) {
        /* Zero the DEVICE solution, not the host staging buffer. The score
         * kernel reads `d_sol`; the host `h_indterm` is re-downloaded from
         * `d_indterm` at the top of every pipeline run, so zeroing it changed
         * nothing. Without this, a singular frame scored against the previous
         * frame's solution — or, on the first frame, against whatever the
         * device allocator handed back. ADR-1218. */
        CHECK_CUDA_RETURN(
            cu_f, cuMemsetD8Async(d_sol, 0, (size_t)sz * (size_t)nb * sizeof(float), s->stream));
    } else {
        /* QR factorize cov_mat → Q, R. */
        (void)speed_internal_qr_factorize(s->h_cov_mat, sz, s->h_Q, s->h_R, s->h_qr_scratch);

        /* Q^T × indterm → solution (in-place in h_indterm). */
        speed_internal_qt_multiply(s->h_Q, h_indterm, sz, nb, s->h_qt_scratch);

        /* Upload R and Q^T×indterm to device for GPU backward substitution. */
        CHECK_CUDA_RETURN(
            cu_f,
            cuMemcpyHtoDAsync(s->d_R, s->h_R, (size_t)sz * (size_t)sz * sizeof(float), s->stream));
        CHECK_CUDA_RETURN(cu_f,
                          cuMemcpyHtoDAsync(d_sol, h_indterm,
                                            (size_t)sz * (size_t)nb * sizeof(float), s->stream));

        /* GPU Kernel 4: backward substitution. */
        const int err = launch_backward_substitution(s, cu_f, d_sol, (uint32_t)nb);
        if (err)
            return err;
    }

    /* Upload eigenvalues to device for score kernel. */
    CHECK_CUDA_RETURN(cu_f, cuMemcpyHtoDAsync(s->d_eigenvalues, s->h_eigenvalues,
                                              (size_t)sz * sizeof(float), s->stream));

    return 0;
}

/* ------------------------------------------------------------------ */
/* Run score kernel and aggregate to a single float score             */
/* ------------------------------------------------------------------ */

typedef struct SpeedSpatialScores {
    float reference;
    float distorted;
} SpeedSpatialScores;

static int launch_score_kernel(SpeedChromaCudaState *s, CudaFunctions *cu_f, uint32_t num_blocks)
{
    const float sigma_nn = (float)s->opt.speed_sigma_nn;
    const uint32_t grid = (num_blocks + SC_SCORE_BLOCK - 1u) / SC_SCORE_BLOCK;
    void *args[] = {
        (void *)&s->d_eigenvalues_ref, (void *)&s->d_eigenvalues,   (void *)&s->d_sol_ref,
        (void *)&s->d_sol_dis,         (void *)&s->d_indterm_ref,   (void *)&s->d_indterm_dis,
        (void *)&s->d_ref_entropies,   (void *)&s->d_ref_variances, (void *)&s->d_dis_entropies,
        (void *)&s->d_dis_variances,   (void *)&num_blocks,         (void *)&sigma_nn};

    CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(s->func_score, grid, 1u, 1u, SC_SCORE_BLOCK, 1u, 1u, 0u,
                                           s->stream, args, NULL));
    return 0;
}

static int download_score_arrays(SpeedChromaCudaState *s, CudaFunctions *cu_f, uint32_t num_blocks)
{
    const size_t array_bytes = (size_t)num_blocks * sizeof(float);
    CHECK_CUDA_RETURN(
        cu_f, cuMemcpyDtoHAsync(s->h_ref_entropies, s->d_ref_entropies, array_bytes, s->stream));
    CHECK_CUDA_RETURN(
        cu_f, cuMemcpyDtoHAsync(s->h_ref_variances, s->d_ref_variances, array_bytes, s->stream));
    CHECK_CUDA_RETURN(
        cu_f, cuMemcpyDtoHAsync(s->h_dis_entropies, s->d_dis_entropies, array_bytes, s->stream));
    CHECK_CUDA_RETURN(
        cu_f, cuMemcpyDtoHAsync(s->h_dis_variances, s->d_dis_variances, array_bytes, s->stream));
    CHECK_CUDA_RETURN(cu_f, cuStreamSynchronize(s->stream));
    return 0;
}

static SpeedSpatialScores calculate_spatial_scores(float ref_entropy, float dis_entropy,
                                                   float ref_variance, float dis_variance,
                                                   int weight_mode)
{
    SpeedSpatialScores scores = {0.0f, 0.0f};
    const float mean_variance = (ref_variance + dis_variance) * 0.5f;

    switch (weight_mode) {
    case 0:
        scores.reference = ref_entropy * log2f(1.0f + ref_variance);
        scores.distorted = dis_entropy * log2f(1.0f + dis_variance);
        break;
    case 1:
        scores.reference = ref_entropy * log2f(1.0f + ref_variance);
        scores.distorted = dis_entropy * log2f(1.0f + ref_variance);
        break;
    case 2:
        scores.reference = ref_entropy * log2f(1.0f + dis_variance);
        scores.distorted = dis_entropy * log2f(1.0f + dis_variance);
        break;
    case 3:
        scores.reference = ref_entropy * log2f(1.0f + mean_variance);
        scores.distorted = dis_entropy * log2f(1.0f + mean_variance);
        break;
    case 4:
        scores.reference = ref_entropy * log2f(1.0f + ref_variance);
        scores.distorted = dis_entropy * log2f(1.0f + mean_variance);
        break;
    case 5:
        scores.reference = ref_entropy * log2f(1.0f + ref_variance);
        scores.distorted = dis_entropy * log2f(1.0f + 0.75f * ref_variance + 0.25f * dis_variance);
        break;
    case 6:
        scores.reference = ref_entropy * log2f(1.0f + ref_variance);
        scores.distorted = dis_entropy * log2f(1.0f + 0.25f * ref_variance + 0.75f * dis_variance);
        break;
    default:
        break;
    }

    return scores;
}

static float aggregate_speed_score(const SpeedChromaCudaState *s, uint32_t num_blocks)
{
    const float base_entropy =
        (float)SC_ELEMENTS *
        (log2f((1.0f + (float)s->opt.speed_nn_floor) * (float)s->opt.speed_sigma_nn) +
         log2f(2.0f * 3.14159265358979323846f * 2.71828182845904523536f));
    float total = 0.0f;

    for (uint32_t i = 0; i < num_blocks; ++i) {
        const float ref_entropy = s->h_ref_entropies[i];
        const float dis_entropy = s->h_dis_entropies[i];
        if (ref_entropy < base_entropy && dis_entropy < base_entropy)
            continue;

        const SpeedSpatialScores scores =
            calculate_spatial_scores(ref_entropy, dis_entropy, s->h_ref_variances[i],
                                     s->h_dis_variances[i], s->opt.speed_weight_var_mode);
        total += fabsf(scores.reference - scores.distorted);
    }

    return total / (float)num_blocks;
}

static int run_score_and_collect(SpeedChromaCudaState *s, CudaFunctions *cu_f, float *score_out)
{
    const uint32_t num_blocks = (uint32_t)s->dim.num_blocks;
    int err = launch_score_kernel(s, cu_f, num_blocks);
    if (err)
        return err;
    err = download_score_arrays(s, cu_f, num_blocks);
    if (err)
        return err;

    *score_out = aggregate_speed_score(s, num_blocks);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Extract one channel (U or V) and return the score                   */
/* ------------------------------------------------------------------ */

static int download_host_picture_plane(CudaFunctions *cu_f, const VmafPicture *device_picture,
                                       int channel, VmafPicture *host_picture, uint8_t **raw_plane)
{
    const size_t raw_bytes =
        (size_t)device_picture->h[channel] * (size_t)device_picture->stride[channel];
    uint8_t *const raw = (uint8_t *)aligned_malloc(raw_bytes, 32);
    if (!raw)
        return -ENOMEM;

    const CUresult copy_result =
        cu_f->cuMemcpyDtoH(raw, (CUdeviceptr)device_picture->data[channel], raw_bytes);
    if (copy_result != CUDA_SUCCESS) {
        aligned_free(raw);
        return vmaf_cuda_result_to_errno((int)copy_result);
    }

    *host_picture = *device_picture;
    host_picture->data[channel] = raw;
    *raw_plane = raw;
    return 0;
}

static int prepare_filtered_planes(SpeedChromaCudaState *s, CudaFunctions *cu_f,
                                   const VmafPicture *ref_pic, const VmafPicture *dist_pic,
                                   int channel)
{
    const size_t stride_pixels = s->float_stride / sizeof(float);
    const size_t temporary_elements = 2u * s->dim.alloc_height * stride_pixels;
    float *const temporary = (float *)aligned_malloc(temporary_elements * sizeof(float), 32);
    if (!temporary)
        return -ENOMEM;

    uint8_t *raw_ref = NULL;
    uint8_t *raw_dis = NULL;
    VmafPicture host_ref;
    VmafPicture host_dis;

    /* The CUDA pipeline feeds DEVICE-resident pictures (ref_pic->data[] are
     * CUdeviceptr, as in adm/vif_cuda), but speed_chroma runs its Gaussian
     * filter on the CPU, so the picture_copy() calls below read HOST memory.
     * Download the chroma plane to host staging first — reading the device
     * pointer on the host SEGVs. CUDA tests don't run in CI (no GPU runner),
     * so this was never caught. */
    int err = download_host_picture_plane(cu_f, ref_pic, channel, &host_ref, &raw_ref);
    if (!err)
        err = download_host_picture_plane(cu_f, dist_pic, channel, &host_dis, &raw_dis);
    if (err) {
        aligned_free(raw_ref);
        aligned_free(raw_dis);
        aligned_free(temporary);
        return err;
    }

    picture_copy(s->h_plane_ref, s->float_stride, &host_ref, -128, ref_pic->bpc, channel);
    speed_internal_filter_and_downscale(&s->dim, &s->opt, s->h_plane_ref, temporary,
                                        s->float_stride);
    picture_copy(s->h_plane_dis, s->float_stride, &host_dis, -128, dist_pic->bpc, channel);
    speed_internal_filter_and_downscale(&s->dim, &s->opt, s->h_plane_dis, temporary,
                                        s->float_stride);

    aligned_free(raw_ref);
    aligned_free(raw_dis);
    aligned_free(temporary);
    return 0;
}

static int process_speed_plane(SpeedChromaCudaState *s, CudaFunctions *cu_f, CUdeviceptr d_indterm,
                               CUdeviceptr d_solution, float *host_plane, size_t plane_bytes,
                               bool *singular)
{
    int err = run_gpu_pipeline(s, cu_f, s->d_plane, d_indterm, host_plane, plane_bytes);
    if (err)
        return err;
    return run_cpu_linalg(s, cu_f,
                          d_indterm == s->d_indterm_ref ? s->h_indterm_ref : s->h_indterm_dis,
                          d_solution, singular);
}

static int extract_channel(SpeedChromaCudaState *s, CudaFunctions *cu_f, const VmafPicture *ref_pic,
                           const VmafPicture *dist_pic, int channel, float *score_out,
                           bool *singular_out)
{
    int err = prepare_filtered_planes(s, cu_f, ref_pic, dist_pic, channel);
    if (err)
        return err;

    /* Operating-resolution plane size in bytes (only the valid region). */
    const size_t stride_pixels = s->float_stride / sizeof(float);
    const size_t plane_op_bytes = (size_t)s->dim.truncated_height * stride_pixels * sizeof(float);

    bool singular_ref = false;
    err = process_speed_plane(s, cu_f, s->d_indterm_ref, s->d_sol_ref, s->h_plane_ref,
                              plane_op_bytes, &singular_ref);
    if (err)
        return err;

    /* Stash the reference eigenvalues aside before the distorted linalg pass
     * overwrites s->d_eigenvalues. The CPU reference (est_params in speed.c)
     * computes SEPARATE ref and dis covariance + eigenvalues; the score kernel
     * needs both. The copy is enqueued on the same stream as the eigenvalue
     * upload before it, so the stream orders the two and no host stall is
     * needed. */
    CHECK_CUDA_RETURN(cu_f, cuMemcpyDtoDAsync(s->d_eigenvalues_ref, s->d_eigenvalues,
                                              (size_t)SC_ELEMENTS * sizeof(float), s->stream));

    bool singular_dis = false;
    err = process_speed_plane(s, cu_f, s->d_indterm_dis, s->d_sol_dis, s->h_plane_dis,
                              plane_op_bytes, &singular_dis);
    if (err)
        return err;

    *singular_out = singular_ref || singular_dis;

    /* Exactly one side numerically unstable: report 0 rather than the inflated
     * score a zeroed solution on one side produces. Verbatim the CPU rule in
     * speed_extract_score() (speed.c), which this twin has to match. */
    if (singular_ref != singular_dis) {
        *score_out = 0.0f;
        return 0;
    }

    /* GPU score kernel → aggregate → score_out. The kernel reads
     * d_eigenvalues_ref for the ref entropy and d_eigenvalues (now holding the
     * dis eigenvalues) for the dis entropy. */
    return run_score_and_collect(s, cu_f, score_out);
}

/* ------------------------------------------------------------------ */
/* Extractor lifecycle                                                 */
/* ------------------------------------------------------------------ */

/* Release the module and stream created during init.
 *
 * `free_cuda_buffers()` only releases device/host BUFFERS (cuMemFree /
 * cuMemFreeHost); it never touches `s->module` or `s->stream`. Any init failure
 * after cuModuleLoadData() therefore leaked the module, and any failure after
 * cuStreamCreate() leaked the stream as well.
 *
 * This mirrors close_fex_cuda()'s release order exactly -- stream first, then
 * module, each NULL-guarded, and with no context push, because after the
 * `cuCtxPopCurrent` in the failure labels the context is no longer current and
 * close_fex_cuda() releases them the same way.
 *
 * Regression note: PR #1007 added these calls inline to both failure labels and
 * PR #1029 -- merged the same day as a descendant of #1007 -- removed them
 * again. Factoring the release into one helper keeps the two labels from
 * drifting apart a second time. docs/state.md:
 * T-CUDA-INIT-SUBMIT-LEAKS-2026-06-19.
 */
static void release_cuda_module_and_stream(SpeedChromaCudaState *s, CudaFunctions *cu_f)
{
    if (!s || !cu_f)
        return;
    if (s->stream) {
        (void)cu_f->cuStreamDestroy(s->stream);
        s->stream = NULL;
    }
    if (s->module) {
        (void)cu_f->cuModuleUnload(s->module);
        s->module = NULL;
    }
}

typedef struct SpeedCudaBufferSizes {
    size_t plane;
    size_t indterm;
    size_t covariance;
    size_t score;
    size_t eigenvalues;
    size_t eig_scratch;
} SpeedCudaBufferSizes;

static int get_chroma_dimensions(enum VmafPixelFormat pix_fmt, unsigned width, unsigned height,
                                 unsigned *chroma_width, unsigned *chroma_height)
{
    *chroma_width = width;
    *chroma_height = height;
    switch (pix_fmt) {
    case VMAF_PIX_FMT_UNKNOWN:
    case VMAF_PIX_FMT_YUV400P:
        return -EINVAL;
    case VMAF_PIX_FMT_YUV420P:
        *chroma_width /= 2u;
        *chroma_height /= 2u;
        break;
    case VMAF_PIX_FMT_YUV422P:
        *chroma_width /= 2u;
        break;
    case VMAF_PIX_FMT_YUV444P:
        break;
    }
    return 0;
}

static int configure_speed_geometry(SpeedChromaCudaState *s, enum VmafPixelFormat pix_fmt,
                                    unsigned width, unsigned height,
                                    SpeedCudaBufferSizes *buffer_sizes)
{
    unsigned chroma_width = 0;
    unsigned chroma_height = 0;
    int err = get_chroma_dimensions(pix_fmt, width, height, &chroma_width, &chroma_height);
    if (err)
        return err;

    s->opt = (SpeedInternalOptions){
        .speed_kernelscale = s->speed_chroma_kernelscale,
        .speed_prescale = s->speed_chroma_prescale,
        .speed_prescale_method = s->speed_chroma_prescale_method,
        .speed_sigma_nn = s->speed_chroma_sigma_nn,
        .speed_nn_floor = s->speed_chroma_nn_floor,
        .speed_weight_var_mode = s->speed_weight_var_mode,
    };
    err = speed_internal_init_dimensions(&s->dim, (int)chroma_width, (int)chroma_height,
                                         s->opt.speed_prescale);
    if (err)
        return err;

    s->float_stride = speed_internal_float_stride(s->dim.alloc_width);
    const size_t stride_pixels = s->float_stride / sizeof(float);
    const size_t num_blocks = s->dim.num_blocks;
    buffer_sizes->plane = s->dim.alloc_height * stride_pixels * sizeof(float);
    buffer_sizes->indterm = (size_t)SC_ELEMENTS * num_blocks * sizeof(float);
    buffer_sizes->covariance = (size_t)SC_ELEMENTS * (size_t)SC_ELEMENTS * sizeof(float);
    buffer_sizes->score = num_blocks * sizeof(float);
    buffer_sizes->eigenvalues = (size_t)SC_ELEMENTS * sizeof(float);
    buffer_sizes->eig_scratch =
        ((size_t)SC_ELEMENTS * (size_t)SC_ELEMENTS + 4u * (size_t)SC_ELEMENTS) * sizeof(float);
    return 0;
}

static int load_speed_cuda_kernels(SpeedChromaCudaState *s, CudaFunctions *cu_f)
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

static int allocate_device_buffer(CUdeviceptr *buffer, size_t bytes, CudaFunctions *cu_f)
{
    CHECK_CUDA_RETURN(cu_f, cuMemAlloc(buffer, bytes));
    return 0;
}

static int allocate_device_work_buffers(SpeedChromaCudaState *s, CudaFunctions *cu_f,
                                        const SpeedCudaBufferSizes *sizes)
{
    int err = allocate_device_buffer(&s->d_plane, sizes->plane, cu_f);
    if (!err)
        err = allocate_device_buffer(&s->d_means, sizes->indterm, cu_f);
    if (!err)
        err = allocate_device_buffer(&s->d_cov_mat, sizes->covariance, cu_f);
    if (!err)
        err = allocate_device_buffer(&s->d_indterm_ref, sizes->indterm, cu_f);
    if (!err)
        err = allocate_device_buffer(&s->d_indterm_dis, sizes->indterm, cu_f);
    if (!err)
        err = allocate_device_buffer(&s->d_sol_ref, sizes->indterm, cu_f);
    if (!err)
        err = allocate_device_buffer(&s->d_sol_dis, sizes->indterm, cu_f);
    if (!err)
        err = allocate_device_buffer(&s->d_R, sizes->covariance, cu_f);
    if (!err)
        err = allocate_device_buffer(&s->d_eigenvalues, sizes->eigenvalues, cu_f);
    if (!err)
        err = allocate_device_buffer(&s->d_eigenvalues_ref, sizes->eigenvalues, cu_f);
    return err;
}

static int allocate_device_score_buffers(SpeedChromaCudaState *s, CudaFunctions *cu_f,
                                         const SpeedCudaBufferSizes *sizes)
{
    int err = allocate_device_buffer(&s->d_ref_entropies, sizes->score, cu_f);
    if (!err)
        err = allocate_device_buffer(&s->d_ref_variances, sizes->score, cu_f);
    if (!err)
        err = allocate_device_buffer(&s->d_dis_entropies, sizes->score, cu_f);
    if (!err)
        err = allocate_device_buffer(&s->d_dis_variances, sizes->score, cu_f);
    return err;
}

static int allocate_pinned_buffers(SpeedChromaCudaState *s, CudaFunctions *cu_f,
                                   const SpeedCudaBufferSizes *sizes)
{
    CHECK_CUDA_RETURN(cu_f, cuMemHostAlloc((void **)&s->h_cov_mat, sizes->covariance, 0x01u));
    CHECK_CUDA_RETURN(cu_f, cuMemHostAlloc((void **)&s->h_ref_entropies, sizes->score, 0x01u));
    CHECK_CUDA_RETURN(cu_f, cuMemHostAlloc((void **)&s->h_ref_variances, sizes->score, 0x01u));
    CHECK_CUDA_RETURN(cu_f, cuMemHostAlloc((void **)&s->h_dis_entropies, sizes->score, 0x01u));
    CHECK_CUDA_RETURN(cu_f, cuMemHostAlloc((void **)&s->h_dis_variances, sizes->score, 0x01u));
    return 0;
}

static int pop_cuda_context(CudaFunctions *cu_f)
{
    CHECK_CUDA_RETURN(cu_f, cuCtxPopCurrent(NULL));
    return 0;
}

static int pop_cuda_context_with_retry(CudaFunctions *cu_f)
{
    const int err = pop_cuda_context(cu_f);
    if (err)
        (void)pop_cuda_context(cu_f);
    return err;
}

static int initialize_cuda_resources(VmafFeatureExtractor *fex, SpeedChromaCudaState *s,
                                     CudaFunctions *cu_f, const SpeedCudaBufferSizes *sizes)
{
    CHECK_CUDA_RETURN(cu_f, cuCtxPushCurrent(fex->cu_state->ctx));

    int err = load_speed_cuda_kernels(s, cu_f);
    if (!err)
        err = allocate_device_work_buffers(s, cu_f, sizes);
    if (!err)
        err = allocate_device_score_buffers(s, cu_f, sizes);
    if (!err)
        err = allocate_pinned_buffers(s, cu_f, sizes);

    const int pop_err = pop_cuda_context_with_retry(cu_f);
    if (!err)
        err = pop_err;
    if (err) {
        release_cuda_module_and_stream(s, cu_f);
        free_cuda_buffers(s, cu_f);
    }
    return err;
}

static int allocate_cpu_buffers(SpeedChromaCudaState *s, const SpeedCudaBufferSizes *sizes)
{
    s->h_plane_ref = (float *)aligned_malloc(sizes->plane, 32);
    s->h_plane_dis = (float *)aligned_malloc(sizes->plane, 32);
    s->h_eigenvalues = (float *)aligned_malloc(sizes->eigenvalues, 32);
    s->h_eig_scratch = (float *)aligned_malloc(sizes->eig_scratch, 32);
    s->h_Q = (float *)aligned_malloc(sizes->covariance, 32);
    s->h_R = (float *)aligned_malloc(sizes->covariance, 32);
    s->h_qr_scratch = (float *)aligned_malloc(4u * sizes->covariance, 32);
    s->h_indterm_ref = (float *)aligned_malloc(sizes->indterm, 32);
    s->h_indterm_dis = (float *)aligned_malloc(sizes->indterm, 32);
    s->h_qt_scratch = (float *)aligned_malloc(sizes->indterm, 32);

    if (!s->h_plane_ref || !s->h_plane_dis || !s->h_eigenvalues || !s->h_eig_scratch || !s->h_Q ||
        !s->h_R || !s->h_qr_scratch || !s->h_indterm_ref || !s->h_indterm_dis || !s->h_qt_scratch)
        return -ENOMEM;
    return 0;
}

static int init_fex_cuda(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                         unsigned w, unsigned h)
{
    (void)bpc;
    SpeedChromaCudaState *s = fex->priv;
    CudaFunctions *cu_f = fex->cu_state->f;
    SpeedCudaBufferSizes buffer_sizes;
    int err = configure_speed_geometry(s, pix_fmt, w, h, &buffer_sizes);
    if (err)
        return err;
    err = initialize_cuda_resources(fex, s, cu_f, &buffer_sizes);
    if (err)
        return err;
    err = allocate_cpu_buffers(s, &buffer_sizes);
    if (err) {
        release_cuda_module_and_stream(s, cu_f);
        free_cuda_buffers(s, cu_f);
        return err;
    }

    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (!s->feature_name_dict) {
        release_cuda_module_and_stream(s, cu_f);
        free_cuda_buffers(s, cu_f);
        return -ENOMEM;
    }

    return 0;
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
    CHECK_CUDA_RETURN(cu_f, cuCtxPushCurrent(fex->cu_state->ctx));

    float score_u = 0.0f;
    float score_v = 0.0f;
    bool singular_u = false;
    bool singular_v = false;
    int err_u = extract_channel(s, cu_f, ref_pic, dist_pic, 1, &score_u, &singular_u);
    int err_v = extract_channel(s, cu_f, ref_pic, dist_pic, 2, &score_v, &singular_v);

    const int pop_err = pop_cuda_context_with_retry(cu_f);
    if (pop_err)
        return pop_err;

    /* A hard failure (CUDA API error, allocation failure) fails the frame, and
     * is NOT the singular-matrix condition -- conflating the two is what made
     * ADR-1202's 4K launch failure surface as three silent 0.0 scores on an
     * exit-0 run. Singularity arrives via `singular_u` / `singular_v`. */
    if (err_u)
        return err_u;
    if (err_v)
        return err_v;

    const float score_uv = combine_chroma_uv(score_u, score_v, singular_u, singular_v);

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
}

static int close_fex_cuda(VmafFeatureExtractor *fex)
{
    SpeedChromaCudaState *s = fex->priv;
    speed_internal_report_singular(&s->singular_tally, "speed_chroma_cuda");
    if (fex->cu_state && fex->cu_state->f) {
        free_cuda_buffers(s, fex->cu_state->f);
        release_cuda_module_and_stream(s, fex->cu_state->f);
    }
    if (s->feature_name_dict)
        vmaf_dictionary_free(&s->feature_name_dict);
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

/* NOLINTEND(modernize-use-nullptr) */
