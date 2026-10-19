/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright (c) 2019 Joshua Holmer
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent AND MIT
 *
 *  ciede2000 feature extractor on the SYCL backend (T7-23 /
 *  ADR-0182, GPU long-tail batch 1c part 3). SYCL twin of
 *  ciede_vulkan (PR #136 / ADR-0187) and ciede_cuda (this PR's
 *  batch 1c part 2).
 *
 *  Self-contained submit / collect — does *not* register with
 *  vmaf_sycl_graph_register because the shared_frame buffers are
 *  luma-only and ciede needs full Y/U/V. Each submit packs the
 *  ref/dis Y, U and V planes at their native resolution into
 *  pinned staging, uploads them, launches one kernel and reads
 *  back one float per pixel. The kernel reads chroma at
 *  (x >> ss_hor, y >> ss_ver), the nearest-neighbour upsample
 *  of ciede.c::scale_chroma_planes, as the CUDA and HIP twins do.
 *
 *  Numerical contract (ADR-1436, after ADR-1426 for the CUDA twin).
 *  ciede.c computes in double and stores in float. The kernel runs
 *  its statements in that order with every double as an fp32 pair
 *  and every math-library call as a pair function
 *  (sycl_ciede_math.h; ADR-0220: no fp64 type on the device), and
 *  rounds to float where the reference does. It stores the float
 *  of every pixel at its raster position; the host adds them into
 *  one double in the reference's order and applies
 *  `45 - 20 * log10(sum / (w * h))`. A pixel differs from the CPU's
 *  by one float step where a pair does not decide a rounding or
 *  where the host math library is not correctly rounded: the score
 *  is close to the CPU's (about 1e-11), not bit-identical.
 */

#include <sycl/sycl.hpp>

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "config.h"
#include "feature/ciede_frame_sum.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature_name.h"
#include "log.h"
#include "picture.h"
#include "sycl/common.h"

#include "sycl_ciede_math.h"
#include "sycl_compat.h"

namespace
{

/* Y, U, V. */
static constexpr unsigned CIEDE_SYCL_PLANES = 3U;
/* The kernel's sub-group size and register file (VmafSyclKernelShape).
 * Measured on an Arc A380 at 3840x2160, ms per frame: SIMD-8 76, SIMD-16 50,
 * SIMD-32 38 but with 16 KiB of register spills, which is scratch memory
 * (ADR-1395); with the 256-entry register file 147, 81 and 78. */
static constexpr int CIEDE_SYCL_SG = 16;
static constexpr int CIEDE_SYCL_GRF = 0;
/* sycl_ff_math.h's two tables, one after the other. */
static constexpr size_t CIEDE_TABLE_BYTES =
    (vmaf_sycl_ffm::kAtanTableFloats + vmaf_sycl_ffm::kSinCosTableFloats) * sizeof(float);

struct CiedeStateSycl {
    /* Frame geometry. */
    unsigned width;
    unsigned height;
    unsigned bpc;
    enum VmafPixelFormat pix_fmt;
    /* 1 when chroma is half-size along that axis (4:2:0 both, 4:2:2
     * horizontal only), 0 for 4:4:4. */
    unsigned ss_hor;
    unsigned ss_ver;
    /* Native per-plane geometry; chroma uses picture.c's ceil rule
     * `(w + ss_hor) >> ss_hor` so odd luma sizes stage every column. */
    unsigned plane_w[CIEDE_SYCL_PLANES];
    unsigned plane_h[CIEDE_SYCL_PLANES];
    size_t row_bytes[CIEDE_SYCL_PLANES];

    /* SYCL state back-pointer. */
    VmafSyclState *sycl_state;

    /* Host-pinned staging and device USM, one tightly packed buffer
     * per plane at native resolution: 4:2:0 moves half the bytes the
     * former luma-resolution upscale did, and the host no longer
     * writes one byte per luma pixel per plane. */
    void *h_ref[CIEDE_SYCL_PLANES];
    void *h_dis[CIEDE_SYCL_PLANES];
    void *d_ref[CIEDE_SYCL_PLANES];
    void *d_dis[CIEDE_SYCL_PLANES];

