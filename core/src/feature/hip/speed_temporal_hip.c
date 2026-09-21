/**
 *  Copyright 2016-2025 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  speed_temporal feature extractor — HIP backend with real on-device
 *  GPU kernels (ADR-0567).
 *
 *  Temporal design: two ping-pong host float buffers hold converted luma
 *  planes.  Each frame the GPU runs the full SpEED pipeline on the
 *  temporal difference (prev − cur) rather than the raw plane — matching
 *  the CPU twin in speed.c.  Frame 0 emits score 0 (no previous frame).
 *
 *  Algorithm split: identical to speed_chroma_hip.c.  See that file and
 *  ADR-0567 for the full GPU/CPU split rationale.
 *
 *  HIP adaptation notes: same as speed_chroma_hip.c.
 */

#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature_name.h"
#include "log.h"
#include "mem.h"
#include "picture.h"
#include "picture_copy.h"

#include "feature/speed_internal.h"
#include "hip/speed_temporal_hip.h"

#ifdef HAVE_HIPCC
#include <hip/hip_runtime_api.h>

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `NULL` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

extern const unsigned char speed_score_hsaco[];
extern const unsigned int speed_score_hsaco_len;
#endif /* HAVE_HIPCC */

#define ST_BLOCK_SIZE (5u)
#define ST_ELEMENTS (25u)
#define ST_COV_BLOCK (256u)
#define ST_MEANS_BLOCK (256u)
#define ST_INDTERM_BLOCK (256u)
#define ST_SCORE_BLOCK (256u)
/* ST_SOLVE_WARP is no longer a compile-time constant; the actual wavefront
 * size is queried at init time from hipDeviceProp_t.warpSize and stored in
 * SpeedTemporalHipState.solve_warp.  This default covers GCN/RDNA1. */
#define ST_SOLVE_WARP_DEFAULT (64u)

#define ST_DEFAULT_SIGMA_NN (0.29)
#define ST_DEFAULT_MAX_VAL (1000.0)
#define ST_DEFAULT_NN_FLOOR (0.0)
#define ST_DEFAULT_KERNELSCALE (1.0)
#define ST_DEFAULT_PRESCALE (1.0)
#define ST_DEFAULT_PRESCALE_METHOD ("nearest")

/* ------------------------------------------------------------------ */
/* Private extractor state                                             */
/* ------------------------------------------------------------------ */

typedef struct SpeedTemporalHipState {
#ifdef HAVE_HIPCC
    hipModule_t module;
    hipFunction_t func_means;
    hipFunction_t func_cov;
    hipFunction_t func_indterm;
    hipFunction_t func_solve;
    hipFunction_t func_score;
    hipStream_t stream;
    unsigned solve_warp; /* actual device wavefront size (32 or 64) */
#endif

    SpeedInternalDimensions dim;
    SpeedInternalOptions opt;
    size_t float_stride;

    /* Ping-pong host luma plane buffers. */
    float *h_ref[2];
    float *h_dis[2];

#ifdef HAVE_HIPCC
    void *d_plane;
    void *d_means;
    void *d_cov_mat;
    void *d_indterm_ref;
    void *d_indterm_dis;
    void *d_sol_ref;
    void *d_sol_dis;
    void *d_R;
    void *d_eigenvalues;     /* dis eigenvalues after the dis linalg pass */
    void *d_eigenvalues_ref; /* ref eigenvalues, stashed before the dis linalg
                              * overwrites the shared d_eigenvalues buffer */
    void *d_ref_ent;
    void *d_ref_var;
    void *d_dis_ent;
    void *d_dis_var;

    float *h_cov_mat;
    float *h_ref_ent;
    float *h_ref_var;
    float *h_dis_ent;
    float *h_dis_var;
#endif /* HAVE_HIPCC */

    float *h_eigenvalues;
    float *h_eig_scratch;
    float *h_Q;
    float *h_R;
    float *h_qr_scratch;
    float *h_indterm_ref;
    float *h_indterm_dis;
    float *h_qt_scratch;

    unsigned frame_index;

    double speed_temporal_kernelscale;
    double speed_temporal_prescale;
    char *speed_temporal_prescale_method;
    double speed_temporal_sigma_nn;
    double speed_temporal_nn_floor;
    double speed_temporal_max_val;
    bool speed_temporal_use_ref_diff;

    VmafDictionary *feature_name_dict;
    /* Singular covariance matrices are counted, not logged per solve. */
    SpeedInternalSingularTally singular_tally;
} SpeedTemporalHipState;

