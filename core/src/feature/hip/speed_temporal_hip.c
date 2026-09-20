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

#include "vmaf_nullptr.h"

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

/* lint rationale: C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `VMAF_NULLPTR` and
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

#define SPEED_TEMPORAL_DOUBLE_OPTION(NAME, HELP, FIELD, DEFAULT, MINIMUM, MAXIMUM, ALIAS)          \
    {                                                                                              \
        .name = (NAME),                                                                            \
        .help = (HELP),                                                                            \
        .offset = offsetof(SpeedTemporalHipState, FIELD),                                          \
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
        .offset = offsetof(SpeedTemporalHipState, FIELD),                                          \
        .type = VMAF_OPT_TYPE_STRING,                                                              \
        .default_val.s = (DEFAULT),                                                                \
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,                                                      \
        .alias = (ALIAS),                                                                          \
    }
#define SPEED_TEMPORAL_BOOL_OPTION(NAME, HELP, FIELD, DEFAULT, ALIAS)                              \
    {                                                                                              \
        .name = (NAME),                                                                            \
        .help = (HELP),                                                                            \
        .offset = offsetof(SpeedTemporalHipState, FIELD),                                          \
        .type = VMAF_OPT_TYPE_BOOL,                                                                \
        .default_val.b = (DEFAULT),                                                                \
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,                                                      \
        .alias = (ALIAS),                                                                          \
    }

static const VmafOption options_temporal[] = {
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

static void free_hip_buffers_st(SpeedTemporalHipState *s)
{
#define FD(p)                                                                                      \
    do {                                                                                           \
        if ((p)) {                                                                                 \
            (void)hipFree((p));                                                                    \
            (p) = VMAF_NULLPTR;                                                                    \
        }                                                                                          \
    } while (0)
#define FH(p)                                                                                      \
    do {                                                                                           \
        if ((p)) {                                                                                 \
            (void)hipHostFree((p));                                                                \
            (p) = VMAF_NULLPTR;                                                                    \
        }                                                                                          \
    } while (0)
    FD(s->d_plane);
    FD(s->d_means);
    FD(s->d_cov_mat);
    FD(s->d_indterm_ref);
    FD(s->d_indterm_dis);
    FD(s->d_sol_ref);
    FD(s->d_sol_dis);
    FD(s->d_R);
    FD(s->d_eigenvalues);
    FD(s->d_eigenvalues_ref);
    FD(s->d_ref_ent);
    FD(s->d_ref_var);
    FD(s->d_dis_ent);
    FD(s->d_dis_var);
    FH(s->h_cov_mat);
    FH(s->h_ref_ent);
    FH(s->h_ref_var);
    FH(s->h_dis_ent);
    FH(s->h_dis_var);
    if (s->module) {
        (void)hipModuleUnload(s->module);
        s->module = VMAF_NULLPTR;
    }
    if (s->stream) {
        (void)hipStreamDestroy(s->stream);
        s->stream = VMAF_NULLPTR;
    }
#undef FD
#undef FH
}

static int st_hip_module_load(SpeedTemporalHipState *s)
{
    hipError_t rc = hipModuleLoadData(&s->module, speed_score_hsaco);
    if (rc != hipSuccess)
        return hip_rc_st(rc);

#define GET_FN(field, name)                                                                        \
    do {                                                                                           \
        rc = hipModuleGetFunction(&(s->field), s->module, (name));                                 \
        if (rc != hipSuccess) {                                                                    \
            (void)hipModuleUnload(s->module);                                                      \
            s->module = VMAF_NULLPTR;                                                              \
            return hip_rc_st(rc);                                                                  \
        }                                                                                          \
    } while (0)

    GET_FN(func_means, "speed_means_hip_kernel");
    GET_FN(func_cov, "speed_cov_hip_kernel");
    GET_FN(func_indterm, "speed_indterm_hip_kernel");
    GET_FN(func_solve, "speed_solve_hip_kernel");
    GET_FN(func_score, "speed_score_hip_kernel");
#undef GET_FN
    return 0;
}

static int st_hip_bufs_alloc(SpeedTemporalHipState *s)
{
    const size_t stride_px = s->float_stride / sizeof(float);
    const size_t nb = s->dim.num_blocks;
    const size_t plane_bytes = s->dim.alloc_height * stride_px * sizeof(float);
    const size_t indterm_bytes = ST_ELEMENTS * nb * sizeof(float);
    const size_t cov_bytes = ST_ELEMENTS * ST_ELEMENTS * sizeof(float);
    const size_t score_bytes = nb * sizeof(float);

#define AD(f, sz)                                                                                  \
    do {                                                                                           \
        if (hipMalloc(&(s->f), (sz)) != hipSuccess)                                                \
            return -ENOMEM;                                                                        \
    } while (0)
#define AH(f, sz)                                                                                  \
    do {                                                                                           \
        if (hipHostMalloc((void **)&(s->f), (sz), 0) != hipSuccess)                                \
            return -ENOMEM;                                                                        \
    } while (0)

    AD(d_plane, plane_bytes);
    AD(d_means, indterm_bytes);
    AD(d_cov_mat, cov_bytes);
    AD(d_indterm_ref, indterm_bytes);
    AD(d_indterm_dis, indterm_bytes);
    AD(d_sol_ref, indterm_bytes);
    AD(d_sol_dis, indterm_bytes);
    AD(d_R, cov_bytes);
    AD(d_eigenvalues, ST_ELEMENTS * sizeof(float));
    AD(d_eigenvalues_ref, ST_ELEMENTS * sizeof(float));
    AD(d_ref_ent, score_bytes);
    AD(d_ref_var, score_bytes);
    AD(d_dis_ent, score_bytes);
    AD(d_dis_var, score_bytes);
    AH(h_cov_mat, cov_bytes);
    AH(h_ref_ent, score_bytes);
    AH(h_ref_var, score_bytes);
    AH(h_dis_ent, score_bytes);
    AH(h_dis_var, score_bytes);
#undef AD
#undef AH
    return 0;
}

static int run_gpu_pipeline_st(SpeedTemporalHipState *s, const float *h_plane, void *d_indterm,
                               float *h_indterm)
{
    uint32_t num_blocks = (uint32_t)s->dim.num_blocks;
    uint32_t num_blocks_h = (uint32_t)s->dim.num_blocks_horizontal;
    uint32_t op_w = (uint32_t)s->dim.truncated_width;
    uint32_t stride_px = (uint32_t)(s->float_stride / sizeof(float));
    uint32_t submatrix_w = (uint32_t)s->dim.submatrix_width;
    uint32_t submatrix_h = (uint32_t)s->dim.submatrix_height;
    const size_t plane_bytes = s->dim.truncated_height * stride_px * sizeof(float);
    const size_t indterm_bytes = (size_t)ST_ELEMENTS * num_blocks * sizeof(float);

    hipError_t rc =
        hipMemcpyAsync(s->d_plane, h_plane, plane_bytes, hipMemcpyHostToDevice, s->stream);
    if (rc != hipSuccess)
        return hip_rc_st(rc);

    {
        const uint32_t grid_x = (num_blocks + ST_MEANS_BLOCK - 1u) / ST_MEANS_BLOCK;
        void *args[] = {&s->d_plane,   &s->d_means, &op_w,        &stride_px,
                        &num_blocks_h, &num_blocks, &submatrix_w, &submatrix_h};
        rc = hipModuleLaunchKernel(s->func_means, grid_x, 1u, 1u, ST_MEANS_BLOCK, 1u, 1u, 0u,
                                   s->stream, args, VMAF_NULLPTR);
        if (rc != hipSuccess)
            return hip_rc_st(rc);
    }
    {
        const size_t smem = ST_COV_BLOCK * sizeof(double);
        void *args[] = {&s->d_plane,   &s->d_means, &s->d_cov_mat, &stride_px,
                        &num_blocks_h, &num_blocks, &submatrix_w,  &submatrix_h};
        rc = hipModuleLaunchKernel(s->func_cov, ST_ELEMENTS, ST_ELEMENTS, 1u, ST_COV_BLOCK, 1u, 1u,
                                   (uint32_t)smem, s->stream, args, VMAF_NULLPTR);
        if (rc != hipSuccess)
            return hip_rc_st(rc);
    }
    {
        const uint32_t total = ST_ELEMENTS * num_blocks;
        const uint32_t grid_x = (total + ST_INDTERM_BLOCK - 1u) / ST_INDTERM_BLOCK;
        void *args[] = {&s->d_plane, &d_indterm, &stride_px, &num_blocks_h, &num_blocks};
        rc = hipModuleLaunchKernel(s->func_indterm, grid_x, 1u, 1u, ST_INDTERM_BLOCK, 1u, 1u, 0u,
                                   s->stream, args, VMAF_NULLPTR);
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
        (void)speed_internal_qr_factorize(s->h_cov_mat, sz, s->h_Q, s->h_R, s->h_qr_scratch);
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
        uint32_t u_nb = (uint32_t)nb;
        void *args[] = {&s->d_R, &d_sol, &u_nb};
        rc = hipModuleLaunchKernel(s->func_solve, u_nb, 1u, 1u, s->solve_warp, 1u, 1u, 0u,
                                   s->stream, args, VMAF_NULLPTR);
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

static int run_score_st(SpeedTemporalHipState *s, float *score_out)
{
    uint32_t num_blocks = (uint32_t)s->dim.num_blocks;
    float sigma_nn = (float)s->opt.speed_sigma_nn;
    hipError_t rc = hipSuccess;

    /* K5: entropy + score. The kernel reads d_eigenvalues_ref for the ref
     * entropy and d_eigenvalues (the dis eigenvalues uploaded by the dis
     * run_cpu_linalg_st pass) for the dis entropy. */
    {
        const uint32_t grid = (num_blocks + ST_SCORE_BLOCK - 1u) / ST_SCORE_BLOCK;
        void *args[] = {&s->d_eigenvalues_ref, &s->d_eigenvalues, &s->d_sol_ref, &s->d_sol_dis,
                        &s->d_indterm_ref,     &s->d_indterm_dis, &s->d_ref_ent, &s->d_ref_var,
                        &s->d_dis_ent,         &s->d_dis_var,     &num_blocks,   &sigma_nn};
        rc = hipModuleLaunchKernel(s->func_score, grid, 1u, 1u, ST_SCORE_BLOCK, 1u, 1u, 0u,
                                   s->stream, args, VMAF_NULLPTR);
        if (rc != hipSuccess)
            return hip_rc_st(rc);
    }

    const size_t ab = (size_t)num_blocks * sizeof(float);
    rc = hipMemcpyAsync(s->h_ref_ent, s->d_ref_ent, ab, hipMemcpyDeviceToHost, s->stream);
    if (rc != hipSuccess)
        return hip_rc_st(rc);
    rc = hipMemcpyAsync(s->h_ref_var, s->d_ref_var, ab, hipMemcpyDeviceToHost, s->stream);
    if (rc != hipSuccess)
        return hip_rc_st(rc);
    rc = hipMemcpyAsync(s->h_dis_ent, s->d_dis_ent, ab, hipMemcpyDeviceToHost, s->stream);
    if (rc != hipSuccess)
        return hip_rc_st(rc);
    rc = hipMemcpyAsync(s->h_dis_var, s->d_dis_var, ab, hipMemcpyDeviceToHost, s->stream);
    if (rc != hipSuccess)
        return hip_rc_st(rc);
    rc = hipStreamSynchronize(s->stream);
    if (rc != hipSuccess)
        return hip_rc_st(rc);

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
        /* speed_temporal uses weight_var_mode = 0. */
        total += fabsf(re * log2f(1.0f + rv) - de * log2f(1.0f + dv));
    }
    *score_out = total / (float)num_blocks;
    return 0;
}

