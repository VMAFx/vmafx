/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  speed_temporal feature extractor — SYCL backend with real on-device
 *  GPU kernels (ADR-0567).
 *
 *  Temporal design: two ping-pong host float-plane buffers hold the
 *  converted luma planes.  Each frame the GPU runs the full SpEED
 *  pipeline on the temporal difference (prev − cur) rather than the
 *  raw plane — matching the CPU twin in speed.c.  Frame 0 emits
 *  score 0 (no previous frame available).
 *
 *  Algorithm split: identical to speed_chroma_sycl.cpp.  See that file
 *  and ADR-0567 for the full GPU/CPU split rationale.
 *
 *  Output feature: Speed_temporal_feature_speed_temporal_score.
 */

#include <sycl/sycl.hpp>

#include "sycl_compat.h"

#include <cerrno>
#include <cmath>
#include <cstring>

#include "config.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature_name.h"
#include "log.h"
#include "mem.h"
#include "picture.h"
#include "picture_copy.h"
#include "sycl/common.h"
#include "feature/speed_internal.h"

namespace
{

constexpr uint32_t SP_ELEMENTS = 25u;
constexpr uint32_t SP_BLOCK_SIZE = 5u;
constexpr uint32_t COV_WG = 256u;
constexpr uint32_t MEANS_WG = 256u;
constexpr uint32_t INDTERM_WG = 256u;
constexpr uint32_t SCORE_WG = 256u;
constexpr uint32_t SOLVE_WG = 32u;

/* ------------------------------------------------------------------ */
/* SYCL GPU kernels (identical to speed_chroma_sycl.cpp)             */
/* ------------------------------------------------------------------ */

static void launch_means(sycl::queue &q, const float *plane, float *means, uint32_t op_w,
                         uint32_t stride_px, uint32_t num_blocks_h, uint32_t num_blocks,
                         uint32_t submatrix_w, uint32_t submatrix_h)
{
    const size_t global = ((num_blocks + MEANS_WG - 1u) / MEANS_WG) * MEANS_WG;
    q.submit([&](sycl::handler &cgh) {
        cgh.parallel_for(sycl::nd_range<1>(global, MEANS_WG), [=](sycl::nd_item<1> it) {
            const uint32_t tile_idx = (uint32_t)it.get_global_id(0);
            if (tile_idx >= num_blocks)
                return;
            const uint32_t tile_x = tile_idx % num_blocks_h;
            const uint32_t tile_y = tile_idx / num_blocks_h;
            for (uint32_t elem = 0; elem < SP_ELEMENTS; ++elem) {
                const uint32_t er = elem / SP_BLOCK_SIZE;
                const uint32_t ec = elem % SP_BLOCK_SIZE;
                const uint32_t sr = tile_y * SP_BLOCK_SIZE + er;
                const uint32_t sc_col = tile_x * SP_BLOCK_SIZE + ec;
                double acc = 0.0;
                for (uint32_t i = 0; i < submatrix_h; ++i)
                    for (uint32_t j = 0; j < submatrix_w; ++j)
                        acc += (double)plane[(sr + i) * stride_px + (sc_col + j)];
                means[elem * num_blocks + tile_idx] =
                    (float)(acc / (double)(submatrix_w * submatrix_h));
            }
            (void)op_w;
        });
    });
}

static void launch_cov(sycl::queue &q, const float *plane, const float *means, float *cov_mat,
                       uint32_t stride_px, uint32_t num_blocks_h, uint32_t num_blocks,
                       uint32_t submatrix_w, uint32_t submatrix_h)
{
    const size_t total_wg = SP_ELEMENTS * SP_ELEMENTS;
    q.submit([&](sycl::handler &cgh) {
        sycl::local_accessor<double, 1> s_partial(sycl::range<1>(COV_WG), cgh);
        cgh.parallel_for(sycl::nd_range<1>(total_wg * COV_WG, COV_WG), [=](sycl::nd_item<1> it) {
            const uint32_t x_index = (uint32_t)(it.get_group(0) / SP_ELEMENTS);
            const uint32_t y_index = (uint32_t)(it.get_group(0) % SP_ELEMENTS);
            const uint32_t tid = (uint32_t)it.get_local_id(0);
            s_partial[tid] = 0.0;
            it.barrier(sycl::access::fence_space::local_space);

            const uint32_t xr = x_index / SP_BLOCK_SIZE;
            const uint32_t xc = x_index % SP_BLOCK_SIZE;
            const uint32_t yr = y_index / SP_BLOCK_SIZE;
            const uint32_t yc = y_index % SP_BLOCK_SIZE;

            double local_sum = 0.0;
            for (uint32_t tile = tid; tile < num_blocks; tile += COV_WG) {
                const uint32_t tx = tile % num_blocks_h;
                const uint32_t ty = tile / num_blocks_h;
                const double mx = (double)means[x_index * num_blocks + tile];
                const double my = (double)means[y_index * num_blocks + tile];
                const uint32_t srx = ty * SP_BLOCK_SIZE + xr;
                const uint32_t scx = tx * SP_BLOCK_SIZE + xc;
                const uint32_t sry = ty * SP_BLOCK_SIZE + yr;
                const uint32_t scy = tx * SP_BLOCK_SIZE + yc;
                double cov = 0.0;
                for (uint32_t i = 0; i < submatrix_h; ++i)
                    for (uint32_t j = 0; j < submatrix_w; ++j) {
                        double vx = (double)plane[(srx + i) * stride_px + (scx + j)];
                        double vy = (double)plane[(sry + i) * stride_px + (scy + j)];
                        cov += (vx - mx) * (vy - my);
                    }
                local_sum += cov / (double)(submatrix_w * submatrix_h);
            }
            s_partial[tid] = local_sum;
            it.barrier(sycl::access::fence_space::local_space);

            for (uint32_t s = COV_WG / 2u; s > 0u; s >>= 1u) {
                if (tid < s)
                    s_partial[tid] += s_partial[tid + s];
                it.barrier(sycl::access::fence_space::local_space);
            }
            if (tid == 0u)
                cov_mat[x_index * SP_ELEMENTS + y_index] = (float)s_partial[0];
        });
    });
}

static void launch_indterm(sycl::queue &q, const float *plane, float *indterm, uint32_t stride_px,
                           uint32_t num_blocks_h, uint32_t num_blocks)
{
    const uint32_t total = SP_ELEMENTS * num_blocks;
    const size_t global = ((total + INDTERM_WG - 1u) / INDTERM_WG) * INDTERM_WG;
    q.submit([&](sycl::handler &cgh) {
        cgh.parallel_for(sycl::nd_range<1>(global, INDTERM_WG), [=](sycl::nd_item<1> it) {
            const uint32_t idx = (uint32_t)it.get_global_id(0);
            if (idx >= total)
                return;
            const uint32_t elem = idx / num_blocks;
            const uint32_t tile_idx = idx % num_blocks;
            const uint32_t tile_x = tile_idx % num_blocks_h;
            const uint32_t tile_y = tile_idx / num_blocks_h;
            const uint32_t er = elem / SP_BLOCK_SIZE;
            const uint32_t ec = elem % SP_BLOCK_SIZE;
            const uint32_t pr = tile_y * SP_BLOCK_SIZE + er;
            const uint32_t pc = tile_x * SP_BLOCK_SIZE + ec;
            indterm[elem * num_blocks + tile_idx] = plane[pr * stride_px + pc];
        });
    });
}

static void launch_solve(sycl::queue &q, const float *R, float *rhs, uint32_t num_blocks)
{
    const size_t warps = ((num_blocks + 7u) / 8u) * 8u;
    const size_t global = warps * SOLVE_WG;
    const size_t local = SOLVE_WG * 8u;
    q.submit([&](sycl::handler &cgh) {
        cgh.parallel_for(sycl::nd_range<1>(global, local), [=](sycl::nd_item<1> it) {
            const uint32_t warp_id = (uint32_t)(it.get_global_id(0) / SOLVE_WG);
            const uint32_t lane = (uint32_t)(it.get_global_id(0) % SOLVE_WG);
            if (warp_id >= num_blocks || lane >= SP_ELEMENTS)
                return;
            const uint32_t col = warp_id;
            for (int32_t i = (int32_t)(SP_ELEMENTS - 1u); i >= 0; --i) {
                if ((int32_t)lane == i) {
                    float val = rhs[(uint32_t)i * num_blocks + col];
                    const float denom = R[(uint32_t)i * SP_ELEMENTS + (uint32_t)i];
                    for (uint32_t k = (uint32_t)(i + 1); k < SP_ELEMENTS; ++k)
                        val -= rhs[k * num_blocks + col] * R[(uint32_t)i * SP_ELEMENTS + k];
                    rhs[(uint32_t)i * num_blocks + col] =
                        (sycl::fabs(denom) > 1e-8f) ? val / denom : 0.0f;
                }
                sycl::group_barrier(it.get_group());
            }
        });
    });
}

static void launch_score(sycl::queue &q, const float *eigenvalues, const float *ref_sol,
                         const float *dis_sol, const float *ref_indterm, const float *dis_indterm,
                         float *ref_ent, float *ref_var, float *dis_ent, float *dis_var,
                         uint32_t num_blocks, float sigma_nn)
{
    const size_t global = ((num_blocks + SCORE_WG - 1u) / SCORE_WG) * SCORE_WG;
    const float log2e_2pi = sycl::log2(2.0f * 3.14159265358979323846f * 2.71828182845904523536f);
    q.submit([&](sycl::handler &cgh) {
        cgh.parallel_for(sycl::nd_range<1>(global, SCORE_WG), [=](sycl::nd_item<1> it) {
            const uint32_t tile = (uint32_t)it.get_global_id(0);
            if (tile >= num_blocks)
                return;
            float rv = 0.0f;
            float dv = 0.0f;
            for (uint32_t elem = 0; elem < SP_ELEMENTS; ++elem) {
                const uint32_t idx = elem * num_blocks + tile;
                rv += ref_sol[idx] * ref_indterm[idx];
                dv += dis_sol[idx] * dis_indterm[idx];
            }
            rv /= (float)SP_ELEMENTS;
            dv /= (float)SP_ELEMENTS;
            ref_var[tile] = rv;
            dis_var[tile] = dv;
            float re = 0.0f;
            float de = 0.0f;
            for (uint32_t k = 0; k < SP_ELEMENTS; ++k) {
                float lk = eigenvalues[k] < 0.0f ? 0.0f : eigenvalues[k];
                re += sycl::log2(lk * rv + sigma_nn) + log2e_2pi;
                de += sycl::log2(lk * dv + sigma_nn) + log2e_2pi;
            }
            ref_ent[tile] = re;
            dis_ent[tile] = de;
        });
    });
}

/* ------------------------------------------------------------------ */
/* Per-extractor state                                                 */
/* ------------------------------------------------------------------ */

struct SpeedTemporalSyclState {
    VmafSyclState *sycl_state;

