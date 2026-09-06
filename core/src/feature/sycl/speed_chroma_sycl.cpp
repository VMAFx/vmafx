/**
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
constexpr uint32_t SOLVE_WG = 32u; /* one warp per column */

/* ------------------------------------------------------------------ */
/* SYCL GPU kernels                                                    */
/* ------------------------------------------------------------------ */

/* Kernel 1: means[25] (one global scalar value per element)
 *
 * CPU parity (compute_mean, called from compute_covariance_matrix): each of
 * the 25 means is over a single GLOBAL window across the whole truncated plane
 * at start (er, ec) — NOT per-tile. The historic per-tile origin
 * (tile_y*5 + er) over-read the plane and produced 25*num_blocks block-local
 * means, giving a wrong covariance and ~7x-low SpEED scores. One work-item per
 * element position writes means[elem] (scalar). means[] stays over-allocated
 * (25*num_blocks); only [0, 25) are written/read now. */
static void launch_means(sycl::queue &q, const float *plane, float *means, uint32_t op_w,
                         uint32_t stride_px, uint32_t num_blocks_h, uint32_t num_blocks,
                         uint32_t submatrix_w, uint32_t submatrix_h)
{
    (void)op_w;
    (void)num_blocks_h;
    (void)num_blocks;
    const size_t global = ((SP_ELEMENTS + MEANS_WG - 1u) / MEANS_WG) * MEANS_WG;
    q.submit([&](sycl::handler &cgh) {
        cgh.parallel_for(sycl::nd_range<1>(global, MEANS_WG), [=](sycl::nd_item<1> it) {
            const uint32_t elem = (uint32_t)it.get_global_id(0);
            if (elem >= SP_ELEMENTS)
                return;
            const uint32_t er = elem / SP_BLOCK_SIZE;
            const uint32_t ec = elem % SP_BLOCK_SIZE;
            float acc = 0.0f;
            for (uint32_t i = 0; i < submatrix_h; ++i)
                for (uint32_t j = 0; j < submatrix_w; ++j)
                    acc += plane[(er + i) * stride_px + (ec + j)];
            means[elem] = acc / (float)(submatrix_w * submatrix_h);
        });
    });
}

/* Kernel 2: covariance matrix (625 work-groups, one per (x_index, y_index))
 *
 * CPU parity (compute_covariance): one GLOBAL submatrix sweep with the scalar
 * global means, divided by N once. The historic per-tile loop (displaced
 * origins tile_y*5 + xr, per-tile means, per-tile /N) summed num_blocks
 * block-local covariances instead — wrong matrix, ~7x-low scores. The work-
 * group's threads stride over the submatrix_h × submatrix_w pixels at
 * (xr+i, xc+j)/(yr+i, yc+j), reduce in local memory, and divide by N once. */
static void launch_cov(sycl::queue &q, const float *plane, const float *means, float *cov_mat,
                       uint32_t stride_px, uint32_t num_blocks_h, uint32_t num_blocks,
                       uint32_t submatrix_w, uint32_t submatrix_h)
{
    (void)num_blocks_h;
    (void)num_blocks;
    /* 625 work-groups of COV_WG threads, one per (x_index, y_index) pair. */
    const size_t total_wg = SP_ELEMENTS * SP_ELEMENTS;
    q.submit([&](sycl::handler &cgh) {
        sycl::local_accessor<float, 1> s_partial(sycl::range<1>(COV_WG), cgh);
        cgh.parallel_for(sycl::nd_range<1>(total_wg * COV_WG, COV_WG), [=](sycl::nd_item<1> it) {
            const uint32_t x_index = (uint32_t)(it.get_group(0) / SP_ELEMENTS);
            const uint32_t y_index = (uint32_t)(it.get_group(0) % SP_ELEMENTS);
            const uint32_t tid = (uint32_t)it.get_local_id(0);

            const uint32_t xr = x_index / SP_BLOCK_SIZE;
            const uint32_t xc = x_index % SP_BLOCK_SIZE;
            const uint32_t yr = y_index / SP_BLOCK_SIZE;
            const uint32_t yc = y_index % SP_BLOCK_SIZE;
            const float mean_x = means[x_index];
            const float mean_y = means[y_index];

            const uint32_t total = submatrix_h * submatrix_w;
            float local_sum = 0.0f;
            for (uint32_t p = tid; p < total; p += COV_WG) {
                const uint32_t i = p / submatrix_w;
                const uint32_t j = p % submatrix_w;
                const float vx = plane[(xr + i) * stride_px + (xc + j)];
                const float vy = plane[(yr + i) * stride_px + (yc + j)];
                local_sum += (vx - mean_x) * (vy - mean_y);
            }
            s_partial[tid] = local_sum;
            it.barrier(sycl::access::fence_space::local_space);

            for (uint32_t s = COV_WG / 2u; s > 0u; s >>= 1u) {
                if (tid < s)
                    s_partial[tid] += s_partial[tid + s];
                it.barrier(sycl::access::fence_space::local_space);
            }
            if (tid == 0u)
                cov_mat[x_index * SP_ELEMENTS + y_index] = s_partial[0] / (float)total;
        });
    });
}

