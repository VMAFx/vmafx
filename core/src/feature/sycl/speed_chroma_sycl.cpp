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

namespace
{

constexpr uint32_t SP_ELEMENTS = 25u;
constexpr uint32_t SP_BLOCK_SIZE = 5u;
constexpr uint32_t COV_WG = 256u;
constexpr uint32_t MEANS_WG = 256u;
constexpr uint32_t INDTERM_WG = 256u;
constexpr uint32_t SCORE_WG = 256u;
constexpr uint32_t SOLVE_WG = 32u; /* one warp per column */

struct CovarianceArgs {
    const float *plane;
    const float *means;
    float *matrix;
    uint32_t stride;
    uint32_t width;
    uint32_t height;
};

struct FloatExpansion {
    float high;
    float low;
};

} // namespace

/* ------------------------------------------------------------------ */
/* SYCL GPU kernels                                                    */
/* ------------------------------------------------------------------ */

namespace
{

static inline FloatExpansion add_product(FloatExpansion sum, float lhs, float rhs)
{
    const float product = lhs * rhs;
    const float product_error = sycl::fma(lhs, rhs, -product);
    const float high = sum.high + product;
    const float bias = high - sum.high;
    const float add_error = (sum.high - (high - bias)) + (product - bias);
    const float tail = sum.low + product_error + add_error;
    const float normalized = high + tail;
    return {.high = normalized, .low = tail - (normalized - high)};
}

} // namespace

namespace
{

static inline FloatExpansion add_expansions(FloatExpansion lhs, FloatExpansion rhs)
{
    const float high = lhs.high + rhs.high;
    const float bias = high - lhs.high;
    const float error = (lhs.high - (high - bias)) + (rhs.high - bias);
    const float tail = lhs.low + rhs.low + error;
    const float normalized = high + tail;
    return {.high = normalized, .low = tail - (normalized - high)};
}

} // namespace

namespace
{

static inline FloatExpansion covariance_sum(const CovarianceArgs &args, uint32_t x_index,
                                            uint32_t y_index, uint32_t thread)
{
    const uint32_t x_row = x_index / SP_BLOCK_SIZE;
    const uint32_t x_column = x_index % SP_BLOCK_SIZE;
    const uint32_t y_row = y_index / SP_BLOCK_SIZE;
    const uint32_t y_column = y_index % SP_BLOCK_SIZE;
    const uint32_t total = args.height * args.width;
    FloatExpansion sum{};
    for (uint32_t position = thread; position < total; position += COV_WG) {
        const uint32_t row = position / args.width;
        const uint32_t column = position % args.width;
        const float x = args.plane[(x_row + row) * args.stride + x_column + column];
        const float y = args.plane[(y_row + row) * args.stride + y_column + column];
        sum = add_product(sum, x - args.means[x_index], y - args.means[y_index]);
    }
    return add_expansions(sum, {});
}

} // namespace

namespace
{

static inline void reduce_covariance(sycl::nd_item<1> item, const CovarianceArgs &args,
                                     const sycl::local_accessor<float, 1> &high,
                                     const sycl::local_accessor<float, 1> &low)
{
    const uint32_t thread = (uint32_t)item.get_local_id(0);
    for (uint32_t span = COV_WG / 2u; span > 0u; span >>= 1u) {
        if (thread < span) {
            FloatExpansion const combined =
                add_expansions({.high = high[thread], .low = low[thread]},
                               {.high = high[thread + span], .low = low[thread + span]});
            high[thread] = combined.high;
            low[thread] = combined.low;
        }
        item.barrier(sycl::access::fence_space::local_space);
    }
    if (thread == 0u) {
        const uint32_t x_index = (uint32_t)(item.get_group(0) / SP_ELEMENTS);
        const uint32_t y_index = (uint32_t)(item.get_group(0) % SP_ELEMENTS);
        const float denominator = (float)(args.height * args.width);
        args.matrix[x_index * SP_ELEMENTS + y_index] = high[0] / denominator + low[0] / denominator;
    }
}

} // namespace