    SpeedInternalDimensions dim;
    SpeedInternalOptions opt;
    size_t float_stride;

    /* Ping-pong host luma plane buffers. */
    float *h_ref[2];
    float *h_dis[2];

    /* Device USM buffers. */
    float *d_plane;
    float *d_means;
    float *d_cov_mat;
    float *d_indterm_ref;
    float *d_indterm_dis;
    float *d_sol_ref;
    float *d_sol_dis;
    float *d_R;
    float *d_eigenvalues;
    float *d_ref_ent;
    float *d_ref_var;
    float *d_dis_ent;
    float *d_dis_var;

    /* Shared host buffers (SYCL host_alloc for D2H). */
    float *h_cov_mat;
    float *h_ref_ent;
    float *h_ref_var;
    float *h_dis_ent;
    float *h_dis_var;

    /* CPU-only scratch. */
    float *h_eigenvalues;
    float *h_eig_scratch;
    float *h_Q;
    float *h_R;
    float *h_qr_scratch;
    float *h_indterm_ref;
    float *h_indterm_dis;
    float *h_qt_scratch;

    /* Frame bookkeeping. */
    unsigned frame_index;

    double speed_temporal_kernelscale;
    double speed_temporal_prescale;
    char *speed_temporal_prescale_method;
    double speed_temporal_sigma_nn;
    double speed_temporal_nn_floor;
    double speed_temporal_max_val;
    bool speed_temporal_use_ref_diff;