#define SPEED_TEMPORAL_DOUBLE_OPTION(name_, alias_, help_, member_, default_, min_, max_)          \
    {                                                                                              \
        .name = name_,                                                                             \
        .help = help_,                                                                             \
        .alias = alias_,                                                                           \
        .offset = offsetof(SpeedTemporalHipState, member_),                                        \
        .type = VMAF_OPT_TYPE_DOUBLE,                                                              \
        .default_val.d = default_,                                                                 \
        .min = min_,                                                                               \
        .max = max_,                                                                               \
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,                                                      \
    }
#define SPEED_TEMPORAL_STRING_OPTION(name_, alias_, help_, member_, default_)                      \
    {                                                                                              \
        .name = name_,                                                                             \
        .help = help_,                                                                             \
        .alias = alias_,                                                                           \
        .offset = offsetof(SpeedTemporalHipState, member_),                                        \
        .type = VMAF_OPT_TYPE_STRING,                                                              \
        .default_val.s = default_,                                                                 \
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,                                                      \
    }
#define SPEED_TEMPORAL_BOOL_OPTION(name_, alias_, help_, member_)                                  \
    {                                                                                              \
        .name = name_,                                                                             \
        .help = help_,                                                                             \
        .alias = alias_,                                                                           \
        .offset = offsetof(SpeedTemporalHipState, member_),                                        \
        .type = VMAF_OPT_TYPE_BOOL,                                                                \
        .default_val.b = false,                                                                    \
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,                                                      \
    }

static const VmafOption options_temporal[] = {
    SPEED_TEMPORAL_DOUBLE_OPTION("speed_kernelscale", "ks",
                                 "scaling factor for the Gaussian kernel",
                                 speed_temporal_kernelscale, ST_DEFAULT_KERNELSCALE, 0.1, 4.0),
    SPEED_TEMPORAL_DOUBLE_OPTION("speed_prescale", "ps", "scaling factor for the frame",
                                 speed_temporal_prescale, ST_DEFAULT_PRESCALE, 0.1, 4.0),
    SPEED_TEMPORAL_STRING_OPTION("speed_prescale_method", "psm",
                                 "scaling method [nearest, bilinear, bicubic, lanczos4]",
                                 speed_temporal_prescale_method, ST_DEFAULT_PRESCALE_METHOD),
    SPEED_TEMPORAL_DOUBLE_OPTION("speed_sigma_nn", "snn", "standard deviation of neural noise",
                                 speed_temporal_sigma_nn, ST_DEFAULT_SIGMA_NN, 0.1, 2.0),
    SPEED_TEMPORAL_DOUBLE_OPTION("speed_nn_floor", "nnf", "neural noise floor fraction",
                                 speed_temporal_nn_floor, ST_DEFAULT_NN_FLOOR, 0.0, 1.0),
    SPEED_TEMPORAL_DOUBLE_OPTION("speed_max_val", "mxv", "clip output to this maximum",
                                 speed_temporal_max_val, ST_DEFAULT_MAX_VAL, 0.0, 1000.0),
    SPEED_TEMPORAL_BOOL_OPTION("speed_use_ref_diff", "urd",
                               "use reference frame difference instead of distorted",
                               speed_temporal_use_ref_diff),
    {0}};

/* ------------------------------------------------------------------ */
/* HIP helpers                                                         */
/* ------------------------------------------------------------------ */

static void subtract_plane(float *a, const float *b, int w, int h, size_t stride_bytes)
{
    const size_t stride_px = stride_bytes / sizeof(float);
    for (int i = 0; i < h; i++) {
        for (int j = 0; j < w; j++)
            a[(size_t)i * stride_px + (size_t)j] -= b[(size_t)i * stride_px + (size_t)j];
    }
}

#ifdef HAVE_HIPCC

static int hip_rc_st(hipError_t rc)
{
    if (rc == hipSuccess)
        return 0;
    switch (rc) {
    case hipErrorOutOfMemory:
        return -ENOMEM;
    case hipErrorInvalidValue:
        return -EINVAL;
    case hipErrorNoDevice:
        return -ENODEV;
    case hipErrorNotSupported:
        return -ENOSYS;
    default:
        return -EIO;
    }
}

static void record_st_hip_error(int *first_err, hipError_t rc, const char *operation)
{
    if (rc == hipSuccess)
        return;
    vmaf_log(VMAF_LOG_LEVEL_ERROR, "speed_temporal_hip %s failed with HIP error %d\n", operation,
             (int)rc);
    if (!*first_err)
        *first_err = hip_rc_st(rc);
}

static void release_st_device_pointer(void **ptr, int *first_err, const char *name)
{
    if (!*ptr)
        return;
    record_st_hip_error(first_err, hipFree(*ptr), name);
    *ptr = NULL;
}