namespace
{

static void launch_cov(sycl::queue &queue, const CovarianceArgs &args)
{
    const size_t groups = (size_t)SP_ELEMENTS * SP_ELEMENTS;
    queue.submit([&](sycl::handler &handler) {
        sycl::local_accessor<float, 1> const high(sycl::range<1>(COV_WG), handler);
        sycl::local_accessor<float, 1> const low(sycl::range<1>(COV_WG), handler);
        handler.parallel_for(
            sycl::nd_range<1>(groups * COV_WG, COV_WG), [=](sycl::nd_item<1> item) {
                const uint32_t x_index = (uint32_t)(item.get_group(0) / SP_ELEMENTS);
                const uint32_t y_index = (uint32_t)(item.get_group(0) % SP_ELEMENTS);
                const uint32_t thread = (uint32_t)item.get_local_id(0);
                FloatExpansion const sum = covariance_sum(args, x_index, y_index, thread);
                high[thread] = sum.high;
                low[thread] = sum.low;
                item.barrier(sycl::access::fence_space::local_space);
                reduce_covariance(item, args, high, low);
            });
    });
}

} // namespace

/* Kernel 3: independent term */
namespace
{

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

} // namespace

/* Kernel 4: backward substitution (one sub-group per column) */
namespace
{

static inline void solve_column(sycl::nd_item<1> item, const float *matrix, float *rhs,
                                uint32_t columns)
{
    const uint32_t column = (uint32_t)(item.get_global_id(0) / SOLVE_WG);
    const uint32_t lane = (uint32_t)(item.get_global_id(0) % SOLVE_WG);
    const bool active = column < columns && lane < SP_ELEMENTS;
    for (int32_t row = (int32_t)(SP_ELEMENTS - 1u); row >= 0; --row) {
        if (active && std::cmp_equal(lane, row)) {
            float value = rhs[(uint32_t)row * columns + column];
            const float pivot = matrix[(uint32_t)row * SP_ELEMENTS + (uint32_t)row];
            for (uint32_t k = (uint32_t)(row + 1); k < SP_ELEMENTS; ++k) {
                value -= rhs[k * columns + column] * matrix[(uint32_t)row * SP_ELEMENTS + k];
            }
            rhs[(uint32_t)row * columns + column] =
                sycl::fabs(pivot) > SPEED_INTERNAL_EIGENVALUE_EPS ? value / pivot : 0.0f;
        }
        sycl::group_barrier(item.get_group());
    }
}

} // namespace

namespace
{

static void launch_solve(sycl::queue &queue, const float *matrix, float *rhs, uint32_t columns)
{
    /* Eight sub-groups share a work-group; every lane reaches each barrier. */
    const size_t subgroups = (size_t)((columns + 7u) / 8u) * 8u;
    const size_t local = (size_t)SOLVE_WG * 8u;
    queue.submit([&](sycl::handler &handler) {
        handler.parallel_for(
            sycl::nd_range<1>(subgroups * SOLVE_WG, local),
            [=](sycl::nd_item<1> item) { solve_column(item, matrix, rhs, columns); });
    });
}

} // namespace

/* Kernel 5: per-tile entropy + score
 *
 * The CPU reference (est_params in speed.c) eigendecomposes SEPARATE ref and
 * dis covariance matrices, so the ref entropy uses ref_eigenvalues and the dis
 * entropy uses dis_eigenvalues — they are NOT shared. The previous single-
 * eigenvalue-array signature reused the dis eigenvalues for both entropies. */
namespace
{

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

} // namespace

/* ------------------------------------------------------------------ */
/* Per-extractor state                                                 */
/* ------------------------------------------------------------------ */

namespace
{

struct SpeedChromaSyclState {
    VmafSyclState *sycl_state;
    SpeedInternalDimensions dim;
    SpeedInternalOptions opt;
    size_t float_stride;
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
    float *h_cov_mat;
    float *h_ref_ent;
    float *h_ref_var;
    float *h_dis_ent;
    float *h_dis_var;
    SpeedInternalSingularTally singular_tally;
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
    double speed_chroma_kernelscale;
    double speed_chroma_prescale;
    char *speed_chroma_prescale_method;
    double speed_chroma_sigma_nn;
    double speed_chroma_nn_floor;
    double speed_chroma_max_val;
    int speed_weight_var_mode;

    VmafDictionary *feature_name_dict;
};

} // namespace