static int speed_temporal_init_device(SpeedTemporalHipState *s)
{
    int err = st_hip_module_load(s);
    if (err)
        return err;

    if (hipStreamCreate(&s->stream) != hipSuccess)
        return -EIO;

    int device = 0;
    hipDeviceProp_t properties;
    (void)hipGetDevice(&device);
    s->solve_warp = (hipGetDeviceProperties(&properties, device) == hipSuccess) ?
                        (unsigned)properties.warpSize :
                        ST_SOLVE_WARP_DEFAULT;
    return st_hip_bufs_alloc(s);
}

#endif /* HAVE_HIPCC */

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

static void speed_temporal_set_options(SpeedTemporalHipState *s)
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

static void speed_temporal_free_cpu(SpeedTemporalHipState *s)
{
#define FREE_A(field)                                                                              \
    do {                                                                                           \
        aligned_free(s->field);                                                                    \
        s->field = VMAF_NULLPTR;                                                                   \
    } while (0)
    FREE_A(h_ref[0]);
    FREE_A(h_ref[1]);
    FREE_A(h_dis[0]);
    FREE_A(h_dis[1]);
    FREE_A(h_eigenvalues);
    FREE_A(h_eig_scratch);
    FREE_A(h_Q);
    FREE_A(h_R);
    FREE_A(h_qr_scratch);
    FREE_A(h_indterm_ref);
    FREE_A(h_indterm_dis);
    FREE_A(h_qt_scratch);
#undef FREE_A
}

