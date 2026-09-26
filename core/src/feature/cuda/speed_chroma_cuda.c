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

/* sc_free_device_and_pinned - the device allocations and the pinned host
 * staging buffers.
 *
 * HISS-04: lifted verbatim out of free_cuda_buffers; same set, same order.
 */
static int sc_free_device(SpeedChromaCudaState *s, VmafCudaState *cu_state)
{
    int rc = 0;
#define FREE_DPTR(p)                                                                               \
    do {                                                                                           \
        const int free_err = vmaf_cuda_deviceptr_free_owned(cu_state, &(p));                       \
        if (free_err != 0 && rc == 0)                                                              \
            rc = free_err;                                                                         \
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
    return rc;
}

static int sc_free_pinned(SpeedChromaCudaState *s, VmafCudaState *cu_state)
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

/* sc_free_host_aligned - the aligned_alloc host scratch buffers.
 *
 * HISS-04: lifted verbatim out of free_cuda_buffers; same set, same order.
 */
static void sc_free_host_aligned(SpeedChromaCudaState *s)
{
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

static int free_cuda_buffers(SpeedChromaCudaState *s, VmafCudaState *cu_state)
{
    if (!s)
        return -EINVAL;
    if (!cu_state || !cu_state->f || !cu_state->ctx) {
        sc_free_host_aligned(s);
        return -EINVAL;
    }

    int rc = sc_free_device(s, cu_state);
    const int pinned_rc = sc_free_pinned(s, cu_state);
    if (!rc)
        rc = pinned_rc;
    sc_free_host_aligned(s);
    return rc;
}

/* ------------------------------------------------------------------ */
/* GPU kernel: run covariance + indterm pipeline for one plane         */
/* ------------------------------------------------------------------ */

/* sc_launch_plane_kernels - the means, covariance and independent-term
 * launches for one plane.
 *
 * HISS-04: lifted verbatim out of run_gpu_pipeline. Inside a helper the
 * macro is CHECK_CUDA_RETURN rather than CHECK_CUDA_GOTO; the former `fail`
 * label did nothing but return `_cuda_err`, so the caller propagating the
 * same errno is identical.
 */
static int sc_launch_plane_kernels(SpeedChromaCudaState *s, CudaFunctions *cu_f,
                                   CUdeviceptr d_plane, CUdeviceptr d_indterm, uint32_t num_blocks,
                                   uint32_t num_blocks_h, uint32_t op_w, uint32_t stride_px,
                                   uint32_t submatrix_w, uint32_t submatrix_h)
{
    /* --- Kernel 1: means ------------------------------------------ */
    {
        const uint32_t grid_x = (num_blocks + SC_MEANS_BLOCK - 1u) / SC_MEANS_BLOCK;
        void *args[] = {(void *)&d_plane,     (void *)&s->d_means,   (void *)&op_w,
                        (void *)&stride_px,   (void *)&num_blocks_h, (void *)&num_blocks,
                        (void *)&submatrix_w, (void *)&submatrix_h};
        CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(s->func_means, grid_x, 1u, 1u, SC_MEANS_BLOCK, 1u,
                                               1u, 0u, s->stream, args, NULL));
    }

    /* --- Kernel 2: covariance matrix (625 blocks × COV_BLOCK threads) */
    {
        void *args[] = {(void *)&d_plane,     (void *)&s->d_means,   (void *)&s->d_cov_mat,
                        (void *)&stride_px,   (void *)&num_blocks_h, (void *)&num_blocks,
                        (void *)&submatrix_w, (void *)&submatrix_h};
        CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(s->func_cov, SC_ELEMENTS, SC_ELEMENTS, 1u,
                                               SC_COV_BLOCK, 1u, 1u, 0u, s->stream, args, NULL));
    }

    /* --- Kernel 3: independent term -------------------------------- */
    {
        const uint32_t total = SC_ELEMENTS * num_blocks;
        const uint32_t grid_x = (total + SC_INDTERM_BLOCK - 1u) / SC_INDTERM_BLOCK;
        void *args[] = {(void *)&d_plane, (void *)&d_indterm, (void *)&stride_px,
                        (void *)&num_blocks_h, (void *)&num_blocks};
        CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(s->func_indterm, grid_x, 1u, 1u, SC_INDTERM_BLOCK,
                                               1u, 1u, 0u, s->stream, args, NULL));
    }
    return 0;
}

