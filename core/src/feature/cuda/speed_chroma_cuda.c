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
#include "cuda/speed_chroma_cuda.h"

/* lint rationale: C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `VMAF_NULLPTR` and
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

#define SPEED_CHROMA_DOUBLE_OPTION(NAME, HELP, FIELD, DEFAULT, MINIMUM, MAXIMUM, ALIAS)            \
    {                                                                                              \
        .name = (NAME),                                                                            \
        .help = (HELP),                                                                            \
        .offset = offsetof(SpeedChromaCudaState, FIELD),                                           \
        .type = VMAF_OPT_TYPE_DOUBLE,                                                              \
        .default_val.d = (DEFAULT),                                                                \
        .min = (MINIMUM),                                                                          \
        .max = (MAXIMUM),                                                                          \
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,                                                      \
        .alias = (ALIAS),                                                                          \
    }
#define SPEED_CHROMA_STRING_OPTION(NAME, HELP, FIELD, DEFAULT, ALIAS)                              \
    {                                                                                              \
        .name = (NAME),                                                                            \
        .help = (HELP),                                                                            \
        .offset = offsetof(SpeedChromaCudaState, FIELD),                                           \
        .type = VMAF_OPT_TYPE_STRING,                                                              \
        .default_val.s = (DEFAULT),                                                                \
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,                                                      \
        .alias = (ALIAS),                                                                          \
    }
#define SPEED_CHROMA_INT_OPTION(NAME, HELP, FIELD, DEFAULT, MINIMUM, MAXIMUM, ALIAS)               \
    {                                                                                              \
        .name = (NAME),                                                                            \
        .help = (HELP),                                                                            \
        .offset = offsetof(SpeedChromaCudaState, FIELD),                                           \
        .type = VMAF_OPT_TYPE_INT,                                                                 \
        .default_val.d = (DEFAULT),                                                                \
        .min = (MINIMUM),                                                                          \
        .max = (MAXIMUM),                                                                          \
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,                                                      \
        .alias = (ALIAS),                                                                          \
    }

static const VmafOption options[] = {
    SPEED_CHROMA_DOUBLE_OPTION("speed_kernelscale", "scaling factor for the Gaussian kernel",
                               speed_chroma_kernelscale, SC_DEFAULT_KERNELSCALE, 0.1, 4.0, "ks"),
    SPEED_CHROMA_DOUBLE_OPTION("speed_prescale", "scaling factor for the frame",
                               speed_chroma_prescale, SC_DEFAULT_PRESCALE, 0.1, 4.0, "ps"),
    SPEED_CHROMA_STRING_OPTION("speed_prescale_method",
                               "scaling method [nearest, bilinear, bicubic, lanczos4]",
                               speed_chroma_prescale_method, SC_DEFAULT_PRESCALE_METHOD, "psm"),
    SPEED_CHROMA_DOUBLE_OPTION("speed_sigma_nn", "standard deviation of neural noise",
                               speed_chroma_sigma_nn, SC_DEFAULT_SIGMA_NN, 0.1, 2.0, "snn"),
    SPEED_CHROMA_DOUBLE_OPTION("speed_nn_floor", "neural noise floor fraction",
                               speed_chroma_nn_floor, SC_DEFAULT_NN_FLOOR, 0.0, 1.0, "nnf"),
    SPEED_CHROMA_DOUBLE_OPTION("speed_max_val", "clip output to this maximum", speed_chroma_max_val,
                               SC_DEFAULT_MAX_VAL, 0.0, 1000.0, "mxv"),
    SPEED_CHROMA_INT_OPTION("speed_weight_var_mode", "variance weighting mode (0-6)",
                            speed_weight_var_mode, 0, 0, 6, "wvm"),
    {0},
};

#undef SPEED_CHROMA_INT_OPTION
#undef SPEED_CHROMA_STRING_OPTION
#undef SPEED_CHROMA_DOUBLE_OPTION

/* ------------------------------------------------------------------ */
/* Free all device and pinned-host buffers                            */
/* ------------------------------------------------------------------ */

