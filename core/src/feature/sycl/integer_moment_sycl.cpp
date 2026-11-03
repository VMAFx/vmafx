/* Upstream-mirror filename: defines float_moment symbol despite the integer_ prefix (matches Netflix upstream). See ADR-0549. */
/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  float_moment feature extractor on the SYCL backend
 *  (T7-23 / ADR-0182, GPU long-tail batch 1d part 3). SYCL twin
 *  of moment_vulkan (PR #133) and moment_cuda (this PR's batch
 *  1d part 2).
 *
 *  Algorithm (mirrors core/src/feature/float_moment.c::extract):
 *      for each pixel:
 *          ref1 += ref;        ref2 += ref * ref;
 *          dis1 += dis;        dis2 += dis * dis;
 *      host divides each accumulator by w*h.
 *
 *  Single kernel per frame; four sycl::atomic_ref<int64_t>
 *  reductions into the four-slot device counter. Pattern:
 *  register with vmaf_sycl_graph_register and ride the combined
 *  graph submit/wait machinery (mirrors psnr_sycl).
 *
 *  Bit-exactness contract (ADR-1449): the four sums are the CPU's.
 *  moment.c adds the samples, and their squares, into one double per
 *  output. The first moments' terms are exact, so their integer sum is the
 *  CPU's sum. For the second moments the CPU forms each square in float
 *  (`const float term = pic_ * pic_`) and adds the floats: up to 12 bits a
 *  square has at most 24 significant bits and the float is the integer
 *  square, but at 16 bits it is the square rounded to 24 bits. The kernel
 *  therefore adds the float square (moment_float_square()), an integer
 *  below 2^32, so the int64 sum is exact and equals the CPU's double sum
 *  while that sum is below 2^53 units of 1 / scaler^2, which holds for
 *  every frame of up to 2^21 pixels and for every 8-, 10- and 12-bit
 *  frame.
 *
 *  Past 2^53 units (ADR-1497) the CPU's own running sum rounds as it adds.
 *  On a frame that can get there (vmaf_moment_sum_may_round()) four more
 *  kernels replace the two second-moment sums with the CPU's, with the lane
 *  steps of feature/float_moment_sum.h, as the CUDA and HIP twins do
 *  (feature/float_moment_sum_gpu.h): each row's exact sum, a plan per row,
 *  each planned row's increments composed in pixel order, and one walk per
 *  plane that adds a row from its increments or as its runs, and a run that
 *  crosses a binade term by term. Integers only (no fp64, ADR-0220), and no
 *  scratch memory (ADR-1395). They return at once while a plane's exact sum
 *  is at most 2^53 units.
 */

#include <sycl/sycl.hpp>

#include <cerrno>
#include <cstdint>
#include <cstdlib>

#include "config.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature_name.h"
#include "float_moment_scaler.h"
#include "log.h"
#include "picture.h"
#include "sycl/common.h"
#include "sycl_compat.h"

namespace
{

struct MomentStateSycl {
    /* Frame geometry. */
    unsigned width;
    unsigned height;
    unsigned bpc;

    /* SYCL state back-pointer. */
    VmafSyclState *sycl_state;

    /* Device + host accumulators — 4 slots: ref1, dis1, ref2, dis2. */
    int64_t *d_sums;
    int64_t *h_sums;

    /* Per-row device arrays of the CPU's second-moment sums past 2^53 units
     * (ADR-1497), allocated only for a frame whose sums can get there. */
    uint64_t *d_row_totals;
    int *d_row_plans;
    int64_t *d_row_units;

    /* Submit/collect plumbing. */
    bool has_pending;
    unsigned pending_index;

    VmafDictionary *feature_name_dict;
};

/* The term of moment.c::compute_2nd_moment() for the raw sample `v`, in
 * units of 1 / scaler^2: picture_copy() divides the sample by the scaler (a
 * power of two, exact) and the square is one fp32 product, rounded to nearest
 * even. Scaling by a power of two does not change which bits are rounded
 * away, so the float square of the raw sample has the same significand. Its
 * value is an integer below 2^32. Up to 12 bits it is the integer square. */
VMAF_SYCL_ALWAYS_INLINE int64_t moment_float_square(uint32_t v)
{
    const auto sample = (float)v;
    const float square = sample * sample;
    return (int64_t)square;
}

} /* anonymous namespace */