    /* One float per pixel, in raster order: the kernel's output and the
     * host's copy of it. */
    float *d_terms;
    float *h_terms;

    /* sycl_ff_math.h's tables in device memory (atan first, then sin / cos),
     * copied ahead of the first frame's kernel. */
    float *d_tables;
    bool tables_uploaded;

    /* ciede.c's constants for this bit depth, as fp32 pairs. */
    vmaf_sycl_ciede::Constants constants;

    /* Submit/collect plumbing. */
    bool has_pending;
    unsigned pending_index;

    VmafDictionary *feature_name_dict;
};

/* Copy `rows` rows of plane `p` into a tightly packed host buffer: one
 * memcpy when the picture rows are already contiguous, one per row
 * otherwise. Byte-oriented, so it serves every bit depth. */
static void stage_plane(const VmafPicture *pic, unsigned p, void *dst, size_t row_bytes,
                        unsigned rows)
{
    const auto *src = static_cast<const uint8_t *>(pic->data[p]);
    auto *out = static_cast<uint8_t *>(dst);
    const auto stride = static_cast<size_t>(pic->stride[p]);
    if (stride == row_bytes) {
        std::memcpy(out, src, row_bytes * rows);
        return;
    }
    for (unsigned i = 0; i < rows; i++) {
        std::memcpy(out, src, row_bytes);
        src += stride;
        out += row_bytes;
    }
}

/* Everything the kernel reads, captured by value as one struct so the
 * output pointer travels with its geometry (same shape as
 * PsnrKernelArgs / FpsnrOutput). Planes are packed at native size. */
struct CiedeKernelArgs {
    const void *ref[CIEDE_SYCL_PLANES];
    const void *dis[CIEDE_SYCL_PLANES];
    float *terms;
    vmaf_sycl_ffm::Tables tables;
    vmaf_sycl_ciede::Constants constants;
    unsigned width;
    unsigned height;
    unsigned chroma_w;
    unsigned ss_hor;
    unsigned ss_ver;
    unsigned bpc;
};

/* One pixel's Y/U/V as float from native-resolution planes. */
template <typename T>
static inline vmaf_sycl_ciede::Samples load_yuv(const void *const planes[CIEDE_SYCL_PLANES],
                                                size_t off_y, size_t off_c)
{
    return {.y = (float)static_cast<const T *>(planes[0])[off_y],
            .u = (float)static_cast<const T *>(planes[1])[off_c],
            .v = (float)static_cast<const T *>(planes[2])[off_c]};
}

/* ΔE2000 of the pixel at (x, y). Chroma is read at
 * (x >> ss_hor, y >> ss_ver): output row i of scale_chroma_planes
 * reads input row i >> 1 on 4:2:0 and column j >> 1 whenever the
 * format is not 4:4:4. */
__attribute__((flatten, always_inline)) static inline float ciede_pixel(const CiedeKernelArgs &a,
                                                                        size_t x, size_t y)
{
    const size_t cx = a.ss_hor ? (x >> 1) : x;
    const size_t cy = a.ss_ver ? (y >> 1) : y;
    const size_t off_y = y * (size_t)a.width + x;
    const size_t off_c = cy * (size_t)a.chroma_w + cx;
    if (a.bpc <= 8) {
        return vmaf_sycl_ciede::pixel(load_yuv<uint8_t>(a.ref, off_y, off_c),
                                      load_yuv<uint8_t>(a.dis, off_y, off_c), a.constants,
                                      a.tables);
    }
    return vmaf_sycl_ciede::pixel(load_yuv<uint16_t>(a.ref, off_y, off_c),
                                  load_yuv<uint16_t>(a.dis, off_y, off_c), a.constants, a.tables);
}

/* One work-item per pixel stores that pixel's float at its raster position.
 * There is no reduction on the device: extract() adds every pixel into one
 * double, row after row, and in a large frame those additions round. */
class CiedeKernel : public VmafSyclKernelShape<CIEDE_SYCL_SG, CIEDE_SYCL_GRF>
{
  public:
    explicit CiedeKernel(const CiedeKernelArgs &args) : a_(args)
    {
    }

