/**
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
#include <string.h>

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
#define __HIP_PLATFORM_AMD__ 1
#include <hip/hip_runtime_api.h>

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
    void *d_eigenvalues;
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
} SpeedTemporalHipState;

static const VmafOption options_temporal[] = {
    {
        .name = "speed_kernelscale",
        .help = "scaling factor for the Gaussian kernel",
        .offset = offsetof(SpeedTemporalHipState, speed_temporal_kernelscale),
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
        .offset = offsetof(SpeedTemporalHipState, speed_temporal_prescale),
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
        .offset = offsetof(SpeedTemporalHipState, speed_temporal_prescale_method),
        .type = VMAF_OPT_TYPE_STRING,
        .default_val.s = ST_DEFAULT_PRESCALE_METHOD,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "psm",
    },
    {
        .name = "speed_sigma_nn",
        .help = "standard deviation of neural noise",
        .offset = offsetof(SpeedTemporalHipState, speed_temporal_sigma_nn),
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
        .offset = offsetof(SpeedTemporalHipState, speed_temporal_nn_floor),
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
        .offset = offsetof(SpeedTemporalHipState, speed_temporal_max_val),
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
        .offset = offsetof(SpeedTemporalHipState, speed_temporal_use_ref_diff),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = false,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "urd",
    },
    {0},
};

/* ------------------------------------------------------------------ */
/* HIP helpers                                                         */
/* ------------------------------------------------------------------ */

static void subtract_plane(float *a, const float *b, int w, int h, size_t stride_bytes)
{
    const size_t stride_px = stride_bytes / sizeof(float);
    for (int i = 0; i < h; i++)
        for (int j = 0; j < w; j++)
            a[(size_t)i * stride_px + (size_t)j] -= b[(size_t)i * stride_px + (size_t)j];
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
            (p) = NULL;                                                                            \
        }                                                                                          \
    } while (0)