    VmafDictionary *feature_name_dict;
};

static void free_sycl_state_st(SpeedTemporalSyclState *s)
{
    sycl::queue *q = (sycl::queue *)vmaf_sycl_get_queue_ptr(s->sycl_state);
#define FREE_D(p)                                                                                  \
    do {                                                                                           \
        if ((p)) {                                                                                 \
            sycl::free((p), *q);                                                                   \
            (p) = nullptr;                                                                         \
        }                                                                                          \
    } while (0)
#define FREE_A(p)                                                                                  \
    do {                                                                                           \
        if ((p)) {                                                                                 \
            aligned_free((p));                                                                     \
            (p) = nullptr;                                                                         \
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
    FREE_D(s->d_ref_ent);
    FREE_D(s->d_ref_var);
    FREE_D(s->d_dis_ent);
    FREE_D(s->d_dis_var);
    FREE_D(s->h_cov_mat);
    FREE_D(s->h_ref_ent);
    FREE_D(s->h_ref_var);
    FREE_D(s->h_dis_ent);
    FREE_D(s->h_dis_var);
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
#undef FREE_A
}

static void subtract_plane(float *a, const float *b, int w, int h, size_t stride_bytes)
{
    const size_t stride_px = stride_bytes / sizeof(float);
    for (int i = 0; i < h; ++i)
        for (int j = 0; j < w; ++j)
            a[(size_t)i * stride_px + (size_t)j] -= b[(size_t)i * stride_px + (size_t)j];
}

/* ------------------------------------------------------------------ */
/* GPU + CPU pipeline for one temporal-diff plane                     */
/* ------------------------------------------------------------------ */

static int run_channel_st(SpeedTemporalSyclState *s, float *h_plane, float *h_indterm,
                          float *d_indterm, float *d_sol)
{
    sycl::queue &q = *(sycl::queue *)vmaf_sycl_get_queue_ptr(s->sycl_state);
    const uint32_t num_blocks = (uint32_t)s->dim.num_blocks;
    const uint32_t num_blocks_h = (uint32_t)s->dim.num_blocks_horizontal;
    const uint32_t op_w = (uint32_t)s->dim.truncated_width;
    const uint32_t stride_px = (uint32_t)(s->float_stride / sizeof(float));
    const uint32_t submatrix_w = (uint32_t)s->dim.submatrix_width;
    const uint32_t submatrix_h = (uint32_t)s->dim.submatrix_height;
    const size_t plane_bytes = s->dim.truncated_height * stride_px * sizeof(float);
    const size_t indterm_bytes = (size_t)SP_ELEMENTS * num_blocks * sizeof(float);

    q.memcpy(s->d_plane, h_plane, plane_bytes);
    q.wait();

    launch_means(q, s->d_plane, s->d_means, op_w, stride_px, num_blocks_h, num_blocks, submatrix_w,
                 submatrix_h);
    launch_cov(q, s->d_plane, s->d_means, s->d_cov_mat, stride_px, num_blocks_h, num_blocks,
               submatrix_w, submatrix_h);
    launch_indterm(q, s->d_plane, d_indterm, stride_px, num_blocks_h, num_blocks);
    q.wait();

    q.memcpy(s->h_cov_mat, s->d_cov_mat, SP_ELEMENTS * SP_ELEMENTS * sizeof(float));
    q.memcpy(h_indterm, d_indterm, indterm_bytes);
    q.wait();

    const int sz = (int)SP_ELEMENTS;
    const int nb = (int)num_blocks;
    speed_internal_compute_eigenvalues(s->h_cov_mat, s->h_eigenvalues, sz, s->h_eig_scratch);
    bool regular = speed_internal_is_matrix_regular(s->h_eigenvalues, SP_ELEMENTS);

    if (!regular) {
        vmaf_log(VMAF_LOG_LEVEL_WARNING,
                 "speed_temporal_sycl: covariance matrix singular, zeroing solution\n");
        memset(h_indterm, 0, indterm_bytes);
    } else {
        speed_internal_qr_factorize(s->h_cov_mat, sz, s->h_Q, s->h_R, s->h_qr_scratch);
        speed_internal_qt_multiply(s->h_Q, h_indterm, sz, nb, s->h_qt_scratch);
        q.memcpy(s->d_R, s->h_R, (size_t)sz * (size_t)sz * sizeof(float));
        q.memcpy(d_sol, h_indterm, indterm_bytes);
        q.wait();
        launch_solve(q, s->d_R, d_sol, num_blocks);
        q.wait();
    }

    q.memcpy(s->d_eigenvalues, s->h_eigenvalues, (size_t)sz * sizeof(float));
    q.wait();
    return 0;
}

static int score_aggregate_st(SpeedTemporalSyclState *s, float *score_out)
{
    sycl::queue &q = *(sycl::queue *)vmaf_sycl_get_queue_ptr(s->sycl_state);
    const uint32_t num_blocks = (uint32_t)s->dim.num_blocks;
    const float sigma_nn = (float)s->opt.speed_sigma_nn;

    launch_score(q, s->d_eigenvalues, s->d_sol_ref, s->d_sol_dis, s->d_indterm_ref,
                 s->d_indterm_dis, s->d_ref_ent, s->d_ref_var, s->d_dis_ent, s->d_dis_var,
                 num_blocks, sigma_nn);

    const size_t ab = (size_t)num_blocks * sizeof(float);
    q.memcpy(s->h_ref_ent, s->d_ref_ent, ab);
    q.memcpy(s->h_ref_var, s->d_ref_var, ab);
    q.memcpy(s->h_dis_ent, s->d_dis_ent, ab);
    q.memcpy(s->h_dis_var, s->d_dis_var, ab);
    q.wait();

    const float base_entropy =
        (float)SP_ELEMENTS *
        (std::log2f((1.0f + (float)s->opt.speed_nn_floor) * (float)s->opt.speed_sigma_nn) +
         std::log2f(2.0f * 3.14159265358979323846f * 2.71828182845904523536f));

    float total = 0.0f;
    for (uint32_t i = 0; i < num_blocks; ++i) {
        const float re = s->h_ref_ent[i];
        const float de = s->h_dis_ent[i];
        if (re < base_entropy && de < base_entropy)
            continue;
        const float rv = s->h_ref_var[i];
        const float dv = s->h_dis_var[i];
        /* speed_temporal uses weight_var_mode = 0. */
        float spatial_ref = re * std::log2f(1.0f + rv);
        float spatial_dis = de * std::log2f(1.0f + dv);
        total += std::fabs(spatial_ref - spatial_dis);
    }
    *score_out = total / (float)num_blocks;
    return 0;
}

} /* anonymous namespace */

extern "C" {

static const VmafOption options_temporal[] = {
    {
        .name = "speed_kernelscale",
        .help = "scaling factor for the Gaussian kernel",
        .offset = offsetof(SpeedTemporalSyclState, speed_temporal_kernelscale),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val = {.d = 1.0},
        .min = 0.1,
        .max = 4.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "ks",
    },
    {
        .name = "speed_prescale",
        .help = "scaling factor for the frame",
        .offset = offsetof(SpeedTemporalSyclState, speed_temporal_prescale),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val = {.d = 1.0},
        .min = 0.1,
        .max = 4.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "ps",
    },
    {
        .name = "speed_prescale_method",
        .help = "scaling method [nearest, bilinear, bicubic, lanczos4]",
        .offset = offsetof(SpeedTemporalSyclState, speed_temporal_prescale_method),
        .type = VMAF_OPT_TYPE_STRING,
        .default_val = {.s = "nearest"},
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "psm",
    },
    {
        .name = "speed_sigma_nn",
        .help = "standard deviation of neural noise",
        .offset = offsetof(SpeedTemporalSyclState, speed_temporal_sigma_nn),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val = {.d = 0.29},
        .min = 0.1,
        .max = 2.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "snn",
    },
    {
        .name = "speed_nn_floor",
        .help = "neural noise floor fraction",
        .offset = offsetof(SpeedTemporalSyclState, speed_temporal_nn_floor),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val = {.d = 0.0},
        .min = 0.0,
        .max = 1.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "nnf",
    },
    {
        .name = "speed_max_val",
        .help = "clip output to this maximum",
        .offset = offsetof(SpeedTemporalSyclState, speed_temporal_max_val),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val = {.d = 1000.0},
        .min = 0.0,
        .max = 1000.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "mxv",
    },
    {
        .name = "speed_use_ref_diff",
        .help = "use reference frame difference instead of distorted",
        .offset = offsetof(SpeedTemporalSyclState, speed_temporal_use_ref_diff),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val = {.b = false},
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "urd",
    },
    {0},
};

/* forward decl for init failure cleanup — SY-2a */
static int close_temporal_sycl(VmafFeatureExtractor *fex);

static int init_temporal_sycl(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                              unsigned w, unsigned h)
{
    (void)pix_fmt;
    (void)bpc;
    SpeedTemporalSyclState *s = (SpeedTemporalSyclState *)fex->priv;

    s->sycl_state = fex->sycl_state;
    s->opt = SpeedInternalOptions{
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

    sycl::queue &q = *(sycl::queue *)vmaf_sycl_get_queue_ptr(s->sycl_state);
    const size_t stride_px = s->float_stride / sizeof(float);
    const size_t nb = s->dim.num_blocks;
    const size_t plane_bytes = s->dim.alloc_height * stride_px * sizeof(float);
    const size_t indterm_bytes = SP_ELEMENTS * nb * sizeof(float);
    const size_t cov_bytes = SP_ELEMENTS * SP_ELEMENTS * sizeof(float);
    const size_t score_bytes = nb * sizeof(float);

#define ALLOC_D(field, sz) s->field = sycl::malloc_device<float>((sz) / sizeof(float), q)
#define ALLOC_H(field, sz) s->field = sycl::malloc_host<float>((sz) / sizeof(float), q)
#define ALLOC_A(field, sz) s->field = (float *)aligned_malloc((sz), 32)

    ALLOC_D(d_plane, plane_bytes);
    ALLOC_D(d_means, indterm_bytes);
    ALLOC_D(d_cov_mat, cov_bytes);
    ALLOC_D(d_indterm_ref, indterm_bytes);
    ALLOC_D(d_indterm_dis, indterm_bytes);
    ALLOC_D(d_sol_ref, indterm_bytes);
    ALLOC_D(d_sol_dis, indterm_bytes);
    ALLOC_D(d_R, cov_bytes);
    ALLOC_D(d_eigenvalues, SP_ELEMENTS * sizeof(float));
    ALLOC_D(d_ref_ent, score_bytes);
    ALLOC_D(d_ref_var, score_bytes);
    ALLOC_D(d_dis_ent, score_bytes);
    ALLOC_D(d_dis_var, score_bytes);
    ALLOC_H(h_cov_mat, cov_bytes);
    ALLOC_H(h_ref_ent, score_bytes);
    ALLOC_H(h_ref_var, score_bytes);
    ALLOC_H(h_dis_ent, score_bytes);
    ALLOC_H(h_dis_var, score_bytes);
    ALLOC_A(h_ref[0], plane_bytes);
    ALLOC_A(h_ref[1], plane_bytes);
    ALLOC_A(h_dis[0], plane_bytes);
    ALLOC_A(h_dis[1], plane_bytes);
    ALLOC_A(h_eigenvalues, SP_ELEMENTS * sizeof(float));
    ALLOC_A(h_eig_scratch, (SP_ELEMENTS * SP_ELEMENTS + 3u * SP_ELEMENTS) * sizeof(float));
    ALLOC_A(h_Q, cov_bytes);
    ALLOC_A(h_R, cov_bytes);
    ALLOC_A(h_qr_scratch, 4u * cov_bytes);
    ALLOC_A(h_indterm_ref, indterm_bytes);
    ALLOC_A(h_indterm_dis, indterm_bytes);
    ALLOC_A(h_qt_scratch, indterm_bytes);

#undef ALLOC_D
#undef ALLOC_H
#undef ALLOC_A

    if (!s->d_plane || !s->h_ref[0] || !s->h_ref[1] || !s->h_dis[0] || !s->h_dis[1] ||
        !s->h_eigenvalues || !s->h_Q || !s->h_R) {
        close_temporal_sycl(fex);
        return -ENOMEM;
    }

    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (!s->feature_name_dict) {
        close_temporal_sycl(fex);
        return -ENOMEM;
    }

    s->frame_index = 0;
    return 0;
}

static int extract_temporal_sycl(VmafFeatureExtractor *fex, VmafPicture *ref_pic,
                                 VmafPicture *ref_pic_90, VmafPicture *dist_pic,
                                 VmafPicture *dist_pic_90, unsigned index,
                                 VmafFeatureCollector *feature_collector)
{
    (void)ref_pic_90;
    (void)dist_pic_90;
    SpeedTemporalSyclState *s = (SpeedTemporalSyclState *)fex->priv;

    const int cyclic = (int)(index % 2u);
    const int other = (int)((index + 1u) % 2u);

    /* Copy current frame luma planes into ping-pong slots. */
    picture_copy(s->h_ref[cyclic], s->float_stride, ref_pic, -128, ref_pic->bpc, 0);
    picture_copy(s->h_dis[cyclic], s->float_stride, dist_pic, -128, dist_pic->bpc, 0);

    /* Frame 0: no previous frame to diff; emit score 0. */
    if (index == 0) {
        return vmaf_feature_collector_append_with_dict(
            feature_collector, s->feature_name_dict, "Speed_temporal_feature_speed_temporal_score",
            0.0, index);
    }

    /* Temporal difference: other_index holds the previous frame. */
    const int orig_w = (int)s->dim.original_width;
    const int orig_h = (int)s->dim.original_height;
    subtract_plane(s->h_ref[other], s->h_ref[cyclic], orig_w, orig_h, s->float_stride);
    if (s->speed_temporal_use_ref_diff)
        subtract_plane(s->h_dis[other], s->h_ref[cyclic], orig_w, orig_h, s->float_stride);
    else
        subtract_plane(s->h_dis[other], s->h_dis[cyclic], orig_w, orig_h, s->float_stride);

    /* Filter+downscale the temporal-diff planes. */
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

    /* GPU pipeline: reference diff. */
    int err = run_channel_st(s, s->h_ref[other], s->h_indterm_ref, s->d_indterm_ref, s->d_sol_ref);
    if (err)
        return err;

    /* Save reference covariance basis (SpEED uses ref basis for both). */
    float saved_cov[SP_ELEMENTS * SP_ELEMENTS];
    memcpy(saved_cov, s->h_cov_mat, sizeof(saved_cov));

    /* GPU pipeline: distorted diff — reuses d_cov_mat allocation. */
    err = run_channel_st(s, s->h_dis[other], s->h_indterm_dis, s->d_indterm_dis, s->d_sol_dis);
    if (err)
        return err;

    /* Restore reference cov so score kernel uses consistent eigenvalues. */
    memcpy(s->h_cov_mat, saved_cov, sizeof(saved_cov));

    float score = 0.0f;
    err = score_aggregate_st(s, &score);
    if (err)
        return err;

    const double mxv = s->speed_temporal_max_val;
    const double clipped = (double)score < mxv ? (double)score : mxv;
    return vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "Speed_temporal_feature_speed_temporal_score",
                                                   clipped, index);
}

static int close_temporal_sycl(VmafFeatureExtractor *fex)
{
    SpeedTemporalSyclState *s = (SpeedTemporalSyclState *)fex->priv;
    if (s->sycl_state)
        free_sycl_state_st(s);
    if (s->feature_name_dict)
        vmaf_dictionary_free(&s->feature_name_dict);
    return 0;
}

static const char *provided_features_temporal[] = {
    "Speed_temporal_feature_speed_temporal_score",
    nullptr,
};

/* ADR-0567: real SYCL GPU kernels for speed_temporal.
 * TEMPORAL flag guarantees sequential frame submission (ping-pong diff
 * requires frame ordering). */
VmafFeatureExtractor vmaf_fex_speed_temporal_sycl = {
    .name = "speed_temporal_sycl",
    .init = init_temporal_sycl,
    .extract = extract_temporal_sycl,
    .close = close_temporal_sycl,
    .options = options_temporal,
    .priv_size = sizeof(SpeedTemporalSyclState),
    .provided_features = provided_features_temporal,
    .flags = VMAF_FEATURE_EXTRACTOR_TEMPORAL | VMAF_FEATURE_EXTRACTOR_SYCL,
};

} /* extern "C" */