static void release_st_host_pointer(float **ptr, int *first_err, const char *name)
{
    if (!*ptr)
        return;
    record_st_hip_error(first_err, hipHostFree(*ptr), name);
    *ptr = NULL;
}

static int free_hip_buffers_st(SpeedTemporalHipState *s, bool synchronize)
{
    int first_err = 0;
    if (synchronize && s->stream)
        record_st_hip_error(&first_err, hipStreamSynchronize(s->stream), "stream synchronize");
    release_st_device_pointer(&s->d_plane, &first_err, "plane free");
    release_st_device_pointer(&s->d_means, &first_err, "means free");
    release_st_device_pointer(&s->d_cov_mat, &first_err, "covariance free");
    release_st_device_pointer(&s->d_indterm_ref, &first_err, "reference indterm free");
    release_st_device_pointer(&s->d_indterm_dis, &first_err, "distorted indterm free");
    release_st_device_pointer(&s->d_sol_ref, &first_err, "reference solution free");
    release_st_device_pointer(&s->d_sol_dis, &first_err, "distorted solution free");
    release_st_device_pointer(&s->d_R, &first_err, "R matrix free");
    release_st_device_pointer(&s->d_eigenvalues, &first_err, "eigenvalues free");
    release_st_device_pointer(&s->d_eigenvalues_ref, &first_err, "reference eigenvalues free");
    release_st_device_pointer(&s->d_ref_ent, &first_err, "reference entropy free");
    release_st_device_pointer(&s->d_ref_var, &first_err, "reference variance free");
    release_st_device_pointer(&s->d_dis_ent, &first_err, "distorted entropy free");
    release_st_device_pointer(&s->d_dis_var, &first_err, "distorted variance free");
    release_st_host_pointer(&s->h_cov_mat, &first_err, "host covariance free");
    release_st_host_pointer(&s->h_ref_ent, &first_err, "host reference entropy free");
    release_st_host_pointer(&s->h_ref_var, &first_err, "host reference variance free");
    release_st_host_pointer(&s->h_dis_ent, &first_err, "host distorted entropy free");
    release_st_host_pointer(&s->h_dis_var, &first_err, "host distorted variance free");
    if (s->module) {
        record_st_hip_error(&first_err, hipModuleUnload(s->module), "module unload");
        s->module = NULL;
    }
    if (s->stream) {
        record_st_hip_error(&first_err, hipStreamDestroy(s->stream), "stream destroy");
        s->stream = NULL;
    }
    return first_err;
}

static int st_hip_module_load(SpeedTemporalHipState *s)
{
    int err = hip_rc_st(hipModuleLoadData(&s->module, speed_score_hsaco));
    if (!err)
        err = hip_rc_st(hipModuleGetFunction(&s->func_means, s->module, "speed_means_hip_kernel"));
    if (!err)
        err = hip_rc_st(hipModuleGetFunction(&s->func_cov, s->module, "speed_cov_hip_kernel"));
    if (!err)
        err = hip_rc_st(
            hipModuleGetFunction(&s->func_indterm, s->module, "speed_indterm_hip_kernel"));
    if (!err)
        err = hip_rc_st(hipModuleGetFunction(&s->func_solve, s->module, "speed_solve_hip_kernel"));
    if (!err)
        err = hip_rc_st(hipModuleGetFunction(&s->func_score, s->module, "speed_score_hip_kernel"));
    return err;
}