#define FH(p)                                                                                      \
    do {                                                                                           \
        if ((p)) {                                                                                 \
            (void)hipHostFree((p));                                                                \
            (p) = NULL;                                                                            \
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
        s->module = NULL;
    }
    if (s->stream) {
        (void)hipStreamDestroy(s->stream);
        s->stream = NULL;
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
            s->module = NULL;                                                                      \
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
        void *args[] = {&s->d_plane,   &s->d_means, &op_w,        &stride_px,
                        &num_blocks_h, &num_blocks, &submatrix_w, &submatrix_h};
        rc = hipModuleLaunchKernel(s->func_means, grid_x, 1u, 1u, ST_MEANS_BLOCK, 1u, 1u, 0u,
                                   s->stream, args, NULL);
        if (rc != hipSuccess)
            return hip_rc_st(rc);
    }
    {
        const size_t smem = ST_COV_BLOCK * sizeof(double);
        void *args[] = {&s->d_plane,   &s->d_means, &s->d_cov_mat, &stride_px,
                        &num_blocks_h, &num_blocks, &submatrix_w,  &submatrix_h};
        rc = hipModuleLaunchKernel(s->func_cov, ST_ELEMENTS, ST_ELEMENTS, 1u, ST_COV_BLOCK, 1u, 1u,
                                   (uint32_t)smem, s->stream, args, NULL);
        if (rc != hipSuccess)
            return hip_rc_st(rc);
    }
    {
        const uint32_t total = ST_ELEMENTS * num_blocks;
        const uint32_t grid_x = (total + ST_INDTERM_BLOCK - 1u) / ST_INDTERM_BLOCK;
        void *args[] = {&s->d_plane, &d_indterm, &stride_px, &num_blocks_h, &num_blocks};
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

static int run_cpu_linalg_st(SpeedTemporalHipState *s, float *h_indterm, void *d_sol)
{
    const int sz = (int)ST_ELEMENTS;
    const int nb = (int)s->dim.num_blocks;
    const size_t indterm_bytes = (size_t)ST_ELEMENTS * (size_t)nb * sizeof(float);

    speed_internal_compute_eigenvalues(s->h_cov_mat, s->h_eigenvalues, sz, s->h_eig_scratch);
    bool regular = speed_internal_is_matrix_regular(s->h_eigenvalues, (size_t)sz);

    if (!regular) {
        vmaf_log(VMAF_LOG_LEVEL_WARNING,
                 "speed_temporal_hip: covariance matrix singular, zeroing solution\n");
        (void)memset(h_indterm, 0, indterm_bytes);
        return 0;
    }

    (void)speed_internal_qr_factorize(s->h_cov_mat, sz, s->h_Q, s->h_R, s->h_qr_scratch);
    speed_internal_qt_multiply(s->h_Q, h_indterm, sz, nb, s->h_qt_scratch);

    hipError_t rc = hipMemcpyAsync(s->d_R, s->h_R, (size_t)sz * (size_t)sz * sizeof(float),
                                   hipMemcpyHostToDevice, s->stream);
    if (rc != hipSuccess)
        return hip_rc_st(rc);
    rc = hipMemcpyAsync(d_sol, h_indterm, indterm_bytes, hipMemcpyHostToDevice, s->stream);
    if (rc != hipSuccess)
        return hip_rc_st(rc);

    /* K4: backward substitution — one wavefront per column.
     * blockDim.x = s->solve_warp (32 on RDNA2+, 64 on GCN/RDNA1). */
    const uint32_t u_nb = (uint32_t)nb;
    void *args[] = {&s->d_R, &d_sol, &u_nb};
    rc = hipModuleLaunchKernel(s->func_solve, u_nb, 1u, 1u, s->solve_warp, 1u, 1u, 0u, s->stream,
                               args, NULL);
    if (rc != hipSuccess)
        return hip_rc_st(rc);
    rc = hipStreamSynchronize(s->stream);
    return hip_rc_st(rc);
}

static int run_score_st(SpeedTemporalHipState *s, float *score_out)
{
    const uint32_t num_blocks = (uint32_t)s->dim.num_blocks;
    const float sigma_nn = (float)s->opt.speed_sigma_nn;

    hipError_t rc = hipMemcpyAsync(s->d_eigenvalues, s->h_eigenvalues, ST_ELEMENTS * sizeof(float),
                                   hipMemcpyHostToDevice, s->stream);
    if (rc != hipSuccess)
        return hip_rc_st(rc);

    {
        const uint32_t grid = (num_blocks + ST_SCORE_BLOCK - 1u) / ST_SCORE_BLOCK;
        void *args[] = {&s->d_eigenvalues, &s->d_sol_ref, &s->d_sol_dis, &s->d_indterm_ref,
                        &s->d_indterm_dis, &s->d_ref_ent, &s->d_ref_var, &s->d_dis_ent,
                        &s->d_dis_var,     &num_blocks,   &sigma_nn};
        rc = hipModuleLaunchKernel(s->func_score, grid, 1u, 1u, ST_SCORE_BLOCK, 1u, 1u, 0u,
                                   s->stream, args, NULL);
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

#endif /* HAVE_HIPCC */

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

static int init_temporal_hip(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                             unsigned w, unsigned h)
{
    (void)pix_fmt;
    (void)bpc;
    SpeedTemporalHipState *s = fex->priv;

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
    const size_t nb = s->dim.num_blocks;
    const size_t plane_bytes = s->dim.alloc_height * stride_px * sizeof(float);
    const size_t indterm_bytes = ST_ELEMENTS * nb * sizeof(float);
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

    if (!s->h_ref[0] || !s->h_ref[1] || !s->h_dis[0] || !s->h_dis[1] || !s->h_eigenvalues ||
        !s->h_Q || !s->h_R) {
        err = -ENOMEM;
        goto free_cpu;
    }

#ifdef HAVE_HIPCC
    err = st_hip_module_load(s);
    if (err)
        goto free_cpu;

    if (hipStreamCreate(&s->stream) != hipSuccess) {
        err = -EIO;
        goto free_module;
    }

    /* Query actual wavefront size — 64 on GCN/RDNA1, 32 on RDNA2+.
     * Used as blockDim.x for speed_solve_hip_kernel. */
    {
        int dev = 0;
        hipDeviceProp_t prop;
        (void)hipGetDevice(&dev);
        s->solve_warp = (hipGetDeviceProperties(&prop, dev) == hipSuccess) ?
                            (unsigned)prop.warpSize :
                            ST_SOLVE_WARP_DEFAULT;
    }

    err = st_hip_bufs_alloc(s);
    if (err)
        goto free_hip;
#else
    return -ENOSYS;
#endif /* HAVE_HIPCC */

    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (!s->feature_name_dict) {
        err = -ENOMEM;
#ifdef HAVE_HIPCC
        goto free_hip;
#endif
    }

    s->frame_index = 0;
    return 0;

#ifdef HAVE_HIPCC
free_hip:
    free_hip_buffers_st(s);
    goto free_cpu;
free_module:
    if (s->module) {
        (void)hipModuleUnload(s->module);
        s->module = NULL;
    }
#endif /* HAVE_HIPCC */
free_cpu:
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
    return err;
}

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
    SpeedTemporalHipState *s = fex->priv;

    const int cyclic = (int)(index % 2u);
    const int other = (int)((index + 1u) % 2u);

    picture_copy(s->h_ref[cyclic], s->float_stride, ref_pic, -128, ref_pic->bpc, 0);
    picture_copy(s->h_dis[cyclic], s->float_stride, dist_pic, -128, dist_pic->bpc, 0);

    if (index == 0) {
        return vmaf_feature_collector_append_with_dict(
            feature_collector, s->feature_name_dict, "Speed_temporal_feature_speed_temporal_score",
            0.0, index);
    }

    const int orig_w = (int)s->dim.original_width;
    const int orig_h = (int)s->dim.original_height;
    subtract_plane(s->h_ref[other], s->h_ref[cyclic], orig_w, orig_h, s->float_stride);
    if (s->speed_temporal_use_ref_diff)
        subtract_plane(s->h_dis[other], s->h_ref[cyclic], orig_w, orig_h, s->float_stride);
    else
        subtract_plane(s->h_dis[other], s->h_dis[cyclic], orig_w, orig_h, s->float_stride);

    const size_t stride_px = s->float_stride / sizeof(float);
    const size_t tmp_size = 2u * s->dim.alloc_height * stride_px;
    float *tmp_filter = (float *)aligned_malloc(tmp_size * sizeof(float), 32);
    if (!tmp_filter)
        return -ENOMEM;

    speed_internal_filter_and_downscale(&s->dim, &s->opt, s->h_ref[other], tmp_filter,
                                        s->float_stride);
    speed_internal_filter_and_downscale(&s->dim, &s->opt, s->h_dis[other], tmp_filter,
                                        s->float_stride);
    aligned_free(tmp_filter);

    int err = run_gpu_pipeline_st(s, s->h_ref[other], s->d_indterm_ref, s->h_indterm_ref);
    if (err)
        return err;

    float saved_cov[ST_ELEMENTS * ST_ELEMENTS];
    (void)memcpy(saved_cov, s->h_cov_mat, sizeof(saved_cov));

    err = run_cpu_linalg_st(s, s->h_indterm_ref, s->d_sol_ref);
    if (err)
        return err;

    err = run_gpu_pipeline_st(s, s->h_dis[other], s->d_indterm_dis, s->h_indterm_dis);
    if (err)
        return err;

    (void)memcpy(s->h_cov_mat, saved_cov, sizeof(saved_cov));

    err = run_cpu_linalg_st(s, s->h_indterm_dis, s->d_sol_dis);
    if (err)
        return err;

    float score = 0.0f;
    err = run_score_st(s, &score);
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
#ifdef HAVE_HIPCC
    free_hip_buffers_st(s);
#endif
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
    if (s->feature_name_dict)
        vmaf_dictionary_free(&s->feature_name_dict);
    return 0;
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
