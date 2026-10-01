/* Upstream-mirror filename: defines float_psnr symbol despite the integer_ prefix (matches Netflix upstream). See ADR-0549. */
/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  PSNR feature extractor on the SYCL backend (T7-23 / ADR-0182,
 *  GPU long-tail batch 1b part 2; chroma extension T3-15(b) second
 *  port, 2026-05-09 — see research digest
 *  `docs/research/0090-t3-15-gpu-coverage-long-tail-2026-05-09.md`,
 *  Vulkan precedent in [ADR-0216](../../docs/adr/0216-vulkan-chroma-psnr.md),
 *  CUDA twin in PR #520 / commit 7f3d58a5).
 *
 *  Algorithm (mirrors core/src/feature/integer_psnr.c::sse_line_{8,16}):
 *      diff = (int64)ref - (int64)dis;     (per pixel)
 *      sse  += diff * diff;                (int64 reduction)
 *  Each work-item sums PSNR_PIXELS_PER_ITEM pixels, the work-group
 *  reduces them and adds its total to the plane's accumulator with one
 *  atomic. Integer sums, so the result is the per-pixel-atomic one.
 *
 *  One SSE reduction per active plane (Y, Cb, Cr) per frame; the
 *  same plane-agnostic kernel is invoked three times against per-
 *  plane (w, h) and per-plane device buffers. Chroma planes are
 *  sized per the active subsampling (4:2:0 → w/2 × h/2,
 *  4:2:2 → w/2 × h, 4:4:4 → w × h). YUV400 clamps `n_planes = 1`.
 *
 *  Every plane comes from the SYCL state: luma from the shared frame,
 *  Cb / Cr from the opt-in shared chroma planes (ADR-1369), uploaded
 *  once per frame for every twin that reads them. Direct enqueue on
 *  the combined queue preserves in-order ordering with the
 *  graph-replayed luma kernel.
 *
 *  Phases (combined-graph contract — see `vmaf_sycl_graph_register`
 *  docs in `core/src/sycl/common.h`):
 *      submit   : upload the shared chroma planes (once per frame).
 *      pre_fn   : zero all 3 SSE accumulators.
 *      enqueue  : luma SSE reduction kernel (graph-recordable).
 *      post_fn  : chroma SSE reduction kernels (direct) + D2H all 3
 *                 SSE accumulators.
 *
 *  Pattern: register with vmaf_sycl_graph_register and ride the
 *  combined-graph submit/wait machinery just like motion_sycl.
 *
 *  Options: the full CPU `psnr` table. The device only reduces SSE;
 *  `enable_mse`, `enable_apsnr`, `reduced_hbd_peak`, `min_sse` and
 *  `uncapped` act on the host through the psnr_score.h helpers the
 *  CPU extractor uses, so every option is bit-exact with the CPU.
 */

#include <sycl/sycl.hpp>

#include <cerrno>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <cstring>

#include "config.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature_name.h"
#include "log.h"
#include "picture.h"
#include "psnr_score.h"
#include "sycl/common.h"

namespace
{

constexpr unsigned PSNR_NUM_PLANES = 3U;

} // namespace

namespace
{

struct PsnrStateSycl {
    /* Per-plane frame geometry. Plane 0 = luma. */
    unsigned width[PSNR_NUM_PLANES];
    unsigned height[PSNR_NUM_PLANES];
    unsigned bpc;
    /* `vmaf_psnr_peak()` of bpc and `reduced_hbd_peak`. */
    uint32_t peak;
    /* Per-plane `vmaf_psnr_max()`: `(6 * bpc) + 12`, or the `min_sse`
     * ceiling derived from that plane's sample count. */
    double psnr_max[PSNR_NUM_PLANES];
    /* `enable_chroma` option: when false, only luma is dispatched.
     * Default true mirrors CPU integer_psnr.c — see ADR-0453. */
    bool enable_chroma;
    /* CPU integer_psnr.c options, applied on the host to the per-plane
     * SSE the device reduces (psnr_score.h, ADR-1365; `uncapped`:
     * ADR-1193). `enable_mse` adds `mse_{y,cb,cr}`; `enable_apsnr` sums
     * SSE and sample count across frames for the flush aggregates. */
    bool enable_mse;
    bool enable_apsnr;
    bool reduced_hbd_peak;
    bool uncapped;
    double min_sse;
    uint64_t apsnr_sse[PSNR_NUM_PLANES];
    uint64_t apsnr_n_pixels[PSNR_NUM_PLANES];
    /* Number of active planes (1 for YUV400, 3 otherwise). */
    unsigned n_planes;

    /* SYCL state back-pointer. */
    VmafSyclState *sycl_state;

    /* Per-plane device + host SSE accumulators. Plane 0 is the luma
     * accumulator written by the graph-recorded enqueue kernel;
     * planes 1/2 are written by chroma kernels in post_fn. */
    int64_t *d_sse[PSNR_NUM_PLANES];
    int64_t *h_sse[PSNR_NUM_PLANES];

    /* Submit/collect plumbing. */
    bool has_pending;
    unsigned pending_index;

    VmafDictionary *feature_name_dict;
};

} // namespace

namespace
{

struct PsnrKernelArgs {
    const void *ref;
    const void *dis;
    int64_t *sse;
    unsigned width;
    unsigned height;
    unsigned bpc;
};

} // namespace

namespace
{

/* Pixels per work-item and work-items per group of the SSE reduction. */
constexpr size_t PSNR_PIXELS_PER_ITEM = 16U;
constexpr size_t PSNR_WG = 256U;

/* Squared error of one pixel, as integer_psnr.c::sse_line_{8,16}.
 * |diff| <= 65535, so the square fits 32 unsigned bits at every depth. */
static inline uint32_t psnr_pixel_se(const PsnrKernelArgs &args, size_t off)
{
    int32_t diff;
    if (args.bpc <= 8) {
        diff = (int32_t)static_cast<const uint8_t *>(args.ref)[off] -
               (int32_t)static_cast<const uint8_t *>(args.dis)[off];
    } else {
        diff = (int32_t)static_cast<const uint16_t *>(args.ref)[off] -
               (int32_t)static_cast<const uint16_t *>(args.dis)[off];
    }
    const auto magnitude = (uint32_t)(diff < 0 ? -diff : diff);
    return magnitude * magnitude;
}

/* One work-item's PSNR_PIXELS_PER_ITEM pixels, one grid apart (coalesced
 * loads). `Sum` is 32 bits while 16 squares of (2^bpc - 1) fit (bpc <= 12,
 * < 2^28), 64 bits above: exact either way. */
template <typename Sum>
static inline uint64_t psnr_item_sse(const PsnrKernelArgs &args, size_t first, size_t stride,
                                     size_t pixels)
{
    Sum se = 0;
    for (size_t k = 0; k < PSNR_PIXELS_PER_ITEM; k++) {
        const size_t off = first + (k * stride);
        if (off < pixels) {
            se += psnr_pixel_se(args, off);
        }
    }
    return (uint64_t)se;
}

} // namespace

namespace
{

/* SSE of one plane, tightly packed at `width * bytes_per_pixel`. The
 * work-group reduces its items' sums and adds the total to the accumulator
 * with one atomic. Integer sums: the order cannot change the result, which
 * equals the per-pixel atomic the kernel used before (ADR-1369). */
static void launch_sse(sycl::queue &q, PsnrKernelArgs args)
{
    const size_t pixels = (size_t)args.width * args.height;
    const size_t items = (pixels + PSNR_PIXELS_PER_ITEM - 1U) / PSNR_PIXELS_PER_ITEM;
    const size_t global = (items + PSNR_WG - 1U) / PSNR_WG * PSNR_WG;
    const sycl::nd_range<1> ndr{sycl::range<1>{global}, sycl::range<1>{PSNR_WG}};

    q.submit([=](sycl::handler &h) {
        h.parallel_for(ndr, [=](sycl::nd_item<1> item) {
            const size_t first = item.get_global_id(0);
            const uint64_t se = (args.bpc <= 12U) ?
                                    psnr_item_sse<uint32_t>(args, first, global, pixels) :
                                    psnr_item_sse<uint64_t>(args, first, global, pixels);
            const uint64_t group_se = sycl::reduce_over_group(item.get_group(), se, sycl::plus<>());
            if (item.get_local_id(0) == 0U) {
                sycl::atomic_ref<int64_t, sycl::memory_order::relaxed, sycl::memory_scope::device,
                                 sycl::access::address_space::global_space> const accum(*args.sse);
                accum.fetch_add((int64_t)group_se);
            }
        });
    });
}

} // namespace

namespace
{

/* Pre-graph: zero the SSE accumulators (direct enqueue, outside graph). */
static void psnr_pre_graph(void *queue_ptr, void *priv)
{
    sycl::queue &q = *static_cast<sycl::queue *>(queue_ptr);
    auto *s = static_cast<PsnrStateSycl *>(priv);
    for (unsigned p = 0; p < s->n_planes; p++) {
        q.memset(s->d_sse[p], 0, sizeof(int64_t));
    }
}

} // namespace

namespace
{

template <typename State>
static void launch_psnr_luma(sycl::queue &queue, State *state, void *reference, void *distorted)
{
    launch_sse(queue, {.ref = reference,
                       .dis = distorted,
                       .sse = state->d_sse[0],
                       .width = state->width[0],
                       .height = state->height[0],
                       .bpc = state->bpc});
}

/* Graph-recorded: the luma SSE reduction kernel. Chroma stays out of
 * the graph: the recorded graph captures only the luma slot pointers
 * the combined graph hands its extractors, and post_fn reads the
 * compute slot's chroma planes when it runs. */
static void enqueue_psnr_work(void *queue_ptr, void *priv, void *shared_ref, void *shared_dis)
{
    sycl::queue &q = *static_cast<sycl::queue *>(queue_ptr);
    auto *s = static_cast<PsnrStateSycl *>(priv);
    launch_psnr_luma(q, s, shared_ref, shared_dis);
}

} // namespace

namespace
{

/* Post-graph: chroma SSE kernels (direct, post-graph) + D2H copy of
 * all SSE accumulators. graph_submit put the combined queue behind the
 * frame's last upload, chroma included (the chroma went up in submit,
 * before graph_submit), and the queue is in-order, so the D2H sees the
 * luma kernel from the graph + chroma kernels above. */
static void psnr_post_graph(void *queue_ptr, void *priv)
{
    sycl::queue &q = *static_cast<sycl::queue *>(queue_ptr);
    auto *s = static_cast<PsnrStateSycl *>(priv);
    for (unsigned p = 1; p < s->n_planes; p++) {
        launch_sse(q, {.ref = vmaf_sycl_get_shared_plane(s->sycl_state, 1, p),
                       .dis = vmaf_sycl_get_shared_plane(s->sycl_state, 0, p),
                       .sse = s->d_sse[p],
                       .width = s->width[p],
                       .height = s->height[p],
                       .bpc = s->bpc});
    }
    for (unsigned p = 0; p < s->n_planes; p++) {
        q.memcpy(s->h_sse[p], s->d_sse[p], sizeof(int64_t));
    }
}

} // namespace

namespace
{

/* No per-slot config — psnr is stateless across frames. */
static void config_psnr_slot(void *priv, int slot)
{
    (void)priv;
    (void)slot;
}

} // namespace

namespace
{

/* The CPU integer_psnr.c table: same names, defaults and range. */
static const VmafOption options_psnr_sycl[] = {
    {
        .name = "enable_chroma",
        .help = "enable calculation for chroma channels",
        .offset = offsetof(PsnrStateSycl, enable_chroma),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val = {.b = true},
    },
    {
        .name = "enable_mse",
        .help = "enable MSE calculation",
        .offset = offsetof(PsnrStateSycl, enable_mse),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val = {.b = false},
    },
    {
        .name = "enable_apsnr",
        .help = "enable APSNR calculation",
        .offset = offsetof(PsnrStateSycl, enable_apsnr),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val = {.b = false},
    },
    {
        .name = "reduced_hbd_peak",
        .help = "reduce hbd peak value to align with scaled 8-bit content",
        .offset = offsetof(PsnrStateSycl, reduced_hbd_peak),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val = {.b = false},
    },
    {
        .name = "min_sse",
        .help = "constrain the minimum possible sse",
        .offset = offsetof(PsnrStateSycl, min_sse),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val = {.d = 0.0},
        .min = 0.0,
        .max = DBL_MAX,
    },
    {
        .name = "uncapped",
        .help = "report the true PSNR instead of truncating at the psnr_max ceiling "
                "(an all-zero SSE still reports psnr_max)",
        .offset = offsetof(PsnrStateSycl, uncapped),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val = {.b = false},
    },
    {.name = nullptr}};

} // namespace

namespace
{

static void configure_geometry(PsnrStateSycl *s, enum VmafPixelFormat pix_fmt, unsigned w,
                               unsigned h)
{
    s->width[0] = w;
    s->height[0] = h;
    if (pix_fmt == VMAF_PIX_FMT_YUV400P) {
        s->n_planes = 1U;
        s->width[1] = s->width[2] = 0U;
        s->height[1] = s->height[2] = 0U;
    } else {
        s->n_planes = PSNR_NUM_PLANES;
        const int ss_hor = (pix_fmt != VMAF_PIX_FMT_YUV444P);
        const int ss_ver = (pix_fmt == VMAF_PIX_FMT_YUV420P);
        const unsigned cw = ss_hor ? ((w + 1U) >> 1) : w;
        const unsigned ch = ss_ver ? ((h + 1U) >> 1) : h;
        s->width[1] = s->width[2] = cw;
        s->height[1] = s->height[2] = ch;
    }
    if (!s->enable_chroma && s->n_planes > 1U) {
        s->n_planes = 1U;
        s->width[1] = s->width[2] = 0U;
        s->height[1] = s->height[2] = 0U;
    }
}

} // namespace

namespace
{

static int allocate_sse(PsnrStateSycl *s)
{
    for (unsigned p = 0; p < s->n_planes; p++) {
        s->d_sse[p] =
            static_cast<int64_t *>(vmaf_sycl_malloc_device(s->sycl_state, sizeof(int64_t)));
        s->h_sse[p] = static_cast<int64_t *>(vmaf_sycl_malloc_host(s->sycl_state, sizeof(int64_t)));
        if (!s->d_sse[p] || !s->h_sse[p]) {
            vmaf_log(VMAF_LOG_LEVEL_ERROR, "psnr_sycl: SSE accumulator alloc failed\n");
            return -ENOMEM;
        }
    }
    return 0;
}

} // namespace

namespace
{

/* Chroma comes from the state's shared Cb / Cr planes (ADR-1369):
 * idempotent, so every twin that reads chroma shares one upload. */
static int allocate_chroma(PsnrStateSycl *s)
{
    if (s->n_planes < 2U) {
        return 0;
    }
    const int err = vmaf_sycl_shared_chroma_init(s->sycl_state, s->width[1], s->height[1]);
    if (err) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "psnr_sycl: shared chroma planes unavailable (%d)\n", err);
    }
    return err;
}

} // namespace

