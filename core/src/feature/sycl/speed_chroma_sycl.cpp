/**
 *  Copyright 2016-2025 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  speed_chroma feature extractor — SYCL backend with real on-device
 *  GPU kernels (ADR-0567).
 *
 *  Algorithm split: identical to speed_chroma_cuda.c.
 *  GPU kernels (SYCL nd_range): means, covariance, indterm,
 *  backward-substitution, score.
 *  CPU: filter_and_downscale, 25×25 eigendecomp, QR factorize, Qt×B.
 *
 *  Numerical contract: places=4 vs CPU reference (ADR-0214 / ADR-0567).
 */

#include <sycl/sycl.hpp>

#include "sycl_compat.h"

#include <cerrno>
#include <cmath>
#include <numbers>
#include <cstring>
#include <utility>

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

/*
 * lint rationale: ADR-1266 keeps
 * file-local SYCL helpers static because Praetor misclassifies namespace scopes as functions.
 */

constexpr uint32_t SP_ELEMENTS = 25u;
constexpr uint32_t SP_BLOCK_SIZE = 5u;
constexpr uint32_t COV_WG = 256u;
constexpr uint32_t MEANS_WG = 256u;
constexpr uint32_t INDTERM_WG = 256u;
constexpr uint32_t SCORE_WG = 256u;
constexpr uint32_t SOLVE_WG = 32u; /* one warp per column */

/* ------------------------------------------------------------------ */
/* SYCL GPU kernels                                                    */
/* ------------------------------------------------------------------ */

struct SpeedCompensatedSum {
    float hi;
    float lo;
};

static SpeedCompensatedSum speed_add_product(SpeedCompensatedSum sum, float dx, float dy)
{
    const float prod = dx * dy;
    const float perr = sycl::fma(dx, dy, -prod);
    const float sum_hi = sum.hi + prod;
    const float bias = sum_hi - sum.hi;
    const float err = (sum.hi - (sum_hi - bias)) + (prod - bias);
    const float t = sum.lo + perr + err;
    const float renorm = sum_hi + t;
    return {renorm, t - (renorm - sum_hi)};
}

static SpeedCompensatedSum speed_combine_sums(SpeedCompensatedSum a, SpeedCompensatedSum b)
{
    const float sum_hi = a.hi + b.hi;
    const float bias = sum_hi - a.hi;
    const float err = (a.hi - (sum_hi - bias)) + (b.hi - bias);
    const float t = a.lo + b.lo + err;
    const float renorm = sum_hi + t;
    return {renorm, t - (renorm - sum_hi)};
}

static SpeedCompensatedSum speed_cov_partial(const float *plane, uint32_t stride_px,
                                             uint32_t submatrix_w, uint32_t total, uint32_t xr,
                                             uint32_t xc, uint32_t yr, uint32_t yc, float mean_x,
                                             float mean_y, uint32_t tid)
{
    SpeedCompensatedSum sum{0.0f, 0.0f};
    for (uint32_t p = tid; p < total; p += COV_WG) {
        const uint32_t i = p / submatrix_w;
        const uint32_t j = p % submatrix_w;
        const float vx = plane[(xr + i) * stride_px + (xc + j)];
        const float vy = plane[(yr + i) * stride_px + (yc + j)];
        sum = speed_add_product(sum, vx - mean_x, vy - mean_y);
    }
    const float renorm = sum.hi + sum.lo;
    return {renorm, sum.lo - (renorm - sum.hi)};
}

/* One global submatrix sweep matches the CPU covariance. The compensated fp32
 * pair reproduces the precision of the CPU's double accumulator on fp64-less
 * Arc devices; keep the operation order in the helpers above unchanged. */
