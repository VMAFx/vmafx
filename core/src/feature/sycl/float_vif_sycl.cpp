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
 *  Per-frame flow: 4 compute + 3 decimate launches. Self-contained
 *  submit/collect — does NOT register with vmaf_sycl_graph_register
 *  (the multi-scale layout doesn't fit the shared_frame model).
 */

#include <sycl/sycl.hpp>

#include "sycl_compat.h"

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstring>
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

} // namespace

namespace
{

struct FloatVifStateSycl {
    bool debug;
    double vif_enhn_gain_limit;
    double vif_kernelscale;
    double vif_sigma_nsq;
    bool vif_skip_scale0; /* host-side suppression: emit 0.0 for scale-0, mirrors float_vif.c */

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

    /* Per-scale (num, den) partials. */
    float *d_num[4];
    float *d_den[4];
    float *h_num[4];
    float *h_den[4];
    unsigned wg_count[4];

    bool has_pending;
    unsigned pending_index;

    VmafDictionary *feature_name_dict;
};

} // namespace

namespace
{

constexpr float FVIF_COEFF_S0[FVIF_MAX_FW] = {
    0.00745626912f, 0.0142655009f, 0.0250313189f, 0.0402820669f, 0.0594526194f, 0.0804751068f,
    0.0999041125f,  0.113746084f,  0.118773937f,  0.113746084f,  0.0999041125f, 0.0804751068f,
    0.0594526194f,  0.0402820669f, 0.0250313189f, 0.0142655009f, 0.00745626912f};
constexpr float FVIF_COEFF_S1[FVIF_MAX_FW] = {
    0.0189780835f, 0.0558981746f, 0.120920904f,  0.192116052f, 0.224173605f, 0.192116052f,
    0.120920904f,  0.0558981746f, 0.0189780835f, 0.0f,         0.0f,         0.0f,
    0.0f,          0.0f,          0.0f,          0.0f,         0.0f};
constexpr float FVIF_COEFF_S2[FVIF_MAX_FW] = {
    0.054488685f, 0.244201347f, 0.402619958f, 0.244201347f, 0.054488685f, 0.0f, 0.0f, 0.0f, 0.0f,
    0.0f,         0.0f,         0.0f,         0.0f,         0.0f,         0.0f, 0.0f, 0.0f};
constexpr float FVIF_COEFF_S3[FVIF_MAX_FW] = {
    0.166378498f, 0.667243004f, 0.166378498f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
    0.0f,         0.0f,         0.0f,         0.0f, 0.0f, 0.0f, 0.0f, 0.0f};

} // namespace

namespace
{

using VifLocal = sycl::local_accessor<float, 1>;

struct VifComputeArgs {
    const void *reference_raw;
    const void *distorted_raw;
    const float *reference_float;
    const float *distorted_float;
    float *numerator;
    float *denominator;
    unsigned raw_stride;
    unsigned float_stride;
    unsigned width;
    unsigned height;
    unsigned bpc;
    unsigned group_columns;
    float noise_variance;
    float gain_limit;
    float sigma_max_inverse;
    float coefficients[FVIF_MAX_FW];
};

struct VifScratch {
    VifLocal reference;
    VifLocal distorted;
    VifLocal vertical_reference_mean;
    VifLocal vertical_distorted_mean;
    VifLocal vertical_reference_square;
    VifLocal vertical_distorted_square;
    VifLocal vertical_cross_product;
    VifLocal numerator_subgroups;
    VifLocal denominator_subgroups;
};

struct VifMoments {
    float reference_mean;
    float distorted_mean;
    float reference_square;
    float distorted_square;
    float cross_product;
};

struct VifContribution {
    float numerator;
    float denominator;
};

} // namespace