static int run_gpu_pipeline(SpeedChromaCudaState *s, CudaFunctions *cu_f, CUdeviceptr d_plane,
                            CUdeviceptr d_indterm, float *h_plane, size_t plane_op_bytes)
{
    /* Upload operating-resolution plane to device (synchronous for simplicity;
     * the CPU eigendecomp below is the latency-hiding opportunity). */
    CHECK_CUDA_RETURN(cu_f, cuMemcpyHtoDAsync(d_plane, h_plane, plane_op_bytes, s->stream));

    const uint32_t num_blocks = (uint32_t)s->dim.num_blocks;
    const uint32_t num_blocks_h = (uint32_t)s->dim.num_blocks_horizontal;
    const uint32_t op_w = (uint32_t)s->dim.truncated_width;
    const uint32_t stride_px = (uint32_t)(s->float_stride / sizeof(float));
    const uint32_t submatrix_w = (uint32_t)s->dim.submatrix_width;
    const uint32_t submatrix_h = (uint32_t)s->dim.submatrix_height;

    const int err = sc_launch_plane_kernels(s, cu_f, d_plane, d_indterm, num_blocks, num_blocks_h,
                                            op_w, stride_px, submatrix_w, submatrix_h);
    if (err)
        return err;

    /* Both downloads the host pass needs — the covariance matrix for the
     * eigendecomposition and the independent term for the Q^T multiply — are
     * enqueued before the one stall that waits for them. They used to be a
     * copy, a stall, a copy and a second stall: a whole extra device round trip
     * per plane per frame for ordering the stream already gives. */
    CHECK_CUDA_RETURN(cu_f,
                      cuMemcpyDtoHAsync(s->h_cov_mat, s->d_cov_mat,
                                        SC_ELEMENTS * SC_ELEMENTS * sizeof(float), s->stream));
    const size_t indterm_bytes = (size_t)SC_ELEMENTS * num_blocks * sizeof(float);
    CHECK_CUDA_RETURN(cu_f, cuMemcpyDtoHAsync((void *)(uintptr_t)((d_indterm == s->d_indterm_ref) ?
                                                                      s->h_indterm_ref :
                                                                      s->h_indterm_dis),
                                              d_indterm, indterm_bytes, s->stream));
    CHECK_CUDA_RETURN(cu_f, cuStreamSynchronize(s->stream));

    return 0;
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
    int _cuda_err = 0;
    const uint32_t warps_per_block =
        (u_nb < SC_SOLVE_WARPS_PER_BLOCK) ? (u_nb ? u_nb : 1u) : SC_SOLVE_WARPS_PER_BLOCK;
    const uint32_t threads = warps_per_block * SC_SOLVE_WARP;
    const uint32_t blocks = (u_nb + warps_per_block - 1u) / warps_per_block;
    void *args[] = {(void *)&s->d_R, (void *)&d_sol, (void *)&u_nb};

    CHECK_CUDA_GOTO(
        cu_f,
        cuLaunchKernel(s->func_solve, blocks, 1u, 1u, threads, 1u, 1u, 0u, s->stream, args, NULL),
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
    int _cuda_err = 0;

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
        _cuda_err = launch_backward_substitution(s, cu_f, d_sol, (uint32_t)nb);
        if (_cuda_err)
            return _cuda_err;
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

/* sc_run_score_kernel - the per-tile entropy/variance kernel plus the four
 * D2H copies it feeds.
 *
 * HISS-04: lifted verbatim out of run_score_and_collect. Inside a helper the
 * macro is CHECK_CUDA_RETURN rather than CHECK_CUDA_GOTO; the former `fail`
 * label did nothing but return `_cuda_err`.
 */
static int sc_run_score_kernel(SpeedChromaCudaState *s, CudaFunctions *cu_f, uint32_t num_blocks,
                               const float *sigma_nn)
{
    /* GPU Kernel 5: per-tile entropy + variance. */
    {
        const uint32_t grid = (num_blocks + SC_SCORE_BLOCK - 1u) / SC_SCORE_BLOCK;
        void *args[] = {
            (void *)&s->d_eigenvalues_ref, (void *)&s->d_eigenvalues,   (void *)&s->d_sol_ref,
            (void *)&s->d_sol_dis,         (void *)&s->d_indterm_ref,   (void *)&s->d_indterm_dis,
            (void *)&s->d_ref_entropies,   (void *)&s->d_ref_variances, (void *)&s->d_dis_entropies,
            (void *)&s->d_dis_variances,   (void *)&num_blocks,         (void *)sigma_nn};
        CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(s->func_score, grid, 1u, 1u, SC_SCORE_BLOCK, 1u, 1u,
                                               0u, s->stream, args, NULL));
    }

    /* D2H: four arrays of num_blocks floats. */
    const size_t ab = (size_t)num_blocks * sizeof(float);
    CHECK_CUDA_RETURN(cu_f,
                      cuMemcpyDtoHAsync(s->h_ref_entropies, s->d_ref_entropies, ab, s->stream));
    CHECK_CUDA_RETURN(cu_f,
                      cuMemcpyDtoHAsync(s->h_ref_variances, s->d_ref_variances, ab, s->stream));
    CHECK_CUDA_RETURN(cu_f,
                      cuMemcpyDtoHAsync(s->h_dis_entropies, s->d_dis_entropies, ab, s->stream));
    CHECK_CUDA_RETURN(cu_f,
                      cuMemcpyDtoHAsync(s->h_dis_variances, s->d_dis_variances, ab, s->stream));
    CHECK_CUDA_RETURN(cu_f, cuStreamSynchronize(s->stream));
    return 0;
}

/* sc_aggregate_score - the host-side score aggregation.
 *
 * HISS-04: lifted verbatim out of run_score_and_collect; every accumulation
 * statement is unchanged and stays whole inside this function.
 */
static float sc_aggregate_score(const SpeedChromaCudaState *s, uint32_t num_blocks)
{
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

    return total / (float)num_blocks;
}

static int run_score_and_collect(SpeedChromaCudaState *s, CudaFunctions *cu_f, float *score_out)
{
    const uint32_t num_blocks = (uint32_t)s->dim.num_blocks;
    const float sigma_nn = (float)s->opt.speed_sigma_nn;

    const int err = sc_run_score_kernel(s, cu_f, num_blocks, &sigma_nn);
    if (err)
        return err;

    *score_out = sc_aggregate_score(s, num_blocks);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Extract one channel (U or V) and return the score                   */
/* ------------------------------------------------------------------ */

/* sc_prepare_planes - download both chroma planes to host staging, then CPU
 * copy + Gaussian filter + downscale each into its float plane.
 *
 * HISS-04: lifted verbatim out of extract_channel; the scratch allocations,
 * the failure frees and the filter order are unchanged.
 */
static int sc_prepare_planes(SpeedChromaCudaState *s, CudaFunctions *cu_f, VmafPicture *ref_pic,
                             VmafPicture *dist_pic, int channel)
{
    /* Allocate a local tmp buffer for filter_and_downscale. */
    const size_t stride_px = s->float_stride / sizeof(float);
    const size_t tmp_size = 2u * s->dim.alloc_height * stride_px;
    float *tmp_filter = (float *)aligned_malloc(tmp_size * sizeof(float), 32);
    if (!tmp_filter)
        return -ENOMEM;

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
    return 0;
}

/* sc_run_plane_pass - GPU pipeline plus CPU eigendecomp/QR for one plane.
 *
 * HISS-04: lifted verbatim out of extract_channel; the two calls keep their
 * order, so the shared device buffers are written in the same sequence.
 */
static int sc_run_plane_pass(SpeedChromaCudaState *s, CudaFunctions *cu_f, float *h_plane,
                             CUdeviceptr d_indterm, float *h_indterm, CUdeviceptr d_sol,
                             size_t plane_op_bytes, bool *singular)
{
    const int err = run_gpu_pipeline(s, cu_f, s->d_plane, d_indterm, h_plane, plane_op_bytes);
    if (err)
        return err;
    return run_cpu_linalg(s, cu_f, h_indterm, d_sol, singular);
}

static int extract_channel(SpeedChromaCudaState *s, CudaFunctions *cu_f, VmafPicture *ref_pic,
                           VmafPicture *dist_pic, int channel, float *score_out, bool *singular_out)
{
    int err = sc_prepare_planes(s, cu_f, ref_pic, dist_pic, channel);
    if (err)
        return err;

    /* Operating-resolution plane size in bytes (only the valid region). */
    const size_t stride_px = s->float_stride / sizeof(float);
    const size_t plane_op_bytes = (size_t)s->dim.truncated_height * stride_px * sizeof(float);

    /* Reference: GPU pipeline (means -> cov -> indterm) then CPU eigendecomp
     * + QR. Uploads ref eigenvalues into the shared s->d_eigenvalues buffer. */
    bool singular_ref = false;
    err = sc_run_plane_pass(s, cu_f, s->h_plane_ref, s->d_indterm_ref, s->h_indterm_ref,
                            s->d_sol_ref, plane_op_bytes, &singular_ref);
    if (err)
        return err;

    /* Stash the reference eigenvalues aside before the distorted linalg pass
     * overwrites s->d_eigenvalues. The CPU reference (est_params in speed.c)
     * computes SEPARATE ref and dis covariance + eigenvalues; the score kernel
     * needs both. The copy is enqueued on the same stream as the eigenvalue
     * upload before it, so the stream orders the two and no host stall is
     * needed. */
    CHECK_CUDA_RETURN(cu_f, cuMemcpyDtoDAsync(s->d_eigenvalues_ref, s->d_eigenvalues,
                                              SC_ELEMENTS * sizeof(float), s->stream));

    /* Distorted: same two passes (keeps the DIS covariance in h_cov_mat — no
     * save/restore of the ref covariance). Uploads dis eigenvalues into
     * s->d_eigenvalues. */
    bool singular_dis = false;
    err = sc_run_plane_pass(s, cu_f, s->h_plane_dis, s->d_indterm_dis, s->h_indterm_dis,
                            s->d_sol_dis, plane_op_bytes, &singular_dis);
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

/* speed_chroma_init_unwind - the single teardown path for init_fex_cuda.
 *
 * The stream must quiesce before any memory it may reference is released.
 * Buffer failures retain their owner fields for retry, and the module is
 * unloaded last. The original init error remains authoritative.
 */
static int speed_chroma_init_unwind(SpeedChromaCudaState *s, VmafCudaState *cu_state, int err)
{
    int rc = err;
    const int phase_rc = vmaf_cuda_stream_destroy(cu_state, &s->stream, true);
    if (phase_rc)
        return rc ? rc : phase_rc;

    int e = free_cuda_buffers(s, cu_state);
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

/* sc_chroma_dims - derive chroma plane dimensions from luma + pixel format.
 *
 * HISS-04: lifted verbatim out of init_fex_cuda.
 */
static int sc_chroma_dims(enum VmafPixelFormat pix_fmt, unsigned *cw, unsigned *ch)
{
    switch (pix_fmt) {
    case VMAF_PIX_FMT_UNKNOWN:
    case VMAF_PIX_FMT_YUV400P:
        return -EINVAL;
    case VMAF_PIX_FMT_YUV420P:
        *cw /= 2u;
        *ch /= 2u;
        break;
    case VMAF_PIX_FMT_YUV422P:
        *cw /= 2u;
        break;
    case VMAF_PIX_FMT_YUV444P:
        break;
    }
    return 0;
}

/* sc_fill_options - map the extractor options into SpeedInternalOptions.
 *
 * HISS-04: lifted verbatim out of init_fex_cuda.
 */
static void sc_fill_options(SpeedChromaCudaState *s)
{
    /* Fill options struct from extractor options (set by the option table). */
    s->opt = (SpeedInternalOptions){
        .speed_kernelscale = s->speed_chroma_kernelscale,
        .speed_prescale = s->speed_chroma_prescale,
        .speed_prescale_method = s->speed_chroma_prescale_method,
        .speed_sigma_nn = s->speed_chroma_sigma_nn,
        .speed_nn_floor = s->speed_chroma_nn_floor,
        .speed_weight_var_mode = s->speed_weight_var_mode,
    };
}

/* sc_init_unwind_pop - the body the former `fail_pop` / `fail_after_pop`
 * labels shared, verbatim and in the same order.
 */
static int sc_init_unwind_pop(SpeedChromaCudaState *s, VmafCudaState *cu_state, int cuda_err)
{
    CudaFunctions *const cu_f = cu_state->f;
    const CUresult pop_res = cu_f->cuCtxPopCurrent(NULL);
    if (pop_res != CUDA_SUCCESS)
        (void)cu_f->cuCtxPopCurrent(NULL);
    return speed_chroma_init_unwind(s, cu_state, cuda_err);
}

/* sc_get_kernels - load the PTX module, resolve the kernels, create the
 * stream.
 *
 * HISS-04: lifted verbatim out of init_fex_cuda. Inside a helper the macro is
 * CHECK_CUDA_RETURN rather than CHECK_CUDA_GOTO; the caller routes a non-zero
 * return into sc_init_unwind_pop(), which is the body `fail_pop` ran.
 */
static int sc_get_kernels(SpeedChromaCudaState *s, CudaFunctions *cu_f)
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

/* sc_alloc_buffers - every device allocation plus the pinned host staging.
 *
 * HISS-04: lifted verbatim out of init_fex_cuda; same set, same order.
 */
static int sc_alloc_buffers(SpeedChromaCudaState *s, CudaFunctions *cu_f, size_t plane_alloc,
                            size_t indterm_bytes, size_t cov_bytes, size_t score_bytes)
{
    /* Allocate device buffers. */
#define ALLOC_DPTR(field, sz)                                                                      \
    do {                                                                                           \
        CHECK_CUDA_RETURN(cu_f, cuMemAlloc(&(s->field), (sz)));                                    \
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
        CHECK_CUDA_RETURN(cu_f, cuMemHostAlloc((void **)&(s->field), (sz), 0x01u));                \
    } while (0)

    ALLOC_HOST(h_cov_mat, cov_bytes);
    ALLOC_HOST(h_ref_entropies, score_bytes);
    ALLOC_HOST(h_ref_variances, score_bytes);
    ALLOC_HOST(h_dis_entropies, score_bytes);
    ALLOC_HOST(h_dis_variances, score_bytes);
#undef ALLOC_HOST
    return 0;
}

/* sc_init_cuda - push the context, set the device side up, pop it again.
 *
 * HISS-01 / HISS-04: lifted out of init_fex_cuda. The `fail` and
 * `fail_after_pop` labels keep their CHECK_CUDA_GOTO sites and their bodies;
 * the `fail_pop` body became sc_init_unwind_pop(), reached on exactly the
 * failures that used to jump to it.
 */
static int sc_init_cuda(VmafFeatureExtractor *fex, SpeedChromaCudaState *s, CudaFunctions *cu_f,
                        size_t plane_alloc, size_t indterm_bytes, size_t cov_bytes,
                        size_t score_bytes)
{
    int _cuda_err = 0;
    /* Load PTX and get kernel function handles. */
    CHECK_CUDA_GOTO(cu_f, cuCtxPushCurrent(fex->cu_state->ctx), fail);

    int err = sc_get_kernels(s, cu_f);
    if (err)
        return sc_init_unwind_pop(s, fex->cu_state, err);

    err = sc_alloc_buffers(s, cu_f, plane_alloc, indterm_bytes, cov_bytes, score_bytes);
    if (err)
        return sc_init_unwind_pop(s, fex->cu_state, err);

    CHECK_CUDA_GOTO(cu_f, cuCtxPopCurrent(NULL), fail_after_pop);
    return 0;

fail_after_pop:
    return sc_init_unwind_pop(s, fex->cu_state, _cuda_err);
fail:
    return _cuda_err;
}

/* sc_alloc_host_scratch - the aligned CPU-side scratch buffers.
 *
 * HISS-04: lifted verbatim out of init_fex_cuda; the sizes, the order and the
 * single combined NULL check are unchanged.
 */
static int sc_alloc_host_scratch(SpeedChromaCudaState *s, VmafCudaState *cu_state,
                                 size_t plane_alloc, size_t cov_bytes, size_t indterm_bytes)
{
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
        return speed_chroma_init_unwind(s, cu_state, -ENOMEM);
    }
    return 0;
}

static int init_fex_cuda(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                         unsigned w, unsigned h)
{
    (void)bpc;

    /* Derive chroma plane dimensions from luma dimensions + pixel format. */
    unsigned cw = w;
    unsigned ch = h;
    const int dim_err = sc_chroma_dims(pix_fmt, &cw, &ch);
    if (dim_err)
        return dim_err;

    SpeedChromaCudaState *s = fex->priv;
    CudaFunctions *cu_f = fex->cu_state->f;
    sc_fill_options(s);

    /* Compute SpEED dimensions. */
    int err = speed_internal_init_dimensions(&s->dim, (int)cw, (int)ch, s->opt.speed_prescale);
    if (err)
        return err;

    s->float_stride = speed_internal_float_stride(s->dim.alloc_width);

    const size_t stride_px = s->float_stride / sizeof(float);
    const size_t num_blocks = s->dim.num_blocks;
    const size_t plane_alloc = s->dim.alloc_height * stride_px * sizeof(float);
    const size_t indterm_bytes = SC_ELEMENTS * num_blocks * sizeof(float);
    const size_t cov_bytes = SC_ELEMENTS * SC_ELEMENTS * sizeof(float);
    const size_t score_bytes = num_blocks * sizeof(float);

    err = sc_init_cuda(fex, s, cu_f, plane_alloc, indterm_bytes, cov_bytes, score_bytes);
    if (err)
        return err;

    err = sc_alloc_host_scratch(s, fex->cu_state, plane_alloc, cov_bytes, indterm_bytes);
    if (err)
        return err;

    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (!s->feature_name_dict)
        return speed_chroma_init_unwind(s, fex->cu_state, -ENOMEM);

    return 0;
}

/* Clamp and emit the three chroma scores. Lifted out of extract_fex_cuda() to
 * keep that function inside the 60-LOC limit (HISS-04).
 *
 * The clamps here were `CLIP(x)`, a less-than comparison, and every comparison
 * against NaN is false, so a non-finite score was published as
 * speed_chroma_max_val -- a finite, plausible 1000.0 standing in for a
 * computation that produced no number, and invisible to the parity harness's
 * own isfinite() assertion. speed_internal_clamp_score() checks finiteness
 * first and fails the frame, matching the CPU reference and the brisque.c /
 * y_funque_plus.c convention. Finite scores clamp exactly as before. */
static int append_chroma_scores(SpeedChromaCudaState *s, VmafFeatureCollector *feature_collector,
                                float score_u, float score_v, float score_uv, unsigned index)
{
    const double mxv = s->speed_chroma_max_val;
    double clamped_u = 0.0;
    double clamped_v = 0.0;
    double clamped_uv = 0.0;
    int err = speed_internal_clamp_score(score_u, mxv, index, "speed_chroma_cuda", "speed_chroma_u",
                                         &clamped_u);
    if (err)
        return err;
    err = speed_internal_clamp_score(score_v, mxv, index, "speed_chroma_cuda", "speed_chroma_v",
                                     &clamped_v);
    if (err)
        return err;
    err = speed_internal_clamp_score(score_uv, mxv, index, "speed_chroma_cuda", "speed_chroma_uv",
                                     &clamped_uv);
    if (err)
        return err;

    err |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "Speed_chroma_feature_speed_chroma_u_score",
                                                   clamped_u, index);
    err |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "Speed_chroma_feature_speed_chroma_v_score",
                                                   clamped_v, index);
    err |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "Speed_chroma_feature_speed_chroma_uv_score",
                                                   clamped_uv, index);
    return err;
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
    int _cuda_err = 0;

    CHECK_CUDA_GOTO(cu_f, cuCtxPushCurrent(fex->cu_state->ctx), fail);

    float score_u = 0.0f;
    float score_v = 0.0f;
    bool singular_u = false;
    bool singular_v = false;
    int err_u = extract_channel(s, cu_f, ref_pic, dist_pic, 1, &score_u, &singular_u);
    int err_v = extract_channel(s, cu_f, ref_pic, dist_pic, 2, &score_v, &singular_v);

    CHECK_CUDA_GOTO(cu_f, cuCtxPopCurrent(NULL), fail_after_push);

    /* A hard failure (CUDA API error, allocation failure) fails the frame, and
     * is NOT the singular-matrix condition -- conflating the two is what made
     * ADR-1202's 4K launch failure surface as three silent 0.0 scores on an
     * exit-0 run. Singularity arrives via `singular_u` / `singular_v`. */
    if (err_u)
        return err_u;
    if (err_v)
        return err_v;

    const float score_uv = combine_chroma_uv(score_u, score_v, singular_u, singular_v);

    /* Every clamp here was a less-than comparison, and every comparison
     * against NaN is false, so a non-finite score was published as
     * speed_chroma_max_val -- a finite, plausible 1000.0 standing in for a
     * computation that produced no number, and invisible to the parity
     * harness's own isfinite() assertion. speed_internal_clamp_score() checks
     * finiteness first and fails the frame, matching the CPU reference and
     * the brisque.c / y_funque_plus.c convention. Finite scores clamp exactly
     * as before. */
    return append_chroma_scores(s, feature_collector, score_u, score_v, score_uv, index);

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
    speed_internal_report_singular(&s->singular_tally, "speed_chroma_cuda");
    return speed_chroma_init_unwind(s, fex->cu_state, 0);
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