static void launch_cov(sycl::queue &q, const float *plane, const float *means, float *cov_mat,
                       uint32_t stride_px, uint32_t num_blocks_h, uint32_t num_blocks,
                       uint32_t submatrix_w, uint32_t submatrix_h)
{
    (void)num_blocks_h;
    (void)num_blocks;
    /* 625 work-groups of COV_WG threads, one per (x_index, y_index) pair. */
    const size_t total_wg = (size_t)SP_ELEMENTS * SP_ELEMENTS;
    q.submit([&](sycl::handler &cgh) {
        sycl::local_accessor<float, 1> const s_partial(sycl::range<1>(COV_WG), cgh);
        sycl::local_accessor<float, 1> const s_partial_lo(sycl::range<1>(COV_WG), cgh);
        cgh.parallel_for(sycl::nd_range<1>(total_wg * COV_WG, COV_WG), [=](sycl::nd_item<1> it) {
            const uint32_t x_index = (uint32_t)(it.get_group(0) / SP_ELEMENTS);
            const uint32_t y_index = (uint32_t)(it.get_group(0) % SP_ELEMENTS);
            const uint32_t tid = (uint32_t)it.get_local_id(0);

            const uint32_t xr = x_index / SP_BLOCK_SIZE;
            const uint32_t xc = x_index % SP_BLOCK_SIZE;
            const uint32_t yr = y_index / SP_BLOCK_SIZE;
            const uint32_t yc = y_index % SP_BLOCK_SIZE;
            const uint32_t total = submatrix_h * submatrix_w;
            const SpeedCompensatedSum partial =
                speed_cov_partial(plane, stride_px, submatrix_w, total, xr, xc, yr, yc,
                                  means[x_index], means[y_index], tid);
            s_partial[tid] = partial.hi;
            s_partial_lo[tid] = partial.lo;
            it.barrier(sycl::access::fence_space::local_space);

            for (uint32_t s = COV_WG / 2u; s > 0u; s >>= 1u) {
                if (tid < s) {
                    const SpeedCompensatedSum sum =
                        speed_combine_sums({s_partial[tid], s_partial_lo[tid]},
                                           {s_partial[tid + s], s_partial_lo[tid + s]});
                    s_partial[tid] = sum.hi;
                    s_partial_lo[tid] = sum.lo;
                }
                it.barrier(sycl::access::fence_space::local_space);
            }
            if (tid == 0u) {
                const float denom = (float)total;
                cov_mat[x_index * SP_ELEMENTS + y_index] =
                    s_partial[0] / denom + s_partial_lo[0] / denom;
            }
        });
    });
}

