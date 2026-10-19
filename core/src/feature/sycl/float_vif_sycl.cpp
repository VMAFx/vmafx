/**
 *  Copyright 2016-2020 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  float_vif feature kernel on the SYCL backend (T7-23 / batch 3
 *  part 5c — ADR-0192 / ADR-0197). SYCL twin of float_vif_vulkan +
 *  float_vif_cuda.
 *
 *  v1: kernelscale=1.0 only. CPU's VIF_OPT_HANDLE_BORDERS branch:
 *  per-scale dims = prev/2 (no border crop); decimate samples at
 *  (2*gx, 2*gy) with mirror padding on input filter taps.
 *
 *  Per-frame flow: 3 decimate launches and, per scale, a filter, a
 *  statistic and a row-sum launch; one readback.
 *  Self-contained submit/collect — does NOT register with
 *  vmaf_sycl_graph_register (the multi-scale layout doesn't fit the
 *  shared_frame model).
 *
 *  Numerical contract (ADR-1422, after ADR-1412 for the CUDA twin). The twin
 *  returns the CPU extractor's values bit for bit:
 *   - taps: the four filters come from vif_get_filter(), as float_vif.c
 *     derives them; no kernel holds a tap literal.
 *   - filters: each tap one rounded fp32 multiply and one rounded fp32 add,
 *     taps in order, vertical pass then horizontal (the TU builds with
 *     contraction off, ADR-1367).
 *   - statistic: vif_pixel_statistic_s() and log2f_approx() operation for
 *     operation; the two fp64 expressions of the reference without the fp64
 *     type (sycl_float_vif_math.h).
 *   - sums: vif_statistic_s() adds the terms of a row into one fp32
 *     accumulator and the rows into another. The filter kernel stores each
 *     pixel's variances, the statistic kernel turns them into the two terms,
 *     a row kernel adds every row left to right, and the host adds the rows.
 *
 *  No kernel uses scratch memory (ADR-1395): the statistic has its own
 *  kernel because its 64-bit integer path spilled next to the filter's tile.
 */

#include <sycl/sycl.hpp>

#include "sycl_compat.h"
#include "sycl_float_vif_math.h"
#include "sycl_tile_index.h"

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>

#include "config.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature_name.h"
#include "feature/nonfinite_score.h"
#include "vif_tools.h"
#include "log.h"
#include "picture.h"
#include "sycl/common.h"

namespace
{

constexpr int FVIF_BX = 16;
constexpr int FVIF_BY = 16;
constexpr int FVIF_MAX_FW = 17;
constexpr int FVIF_MAX_HFW = 8;

/* One scale's filter, as a kernel argument. The kernels index it with the
 * constants of their unrolled tap loops only: an index known at run time
 * would put the array in private memory (ADR-1395). */
struct VifTaps {
    float tap[FVIF_MAX_FW];
};

} // namespace

namespace
{

struct FloatVifStateSycl {
    bool debug;
    double vif_enhn_gain_limit;
    double vif_kernelscale;
    double vif_sigma_nsq;
    bool vif_skip_scale0; /* host-side suppression: emit 0.0 for scale-0, mirrors float_vif.c */
    double vif_scale1_min_val;
    double vif_scale2_min_val;
    double vif_scale3_min_val;

    unsigned width;
    unsigned height;
    unsigned bpc;
    unsigned scale_w[4];
    unsigned scale_h[4];

    VmafSyclState *sycl_state;

    /* Pinned host raw uploads. */
    void *h_ref_raw;
    void *h_dis_raw;
    /* Device raw + ping-pong float buffers. */
    void *d_ref_raw;
    void *d_dis_raw;
    float *d_ref_buf[2];
    float *d_dis_buf[2];

    /* vif_get_filter() taps of the four scales; a launch hands its scale's
     * taps to the kernel by value. */
    VifTaps taps[4];
    /* Per-pixel planes of the scale being computed (scale-0 size; the queue
     * is in order, so the scales share them). The filter kernel stores
     * sigma1_sq, sigma2_sq and sigma12; the statistic kernel replaces the
     * first two by the numerator and denominator terms. */
    float *d_num_terms;
    float *d_den_terms;
    float *d_sigma12;
    /* Row sums of every scale, [num rows | den rows] per scale at
     * row_offset[scale]; one copy to the host per frame. */
    float *d_rows;
    float *h_rows;
    size_t row_offset[4];
    size_t row_floats;

    bool has_pending;
    unsigned pending_index;

    VmafDictionary *feature_name_dict;
};

} // namespace

namespace
{

/* Filter widths of vif_get_filter_size(scale, 1.0). The taps themselves are
 * computed on the host by vif_get_filter() and read from device memory. */
template <int SCALE> struct VifFilterConstants {
    static constexpr int width = (1 << (4 - SCALE)) + 1;
    static constexpr int half_width = width / 2;
};

} // namespace