static void speed_chroma_free_device(SpeedChromaCudaState *s, CudaFunctions *cu_f)
{
#define FREE_DPTR(p)                                                                               \
    do {                                                                                           \
        if ((p)) {                                                                                 \
            (void)cu_f->cuMemFree((p));                                                            \
            (p) = 0;                                                                               \
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
#undef FREE_DPTR
}

static void speed_chroma_free_pinned(SpeedChromaCudaState *s, CudaFunctions *cu_f)
{
#define FREE_HOST(p)                                                                               \
    do {                                                                                           \
        if ((p)) {                                                                                 \
            (void)cu_f->cuMemFreeHost((p));                                                        \
            (p) = VMAF_NULLPTR;                                                                    \
        }                                                                                          \
    } while (0)
    FREE_HOST(s->h_cov_mat);
    FREE_HOST(s->h_ref_entropies);
    FREE_HOST(s->h_ref_variances);
    FREE_HOST(s->h_dis_entropies);
    FREE_HOST(s->h_dis_variances);

#undef FREE_HOST
}

static void speed_chroma_free_cpu_buffer(float **buffer)
{
    aligned_free(*buffer);
    *buffer = VMAF_NULLPTR;
}

static void speed_chroma_free_cpu(SpeedChromaCudaState *s)
{
    speed_chroma_free_cpu_buffer(&s->h_plane_ref);
    speed_chroma_free_cpu_buffer(&s->h_plane_dis);
    speed_chroma_free_cpu_buffer(&s->h_eigenvalues);
    speed_chroma_free_cpu_buffer(&s->h_eig_scratch);
    speed_chroma_free_cpu_buffer(&s->h_Q);
    speed_chroma_free_cpu_buffer(&s->h_R);
    speed_chroma_free_cpu_buffer(&s->h_qr_scratch);
    speed_chroma_free_cpu_buffer(&s->h_indterm_ref);
    speed_chroma_free_cpu_buffer(&s->h_indterm_dis);
    speed_chroma_free_cpu_buffer(&s->h_qt_scratch);
}

static void free_cuda_buffers(SpeedChromaCudaState *s, CudaFunctions *cu_f)
{
    speed_chroma_free_device(s, cu_f);
    speed_chroma_free_pinned(s, cu_f);
    speed_chroma_free_cpu(s);
}

/* ------------------------------------------------------------------ */
/* GPU kernel: run covariance + indterm pipeline for one plane         */
/* ------------------------------------------------------------------ */

static int speed_chroma_download_pipeline(SpeedChromaCudaState *s, CudaFunctions *cu_f,
                                          CUdeviceptr d_indterm, uint32_t num_blocks)
{
    CHECK_CUDA_RETURN(cu_f,
                      cuMemcpyDtoHAsync(s->h_cov_mat, s->d_cov_mat,
                                        (size_t)SC_ELEMENTS * (size_t)SC_ELEMENTS * sizeof(float),
                                        s->stream));
    const size_t bytes = (size_t)SC_ELEMENTS * num_blocks * sizeof(float);
    float *host = d_indterm == s->d_indterm_ref ? s->h_indterm_ref : s->h_indterm_dis;
    CHECK_CUDA_RETURN(cu_f, cuMemcpyDtoHAsync(host, d_indterm, bytes, s->stream));
    CHECK_CUDA_RETURN(cu_f, cuStreamSynchronize(s->stream));
    return 0;
}

static int run_gpu_pipeline(SpeedChromaCudaState *s, CudaFunctions *cu_f, CUdeviceptr d_plane,
                            CUdeviceptr d_indterm, float *h_plane, size_t plane_op_bytes)
{
    /* Upload operating-resolution plane to device (synchronous for simplicity;
     * the CPU eigendecomp below is the latency-hiding opportunity). */
    int _cuda_err;
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
                                       s->stream, args, VMAF_NULLPTR),
                        fail);
    }

    /* --- Kernel 2: covariance matrix (625 blocks × COV_BLOCK threads) */
    {
        void *args[] = {(void *)&d_plane,     (void *)&s->d_means,   (void *)&s->d_cov_mat,
                        (void *)&stride_px,   (void *)&num_blocks_h, (void *)&num_blocks,
                        (void *)&submatrix_w, (void *)&submatrix_h};
        CHECK_CUDA_GOTO(cu_f,
                        cuLaunchKernel(s->func_cov, SC_ELEMENTS, SC_ELEMENTS, 1u, SC_COV_BLOCK, 1u,
                                       1u, 0u, s->stream, args, VMAF_NULLPTR),
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
                                       0u, s->stream, args, VMAF_NULLPTR),
                        fail);
    }

    return speed_chroma_download_pipeline(s, cu_f, d_indterm, num_blocks);