static int speed_temporal_alloc_cpu(SpeedTemporalHipState *s)
{
    const size_t stride_px = s->float_stride / sizeof(float);
    const size_t plane_bytes = s->dim.alloc_height * stride_px * sizeof(float);
    const size_t indterm_bytes = ST_ELEMENTS * s->dim.num_blocks * sizeof(float);
    const size_t cov_bytes = ST_ELEMENTS * ST_ELEMENTS * sizeof(float);

#define ALLOC_A(field, sz) s->field = (float *)aligned_malloc((sz), 32)
    ALLOC_A(h_ref[0], plane_bytes);
    ALLOC_A(h_ref[1], plane_bytes);
    ALLOC_A(h_dis[0], plane_bytes);
    ALLOC_A(h_dis[1], plane_bytes);
    ALLOC_A(h_eigenvalues, ST_ELEMENTS * sizeof(float));
    ALLOC_A(h_eig_scratch, (ST_ELEMENTS * ST_ELEMENTS + 4u * ST_ELEMENTS) * sizeof(float));
    ALLOC_A(h_Q, cov_bytes);
    ALLOC_A(h_R, cov_bytes);
    ALLOC_A(h_qr_scratch, 4u * cov_bytes);
    ALLOC_A(h_indterm_ref, indterm_bytes);
    ALLOC_A(h_indterm_dis, indterm_bytes);
    ALLOC_A(h_qt_scratch, indterm_bytes);
#undef ALLOC_A

    return s->h_ref[0] && s->h_ref[1] && s->h_dis[0] && s->h_dis[1] && s->h_eigenvalues &&
                   s->h_eig_scratch && s->h_Q && s->h_R && s->h_qr_scratch && s->h_indterm_ref &&
                   s->h_indterm_dis && s->h_qt_scratch ?
               0 :
               -ENOMEM;
}