namespace
{

/* Peak, per-plane psnr_max and empty APSNR totals, as CPU integer_psnr.c::init
 * derives them (psnr_score.h). Inactive planes keep the default ceiling: a
 * zero plane size would turn a min_sse ceiling into -inf, and nothing reads it. */
static void configure_scores(PsnrStateSycl *s, unsigned bpc)
{
    s->bpc = bpc;
    s->peak = vmaf_psnr_peak(bpc, s->reduced_hbd_peak);
    for (unsigned p = 0; p < PSNR_NUM_PLANES; p++) {
        const double min_sse = p < s->n_planes ? s->min_sse : 0.0;
        s->psnr_max[p] = vmaf_psnr_max(bpc, s->peak, min_sse, s->width[p], s->height[p]);
        s->apsnr_sse[p] = 0U;
        s->apsnr_n_pixels[p] = 0U;
    }
}

} // namespace

namespace
{

static int close_fex_sycl(VmafFeatureExtractor *fex);

static int init_fex_sycl(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                         unsigned w, unsigned h)
{
    auto *s = static_cast<PsnrStateSycl *>(fex->priv);
    configure_geometry(s, pix_fmt, w, h);
    configure_scores(s, bpc);

    if (!fex->sycl_state) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "psnr_sycl: no SYCL state\n");
        return -EINVAL;
    }

    s->sycl_state = fex->sycl_state;
    int const err = vmaf_sycl_shared_frame_init(s->sycl_state, w, h, bpc);
    if (err) {
        return err;
    }
    const int alloc_err = allocate_sse(s);
    if (alloc_err) {
        (void)close_fex_sycl(fex);
        return alloc_err;
    }
    const int chroma_err = allocate_chroma(s);
    if (chroma_err) {
        (void)close_fex_sycl(fex);
        return chroma_err;
    }

    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (!s->feature_name_dict) {
        (void)close_fex_sycl(fex);
        return -ENOMEM;
    }

    s->has_pending = false;

    int const err2 = vmaf_sycl_graph_register(s->sycl_state, enqueue_psnr_work, psnr_pre_graph,
                                              psnr_post_graph, config_psnr_slot, s, "PSNR");
    if (err2) {
        (void)close_fex_sycl(fex);
        return err2;
    }

    return 0;
}

} // namespace