fail:
    return _cuda_err;
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
    int _cuda_err;
    const uint32_t warps_per_block =
        (u_nb < SC_SOLVE_WARPS_PER_BLOCK) ? (u_nb ? u_nb : 1u) : SC_SOLVE_WARPS_PER_BLOCK;
    const uint32_t threads = warps_per_block * SC_SOLVE_WARP;
    const uint32_t blocks = (u_nb + warps_per_block - 1u) / warps_per_block;
    void *args[] = {(void *)&s->d_R, (void *)&d_sol, (void *)&u_nb};

    CHECK_CUDA_GOTO(cu_f,
                    cuLaunchKernel(s->func_solve, blocks, 1u, 1u, threads, 1u, 1u, 0u, s->stream,
                                   args, VMAF_NULLPTR),
                    fail);
    /* No stall: the solution stays on the device and its only consumer, the
     * score kernel, is enqueued on this same stream, which orders it. */
    return 0;

fail:
    return _cuda_err;
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
    int _cuda_err;

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
        CHECK_CUDA_GOTO(
            cu_f, cuMemsetD8Async(d_sol, 0, (size_t)sz * (size_t)nb * sizeof(float), s->stream),
            fail);
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
        const int launch_err = launch_backward_substitution(s, cu_f, d_sol, (uint32_t)nb);
        if (launch_err)
            return launch_err;
    }

    /* Upload eigenvalues to device for score kernel. */
    CHECK_CUDA_GOTO(cu_f,
                    cuMemcpyHtoDAsync(s->d_eigenvalues, s->h_eigenvalues,
                                      (size_t)sz * sizeof(float), s->stream),
                    fail);

    return 0;

fail:
    return _cuda_err;
}

/* ------------------------------------------------------------------ */
/* Run score kernel and aggregate to a single float score             */
/* ------------------------------------------------------------------ */

static float speed_chroma_score_difference(float ref_entropy, float dis_entropy, float ref_var,
                                           float dis_var, int mode)
{
    float spatial_ref = 0.0f;
    float spatial_dis = 0.0f;
    if (mode == 0) {
        spatial_ref = ref_entropy * log2f(1.0f + ref_var);
        spatial_dis = dis_entropy * log2f(1.0f + dis_var);
    } else if (mode == 1) {
        spatial_ref = ref_entropy * log2f(1.0f + ref_var);
        spatial_dis = dis_entropy * log2f(1.0f + ref_var);
    } else if (mode == 2) {
        spatial_ref = ref_entropy * log2f(1.0f + dis_var);
        spatial_dis = dis_entropy * log2f(1.0f + dis_var);
    } else if (mode == 3) {
        const float mean_var = (ref_var + dis_var) * 0.5f;
        spatial_ref = ref_entropy * log2f(1.0f + mean_var);
        spatial_dis = dis_entropy * log2f(1.0f + mean_var);
    } else if (mode == 4) {
        spatial_ref = ref_entropy * log2f(1.0f + ref_var);
        spatial_dis = dis_entropy * log2f(1.0f + (ref_var + dis_var) * 0.5f);
    } else if (mode == 5) {
        spatial_ref = ref_entropy * log2f(1.0f + ref_var);
        spatial_dis = dis_entropy * log2f(1.0f + 0.75f * ref_var + 0.25f * dis_var);
    } else if (mode == 6) {
        spatial_ref = ref_entropy * log2f(1.0f + ref_var);
        spatial_dis = dis_entropy * log2f(1.0f + 0.25f * ref_var + 0.75f * dis_var);
    }
    return fabsf(spatial_ref - spatial_dis);
}

