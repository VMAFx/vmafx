/* Upstream-mirror filename: defines float_moment symbol despite the integer_ prefix (matches Netflix upstream). See ADR-0549. */
/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
 *
 *  float_moment feature extractor on the SYCL backend
 *  (T7-23 / ADR-0182, GPU long-tail batch 1d part 3). SYCL twin
 *  of moment_vulkan (PR #133) and moment_cuda (this PR's batch
 *  1d part 2).
 *
 *  Algorithm (mirrors libvmaf/src/feature/float_moment.c::extract):
 *      for each pixel:
 *          ref1 += ref;        ref2 += ref * ref;
 *          dis1 += dis;        dis2 += dis * dis;
 *      host divides each accumulator by w*h.
 *
 *  Single kernel per frame; four sycl::atomic_ref<int64_t>
 *  reductions into the four-slot device counter. Pattern:
 *  register with vmaf_sycl_graph_register and ride the combined
 *  graph submit/wait machinery (mirrors psnr_sycl).
 */

#include <sycl/sycl.hpp>

#include <cerrno>
#include <cstdint>
#include <cstdlib>

extern "C" {
#include "config.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature_name.h"
#include "log.h"
#include "picture.h"
#include "sycl/common.h"
}

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

    /* Submit/collect plumbing. */
    bool has_pending;
    unsigned pending_index;

    VmafDictionary *feature_name_dict;
};

/* Per-pixel moment kernel. Reads the shared ref/dis frame
 * buffers (uint8 packed at ≤8bpc, uint16 packed at ≥10bpc,
 * tightly packed at `width * bytes_per_pixel`). Atomic-adds
 * each pixel's four contributions to the device accumulators. */
static void launch_moment(sycl::queue &q, void *shared_ref, void *shared_dis, int64_t *d_sums,
                          unsigned width, unsigned height, unsigned bpc)
{
    sycl::range<2> global{(size_t)height, (size_t)width};
    const unsigned e_w = width;
    const unsigned e_bpc = bpc;
    void *ref_in = shared_ref;
    void *dis_in = shared_dis;

    q.submit([=](sycl::handler &h) {
        h.parallel_for(global, [=](sycl::id<2> id) {
            const size_t y = id[0];
            const size_t x = id[1];
            const size_t off = y * (size_t)e_w + x;
            int64_t r;
            int64_t d;
            if (e_bpc <= 8) {
                r = (int64_t)static_cast<const uint8_t *>(ref_in)[off];
                d = (int64_t)static_cast<const uint8_t *>(dis_in)[off];
            } else {
                r = (int64_t)static_cast<const uint16_t *>(ref_in)[off];
                d = (int64_t)static_cast<const uint16_t *>(dis_in)[off];
            }
            using atomic64 =
                sycl::atomic_ref<int64_t, sycl::memory_order::relaxed, sycl::memory_scope::device,
                                 sycl::access::address_space::global_space>;
            atomic64(d_sums[0]).fetch_add(r);
            atomic64(d_sums[1]).fetch_add(d);
            atomic64(d_sums[2]).fetch_add(r * r);
            atomic64(d_sums[3]).fetch_add(d * d);
        });
    });
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

static const VmafOption options_moment_sycl[] = {{0}};

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
    if (!s->d_sums || !s->h_sums) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "float_moment_sycl: device memory allocation failed\n");
        return -ENOMEM;
    }

    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (!s->feature_name_dict)
        return -ENOMEM;

    s->has_pending = false;

    int const err2 = vmaf_sycl_graph_register(state, enqueue_moment_work, moment_pre_graph,
                                              moment_post_graph, config_moment_slot, s, "MOMENT");
    if (err2)
        return err2;

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

    vmaf_sycl_graph_wait(state);

    const double n_pixels = (double)s->width * (double)s->height;
    const double ref1 = (double)s->h_sums[0] / n_pixels;
    const double dis1 = (double)s->h_sums[1] / n_pixels;
    const double ref2 = (double)s->h_sums[2] / n_pixels;
    const double dis2 = (double)s->h_sums[3] / n_pixels;

    int err = vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                      "float_moment_ref1st", ref1, index);
    if (!err)
        err = vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                      "float_moment_dis1st", dis1, index);
    if (!err)
        err = vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                      "float_moment_ref2nd", ref2, index);
    if (!err)
        err = vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                      "float_moment_dis2nd", dis2, index);
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
    NULL,
};

extern "C" VmafFeatureExtractor vmaf_fex_float_moment_sycl = {
    .name = "float_moment_sycl",
    .init = init_fex_sycl,
    .extract = NULL,
    .flush = NULL,
    .close = close_fex_sycl,
    .submit = submit_fex_sycl,
    .collect = collect_fex_sycl,
    .options = options_moment_sycl,
    .priv_size = sizeof(MomentStateSycl),
    .flags = VMAF_FEATURE_EXTRACTOR_SYCL,
    .provided_features = provided_features_moment_sycl,
    .chars =
        {
            .n_dispatches_per_frame = 1,
            .is_reduction_only = true,
            .min_useful_frame_area = 1920U * 1080U,
            .dispatch_hint = VMAF_FEATURE_DISPATCH_AUTO,
        },
};

} /* extern "C" */