namespace
{

template <typename T> static void free_usm(sycl::queue const &queue, T *&pointer)
{
    if (pointer) {
        sycl::free(pointer, queue);
        pointer = nullptr;
    }
}

template <typename T> static void free_aligned(T *&pointer)
{
    if (pointer) {
        aligned_free(pointer);
        pointer = nullptr;
    }
}

} // namespace

namespace
{

static void free_sycl_state(SpeedChromaSyclState *s)
{
    const sycl::queue &queue = *static_cast<sycl::queue *>(vmaf_sycl_get_queue_ptr(s->sycl_state));
    free_usm(queue, s->d_plane);
    free_usm(queue, s->d_means);
    free_usm(queue, s->d_cov_mat);
    free_usm(queue, s->d_indterm_ref);
    free_usm(queue, s->d_indterm_dis);
    free_usm(queue, s->d_sol_ref);
    free_usm(queue, s->d_sol_dis);
    free_usm(queue, s->d_R);
    free_usm(queue, s->d_eigenvalues);
    free_usm(queue, s->d_eigenvalues_ref);
    free_usm(queue, s->d_ref_ent);
    free_usm(queue, s->d_ref_var);
    free_usm(queue, s->d_dis_ent);
    free_usm(queue, s->d_dis_var);
    free_usm(queue, s->h_cov_mat);
    free_usm(queue, s->h_ref_ent);
    free_usm(queue, s->h_ref_var);
    free_usm(queue, s->h_dis_ent);
    free_usm(queue, s->h_dis_var);
    free_aligned(s->h_plane_ref);
    free_aligned(s->h_plane_dis);
    free_aligned(s->h_eigenvalues);
    free_aligned(s->h_eig_scratch);
    free_aligned(s->h_Q);
    free_aligned(s->h_R);
    free_aligned(s->h_qr_scratch);
    free_aligned(s->h_indterm_ref);
    free_aligned(s->h_indterm_dis);
    free_aligned(s->h_qt_scratch);
}

} // namespace

/* ------------------------------------------------------------------ */
/* GPU + CPU pipeline for one plane                                   */
/* ------------------------------------------------------------------ */

/* uv from u and v, imputing across a singular channel exactly as extract_fex()
 * in speed.c does. */
namespace
{

static float combine_chroma_uv(float score_u, float score_v, bool singular_u, bool singular_v)
{
    if (singular_u && !singular_v)
        return score_v;
    if (singular_v && !singular_u)
        return score_u;
    return (score_u + score_v) * 0.5f;
}

} // namespace

namespace
{

static void upload_channel(SpeedChromaSyclState *s, sycl::queue &queue, float *plane)
{
    const uint32_t stride = (uint32_t)(s->float_stride / sizeof(float));
    const size_t bytes = s->dim.truncated_height * stride * sizeof(float);
    queue.memcpy(s->d_plane, plane, bytes);
    queue.wait();
    float means[SP_ELEMENTS];
    speed_internal_compute_means(&s->dim, plane, means, stride);
    queue.memcpy(s->d_means, means, sizeof(means));
    queue.wait();
}

} // namespace

namespace
{

static void compute_channel_statistics(SpeedChromaSyclState *s, sycl::queue &queue,
                                       float *host_indterm, float *device_indterm)
{
    const uint32_t blocks = (uint32_t)s->dim.num_blocks;
    const uint32_t block_columns = (uint32_t)s->dim.num_blocks_horizontal;
    const uint32_t stride = (uint32_t)(s->float_stride / sizeof(float));
    launch_cov(queue, {.plane = s->d_plane,
                       .means = s->d_means,
                       .matrix = s->d_cov_mat,
                       .stride = stride,
                       .width = (uint32_t)s->dim.submatrix_width,
                       .height = (uint32_t)s->dim.submatrix_height});
    launch_indterm(queue, s->d_plane, device_indterm, stride, block_columns, blocks);
    queue.wait();
    const size_t matrix_bytes = (size_t)SP_ELEMENTS * SP_ELEMENTS * sizeof(float);
    const size_t indterm_bytes = (size_t)SP_ELEMENTS * blocks * sizeof(float);
    queue.memcpy(s->h_cov_mat, s->d_cov_mat, matrix_bytes);
    queue.memcpy(host_indterm, device_indterm, indterm_bytes);
    queue.wait();
}

} // namespace