static int st_hip_bufs_alloc(SpeedTemporalHipState *s)
{
    const size_t stride_px = s->float_stride / sizeof(float);
    const size_t nb = s->dim.num_blocks;
    const size_t plane_bytes = s->dim.alloc_height * stride_px * sizeof(float);
    const size_t indterm_bytes = ST_ELEMENTS * nb * sizeof(float);
    const size_t cov_bytes = ST_ELEMENTS * ST_ELEMENTS * sizeof(float);
    const size_t score_bytes = nb * sizeof(float);

    int err = hip_rc_st(hipMalloc(&s->d_plane, plane_bytes));
    if (!err)
        err = hip_rc_st(hipMalloc(&s->d_means, indterm_bytes));
    if (!err)
        err = hip_rc_st(hipMalloc(&s->d_cov_mat, cov_bytes));
    if (!err)
        err = hip_rc_st(hipMalloc(&s->d_indterm_ref, indterm_bytes));
    if (!err)
        err = hip_rc_st(hipMalloc(&s->d_indterm_dis, indterm_bytes));
    if (!err)
        err = hip_rc_st(hipMalloc(&s->d_sol_ref, indterm_bytes));
    if (!err)
        err = hip_rc_st(hipMalloc(&s->d_sol_dis, indterm_bytes));
    if (!err)
        err = hip_rc_st(hipMalloc(&s->d_R, cov_bytes));
    if (!err)
        err = hip_rc_st(hipMalloc(&s->d_eigenvalues, ST_ELEMENTS * sizeof(float)));
    if (!err)
        err = hip_rc_st(hipMalloc(&s->d_eigenvalues_ref, ST_ELEMENTS * sizeof(float)));
    if (!err)
        err = hip_rc_st(hipMalloc(&s->d_ref_ent, score_bytes));
    if (!err)
        err = hip_rc_st(hipMalloc(&s->d_ref_var, score_bytes));
    if (!err)
        err = hip_rc_st(hipMalloc(&s->d_dis_ent, score_bytes));
    if (!err)
        err = hip_rc_st(hipMalloc(&s->d_dis_var, score_bytes));
    if (!err)
        err = hip_rc_st(hipHostMalloc((void **)&s->h_cov_mat, cov_bytes, 0));
    if (!err)
        err = hip_rc_st(hipHostMalloc((void **)&s->h_ref_ent, score_bytes, 0));
    if (!err)
        err = hip_rc_st(hipHostMalloc((void **)&s->h_ref_var, score_bytes, 0));
    if (!err)
        err = hip_rc_st(hipHostMalloc((void **)&s->h_dis_ent, score_bytes, 0));
    if (!err)
        err = hip_rc_st(hipHostMalloc((void **)&s->h_dis_var, score_bytes, 0));
    return err;
}

static int run_gpu_pipeline_st(SpeedTemporalHipState *s, const float *h_plane, void *d_indterm,
                               float *h_indterm)
{
    const uint32_t num_blocks = (uint32_t)s->dim.num_blocks;
    const uint32_t num_blocks_h = (uint32_t)s->dim.num_blocks_horizontal;
    const uint32_t op_w = (uint32_t)s->dim.truncated_width;
    const uint32_t stride_px = (uint32_t)(s->float_stride / sizeof(float));
    const uint32_t submatrix_w = (uint32_t)s->dim.submatrix_width;
    const uint32_t submatrix_h = (uint32_t)s->dim.submatrix_height;
    const size_t plane_bytes = s->dim.truncated_height * stride_px * sizeof(float);
    const size_t indterm_bytes = (size_t)ST_ELEMENTS * num_blocks * sizeof(float);

    hipError_t rc =
        hipMemcpyAsync(s->d_plane, h_plane, plane_bytes, hipMemcpyHostToDevice, s->stream);
    if (rc != hipSuccess)
        return hip_rc_st(rc);

    {
        const uint32_t grid_x = (num_blocks + ST_MEANS_BLOCK - 1u) / ST_MEANS_BLOCK;
        void *args[] = {(void *)&s->d_plane,  (void *)&s->d_means,   (void *)&op_w,
                        (void *)&stride_px,   (void *)&num_blocks_h, (void *)&num_blocks,
                        (void *)&submatrix_w, (void *)&submatrix_h};
        rc = hipModuleLaunchKernel(s->func_means, grid_x, 1u, 1u, ST_MEANS_BLOCK, 1u, 1u, 0u,
                                   s->stream, args, NULL);
        if (rc != hipSuccess)
            return hip_rc_st(rc);
    }
    {
        const size_t smem = ST_COV_BLOCK * sizeof(double);
        void *args[] = {(void *)&s->d_plane,  (void *)&s->d_means,   (void *)&s->d_cov_mat,
                        (void *)&stride_px,   (void *)&num_blocks_h, (void *)&num_blocks,
                        (void *)&submatrix_w, (void *)&submatrix_h};
        rc = hipModuleLaunchKernel(s->func_cov, ST_ELEMENTS, ST_ELEMENTS, 1u, ST_COV_BLOCK, 1u, 1u,
                                   (uint32_t)smem, s->stream, args, NULL);
        if (rc != hipSuccess)
            return hip_rc_st(rc);
    }
    {
        const uint32_t total = ST_ELEMENTS * num_blocks;
        const uint32_t grid_x = (total + ST_INDTERM_BLOCK - 1u) / ST_INDTERM_BLOCK;
        void *args[] = {(void *)&s->d_plane, (void *)&d_indterm, (void *)&stride_px,
                        (void *)&num_blocks_h, (void *)&num_blocks};
        rc = hipModuleLaunchKernel(s->func_indterm, grid_x, 1u, 1u, ST_INDTERM_BLOCK, 1u, 1u, 0u,
                                   s->stream, args, NULL);
        if (rc != hipSuccess)
            return hip_rc_st(rc);
    }

    rc = hipMemcpyAsync(s->h_cov_mat, s->d_cov_mat, ST_ELEMENTS * ST_ELEMENTS * sizeof(float),
                        hipMemcpyDeviceToHost, s->stream);
    if (rc != hipSuccess)
        return hip_rc_st(rc);
    rc = hipMemcpyAsync(h_indterm, d_indterm, indterm_bytes, hipMemcpyDeviceToHost, s->stream);
    if (rc != hipSuccess)
        return hip_rc_st(rc);
    rc = hipStreamSynchronize(s->stream);
    return hip_rc_st(rc);
}