namespace
{

static int submit_fex_sycl(VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                           VmafPicture *dist_pic, VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic_90;
    (void)dist_pic_90;

    auto *s = static_cast<PsnrStateSycl *>(fex->priv);
    VmafSyclState *state = fex->sycl_state;

    /* Upload the frame's chroma BEFORE graph_submit, which puts the
     * combined queue behind the last upload. Once per frame for every
     * twin; the zero-copy import path hands no host pictures and
     * imports luma only. */
    if (s->n_planes > 1U) {
        int const chroma_err = (ref_pic && dist_pic) ?
                                   vmaf_sycl_shared_chroma_upload(state, ref_pic, dist_pic) :
                                   -EINVAL;
        if (chroma_err) {
            vmaf_log(VMAF_LOG_LEVEL_ERROR, "psnr_sycl: frame %u chroma not on the device (%d)\n",
                     index, chroma_err);
            return chroma_err;
        }
    }

    int const err = vmaf_sycl_graph_submit(state);
    if (err) {
        return err;
    }

    s->pending_index = index;
    s->has_pending = true;
    return 0;
}

} // namespace

namespace
{

/* Feature names — same arrays as the CPU path
 * (core/src/feature/integer_psnr.c::psnr_name / mse_name / flush). */
static const char *const psnr_name[PSNR_NUM_PLANES] = {"psnr_y", "psnr_cb", "psnr_cr"};
static const char *const mse_name[PSNR_NUM_PLANES] = {"mse_y", "mse_cb", "mse_cr"};
static const char *const apsnr_name[PSNR_NUM_PLANES] = {"apsnr_y", "apsnr_cb", "apsnr_cr"};

} // namespace