static void speed_chroma_aggregate_score(SpeedChromaCudaState *s, uint32_t num_blocks,
                                         float *score_out)
{
    const float base_entropy =
        (float)SC_ELEMENTS *
        (log2f((1.0f + (float)s->opt.speed_nn_floor) * (float)s->opt.speed_sigma_nn) +
         log2f(2.0f * 3.14159265358979323846f * 2.71828182845904523536f));
    float total = 0.0f;
    for (uint32_t i = 0; i < num_blocks; i++) {
        const float ref_entropy = s->h_ref_entropies[i];
        const float dis_entropy = s->h_dis_entropies[i];
        if (ref_entropy < base_entropy && dis_entropy < base_entropy)
            continue;
        total += speed_chroma_score_difference(ref_entropy, dis_entropy, s->h_ref_variances[i],
                                               s->h_dis_variances[i], s->opt.speed_weight_var_mode);
    }
    *score_out = total / (float)num_blocks;
}

static int run_score_and_collect(SpeedChromaCudaState *s, CudaFunctions *cu_f, float *score_out)
{
    const uint32_t num_blocks = (uint32_t)s->dim.num_blocks;
    const float sigma_nn = (float)s->opt.speed_sigma_nn;
    int _cuda_err;

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
                                       s->stream, args, VMAF_NULLPTR),
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

    speed_chroma_aggregate_score(s, num_blocks, score_out);
    return 0;

fail:
    return _cuda_err;
}

/* ------------------------------------------------------------------ */
/* Extract one channel (U or V) and return the score                   */
/* ------------------------------------------------------------------ */

static int speed_chroma_prepare_plane(SpeedChromaCudaState *s, CudaFunctions *cu_f,
                                      const VmafPicture *ref, const VmafPicture *dist, int channel)
{
    const size_t stride = s->float_stride / sizeof(float);
    const size_t scratch_elements = 2u * s->dim.alloc_height * stride;
    float *scratch = (float *)aligned_malloc(scratch_elements * sizeof(float), 32);
    const size_t ref_bytes = (size_t)ref->h[channel] * ref->stride[channel];
    const size_t dist_bytes = (size_t)dist->h[channel] * dist->stride[channel];
    uint8_t *raw_ref = (uint8_t *)aligned_malloc(ref_bytes, 32);
    uint8_t *raw_dist = (uint8_t *)aligned_malloc(dist_bytes, 32);
    if (!scratch || !raw_ref || !raw_dist) {
        aligned_free(scratch);
        aligned_free(raw_ref);
        aligned_free(raw_dist);
        return -ENOMEM;
    }
    const bool copy_failed =
        cu_f->cuMemcpyDtoH(raw_ref, (CUdeviceptr)ref->data[channel], ref_bytes) != CUDA_SUCCESS ||
        cu_f->cuMemcpyDtoH(raw_dist, (CUdeviceptr)dist->data[channel], dist_bytes) != CUDA_SUCCESS;
    if (!copy_failed) {
        VmafPicture host_ref = *ref;
        VmafPicture host_dist = *dist;
        host_ref.data[channel] = raw_ref;
        host_dist.data[channel] = raw_dist;
        picture_copy(s->h_plane_ref, s->float_stride, &host_ref, -128, ref->bpc, channel);
        speed_internal_filter_and_downscale(&s->dim, &s->opt, s->h_plane_ref, scratch,
                                            s->float_stride);
        picture_copy(s->h_plane_dis, s->float_stride, &host_dist, -128, dist->bpc, channel);
        speed_internal_filter_and_downscale(&s->dim, &s->opt, s->h_plane_dis, scratch,
                                            s->float_stride);
    }
    aligned_free(scratch);
    aligned_free(raw_ref);
    aligned_free(raw_dist);
    return copy_failed ? -EIO : 0;
}