namespace
{

using VifLocal = sycl::local_accessor<float, 1>;

struct VifComputeArgs {
    const void *reference_raw;
    const void *distorted_raw;
    const float *reference_float;
    const float *distorted_float;
    VifTaps taps;
    float *sigma1_sq;
    float *sigma2_sq;
    float *sigma12;
    unsigned raw_stride;
    unsigned float_stride;
    unsigned width;
    unsigned height;
    unsigned bpc;
};

/* What the statistic kernel reads and writes: each pixel's two terms replace
 * its two variances. */
struct VifStatisticArgs {
    float *sigma1_sq_then_numerator;
    float *sigma2_sq_then_denominator;
    const float *sigma12;
    vmaf_sycl_fvif::StatisticParams statistic;
};

struct VifScratch {
    VifLocal reference;
    VifLocal distorted;
    VifLocal vertical_reference_mean;
    VifLocal vertical_distorted_mean;
    VifLocal vertical_reference_square;
    VifLocal vertical_distorted_square;
    VifLocal vertical_cross_product;
};

struct VifMoments {
    float reference_mean;
    float distorted_mean;
    float reference_square;
    float distorted_square;
    float cross_product;
};

/* What the row-sum kernel reads and writes. */
struct VifRowSumArgs {
    const float *numerator_terms;
    const float *denominator_terms;
    float *numerator_rows;
    float *denominator_rows;
    unsigned width;
    unsigned height;
};

} // namespace

namespace
{

struct VifDecimateArgs {
    const void *reference_raw;
    const void *distorted_raw;
    const float *reference_float;
    const float *distorted_float;
    VifTaps taps;
    float *reference_output;
    float *distorted_output;
    unsigned raw_stride;
    unsigned float_stride;
    unsigned output_stride;
    unsigned output_width;
    unsigned output_height;
    unsigned input_width;
    unsigned input_height;
    unsigned bpc;
};

struct VifSamplePair {
    float reference;
    float distorted;
};

} // namespace

namespace
{

static inline int vif_mirror(int index, int extent)
{
    if (index < 0) {
        return -index;
    }
    if (index >= extent) {
        return 2 * extent - index - 2;
    }
    return index;
}

static inline float vif_raw_scale(unsigned bpc)
{
    if (bpc == 10u) {
        return 4.0f;
    }
    if (bpc == 12u) {
        return 16.0f;
    }
    return bpc == 16u ? 256.0f : 1.0f;
}

static inline float read_vif_raw_plane(const void *plane, unsigned stride, unsigned bpc, int y,
                                       int x)
{
    const size_t row_offset = (size_t)y * stride;
    if (bpc <= 8u) {
        return (float)static_cast<const uint8_t *>(plane)[row_offset + (size_t)x] - 128.0f;
    }
    const auto *row =
        reinterpret_cast<const uint16_t *>(static_cast<const uint8_t *>(plane) + row_offset);
    return (float)row[x] / vif_raw_scale(bpc) - 128.0f;
}

} // namespace

namespace
{

static inline float read_vif_raw(const VifComputeArgs &args, const void *plane, int y, int x)
{
    return read_vif_raw_plane(plane, args.raw_stride, args.bpc, y, x);
}

template <int SCALE>
static inline float read_vif_sample(const VifComputeArgs &args, const void *raw, const float *plane,
                                    int y, int x)
{
    if constexpr (SCALE == 0) {
        return read_vif_raw(args, raw, y, x);
    }
    return plane[(size_t)y * args.float_stride + (size_t)x];
}

} // namespace

namespace
{

template <int SCALE>
static inline void load_vif_tile(sycl::nd_item<2> item, const VifComputeArgs &args,
                                 const VifScratch &scratch)
{
    using F = VifFilterConstants<SCALE>;
    constexpr int half_width = F::half_width;
    constexpr size_t maximum_tile_width = FVIF_BX + 2 * FVIF_MAX_HFW;
    const int tile_width = FVIF_BX + 2 * half_width;
    const int tile_height = FVIF_BY + 2 * half_width;
    const int origin_y = (int)(item.get_group(0) * FVIF_BY) - half_width;
    const int origin_x = (int)(item.get_group(1) * FVIF_BX) - half_width;
    const int local = (int)(item.get_local_id(0) * FVIF_BX + item.get_local_id(1));
    for (int offset = local; offset < tile_height * tile_width; offset += FVIF_BX * FVIF_BY) {
        const int tile_y = offset / tile_width;
        const int tile_x = offset - tile_y * tile_width;
        const int y =
            vmaf_sycl_tile_index(vif_mirror(origin_y + tile_y, (int)args.height), (int)args.height);
        const int x =
            vmaf_sycl_tile_index(vif_mirror(origin_x + tile_x, (int)args.width), (int)args.width);
        const size_t index = (size_t)tile_y * maximum_tile_width + (size_t)tile_x;
        scratch.reference[index] =
            read_vif_sample<SCALE>(args, args.reference_raw, args.reference_float, y, x);
        scratch.distorted[index] =
            read_vif_sample<SCALE>(args, args.distorted_raw, args.distorted_float, y, x);
    }
}

} // namespace

namespace
{

template <int SCALE>
static inline VifMoments vertical_vif_moments(const VifTaps &taps, const VifScratch &scratch,
                                              int row, int column)
{
    using F = VifFilterConstants<SCALE>;
    constexpr int width = F::width;
    constexpr int maximum_tile_width = FVIF_BX + 2 * FVIF_MAX_HFW;
    VifMoments moments{};
#pragma unroll
    for (int tap = 0; tap < width; ++tap) {
        const float coefficient = taps.tap[tap];
        const size_t index = (size_t)(row + tap) * maximum_tile_width + (size_t)column;
        const float reference = scratch.reference[index];
        const float distorted = scratch.distorted[index];
        moments.reference_mean += coefficient * reference;
        moments.distorted_mean += coefficient * distorted;
        moments.reference_square += coefficient * (reference * reference);
        moments.distorted_square += coefficient * (distorted * distorted);
        moments.cross_product += coefficient * (reference * distorted);
    }
    return moments;
}

} // namespace