static void speed_temporal_cleanup_init(SpeedTemporalHipState *s)
{
#ifdef HAVE_HIPCC
    free_hip_buffers_st(s);
#endif
    speed_temporal_free_cpu(s);
}

static int init_temporal_hip(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                             unsigned w, unsigned h)
{
    (void)pix_fmt;
    (void)bpc;
    SpeedTemporalHipState *s = fex->priv;

    speed_temporal_set_options(s);
    int err = speed_internal_init_dimensions(&s->dim, (int)w, (int)h, s->opt.speed_prescale);
    if (err)
        return err;
    s->float_stride = speed_internal_float_stride(s->dim.alloc_width);
    err = speed_temporal_alloc_cpu(s);
    if (err) {
        speed_temporal_cleanup_init(s);
        return err;
    }

#ifdef HAVE_HIPCC
    err = speed_temporal_init_device(s);
#else
    err = -ENOSYS;
#endif /* HAVE_HIPCC */
    if (err) {
        speed_temporal_cleanup_init(s);
        return err;
    }

    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (!s->feature_name_dict) {
        speed_temporal_cleanup_init(s);
        return -ENOMEM;
    }

    s->frame_index = 0;
    return 0;
}

#ifdef HAVE_HIPCC
static void speed_temporal_prepare_diff(SpeedTemporalHipState *s, int current, int previous)
{
    const int width = (int)s->dim.original_width;
    const int height = (int)s->dim.original_height;
    subtract_plane(s->h_ref[previous], s->h_ref[current], width, height, s->float_stride);
    if (s->speed_temporal_use_ref_diff)
        subtract_plane(s->h_dis[previous], s->h_ref[current], width, height, s->float_stride);
    else
        subtract_plane(s->h_dis[previous], s->h_dis[current], width, height, s->float_stride);
}