namespace
{

/* Score one plane from its device-reduced SSE, in CPU order: `psnr_*`,
 * then `mse_*` when `enable_mse` is set. `enable_apsnr` folds the SSE into
 * the clip totals that flush_fex_sycl() publishes. */
static int emit_plane(PsnrStateSycl *s, unsigned p, unsigned index,
                      VmafFeatureCollector *feature_collector)
{
    const auto sse = static_cast<uint64_t>(*s->h_sse[p]);
    if (s->enable_apsnr) {
        s->apsnr_sse[p] += sse;
        s->apsnr_n_pixels[p] += (uint64_t)s->height[p] * s->width[p];
    }
    const double mse = (double)sse / ((double)s->width[p] * (double)s->height[p]);
    const double psnr =
        vmaf_psnr_from_mse(mse, (double)s->peak * (double)s->peak, s->psnr_max[p], s->uncapped);
    int err = vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                      psnr_name[p], psnr, index);
    if (!err && s->enable_mse) {
        err = vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                      mse_name[p], mse, index);
    }
    return err;
}

} // namespace

namespace
{

static int collect_fex_sycl(VmafFeatureExtractor *fex, unsigned index,
                            VmafFeatureCollector *feature_collector)
{
    auto *s = static_cast<PsnrStateSycl *>(fex->priv);
    VmafSyclState *state = fex->sycl_state;

    /* A failed wait leaves the SSE stale: fail, never score it. */
    int const wait_err = vmaf_sycl_graph_wait(state);
    if (wait_err)
        return wait_err;

    int rc = 0;
    for (unsigned p = 0; p < s->n_planes; p++) {
        const int e = emit_plane(s, p, index, feature_collector);
        if (e && rc == 0) {
            rc = e;
        }
    }
    return rc;
}

/* `enable_apsnr`: publish the clip-aggregate APSNR of every active plane,
 * exactly as CPU integer_psnr.c::flush does. Runs after the final
 * collect (libvmaf.c flush_context_sycl), so the totals are complete. */
static int flush_fex_sycl(VmafFeatureExtractor *fex, VmafFeatureCollector *feature_collector)
{
    const auto *s = static_cast<const PsnrStateSycl *>(fex->priv);
    int err = 0;
    if (s->enable_apsnr) {
        for (unsigned p = 0; p < s->n_planes; p++) {
            const double apsnr =
                vmaf_psnr_aggregate(s->peak, s->apsnr_sse[p], s->apsnr_n_pixels[p], s->psnr_max[p]);
            err |= vmaf_feature_collector_set_aggregate(feature_collector, apsnr_name[p], apsnr);
        }
    }
    return (err < 0) ? err : !err;
}

} // namespace