/* The CPU's second-moment sums past 2^53 units (ADR-1497): integers only,
 * every function inlined (a call is a scratch-memory frame, ADR-1395), and a
 * sample's term is moment_float_square(). */
#define VMAF_ORDSUM_FUNC static VMAF_SYCL_ALWAYS_INLINE
#define VMAF_ORDSUM_NO_FP64
#define VMAF_MOMENT_SQUARE(v) moment_float_square(v)
#include "../float_moment_sum.h"

namespace
{

/* Per-pixel moment kernel. Reads the shared ref/dis frame
 * buffers (uint8 packed at ≤8bpc, uint16 packed at >8bpc,
 * tightly packed at `width * bytes_per_pixel`). Atomic-adds
 * each pixel's four contributions to the device accumulators. */
static void launch_moment(sycl::queue &q, void *shared_ref, void *shared_dis, int64_t *d_sums,
                          unsigned width, unsigned height, unsigned bpc)
{
    sycl::range<2> const global{(size_t)height, (size_t)width};
    const unsigned e_w = width;
    const unsigned e_bpc = bpc;
    void const *ref_in = shared_ref;
    void const *dis_in = shared_dis;
    int64_t *const e_sums = d_sums;

    q.submit([=](sycl::handler &h) {
        h.parallel_for(global, [=](sycl::id<2> id) {
            const size_t y = id[0];
            const size_t x = id[1];
            const size_t off = y * (size_t)e_w + x;
            uint32_t r;
            uint32_t d;
            if (e_bpc <= 8) {
                r = static_cast<const uint8_t *>(ref_in)[off];
                d = static_cast<const uint8_t *>(dis_in)[off];
            } else {
                r = static_cast<const uint16_t *>(ref_in)[off];
                d = static_cast<const uint16_t *>(dis_in)[off];
            }
            using atomic64 =
                sycl::atomic_ref<int64_t, sycl::memory_order::relaxed, sycl::memory_scope::device,
                                 sycl::access::address_space::global_space>;
            atomic64(e_sums[0]).fetch_add((int64_t)r);
            atomic64(e_sums[1]).fetch_add((int64_t)d);
            atomic64(e_sums[2]).fetch_add(moment_float_square(r));
            atomic64(e_sums[3]).fetch_add(moment_float_square(d));
        });
    });
}

/* The plane's exact sum is at most 2^53 units: it is the CPU's sum. */
VMAF_SYCL_ALWAYS_INLINE bool moment_sum_is_exact(const VmafMomentSumArgs &a, unsigned plane)
{
    return a.sums[2u + plane] <= VMAF_MOMENT_SUM_EXACT_END;
}

/* Row `row` of plane `plane`; the planes are picked by value, not by index,
 * so the argument block stays out of private memory. */
VMAF_SYCL_ALWAYS_INLINE const uint16_t *moment_sum_row(const VmafMomentSumArgs &a, unsigned plane,
                                                       unsigned row)
{
    const uint8_t *luma = plane == 0u ? a.luma[0] : a.luma[1];
    const size_t stride = plane == 0u ? a.stride[0] : a.stride[1];
    return vmaf_moment_sum_line(luma, stride, row);
}

template <typename T> VMAF_SYCL_ALWAYS_INLINE T *moment_local(const sycl::local_accessor<T, 1> &acc)
{
    return acc.template get_multi_ptr<sycl::access::decorated::no>().get();
}

/* Each row's exact sum: group (plane, row). */
static void launch_row_totals(sycl::queue &q, const VmafMomentSumArgs &a)
{
    const size_t lanes = VMAF_MOMENT_SUM_LANES;
    const sycl::nd_range<2> range{{VMAF_MOMENT_SUM_PLANES, (size_t)a.height * lanes}, {1, lanes}};
    q.submit([&](sycl::handler &h) {
        h.parallel_for(range, [=](sycl::nd_item<2> item) {
            const auto plane = (unsigned)item.get_group(0);
            const auto row = (unsigned)item.get_group(1);
            const auto lane = (unsigned)item.get_local_id(1);
            if (moment_sum_is_exact(a, plane))
                return;
            const uint64_t mine =
                vmaf_moment_sum_lane_total(moment_sum_row(a, plane, row), a.width, lane);
            const uint64_t total = sycl::reduce_over_group(item.get_group(), mine, sycl::plus<>());
            if (lane == 0u)
                a.row_totals[(size_t)plane * a.height + row] = total;
        });
    });
}

/* The plan of every row of one plane, group (plane): the lanes stage a batch
 * of row sums, lane 0 follows their prefix, the lanes store the plans. */
static void launch_row_plans(sycl::queue &q, const VmafMomentSumArgs &a)
{
    const size_t batch = VMAF_MOMENT_SUM_BATCH;
    q.submit([&](sycl::handler &h) {
        const sycl::local_accessor<uint64_t, 1> staged_totals(sycl::range<1>(batch), h);
        const sycl::local_accessor<int, 1> staged_plans(sycl::range<1>(batch), h);
        h.parallel_for(
            sycl::nd_range<1>{VMAF_MOMENT_SUM_PLANES * batch, batch}, [=](sycl::nd_item<1> item) {
                const auto plane = (unsigned)item.get_group(0);
                const auto lane = (unsigned)item.get_local_id(0);
                if (moment_sum_is_exact(a, plane))
                    return;
                uint64_t *totals = moment_local(staged_totals);
                int *plans = moment_local(staged_plans);
                const size_t base = (size_t)plane * a.height;
                uint64_t prefix = 0u;
                for (unsigned first = 0; first < a.height; first += VMAF_MOMENT_SUM_BATCH) {
                    const unsigned left = a.height - first;
                    const unsigned count =
                        left < VMAF_MOMENT_SUM_BATCH ? left : VMAF_MOMENT_SUM_BATCH;
                    if (lane < count)
                        totals[lane] = a.row_totals[base + first + lane];
                    sycl::group_barrier(item.get_group());
                    if (lane == 0u)
                        vmaf_moment_sum_plan_batch(&prefix, totals, plans, count);
                    sycl::group_barrier(item.get_group());
                    if (lane < count)
                        a.plans[base + first + lane] = plans[lane];
                    sycl::group_barrier(item.get_group());
                }
            });
    });
}

/* One work-item of launch_row_units(): group (plane, row). A row without a
 * binade plan gets zeros, which the walk never uses. */
VMAF_SYCL_ALWAYS_INLINE void moment_row_units_item(const VmafMomentSumArgs &a, int64_t *units,
                                                   sycl::nd_item<2> item)
{
    const auto plane = (unsigned)item.get_group(0);
    const auto row = (unsigned)item.get_group(1);
    const auto lane = (unsigned)item.get_local_id(1);
    if (moment_sum_is_exact(a, plane))
        return;
    const size_t at = (size_t)plane * a.height + row;
    const int plan = a.plans[at];
    if (!vmaf_moment_sum_plan_is_binade(plan)) {
        if (lane == 0u) {
            a.row_units[(size_t)2u * at] = 0;
            a.row_units[((size_t)2u * at) + 1u] = 0;
        }
        return;
    }
    vmaf_moment_sum_lane_units(moment_sum_row(a, plane, row), a.width, plan, lane, units);
    sycl::group_barrier(item.get_group());
    for (unsigned step = 1u; step < VMAF_MOMENT_SUM_LANES; step <<= 1u) {
        vmaf_moment_sum_tree_step(units, lane, step);
        sycl::group_barrier(item.get_group());
    }
    if (lane == 0u) {
        a.row_units[(size_t)2u * at] = units[0];
        a.row_units[((size_t)2u * at) + 1u] = units[1];
    }
}

/* Each planned row's increments under its plan: a lane composes its run,
 * then the lanes are composed in order. */
static void launch_row_units(sycl::queue &q, const VmafMomentSumArgs &a)
{
    const size_t lanes = VMAF_MOMENT_SUM_LANES;
    const sycl::nd_range<2> range{{VMAF_MOMENT_SUM_PLANES, (size_t)a.height * lanes}, {1, lanes}};
    q.submit([&](sycl::handler &h) {
        const sycl::local_accessor<int64_t, 1> lds(sycl::range<1>(2u * lanes), h);
        h.parallel_for(range, [=](sycl::nd_item<2> item) {
            moment_row_units_item(a, moment_local(lds), item);
        });
    });
}

/* Local memory of the walk: the staged batch, the runs of a row, and what
 * lane 0 tells the group (command, operand) and the sum a row starts at. */
struct MomentWalkLocal {
    sycl::local_accessor<int, 1> plans;
    sycl::local_accessor<uint64_t, 1> totals;
    sycl::local_accessor<int64_t, 1> units;
    sycl::local_accessor<uint64_t, 1> run_totals;
    sycl::local_accessor<int64_t, 1> run_low;
    sycl::local_accessor<int64_t, 1> run_high;
    sycl::local_accessor<unsigned, 1> control;
    sycl::local_accessor<uint64_t, 1> walked;
};

/* Every lane's step of one round of the walk. */
VMAF_SYCL_ALWAYS_INLINE void moment_walk_lanes(const VmafMomentSumArgs &a, const MomentWalkLocal &l,
                                               unsigned plane, unsigned lane, unsigned todo,
                                               unsigned what)
{
    const size_t base = (size_t)plane * a.height;
    if (todo == VMAF_MOMENT_WALK_LOAD) {
        vmaf_moment_sum_walk_stage(a.plans + base, a.row_totals + base, a.row_units + (2u * base),
                                   a.height, what * VMAF_MOMENT_SUM_BATCH, lane,
                                   moment_local(l.plans), moment_local(l.totals),
                                   moment_local(l.units));
        return;
    }
    vmaf_moment_sum_walk_run(moment_sum_row(a, plane, what), a.width, l.walked[0], lane,
                             moment_local(l.run_totals), moment_local(l.run_low),
                             moment_local(l.run_high));
}

/* Lane 0's step of one round of the walk: the runs of a row, then the staged
 * rows until one needs its runs; it tells the group what comes next. */
VMAF_SYCL_ALWAYS_INLINE void moment_walk_lane0(const VmafMomentSumArgs &a, const MomentWalkLocal &l,
                                               unsigned plane, unsigned todo, unsigned what,
                                               uint64_t *sum, unsigned *row, unsigned *first)
{
    if (todo == VMAF_MOMENT_WALK_RUNS) {
        *sum = vmaf_moment_sum_walk_row_runs(moment_sum_row(a, plane, what), a.width, *sum,
                                             moment_local(l.run_totals), moment_local(l.run_low),
                                             moment_local(l.run_high));
        *row = what + 1u;
    } else {
        *first = what * VMAF_MOMENT_SUM_BATCH;
    }
    unsigned next = 0u;
    l.control[0] = vmaf_moment_sum_walk_next(a.height, *first, sum, row, moment_local(l.plans),
                                             moment_local(l.totals), moment_local(l.units), &next);
    l.control[1] = next;
    l.walked[0] = *sum;
}

/* The CPU's second-moment sum of one plane, group (plane). Lane 0 walks the
 * rows, a staged batch at a time; when a row must be added as runs, every
 * lane computes one into local memory and lane 0 adds them in order. Every
 * round stages a batch or consumes a row, which bounds the loop. */
static void launch_ordered_totals(sycl::queue &q, const VmafMomentSumArgs &a)
{
    const size_t lanes = VMAF_MOMENT_SUM_LANES;
    const size_t batch = VMAF_MOMENT_SUM_BATCH;
    q.submit([&](sycl::handler &h) {
        const MomentWalkLocal l{
            .plans = sycl::local_accessor<int, 1>(sycl::range<1>(batch), h),
            .totals = sycl::local_accessor<uint64_t, 1>(sycl::range<1>(batch), h),
            .units = sycl::local_accessor<int64_t, 1>(sycl::range<1>(2u * batch), h),
            .run_totals = sycl::local_accessor<uint64_t, 1>(sycl::range<1>(lanes), h),
            .run_low = sycl::local_accessor<int64_t, 1>(sycl::range<1>(2u * lanes), h),
            .run_high = sycl::local_accessor<int64_t, 1>(sycl::range<1>(2u * lanes), h),
            .control = sycl::local_accessor<unsigned, 1>(sycl::range<1>(2u), h),
            .walked = sycl::local_accessor<uint64_t, 1>(sycl::range<1>(1u), h),
        };
        h.parallel_for(sycl::nd_range<1>{VMAF_MOMENT_SUM_PLANES * lanes, lanes},
                       [=](sycl::nd_item<1> item) {
                           const auto plane = (unsigned)item.get_group(0);
                           const auto lane = (unsigned)item.get_local_id(0);
                           if (moment_sum_is_exact(a, plane))
                               return;
                           if (lane == 0u) {
                               l.control[0] = VMAF_MOMENT_WALK_LOAD;
                               l.control[1] = 0u;
                               l.walked[0] = 0u;
                           }
                           uint64_t sum = 0u;
                           unsigned row = 0u;
                           unsigned first = 0u;
                           const unsigned rounds = a.height + a.height / VMAF_MOMENT_SUM_BATCH + 2u;
                           for (unsigned round = 0; round < rounds; round++) {
                               sycl::group_barrier(item.get_group());
                               const unsigned todo = l.control[0];
                               const unsigned what = l.control[1];
                               if (todo == VMAF_MOMENT_WALK_DONE)
                                   break;
                               moment_walk_lanes(a, l, plane, lane, todo, what);
                               sycl::group_barrier(item.get_group());
                               if (lane == 0u)
                                   moment_walk_lane0(a, l, plane, todo, what, &sum, &row, &first);
                           }
                           if (lane == 0u)
                               a.sums[2u + plane] = sum;
                       });
    });
}

/* The four kernels that replace the second-moment sums with the CPU's past
 * 2^53 units, after the frame kernel on the in-order queue. */
static void launch_moment_sum(sycl::queue &q, const MomentStateSycl *s, void *shared_ref,
                              void *shared_dis)
{
    const size_t row_bytes = (size_t)s->width * sizeof(uint16_t);
    const VmafMomentSumArgs a = {
        .luma = {static_cast<const uint8_t *>(shared_ref),
                 static_cast<const uint8_t *>(shared_dis)},
        .stride = {row_bytes, row_bytes},
        .width = s->width,
        .height = s->height,
        .row_totals = s->d_row_totals,
        .plans = s->d_row_plans,
        .row_units = s->d_row_units,
        .sums = reinterpret_cast<uint64_t *>(s->d_sums),
    };
    launch_row_totals(q, a);
    launch_row_plans(q, a);
    launch_row_units(q, a);
    launch_ordered_totals(q, a);
}

/* Pre-graph: zero all four accumulators. */
static void moment_pre_graph(void *queue_ptr, void *priv)
{
    sycl::queue &q = *static_cast<sycl::queue *>(queue_ptr);
    auto *s = static_cast<MomentStateSycl *>(priv);
    q.memset(s->d_sums, 0, 4u * sizeof(int64_t));
}

static void enqueue_moment_work(void *queue_ptr, void *priv, void *shared_ref, void *shared_dis)
{
    sycl::queue &q = *static_cast<sycl::queue *>(queue_ptr);
    auto *s = static_cast<MomentStateSycl *>(priv);
    launch_moment(q, shared_ref, shared_dis, s->d_sums, s->width, s->height, s->bpc);
    if (s->d_row_totals)
        launch_moment_sum(q, s, shared_ref, shared_dis);
}

static void moment_post_graph(void *queue_ptr, void *priv)
{
    sycl::queue &q = *static_cast<sycl::queue *>(queue_ptr);
    auto *s = static_cast<MomentStateSycl *>(priv);
    q.memcpy(s->h_sums, s->d_sums, 4u * sizeof(int64_t));
}

static void config_moment_slot(void *priv, int slot)
{
    (void)priv;
    (void)slot;
}

} /* anonymous namespace */