static int speed_temporal_filter_diff(SpeedTemporalHipState *s, int previous)
{
    const size_t stride_px = s->float_stride / sizeof(float);
    const size_t tmp_size = 2u * s->dim.alloc_height * stride_px;
    float *tmp_filter = (float *)aligned_malloc(tmp_size * sizeof(float), 32);
    if (!tmp_filter)
        return -ENOMEM;

    speed_internal_filter_and_downscale(&s->dim, &s->opt, s->h_ref[previous], tmp_filter,
                                        s->float_stride);
    speed_internal_filter_and_downscale(&s->dim, &s->opt, s->h_dis[previous], tmp_filter,
                                        s->float_stride);
    aligned_free(tmp_filter);
    return 0;
}

static int speed_temporal_score_diff(SpeedTemporalHipState *s, int previous, float *score)
{
    bool singular_ref = false;
    int err = run_gpu_pipeline_st(s, s->h_ref[previous], s->d_indterm_ref, s->h_indterm_ref);
    if (!err)
        err = run_cpu_linalg_st(s, s->h_indterm_ref, s->d_sol_ref, &singular_ref);
    if (err)
        return err;

    const hipError_t copy_err =
        hipMemcpyDtoD(s->d_eigenvalues_ref, s->d_eigenvalues, ST_ELEMENTS * sizeof(float));
    if (copy_err != hipSuccess)
        return hip_rc_st(copy_err);

    bool singular_dis = false;
    err = run_gpu_pipeline_st(s, s->h_dis[previous], s->d_indterm_dis, s->h_indterm_dis);
    if (!err)
        err = run_cpu_linalg_st(s, s->h_indterm_dis, s->d_sol_dis, &singular_dis);
    if (err)
        return err;

    *score = 0.0f;
    return singular_ref == singular_dis ? run_score_st(s, score) : 0;
}
#endif /* HAVE_HIPCC */

static int extract_temporal_hip(VmafFeatureExtractor *fex, const VmafPicture *ref_pic,
                                const VmafPicture *ref_pic_90, const VmafPicture *dist_pic,
                                const VmafPicture *dist_pic_90, unsigned index,
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
    SpeedTemporalHipState *s = fex->priv;

    const int cyclic = (int)(index % 2u);
    const int other = (int)((index + 1u) % 2u);

    picture_copy(s->h_ref[cyclic], s->float_stride, ref_pic, -128, ref_pic->bpc, 0);
    picture_copy(s->h_dis[cyclic], s->float_stride, dist_pic, -128, dist_pic->bpc, 0);
    if (index == 0)
        return vmaf_feature_collector_append_with_dict(
            feature_collector, s->feature_name_dict, "Speed_temporal_feature_speed_temporal_score",
            0.0, index);

    speed_temporal_prepare_diff(s, cyclic, other);
    int err = speed_temporal_filter_diff(s, other);
    if (err)
        return err;

    float score = 0.0f;
    err = speed_temporal_score_diff(s, other, &score);
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
    SpeedTemporalHipState *s = fex->priv;
    speed_internal_report_singular(&s->singular_tally, "speed_temporal_hip");
#ifdef HAVE_HIPCC
    free_hip_buffers_st(s);
#endif
    speed_temporal_free_cpu(s);
    if (s->feature_name_dict)
        vmaf_dictionary_free(&s->feature_name_dict);
    return 0;
}

static const char *provided_features_temporal[] = {
    "Speed_temporal_feature_speed_temporal_score",
    VMAF_NULLPTR,
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