    VMAF_SYCL_FUNCTOR_SG_SIZE(CIEDE_SYCL_SG) void operator()(sycl::id<2> id) const
    {
        a_.terms[id[0] * (size_t)a_.width + id[1]] = ciede_pixel(a_, id[1], id[0]);
    }

  private:
    CiedeKernelArgs a_;
};

static void launch_ciede(sycl::queue &q, const CiedeKernelArgs &args)
{
    q.submit([&](sycl::handler &h) {
        h.parallel_for(sycl::range<2>{args.height, args.width}, CiedeKernel(args));
    });
}

/* Record frame geometry, chroma subsampling and per-plane sizes. */
static void ciede_set_geometry(CiedeStateSycl *s, enum VmafPixelFormat pix_fmt, unsigned bpc,
                               unsigned w, unsigned h)
{
    s->width = w;
    s->height = h;
    s->bpc = bpc;
    s->pix_fmt = pix_fmt;
    s->ss_hor = (pix_fmt != VMAF_PIX_FMT_YUV444P) ? 1U : 0U;
    s->ss_ver = (pix_fmt == VMAF_PIX_FMT_YUV420P) ? 1U : 0U;
    const size_t bpp = (bpc <= 8U) ? 1U : 2U;
    for (unsigned p = 0; p < CIEDE_SYCL_PLANES; p++) {
        const unsigned sh = (p > 0U) ? s->ss_hor : 0U;
        const unsigned sv = (p > 0U) ? s->ss_ver : 0U;
        s->plane_w[p] = (w + sh) >> sh;
        s->plane_h[p] = (h + sv) >> sv;
        s->row_bytes[p] = (size_t)s->plane_w[p] * bpp;
    }
    s->constants = vmaf_sycl_ciede::make_constants(bpc);
    s->tables_uploaded = false;
}

/* Allocate staging, device planes, the per-pixel terms and the tables. Stops at the first
 * failure; close_fex_sycl releases whatever was allocated. */
static bool ciede_alloc_buffers(CiedeStateSycl *s)
{
    VmafSyclState *state = s->sycl_state;
    for (unsigned p = 0; p < CIEDE_SYCL_PLANES; p++) {
        const size_t bytes = s->row_bytes[p] * s->plane_h[p];
        s->h_ref[p] = vmaf_sycl_malloc_host(state, bytes);
        s->h_dis[p] = vmaf_sycl_malloc_host(state, bytes);
        s->d_ref[p] = vmaf_sycl_malloc_device(state, bytes);
        s->d_dis[p] = vmaf_sycl_malloc_device(state, bytes);
        if (!s->h_ref[p] || !s->h_dis[p] || !s->d_ref[p] || !s->d_dis[p])
            return false;
    }
    const size_t term_bytes = (size_t)s->width * s->height * sizeof(float);
    s->d_terms = static_cast<float *>(vmaf_sycl_malloc_device(state, term_bytes));
    s->h_terms = static_cast<float *>(vmaf_sycl_malloc_host(state, term_bytes));
    s->d_tables = static_cast<float *>(vmaf_sycl_malloc_device(state, CIEDE_TABLE_BYTES));
    return s->d_terms != nullptr && s->h_terms != nullptr && s->d_tables != nullptr;
}

/* Every plane of both pictures into d_ref / d_dis: staged on the host and
 * enqueued plane by plane so the DMA of one plane overlaps the host packing
 * the next, or, for frames of the VMAFx API on this device, copied on the
 * device (ADR-2091; the frame's planes are never read on the host). */
static int upload_planes(sycl::queue &q, const CiedeStateSycl *s, const VmafPicture *ref_pic,
                         const VmafPicture *dist_pic)
{
    const bool device =
        vmaf_sycl_picture_on_device(ref_pic) || vmaf_sycl_picture_on_device(dist_pic);
    for (unsigned p = 0; p < CIEDE_SYCL_PLANES; p++) {
        const size_t row = s->row_bytes[p];
        if (device) {
            const int err = vmaf_sycl_picture_read_plane(ref_pic, p, &q, s->d_ref[p], row, row,
                                                         s->plane_h[p], nullptr) ||
                            vmaf_sycl_picture_read_plane(dist_pic, p, &q, s->d_dis[p], row, row,
                                                         s->plane_h[p], nullptr);
            if (err) {
                return -EIO;
            }
            continue;
        }
        const size_t bytes = row * s->plane_h[p];
        stage_plane(ref_pic, p, s->h_ref[p], row, s->plane_h[p]);
        q.memcpy(s->d_ref[p], s->h_ref[p], bytes);
        stage_plane(dist_pic, p, s->h_dis[p], row, s->plane_h[p]);
        q.memcpy(s->d_dis[p], s->h_dis[p], bytes);
    }
    return 0;
}

/* True when `pic` has the geometry the staging buffers were sized for. */
static bool ciede_picture_matches(const CiedeStateSycl *s, const VmafPicture *pic)
{
    if (!pic || pic->bpc != s->bpc)
        return false;
    for (unsigned p = 0; p < CIEDE_SYCL_PLANES; p++) {
        if (!pic->data[p] || pic->w[p] != s->plane_w[p] || pic->h[p] != s->plane_h[p])
            return false;
    }
    return true;
}

} /* anonymous namespace */