/* `singular_out` reports a singular covariance matrix, which is NOT a failure:
 * the CPU reference zeroes the solution and reports it separately so the caller
 * can apply the one-sided-zero rule in speed_extract_score(). The return value
 * stays reserved for hard HIP failures. Mirrors the chroma twin (ADR-1202) and
 * ADR-1218. */
static int run_cpu_linalg_st(SpeedTemporalHipState *s, float *h_indterm, void *d_sol,
                             bool *singular_out)
{
    const int sz = (int)ST_ELEMENTS;
    const int nb = (int)s->dim.num_blocks;
    const size_t indterm_bytes = (size_t)ST_ELEMENTS * (size_t)nb * sizeof(float);

    speed_internal_compute_eigenvalues(s->h_cov_mat, s->h_eigenvalues, sz, s->h_eig_scratch);
    bool regular = speed_internal_is_matrix_regular(s->h_eigenvalues, (size_t)sz);

    hipError_t rc = hipSuccess;
    *singular_out = !regular;
    speed_internal_tally_solve(&s->singular_tally, !regular, "speed_temporal_hip");
    if (!regular) {
        /* Zero the DEVICE solution, not the host staging buffer. The score
         * kernel reads `d_sol`; the host `h_indterm` is re-downloaded from
         * `d_indterm` at the top of every pipeline run, so zeroing it changed
         * nothing. Without this, a singular frame scored against the previous
         * frame's solution — or, on the first frame, against whatever the
         * device allocator handed back. ADR-1218. */
        rc = hipMemsetAsync(d_sol, 0, indterm_bytes, s->stream);
        if (rc != hipSuccess)
            return hip_rc_st(rc);
    } else {
        const int qr_err =
            speed_internal_qr_factorize(s->h_cov_mat, sz, s->h_Q, s->h_R, s->h_qr_scratch);
        if (qr_err)
            return qr_err;
        speed_internal_qt_multiply(s->h_Q, h_indterm, sz, nb, s->h_qt_scratch);

        rc = hipMemcpyAsync(s->d_R, s->h_R, (size_t)sz * (size_t)sz * sizeof(float),
                            hipMemcpyHostToDevice, s->stream);
        if (rc != hipSuccess)
            return hip_rc_st(rc);
        rc = hipMemcpyAsync(d_sol, h_indterm, indterm_bytes, hipMemcpyHostToDevice, s->stream);
        if (rc != hipSuccess)
            return hip_rc_st(rc);

        /* K4: backward substitution — one wavefront per column.
         * blockDim.x = s->solve_warp (32 on RDNA2+, 64 on GCN/RDNA1). */
        const uint32_t u_nb = (uint32_t)nb;
        void *args[] = {(void *)&s->d_R, (void *)&d_sol, (void *)&u_nb};
        rc = hipModuleLaunchKernel(s->func_solve, u_nb, 1u, 1u, s->solve_warp, 1u, 1u, 0u,
                                   s->stream, args, NULL);
        if (rc != hipSuccess)
            return hip_rc_st(rc);
        rc = hipStreamSynchronize(s->stream);
        if (rc != hipSuccess)
            return hip_rc_st(rc);
    }

    /* H2D eigenvalues for the score kernel. Each pass uploads its own
     * eigenvalues into the shared d_eigenvalues buffer; the caller stashes the
     * ref eigenvalues into d_eigenvalues_ref between the ref and dis passes. */
    rc = hipMemcpyAsync(s->d_eigenvalues, s->h_eigenvalues, ST_ELEMENTS * sizeof(float),
                        hipMemcpyHostToDevice, s->stream);
    if (rc != hipSuccess)
        return hip_rc_st(rc);
    rc = hipStreamSynchronize(s->stream);
    return hip_rc_st(rc);
}

