/**
 *  Copyright 2016-2025 Netflix, Inc.
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

#include <bit>
#include <cerrno>
#include <cmath>
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
constexpr uint32_t SOLVE_WG = 32u;

struct SolveArgs {
    const float *matrix;
    float *rhs;
    uint32_t blocks;
};

struct CovarianceOutput {
    float *values;
};

constexpr float SP_PI = std::bit_cast<float>(uint32_t{0x40490fdbU});
constexpr float SP_E = std::bit_cast<float>(uint32_t{0x402df854U});

} // namespace

namespace
{

static inline float mean_element(const float *plane, uint32_t stride, uint32_t width,
                                 uint32_t height, uint32_t element)
{
    const uint32_t element_row = element / SP_BLOCK_SIZE;
    const uint32_t element_col = element % SP_BLOCK_SIZE;
    float sum = 0.0f;
    for (uint32_t row = 0; row < height; ++row) {
        for (uint32_t col = 0; col < width; ++col) {
            sum += plane[(element_row + row) * stride + (element_col + col)];
        }
    }
    return sum / (float)(width * height);
}

} // namespace

namespace
{

/* ------------------------------------------------------------------ */
/* SYCL GPU kernels (identical to speed_chroma_sycl.cpp)             */
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
    const size_t global =
        ((static_cast<size_t>(SP_ELEMENTS) + MEANS_WG - 1u) / MEANS_WG) * MEANS_WG;
    q.submit([&](sycl::handler &cgh) {
        cgh.parallel_for(sycl::nd_range<1>(global, MEANS_WG), [=](sycl::nd_item<1> it) {
            const uint32_t elem = (uint32_t)it.get_global_id(0);
            if (elem >= SP_ELEMENTS) {
                return;
            }
            means[elem] = mean_element(plane, stride_px, submatrix_w, submatrix_h, elem);
        });
    });
}

} // namespace

namespace
{

static inline float covariance_sum(const float *plane, const float *means, uint32_t stride,
                                   uint32_t width, uint32_t height, uint32_t x_index,
                                   uint32_t y_index, uint32_t thread)
{
    const uint32_t x_row = x_index / SP_BLOCK_SIZE;
    const uint32_t x_col = x_index % SP_BLOCK_SIZE;
    const uint32_t y_row = y_index / SP_BLOCK_SIZE;
    const uint32_t y_col = y_index % SP_BLOCK_SIZE;
    const float mean_x = means[x_index];
    const float mean_y = means[y_index];
    const uint32_t total = height * width;
    float sum = 0.0f;
    for (uint32_t pixel = thread; pixel < total; pixel += COV_WG) {
        const uint32_t row = pixel / width;
        const uint32_t col = pixel % width;
        const float x = plane[(x_row + row) * stride + (x_col + col)];
        const float y = plane[(y_row + row) * stride + (y_col + col)];
        sum += (x - mean_x) * (y - mean_y);
    }
    return sum;
}

} // namespace

namespace
{

static inline void reduce_covariance(sycl::nd_item<1> item,
                                     const sycl::local_accessor<float, 1> &partial,
                                     float *covariance, uint32_t x_index, uint32_t y_index,
                                     uint32_t count)
{
    const uint32_t thread = (uint32_t)item.get_local_id(0);
    for (uint32_t width = COV_WG / 2u; width > 0u; width >>= 1u) {
        if (thread < width) {
            partial[thread] += partial[thread + width];
        }
        item.barrier(sycl::access::fence_space::local_space);
    }
    if (thread == 0u) {
        covariance[x_index * SP_ELEMENTS + y_index] = partial[0] / (float)count;
    }
}

} // namespace