extern "C" {

static const VmafOption options_ciede_sycl[] = {{.name = nullptr}};

// NOLINTBEGIN(misc-use-anonymous-namespace, misc-use-internal-linkage) — ADR-0141 §2 load-bearing invariant: the
// `init_fex_sycl` / `submit_fex_sycl` / `collect_fex_sycl` / `close_fex_sycl`
// entry points use C-style `static` rather than an anonymous namespace because
// their addresses are stored in the `extern "C" VmafFeatureExtractor` struct at
// the bottom of this file, which the C ABI consumes through the
// function-pointer types in `feature_extractor.h`. A namespace cannot appear
// inside this linkage specification at all. Same band, same reason, as
// float_adm_sycl.cpp and speed_chroma_sycl.cpp. Per CLAUDE.md §12 r12 these are
// load-bearing invariants of the SYCL <-> libvmaf C-API ABI. ADR-0278.
static int close_fex_sycl(VmafFeatureExtractor *fex);

static int init_fex_sycl(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                         unsigned w, unsigned h)
{
    if (pix_fmt == VMAF_PIX_FMT_YUV400P)
        return -EINVAL;

    auto *s = static_cast<CiedeStateSycl *>(fex->priv);
    ciede_set_geometry(s, pix_fmt, bpc, w, h);

    if (!fex->sycl_state) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "ciede_sycl: no SYCL state\n");
        return -EINVAL;
    }
    s->sycl_state = fex->sycl_state;

    if (!ciede_alloc_buffers(s)) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "ciede_sycl: USM allocation failed\n");
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
    return 0;
}