namespace
{

struct VifDecimateArgs {
    const void *reference_raw;
    const void *distorted_raw;
    const float *reference_float;
    const float *distorted_float;
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
    float coefficients[FVIF_MAX_FW];
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
    constexpr int width = SCALE == 0 ? 17 : SCALE == 1 ? 9 : SCALE == 2 ? 5 : 3;
    constexpr int half_width = width / 2;
    constexpr size_t maximum_tile_width = FVIF_BX + 2 * FVIF_MAX_HFW;
    const int tile_width = FVIF_BX + 2 * half_width;
    const int tile_height = FVIF_BY + 2 * half_width;
    const int origin_y = (int)(item.get_group(0) * FVIF_BY) - half_width;
    const int origin_x = (int)(item.get_group(1) * FVIF_BX) - half_width;
    const int local = (int)(item.get_local_id(0) * FVIF_BX + item.get_local_id(1));
    for (int offset = local; offset < tile_height * tile_width; offset += FVIF_BX * FVIF_BY) {
        const int tile_y = offset / tile_width;
        const int tile_x = offset - tile_y * tile_width;
        const int y = vif_mirror(origin_y + tile_y, (int)args.height);
        const int x = vif_mirror(origin_x + tile_x, (int)args.width);
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
static inline VifMoments vertical_vif_moments(const VifComputeArgs &args, const VifScratch &scratch,
                                              int row, int column)
{
    constexpr int width = SCALE == 0 ? 17 : SCALE == 1 ? 9 : SCALE == 2 ? 5 : 3;
    constexpr int maximum_tile_width = FVIF_BX + 2 * FVIF_MAX_HFW;
    VifMoments moments{};
    for (int tap = 0; tap < width; ++tap) {
        const float coefficient = args.coefficients[tap];
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
static inline void filter_vif_vertical(sycl::nd_item<2> item, const VifComputeArgs &args,
                                       const VifScratch &scratch)
{
    constexpr int width = SCALE == 0 ? 17 : SCALE == 1 ? 9 : SCALE == 2 ? 5 : 3;
    constexpr int half_width = width / 2;
    constexpr int maximum_tile_width = FVIF_BX + 2 * FVIF_MAX_HFW;
    const int tile_width = FVIF_BX + 2 * half_width;
    const int local = (int)(item.get_local_id(0) * FVIF_BX + item.get_local_id(1));
    for (int offset = local; offset < FVIF_BY * tile_width; offset += FVIF_BX * FVIF_BY) {
        const int row = offset / tile_width;
        const int column = offset - row * tile_width;
        const VifMoments moments = vertical_vif_moments<SCALE>(args, scratch, row, column);
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
static inline VifMoments horizontal_vif_moments(sycl::nd_item<2> item, const VifComputeArgs &args,
                                                const VifScratch &scratch)
{
    constexpr int width = SCALE == 0 ? 17 : SCALE == 1 ? 9 : SCALE == 2 ? 5 : 3;
    constexpr int maximum_tile_width = FVIF_BX + 2 * FVIF_MAX_HFW;
    const size_t row = item.get_local_id(0) * maximum_tile_width;
    const size_t column = item.get_local_id(1);
    VifMoments moments{};
    for (int tap = 0; tap < width; ++tap) {
        const float coefficient = args.coefficients[tap];
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

static inline VifContribution vif_contribution(const VifComputeArgs &args,
                                               const VifMoments &moments)
{
    constexpr float epsilon = 1.0e-10f;
    float reference_variance =
        moments.reference_square - moments.reference_mean * moments.reference_mean;
    float distorted_variance =
        moments.distorted_square - moments.distorted_mean * moments.distorted_mean;
    const float covariance =
        moments.cross_product - moments.reference_mean * moments.distorted_mean;
    reference_variance = sycl::fmax(reference_variance, 0.0f);
    distorted_variance = sycl::fmax(distorted_variance, 0.0f);
    float gain = covariance / (reference_variance + epsilon);
    float residual_variance = distorted_variance - gain * covariance;
    if (reference_variance < epsilon) {
        gain = 0.0f;
        residual_variance = distorted_variance;
        reference_variance = 0.0f;
    }
    if (distorted_variance < epsilon) {
        gain = 0.0f;
        residual_variance = 0.0f;
    }
    if (gain < 0.0f) {
        residual_variance = distorted_variance;
        gain = 0.0f;
    }
    residual_variance = sycl::fmax(residual_variance, epsilon);
    gain = sycl::fmin(gain, args.gain_limit);
    VifContribution result = {
        .numerator = sycl::log2(1.0f + (gain * gain * reference_variance) /
                                           (residual_variance + args.noise_variance)),
        .denominator = sycl::log2(1.0f + reference_variance / args.noise_variance),
    };
    if (covariance < 0.0f) {
        result.numerator = 0.0f;
    }
    if (reference_variance < args.noise_variance) {
        result.numerator = 1.0f - distorted_variance * args.sigma_max_inverse;
        result.denominator = 1.0f;
    }
    return result;
}

} // namespace

namespace
{

static inline void reduce_vif_group(sycl::nd_item<2> item, const VifComputeArgs &args,
                                    const VifScratch &scratch, VifContribution value)
{
    sycl::sub_group const subgroup = item.get_sub_group();
    const float numerator = sycl::reduce_over_group(subgroup, value.numerator, sycl::plus<float>{});
    const float denominator =
        sycl::reduce_over_group(subgroup, value.denominator, sycl::plus<float>{});
    const uint32_t subgroup_id = subgroup.get_group_linear_id();
    if (subgroup.get_local_linear_id() == 0) {
        scratch.numerator_subgroups[subgroup_id] = numerator;
        scratch.denominator_subgroups[subgroup_id] = denominator;
    }
    item.barrier(sycl::access::fence_space::local_space);
    const size_t local = item.get_local_id(0) * FVIF_BX + item.get_local_id(1);
    if (local != 0) {
        return;
    }
    float numerator_total = 0.0f;
    float denominator_total = 0.0f;
    for (uint32_t group = 0; group < subgroup.get_group_linear_range(); ++group) {
        numerator_total += scratch.numerator_subgroups[group];
        denominator_total += scratch.denominator_subgroups[group];
    }
    const size_t index = item.get_group(0) * args.group_columns + item.get_group(1);
    args.numerator[index] = numerator_total;
    args.denominator[index] = denominator_total;
}

} // namespace

namespace
{

template <int SCALE> static constexpr const float *vif_coefficients()
{
    if constexpr (SCALE == 0) {
        return FVIF_COEFF_S0;
    }
    if constexpr (SCALE == 1) {
        return FVIF_COEFF_S1;
    }
    if constexpr (SCALE == 2) {
        return FVIF_COEFF_S2;
    }
    return FVIF_COEFF_S3;
}

} // namespace

namespace
{

static VifScratch make_vif_scratch(sycl::handler &handler)
{
    constexpr size_t tile_width = FVIF_BX + 2 * FVIF_MAX_HFW;
    const sycl::range<1> tile_range(tile_width * tile_width);
    const sycl::range<1> vertical_range((size_t)FVIF_BY * tile_width);
    const sycl::range<1> subgroup_range((size_t)FVIF_BX * FVIF_BY / 32);
    return {.reference = VifLocal(tile_range, handler),
            .distorted = VifLocal(tile_range, handler),
            .vertical_reference_mean = VifLocal(vertical_range, handler),
            .vertical_distorted_mean = VifLocal(vertical_range, handler),
            .vertical_reference_square = VifLocal(vertical_range, handler),
            .vertical_distorted_square = VifLocal(vertical_range, handler),
            .vertical_cross_product = VifLocal(vertical_range, handler),
            .numerator_subgroups = VifLocal(subgroup_range, handler),
            .denominator_subgroups = VifLocal(subgroup_range, handler)};
}

} // namespace

namespace
{

template <int SCALE>
static sycl::event
launch_compute(sycl::queue &queue, const void *reference_raw, const void *distorted_raw,
               unsigned raw_stride, const float *reference_float, const float *distorted_float,
               unsigned float_stride, float *numerator, float *denominator, unsigned width,
               unsigned height, unsigned bpc, unsigned group_columns, float noise_variance,
               float gain_limit, float sigma_max_inverse)
{
    VifComputeArgs args = {.reference_raw = reference_raw,
                           .distorted_raw = distorted_raw,
                           .reference_float = reference_float,
                           .distorted_float = distorted_float,
                           .numerator = numerator,
                           .denominator = denominator,
                           .raw_stride = raw_stride,
                           .float_stride = float_stride,
                           .width = width,
                           .height = height,
                           .bpc = bpc,
                           .group_columns = group_columns,
                           .noise_variance = noise_variance,
                           .gain_limit = gain_limit,
                           .sigma_max_inverse = sigma_max_inverse,
                           .coefficients = {}};
    const float *coefficients = vif_coefficients<SCALE>();
    for (int tap = 0; tap < FVIF_MAX_FW; ++tap) {
        args.coefficients[tap] = coefficients[tap];
    }
    const size_t global_x = ((size_t)width + FVIF_BX - 1) / FVIF_BX * FVIF_BX;
    const size_t global_y = ((size_t)height + FVIF_BY - 1) / FVIF_BY * FVIF_BY;
    return queue.submit([&](sycl::handler &handler) {
        const VifScratch scratch = make_vif_scratch(handler);
        const sycl::nd_range<2> range({global_y, global_x}, {FVIF_BY, FVIF_BX});
        handler.parallel_for(range, [=](sycl::nd_item<2> item) VMAF_SYCL_REQD_SG_SIZE(32) {
            load_vif_tile<SCALE>(item, args, scratch);
            item.barrier(sycl::access::fence_space::local_space);
            filter_vif_vertical<SCALE>(item, args, scratch);
            item.barrier(sycl::access::fence_space::local_space);
            VifContribution value{};
            if (item.get_global_id(1) < args.width && item.get_global_id(0) < args.height) {
                value = vif_contribution(args, horizontal_vif_moments<SCALE>(item, args, scratch));
            }
            reduce_vif_group(item, args, scratch, value);
        });
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
    constexpr int width = SCALE == 1 ? 9 : SCALE == 2 ? 5 : 3;
    constexpr int half_width = width / 2;
    VifSamplePair result{};
    for (int horizontal_tap = 0; horizontal_tap < width; ++horizontal_tap) {
        const float horizontal_coefficient = args.coefficients[horizontal_tap];
        const int source_x = vif_mirror(2 * x - half_width + horizontal_tap, (int)args.input_width);
        VifSamplePair vertical{};
        for (int vertical_tap = 0; vertical_tap < width; ++vertical_tap) {
            const float vertical_coefficient = args.coefficients[vertical_tap];
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

} // namespace

namespace
{

template <int SCALE>
static sycl::event
launch_decimate(sycl::queue &queue, const void *reference_raw, const void *distorted_raw,
                unsigned raw_stride, const float *reference_float, const float *distorted_float,
                unsigned float_stride, float *reference_output, float *distorted_output,
                unsigned output_stride, unsigned output_width, unsigned output_height,
                unsigned input_width, unsigned input_height, unsigned bpc)
{
    VifDecimateArgs args = {.reference_raw = reference_raw,
                            .distorted_raw = distorted_raw,
                            .reference_float = reference_float,
                            .distorted_float = distorted_float,
                            .reference_output = reference_output,
                            .distorted_output = distorted_output,
                            .raw_stride = raw_stride,
                            .float_stride = float_stride,
                            .output_stride = output_stride,
                            .output_width = output_width,
                            .output_height = output_height,
                            .input_width = input_width,
                            .input_height = input_height,
                            .bpc = bpc,
                            .coefficients = {}};
    const float *coefficients = vif_coefficients<SCALE>();
    for (int tap = 0; tap < FVIF_MAX_FW; ++tap) {
        args.coefficients[tap] = coefficients[tap];
    }
    const size_t global_x = ((size_t)output_width + FVIF_BX - 1) / FVIF_BX * FVIF_BX;
    const size_t global_y = ((size_t)output_height + FVIF_BY - 1) / FVIF_BY * FVIF_BY;
    return queue.submit([&](sycl::handler &handler) {
        const sycl::nd_range<2> range({global_y, global_x}, {FVIF_BY, FVIF_BX});
        handler.parallel_for(range, [=](sycl::nd_item<2> item) {
            const size_t x = item.get_global_id(1);
            const size_t y = item.get_global_id(0);
            if (x >= args.output_width || y >= args.output_height) {
                return;
            }
            const VifSamplePair value = decimate_vif_pixel<SCALE>(args, (int)x, (int)y);
            const size_t index = y * args.output_stride + x;
            args.reference_output[index] = value.reference;
            args.distorted_output[index] = value.distorted;
        });
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

static bool allocate_vif_partials(FloatVifStateSycl &state)
{
    bool allocated = true;
    for (int scale = 0; scale < 4; ++scale) {
        const unsigned columns = (state.scale_w[scale] + FVIF_BX - 1u) / FVIF_BX;
        const unsigned rows = (state.scale_h[scale] + FVIF_BY - 1u) / FVIF_BY;
        state.wg_count[scale] = columns * rows;
        const size_t bytes = (size_t)state.wg_count[scale] * sizeof(float);
        state.d_num[scale] = static_cast<float *>(vmaf_sycl_malloc_device(state.sycl_state, bytes));
        state.d_den[scale] = static_cast<float *>(vmaf_sycl_malloc_device(state.sycl_state, bytes));
        state.h_num[scale] = static_cast<float *>(vmaf_sycl_malloc_host(state.sycl_state, bytes));
        state.h_den[scale] = static_cast<float *>(vmaf_sycl_malloc_host(state.sycl_state, bytes));
        allocated = allocated && state.d_num[scale] != nullptr && state.d_den[scale] != nullptr &&
                    state.h_num[scale] != nullptr && state.h_den[scale] != nullptr;
    }
    return allocated;
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
    if (!allocate_vif_planes(state) || !allocate_vif_partials(state)) {
        (void)close_fex_sycl(fex);
        return -ENOMEM;
    }
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

static unsigned upload_vif_pictures(FloatVifStateSycl &state, sycl::queue &queue,
                                    VmafPicture *reference, VmafPicture *distorted)
{
    const size_t bytes_per_pixel = state.bpc <= 8 ? 1u : 2u;
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

static void reset_vif_partials(FloatVifStateSycl &state, sycl::queue &queue)
{
    for (int scale = 0; scale < 4; ++scale) {
        const size_t bytes = (size_t)state.wg_count[scale] * sizeof(float);
        queue.memset(state.d_num[scale], 0, bytes);
        queue.memset(state.d_den[scale], 0, bytes);
    }
}

} // namespace

namespace
{

static void launch_vif_scale_zero(const FloatVifStateSycl &state, sycl::queue &queue,
                                  unsigned raw_stride, float noise_variance, float gain_limit,
                                  float sigma_max_inverse)
{
    const unsigned columns = (state.scale_w[0] + FVIF_BX - 1u) / FVIF_BX;
    launch_compute<0>(queue, state.d_ref_raw, state.d_dis_raw, raw_stride, nullptr, nullptr,
                      state.scale_w[0], state.d_num[0], state.d_den[0], state.scale_w[0],
                      state.scale_h[0], state.bpc, columns, noise_variance, gain_limit,
                      sigma_max_inverse);
}

} // namespace

namespace
{

template <int SCALE>
static void launch_vif_scaled(const FloatVifStateSycl &state, sycl::queue &queue,
                              unsigned raw_stride, float noise_variance, float gain_limit,
                              float sigma_max_inverse)
{
    constexpr int scale = SCALE;
    const unsigned output_buffer = (scale - 1) % 2u;
    const bool input_is_raw = scale == 1;
    const unsigned input_buffer = output_buffer == 0 ? 1u : 0u;
    const float *reference_input = input_is_raw ? nullptr : state.d_ref_buf[input_buffer];
    const float *distorted_input = input_is_raw ? nullptr : state.d_dis_buf[input_buffer];
    const unsigned input_stride = input_is_raw ? 0u : state.scale_w[scale - 1];
    float *reference_output = state.d_ref_buf[output_buffer];
    float *distorted_output = state.d_dis_buf[output_buffer];
    launch_decimate<scale>(queue, state.d_ref_raw, state.d_dis_raw, raw_stride, reference_input,
                           distorted_input, input_stride, reference_output, distorted_output,
                           state.scale_w[scale], state.scale_w[scale], state.scale_h[scale],
                           state.scale_w[scale - 1], state.scale_h[scale - 1], state.bpc);
    const unsigned columns = (state.scale_w[scale] + FVIF_BX - 1u) / FVIF_BX;
    launch_compute<scale>(queue, nullptr, nullptr, 0, reference_output, distorted_output,
                          state.scale_w[scale], state.d_num[scale], state.d_den[scale],
                          state.scale_w[scale], state.scale_h[scale], state.bpc, columns,
                          noise_variance, gain_limit, sigma_max_inverse);
}

static void launch_vif_scaled_ladder(const FloatVifStateSycl &state, sycl::queue &queue,
                                     unsigned raw_stride, float noise_variance, float gain_limit,
                                     float sigma_max_inverse)
{
    launch_vif_scaled<1>(state, queue, raw_stride, noise_variance, gain_limit, sigma_max_inverse);
    launch_vif_scaled<2>(state, queue, raw_stride, noise_variance, gain_limit, sigma_max_inverse);
    launch_vif_scaled<3>(state, queue, raw_stride, noise_variance, gain_limit, sigma_max_inverse);
}

} // namespace

namespace
{

static void download_vif_partials(FloatVifStateSycl &state, sycl::queue &queue)
{
    for (int scale = 0; scale < 4; ++scale) {
        const size_t bytes = (size_t)state.wg_count[scale] * sizeof(float);
        queue.memcpy(state.h_num[scale], state.d_num[scale], bytes);
        queue.memcpy(state.h_den[scale], state.d_den[scale], bytes);
    }
}

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
    reset_vif_partials(state, *queue);
    const float noise_variance = (float)state.vif_sigma_nsq;
    const float gain_limit = (float)state.vif_enhn_gain_limit;
    const float sigma_max_inverse =
        (float)(std::pow((float)state.vif_sigma_nsq, 2.0f) / (255.0 * 255.0));
    launch_vif_scale_zero(state, *queue, raw_stride, noise_variance, gain_limit, sigma_max_inverse);
    launch_vif_scaled_ladder(state, *queue, raw_stride, noise_variance, gain_limit,
                             sigma_max_inverse);
    download_vif_partials(state, *queue);
    state.pending_index = index;
    state.has_pending = true;
    return 0;
}

} // namespace

namespace
{

static void sum_vif_partials(const FloatVifStateSycl &state, double *scores)
{
    for (int scale = 0; scale < 4; ++scale) {
        double numerator = 0.0;
        double denominator = 0.0;
        for (unsigned group = 0; group < state.wg_count[scale]; ++group) {
            numerator += (double)state.h_num[scale][group];
            denominator += (double)state.h_den[scale][group];
        }
        const size_t output = (size_t)scale * 2;
        scores[output] = numerator;
        scores[output + 1] = denominator;
    }
}

static int emit_vif_scores(const FloatVifStateSycl &state, const double scores[8], unsigned index,
                           VmafFeatureCollector *collector)
{
    VmafVifScoreSet output = {
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
    sum_vif_partials(state, scores);
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
        for (int i = 0; i < 4; i++) {
            if (s->d_num[i])
                vmaf_sycl_free(s->sycl_state, s->d_num[i]);
            if (s->d_den[i])
                vmaf_sycl_free(s->sycl_state, s->d_den[i]);
            if (s->h_num[i])
                vmaf_sycl_free(s->sycl_state, s->h_num[i]);
            if (s->h_den[i])
                vmaf_sycl_free(s->sycl_state, s->h_den[i]);
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