extern "C" {

// NOLINTBEGIN(misc-use-anonymous-namespace, misc-use-internal-linkage) — ADR-0141 §2 load-bearing invariant: the
// `init_fex_sycl` / `submit_fex_sycl` / `collect_fex_sycl` / `close_fex_sycl`
// entry points and the `provided_features_*` table use C-style `static` rather
// than an anonymous namespace because their addresses are stored in the
// `extern "C" VmafFeatureExtractor` struct at the bottom of this file, which
// the C ABI consumes through the function-pointer types in
// `feature_extractor.h`. A namespace cannot appear inside this linkage
// specification at all. Same band, same reason, as integer_motion_sycl.cpp and
// integer_adm_sycl.cpp. Per CLAUDE.md §12 r12 these are load-bearing
// invariants of the SYCL <-> libvmaf C-API ABI.
static const VmafOption options_moment_sycl[] = {{.name = nullptr}};

static int close_fex_sycl(VmafFeatureExtractor *fex);

/* The per-row device arrays of the CPU's second-moment sums for a frame
 * whose sums can pass 2^53 units; none otherwise. False when an allocation
 * failed. */
static bool moment_sum_alloc(MomentStateSycl *s, VmafSyclState *state)
{
    if (!vmaf_moment_sum_may_round(s->width, s->height, s->bpc))
        return true;
    const size_t rows = (size_t)VMAF_MOMENT_SUM_PLANES * s->height;
    s->d_row_totals =
        static_cast<uint64_t *>(vmaf_sycl_malloc_device(state, rows * sizeof(uint64_t)));
    s->d_row_plans = static_cast<int *>(vmaf_sycl_malloc_device(state, rows * sizeof(int)));
    s->d_row_units =
        static_cast<int64_t *>(vmaf_sycl_malloc_device(state, rows * 2u * sizeof(int64_t)));
    return s->d_row_totals && s->d_row_plans && s->d_row_units;
}

static int init_fex_sycl(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                         unsigned w, unsigned h)
{
    (void)pix_fmt;
    auto *s = static_cast<MomentStateSycl *>(fex->priv);

    s->width = w;
    s->height = h;
    s->bpc = bpc;

    if (!fex->sycl_state) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "float_moment_sycl: no SYCL state\n");
        return -EINVAL;
    }

    VmafSyclState *state = fex->sycl_state;
    s->sycl_state = state;

    int const err = vmaf_sycl_shared_frame_init(state, w, h, bpc);
    if (err)
        return err;

    s->d_sums = static_cast<int64_t *>(vmaf_sycl_malloc_device(state, 4u * sizeof(int64_t)));
    s->h_sums = static_cast<int64_t *>(vmaf_sycl_malloc_host(state, 4u * sizeof(int64_t)));
    const bool rows_ok = moment_sum_alloc(s, state);
    if (!s->d_sums || !s->h_sums || !rows_ok) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "float_moment_sycl: device memory allocation failed\n");
        (void)close_fex_sycl(fex);
        return -ENOMEM;
    }

    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (!s->feature_name_dict) {
        (void)close_fex_sycl(fex);
        return -ENOMEM;
    }

    s->has_pending = false;

    int const err2 = vmaf_sycl_graph_register(state, enqueue_moment_work, moment_pre_graph,
                                              moment_post_graph, config_moment_slot, s, "MOMENT");
    if (err2) {
        (void)close_fex_sycl(fex);
        return err2;
    }

    return 0;
}