static int launch_score_st(SpeedTemporalHipState *s, uint32_t num_blocks)
{
    const float sigma_nn = (float)s->opt.speed_sigma_nn;
    const uint32_t grid = (num_blocks + ST_SCORE_BLOCK - 1u) / ST_SCORE_BLOCK;
    void *args[] = {
        (void *)&s->d_eigenvalues_ref, (void *)&s->d_eigenvalues, (void *)&s->d_sol_ref,
        (void *)&s->d_sol_dis,         (void *)&s->d_indterm_ref, (void *)&s->d_indterm_dis,
        (void *)&s->d_ref_ent,         (void *)&s->d_ref_var,     (void *)&s->d_dis_ent,
        (void *)&s->d_dis_var,         (void *)&num_blocks,       (void *)&sigma_nn,
    };
    return hip_rc_st(hipModuleLaunchKernel(s->func_score, grid, 1u, 1u, ST_SCORE_BLOCK, 1u, 1u, 0u,
                                           s->stream, args, NULL));
}

static int download_score_st(SpeedTemporalHipState *s, uint32_t num_blocks)
{
    const size_t ab = (size_t)num_blocks * sizeof(float);
    int err =
        hip_rc_st(hipMemcpyAsync(s->h_ref_ent, s->d_ref_ent, ab, hipMemcpyDeviceToHost, s->stream));
    if (!err)
        err = hip_rc_st(
            hipMemcpyAsync(s->h_ref_var, s->d_ref_var, ab, hipMemcpyDeviceToHost, s->stream));
    if (!err)
        err = hip_rc_st(
            hipMemcpyAsync(s->h_dis_ent, s->d_dis_ent, ab, hipMemcpyDeviceToHost, s->stream));
    if (!err)
        err = hip_rc_st(
            hipMemcpyAsync(s->h_dis_var, s->d_dis_var, ab, hipMemcpyDeviceToHost, s->stream));
    if (!err)
        err = hip_rc_st(hipStreamSynchronize(s->stream));
    return err;
}

static float aggregate_score_st(const SpeedTemporalHipState *s, uint32_t num_blocks)
{
    const float base_entropy =
        (float)ST_ELEMENTS *
        (log2f((1.0f + (float)s->opt.speed_nn_floor) * (float)s->opt.speed_sigma_nn) +
         log2f(2.0f * 3.14159265358979323846f * 2.71828182845904523536f));
    float total = 0.0f;
    for (uint32_t i = 0; i < num_blocks; ++i) {
        const float re = s->h_ref_ent[i];
        const float de = s->h_dis_ent[i];
        if (re < base_entropy && de < base_entropy)
            continue;
        const float rv = s->h_ref_var[i];
        const float dv = s->h_dis_var[i];
        total += fabsf(re * log2f(1.0f + rv) - de * log2f(1.0f + dv));
    }
    return total / (float)num_blocks;
}

static int run_score_st(SpeedTemporalHipState *s, float *score_out)
{
    const uint32_t num_blocks = (uint32_t)s->dim.num_blocks;
    int err = launch_score_st(s, num_blocks);
    if (!err)
        err = download_score_st(s, num_blocks);
    if (!err)
        *score_out = aggregate_score_st(s, num_blocks);
    return err;
}

#endif /* HAVE_HIPCC */

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

static void free_temporal_cpu_buffers(SpeedTemporalHipState *s)
{
    aligned_free(s->h_ref[0]);
    aligned_free(s->h_ref[1]);
    aligned_free(s->h_dis[0]);
    aligned_free(s->h_dis[1]);
    aligned_free(s->h_eigenvalues);
    aligned_free(s->h_eig_scratch);
    aligned_free(s->h_Q);
    aligned_free(s->h_R);
    aligned_free(s->h_qr_scratch);
    aligned_free(s->h_indterm_ref);
    aligned_free(s->h_indterm_dis);
    aligned_free(s->h_qt_scratch);
    s->h_ref[0] = NULL;
    s->h_ref[1] = NULL;
    s->h_dis[0] = NULL;
    s->h_dis[1] = NULL;
    s->h_eigenvalues = NULL;
    s->h_eig_scratch = NULL;
    s->h_Q = NULL;
    s->h_R = NULL;
    s->h_qr_scratch = NULL;
    s->h_indterm_ref = NULL;
    s->h_indterm_dis = NULL;
    s->h_qt_scratch = NULL;
}

static int init_temporal_dimensions(SpeedTemporalHipState *s, unsigned w, unsigned h)
{
    s->opt = (SpeedInternalOptions){
        .speed_kernelscale = s->speed_temporal_kernelscale,
        .speed_prescale = s->speed_temporal_prescale,
        .speed_prescale_method = s->speed_temporal_prescale_method,
        .speed_sigma_nn = s->speed_temporal_sigma_nn,
        .speed_nn_floor = s->speed_temporal_nn_floor,
        .speed_weight_var_mode = 0,
    };
    const int err = speed_internal_init_dimensions(&s->dim, (int)w, (int)h, s->opt.speed_prescale);
    if (err)
        return err;
    s->float_stride = speed_internal_float_stride(s->dim.alloc_width);
    return s->float_stride ? 0 : -EOVERFLOW;
}