/* Kernel 3: independent term */
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

/* Kernel 4: backward substitution (one sub-group per column) */
static void launch_solve(sycl::queue &q, const float *R, float *rhs, uint32_t num_blocks)
{
    /* Each warp (32 threads) handles one column; threads 25-31 idle. */
    const size_t warps = ((num_blocks + 7u) / 8u) * 8u; /* round up to 8 warps per block */
    const size_t global = warps * SOLVE_WG;
    const size_t local = SOLVE_WG * 8u;
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
                if (active && (int32_t)lane == i) {
                    float val = rhs[(uint32_t)i * num_blocks + col];
                    const float denom = R[(uint32_t)i * SP_ELEMENTS + (uint32_t)i];
                    for (uint32_t k = (uint32_t)(i + 1); k < SP_ELEMENTS; ++k)
                        val -= rhs[k * num_blocks + col] * R[(uint32_t)i * SP_ELEMENTS + k];
                    rhs[(uint32_t)i * num_blocks + col] =
                        (sycl::fabs(denom) > 1e-8f) ? val / denom : 0.0f;
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
                float ref_lk = ref_eigenvalues[k] < 0.0f ? 0.0f : ref_eigenvalues[k];
                float dis_lk = dis_eigenvalues[k] < 0.0f ? 0.0f : dis_eigenvalues[k];
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

static int run_channel(SpeedChromaSyclState *s, float *h_plane, float *h_indterm, float *d_indterm,
                       float *d_sol, bool *singular_out)
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

    /* H2D upload. */
    q.memcpy(s->d_plane, h_plane, plane_bytes);
    q.wait();

    /* GPU kernels. */
    launch_means(q, s->d_plane, s->d_means, op_w, stride_px, num_blocks_h, num_blocks, submatrix_w,
                 submatrix_h);
    launch_cov(q, s->d_plane, s->d_means, s->d_cov_mat, stride_px, num_blocks_h, num_blocks,
               submatrix_w, submatrix_h);
    launch_indterm(q, s->d_plane, d_indterm, stride_px, num_blocks_h, num_blocks);
    q.wait();

    /* D2H: cov_mat and indterm. */
    q.memcpy(s->h_cov_mat, s->d_cov_mat, SP_ELEMENTS * SP_ELEMENTS * sizeof(float));
    q.memcpy(h_indterm, d_indterm, indterm_bytes);
    q.wait();

    /* CPU: eigendecomp. */
    const int sz = (int)SP_ELEMENTS;
    const int nb = (int)num_blocks;
    speed_internal_compute_eigenvalues(s->h_cov_mat, s->h_eigenvalues, sz, s->h_eig_scratch);
    bool regular = speed_internal_is_matrix_regular(s->h_eigenvalues, SP_ELEMENTS);

    /* A singular covariance matrix is NOT a failure: the CPU reference zeroes
     * the solution and reports it separately so the caller can impute. The
     * return value stays reserved for hard failures. See ADR-1202. */
    *singular_out = !regular;
    if (!regular) {
        vmaf_log(VMAF_LOG_LEVEL_WARNING,
                 "speed_chroma_sycl: covariance matrix singular, zeroing solution\n");
        memset(h_indterm, 0, indterm_bytes);
    } else {
        speed_internal_qr_factorize(s->h_cov_mat, sz, s->h_Q, s->h_R, s->h_qr_scratch);
        speed_internal_qt_multiply(s->h_Q, h_indterm, sz, nb, s->h_qt_scratch);
        /* H2D: R and Q^T×indterm. */
        q.memcpy(s->d_R, s->h_R, (size_t)sz * (size_t)sz * sizeof(float));
        q.memcpy(d_sol, h_indterm, indterm_bytes);
        q.wait();
        launch_solve(q, s->d_R, d_sol, num_blocks);
        q.wait();
    }

    /* H2D: eigenvalues. */
    q.memcpy(s->d_eigenvalues, s->h_eigenvalues, (size_t)sz * sizeof(float));
    q.wait();
    return 0;
}

/* ------------------------------------------------------------------ */
/* Lifecycle (C wrappers)                                             */
/* ------------------------------------------------------------------ */

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
         std::log2f(2.0f * 3.14159265358979323846f * 2.71828182845904523536f));

    float total = 0.0f;
    for (uint32_t i = 0; i < num_blocks; ++i) {
        float re = s->h_ref_ent[i];
        float de = s->h_dis_ent[i];
        if (re < base_entropy && de < base_entropy)
            continue;
        float rv = s->h_ref_var[i];
        float dv = s->h_dis_var[i];
        const int wvm = s->opt.speed_weight_var_mode;
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
            float mv = (rv + dv) * 0.5f;
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
        total += std::fabs(sr - sd);
    }
    *score_out = total / (float)num_blocks;
    return 0;
}

} /* anonymous namespace */