static int extract_channel(SpeedChromaCudaState *s, CudaFunctions *cu_f, const VmafPicture *ref_pic,
                           const VmafPicture *dist_pic, int channel, float *score_out,
                           bool *singular_out)
{
    const size_t stride_px = s->float_stride / sizeof(float);
    int err = speed_chroma_prepare_plane(s, cu_f, ref_pic, dist_pic, channel);
    if (err)
        return err;
    const size_t plane_op_bytes = (size_t)s->dim.truncated_height * stride_px * sizeof(float);
    err = run_gpu_pipeline(s, cu_f, s->d_plane, s->d_indterm_ref, s->h_plane_ref, plane_op_bytes);
    if (err)
        return err;

    bool singular_ref = false;
    err = run_cpu_linalg(s, cu_f, s->h_indterm_ref, s->d_sol_ref, &singular_ref);
    if (err)
        return err;

    CHECK_CUDA_RETURN(cu_f, cuMemcpyDtoDAsync(s->d_eigenvalues_ref, s->d_eigenvalues,
                                              (size_t)SC_ELEMENTS * sizeof(float), s->stream));
    err = run_gpu_pipeline(s, cu_f, s->d_plane, s->d_indterm_dis, s->h_plane_dis, plane_op_bytes);
    if (err)
        return err;

    bool singular_dis = false;
    err = run_cpu_linalg(s, cu_f, s->h_indterm_dis, s->d_sol_dis, &singular_dis);
    if (err)
        return err;

    *singular_out = singular_ref || singular_dis;

    if (singular_ref != singular_dis) {
        *score_out = 0.0f;
        return 0;
    }

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
 * module, each VMAF_NULLPTR-guarded, and with no context push, because after the
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
        s->stream = VMAF_NULLPTR;
    }
    if (s->module) {
        (void)cu_f->cuModuleUnload(s->module);
        s->module = VMAF_NULLPTR;
    }
}

static void speed_chroma_set_options(SpeedChromaCudaState *s)
{
    s->opt = (SpeedInternalOptions){
        .speed_kernelscale = s->speed_chroma_kernelscale,
        .speed_prescale = s->speed_chroma_prescale,
        .speed_prescale_method = s->speed_chroma_prescale_method,
        .speed_sigma_nn = s->speed_chroma_sigma_nn,
        .speed_nn_floor = s->speed_chroma_nn_floor,
        .speed_weight_var_mode = s->speed_weight_var_mode,
    };
}

static int speed_chroma_load_module(SpeedChromaCudaState *s, CudaFunctions *cu_f)
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

static int speed_chroma_create_stream(SpeedChromaCudaState *s, CudaFunctions *cu_f)
{
    CHECK_CUDA_RETURN(cu_f, cuStreamCreate(&s->stream, CU_STREAM_NON_BLOCKING));
    return 0;
}

static int speed_chroma_alloc_device(SpeedChromaCudaState *s, CudaFunctions *cu_f)
{
    const size_t stride = s->float_stride / sizeof(float);
    const size_t plane = s->dim.alloc_height * stride * sizeof(float);
    const size_t indterm = (size_t)SC_ELEMENTS * s->dim.num_blocks * sizeof(float);
    const size_t covariance = (size_t)SC_ELEMENTS * (size_t)SC_ELEMENTS * sizeof(float);
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
                            (size_t)SC_ELEMENTS * sizeof(float),
                            (size_t)SC_ELEMENTS * sizeof(float),
                            score,
                            score,
                            score,
                            score};
    for (size_t i = 0; i < sizeof(buffers) / sizeof(buffers[0]); i++)
        CHECK_CUDA_RETURN(cu_f, cuMemAlloc(buffers[i], sizes[i]));
    return 0;
}

static int speed_chroma_alloc_pinned(SpeedChromaCudaState *s, CudaFunctions *cu_f)
{
    const size_t covariance = (size_t)SC_ELEMENTS * (size_t)SC_ELEMENTS * sizeof(float);
    const size_t score = s->dim.num_blocks * sizeof(float);
    float **buffers[] = {&s->h_cov_mat, &s->h_ref_entropies, &s->h_ref_variances,
                         &s->h_dis_entropies, &s->h_dis_variances};
    const size_t sizes[] = {covariance, score, score, score, score};
    for (size_t i = 0; i < sizeof(buffers) / sizeof(buffers[0]); i++)
        CHECK_CUDA_RETURN(cu_f, cuMemHostAlloc((void **)buffers[i], sizes[i], 0x01u));
    return 0;
}

static int speed_chroma_init_device(VmafFeatureExtractor *fex, SpeedChromaCudaState *s)
{
    CudaFunctions *cu_f = fex->cu_state->f;
    const CUresult push = cu_f->cuCtxPushCurrent(fex->cu_state->ctx);
    if (push != CUDA_SUCCESS)
        return vmaf_cuda_result_to_errno((int)push);
    int ret = speed_chroma_load_module(s, cu_f);
    if (!ret)
        ret = speed_chroma_create_stream(s, cu_f);
    if (!ret)
        ret = speed_chroma_alloc_device(s, cu_f);
    if (!ret)
        ret = speed_chroma_alloc_pinned(s, cu_f);
    const CUresult pop = cu_f->cuCtxPopCurrent(VMAF_NULLPTR);
    if (!ret && pop != CUDA_SUCCESS)
        ret = vmaf_cuda_result_to_errno((int)pop);
    return ret;
}