static int allocate_temporal_cpu_buffers(SpeedTemporalHipState *s)
{
    if (s->dim.alloc_height > SIZE_MAX / s->float_stride ||
        s->dim.num_blocks > SIZE_MAX / (ST_ELEMENTS * sizeof(float)))
        return -EOVERFLOW;
    const size_t plane_bytes = s->dim.alloc_height * s->float_stride;
    const size_t indterm_bytes = ST_ELEMENTS * s->dim.num_blocks * sizeof(float);
    const size_t cov_bytes = ST_ELEMENTS * ST_ELEMENTS * sizeof(float);
    s->h_ref[0] = aligned_malloc(plane_bytes, 32);
    s->h_ref[1] = aligned_malloc(plane_bytes, 32);
    s->h_dis[0] = aligned_malloc(plane_bytes, 32);
    s->h_dis[1] = aligned_malloc(plane_bytes, 32);
    s->h_eigenvalues = aligned_malloc(ST_ELEMENTS * sizeof(float), 32);
    s->h_eig_scratch =
        aligned_malloc((ST_ELEMENTS * ST_ELEMENTS + 4u * ST_ELEMENTS) * sizeof(float), 32);
    s->h_Q = aligned_malloc(cov_bytes, 32);
    s->h_R = aligned_malloc(cov_bytes, 32);
    s->h_qr_scratch = aligned_malloc(4u * cov_bytes, 32);
    s->h_indterm_ref = aligned_malloc(indterm_bytes, 32);
    s->h_indterm_dis = aligned_malloc(indterm_bytes, 32);
    s->h_qt_scratch = aligned_malloc(indterm_bytes, 32);
    if (!s->h_ref[0] || !s->h_ref[1] || !s->h_dis[0] || !s->h_dis[1] || !s->h_eigenvalues ||
        !s->h_eig_scratch || !s->h_Q || !s->h_R || !s->h_qr_scratch || !s->h_indterm_ref ||
        !s->h_indterm_dis || !s->h_qt_scratch)
        return -ENOMEM;
    return 0;
}

#ifdef HAVE_HIPCC
static int init_temporal_hip_runtime(SpeedTemporalHipState *s)
{
    int err = st_hip_module_load(s);
    if (!err)
        err = hip_rc_st(hipStreamCreate(&s->stream));
    int device = 0;
    hipDeviceProp_t properties;
    if (!err)
        err = hip_rc_st(hipGetDevice(&device));
    if (!err)
        err = hip_rc_st(hipGetDeviceProperties(&properties, device));
    if (!err && properties.warpSize <= 0)
        err = -EIO;
    if (!err)
        s->solve_warp = (unsigned)properties.warpSize;
    if (!err)
        err = st_hip_bufs_alloc(s);
    return err;
}
#endif

static int init_temporal_hip(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                             unsigned w, unsigned h)
{
    (void)pix_fmt;
    (void)bpc;
    SpeedTemporalHipState *const s = fex->priv;
    int err = init_temporal_dimensions(s, w, h);
#ifndef HAVE_HIPCC
    return err ? err : -ENOSYS;
#else
    if (!err)
        err = allocate_temporal_cpu_buffers(s);
    if (!err)
        err = init_temporal_hip_runtime(s);
    if (!err) {
        s->feature_name_dict =
            vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
        if (!s->feature_name_dict)
            err = -ENOMEM;
    }
    if (err) {
        const int hip_cleanup_err = free_hip_buffers_st(s, false);
        if (hip_cleanup_err)
            vmaf_log(VMAF_LOG_LEVEL_ERROR, "speed_temporal_hip initialization cleanup failed: %d\n",
                     hip_cleanup_err);
        free_temporal_cpu_buffers(s);
    } else {
        s->frame_index = 0;
    }
    return err;
#endif
}

#ifdef HAVE_HIPCC
static int prepare_temporal_diffs(SpeedTemporalHipState *s, int cyclic, int other)
{
    const int width = (int)s->dim.original_width;
    const int height = (int)s->dim.original_height;
    subtract_plane(s->h_ref[other], s->h_ref[cyclic], width, height, s->float_stride);
    const float *const comparison =
        s->speed_temporal_use_ref_diff ? s->h_ref[cyclic] : s->h_dis[cyclic];
    subtract_plane(s->h_dis[other], comparison, width, height, s->float_stride);
    if (s->dim.alloc_height > SIZE_MAX / s->float_stride / 2u)
        return -EOVERFLOW;
    const size_t tmp_bytes = 2u * s->dim.alloc_height * s->float_stride;
    float *const tmp_filter = aligned_malloc(tmp_bytes, 32);
    if (!tmp_filter)
        return -ENOMEM;
    speed_internal_filter_and_downscale(&s->dim, &s->opt, s->h_ref[other], tmp_filter,
                                        s->float_stride);
    speed_internal_filter_and_downscale(&s->dim, &s->opt, s->h_dis[other], tmp_filter,
                                        s->float_stride);
    aligned_free(tmp_filter);
    return 0;
}