namespace
{

template <int SCALE>
static inline void filter_vif_vertical(sycl::nd_item<2> item, const VifTaps &taps,
                                       const VifScratch &scratch)
{
    using F = VifFilterConstants<SCALE>;
    constexpr int half_width = F::half_width;
    constexpr int maximum_tile_width = FVIF_BX + 2 * FVIF_MAX_HFW;
    const int tile_width = FVIF_BX + 2 * half_width;
    const int local = (int)(item.get_local_id(0) * FVIF_BX + item.get_local_id(1));
    for (int offset = local; offset < FVIF_BY * tile_width; offset += FVIF_BX * FVIF_BY) {
        const int row = offset / tile_width;
        const int column = offset - row * tile_width;
        const VifMoments moments = vertical_vif_moments<SCALE>(taps, scratch, row, column);
        const size_t index = (size_t)row * maximum_tile_width + (size_t)column;
        scratch.vertical_reference_mean[index] = moments.reference_mean;
        scratch.vertical_distorted_mean[index] = moments.distorted_mean;
        scratch.vertical_reference_square[index] = moments.reference_square;
        scratch.vertical_distorted_square[index] = moments.distorted_square;
        scratch.vertical_cross_product[index] = moments.cross_product;
    }
}

} // namespace

namespace
{

template <int SCALE>
static inline VifMoments horizontal_vif_moments(sycl::nd_item<2> item, const VifTaps &taps,
                                                const VifScratch &scratch)
{
    using F = VifFilterConstants<SCALE>;
    constexpr int width = F::width;
    constexpr int maximum_tile_width = FVIF_BX + 2 * FVIF_MAX_HFW;
    const size_t row = item.get_local_id(0) * maximum_tile_width;
    const size_t column = item.get_local_id(1);
    VifMoments moments{};
#pragma unroll
    for (int tap = 0; tap < width; ++tap) {
        const float coefficient = taps.tap[tap];
        const size_t index = row + column + (size_t)tap;
        moments.reference_mean += coefficient * scratch.vertical_reference_mean[index];
        moments.distorted_mean += coefficient * scratch.vertical_distorted_mean[index];
        moments.reference_square += coefficient * scratch.vertical_reference_square[index];
        moments.distorted_square += coefficient * scratch.vertical_distorted_square[index];
        moments.cross_product += coefficient * scratch.vertical_cross_product[index];
    }
    return moments;
}

} // namespace

namespace
{

/* vif_pixel_statistic_s()'s variances of one pixel, stored for the statistic
 * kernel. */
static inline void store_vif_sigmas(sycl::nd_item<2> item, const VifComputeArgs &args,
                                    const VifMoments &moments)
{
    const vmaf_sycl_fvif::Sigmas sigmas =
        vmaf_sycl_fvif::pixel_sigmas({.mu1 = moments.reference_mean,
                                      .mu2 = moments.distorted_mean,
                                      .xx = moments.reference_square,
                                      .yy = moments.distorted_square,
                                      .xy = moments.cross_product});
    const size_t index = item.get_global_id(0) * args.width + item.get_global_id(1);
    args.sigma1_sq[index] = sigmas.sigma1_sq;
    args.sigma2_sq[index] = sigmas.sigma2_sq;
    args.sigma12[index] = sigmas.sigma12;
}

} // namespace

namespace
{

static VifScratch make_vif_scratch(sycl::handler &handler)
{
    constexpr size_t tile_width = FVIF_BX + 2 * FVIF_MAX_HFW;
    const sycl::range<1> tile_range(tile_width * tile_width);
    const sycl::range<1> vertical_range((size_t)FVIF_BY * tile_width);
    return {.reference = VifLocal(tile_range, handler),
            .distorted = VifLocal(tile_range, handler),
            .vertical_reference_mean = VifLocal(vertical_range, handler),
            .vertical_distorted_mean = VifLocal(vertical_range, handler),
            .vertical_reference_square = VifLocal(vertical_range, handler),
            .vertical_distorted_square = VifLocal(vertical_range, handler),
            .vertical_cross_product = VifLocal(vertical_range, handler)};
}

} // namespace

namespace
{

template <int SCALE> constexpr int float_vif_grf_size()
{
    return (SCALE == 0) ? 256 : 0;
}

template <int SCALE>
class FloatVifComputeKernel : public VmafSyclKernelShape<32, float_vif_grf_size<SCALE>()>
{
  public:
    FloatVifComputeKernel(const VifComputeArgs &args, VifScratch scratch)
        : args_(args), scratch_(std::move(scratch))
    {
    }

    VMAF_SYCL_FUNCTOR_SG_SIZE(32) void operator()(sycl::nd_item<2> item) const
    {
        load_vif_tile<SCALE>(item, args_, scratch_);
        item.barrier(sycl::access::fence_space::local_space);
        filter_vif_vertical<SCALE>(item, args_.taps, scratch_);
        item.barrier(sycl::access::fence_space::local_space);
        if (item.get_global_id(1) < args_.width && item.get_global_id(0) < args_.height) {
            store_vif_sigmas(item, args_,
                             horizontal_vif_moments<SCALE>(item, args_.taps, scratch_));
        }
    }