namespace
{

/* Kernel 2: covariance matrix (625 work-groups, one per (x_index, y_index))
 *
 * CPU parity (compute_covariance): one GLOBAL submatrix sweep with the scalar
 * global means, divided by N once. The historic per-tile loop (displaced
 * origins tile_y*5 + xr, per-tile means, per-tile /N) summed num_blocks
 * block-local covariances instead — wrong matrix, ~7x-low scores. The work-
 * group's threads stride over the submatrix_h × submatrix_w pixels at
 * (xr+i, xc+j)/(yr+i, yc+j), reduce in local memory, and divide by N once. */
static void launch_cov(sycl::queue &q, const float *plane, const float *means,
                       CovarianceOutput output, uint32_t stride_px, uint32_t num_blocks_h,
                       uint32_t num_blocks, uint32_t submatrix_w, uint32_t submatrix_h)
{
    (void)num_blocks_h;
    (void)num_blocks;
    /* 625 work-groups of COV_WG threads, one per (x_index, y_index) pair. */
    const size_t total_wg = static_cast<size_t>(SP_ELEMENTS) * SP_ELEMENTS;
    q.submit([&](sycl::handler &cgh) {
        sycl::local_accessor<float, 1> const s_partial(sycl::range<1>(COV_WG), cgh);
        cgh.parallel_for(sycl::nd_range<1>(total_wg * COV_WG, COV_WG), [=](sycl::nd_item<1> it) {
            const uint32_t x_index = (uint32_t)(it.get_group(0) / SP_ELEMENTS);
            const uint32_t y_index = (uint32_t)(it.get_group(0) % SP_ELEMENTS);
            const uint32_t tid = (uint32_t)it.get_local_id(0);

            const uint32_t total = submatrix_h * submatrix_w;
            const float local_sum = covariance_sum(plane, means, stride_px, submatrix_w,
                                                   submatrix_h, x_index, y_index, tid);
            s_partial[tid] = local_sum;
            it.barrier(sycl::access::fence_space::local_space);

            reduce_covariance(it, s_partial, output.values, x_index, y_index, total);
        });
    });
}

} // namespace

namespace
{

static inline float entropy_term(float eigenvalue, float variance, float sigma, float constant)
{
    const float clamped = eigenvalue < 0.0f ? 0.0f : eigenvalue;
    return sycl::log2(clamped * variance + sigma) + constant;
}

static inline float entropy_sum(const float *values, float variance, float sigma, float constant)
{
    float sum = 0.0f;
    sum += entropy_term(values[0], variance, sigma, constant);
    sum += entropy_term(values[1], variance, sigma, constant);
    sum += entropy_term(values[2], variance, sigma, constant);
    sum += entropy_term(values[3], variance, sigma, constant);
    sum += entropy_term(values[4], variance, sigma, constant);
    sum += entropy_term(values[5], variance, sigma, constant);
    sum += entropy_term(values[6], variance, sigma, constant);
    sum += entropy_term(values[7], variance, sigma, constant);
    sum += entropy_term(values[8], variance, sigma, constant);
    sum += entropy_term(values[9], variance, sigma, constant);
    sum += entropy_term(values[10], variance, sigma, constant);
    sum += entropy_term(values[11], variance, sigma, constant);
    sum += entropy_term(values[12], variance, sigma, constant);
    sum += entropy_term(values[13], variance, sigma, constant);
    sum += entropy_term(values[14], variance, sigma, constant);
    sum += entropy_term(values[15], variance, sigma, constant);
    sum += entropy_term(values[16], variance, sigma, constant);
    sum += entropy_term(values[17], variance, sigma, constant);
    sum += entropy_term(values[18], variance, sigma, constant);
    sum += entropy_term(values[19], variance, sigma, constant);
    sum += entropy_term(values[20], variance, sigma, constant);
    sum += entropy_term(values[21], variance, sigma, constant);
    sum += entropy_term(values[22], variance, sigma, constant);
    sum += entropy_term(values[23], variance, sigma, constant);
    sum += entropy_term(values[24], variance, sigma, constant);
    return sum;
}

} // namespace