namespace
{

static int close_fex_sycl(VmafFeatureExtractor *fex)
{
    auto *s = static_cast<PsnrStateSycl *>(fex->priv);
    if (s->sycl_state) {
        /* Unregister from the combined command graph before freeing priv.
         * Mirrors the fix in integer_motion_sycl.cpp (ADR-0989):
         * vmaf_sycl_graph_unregister() drains combined_queue and removes
         * this extractor's entry so a subsequent VmafContext sharing the
         * same sycl_state does not inherit a dangling priv pointer. */
        (void)vmaf_sycl_graph_unregister(s->sycl_state, s);

        for (unsigned p = 0; p < PSNR_NUM_PLANES; p++) {
            if (s->d_sse[p])
                vmaf_sycl_free(s->sycl_state, s->d_sse[p]);
            if (s->h_sse[p])
                vmaf_sycl_free(s->sycl_state, s->h_sse[p]);
        }
    }
    if (s->feature_name_dict)
        vmaf_dictionary_free(&s->feature_name_dict);
    return 0;
}

} // namespace

namespace
{

/* Provided features — full luma + chroma per the chroma extension
 * (T3-15(b) second port, 2026-05-09; mirrors Vulkan ADR-0216 and
 * CUDA twin in PR #520). For YUV400 sources `init` clamps
 * `n_planes` to 1 and chroma dispatches are skipped at runtime,
 * but the static list still claims chroma so the dispatcher routes
 * `psnr_cb` / `psnr_cr` requests through the SYCL twin. */
static const char *provided_features_psnr_sycl[] = {"psnr_y", "psnr_cb", "psnr_cr", nullptr};

} // namespace

extern "C" VmafFeatureExtractor vmaf_fex_psnr_sycl = {
    .name = "psnr_sycl",
    .init = init_fex_sycl,
    .extract = nullptr,
    .flush = flush_fex_sycl,
    .close = close_fex_sycl,
    .submit = submit_fex_sycl,
    .collect = collect_fex_sycl,
    .options = options_psnr_sycl,
    .priv_size = sizeof(PsnrStateSycl),
    .flags = VMAF_FEATURE_EXTRACTOR_SYCL | VMAF_FEATURE_EXTRACTOR_TEMPORAL,
    .provided_features = provided_features_psnr_sycl,
    /* 3 dispatches/frame (one per plane), reduction-dominated;
     * AUTO + 1080p area matches motion's profile (see ADR-0181 /
     * ADR-0182). Three small dispatches are still well under the
     * threshold where batching pays off vs. AUTO scheduling. */
    .chars =
        {
            .n_dispatches_per_frame = 3,
            .is_reduction_only = true,
            .min_useful_frame_area = 1920U * 1080U,
            .dispatch_hint = VMAF_FEATURE_DISPATCH_AUTO,
        },
};