  private:
    VifComputeArgs args_;
    VifScratch scratch_;
};

template <int SCALE>
static sycl::event launch_compute(sycl::queue &queue, const VifComputeArgs &args)
{
    const size_t global_x = ((size_t)args.width + FVIF_BX - 1) / FVIF_BX * FVIF_BX;
    const size_t global_y = ((size_t)args.height + FVIF_BY - 1) / FVIF_BY * FVIF_BY;
    return queue.submit([&](sycl::handler &handler) {
        const sycl::nd_range<2> range({global_y, global_x}, {FVIF_BY, FVIF_BX});
        const FloatVifComputeKernel<SCALE> kernel(args, make_vif_scratch(handler));
        handler.parallel_for(range, kernel);
    });
}

} // namespace

namespace
{

/* The rest of vif_pixel_statistic_s() for one pixel. The kernel is separate
 * from the filter because the statistic's 64-bit integer path does not fit
 * the filter kernel's registers next to its tile: together they spilled to
 * scratch memory, which this twin must not use (ADR-1395). One work-item per
 * pixel, no local memory, sub-group size 16 with the default register file. */
class FloatVifStatisticKernel : public VmafSyclKernelShape<16, 0>
{
  public:
    explicit FloatVifStatisticKernel(const VifStatisticArgs &args) : args_(args)
    {
    }

    VMAF_SYCL_FUNCTOR_SG_SIZE(16) void operator()(sycl::id<1> pixel) const
    {
        const size_t index = pixel[0];
        const vmaf_sycl_fvif::Term term =
            vmaf_sycl_fvif::pixel_statistic({.sigma1_sq = args_.sigma1_sq_then_numerator[index],
                                             .sigma2_sq = args_.sigma2_sq_then_denominator[index],
                                             .sigma12 = args_.sigma12[index]},
                                            args_.statistic);
        args_.sigma1_sq_then_numerator[index] = term.num;
        args_.sigma2_sq_then_denominator[index] = term.den;
    }

  private:
    VifStatisticArgs args_;
};

static sycl::event launch_vif_statistic_terms(sycl::queue &queue, const VifStatisticArgs &args,
                                              size_t pixels)
{
    return queue.submit([&](sycl::handler &handler) {
        handler.parallel_for(sycl::range<1>(pixels), FloatVifStatisticKernel(args));
    });
}

} // namespace

namespace
{

/* vif_statistic_s()'s inner loop for row `y`: the terms added left to right
 * into one fp32 accumulator per output. The order is the result; do not
 * split, stride or reduce these loops. Two scalar accumulators and four USM
 * pointers, so the kernel needs no scratch memory (ADR-1395). */
static inline void vif_row_sums(const VifRowSumArgs &args, size_t y)
{
    const float *numerator = args.numerator_terms + y * args.width;
    const float *denominator = args.denominator_terms + y * args.width;
    float numerator_sum = 0.0f;
    float denominator_sum = 0.0f;
    for (unsigned x = 0; x < args.width; x++) {
        numerator_sum += numerator[x];
        denominator_sum += denominator[x];
    }
    args.numerator_rows[y] = numerator_sum;
    args.denominator_rows[y] = denominator_sum;
}

/* One work-item per row, sub-group size 16 (the lanes of a hardware thread
 * are rows; ADR-1411 measured narrow sub-groups as the fastest for this
 * shape, and 16 is the narrowest every AOT target accepts, ADR-1468). */
static sycl::event launch_vif_row_sums(sycl::queue &queue, const VifRowSumArgs &args)
{
    return queue.submit([&](sycl::handler &handler) {
        handler.parallel_for(sycl::range<1>(args.height),
                             [=](sycl::id<1> row)
                                 VMAF_SYCL_REQD_SG_SIZE(16) { vif_row_sums(args, row[0]); });
    });
}

} // namespace

namespace
{

template <int SCALE>
static inline VifSamplePair read_decimate_pair(const VifDecimateArgs &args, int y, int x)
{
    if constexpr (SCALE == 1) {
        return {
            .reference = read_vif_raw_plane(args.reference_raw, args.raw_stride, args.bpc, y, x),
            .distorted = read_vif_raw_plane(args.distorted_raw, args.raw_stride, args.bpc, y, x)};
    }
    const size_t index = (size_t)y * args.float_stride + (size_t)x;
    return {.reference = args.reference_float[index], .distorted = args.distorted_float[index]};
}

template <int SCALE>
static inline VifSamplePair decimate_vif_pixel(const VifDecimateArgs &args, int x, int y)
{
    using F = VifFilterConstants<SCALE>;
    constexpr int width = F::width;
    constexpr int half_width = F::half_width;
    VifSamplePair result{};
#pragma unroll
    for (int horizontal_tap = 0; horizontal_tap < width; ++horizontal_tap) {
        const float horizontal_coefficient = args.taps.tap[horizontal_tap];
        const int source_x = vif_mirror(2 * x - half_width + horizontal_tap, (int)args.input_width);
        VifSamplePair vertical{};
#pragma unroll
        for (int vertical_tap = 0; vertical_tap < width; ++vertical_tap) {
            const float vertical_coefficient = args.taps.tap[vertical_tap];
            const int source_y =
                vif_mirror(2 * y - half_width + vertical_tap, (int)args.input_height);
            const VifSamplePair sample = read_decimate_pair<SCALE>(args, source_y, source_x);
            vertical.reference += vertical_coefficient * sample.reference;
            vertical.distorted += vertical_coefficient * sample.distorted;
        }
        result.reference += horizontal_coefficient * vertical.reference;
        result.distorted += horizontal_coefficient * vertical.distorted;
    }
    return result;
}

template <int SCALE> class FloatVifDecimateKernel
{
  public:
    explicit FloatVifDecimateKernel(const VifDecimateArgs &args) : args_(args)
    {
    }

    void operator()(sycl::nd_item<2> item) const
    {
        const size_t x = item.get_global_id(1);
        const size_t y = item.get_global_id(0);
        if (x >= args_.output_width || y >= args_.output_height) {
            return;
        }
        const VifSamplePair value = decimate_vif_pixel<SCALE>(args_, (int)x, (int)y);
        const size_t index = y * args_.output_stride + x;
        args_.reference_output[index] = value.reference;
        args_.distorted_output[index] = value.distorted;
    }

  private:
    VifDecimateArgs args_;
};

} // namespace