namespace
{

static bool has_singular_pivot(const float *matrix)
{
    for (uint32_t row = 0; row < SP_ELEMENTS; ++row) {
        if (std::fabs(matrix[row * SP_ELEMENTS + row]) < SPEED_INTERNAL_EIGENVALUE_EPS) {
            return true;
        }
    }
    return false;
}

} // namespace

namespace
{

static void solve_regular_channel(SpeedChromaSyclState *s, sycl::queue &queue, float *host_indterm,
                                  float *device_solution)
{
    const int size = (int)SP_ELEMENTS;
    const uint32_t blocks = (uint32_t)s->dim.num_blocks;
    const size_t matrix_bytes = (size_t)SP_ELEMENTS * SP_ELEMENTS * sizeof(float);
    const size_t indterm_bytes = (size_t)SP_ELEMENTS * blocks * sizeof(float);
    speed_internal_qt_multiply(s->h_Q, host_indterm, size, (int)blocks, s->h_qt_scratch);
    queue.memcpy(s->d_R, s->h_R, matrix_bytes);
    queue.memcpy(device_solution, host_indterm, indterm_bytes);
    queue.wait();
    launch_solve(queue, s->d_R, device_solution, blocks);
    queue.wait();
}

} // namespace

namespace
{

static void solve_channel(SpeedChromaSyclState *s, sycl::queue &queue, float *host_indterm,
                          float *device_solution, bool *singular)
{
    const int size = (int)SP_ELEMENTS;
    const size_t solution_bytes = (size_t)SP_ELEMENTS * s->dim.num_blocks * sizeof(float);
    speed_internal_compute_eigenvalues(s->h_cov_mat, s->h_eigenvalues, size, s->h_eig_scratch);
    *singular = !speed_internal_is_matrix_regular(s->h_eigenvalues, SP_ELEMENTS);
    speed_internal_tally_solve(&s->singular_tally, *singular, "speed_chroma_sycl");
    if (*singular) {
        queue.memset(device_solution, 0, solution_bytes);
        queue.wait();
        return;
    }
    speed_internal_qr_factorize(s->h_cov_mat, size, s->h_Q, s->h_R, s->h_qr_scratch);
    if (has_singular_pivot(s->h_R)) {
        *singular = true;
        vmaf_log(VMAF_LOG_LEVEL_WARNING,
                 "speed_chroma_sycl: R pivot below regularity epsilon, zeroing solution\n");
        queue.memset(device_solution, 0, solution_bytes);
        queue.wait();
        return;
    }
    solve_regular_channel(s, queue, host_indterm, device_solution);
}

} // namespace

namespace
{

static void run_channel(SpeedChromaSyclState *s, float *host_plane, float *host_indterm,
                        float *device_indterm, float *device_solution, bool *singular)
{
    sycl::queue &queue = *static_cast<sycl::queue *>(vmaf_sycl_get_queue_ptr(s->sycl_state));
    upload_channel(s, queue, host_plane);
    compute_channel_statistics(s, queue, host_indterm, device_indterm);
    solve_channel(s, queue, host_indterm, device_solution, singular);
    queue.memcpy(s->d_eigenvalues, s->h_eigenvalues, SP_ELEMENTS * sizeof(float));
    queue.wait();
}

} // namespace

/* ------------------------------------------------------------------ */
/* Lifecycle (C wrappers)                                             */
/* ------------------------------------------------------------------ */