static int submit_fex_sycl(VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                           VmafPicture *dist_pic, VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic_90;
    (void)dist_pic_90;
    auto *s = static_cast<CiedeStateSycl *>(fex->priv);
    auto *qptr = static_cast<sycl::queue *>(vmaf_sycl_get_queue_ptr(s->sycl_state));
    if (!qptr)
        return -EINVAL;
    if (!ciede_picture_matches(s, ref_pic) || !ciede_picture_matches(s, dist_pic))
        return -EINVAL;
    sycl::queue &q = *qptr;

    if (!s->tables_uploaded) {
        /* The tables do not change. The queue is in order, so the copies
         * precede every kernel that reads them; init issues no queue
         * operation. */
        q.memcpy(s->d_tables, vmaf_sycl_ffm::kAtanTable, sizeof(vmaf_sycl_ffm::kAtanTable));
        q.memcpy(s->d_tables + vmaf_sycl_ffm::kAtanTableFloats, vmaf_sycl_ffm::kSinCosTable,
                 sizeof(vmaf_sycl_ffm::kSinCosTable));
        s->tables_uploaded = true;
    }

    if (upload_planes(q, s, ref_pic, dist_pic) != 0)
        return -EIO;
    CiedeKernelArgs args = {};
    for (unsigned p = 0; p < CIEDE_SYCL_PLANES; p++) {
        args.ref[p] = s->d_ref[p];
        args.dis[p] = s->d_dis[p];
    }
    args.terms = s->d_terms;
    args.tables = {.atan = s->d_tables, .sin_cos = s->d_tables + vmaf_sycl_ffm::kAtanTableFloats};
    args.constants = s->constants;
    args.width = s->width;
    args.height = s->height;
    args.chroma_w = s->plane_w[1];
    args.ss_hor = s->ss_hor;
    args.ss_ver = s->ss_ver;
    args.bpc = s->bpc;
    launch_ciede(q, args);

    /* The only device-to-host copy of the frame; collect() waits on it. */
    q.memcpy(s->h_terms, s->d_terms, (size_t)s->width * s->height * sizeof(float));

    s->pending_index = index;
    s->has_pending = true;
    return 0;
}

static int collect_fex_sycl(VmafFeatureExtractor *fex, unsigned index,
                            VmafFeatureCollector *feature_collector)
{
    auto *s = static_cast<CiedeStateSycl *>(fex->priv);
    auto *qptr = static_cast<sycl::queue *>(vmaf_sycl_get_queue_ptr(s->sycl_state));
    if (!qptr)
        return -EINVAL;
    qptr->wait();

    /* extract(): the frame sum in the reference's order, then its score
     * expression. */
    const double de00_sum = ciede_frame_sum(s->h_terms, (size_t)s->width * s->height);
    const double score = 45. - 20. * std::log10(de00_sum / (s->width * s->height));

    return vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "ciede2000", score, index);
}

static int close_fex_sycl(VmafFeatureExtractor *fex)
{
    auto *s = static_cast<CiedeStateSycl *>(fex->priv);
    if (s->sycl_state) {
        void *const buffers[] = {
            s->h_ref[0], s->h_ref[1], s->h_ref[2], s->h_dis[0], s->h_dis[1],
            s->h_dis[2], s->d_ref[0], s->d_ref[1], s->d_ref[2], s->d_dis[0],
            s->d_dis[1], s->d_dis[2], s->d_terms,  s->h_terms,  s->d_tables,
        };
        for (void *buf : buffers) {
            if (buf)
                vmaf_sycl_free(s->sycl_state, buf);
        }
    }
    if (s->feature_name_dict)
        vmaf_dictionary_free(&s->feature_name_dict);
    return 0;
}

static const char *provided_features_ciede_sycl[] = {"ciede2000", nullptr};

extern "C" VmafFeatureExtractor vmaf_fex_ciede_sycl = {
    .name = "ciede_sycl",
    .init = init_fex_sycl,
    .extract = nullptr,
    .flush = nullptr,
    .close = close_fex_sycl,
    .submit = submit_fex_sycl,
    .collect = collect_fex_sycl,
    .options = options_ciede_sycl,
    .priv_size = sizeof(CiedeStateSycl),
    .flags = VMAF_FEATURE_EXTRACTOR_SYCL,
    .provided_features = provided_features_ciede_sycl,
    .chars =
        {
            .n_dispatches_per_frame = 1,
            .is_reduction_only = false,
            .min_useful_frame_area = 1920U * 1080U,
            .dispatch_hint = VMAF_FEATURE_DISPATCH_AUTO,
        },
};

} /* extern "C" */
// NOLINTEND(misc-use-anonymous-namespace, misc-use-internal-linkage)