namespace
{

template <int SCALE>
static sycl::event launch_decimate(sycl::queue &queue, const VifDecimateArgs &args)
{
    const size_t global_x = ((size_t)args.output_width + FVIF_BX - 1) / FVIF_BX * FVIF_BX;
    const size_t global_y = ((size_t)args.output_height + FVIF_BY - 1) / FVIF_BY * FVIF_BY;
    return queue.submit([&](sycl::handler &handler) {
        const sycl::nd_range<2> range({global_y, global_x}, {FVIF_BY, FVIF_BX});
        const FloatVifDecimateKernel<SCALE> kernel(args);
        handler.parallel_for(range, kernel);
    });
}

} // namespace

namespace
{

template <typename T> static void copy_y_plane(VmafPicture *pic, void *dst, unsigned w, unsigned h)
{
    const T *src = static_cast<const T *>(pic->data[0]);
    T *out = static_cast<T *>(dst);
    const ptrdiff_t src_stride_t = pic->stride[0] / static_cast<ptrdiff_t>(sizeof(T));
    for (unsigned i = 0; i < h; i++) {
        for (unsigned j = 0; j < w; j++)
            out[j] = src[j];
        src += src_stride_t;
        out += w;
    }
}

} // namespace

namespace
{

static const VmafOption options_float_vif_sycl[] = {
    {.name = "debug",
     .help = "debug mode",
     .offset = offsetof(FloatVifStateSycl, debug),
     .type = VMAF_OPT_TYPE_BOOL,
     .default_val = {.b = false}},
    {.name = "vif_enhn_gain_limit",
     .help = "enhancement gain (>=1.0)",
     .alias = "egl",
     .offset = offsetof(FloatVifStateSycl, vif_enhn_gain_limit),
     .type = VMAF_OPT_TYPE_DOUBLE,
     .default_val = {.d = 100.0},
     .min = 1.0,
     .max = 100.0,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "vif_kernelscale",
     .help = "kernel scale",
     .alias = "ks",
     .offset = offsetof(FloatVifStateSycl, vif_kernelscale),
     .type = VMAF_OPT_TYPE_DOUBLE,
     .default_val = {.d = 1.0},
     .min = 0.1,
     .max = 4.0,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM | VMAF_OPT_FLAG_DEFAULT_ONLY},
    {.name = "vif_scale1_min_val",
     .help = "minimum value allowed; smaller values will be set to this value",
     .alias = "s1miv",
     .offset = offsetof(FloatVifStateSycl, vif_scale1_min_val),
     .type = VMAF_OPT_TYPE_DOUBLE,
     .default_val = {.d = 0.0},
     .min = 0.0,
     .max = 1.0,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "vif_scale2_min_val",
     .help = "minimum value allowed; smaller values will be set to this value",
     .alias = "s2miv",
     .offset = offsetof(FloatVifStateSycl, vif_scale2_min_val),
     .type = VMAF_OPT_TYPE_DOUBLE,
     .default_val = {.d = 0.0},
     .min = 0.0,
     .max = 1.0,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "vif_scale3_min_val",
     .help = "minimum value allowed; smaller values will be set to this value",
     .alias = "s3miv",
     .offset = offsetof(FloatVifStateSycl, vif_scale3_min_val),
     .type = VMAF_OPT_TYPE_DOUBLE,
     .default_val = {.d = 0.0},
     .min = 0.0,
     .max = 1.0,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "vif_sigma_nsq",
     .help = "neural noise variance",
     .alias = "snsq",
     .offset = offsetof(FloatVifStateSycl, vif_sigma_nsq),
     .type = VMAF_OPT_TYPE_DOUBLE,
     .default_val = {.d = 2.0},
     .min = 0.0,
     .max = 5.0,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "vif_skip_scale0",
     .help = "when set, skip scale 0 calculations",
     .alias = "ssclz",
     .offset = offsetof(FloatVifStateSycl, vif_skip_scale0),
     .type = VMAF_OPT_TYPE_BOOL,
     .default_val = {.b = false},
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = nullptr}};

} // namespace

namespace
{

static int configure_vif_state(FloatVifStateSycl &state, VmafSyclState *sycl_state, unsigned bpc,
                               unsigned width, unsigned height)
{
    state.width = width;
    state.height = height;
    state.bpc = bpc;
    state.has_pending = false;
    if (state.vif_kernelscale != 1.0 || sycl_state == nullptr) {
        return -EINVAL;
    }
    /* The four-scale ladder makes scale 3 the binding dimension floor. */
    const int minimum_dimension = vif_get_min_dim((float)state.vif_kernelscale);
    if (std::cmp_less(width, minimum_dimension) || std::cmp_less(height, minimum_dimension)) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "float_vif_sycl: width and height must be >= %d for the four-scale VIF "
                 "ladder (got %ux%u)\n",
                 minimum_dimension, width, height);
        return -EINVAL;
    }
    state.scale_w[0] = width;
    state.scale_h[0] = height;
    for (int scale = 1; scale < 4; ++scale) {
        state.scale_w[scale] = state.scale_w[scale - 1] / 2u;
        state.scale_h[scale] = state.scale_h[scale - 1] / 2u;
    }
    state.sycl_state = sycl_state;
    return 0;
}

} // namespace