namespace
{

static std::pair<float, float> weighted_scores(float ref_entropy, float dis_entropy,
                                               float ref_variance, float dis_variance, int mode)
{
    if (mode == 0) {
        return {ref_entropy * std::log2f(1.0f + ref_variance),
                dis_entropy * std::log2f(1.0f + dis_variance)};
    }
    if (mode == 1) {
        return {ref_entropy * std::log2f(1.0f + ref_variance),
                dis_entropy * std::log2f(1.0f + ref_variance)};
    }
    if (mode == 2) {
        return {ref_entropy * std::log2f(1.0f + dis_variance),
                dis_entropy * std::log2f(1.0f + dis_variance)};
    }
    const float mean = (ref_variance + dis_variance) * 0.5f;
    if (mode == 3) {
        return {ref_entropy * std::log2f(1.0f + mean), dis_entropy * std::log2f(1.0f + mean)};
    }
    float dis_weight = mean;
    if (mode == 5) {
        dis_weight = 0.75f * ref_variance + 0.25f * dis_variance;
    } else if (mode == 6) {
        dis_weight = 0.25f * ref_variance + 0.75f * dis_variance;
    }
    return {ref_entropy * std::log2f(1.0f + ref_variance),
            dis_entropy * std::log2f(1.0f + dis_weight)};
}

} // namespace

namespace
{

static float block_score(const SpeedChromaSyclState *s, uint32_t block, float entropy_floor)
{
    const float ref_entropy = s->h_ref_ent[block];
    const float dis_entropy = s->h_dis_ent[block];
    if (ref_entropy < entropy_floor && dis_entropy < entropy_floor) {
        return 0.0f;
    }
    auto const [ref_score, dis_score] =
        weighted_scores(ref_entropy, dis_entropy, s->h_ref_var[block], s->h_dis_var[block],
                        s->opt.speed_weight_var_mode);
    return std::fabs(ref_score - dis_score);
}

} // namespace

namespace
{

static int score_aggregate(SpeedChromaSyclState *s, float *score_out)
{
    sycl::queue &queue = *static_cast<sycl::queue *>(vmaf_sycl_get_queue_ptr(s->sycl_state));
    const uint32_t blocks = (uint32_t)s->dim.num_blocks;
    launch_score(queue, s->d_eigenvalues_ref, s->d_eigenvalues, s->d_sol_ref, s->d_sol_dis,
                 s->d_indterm_ref, s->d_indterm_dis, s->d_ref_ent, s->d_ref_var, s->d_dis_ent,
                 s->d_dis_var, blocks, (float)s->opt.speed_sigma_nn);
    const size_t bytes = (size_t)blocks * sizeof(float);
    queue.memcpy(s->h_ref_ent, s->d_ref_ent, bytes);
    queue.memcpy(s->h_ref_var, s->d_ref_var, bytes);
    queue.memcpy(s->h_dis_ent, s->d_dis_ent, bytes);
    queue.memcpy(s->h_dis_var, s->d_dis_var, bytes);
    queue.wait();
    const float entropy_floor =
        (float)SP_ELEMENTS *
        (std::log2f((1.0f + (float)s->opt.speed_nn_floor) * (float)s->opt.speed_sigma_nn) +
         std::log2f(2.0f * std::numbers::pi_v<float> * std::numbers::e_v<float>));
    float total = 0.0f;
    for (uint32_t block = 0; block < blocks; ++block) {
        total += block_score(s, block, entropy_floor);
    }
    *score_out = total / (float)blocks;
    return 0;
}

} // namespace