static int speed_chroma_alloc_cpu(SpeedChromaCudaState *s)
{
    const size_t stride = s->float_stride / sizeof(float);
    const size_t plane = s->dim.alloc_height * stride * sizeof(float);
    const size_t indterm = (size_t)SC_ELEMENTS * s->dim.num_blocks * sizeof(float);
    const size_t covariance = (size_t)SC_ELEMENTS * (size_t)SC_ELEMENTS * sizeof(float);
    float **buffers[] = {&s->h_plane_ref,   &s->h_plane_dis, &s->h_eigenvalues, &s->h_eig_scratch,
                         &s->h_Q,           &s->h_R,         &s->h_qr_scratch,  &s->h_indterm_ref,
                         &s->h_indterm_dis, &s->h_qt_scratch};
    const size_t sizes[] = {
        plane,
        plane,
        (size_t)SC_ELEMENTS * sizeof(float),
        ((size_t)SC_ELEMENTS * (size_t)SC_ELEMENTS + (size_t)4U * SC_ELEMENTS) * sizeof(float),
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

static int init_fex_cuda(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                         unsigned w, unsigned h)
{
    (void)bpc;
    SpeedChromaCudaState *s = fex->priv;
    CudaFunctions *cu_f = fex->cu_state->f;
    unsigned chroma_width = 0;
    unsigned chroma_height = 0;
    int err = speed_chroma_dimensions(w, h, pix_fmt, &chroma_width, &chroma_height);
    if (err)
        return err;
    speed_chroma_set_options(s);
    err = speed_internal_init_dimensions(&s->dim, (int)chroma_width, (int)chroma_height,
                                         s->opt.speed_prescale);
    if (err)
        return err;
    s->float_stride = speed_internal_float_stride(s->dim.alloc_width);
    err = speed_chroma_alloc_cpu(s);
    if (!err)
        err = speed_chroma_init_device(fex, s);
    if (!err) {
        s->feature_name_dict =
            vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
        if (!s->feature_name_dict)
            err = -ENOMEM;
    }
    if (err) {
        free_cuda_buffers(s, cu_f);
        release_cuda_module_and_stream(s, cu_f);
        return err;
    }
    return 0;
}

static int extract_fex_cuda(VmafFeatureExtractor *fex, const VmafPicture *ref_pic,
                            const VmafPicture *ref_pic_90, const VmafPicture *dist_pic,
                            const VmafPicture *dist_pic_90, unsigned index,
                            VmafFeatureCollector *feature_collector)
{
    (void)ref_pic_90;
    (void)dist_pic_90;

    SpeedChromaCudaState *s = fex->priv;
    CudaFunctions *cu_f = fex->cu_state->f;
    int err = 0;
    int _cuda_err;

    CHECK_CUDA_GOTO(cu_f, cuCtxPushCurrent(fex->cu_state->ctx), fail);

    float score_u = 0.0f;
    float score_v = 0.0f;
    bool singular_u = false;
    bool singular_v = false;
    int err_u = extract_channel(s, cu_f, ref_pic, dist_pic, 1, &score_u, &singular_u);
    int err_v = extract_channel(s, cu_f, ref_pic, dist_pic, 2, &score_v, &singular_v);

    CHECK_CUDA_GOTO(cu_f, cuCtxPopCurrent(VMAF_NULLPTR), fail_after_push);

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

fail_after_push:
    /* The context was pushed but the pop failed: attempt the pop once more so
     * the CUDA context stack is not left unbalanced (a per-frame leak that
     * eventually exhausts the stack), then propagate the original error.
     * Mirrors the fail_after_pop pattern in init_fex_cuda. */
    (void)cu_f->cuCtxPopCurrent(VMAF_NULLPTR);
fail:
    return _cuda_err;
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
    VMAF_NULLPTR,
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