/* Kernel 3: independent term */
static void launch_indterm(sycl::queue &q, const float *plane, float *indterm, uint32_t stride_px,
                           uint32_t num_blocks_h, uint32_t num_blocks)
{
    const uint32_t total = SP_ELEMENTS * num_blocks;
    const size_t global = (size_t)((total + INDTERM_WG - 1u) / INDTERM_WG) * INDTERM_WG;
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

/* Kernel 4: backward substitution (one sub-group per column) */
static void launch_solve(sycl::queue &q, const float *R, float *rhs, uint32_t num_blocks)
{
    /* Each warp (32 threads) handles one column; threads 25-31 idle. */
    const size_t warps = (size_t)((num_blocks + 7u) / 8u) * 8u; /* round up to 8 warps per block */
    const size_t global = warps * SOLVE_WG;
    const size_t local = (size_t)SOLVE_WG * 8u;
    q.submit([&](sycl::handler &cgh) {
        cgh.parallel_for(sycl::nd_range<1>(global, local), [=](sycl::nd_item<1> it) {
            const uint32_t warp_id = (uint32_t)(it.get_global_id(0) / SOLVE_WG);
            const uint32_t lane = (uint32_t)(it.get_global_id(0) % SOLVE_WG);
            /* group_barrier below is a work-GROUP collective: EVERY work-item in
             * the group must execute it on every iteration. Returning early for
             * idle lanes (lane >= SP_ELEMENTS, i.e. 25-31) or surplus warps
             * (warp_id >= num_blocks) made those work-items skip the barrier,
             * deadlocking the group on devices with strict barrier semantics
             * (Intel Arc -> UR_RESULT_ERROR_DEVICE_LOST). Gate only the WORK with
             * `active`; keep all work-items in the barrier loop. */
            const bool active = (warp_id < num_blocks && lane < SP_ELEMENTS);
            const uint32_t col = warp_id;
            for (int32_t i = (int32_t)(SP_ELEMENTS - 1u); i >= 0; --i) {
                if (active && std::cmp_equal(lane, i)) {
                    float val = rhs[(uint32_t)i * num_blocks + col];
                    const float denom = R[(uint32_t)i * SP_ELEMENTS + (uint32_t)i];
                    for (uint32_t k = (uint32_t)(i + 1); k < SP_ELEMENTS; ++k)
                        val -= rhs[k * num_blocks + col] * R[(uint32_t)i * SP_ELEMENTS + k];
                    /* Same epsilon as the CPU reference. The host-side pivot
                     * check in run_channel() means this branch is unreachable
                     * in practice, but it must not disagree with it: it used to
                     * say 1e-8f, so a pivot between the two thresholds was
                     * regular here and singular on the CPU. */
                    rhs[(uint32_t)i * num_blocks + col] =
                        (sycl::fabs(denom) > SPEED_INTERNAL_EIGENVALUE_EPS) ? val / denom : 0.0f;
                }
                /* Fence to ensure row-i result is visible before row i-1. */
                sycl::group_barrier(it.get_group());
            }
        });
    });
}

/* Kernel 5: per-tile entropy + score
 *
 * The CPU reference (est_params in speed.c) eigendecomposes SEPARATE ref and
 * dis covariance matrices, so the ref entropy uses ref_eigenvalues and the dis
 * entropy uses dis_eigenvalues — they are NOT shared. The previous single-
 * eigenvalue-array signature reused the dis eigenvalues for both entropies. */
static void launch_score(sycl::queue &q, const float *ref_eigenvalues, const float *dis_eigenvalues,
                         const float *ref_sol, const float *dis_sol, const float *ref_indterm,
                         const float *dis_indterm, float *ref_ent, float *ref_var, float *dis_ent,
                         float *dis_var, uint32_t num_blocks, float sigma_nn)
{
    const size_t global = (size_t)((num_blocks + SCORE_WG - 1u) / SCORE_WG) * SCORE_WG;
    const float log2e_2pi = sycl::log2(2.0f * std::numbers::pi_v<float> * std::numbers::e_v<float>);
    q.submit([&](sycl::handler &cgh) {
        cgh.parallel_for(sycl::nd_range<1>(global, SCORE_WG), [=](sycl::nd_item<1> it) {
            const uint32_t tile = (uint32_t)it.get_global_id(0);
            if (tile >= num_blocks)
                return;
            /* CPU parity on the ROUNDING, not just the value. The reference
             * divides every term before summing —
             * compute_pointwise_product_and_division() writes
             * `(X[i][j] * Y[i][j]) / denominator` back per element and
             * sum_columns() then adds them — so there are 25 divisions each
             * rounded to float. Summing first and dividing once is the same
             * number in exact arithmetic and a different one in fp32; on the
             * parity test's XOR fixture the two disagreed by 1.45e-4, past the
             * places=4 tolerance. denominator is B * B == SP_ELEMENTS. */
            float rv = 0.0f;
            float dv = 0.0f;
            for (uint32_t elem = 0; elem < SP_ELEMENTS; ++elem) {
                const uint32_t idx = elem * num_blocks + tile;
                rv += (ref_sol[idx] * ref_indterm[idx]) / (float)SP_ELEMENTS;
                dv += (dis_sol[idx] * dis_indterm[idx]) / (float)SP_ELEMENTS;
            }
            ref_var[tile] = rv;
            dis_var[tile] = dv;
            float re = 0.0f;
            float de = 0.0f;
            for (uint32_t k = 0; k < SP_ELEMENTS; ++k) {
                float const ref_lk = ref_eigenvalues[k] < 0.0f ? 0.0f : ref_eigenvalues[k];
                float const dis_lk = dis_eigenvalues[k] < 0.0f ? 0.0f : dis_eigenvalues[k];
                re += sycl::log2(ref_lk * rv + sigma_nn) + log2e_2pi;
                de += sycl::log2(dis_lk * dv + sigma_nn) + log2e_2pi;
            }
            ref_ent[tile] = re;
            dis_ent[tile] = de;
        });
    });
}

/* ------------------------------------------------------------------ */
/* Per-extractor state                                                 */
/* ------------------------------------------------------------------ */

struct SpeedChromaSyclState {
    VmafSyclState *sycl_state;

    SpeedInternalDimensions dim;
    SpeedInternalOptions opt;
    size_t float_stride;

    /* Device USM buffers. */
    float *d_plane;
    float *d_means;
    float *d_cov_mat;
    float *d_indterm_ref;
    float *d_indterm_dis;
    float *d_sol_ref;
    float *d_sol_dis;
    float *d_R;
    float *d_eigenvalues;     /* holds dis eigenvalues after the dis linalg */
    float *d_eigenvalues_ref; /* ref eigenvalues, stashed before dis linalg */
    float *d_ref_ent;
    float *d_ref_var;
    float *d_dis_ent;
    float *d_dis_var;

    /* Shared host ↔ device (host_alloc for D2H). */
    float *h_cov_mat;
    float *h_ref_ent;
    float *h_ref_var;
    float *h_dis_ent;
    float *h_dis_var;

    /* Singular covariance matrices are counted, not logged per solve. */
    SpeedInternalSingularTally singular_tally;

    /* CPU-only scratch buffers. */
    float *h_plane_ref;
    float *h_plane_dis;
    float *h_eigenvalues;
    float *h_eig_scratch;
    float *h_Q;
    float *h_R;
    float *h_qr_scratch;
    float *h_indterm_ref;
    float *h_indterm_dis;
    float *h_qt_scratch;

    /* User options. */
    double speed_chroma_kernelscale;
    double speed_chroma_prescale;
    char *speed_chroma_prescale_method;
    double speed_chroma_sigma_nn;
    double speed_chroma_nn_floor;
    double speed_chroma_max_val;
    int speed_weight_var_mode;

    VmafDictionary *feature_name_dict;
};

static void free_sycl_state(SpeedChromaSyclState *s)
{
    sycl::queue const *q = (sycl::queue *)vmaf_sycl_get_queue_ptr(s->sycl_state);
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
    FREE_D(s->d_eigenvalues_ref);
    FREE_D(s->d_ref_ent);
    FREE_D(s->d_ref_var);
    FREE_D(s->d_dis_ent);
    FREE_D(s->d_dis_var);
    FREE_D(s->h_cov_mat);
    FREE_D(s->h_ref_ent);
    FREE_D(s->h_ref_var);
    FREE_D(s->h_dis_ent);
    FREE_D(s->h_dis_var);
    FREE_A(s->h_plane_ref);
    FREE_A(s->h_plane_dis);
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

/* ------------------------------------------------------------------ */
/* GPU + CPU pipeline for one plane                                   */
/* ------------------------------------------------------------------ */

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

static void speed_upload_plane(SpeedChromaSyclState *s, sycl::queue &q, float *h_plane,
                               uint32_t stride_px)
{
    const size_t plane_bytes = s->dim.truncated_height * stride_px * sizeof(float);
    q.memcpy(s->d_plane, h_plane, plane_bytes);
    q.wait();

    float h_means[SP_ELEMENTS];
    speed_internal_compute_means(&s->dim, h_plane, h_means, stride_px);
    q.memcpy(s->d_means, h_means, sizeof(h_means));
    q.wait();
}

static void speed_compute_channel_inputs(SpeedChromaSyclState *s, sycl::queue &q, float *h_indterm,
                                         float *d_indterm, uint32_t stride_px,
                                         uint32_t num_blocks_h, uint32_t num_blocks)
{
    launch_cov(q, s->d_plane, s->d_means, s->d_cov_mat, stride_px, num_blocks_h, num_blocks,
               (uint32_t)s->dim.submatrix_width, (uint32_t)s->dim.submatrix_height);
    launch_indterm(q, s->d_plane, d_indterm, stride_px, num_blocks_h, num_blocks);
    q.wait();

    const size_t indterm_bytes = (size_t)SP_ELEMENTS * num_blocks * sizeof(float);
    q.memcpy(s->h_cov_mat, s->d_cov_mat, (size_t)SP_ELEMENTS * SP_ELEMENTS * sizeof(float));
    q.memcpy(h_indterm, d_indterm, indterm_bytes);
    q.wait();
}

static void speed_zero_solution(sycl::queue &q, float *d_sol, size_t indterm_bytes)
{
    q.memset(d_sol, 0, indterm_bytes);
    q.wait();
}

static bool speed_has_singular_pivot(const float *r)
{
    for (uint32_t i = 0; i < SP_ELEMENTS; i++) {
        if (std::fabs(r[i * SP_ELEMENTS + i]) < SPEED_INTERNAL_EIGENVALUE_EPS)
            return true;
    }
    return false;
}

/* A QR pivot below the CPU threshold follows the same singular path as a
 * non-regular covariance. Returning false tells the caller to retain the
 * original early return, before the eigenvalue upload. */
static bool speed_solve_regular(SpeedChromaSyclState *s, sycl::queue &q, float *h_indterm,
                                float *d_sol, uint32_t num_blocks, size_t indterm_bytes,
                                bool *singular_out)
{
    const int sz = (int)SP_ELEMENTS;
    speed_internal_qr_factorize(s->h_cov_mat, sz, s->h_Q, s->h_R, s->h_qr_scratch);
    if (speed_has_singular_pivot(s->h_R)) {
        *singular_out = true;
        vmaf_log(VMAF_LOG_LEVEL_WARNING,
                 "speed_chroma_sycl: R pivot below regularity epsilon, zeroing solution\n");
        speed_zero_solution(q, d_sol, indterm_bytes);
        return false;
    }
    speed_internal_qt_multiply(s->h_Q, h_indterm, sz, (int)num_blocks, s->h_qt_scratch);
    q.memcpy(s->d_R, s->h_R, (size_t)sz * (size_t)sz * sizeof(float));
    q.memcpy(d_sol, h_indterm, indterm_bytes);
    q.wait();
    launch_solve(q, s->d_R, d_sol, num_blocks);
    q.wait();
    return true;
}

static int run_channel(SpeedChromaSyclState *s, float *h_plane, float *h_indterm, float *d_indterm,
                       float *d_sol, bool *singular_out)
{
    sycl::queue &q = *(sycl::queue *)vmaf_sycl_get_queue_ptr(s->sycl_state);
    const uint32_t num_blocks = (uint32_t)s->dim.num_blocks;
    const uint32_t num_blocks_h = (uint32_t)s->dim.num_blocks_horizontal;
    const uint32_t stride_px = (uint32_t)(s->float_stride / sizeof(float));
    const size_t indterm_bytes = (size_t)SP_ELEMENTS * num_blocks * sizeof(float);

    speed_upload_plane(s, q, h_plane, stride_px);
    speed_compute_channel_inputs(s, q, h_indterm, d_indterm, stride_px, num_blocks_h, num_blocks);

    speed_internal_compute_eigenvalues(s->h_cov_mat, s->h_eigenvalues, (int)SP_ELEMENTS,
                                       s->h_eig_scratch);
    bool const regular = speed_internal_is_matrix_regular(s->h_eigenvalues, SP_ELEMENTS);
    *singular_out = !regular;
    speed_internal_tally_solve(&s->singular_tally, !regular, "speed_chroma_sycl");
    if (!regular) {
        speed_zero_solution(q, d_sol, indterm_bytes);
    } else if (!speed_solve_regular(s, q, h_indterm, d_sol, num_blocks, indterm_bytes,
                                    singular_out)) {
        return 0;
    }

    q.memcpy(s->d_eigenvalues, s->h_eigenvalues, SP_ELEMENTS * sizeof(float));
    q.wait();
    return 0;
}

/* ------------------------------------------------------------------ */
/* Lifecycle (C wrappers)                                             */
/* ------------------------------------------------------------------ */

static float speed_score_difference(float re, float de, float rv, float dv, int wvm)
{
    float sr = 0.0f;
    float sd = 0.0f;
    if (wvm == 0) {
        sr = re * std::log2f(1.0f + rv);
        sd = de * std::log2f(1.0f + dv);
    } else if (wvm == 1) {
        sr = re * std::log2f(1.0f + rv);
        sd = de * std::log2f(1.0f + rv);
    } else if (wvm == 2) {
        sr = re * std::log2f(1.0f + dv);
        sd = de * std::log2f(1.0f + dv);
    } else if (wvm == 3) {
        float const mv = (rv + dv) * 0.5f;
        sr = re * std::log2f(1.0f + mv);
        sd = de * std::log2f(1.0f + mv);
    } else if (wvm == 4) {
        sr = re * std::log2f(1.0f + rv);
        sd = de * std::log2f(1.0f + (rv + dv) * 0.5f);
    } else if (wvm == 5) {
        sr = re * std::log2f(1.0f + rv);
        sd = de * std::log2f(1.0f + 0.75f * rv + 0.25f * dv);
    } else if (wvm == 6) {
        sr = re * std::log2f(1.0f + rv);
        sd = de * std::log2f(1.0f + 0.25f * rv + 0.75f * dv);
    }
    return std::fabs(sr - sd);
}

static int score_aggregate(SpeedChromaSyclState *s, float *score_out)
{
    sycl::queue &q = *(sycl::queue *)vmaf_sycl_get_queue_ptr(s->sycl_state);
    const uint32_t num_blocks = (uint32_t)s->dim.num_blocks;
    const float sigma_nn = (float)s->opt.speed_sigma_nn;

    /* The kernel reads d_eigenvalues_ref for the ref entropy and d_eigenvalues
     * (now holding the dis eigenvalues) for the dis entropy. */
    launch_score(q, s->d_eigenvalues_ref, s->d_eigenvalues, s->d_sol_ref, s->d_sol_dis,
                 s->d_indterm_ref, s->d_indterm_dis, s->d_ref_ent, s->d_ref_var, s->d_dis_ent,
                 s->d_dis_var, num_blocks, sigma_nn);

    const size_t ab = (size_t)num_blocks * sizeof(float);
    q.memcpy(s->h_ref_ent, s->d_ref_ent, ab);
    q.memcpy(s->h_ref_var, s->d_ref_var, ab);
    q.memcpy(s->h_dis_ent, s->d_dis_ent, ab);
    q.memcpy(s->h_dis_var, s->d_dis_var, ab);
    q.wait();

    const float base_entropy =
        (float)SP_ELEMENTS *
        (std::log2f((1.0f + (float)s->opt.speed_nn_floor) * (float)s->opt.speed_sigma_nn) +
         std::log2f(2.0f * std::numbers::pi_v<float> * std::numbers::e_v<float>));

    float total = 0.0f;
    for (uint32_t i = 0; i < num_blocks; ++i) {
        float const re = s->h_ref_ent[i];
        float const de = s->h_dis_ent[i];
        if (re < base_entropy && de < base_entropy)
            continue;
        float const rv = s->h_ref_var[i];
        float const dv = s->h_dis_var[i];
        total += speed_score_difference(re, de, rv, dv, s->opt.speed_weight_var_mode);
    }
    *score_out = total / (float)num_blocks;
    return 0;
}

// clang-format off
static const VmafOption options_chroma[] = {
    {
        .name = "speed_kernelscale", .help = "scaling factor for the Gaussian kernel",
        .alias = "ks",
        .offset = offsetof(SpeedChromaSyclState, speed_chroma_kernelscale),
        .type = VMAF_OPT_TYPE_DOUBLE, .default_val = {.d = 1.0},
        .min = 0.1, .max = 4.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    }, {
        .name = "speed_prescale", .help = "scaling factor for the frame",
        .alias = "ps",
        .offset = offsetof(SpeedChromaSyclState, speed_chroma_prescale),
        .type = VMAF_OPT_TYPE_DOUBLE, .default_val = {.d = 1.0},
        .min = 0.1, .max = 4.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    }, {
        .name = "speed_prescale_method", .help = "scaling method",
        .alias = "psm",
        .offset = offsetof(SpeedChromaSyclState, speed_chroma_prescale_method),
        .type = VMAF_OPT_TYPE_STRING, .default_val = {.s = "nearest"},
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    }, {
        .name = "speed_sigma_nn", .help = "standard deviation of neural noise",
        .alias = "snn",
        .offset = offsetof(SpeedChromaSyclState, speed_chroma_sigma_nn),
        .type = VMAF_OPT_TYPE_DOUBLE, .default_val = {.d = 0.29},
        .min = 0.1, .max = 2.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    }, {
        .name = "speed_nn_floor", .help = "neural noise floor fraction",
        .alias = "nnf",
        .offset = offsetof(SpeedChromaSyclState, speed_chroma_nn_floor),
        .type = VMAF_OPT_TYPE_DOUBLE, .default_val = {.d = 0.0},
        .min = 0.0, .max = 1.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    }, {
        .name = "speed_max_val", .help = "clip output to this maximum",
        .alias = "mxv",
        .offset = offsetof(SpeedChromaSyclState, speed_chroma_max_val),
        .type = VMAF_OPT_TYPE_DOUBLE, .default_val = {.d = 1000.0},
        .min = 0.0, .max = 1000.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    }, {
        .name = "speed_weight_var_mode", .help = "variance weighting mode (0-6)",
        .alias = "wvm",
        .offset = offsetof(SpeedChromaSyclState, speed_weight_var_mode),
        .type = VMAF_OPT_TYPE_INT, .default_val = {.d = 0},
        .min = 0, .max = 6,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {.name = nullptr},
};
// clang-format on

/* forward decl for init failure cleanup — SY-2a */
static int close_chroma_sycl(VmafFeatureExtractor *fex);

static int speed_chroma_dimensions(enum VmafPixelFormat pix_fmt, unsigned w, unsigned h,
                                   unsigned *cw, unsigned *ch)
{
    *cw = w;
    *ch = h;
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

static void speed_chroma_set_options(SpeedChromaSyclState *s)
{
    s->opt = SpeedInternalOptions{
        .speed_kernelscale = s->speed_chroma_kernelscale,
        .speed_prescale = s->speed_chroma_prescale,
        .speed_prescale_method = s->speed_chroma_prescale_method,
        .speed_sigma_nn = s->speed_chroma_sigma_nn,
        .speed_nn_floor = s->speed_chroma_nn_floor,
        .speed_weight_var_mode = s->speed_weight_var_mode,
    };
}

static void speed_chroma_allocate(SpeedChromaSyclState *s, sycl::queue const &q)
{
    const size_t stride_px = s->float_stride / sizeof(float);
    const size_t nb = s->dim.num_blocks;
    const size_t plane_bytes = s->dim.alloc_height * stride_px * sizeof(float);
    const size_t indterm_bytes = SP_ELEMENTS * nb * sizeof(float);
    const size_t cov_bytes = (size_t)SP_ELEMENTS * SP_ELEMENTS * sizeof(float);
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
    ALLOC_D(d_eigenvalues_ref, SP_ELEMENTS * sizeof(float));
    ALLOC_D(d_ref_ent, score_bytes);
    ALLOC_D(d_ref_var, score_bytes);
    ALLOC_D(d_dis_ent, score_bytes);
    ALLOC_D(d_dis_var, score_bytes);
    ALLOC_H(h_cov_mat, cov_bytes);
    ALLOC_H(h_ref_ent, score_bytes);
    ALLOC_H(h_ref_var, score_bytes);
    ALLOC_H(h_dis_ent, score_bytes);
    ALLOC_H(h_dis_var, score_bytes);
    ALLOC_A(h_plane_ref, plane_bytes);
    ALLOC_A(h_plane_dis, plane_bytes);
    ALLOC_A(h_eigenvalues, SP_ELEMENTS * sizeof(float));
    ALLOC_A(h_eig_scratch, (SP_ELEMENTS * SP_ELEMENTS + 4u * SP_ELEMENTS) * sizeof(float));
    ALLOC_A(h_Q, cov_bytes);
    ALLOC_A(h_R, cov_bytes);
    ALLOC_A(h_qr_scratch, 4u * cov_bytes);
    ALLOC_A(h_indterm_ref, indterm_bytes);
    ALLOC_A(h_indterm_dis, indterm_bytes);
    ALLOC_A(h_qt_scratch, indterm_bytes);

#undef ALLOC_D
#undef ALLOC_H
#undef ALLOC_A
}

static bool speed_chroma_buffers_valid(const SpeedChromaSyclState *s)
{
    return s->d_plane && s->h_plane_ref && s->h_eigenvalues && s->h_Q && s->h_R;
}

static int init_chroma_sycl(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                            unsigned w, unsigned h)
{
    (void)bpc;
    SpeedChromaSyclState *s = (SpeedChromaSyclState *)fex->priv;
    unsigned cw = 0;
    unsigned ch = 0;
    int err = speed_chroma_dimensions(pix_fmt, w, h, &cw, &ch);
    if (err)
        return err;

    s->sycl_state = fex->sycl_state;
    speed_chroma_set_options(s);
    err = speed_internal_init_dimensions(&s->dim, (int)cw, (int)ch, s->opt.speed_prescale);
    if (err)
        return err;
    s->float_stride = speed_internal_float_stride(s->dim.alloc_width);

    sycl::queue const &q = *(sycl::queue *)vmaf_sycl_get_queue_ptr(s->sycl_state);
    speed_chroma_allocate(s, q);
    if (!speed_chroma_buffers_valid(s)) {
        close_chroma_sycl(fex);
        return -ENOMEM;
    }

    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (!s->feature_name_dict) {
        close_chroma_sycl(fex);
        return -ENOMEM;
    }

    return 0;
}

struct SpeedChromaChannelResult {
    float score;
    int err;
    bool singular;
};

static SpeedChromaChannelResult speed_chroma_run_plane(SpeedChromaSyclState *s,
                                                       const VmafPicture *ref_pic, const VmafPicture *dist_pic,
                                                       float *tmp_filter, int plane)
{
    picture_copy(s->h_plane_ref, s->float_stride, ref_pic, -128, ref_pic->bpc, plane);
    speed_internal_filter_and_downscale(&s->dim, &s->opt, s->h_plane_ref, tmp_filter,
                                        s->float_stride);
    picture_copy(s->h_plane_dis, s->float_stride, dist_pic, -128, dist_pic->bpc, plane);
    speed_internal_filter_and_downscale(&s->dim, &s->opt, s->h_plane_dis, tmp_filter,
                                        s->float_stride);

    bool singular_ref = false;
    int err = run_channel(s, s->h_plane_ref, s->h_indterm_ref, s->d_indterm_ref, s->d_sol_ref,
                          &singular_ref);
    if (err)
        return {0.0f, err, false};

    sycl::queue &q = *(sycl::queue *)vmaf_sycl_get_queue_ptr(s->sycl_state);
    q.memcpy(s->d_eigenvalues_ref, s->d_eigenvalues, SP_ELEMENTS * sizeof(float));
    q.wait();

    bool singular_dis = false;
    err = run_channel(s, s->h_plane_dis, s->h_indterm_dis, s->d_indterm_dis, s->d_sol_dis,
                      &singular_dis);
    float score = 0.0f;
    if (!err && singular_ref == singular_dis)
        err = score_aggregate(s, &score);
    return {score, err, singular_ref || singular_dis};
}

static int speed_chroma_append_scores(SpeedChromaSyclState *s,
                                      VmafFeatureCollector *feature_collector, unsigned index,
                                      float score_u, float score_v, float score_uv)
{
    const double mxv = s->speed_chroma_max_val;
    int err = 0;
    err |= vmaf_feature_collector_append_with_dict(
        feature_collector, s->feature_name_dict, "Speed_chroma_feature_speed_chroma_u_score",
        (double)score_u < mxv ? (double)score_u : mxv, index);
    err |= vmaf_feature_collector_append_with_dict(
        feature_collector, s->feature_name_dict, "Speed_chroma_feature_speed_chroma_v_score",
        (double)score_v < mxv ? (double)score_v : mxv, index);
    err |= vmaf_feature_collector_append_with_dict(
        feature_collector, s->feature_name_dict, "Speed_chroma_feature_speed_chroma_uv_score",
        (double)score_uv < mxv ? (double)score_uv : mxv, index);
    return err;
}

static int extract_chroma_sycl(VmafFeatureExtractor *fex, const VmafPicture *ref_pic,
                               const VmafPicture *ref_pic_90, const VmafPicture *dist_pic,
                               const VmafPicture *dist_pic_90, unsigned index,
                               VmafFeatureCollector *feature_collector)
{
    (void)ref_pic_90;
    (void)dist_pic_90;
    SpeedChromaSyclState *s = (SpeedChromaSyclState *)fex->priv;
    const size_t stride_px = s->float_stride / sizeof(float);
    const size_t tmp_size = 2u * s->dim.alloc_height * stride_px;
    float *tmp_filter = (float *)aligned_malloc(tmp_size * sizeof(float), 32);
    if (!tmp_filter)
        return -ENOMEM;

    const SpeedChromaChannelResult u = speed_chroma_run_plane(s, ref_pic, dist_pic, tmp_filter, 1);
    const SpeedChromaChannelResult v = speed_chroma_run_plane(s, ref_pic, dist_pic, tmp_filter, 2);
    aligned_free(tmp_filter);
    if (u.err)
        return u.err;
    if (v.err)
        return v.err;

    const float uv = combine_chroma_uv(u.score, v.score, u.singular, v.singular);
    return speed_chroma_append_scores(s, feature_collector, index, u.score, v.score, uv);
}

static int close_chroma_sycl(VmafFeatureExtractor *fex)
{
    SpeedChromaSyclState *s = (SpeedChromaSyclState *)fex->priv;
    speed_internal_report_singular(&s->singular_tally, "speed_chroma_sycl");
    if (s->sycl_state)
        free_sycl_state(s);
    if (s->feature_name_dict)
        vmaf_dictionary_free(&s->feature_name_dict);
    return 0;
}

static const char *provided_features_chroma[] = {
    "Speed_chroma_feature_speed_chroma_u_score",
    "Speed_chroma_feature_speed_chroma_v_score",
    "Speed_chroma_feature_speed_chroma_uv_score",
    nullptr,
};

/* ADR-0567: real SYCL GPU kernels for speed_chroma. */
/*
 * lint rationale: ADR-1266 ends the
 * file-local helper band before the exported C-linkage descriptor.
 */

extern "C" VmafFeatureExtractor vmaf_fex_speed_chroma_sycl = {
    .name = "speed_chroma_sycl",
    .init = init_chroma_sycl,
    .extract = extract_chroma_sycl,
    .close = close_chroma_sycl,
    .options = options_chroma,
    .priv_size = sizeof(SpeedChromaSyclState),
    .flags = VMAF_FEATURE_EXTRACTOR_SYCL,
    .provided_features = provided_features_chroma,
};