static const VmafOption option_kernelscale = {
    .name = "speed_kernelscale",
    .help = "scaling factor for the Gaussian kernel",
    .alias = "ks",
    .offset = offsetof(SpeedChromaSyclState, speed_chroma_kernelscale),
    .type = VMAF_OPT_TYPE_DOUBLE,
    .default_val = {.d = 1.0},
    .min = 0.1,
    .max = 4.0,
    .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
};
static const VmafOption option_prescale = {
    .name = "speed_prescale",
    .help = "scaling factor for the frame",
    .alias = "ps",
    .offset = offsetof(SpeedChromaSyclState, speed_chroma_prescale),
    .type = VMAF_OPT_TYPE_DOUBLE,
    .default_val = {.d = 1.0},
    .min = 0.1,
    .max = 4.0,
    .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
};
static const VmafOption option_prescale_method = {
    .name = "speed_prescale_method",
    .help = "scaling method",
    .alias = "psm",
    .offset = offsetof(SpeedChromaSyclState, speed_chroma_prescale_method),
    .type = VMAF_OPT_TYPE_STRING,
    .default_val = {.s = "nearest"},
    .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
};
static const VmafOption option_sigma_nn = {
    .name = "speed_sigma_nn",
    .help = "standard deviation of neural noise",
    .alias = "snn",
    .offset = offsetof(SpeedChromaSyclState, speed_chroma_sigma_nn),
    .type = VMAF_OPT_TYPE_DOUBLE,
    .default_val = {.d = 0.29},
    .min = 0.1,
    .max = 2.0,
    .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
};
static const VmafOption option_nn_floor = {
    .name = "speed_nn_floor",
    .help = "neural noise floor fraction",
    .alias = "nnf",
    .offset = offsetof(SpeedChromaSyclState, speed_chroma_nn_floor),
    .type = VMAF_OPT_TYPE_DOUBLE,
    .default_val = {.d = 0.0},
    .min = 0.0,
    .max = 1.0,
    .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
};
static const VmafOption option_maximum = {
    .name = "speed_max_val",
    .help = "clip output to this maximum",
    .alias = "mxv",
    .offset = offsetof(SpeedChromaSyclState, speed_chroma_max_val),
    .type = VMAF_OPT_TYPE_DOUBLE,
    .default_val = {.d = 1000.0},
    .min = 0.0,
    .max = 1000.0,
    .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
};
static const VmafOption option_weight_mode = {
    .name = "speed_weight_var_mode",
    .help = "variance weighting mode (0-6)",
    .alias = "wvm",
    .offset = offsetof(SpeedChromaSyclState, speed_weight_var_mode),
    .type = VMAF_OPT_TYPE_INT,
    .default_val = {.i = 0},
    .min = 0,
    .max = 6,
    .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
};
static const VmafOption options_chroma[] = {
    option_kernelscale, option_prescale, option_prescale_method, option_sigma_nn,
    option_nn_floor,    option_maximum,  option_weight_mode,     {.name = nullptr},
};

namespace
{

static int close_chroma_sycl(VmafFeatureExtractor *fex);

struct AllocationSizes {
    size_t plane;
    size_t indterm;
    size_t covariance;
    size_t score;
};

} // namespace