namespace
{

static bool allocate_vif_planes(FloatVifStateSycl &state)
{
    const size_t bytes_per_pixel = state.bpc <= 8 ? 1u : 2u;
    const size_t raw_bytes = (size_t)state.width * state.height * bytes_per_pixel;
    state.h_ref_raw = vmaf_sycl_malloc_host(state.sycl_state, raw_bytes);
    state.h_dis_raw = vmaf_sycl_malloc_host(state.sycl_state, raw_bytes);
    state.d_ref_raw = vmaf_sycl_malloc_device(state.sycl_state, raw_bytes);
    state.d_dis_raw = vmaf_sycl_malloc_device(state.sycl_state, raw_bytes);
    const size_t float_bytes = (size_t)state.scale_w[1] * state.scale_h[1] * sizeof(float);
    for (int buffer = 0; buffer < 2; ++buffer) {
        state.d_ref_buf[buffer] =
            static_cast<float *>(vmaf_sycl_malloc_device(state.sycl_state, float_bytes));
        state.d_dis_buf[buffer] =
            static_cast<float *>(vmaf_sycl_malloc_device(state.sycl_state, float_bytes));
    }
    return state.h_ref_raw != nullptr && state.h_dis_raw != nullptr && state.d_ref_raw != nullptr &&
           state.d_dis_raw != nullptr && state.d_ref_buf[0] != nullptr &&
           state.d_dis_buf[0] != nullptr && state.d_ref_buf[1] != nullptr &&
           state.d_dis_buf[1] != nullptr;
}

} // namespace

namespace
{

static bool allocate_vif_sums(FloatVifStateSycl &state)
{
    const size_t term_bytes = (size_t)state.width * state.height * sizeof(float);
    state.d_num_terms = static_cast<float *>(vmaf_sycl_malloc_device(state.sycl_state, term_bytes));
    state.d_den_terms = static_cast<float *>(vmaf_sycl_malloc_device(state.sycl_state, term_bytes));
    state.d_sigma12 = static_cast<float *>(vmaf_sycl_malloc_device(state.sycl_state, term_bytes));
    size_t row_floats = 0;
    for (int scale = 0; scale < 4; ++scale) {
        state.row_offset[scale] = row_floats;
        row_floats += 2u * (size_t)state.scale_h[scale];
    }
    state.row_floats = row_floats;
    const size_t row_bytes = row_floats * sizeof(float);
    state.d_rows = static_cast<float *>(vmaf_sycl_malloc_device(state.sycl_state, row_bytes));
    state.h_rows = static_cast<float *>(vmaf_sycl_malloc_host(state.sycl_state, row_bytes));
    return state.d_num_terms != nullptr && state.d_den_terms != nullptr &&
           state.d_sigma12 != nullptr && state.d_rows != nullptr && state.h_rows != nullptr;
}

/* The four filters, as float_vif.c::init() derives them. */
static void init_vif_taps(FloatVifStateSycl &state)
{
    for (int scale = 0; scale < 4; ++scale) {
        state.taps[scale] = {};
        vif_get_filter(state.taps[scale].tap, scale, (float)state.vif_kernelscale);
    }
}

} // namespace

namespace
{

static int close_fex_sycl(VmafFeatureExtractor *fex);

static int init_fex_sycl(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                         unsigned width, unsigned height)
{
    (void)pix_fmt;
    auto &state = *static_cast<FloatVifStateSycl *>(fex->priv);
    const int configure_error = configure_vif_state(state, fex->sycl_state, bpc, width, height);
    if (configure_error != 0) {
        return configure_error;
    }
    if (!allocate_vif_planes(state) || !allocate_vif_sums(state)) {
        (void)close_fex_sycl(fex);
        return -ENOMEM;
    }
    init_vif_taps(state);
    state.feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, &state);
    if (!state.feature_name_dict) {
        (void)close_fex_sycl(fex);
        return -ENOMEM;
    }
    return 0;
}

} // namespace

namespace
{

/* Both luma planes into d_ref_raw / d_dis_raw, packed at the returned
 * stride: packed on the host and uploaded, or, for frames of the VMAFx API
 * on this device, copied on the device (ADR-2091; the frame's planes are
 * never read on the host). 0 when a device copy could not be enqueued. */
static unsigned upload_vif_pictures(FloatVifStateSycl &state, sycl::queue &queue,
                                    VmafPicture *reference, VmafPicture *distorted)
{
    const size_t bytes_per_pixel = state.bpc <= 8 ? 1u : 2u;
    const size_t row = (size_t)state.width * bytes_per_pixel;
    if (vmaf_sycl_picture_on_device(reference) || vmaf_sycl_picture_on_device(distorted)) {
        const bool copied = vmaf_sycl_picture_read_plane(reference, 0, &queue, state.d_ref_raw, row,
                                                         row, state.height, nullptr) == 0 &&
                            vmaf_sycl_picture_read_plane(distorted, 0, &queue, state.d_dis_raw, row,
                                                         row, state.height, nullptr) == 0;
        return copied ? (unsigned)row : 0u;
    }
    if (state.bpc <= 8) {
        copy_y_plane<uint8_t>(reference, state.h_ref_raw, state.width, state.height);
        copy_y_plane<uint8_t>(distorted, state.h_dis_raw, state.width, state.height);
    } else {
        copy_y_plane<uint16_t>(reference, state.h_ref_raw, state.width, state.height);
        copy_y_plane<uint16_t>(distorted, state.h_dis_raw, state.width, state.height);
    }
    const size_t raw_bytes = (size_t)state.width * state.height * bytes_per_pixel;
    queue.memcpy(state.d_ref_raw, state.h_ref_raw, raw_bytes);
    queue.memcpy(state.d_dis_raw, state.h_dis_raw, raw_bytes);
    return (unsigned)((size_t)state.width * bytes_per_pixel);
}

} // namespace