static int submit_fex_sycl(VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                           VmafPicture *dist_pic, VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic;
    (void)ref_pic_90;
    (void)dist_pic;
    (void)dist_pic_90;

    auto *s = static_cast<MomentStateSycl *>(fex->priv);
    VmafSyclState *state = fex->sycl_state;

    int const err = vmaf_sycl_graph_submit(state);
    if (err)
        return err;

    s->pending_index = index;
    s->has_pending = true;
    return 0;
}

static int collect_fex_sycl(VmafFeatureExtractor *fex, unsigned index,
                            VmafFeatureCollector *feature_collector)
{
    auto *s = static_cast<MomentStateSycl *>(fex->priv);
    VmafSyclState *state = fex->sycl_state;

    /* A failed wait leaves the sums stale: fail, never score them. */
    int const wait_err = vmaf_sycl_graph_wait(state);
    if (wait_err)
        return wait_err;

    /* ADR-1212: normalise by the bit-depth scaler exactly as the CPU reference
     * does. `float_moment` runs `picture_copy()` first, which divides every
     * sample by 2^(bpc - 8) at every depth above 8 before `moment.c`
     * accumulates it — see core/src/feature/picture_copy.cpp. This kernel
     * accumulates the RAW codeword, so without this step a 10-bit input
     * reported ref1st/dis1st 4x and ref2nd/dis2nd 16x too large. The device
     * sums are exact integers, so dividing here reproduces the CPU's
     * sum(x / scaler) bit for bit: every term is an exact multiple of
     * 1 / scaler (first moments) or 1 / scaler^2 (the float squares the
     * kernel adds, ADR-1449), and the CPU's running double sum is exact while
     * it is at most 2^53 of those units. A term is below 2^32 units, so that
     * covers every frame of up to 2^21 pixels and every 8-, 10- and 12-bit
     * frame. At every depth vmaf_moment_sum_may_round() bounds the sum by
     * pixels * 2^(2 * bpc). When the sum of squares passes 2^53 units
     * the CPU rounds each further add, and launch_moment_sum() has replaced
     * the two second-moment sums with the CPU's rounded ones (ADR-1497), each
     * a double the conversion below holds exactly. */
    const double moment_scaler = vmaf_float_moment_scaler(s->bpc);
    const double moment_scaler_sq = moment_scaler * moment_scaler;
    const double n_pixels = (double)s->width * (double)s->height;
    const double ref1 = ((double)s->h_sums[0] / moment_scaler) / n_pixels;
    const double dis1 = ((double)s->h_sums[1] / moment_scaler) / n_pixels;
    const double ref2 = ((double)s->h_sums[2] / moment_scaler_sq) / n_pixels;
    const double dis2 = ((double)s->h_sums[3] / moment_scaler_sq) / n_pixels;

    int err = vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                      "float_moment_ref1st", ref1, index);
    if (!err) {
        err = vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                      "float_moment_dis1st", dis1, index);
    }
    if (!err) {
        err = vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                      "float_moment_ref2nd", ref2, index);
    }
    if (!err) {
        err = vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                      "float_moment_dis2nd", dis2, index);
    }
    return err;
}