namespace
{

static int chroma_dimensions(enum VmafPixelFormat format, unsigned width, unsigned height,
                             unsigned *chroma_width, unsigned *chroma_height)
{
    *chroma_width = width;
    *chroma_height = height;
    switch (format) {
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

} // namespace

namespace
{

static int configure_chroma(SpeedChromaSyclState *s, VmafFeatureExtractor *fex,
                            enum VmafPixelFormat format, unsigned width, unsigned height)
{
    unsigned chroma_width = 0;
    unsigned chroma_height = 0;
    const int dimension_error =
        chroma_dimensions(format, width, height, &chroma_width, &chroma_height);
    if (dimension_error) {
        return dimension_error;
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
    const int error = speed_internal_init_dimensions(&s->dim, (int)chroma_width, (int)chroma_height,
                                                     s->opt.speed_prescale);
    if (!error) {
        s->float_stride = speed_internal_float_stride(s->dim.alloc_width);
    }
    return error;
}

} // namespace

namespace
{

static AllocationSizes allocation_sizes(const SpeedChromaSyclState *s)
{
    const size_t stride = s->float_stride / sizeof(float);
    const size_t blocks = s->dim.num_blocks;
    return {
        .plane = s->dim.alloc_height * stride * sizeof(float),
        .indterm = SP_ELEMENTS * blocks * sizeof(float),
        .covariance = (size_t)SP_ELEMENTS * SP_ELEMENTS * sizeof(float),
        .score = blocks * sizeof(float),
    };
}

} // namespace

namespace
{

static void allocate_device_buffers(SpeedChromaSyclState *s, sycl::queue const &queue,
                                    const AllocationSizes &sizes)
{
    s->d_plane = sycl::malloc_device<float>(sizes.plane / sizeof(float), queue);
    s->d_means = sycl::malloc_device<float>(sizes.indterm / sizeof(float), queue);
    s->d_cov_mat = sycl::malloc_device<float>(sizes.covariance / sizeof(float), queue);
    s->d_indterm_ref = sycl::malloc_device<float>(sizes.indterm / sizeof(float), queue);
    s->d_indterm_dis = sycl::malloc_device<float>(sizes.indterm / sizeof(float), queue);
    s->d_sol_ref = sycl::malloc_device<float>(sizes.indterm / sizeof(float), queue);
    s->d_sol_dis = sycl::malloc_device<float>(sizes.indterm / sizeof(float), queue);
    s->d_R = sycl::malloc_device<float>(sizes.covariance / sizeof(float), queue);
    s->d_eigenvalues = sycl::malloc_device<float>(SP_ELEMENTS, queue);
    s->d_eigenvalues_ref = sycl::malloc_device<float>(SP_ELEMENTS, queue);
    s->d_ref_ent = sycl::malloc_device<float>(sizes.score / sizeof(float), queue);
    s->d_ref_var = sycl::malloc_device<float>(sizes.score / sizeof(float), queue);
    s->d_dis_ent = sycl::malloc_device<float>(sizes.score / sizeof(float), queue);
    s->d_dis_var = sycl::malloc_device<float>(sizes.score / sizeof(float), queue);
}

} // namespace

namespace
{

static void allocate_host_buffers(SpeedChromaSyclState *s, sycl::queue const &queue,
                                  const AllocationSizes &sizes)
{
    s->h_cov_mat = sycl::malloc_host<float>(sizes.covariance / sizeof(float), queue);
    s->h_ref_ent = sycl::malloc_host<float>(sizes.score / sizeof(float), queue);
    s->h_ref_var = sycl::malloc_host<float>(sizes.score / sizeof(float), queue);
    s->h_dis_ent = sycl::malloc_host<float>(sizes.score / sizeof(float), queue);
    s->h_dis_var = sycl::malloc_host<float>(sizes.score / sizeof(float), queue);
    s->h_plane_ref = static_cast<float *>(aligned_malloc(sizes.plane, 32));
    s->h_plane_dis = static_cast<float *>(aligned_malloc(sizes.plane, 32));
    s->h_eigenvalues = static_cast<float *>(aligned_malloc(SP_ELEMENTS * sizeof(float), 32));
    const size_t eigen_scratch = (SP_ELEMENTS * SP_ELEMENTS + 4u * SP_ELEMENTS) * sizeof(float);
    s->h_eig_scratch = static_cast<float *>(aligned_malloc(eigen_scratch, 32));
    s->h_Q = static_cast<float *>(aligned_malloc(sizes.covariance, 32));
    s->h_R = static_cast<float *>(aligned_malloc(sizes.covariance, 32));
    s->h_qr_scratch = static_cast<float *>(aligned_malloc(4u * sizes.covariance, 32));
    s->h_indterm_ref = static_cast<float *>(aligned_malloc(sizes.indterm, 32));
    s->h_indterm_dis = static_cast<float *>(aligned_malloc(sizes.indterm, 32));
    s->h_qt_scratch = static_cast<float *>(aligned_malloc(sizes.indterm, 32));
}

} // namespace

namespace
{

static bool allocations_complete(const SpeedChromaSyclState *s)
{
    return s->d_plane && s->d_means && s->d_cov_mat && s->d_indterm_ref && s->d_indterm_dis &&
           s->d_sol_ref && s->d_sol_dis && s->d_R && s->d_eigenvalues && s->d_eigenvalues_ref &&
           s->d_ref_ent && s->d_ref_var && s->d_dis_ent && s->d_dis_var && s->h_cov_mat &&
           s->h_ref_ent && s->h_ref_var && s->h_dis_ent && s->h_dis_var && s->h_plane_ref &&
           s->h_plane_dis && s->h_eigenvalues && s->h_eig_scratch && s->h_Q && s->h_R &&
           s->h_qr_scratch && s->h_indterm_ref && s->h_indterm_dis && s->h_qt_scratch;
}

} // namespace

namespace
{

static int init_chroma_sycl(VmafFeatureExtractor *fex, enum VmafPixelFormat format, unsigned bpc,
                            unsigned width, unsigned height)
{
    (void)bpc;
    auto *s = static_cast<SpeedChromaSyclState *>(fex->priv);
    const int config_error = configure_chroma(s, fex, format, width, height);
    if (config_error) {
        return config_error;
    }
    const sycl::queue &queue = *static_cast<sycl::queue *>(vmaf_sycl_get_queue_ptr(s->sycl_state));
    const AllocationSizes sizes = allocation_sizes(s);
    allocate_device_buffers(s, queue, sizes);
    allocate_host_buffers(s, queue, sizes);
    if (!allocations_complete(s)) {
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

} // namespace

namespace
{

struct ChromaResult {
    float score;
    int error;
    bool singular;
};

static ChromaResult process_chroma_plane(SpeedChromaSyclState *s, VmafPicture *reference,
                                         VmafPicture *distorted, float *filter, int plane)
{
    picture_copy(s->h_plane_ref, s->float_stride, reference, -128, reference->bpc, plane);
    speed_internal_filter_and_downscale(&s->dim, &s->opt, s->h_plane_ref, filter, s->float_stride);
    picture_copy(s->h_plane_dis, s->float_stride, distorted, -128, distorted->bpc, plane);
    speed_internal_filter_and_downscale(&s->dim, &s->opt, s->h_plane_dis, filter, s->float_stride);
    bool reference_singular = false;
    run_channel(s, s->h_plane_ref, s->h_indterm_ref, s->d_indterm_ref, s->d_sol_ref,
                &reference_singular);
    sycl::queue &queue = *static_cast<sycl::queue *>(vmaf_sycl_get_queue_ptr(s->sycl_state));
    queue.memcpy(s->d_eigenvalues_ref, s->d_eigenvalues, SP_ELEMENTS * sizeof(float));
    queue.wait();
    bool distorted_singular = false;
    run_channel(s, s->h_plane_dis, s->h_indterm_dis, s->d_indterm_dis, s->d_sol_dis,
                &distorted_singular);
    float score = 0.0f;
    int error = 0;
    if (reference_singular == distorted_singular) {
        error = score_aggregate(s, &score);
    }
    return {.score = score, .error = error, .singular = reference_singular || distorted_singular};
}

} // namespace

namespace
{

static int append_chroma_scores(SpeedChromaSyclState *s, VmafFeatureCollector *collector,
                                unsigned index, ChromaResult u, ChromaResult v)
{
    const float uv = combine_chroma_uv(u.score, v.score, u.singular, v.singular);
    const double maximum = s->speed_chroma_max_val;
    int error = 0;
    error |= vmaf_feature_collector_append_with_dict(
        collector, s->feature_name_dict, "Speed_chroma_feature_speed_chroma_u_score",
        (double)u.score < maximum ? (double)u.score : maximum, index);
    error |= vmaf_feature_collector_append_with_dict(
        collector, s->feature_name_dict, "Speed_chroma_feature_speed_chroma_v_score",
        (double)v.score < maximum ? (double)v.score : maximum, index);
    error |= vmaf_feature_collector_append_with_dict(
        collector, s->feature_name_dict, "Speed_chroma_feature_speed_chroma_uv_score",
        (double)uv < maximum ? (double)uv : maximum, index);
    return error;
}

} // namespace

namespace
{

static int extract_chroma_sycl(VmafFeatureExtractor *fex, VmafPicture *reference,
                               VmafPicture *reference_90, VmafPicture *distorted,
                               VmafPicture *distorted_90, unsigned index,
                               VmafFeatureCollector *collector)
{
    (void)reference_90;
    (void)distorted_90;
    auto *s = static_cast<SpeedChromaSyclState *>(fex->priv);
    const size_t stride = s->float_stride / sizeof(float);
    const size_t filter_elements = 2u * s->dim.alloc_height * stride;
    float *filter = static_cast<float *>(aligned_malloc(filter_elements * sizeof(float), 32));
    if (!filter) {
        return -ENOMEM;
    }
    const ChromaResult u = process_chroma_plane(s, reference, distorted, filter, 1);
    const ChromaResult v = process_chroma_plane(s, reference, distorted, filter, 2);
    aligned_free(filter);
    if (u.error) {
        return u.error;
    }
    if (v.error) {
        return v.error;
    }
    return append_chroma_scores(s, collector, index, u, v);
}

} // namespace

namespace
{

static int close_chroma_sycl(VmafFeatureExtractor *fex)
{
    auto *s = static_cast<SpeedChromaSyclState *>(fex->priv);
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

} // namespace

/* ADR-0567: real SYCL GPU kernels for speed_chroma. */

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