extern "C" {

static const VmafOption options_chroma[] = {
    {
        .name = "speed_kernelscale",
        .help = "scaling factor for the Gaussian kernel",
        .offset = offsetof(SpeedChromaSyclState, speed_chroma_kernelscale),
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
        .offset = offsetof(SpeedChromaSyclState, speed_chroma_prescale),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val = {.d = 1.0},
        .min = 0.1,
        .max = 4.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "ps",
    },
    {
        .name = "speed_prescale_method",
        .help = "scaling method",
        .offset = offsetof(SpeedChromaSyclState, speed_chroma_prescale_method),
        .type = VMAF_OPT_TYPE_STRING,
        .default_val = {.s = "nearest"},
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "psm",
    },
    {
        .name = "speed_sigma_nn",
        .help = "standard deviation of neural noise",
        .offset = offsetof(SpeedChromaSyclState, speed_chroma_sigma_nn),
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
        .offset = offsetof(SpeedChromaSyclState, speed_chroma_nn_floor),
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
        .offset = offsetof(SpeedChromaSyclState, speed_chroma_max_val),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val = {.d = 1000.0},
        .min = 0.0,
        .max = 1000.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "mxv",
    },
    {
        .name = "speed_weight_var_mode",
        .help = "variance weighting mode (0-6)",
        .offset = offsetof(SpeedChromaSyclState, speed_weight_var_mode),
        .type = VMAF_OPT_TYPE_INT,
        .default_val = {.d = 0},
        .min = 0,
        .max = 6,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "wvm",
    },
    {0},
};

/* forward decl for init failure cleanup — SY-2a */
static int close_chroma_sycl(VmafFeatureExtractor *fex);

static int init_chroma_sycl(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                            unsigned w, unsigned h)
{
    (void)bpc;
    SpeedChromaSyclState *s = (SpeedChromaSyclState *)fex->priv;

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

    s->sycl_state = fex->sycl_state;
    s->opt = SpeedInternalOptions{
        .speed_kernelscale = s->speed_chroma_kernelscale,
        .speed_prescale = s->speed_chroma_prescale,
        .speed_prescale_method = s->speed_chroma_prescale_method,
        .speed_sigma_nn = s->speed_chroma_sigma_nn,
        .speed_nn_floor = s->speed_chroma_nn_floor,
        .speed_weight_var_mode = s->speed_weight_var_mode,
    };

    int err = speed_internal_init_dimensions(&s->dim, (int)cw, (int)ch, s->opt.speed_prescale);
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

    if (!s->d_plane || !s->h_plane_ref || !s->h_eigenvalues || !s->h_Q || !s->h_R) {
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

static int extract_chroma_sycl(VmafFeatureExtractor *fex, VmafPicture *ref_pic,
                               VmafPicture *ref_pic_90, VmafPicture *dist_pic,
                               VmafPicture *dist_pic_90, unsigned index,
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

    sycl::queue &q = *(sycl::queue *)vmaf_sycl_get_queue_ptr(s->sycl_state);
    float score_u = 0.0f;
    float score_v = 0.0f;
    int err_u = 0;
    int err_v = 0;
    bool singular_u = false;
    bool singular_v = false;

    for (int ch = 1; ch <= 2; ++ch) {
        float *h_plane = (ch == 1) ? s->h_plane_ref : s->h_plane_dis;
        float *h_plane_d = (ch == 1) ? s->h_plane_dis : nullptr;
        /* Reuse h_plane_ref/dis for ref/dis of each chroma channel. */
        picture_copy(s->h_plane_ref, s->float_stride, ref_pic, -128, ref_pic->bpc, ch);
        speed_internal_filter_and_downscale(&s->dim, &s->opt, s->h_plane_ref, tmp_filter,
                                            s->float_stride);
        picture_copy(s->h_plane_dis, s->float_stride, dist_pic, -128, dist_pic->bpc, ch);
        speed_internal_filter_and_downscale(&s->dim, &s->opt, s->h_plane_dis, tmp_filter,
                                            s->float_stride);
        (void)h_plane;
        (void)h_plane_d;

        /* Reference channel: GPU pipeline + CPU linalg uploads ref eigenvalues
         * into the shared s->d_eigenvalues buffer. */
        bool singular_ref = false;
        int e = run_channel(s, s->h_plane_ref, s->h_indterm_ref, s->d_indterm_ref, s->d_sol_ref,
                            &singular_ref);
        if (e) {
            if (ch == 1)
                err_u = e;
            else
                err_v = e;
            continue;
        }
        /* Stash the reference eigenvalues aside before the distorted linalg pass
         * overwrites s->d_eigenvalues. The CPU reference (est_params in speed.c)
         * computes SEPARATE ref and dis covariance + eigenvalues; the score
         * kernel needs both. run_channel q.wait()'d its eigenvalue H2D, so the
         * DtoD copy is ordered after it. */
        q.memcpy(s->d_eigenvalues_ref, s->d_eigenvalues, SP_ELEMENTS * sizeof(float));
        q.wait();

        /* Distorted channel: keeps the DIS covariance in h_cov_mat (no
         * save/restore of the ref covariance) and uploads dis eigenvalues into
         * s->d_eigenvalues. */
        bool singular_dis = false;
        e = run_channel(s, s->h_plane_dis, s->h_indterm_dis, s->d_indterm_dis, s->d_sol_dis,
                        &singular_dis);

        /* Exactly one side numerically unstable: report 0 rather than the
         * inflated score a zeroed solution on one side produces. Verbatim the
         * CPU rule in speed_extract_score() (speed.c), which this twin matches. */
        float sc = 0.0f;
        if (!e && singular_ref == singular_dis)
            e = score_aggregate(s, &sc);

        if (ch == 1) {
            err_u = e;
            score_u = sc;
            singular_u = singular_ref || singular_dis;
        } else {
            err_v = e;
            score_v = sc;
            singular_v = singular_ref || singular_dis;
        }
    }

    aligned_free(tmp_filter);

    /* A hard failure (SYCL error, allocation failure) fails the frame, and is NOT
     * the singular-matrix condition -- conflating the two is what made ADR-1202's
     * CUDA launch failure surface as three silent 0.0 scores on an exit-0 run.
     * Singularity arrives via `singular_u` / `singular_v`. */
    if (err_u)
        return err_u;
    if (err_v)
        return err_v;

    const float score_uv = combine_chroma_uv(score_u, score_v, singular_u, singular_v);

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

static int close_chroma_sycl(VmafFeatureExtractor *fex)
{
    SpeedChromaSyclState *s = (SpeedChromaSyclState *)fex->priv;
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
VmafFeatureExtractor vmaf_fex_speed_chroma_sycl = {
    .name = "speed_chroma_sycl",
    .init = init_chroma_sycl,
    .extract = extract_chroma_sycl,
    .close = close_chroma_sycl,
    .options = options_chroma,
    .priv_size = sizeof(SpeedChromaSyclState),
    .provided_features = provided_features_chroma,
    .flags = VMAF_FEATURE_EXTRACTOR_SYCL,
};

} /* extern "C" */