static int close_fex_sycl(VmafFeatureExtractor *fex)
{
    auto *s = static_cast<MomentStateSycl *>(fex->priv);
    if (s->sycl_state) {
        /* Unregister from the combined command graph before freeing priv.
         * Mirrors the fix in integer_motion_sycl.cpp (ADR-0989):
         * vmaf_sycl_graph_unregister() drains combined_queue and removes
         * this extractor's entry so a subsequent VmafContext sharing the
         * same sycl_state does not inherit a dangling priv pointer. */
        (void)vmaf_sycl_graph_unregister(s->sycl_state, s);

        if (s->d_sums)
            vmaf_sycl_free(s->sycl_state, s->d_sums);
        if (s->h_sums)
            vmaf_sycl_free(s->sycl_state, s->h_sums);
        if (s->d_row_totals)
            vmaf_sycl_free(s->sycl_state, s->d_row_totals);
        if (s->d_row_plans)
            vmaf_sycl_free(s->sycl_state, s->d_row_plans);
        if (s->d_row_units)
            vmaf_sycl_free(s->sycl_state, s->d_row_units);
    }
    if (s->feature_name_dict)
        vmaf_dictionary_free(&s->feature_name_dict);
    return 0;
}

static const char *provided_features_moment_sycl[] = {
    "float_moment_ref1st",
    "float_moment_dis1st",
    "float_moment_ref2nd",
    "float_moment_dis2nd",
    nullptr,
};

/* Runs on the zero-copy path (ADR-1688): the moments are those of the shared frame's luma;
 * submit() never reads its picture arguments. */
static bool reads_shared_luma_only(const VmafFeatureExtractor * /*fex*/)
{
    return true;
}

// NOLINTEND(misc-use-anonymous-namespace, misc-use-internal-linkage)

extern "C" VmafFeatureExtractor vmaf_fex_float_moment_sycl = {
    .name = "float_moment_sycl",
    .init = init_fex_sycl,
    .extract = nullptr,
    .flush = nullptr,
    .close = close_fex_sycl,
    .submit = submit_fex_sycl,
    .collect = collect_fex_sycl,
    .options = options_moment_sycl,
    .priv_size = sizeof(MomentStateSycl),
    .flags = VMAF_FEATURE_EXTRACTOR_SYCL,
    .provided_features = provided_features_moment_sycl,
    .reads_shared_luma_only = reads_shared_luma_only,
    .chars =
        {
            .n_dispatches_per_frame = 1,
            .is_reduction_only = true,
            .min_useful_frame_area = 1920U * 1080U,
            .dispatch_hint = VMAF_FEATURE_DISPATCH_AUTO,
        },
};

} /* extern "C" */