namespace
{

static void launch_temporal_indterm(sycl::queue &q, const float *plane, float *indterm,
                                    uint32_t stride_px, uint32_t num_blocks_h, uint32_t num_blocks)
{
    const uint32_t total = SP_ELEMENTS * num_blocks;
    const size_t global =
        ((static_cast<size_t>(total) + INDTERM_WG - 1u) / INDTERM_WG) * INDTERM_WG;
    q.submit([&](sycl::handler &cgh) {
        cgh.parallel_for(sycl::nd_range<1>(global, INDTERM_WG), [=](sycl::nd_item<1> it) {
            const uint32_t idx = (uint32_t)it.get_global_id(0);
            if (idx >= total) {
                return;
            }
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

namespace
{

static inline void solve_step(const SolveArgs &args, uint32_t warp, uint32_t lane, int32_t row)
{
    if (warp >= args.blocks || !std::cmp_equal(lane, row)) {
        return;
    }
    float value = args.rhs[(uint32_t)row * args.blocks + warp];
    const float denominator = args.matrix[(uint32_t)row * SP_ELEMENTS + (uint32_t)row];
    for (uint32_t k = (uint32_t)(row + 1); k < SP_ELEMENTS; ++k) {
        value -= args.rhs[k * args.blocks + warp] * args.matrix[(uint32_t)row * SP_ELEMENTS + k];
    }
    args.rhs[(uint32_t)row * args.blocks + warp] =
        (sycl::fabs(denominator) > 1e-8f) ? value / denominator : 0.0f;
}

} // namespace

namespace
{

static void launch_solve(sycl::queue &q, const SolveArgs &args)
{
    const size_t warps = ((static_cast<size_t>(args.blocks) + 7u) / 8u) * 8u;
    const size_t global = warps * SOLVE_WG;
    const size_t local = static_cast<size_t>(SOLVE_WG) * 8u;
    q.submit([&](sycl::handler &cgh) {
        cgh.parallel_for(sycl::nd_range<1>(global, local), [=](sycl::nd_item<1> it) {
            const uint32_t warp_id = (uint32_t)(it.get_global_id(0) / SOLVE_WG);
            const uint32_t lane = (uint32_t)(it.get_global_id(0) % SOLVE_WG);
            for (int32_t i = (int32_t)(SP_ELEMENTS - 1u); i >= 0; --i) {
                solve_step(args, warp_id, lane, i);
                sycl::group_barrier(it.get_group());
            }
        });
    });
}

} // namespace

namespace
{

/* Kernel 5: per-tile entropy + score
 *
 * The CPU reference (est_params in speed.c) eigendecomposes SEPARATE ref and
 * dis covariance matrices, so the ref entropy uses ref_eigenvalues and the dis
 * entropy uses dis_eigenvalues — they are NOT shared. The previous single-
 * eigenvalue-array signature reused the dis eigenvalues for both entropies. */
static void launch_temporal_score(sycl::queue &q, const float *ref_eigenvalues,
                                  const float *dis_eigenvalues, const float *ref_sol,
                                  const float *dis_sol, const float *ref_indterm,
                                  const float *dis_indterm, float *ref_ent, float *ref_var,
                                  float *dis_ent, float *dis_var, uint32_t num_blocks,
                                  float sigma_nn)
{
    const size_t global = ((static_cast<size_t>(num_blocks) + SCORE_WG - 1u) / SCORE_WG) * SCORE_WG;
    const float log2e_2pi = sycl::log2(2.0f * SP_PI * SP_E);
    q.submit([&](sycl::handler &cgh) {
        cgh.parallel_for(sycl::nd_range<1>(global, SCORE_WG), [=](sycl::nd_item<1> it) {
            const uint32_t tile = (uint32_t)it.get_global_id(0);
            if (tile >= num_blocks) {
                return;
            }
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
            const float re = entropy_sum(ref_eigenvalues, rv, sigma_nn, log2e_2pi);
            const float de = entropy_sum(dis_eigenvalues, dv, sigma_nn, log2e_2pi);
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

struct SpeedTemporalSyclState {
    VmafSyclState *sycl_state;
    SpeedInternalDimensions dim;
    SpeedInternalOptions opt;
    size_t float_stride;
    float *h_ref[2];
    float *h_dis[2];
    float *d_plane;
    float *d_means;
    float *d_cov_mat;
    float *d_indterm_ref;
    float *d_indterm_dis;
    float *d_sol_ref;
    float *d_sol_dis;
    float *d_R;
    float *d_eigenvalues;
    float *d_eigenvalues_ref;
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
};

} // namespace

namespace
{

static void free_device_pointer(const sycl::queue &queue, float *&pointer)
{
    if (pointer) {
        sycl::free(pointer, queue);
        pointer = nullptr;
    }
}

static void free_aligned_pointer(float *&pointer)
{
    if (pointer) {
        aligned_free(pointer);
        pointer = nullptr;
    }
}

} // namespace

namespace
{

static void free_device_buffers(SpeedTemporalSyclState *s, const sycl::queue &queue)
{
    free_device_pointer(queue, s->d_plane);
    free_device_pointer(queue, s->d_means);
    free_device_pointer(queue, s->d_cov_mat);
    free_device_pointer(queue, s->d_indterm_ref);
    free_device_pointer(queue, s->d_indterm_dis);
    free_device_pointer(queue, s->d_sol_ref);
    free_device_pointer(queue, s->d_sol_dis);
    free_device_pointer(queue, s->d_R);
    free_device_pointer(queue, s->d_eigenvalues);
    free_device_pointer(queue, s->d_eigenvalues_ref);
    free_device_pointer(queue, s->d_ref_ent);
    free_device_pointer(queue, s->d_ref_var);
    free_device_pointer(queue, s->d_dis_ent);
    free_device_pointer(queue, s->d_dis_var);
    free_device_pointer(queue, s->h_cov_mat);
    free_device_pointer(queue, s->h_ref_ent);
    free_device_pointer(queue, s->h_ref_var);
    free_device_pointer(queue, s->h_dis_ent);
    free_device_pointer(queue, s->h_dis_var);
}

} // namespace

namespace
{

static void free_host_buffers(SpeedTemporalSyclState *s)
{
    free_aligned_pointer(s->h_ref[0]);
    free_aligned_pointer(s->h_ref[1]);
    free_aligned_pointer(s->h_dis[0]);
    free_aligned_pointer(s->h_dis[1]);
    free_aligned_pointer(s->h_eigenvalues);
    free_aligned_pointer(s->h_eig_scratch);
    free_aligned_pointer(s->h_Q);
    free_aligned_pointer(s->h_R);
    free_aligned_pointer(s->h_qr_scratch);
    free_aligned_pointer(s->h_indterm_ref);
    free_aligned_pointer(s->h_indterm_dis);
    free_aligned_pointer(s->h_qt_scratch);
}

static void free_sycl_state_st(SpeedTemporalSyclState *s)
{
    const auto *queue = static_cast<const sycl::queue *>(vmaf_sycl_get_queue_ptr(s->sycl_state));
    free_device_buffers(s, *queue);
    free_host_buffers(s);
}

} // namespace

namespace
{

static void subtract_plane(float *a, const float *b, int w, int h, size_t stride_bytes)
{
    const size_t stride_px = stride_bytes / sizeof(float);
    for (int i = 0; i < h; ++i) {
        for (int j = 0; j < w; ++j) {
            a[(size_t)i * stride_px + (size_t)j] -= b[(size_t)i * stride_px + (size_t)j];
        }
    }
}

} // namespace

namespace
{

static void compute_channel_matrices(SpeedTemporalSyclState *s, float *host_plane,
                                     float *host_indterm, float *device_indterm, uint32_t blocks,
                                     uint32_t block_columns, uint32_t stride, uint32_t matrix_width,
                                     uint32_t matrix_height)
{
    sycl::queue &queue = *static_cast<sycl::queue *>(vmaf_sycl_get_queue_ptr(s->sycl_state));
    const size_t plane_bytes =
        static_cast<size_t>(s->dim.truncated_height) * stride * sizeof(float);
    const size_t indterm_bytes = (size_t)SP_ELEMENTS * blocks * sizeof(float);
    queue.memcpy(s->d_plane, host_plane, plane_bytes);
    queue.wait();
    launch_means(queue, s->d_plane, s->d_means, (uint32_t)s->dim.truncated_width, stride,
                 block_columns, blocks, matrix_width, matrix_height);
    launch_cov(queue, s->d_plane, s->d_means, {.values = s->d_cov_mat}, stride, block_columns,
               blocks, matrix_width, matrix_height);
    launch_temporal_indterm(queue, s->d_plane, device_indterm, stride, block_columns, blocks);
    queue.wait();
    queue.memcpy(s->h_cov_mat, s->d_cov_mat, (size_t)SP_ELEMENTS * SP_ELEMENTS * sizeof(float));
    queue.memcpy(host_indterm, device_indterm, indterm_bytes);
    queue.wait();
}

} // namespace

namespace
{

static void solve_channel(SpeedTemporalSyclState *s, float *host_indterm, float *device_solution,
                          uint32_t blocks, bool regular)
{
    sycl::queue &queue = *static_cast<sycl::queue *>(vmaf_sycl_get_queue_ptr(s->sycl_state));
    const size_t indterm_bytes = (size_t)SP_ELEMENTS * blocks * sizeof(float);
    if (!regular) {
        queue.memset(device_solution, 0, indterm_bytes);
        queue.wait();
        return;
    }
    const int size = (int)SP_ELEMENTS;
    speed_internal_qr_factorize(s->h_cov_mat, size, s->h_Q, s->h_R, s->h_qr_scratch);
    speed_internal_qt_multiply(s->h_Q, host_indterm, size, (int)blocks, s->h_qt_scratch);
    queue.memcpy(s->d_R, s->h_R, (size_t)size * (size_t)size * sizeof(float));
    queue.memcpy(device_solution, host_indterm, indterm_bytes);
    queue.wait();
    launch_solve(queue, {.matrix = s->d_R, .rhs = device_solution, .blocks = blocks});
    queue.wait();
}

} // namespace

namespace
{

static void run_channel_st(SpeedTemporalSyclState *s, float *h_plane, float *h_indterm,
                           float *d_indterm, float *d_sol, bool *singular_out)
{
    sycl::queue &q = *static_cast<sycl::queue *>(vmaf_sycl_get_queue_ptr(s->sycl_state));
    const uint32_t num_blocks = (uint32_t)s->dim.num_blocks;
    const uint32_t num_blocks_h = (uint32_t)s->dim.num_blocks_horizontal;
    const uint32_t stride_px = (uint32_t)(s->float_stride / sizeof(float));
    const uint32_t submatrix_w = (uint32_t)s->dim.submatrix_width;
    const uint32_t submatrix_h = (uint32_t)s->dim.submatrix_height;
    compute_channel_matrices(s, h_plane, h_indterm, d_indterm, num_blocks, num_blocks_h, stride_px,
                             submatrix_w, submatrix_h);
    const int sz = (int)SP_ELEMENTS;
    speed_internal_compute_eigenvalues(s->h_cov_mat, s->h_eigenvalues, sz, s->h_eig_scratch);
    bool const regular = speed_internal_is_matrix_regular(s->h_eigenvalues, SP_ELEMENTS);
    *singular_out = !regular;
    speed_internal_tally_solve(&s->singular_tally, !regular, "speed_temporal_sycl");
    solve_channel(s, h_indterm, d_sol, num_blocks, regular);
    q.memcpy(s->d_eigenvalues, s->h_eigenvalues, (size_t)sz * sizeof(float));
    q.wait();
}

} // namespace

namespace
{

static void score_aggregate_st(SpeedTemporalSyclState *s, float *score_out)
{
    sycl::queue &q = *static_cast<sycl::queue *>(vmaf_sycl_get_queue_ptr(s->sycl_state));
    const uint32_t num_blocks = (uint32_t)s->dim.num_blocks;
    const float sigma_nn = (float)s->opt.speed_sigma_nn;

    /* The kernel reads d_eigenvalues_ref for the ref entropy and d_eigenvalues
     * (now holding the dis eigenvalues) for the dis entropy. */
    launch_temporal_score(q, s->d_eigenvalues_ref, s->d_eigenvalues, s->d_sol_ref, s->d_sol_dis,
                          s->d_indterm_ref, s->d_indterm_dis, s->d_ref_ent, s->d_ref_var,
                          s->d_dis_ent, s->d_dis_var, num_blocks, sigma_nn);

    const size_t ab = (size_t)num_blocks * sizeof(float);
    q.memcpy(s->h_ref_ent, s->d_ref_ent, ab);
    q.memcpy(s->h_ref_var, s->d_ref_var, ab);
    q.memcpy(s->h_dis_ent, s->d_dis_ent, ab);
    q.memcpy(s->h_dis_var, s->d_dis_var, ab);
    q.wait();

    const float base_entropy =
        (float)SP_ELEMENTS *
        (std::log2f((1.0f + (float)s->opt.speed_nn_floor) * (float)s->opt.speed_sigma_nn) +
         std::log2f(2.0f * SP_PI * SP_E));

    float total = 0.0f;
    for (uint32_t i = 0; i < num_blocks; ++i) {
        const float re = s->h_ref_ent[i];
        const float de = s->h_dis_ent[i];
        if (re < base_entropy && de < base_entropy) {
            continue;
        }
        const float rv = s->h_ref_var[i];
        const float dv = s->h_dis_var[i];
        /* speed_temporal uses weight_var_mode = 0. */
        float const spatial_ref = re * std::log2f(1.0f + rv);
        float const spatial_dis = de * std::log2f(1.0f + dv);
        total += std::fabs(spatial_ref - spatial_dis);
    }
    *score_out = total / (float)num_blocks;
}

} // namespace

namespace
{

static const VmafOption kernelscale_option = {
    .name = "speed_kernelscale",
    .help = "scaling factor for the Gaussian kernel",
    .alias = "ks",
    .offset = offsetof(SpeedTemporalSyclState, speed_temporal_kernelscale),
    .type = VMAF_OPT_TYPE_DOUBLE,
    .default_val = {.d = 1.0},
    .min = 0.1,
    .max = 4.0,
    .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
};
static const VmafOption prescale_option = {
    .name = "speed_prescale",
    .help = "scaling factor for the frame",
    .alias = "ps",
    .offset = offsetof(SpeedTemporalSyclState, speed_temporal_prescale),
    .type = VMAF_OPT_TYPE_DOUBLE,
    .default_val = {.d = 1.0},
    .min = 0.1,
    .max = 4.0,
    .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
};

} // namespace

namespace
{

static const VmafOption prescale_method_option = {
    .name = "speed_prescale_method",
    .help = "scaling method [nearest, bilinear, bicubic, lanczos4]",
    .alias = "psm",
    .offset = offsetof(SpeedTemporalSyclState, speed_temporal_prescale_method),
    .type = VMAF_OPT_TYPE_STRING,
    .default_val = {.s = "nearest"},
    .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
};
static const VmafOption sigma_option = {
    .name = "speed_sigma_nn",
    .help = "standard deviation of neural noise",
    .alias = "snn",
    .offset = offsetof(SpeedTemporalSyclState, speed_temporal_sigma_nn),
    .type = VMAF_OPT_TYPE_DOUBLE,
    .default_val = {.d = 0.29},
    .min = 0.1,
    .max = 2.0,
    .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
};

} // namespace

namespace
{

static const VmafOption floor_option = {
    .name = "speed_nn_floor",
    .help = "neural noise floor fraction",
    .alias = "nnf",
    .offset = offsetof(SpeedTemporalSyclState, speed_temporal_nn_floor),
    .type = VMAF_OPT_TYPE_DOUBLE,
    .default_val = {.d = 0.0},
    .min = 0.0,
    .max = 1.0,
    .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
};
static const VmafOption max_value_option = {
    .name = "speed_max_val",
    .help = "clip output to this maximum",
    .alias = "mxv",
    .offset = offsetof(SpeedTemporalSyclState, speed_temporal_max_val),
    .type = VMAF_OPT_TYPE_DOUBLE,
    .default_val = {.d = 1000.0},
    .min = 0.0,
    .max = 1000.0,
    .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
};

} // namespace

namespace
{

static const VmafOption use_ref_option = {
    .name = "speed_use_ref_diff",
    .help = "use reference frame difference instead of distorted",
    .alias = "urd",
    .offset = offsetof(SpeedTemporalSyclState, speed_temporal_use_ref_diff),
    .type = VMAF_OPT_TYPE_BOOL,
    .default_val = {.b = false},
    .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
};
static const VmafOption options_temporal[] = {
    kernelscale_option, prescale_option,  prescale_method_option, sigma_option,
    floor_option,       max_value_option, use_ref_option,         {.name = nullptr},
};

} // namespace

namespace
{

/* forward decl for init failure cleanup — SY-2a */
static int close_temporal_sycl(VmafFeatureExtractor *fex);

} // namespace

namespace
{

static void allocate_device_buffers(SpeedTemporalSyclState *s, const sycl::queue &queue,
                                    size_t plane_bytes, size_t indterm_bytes, size_t cov_bytes,
                                    size_t score_bytes)
{
    s->d_plane = sycl::malloc_device<float>(plane_bytes / sizeof(float), queue);
    s->d_means = sycl::malloc_device<float>(indterm_bytes / sizeof(float), queue);
    s->d_cov_mat = sycl::malloc_device<float>(cov_bytes / sizeof(float), queue);
    s->d_indterm_ref = sycl::malloc_device<float>(indterm_bytes / sizeof(float), queue);
    s->d_indterm_dis = sycl::malloc_device<float>(indterm_bytes / sizeof(float), queue);
    s->d_sol_ref = sycl::malloc_device<float>(indterm_bytes / sizeof(float), queue);
    s->d_sol_dis = sycl::malloc_device<float>(indterm_bytes / sizeof(float), queue);
    s->d_R = sycl::malloc_device<float>(cov_bytes / sizeof(float), queue);
    s->d_eigenvalues = sycl::malloc_device<float>(SP_ELEMENTS, queue);
    s->d_eigenvalues_ref = sycl::malloc_device<float>(SP_ELEMENTS, queue);
    s->d_ref_ent = sycl::malloc_device<float>(score_bytes / sizeof(float), queue);
    s->d_ref_var = sycl::malloc_device<float>(score_bytes / sizeof(float), queue);
    s->d_dis_ent = sycl::malloc_device<float>(score_bytes / sizeof(float), queue);
    s->d_dis_var = sycl::malloc_device<float>(score_bytes / sizeof(float), queue);
}

} // namespace

namespace
{

static void allocate_host_buffers(SpeedTemporalSyclState *s, const sycl::queue &queue,
                                  size_t plane_bytes, size_t indterm_bytes, size_t cov_bytes,
                                  size_t score_bytes)
{
    s->h_cov_mat = sycl::malloc_host<float>(cov_bytes / sizeof(float), queue);
    s->h_ref_ent = sycl::malloc_host<float>(score_bytes / sizeof(float), queue);
    s->h_ref_var = sycl::malloc_host<float>(score_bytes / sizeof(float), queue);
    s->h_dis_ent = sycl::malloc_host<float>(score_bytes / sizeof(float), queue);
    s->h_dis_var = sycl::malloc_host<float>(score_bytes / sizeof(float), queue);
    s->h_ref[0] = static_cast<float *>(aligned_malloc(plane_bytes, 32));
    s->h_ref[1] = static_cast<float *>(aligned_malloc(plane_bytes, 32));
    s->h_dis[0] = static_cast<float *>(aligned_malloc(plane_bytes, 32));
    s->h_dis[1] = static_cast<float *>(aligned_malloc(plane_bytes, 32));
    s->h_eigenvalues = static_cast<float *>(aligned_malloc(SP_ELEMENTS * sizeof(float), 32));
    s->h_eig_scratch = static_cast<float *>(
        aligned_malloc((SP_ELEMENTS * SP_ELEMENTS + 4u * SP_ELEMENTS) * sizeof(float), 32));
    s->h_Q = static_cast<float *>(aligned_malloc(cov_bytes, 32));
    s->h_R = static_cast<float *>(aligned_malloc(cov_bytes, 32));
    s->h_qr_scratch = static_cast<float *>(aligned_malloc(4u * cov_bytes, 32));
    s->h_indterm_ref = static_cast<float *>(aligned_malloc(indterm_bytes, 32));
    s->h_indterm_dis = static_cast<float *>(aligned_malloc(indterm_bytes, 32));
    s->h_qt_scratch = static_cast<float *>(aligned_malloc(indterm_bytes, 32));
}

} // namespace

namespace
{

static bool allocation_complete(const SpeedTemporalSyclState *s)
{
    return s->d_plane && s->d_means && s->d_cov_mat && s->d_indterm_ref && s->d_indterm_dis &&
           s->d_sol_ref && s->d_sol_dis && s->d_R && s->d_eigenvalues && s->d_eigenvalues_ref &&
           s->d_ref_ent && s->d_ref_var && s->d_dis_ent && s->d_dis_var && s->h_cov_mat &&
           s->h_ref_ent && s->h_ref_var && s->h_dis_ent && s->h_dis_var && s->h_ref[0] &&
           s->h_ref[1] && s->h_dis[0] && s->h_dis[1] && s->h_eigenvalues && s->h_eig_scratch &&
           s->h_Q && s->h_R && s->h_qr_scratch && s->h_indterm_ref && s->h_indterm_dis &&
           s->h_qt_scratch;
}

} // namespace

namespace
{

static int init_temporal_sycl(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                              unsigned w, unsigned h)
{
    (void)pix_fmt;
    (void)bpc;
    auto *s = static_cast<SpeedTemporalSyclState *>(fex->priv);

    s->sycl_state = fex->sycl_state;
    s->opt = SpeedInternalOptions{
        .speed_kernelscale = s->speed_temporal_kernelscale,
        .speed_prescale = s->speed_temporal_prescale,
        .speed_prescale_method = s->speed_temporal_prescale_method,
        .speed_sigma_nn = s->speed_temporal_sigma_nn,
        .speed_nn_floor = s->speed_temporal_nn_floor,
        .speed_weight_var_mode = 0,
    };

    int const err = speed_internal_init_dimensions(&s->dim, (int)w, (int)h, s->opt.speed_prescale);
    if (err) {
        return err;
    }
    s->float_stride = speed_internal_float_stride(s->dim.alloc_width);

    const sycl::queue &q = *static_cast<sycl::queue *>(vmaf_sycl_get_queue_ptr(s->sycl_state));
    const size_t stride_px = s->float_stride / sizeof(float);
    const size_t nb = s->dim.num_blocks;
    const size_t plane_bytes = s->dim.alloc_height * stride_px * sizeof(float);
    const size_t indterm_bytes = static_cast<size_t>(SP_ELEMENTS) * nb * sizeof(float);
    const size_t cov_bytes = static_cast<size_t>(SP_ELEMENTS) * SP_ELEMENTS * sizeof(float);
    const size_t score_bytes = nb * sizeof(float);

    allocate_device_buffers(s, q, plane_bytes, indterm_bytes, cov_bytes, score_bytes);
    allocate_host_buffers(s, q, plane_bytes, indterm_bytes, cov_bytes, score_bytes);
    if (!allocation_complete(s)) {
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

} // namespace

namespace
{

static int prepare_temporal_planes(SpeedTemporalSyclState *s, int current, int previous)
{
    const int width = (int)s->dim.original_width;
    const int height = (int)s->dim.original_height;
    subtract_plane(s->h_ref[previous], s->h_ref[current], width, height, s->float_stride);
    if (s->speed_temporal_use_ref_diff) {
        subtract_plane(s->h_dis[previous], s->h_ref[current], width, height, s->float_stride);
    } else {
        subtract_plane(s->h_dis[previous], s->h_dis[current], width, height, s->float_stride);
    }
    const size_t stride = s->float_stride / sizeof(float);
    const size_t count = 2u * s->dim.alloc_height * stride;
    float *scratch = static_cast<float *>(aligned_malloc(count * sizeof(float), 32));
    if (!scratch) {
        return -ENOMEM;
    }
    speed_internal_filter_and_downscale(&s->dim, &s->opt, s->h_ref[previous], scratch,
                                        s->float_stride);
    speed_internal_filter_and_downscale(&s->dim, &s->opt, s->h_dis[previous], scratch,
                                        s->float_stride);
    aligned_free(scratch);
    return 0;
}

} // namespace

namespace
{

static void compute_temporal_score(SpeedTemporalSyclState *s, int previous, float *score)
{
    sycl::queue &queue = *static_cast<sycl::queue *>(vmaf_sycl_get_queue_ptr(s->sycl_state));
    bool singular_ref = false;
    run_channel_st(s, s->h_ref[previous], s->h_indterm_ref, s->d_indterm_ref, s->d_sol_ref,
                   &singular_ref);
    queue.memcpy(s->d_eigenvalues_ref, s->d_eigenvalues, SP_ELEMENTS * sizeof(float));
    queue.wait();
    bool singular_dis = false;
    run_channel_st(s, s->h_dis[previous], s->h_indterm_dis, s->d_indterm_dis, s->d_sol_dis,
                   &singular_dis);
    if (singular_ref != singular_dis) {
        *score = 0.0f;
        return;
    }
    score_aggregate_st(s, score);
}

} // namespace

namespace
{

static int extract_temporal_sycl(VmafFeatureExtractor *fex, VmafPicture *ref_pic,
                                 VmafPicture *ref_pic_90, VmafPicture *dist_pic,
                                 VmafPicture *dist_pic_90, unsigned index,
                                 VmafFeatureCollector *feature_collector)
{
    (void)ref_pic_90;
    (void)dist_pic_90;
    auto *s = static_cast<SpeedTemporalSyclState *>(fex->priv);

    const int cyclic = (int)(index % 2u);
    const int other = (int)((index + 1u) % 2u);

    picture_copy(s->h_ref[cyclic], s->float_stride, ref_pic, -128, ref_pic->bpc, 0);
    picture_copy(s->h_dis[cyclic], s->float_stride, dist_pic, -128, dist_pic->bpc, 0);
    if (index == 0) {
        return vmaf_feature_collector_append_with_dict(
            feature_collector, s->feature_name_dict, "Speed_temporal_feature_speed_temporal_score",
            0.0, index);
    }

    const int err = prepare_temporal_planes(s, cyclic, other);
    if (err) {
        return err;
    }
    float score = 0.0f;
    compute_temporal_score(s, other, &score);

    /* Every clamp here was a less-than comparison, and every comparison
     * against NaN is false, so a non-finite score was published as
     * speed_temporal_max_val -- a finite, plausible 1000.0 standing in for a
     * computation that produced no number, and invisible to the parity
     * harness's own isfinite() assertion. speed_internal_clamp_score() checks
     * finiteness first and fails the frame, matching the CPU reference and
     * the brisque.c / y_funque_plus.c convention. Finite scores clamp exactly
     * as before. */
    double clipped = 0.0;
    const int clamp_err = speed_internal_clamp_score(
        score, s->speed_temporal_max_val, index, "speed_temporal_sycl", "speed_temporal", &clipped);
    if (clamp_err)
        return clamp_err;
    return vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "Speed_temporal_feature_speed_temporal_score",
                                                   clipped, index);
}

} // namespace

namespace
{

static int close_temporal_sycl(VmafFeatureExtractor *fex)
{
    auto *s = static_cast<SpeedTemporalSyclState *>(fex->priv);
    speed_internal_report_singular(&s->singular_tally, "speed_temporal_sycl");
    if (s->sycl_state) {
        free_sycl_state_st(s);
    }
    if (s->feature_name_dict) {
        vmaf_dictionary_free(&s->feature_name_dict);
    }
    return 0;
}

static const char *provided_features_temporal[] = {
    "Speed_temporal_feature_speed_temporal_score",
    nullptr,
};

} // namespace

/* ADR-0567: real SYCL GPU kernels for speed_temporal.
 * TEMPORAL flag guarantees sequential frame submission (ping-pong diff
 * requires frame ordering). */
extern "C" VmafFeatureExtractor vmaf_fex_speed_temporal_sycl = {
    .name = "speed_temporal_sycl",
    .init = init_temporal_sycl,
    .extract = extract_temporal_sycl,
    .close = close_temporal_sycl,
    .options = options_temporal,
    .priv_size = sizeof(SpeedTemporalSyclState),
    .flags = VMAF_FEATURE_EXTRACTOR_TEMPORAL | VMAF_FEATURE_EXTRACTOR_SYCL,
    .provided_features = provided_features_temporal,
};