namespace
{

/* One scale's statistic: the filter kernel stores the variances, the statistic
 * kernel turns them into the two terms, the row kernel adds each row. `reference` / `distorted` are the float planes of the scale
 * (null at scale 0, which reads the raw planes). */
template <int SCALE>
static void launch_vif_statistic(const FloatVifStateSycl &state, sycl::queue &queue,
                                 unsigned raw_stride, const float *reference,
                                 const float *distorted,
                                 const vmaf_sycl_fvif::StatisticParams &statistic)
{
    constexpr int scale = SCALE;
    launch_compute<SCALE>(queue, {.reference_raw = state.d_ref_raw,
                                  .distorted_raw = state.d_dis_raw,
                                  .reference_float = reference,
                                  .distorted_float = distorted,
                                  .taps = state.taps[scale],
                                  .sigma1_sq = state.d_num_terms,
                                  .sigma2_sq = state.d_den_terms,
                                  .sigma12 = state.d_sigma12,
                                  .raw_stride = raw_stride,
                                  .float_stride = state.scale_w[scale],
                                  .width = state.scale_w[scale],
                                  .height = state.scale_h[scale],
                                  .bpc = state.bpc});
    launch_vif_statistic_terms(queue,
                               {.sigma1_sq_then_numerator = state.d_num_terms,
                                .sigma2_sq_then_denominator = state.d_den_terms,
                                .sigma12 = state.d_sigma12,
                                .statistic = statistic},
                               (size_t)state.scale_w[scale] * state.scale_h[scale]);
    float *rows = state.d_rows + state.row_offset[scale];
    launch_vif_row_sums(queue, {.numerator_terms = state.d_num_terms,
                                .denominator_terms = state.d_den_terms,
                                .numerator_rows = rows,
                                .denominator_rows = rows + state.scale_h[scale],
                                .width = state.scale_w[scale],
                                .height = state.scale_h[scale]});
}

} // namespace

namespace
{

template <int SCALE>
static void launch_vif_scaled(const FloatVifStateSycl &state, sycl::queue &queue,
                              unsigned raw_stride, const vmaf_sycl_fvif::StatisticParams &statistic)
{
    constexpr int scale = SCALE;
    const unsigned output_buffer = (scale - 1) % 2u;
    const bool input_is_raw = scale == 1;
    const unsigned input_buffer = output_buffer == 0 ? 1u : 0u;
    float *reference_output = state.d_ref_buf[output_buffer];
    float *distorted_output = state.d_dis_buf[output_buffer];
    launch_decimate<SCALE>(
        queue, {.reference_raw = state.d_ref_raw,
                .distorted_raw = state.d_dis_raw,
                .reference_float = input_is_raw ? nullptr : state.d_ref_buf[input_buffer],
                .distorted_float = input_is_raw ? nullptr : state.d_dis_buf[input_buffer],
                .taps = state.taps[scale],
                .reference_output = reference_output,
                .distorted_output = distorted_output,
                .raw_stride = raw_stride,
                .float_stride = input_is_raw ? 0u : state.scale_w[scale - 1],
                .output_stride = state.scale_w[scale],
                .output_width = state.scale_w[scale],
                .output_height = state.scale_h[scale],
                .input_width = state.scale_w[scale - 1],
                .input_height = state.scale_h[scale - 1],
                .bpc = state.bpc});
    launch_vif_statistic<SCALE>(state, queue, raw_stride, reference_output, distorted_output,
                                statistic);
}

} // namespace

namespace
{

static int submit_fex_sycl(VmafFeatureExtractor *fex, VmafPicture *reference,
                           VmafPicture *reference_rotated, VmafPicture *distorted,
                           VmafPicture *distorted_rotated, unsigned index)
{
    (void)reference_rotated;
    (void)distorted_rotated;
    auto &state = *static_cast<FloatVifStateSycl *>(fex->priv);
    auto *queue = static_cast<sycl::queue *>(vmaf_sycl_get_queue_ptr(state.sycl_state));
    if (queue == nullptr) {
        return -EINVAL;
    }
    const unsigned raw_stride = upload_vif_pictures(state, *queue, reference, distorted);
    if (raw_stride == 0u) {
        return -EIO;
    }
    const vmaf_sycl_fvif::StatisticParams statistic =
        vmaf_sycl_fvif::make_statistic_params(state.vif_sigma_nsq, state.vif_enhn_gain_limit);
    launch_vif_statistic<0>(state, *queue, raw_stride, nullptr, nullptr, statistic);
    launch_vif_scaled<1>(state, *queue, raw_stride, statistic);
    launch_vif_scaled<2>(state, *queue, raw_stride, statistic);
    launch_vif_scaled<3>(state, *queue, raw_stride, statistic);
    /* The only device-to-host copy of the frame; collect() waits on it. */
    queue->memcpy(state.h_rows, state.d_rows, state.row_floats * sizeof(float));
    state.pending_index = index;
    state.has_pending = true;
    return 0;
}

} // namespace