static int score_temporal_diffs(SpeedTemporalHipState *s, int other, float *score)
{
    int err = run_gpu_pipeline_st(s, s->h_ref[other], s->d_indterm_ref, s->h_indterm_ref);
    bool singular_ref = false;
    if (!err)
        err = run_cpu_linalg_st(s, s->h_indterm_ref, s->d_sol_ref, &singular_ref);
    if (!err)
        err = hip_rc_st(
            hipMemcpyDtoD(s->d_eigenvalues_ref, s->d_eigenvalues, ST_ELEMENTS * sizeof(float)));
    bool singular_dis = false;
    if (!err)
        err = run_gpu_pipeline_st(s, s->h_dis[other], s->d_indterm_dis, s->h_indterm_dis);
    if (!err)
        err = run_cpu_linalg_st(s, s->h_indterm_dis, s->d_sol_dis, &singular_dis);
    *score = 0.0f;
    if (!err && singular_ref == singular_dis)
        err = run_score_st(s, score);
    return err;
}
#endif

static int extract_temporal_hip(VmafFeatureExtractor *fex, VmafPicture *ref_pic,
                                VmafPicture *ref_pic_90, VmafPicture *dist_pic,
                                VmafPicture *dist_pic_90, unsigned index,
                                VmafFeatureCollector *feature_collector)
{
    (void)ref_pic_90;
    (void)dist_pic_90;

#ifndef HAVE_HIPCC
    (void)fex;
    (void)ref_pic;
    (void)dist_pic;
    (void)index;
    (void)feature_collector;
    return -ENOSYS;
#else
    SpeedTemporalHipState *const s = fex->priv;
    const int cyclic = (int)(index % 2u);
    const int other = (int)((index + 1u) % 2u);
    picture_copy(s->h_ref[cyclic], s->float_stride, ref_pic, -128, ref_pic->bpc, 0);
    picture_copy(s->h_dis[cyclic], s->float_stride, dist_pic, -128, dist_pic->bpc, 0);
    if (index == 0)
        return vmaf_feature_collector_append_with_dict(
            feature_collector, s->feature_name_dict, "Speed_temporal_feature_speed_temporal_score",
            0.0, index);
    int err = prepare_temporal_diffs(s, cyclic, other);
    float score = 0.0f;
    if (!err)
        err = score_temporal_diffs(s, other, &score);
    if (err)
        return err;
    const double mxv = s->speed_temporal_max_val;
    const double clipped = (double)score < mxv ? (double)score : mxv;
    return vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "Speed_temporal_feature_speed_temporal_score",
                                                   clipped, index);
#endif /* HAVE_HIPCC */
}

static int close_temporal_hip(VmafFeatureExtractor *fex)
{
    SpeedTemporalHipState *const s = fex->priv;
    speed_internal_report_singular(&s->singular_tally, "speed_temporal_hip");
    int err = 0;
#ifdef HAVE_HIPCC
    err = free_hip_buffers_st(s, true);
#endif
    free_temporal_cpu_buffers(s);
    const int dict_err = vmaf_dictionary_free(&s->feature_name_dict);
    if (dict_err) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "speed_temporal_hip dictionary free failed: %d\n", dict_err);
        if (!err)
            err = dict_err;
    }
    return err;
}

static const char *provided_features_temporal[] = {
    "Speed_temporal_feature_speed_temporal_score",
    NULL,
};

/* ADR-0567: real HIP GPU kernels for speed_temporal.
 * TEMPORAL flag guarantees sequential frame submission (ping-pong diff
 * requires frame ordering). */
VmafFeatureExtractor vmaf_fex_speed_temporal_hip = {
    .name = "speed_temporal_hip",
    .init = init_temporal_hip,
    .extract = extract_temporal_hip,
    .close = close_temporal_hip,
    .options = options_temporal,
    .priv_size = sizeof(SpeedTemporalHipState),
    .provided_features = provided_features_temporal,
    .flags = VMAF_FEATURE_EXTRACTOR_TEMPORAL | VMAF_FEATURE_EXTRACTOR_HIP,
};

/* NOLINTEND(modernize-use-nullptr) */