namespace
{

/* vif_statistic_s()'s outer loop: the row sums added top to bottom into one
 * fp32 accumulator per output. compute_vif() widens the two floats. */
static void sum_vif_rows(const FloatVifStateSycl &state, double *scores)
{
    for (int scale = 0; scale < 4; ++scale) {
        const float *numerator_rows = state.h_rows + state.row_offset[scale];
        const float *denominator_rows = numerator_rows + state.scale_h[scale];
        float numerator = 0.0f;
        float denominator = 0.0f;
        for (unsigned row = 0; row < state.scale_h[scale]; ++row) {
            numerator += numerator_rows[row];
            denominator += denominator_rows[row];
        }
        const size_t output = (size_t)scale * 2;
        scores[output] = (double)numerator;
        scores[output + 1] = (double)denominator;
    }
}

static int emit_vif_scores(const FloatVifStateSycl &state, const double scores[8], unsigned index,
                           VmafFeatureCollector *collector)
{
    VmafVifScoreSet output = {
        .minimum = {state.vif_scale1_min_val, state.vif_scale2_min_val, state.vif_scale3_min_val},
        .use_minimums = true,
        .skip_scale0 = state.vif_skip_scale0,
        .debug = state.debug,
    };
    const size_t start = state.vif_skip_scale0 ? 2u : 0u;
    for (size_t i = 0u; i < 8u; ++i) {
        output.scale[i] = scores[i];
        if (i >= start) {
            output.score_num += i % 2u == 0u ? scores[i] : 0.0;
            output.score_den += i % 2u != 0u ? scores[i] : 0.0;
        }
    }
    output.score = output.score_den > 0.0 ? output.score_num / output.score_den : NAN;
    return vmaf_vif_emit_scores(collector, state.feature_name_dict, "float_vif_sycl", &output,
                                VMAF_VIF_FLOAT_NAMES, index);
}

} // namespace

namespace
{

static int collect_fex_sycl(VmafFeatureExtractor *fex, unsigned index,
                            VmafFeatureCollector *feature_collector)
{
    auto &state = *static_cast<FloatVifStateSycl *>(fex->priv);
    auto *queue = static_cast<sycl::queue *>(vmaf_sycl_get_queue_ptr(state.sycl_state));
    if (queue == nullptr) {
        return -EINVAL;
    }
    queue->wait();
    double scores[8];
    sum_vif_rows(state, scores);
    return emit_vif_scores(state, scores, index, feature_collector);
}

} // namespace

namespace
{

static int close_fex_sycl(VmafFeatureExtractor *fex)
{
    auto *s = static_cast<FloatVifStateSycl *>(fex->priv);
    if (s->sycl_state) {
        if (s->h_ref_raw)
            vmaf_sycl_free(s->sycl_state, s->h_ref_raw);
        if (s->h_dis_raw)
            vmaf_sycl_free(s->sycl_state, s->h_dis_raw);
        if (s->d_ref_raw)
            vmaf_sycl_free(s->sycl_state, s->d_ref_raw);
        if (s->d_dis_raw)
            vmaf_sycl_free(s->sycl_state, s->d_dis_raw);
        for (int i = 0; i < 2; i++) {
            if (s->d_ref_buf[i])
                vmaf_sycl_free(s->sycl_state, s->d_ref_buf[i]);
            if (s->d_dis_buf[i])
                vmaf_sycl_free(s->sycl_state, s->d_dis_buf[i]);
        }
        float *const sums[] = {s->d_num_terms, s->d_den_terms, s->d_sigma12, s->d_rows, s->h_rows};
        for (float *buffer : sums) {
            if (buffer)
                vmaf_sycl_free(s->sycl_state, buffer);
        }
    }
    if (s->feature_name_dict)
        vmaf_dictionary_free(&s->feature_name_dict);
    return 0;
}

} // namespace

namespace
{

static const char *provided_features_float_vif_sycl[] = {"VMAF_feature_vif_scale0_score",
                                                         "VMAF_feature_vif_scale1_score",
                                                         "VMAF_feature_vif_scale2_score",
                                                         "VMAF_feature_vif_scale3_score",
                                                         "vif",
                                                         "vif_num",
                                                         "vif_den",
                                                         "vif_num_scale0",
                                                         "vif_den_scale0",
                                                         "vif_num_scale1",
                                                         "vif_den_scale1",
                                                         "vif_num_scale2",
                                                         "vif_den_scale2",
                                                         "vif_num_scale3",
                                                         "vif_den_scale3",
                                                         nullptr};

} // namespace

extern "C" VmafFeatureExtractor vmaf_fex_float_vif_sycl = {
    .name = "float_vif_sycl",
    .init = init_fex_sycl,
    .extract = nullptr,
    .flush = nullptr,
    .close = close_fex_sycl,
    .submit = submit_fex_sycl,
    .collect = collect_fex_sycl,
    .options = options_float_vif_sycl,
    .priv_size = sizeof(FloatVifStateSycl),
    .flags = VMAF_FEATURE_EXTRACTOR_SYCL,
    .provided_features = provided_features_float_vif_sycl,
};
