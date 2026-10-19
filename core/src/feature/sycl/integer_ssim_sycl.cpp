/* Upstream-mirror filename: defines float_ssim symbol despite the integer_ prefix (matches Netflix upstream). See ADR-0549. */
/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright (c) 2011, Tom Distler (http://tdistler.com)
 *  Copyright 2001-2012 Xiph.Org and contributors.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent AND BSD-3-Clause AND BSD-2-Clause
 *
 *  float_ssim feature extractor on the SYCL backend
 *  (T7-23 / ADR-0188 / ADR-0189, GPU long-tail batch 2 part 1c).
 *  SYCL twin of ssim_vulkan (PR #139) and ssim_cuda (this PR's
 *  batch 2 part 1b).
 *
 *  Self-contained submit / collect — does *not* register with
 *  vmaf_sycl_graph_register (see core/src/feature/sycl/AGENTS.md).
 *  Same approach as ciede_sycl (PR #137).
 *
 *  Per frame (ADR-1370), all on the device, one queue wait in collect:
 *    0. the raw luma samples (uint8 or uint16, packed) go up in one DMA
 *       per plane; launch_decimate() applies picture_copy()'s
 *       normalisation and ssim.c's scale x scale box low-pass +
 *       decimation (iqa_decimate(), KBND_SYMMETRIC edges) with the
 *       CPU's rounding: every product is the CPU's fp32 product and
 *       the CPU's exact double sum is reproduced in int64 fixed point,
 *       so the decimated planes are bit-identical to the CPU's. Scale 1
 *       is the plain picture_copy() conversion.
 *    1. horizontal 11-tap separable Gaussian over ref / cmp /
 *       ref² / cmp² / ref·cmp into 5 device float buffers.
 *       SLM-staged (SY-2, ADR-0458): 26-float tile per WG row
 *       eliminates redundant global-memory reads across neighbours.
 *    2. vertical 11-tap + the CPU's per-window terms, one work-item per
 *       window and no reduction on the device (ADR-1463): the kernel
 *       stores the fp64 bit pattern of `lv * cv * sv` at the window's
 *       raster position.
 *
 *  The host adds the plane of terms into one `double` in raster order, as
 *  iqa/ssim_tools.c does, divides by (W'-10)·(H'-10) over the decimated
 *  W' x H' and emits `float_ssim`. The sum is the CPU's because the terms
 *  and the order are: a sum in another order rounds elsewhere, and on a
 *  frame whose terms cancel that moved the fp32 mean by one step.
 *
 *  Options mirror CPU float_ssim.c. `enable_lcs` switches pass 2 to a
 *  variant that stores the per-window luminance and contrast doubles and
 *  the fp32 structure of iqa/ssim_tools.c (clamped variances, flat-region
 *  covariance clamp); the host forms `lv * cv * sv` and the four sums of
 *  ssim_accumulate_lane() and emits `float_ssim_{l,c,s}` as well.
 *  `enable_db` / `clip_db` act on the host through the shared
 *  nonfinite_score.h SSIM helpers.
 *
 *  `scale` resolves as in ssim.c::compute_ssim (0 = auto from the short
 *  side). The ADR-1324 context check refuses only a geometry the device
 *  cannot compute exactly (decimated plane under the 11x11 Gaussian, or a
 *  scale past SSIM_MAX_EXACT_SCALE); model dispatch and `--feature` then
 *  run the CPU float_ssim, direct requests keep the -EINVAL init error.
 *  fp64-free (Intel Arc A380 lacks native fp64).
 */

#include <sycl/sycl.hpp>

#include <bit>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

#include "config.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature_name.h"
#include "feature/nonfinite_score.h"
#include "log.h"
#include "picture.h"
#include "../iqa/decimate_dim.h"
#include "sycl/common.h"
#include "sycl_compat.h"
#include "sycl_exact_fp.h"
#include "sycl_integer_ssim_math.h"
#include "sycl_ssim_terms.h"

namespace
{

constexpr size_t SSIM_WG_X = 16;
static constexpr size_t SSIM_WG_Y = 8;
static constexpr int SSIM_K = 11;

/* ADR-1370 fixed point of the decimation sum. ssim.c's low-pass tap is
 * 1.0f / (scale * scale) and every sample is a multiple of 2^-8 (16-bit
 * picture_copy()), so each fp32 product is a multiple of 2^-52 up to
 * scale 128, and a whole window sums below 2^8.001: the int64 sum in units
 * of 2^-52 is exact, as is the CPU's double sum over that range.
 * Converting it to fp32 once, round-to-nearest-even, therefore gives the
 * CPU's (float)sum bit for bit. Past scale 128 (short side above 32767
 * px) the CPU's double sum stops being exact; the context check refuses. */
constexpr float SSIM_DECIMATE_FIXED_ONE = 0x1p52f;
constexpr float SSIM_DECIMATE_FIXED_INV = 0x1p-52f;
constexpr int SSIM_MAX_EXACT_SCALE = 128;

/* Same 11-tap normalised Gaussian as the Vulkan + CUDA twins —
 * matches g_gaussian_window_h in iqa/ssim_tools.h byte-for-byte. */
constexpr float G[SSIM_K] = {
    0.001028f, 0.007599f, 0.036001f, 0.109361f, 0.213006f, 0.266012f,
    0.213006f, 0.109361f, 0.036001f, 0.007599f, 0.001028f,
};

using vmaf_sycl_ssim::accumulate_window;
using vmaf_sycl_ssim::add_horizontal_tap;
using vmaf_sycl_ssim::add_vertical_tap;
using vmaf_sycl_ssim::float_ssim_constants;
using vmaf_sycl_ssim::MomentPairs;
using vmaf_sycl_ssim::round_moments;
using vmaf_sycl_ssim::ssim_double_terms;
using vmaf_sycl_ssim::ssim_float_parts;
using vmaf_sycl_ssim::ssim_frame_sums;
using vmaf_sycl_ssim::ssim_product_bits;
using vmaf_sycl_ssim::SsimDoubleTerms;
using vmaf_sycl_ssim::SsimFrameSums;
using vmaf_sycl_ssim::SsimMoments;

} // namespace

namespace
{

struct SsimStateSycl {
    /* Frame geometry. */
    unsigned width;
    unsigned height;
    unsigned bpc;
    int scale_override;
    bool enable_lcs;
    bool enable_db;
    bool clip_db;
    /* vmaf_ssim_max_db(): +inf unless clip_db. */
    double max_db;

    /* ADR-1370 decimation: resolved scale, the decimated plane the SSIM
     * passes run on (== width x height at scale 1), and the raw-sample
     * layout picture_copy() reads (1 or 2 bytes, multiplied by
     * sample_scale = 1 / its divisor). tap_weight = ssim.c's low-pass tap. */
    int scale;
    unsigned dec_width;
    unsigned dec_height;
    unsigned sample_bytes;
    float sample_scale;
    float tap_weight;

    unsigned w_horiz;
    unsigned h_horiz;
    unsigned w_final;
    unsigned h_final;

    float c1;
    float c2;

    /* SYCL state back-pointer. */
    VmafSyclState *sycl_state;

    /* Host-pinned packed raw luma staging and its device copy. */
    void *h_ref_raw;
    void *h_cmp_raw;
    void *d_ref_raw;
    void *d_cmp_raw;
    /* Device USM decimated float ref / cmp + 5 intermediates. */
    float *d_ref;
    float *d_cmp;
    float *d_ref_mu;
    float *d_cmp_mu;
    float *d_ref_sq;
    float *d_cmp_sq;
    float *d_refcmp;
    /* fp64 bit patterns per window in raster order, and their pinned host
     * copy: w_final * h_final SSIM terms, or under enable_lcs twice that,
     * the luminance terms followed by the contrast terms. */
    std::uint64_t *d_terms;
    std::uint64_t *h_terms;
    /* enable_lcs only: the fp32 structure term per window; NULL otherwise. */
    float *d_structure;
    float *h_structure;

    bool has_pending;
    unsigned pending_index;

    VmafDictionary *feature_name_dict;
};

} // namespace

/* Tile width for the horizontal SLM staging (SY-2, ADR-0458):
 * each WG of SSIM_WG_X columns needs SSIM_WG_X + (SSIM_K-1) input
 * floats per row to cover the 11-tap apron.  Two SLM arrays (ref,
 * cmp) of size SSIM_WG_Y * SSIM_TILE_W carry all needed input pixels;
 * the five output channels are computed from SLM with no extra arrays.
 * This eliminates 11 global-memory loads per output channel per pixel
 * (total 55 → 26 loads per pixel pair on Arc A380). */
namespace
{

constexpr size_t SSIM_TILE_W = SSIM_WG_X + (size_t)(SSIM_K - 1); /* 26 */

struct FloatHorizArgs {
    const float *reference;
    const float *comparison;
    float *reference_mean;
    float *comparison_mean;
    float *reference_square;
    float *comparison_square;
    float *cross_product;
    unsigned width;
    unsigned output_width;
    unsigned output_height;
};

struct FloatVertArgs {
    const float *reference_mean;
    const float *comparison_mean;
    const float *reference_square;
    const float *comparison_square;
    const float *cross_product;
    /* fp64 bit patterns per window, raster order: the SSIM term, or in the
     * enable_lcs kernel L in [0, windows) and C in [windows, 2 * windows). */
    std::uint64_t *terms;
    /* enable_lcs kernel only: the fp32 S per window. */
    float *structure;
    unsigned horizontal_width;
    unsigned final_width;
    unsigned final_height;
    float c1;
    float c2;
};

} // namespace

namespace
{

/* ADR-1370: everything launch_decimate() reads, captured by value. */
struct FloatDecimateArgs {
    const void *reference;
    const void *comparison;
    float *reference_out;
    float *comparison_out;
    unsigned width;
    unsigned height;
    unsigned output_width;
    unsigned output_height;
    int scale;
    float sample_scale;
    float tap_weight;
};

/* iqa/convolve.c::KBND_SYMMETRIC: period-2n mirror, edge sample repeated.
 * Identity inside the plane, so it also covers iqa_filter_pixel()'s
 * direct-read interior path. */
static inline int symmetric_index(int position, int extent)
{
    const int period = 2 * extent;
    int folded = position % period;
    if (folded < 0) {
        folded += period;
    }
    return folded >= extent ? period - folded - 1 : folded;
}

/* fp32 product -> integer units of 2^-52; exact (SSIM_DECIMATE_FIXED_ONE). */
static inline std::int64_t decimate_fixed(float product)
{
    const float scaled = product * SSIM_DECIMATE_FIXED_ONE;
    return static_cast<std::int64_t>(scaled);
}

} // namespace

namespace
{

/* One output of iqa_decimate() with ssim.c's low-pass kernel: the
 * picture_copy() value times the tap in fp32 (the CPU's `prod`), summed
 * exactly and rounded to fp32 once, as the CPU's `(float)(double sum)`.
 * Row r of the window is offset r - scale / 2, iqa_filter_pixel()'s
 * -vc .. vc - kh_even for odd and even scales alike. */
template <typename T>
static inline float decimate_sample(const T *plane, const FloatDecimateArgs &args, int centre_x,
                                    int centre_y)
{
    const int half = args.scale / 2;
    std::int64_t sum = 0;
    for (int row = 0; row < args.scale; ++row) {
        const int source_y = symmetric_index(centre_y + row - half, (int)args.height);
        const size_t row_offset = (size_t)source_y * args.width;
        for (int column = 0; column < args.scale; ++column) {
            const int source_x = symmetric_index(centre_x + column - half, (int)args.width);
            const float sample = (float)plane[row_offset + (size_t)source_x] * args.sample_scale;
            const float product = sample * args.tap_weight;
            sum += decimate_fixed(product);
        }
    }
    const sycl::vec<std::int64_t, 1> exact{sum};
    const float rounded = exact.convert<float, sycl::rounding_mode::rte>()[0];
    return rounded * SSIM_DECIMATE_FIXED_INV;
}

} // namespace

namespace
{

/* Both planes in one launch. The windows tile the plane without overlap
 * (stride == window), so each sample is read by one work-item: there is
 * no tap reuse for an SLM tile to exploit (SY-2 targets overlapping
 * stencils). */
template <typename T>
static void launch_decimate_typed(sycl::queue &queue, const FloatDecimateArgs &args)
{
    const size_t global_x = ((args.output_width + SSIM_WG_X - 1) / SSIM_WG_X) * SSIM_WG_X;
    const size_t global_y = ((args.output_height + SSIM_WG_Y - 1) / SSIM_WG_Y) * SSIM_WG_Y;
    sycl::nd_range<2> const range{sycl::range<2>{global_y, global_x},
                                  sycl::range<2>{SSIM_WG_Y, SSIM_WG_X}};
    queue.submit([=](sycl::handler &handler) {
        handler.parallel_for(range, [=](sycl::nd_item<2> item) {
            const size_t x = item.get_global_id(1);
            const size_t y = item.get_global_id(0);
            if (x >= args.output_width || y >= args.output_height) {
                return;
            }
            const int centre_x = (int)x * args.scale;
            const int centre_y = (int)y * args.scale;
            const size_t index = y * args.output_width + x;
            args.reference_out[index] =
                decimate_sample(static_cast<const T *>(args.reference), args, centre_x, centre_y);
            args.comparison_out[index] =
                decimate_sample(static_cast<const T *>(args.comparison), args, centre_x, centre_y);
        });
    });
}

static void launch_decimate(sycl::queue &queue, const FloatDecimateArgs &args,
                            unsigned sample_bytes)
{
    if (sample_bytes == 2U) {
        launch_decimate_typed<std::uint16_t>(queue, args);
    } else {
        launch_decimate_typed<std::uint8_t>(queue, args);
    }
}

} // namespace

namespace
{

static inline void load_float_tile(sycl::nd_item<2> item, const FloatHorizArgs &args,
                                   const sycl::local_accessor<float, 1> &reference,
                                   const sycl::local_accessor<float, 1> &comparison)
{
    const size_t local = item.get_local_id(0) * SSIM_WG_X + item.get_local_id(1);
    const size_t origin_x = item.get_group(1) * SSIM_WG_X;
    const size_t origin_y = item.get_group(0) * SSIM_WG_Y;
    const size_t group_size = SSIM_WG_X * SSIM_WG_Y;
    for (size_t offset = local; offset < SSIM_WG_Y * SSIM_TILE_W; offset += group_size) {
        const size_t y = origin_y + offset / SSIM_TILE_W;
        const size_t x = origin_x + offset % SSIM_TILE_W;
        if (y < args.output_height && x < args.width) {
            const size_t index = y * args.width + x;
            reference[offset] = args.reference[index];
            comparison[offset] = args.comparison[index];
        } else {
            reference[offset] = 0.0f;
            comparison[offset] = 0.0f;
        }
    }
}

} // namespace

namespace
{

static inline SsimMoments horizontal_moments(size_t local_x, size_t local_y,
                                             const sycl::local_accessor<float, 1> &reference,
                                             const sycl::local_accessor<float, 1> &comparison)
{
    MomentPairs sums{};
    for (int tap = 0; tap < SSIM_K; ++tap) {
        const size_t index = local_y * SSIM_TILE_W + local_x + (size_t)tap;
        add_horizontal_tap(sums, reference[index], comparison[index], G[tap]);
    }
    return round_moments(sums);
}

} // namespace

namespace
{

static inline void store_horizontal_moments(sycl::nd_item<2> item, const FloatHorizArgs &args,
                                            const sycl::local_accessor<float, 1> &reference,
                                            const sycl::local_accessor<float, 1> &comparison)
{
    const size_t x = item.get_global_id(1);
    const size_t y = item.get_global_id(0);
    if (x >= args.output_width || y >= args.output_height) {
        return;
    }
    const SsimMoments moments =
        horizontal_moments(item.get_local_id(1), item.get_local_id(0), reference, comparison);
    const size_t index = y * args.output_width + x;
    args.reference_mean[index] = moments.reference_mean;
    args.comparison_mean[index] = moments.comparison_mean;
    args.reference_square[index] = moments.reference_square;
    args.comparison_square[index] = moments.comparison_square;
    args.cross_product[index] = moments.cross_product;
}

} // namespace

namespace
{

static void launch_horiz(sycl::queue &queue, const FloatHorizArgs &args)
{
    const size_t global_x = ((args.output_width + SSIM_WG_X - 1) / SSIM_WG_X) * SSIM_WG_X;
    const size_t global_y = ((args.output_height + SSIM_WG_Y - 1) / SSIM_WG_Y) * SSIM_WG_Y;
    sycl::nd_range<2> const range{sycl::range<2>{global_y, global_x},
                                  sycl::range<2>{SSIM_WG_Y, SSIM_WG_X}};
    queue.submit([&](sycl::handler &handler) {
        sycl::local_accessor<float, 1> const reference(sycl::range<1>(SSIM_WG_Y * SSIM_TILE_W),
                                                       handler);
        sycl::local_accessor<float, 1> const comparison(sycl::range<1>(SSIM_WG_Y * SSIM_TILE_W),
                                                        handler);
        handler.parallel_for(range, [=](sycl::nd_item<2> item) {
            load_float_tile(item, args, reference, comparison);
            item.barrier(sycl::access::fence_space::local_space);
            store_horizontal_moments(item, args, reference, comparison);
        });
    });
}

} // namespace

namespace
{

static inline SsimMoments vertical_moments(const FloatVertArgs &args, size_t x, size_t y)
{
    MomentPairs sums{};
    for (int tap = 0; tap < SSIM_K; ++tap) {
        const size_t index = (y + (size_t)tap) * args.horizontal_width + x;
        const SsimMoments row = {.reference_mean = args.reference_mean[index],
                                 .comparison_mean = args.comparison_mean[index],
                                 .reference_square = args.reference_square[index],
                                 .comparison_square = args.comparison_square[index],
                                 .cross_product = args.cross_product[index]};
        add_vertical_tap(sums, row, G[tap]);
    }
    return round_moments(sums);
}

} // namespace

namespace
{

/* SIMD-16 with the 256-entry register file: the vertical moments and the
 * two fp64 quotients in integers stay in registers (no scratch memory,
 * ADR-1395). */
constexpr int FSSIM_TERM_SG = 16;
constexpr int FSSIM_TERM_GRF = 256;

/* The CPU's lv, cv and sv of the window at (x, y). Flattened into the
 * kernel: a call left in it takes scratch memory for its frame (ADR-1395). */
__attribute__((flatten, always_inline)) static inline SsimDoubleTerms
float_ssim_window_terms(const FloatVertArgs &args, size_t x, size_t y)
{
    return ssim_double_terms(ssim_float_parts(vertical_moments(args, x, y), args.c1, args.c2),
                             args.c1, args.c2);
}

/* Pass 2: one work-item per window stores the fp64 bit pattern of that
 * window's `lv * cv * sv` at its raster position. There is no reduction on
 * the device: iqa_ssim() adds every term into one double, row after row, and
 * those additions round (ADR-1463). */
class FloatSsimTermKernel : public VmafSyclKernelShape<FSSIM_TERM_SG, FSSIM_TERM_GRF>
{
  public:
    explicit FloatSsimTermKernel(const FloatVertArgs &args) : a_(args)
    {
    }

    VMAF_SYCL_FUNCTOR_SG_SIZE(FSSIM_TERM_SG) void operator()(sycl::id<2> id) const
    {
        a_.terms[id[0] * (size_t)a_.final_width + id[1]] =
            ssim_product_bits(float_ssim_window_terms(a_, id[1], id[0]));
    }

  private:
    FloatVertArgs a_;
};

/* enable_lcs variant: lv and cv as fp64 bit patterns and the fp32 sv, each at
 * the window's raster position; the host forms the product and the four
 * sums. */
class FloatSsimLcsKernel : public VmafSyclKernelShape<FSSIM_TERM_SG, FSSIM_TERM_GRF>
{
  public:
    explicit FloatSsimLcsKernel(const FloatVertArgs &args) : a_(args)
    {
    }

    VMAF_SYCL_FUNCTOR_SG_SIZE(FSSIM_TERM_SG) void operator()(sycl::id<2> id) const
    {
        const size_t windows = (size_t)a_.final_width * a_.final_height;
        const size_t index = id[0] * (size_t)a_.final_width + id[1];
        const SsimDoubleTerms terms = float_ssim_window_terms(a_, id[1], id[0]);
        a_.terms[index] = vmaf_sycl_soft::signed_bits(terms.luminance);
        a_.terms[windows + index] = vmaf_sycl_soft::signed_bits(terms.contrast);
        a_.structure[index] = terms.structure;
    }

  private:
    FloatVertArgs a_;
};

static void launch_window_terms(sycl::queue &queue, const FloatVertArgs &args, bool enable_lcs)
{
    const sycl::range<2> windows{args.final_height, args.final_width};
    queue.submit([&](sycl::handler &handler) {
        if (enable_lcs) {
            handler.parallel_for(windows, FloatSsimLcsKernel(args));
        } else {
            handler.parallel_for(windows, FloatSsimTermKernel(args));
        }
    });
}

} // namespace

namespace
{

static int round_to_int(float x)
{
    return (int)(x + (x < 0.0f ? -0.5f : 0.5f));
}
static int min_int(int a, int b)
{
    return a < b ? a : b;
}
static int compute_scale(unsigned w, unsigned h, int override_)
{
    if (override_ > 0) {
        return override_;
    }
    int const scaled = round_to_int((float)min_int((int)w, (int)h) / 256.0f);
    return scaled < 1 ? 1 : scaled;
}

/* ssim.c decimates only above scale 1, to iqa_decimate_dim() samples. */
static unsigned decimated_extent(unsigned extent, int scale)
{
    return scale > 1 ? (unsigned)iqa_decimate_dim((int)extent, scale) : extent;
}

/* ADR-1370: what the device computes exactly — a decimated plane that holds
 * the 11x11 Gaussian and a scale whose window sum is exact in int64. */
static bool float_ssim_geometry_supported(unsigned w, unsigned h, int scale)
{
    return scale <= SSIM_MAX_EXACT_SCALE && decimated_extent(w, scale) >= (unsigned)SSIM_K &&
           decimated_extent(h, scale) >= (unsigned)SSIM_K;
}

/* ADR-1324: dimensions are unavailable to the earlier option-value gate. */
static int check_context_sycl(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                              unsigned w, unsigned h)
{
    (void)pix_fmt;
    (void)bpc;
    const auto *s = static_cast<const SsimStateSycl *>(fex->priv);
    const int scale = compute_scale(w, h, s->scale_override);
    return float_ssim_geometry_supported(w, h, scale) ? 0 : -ENOTSUP;
}

} // namespace

static const VmafOption options_ssim_sycl[] = {
    {
        .name = "enable_lcs",
        .help = "enable luminance, contrast and structure intermediate output",
        .offset = offsetof(SsimStateSycl, enable_lcs),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val = {.b = false},
    },
    {
        .name = "enable_db",
        .help = "write SSIM values as dB",
        .offset = offsetof(SsimStateSycl, enable_db),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val = {.b = false},
    },
    {
        .name = "clip_db",
        .help = "clip dB scores",
        .offset = offsetof(SsimStateSycl, clip_db),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val = {.b = false},
    },
    {
        .name = "scale",
        .help = "decimation scale factor (0=auto, 1=no downscaling, 2-10=explicit)",
        .offset = offsetof(SsimStateSycl, scale_override),
        .type = VMAF_OPT_TYPE_INT,
        .default_val = {.i = 0},
        .min = 0,
        .max = 10,
    },
    {.name = nullptr},
};

namespace
{

/* picture_copy(): 10 / 12 / 16-bit samples are uint16 divided by 4 / 16 /
 * 256 (exact, so a reciprocal multiply matches); every other depth is read
 * as uint8 unscaled. */
static void configure_sample_layout(SsimStateSycl *s, unsigned bpc)
{
    s->sample_bytes = 2U;
    if (bpc == 10U) {
        s->sample_scale = 1.0f / 4.0f;
    } else if (bpc == 12U) {
        s->sample_scale = 1.0f / 16.0f;
    } else if (bpc == 16U) {
        s->sample_scale = 1.0f / 256.0f;
    } else {
        s->sample_bytes = 1U;
        s->sample_scale = 1.0f;
    }
}

static int configure_float_ssim(SsimStateSycl *s, unsigned bpc, unsigned width, unsigned height)
{
    const int scale = compute_scale(width, height, s->scale_override);
    if (!float_ssim_geometry_supported(width, height, scale)) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "ssim_sycl: %ux%u at scale=%d decimates to %ux%u; needs at least the 11x11 "
                 "Gaussian footprint and scale <= %d.\n",
                 width, height, scale, decimated_extent(width, scale),
                 decimated_extent(height, scale), SSIM_MAX_EXACT_SCALE);
        return -EINVAL;
    }
    s->width = width;
    s->height = height;
    s->bpc = bpc;
    s->scale = scale;
    s->dec_width = decimated_extent(width, scale);
    s->dec_height = decimated_extent(height, scale);
    /* ssim.c::ssim_low_pass_alloc: inv2 = 1.0f / (float)(scale * scale). */
    s->tap_weight = 1.0f / (float)(scale * scale);
    configure_sample_layout(s, bpc);
    s->w_horiz = s->dec_width - (SSIM_K - 1);
    s->h_horiz = s->dec_height;
    s->w_final = s->dec_width - (SSIM_K - 1);
    s->h_final = s->dec_height - (SSIM_K - 1);
    float_ssim_constants(&s->c1, &s->c2);
    s->max_db = vmaf_ssim_max_db(s->clip_db, bpc, width, height);
    return 0;
}

} // namespace

namespace
{

template <typename T> static T *allocate_host(VmafSyclState *state, size_t bytes)
{
    return static_cast<T *>(vmaf_sycl_malloc_host(state, bytes));
}

template <typename T> static T *allocate_device(VmafSyclState *state, size_t bytes)
{
    return static_cast<T *>(vmaf_sycl_malloc_device(state, bytes));
}

} // namespace

namespace
{

static void allocate_float_ssim(SsimStateSycl *s)
{
    const size_t raw_bytes = (size_t)s->width * s->height * s->sample_bytes;
    const size_t input_bytes = (size_t)s->dec_width * s->dec_height * sizeof(float);
    const size_t horiz_bytes = (size_t)s->w_horiz * s->h_horiz * sizeof(float);
    const size_t windows = (size_t)s->w_final * s->h_final;
    const size_t terms_bytes = (s->enable_lcs ? 2U : 1U) * windows * sizeof(std::uint64_t);
    s->h_ref_raw = allocate_host<void>(s->sycl_state, raw_bytes);
    s->h_cmp_raw = allocate_host<void>(s->sycl_state, raw_bytes);
    s->d_ref_raw = allocate_device<void>(s->sycl_state, raw_bytes);
    s->d_cmp_raw = allocate_device<void>(s->sycl_state, raw_bytes);
    s->d_ref = allocate_device<float>(s->sycl_state, input_bytes);
    s->d_cmp = allocate_device<float>(s->sycl_state, input_bytes);
    s->d_ref_mu = allocate_device<float>(s->sycl_state, horiz_bytes);
    s->d_cmp_mu = allocate_device<float>(s->sycl_state, horiz_bytes);
    s->d_ref_sq = allocate_device<float>(s->sycl_state, horiz_bytes);
    s->d_cmp_sq = allocate_device<float>(s->sycl_state, horiz_bytes);
    s->d_refcmp = allocate_device<float>(s->sycl_state, horiz_bytes);
    s->d_terms = allocate_device<std::uint64_t>(s->sycl_state, terms_bytes);
    s->h_terms = allocate_host<std::uint64_t>(s->sycl_state, terms_bytes);
    if (s->enable_lcs) {
        s->d_structure = allocate_device<float>(s->sycl_state, windows * sizeof(float));
        s->h_structure = allocate_host<float>(s->sycl_state, windows * sizeof(float));
    }
}

} // namespace

namespace
{

/* Packs the first `width` samples of every luma row into pinned staging so
 * each plane goes up in one DMA. Shared by float_ssim_sycl (ADR-1370) and
 * integer_ssim_sycl. */
template <typename T>
static void pack_integer_plane(T *destination, const VmafPicture *picture, unsigned width,
                               unsigned height)
{
    const auto *source = static_cast<const uint8_t *>(picture->data[0]);
    for (unsigned y = 0; y < height; ++y) {
        __builtin_memcpy(destination + (size_t)y * width, source + (size_t)y * picture->stride[0],
                         (size_t)width * sizeof(T));
    }
}

/* The samples picture_copy() reads: uint16 at 10 / 12 / 16 bits, else the
 * first `width` bytes of each row. */
static void stage_raw_luma(const SsimStateSycl *s, const VmafPicture *picture, void *staging)
{
    if (s->sample_bytes == 2U) {
        pack_integer_plane(static_cast<std::uint16_t *>(staging), picture, s->width, s->height);
    } else {
        pack_integer_plane(static_cast<std::uint8_t *>(staging), picture, s->width, s->height);
    }
}

/* Both pictures' luma rows (`row` bytes each) into `ref_dst` / `cmp_dst`,
 * packed, copied on the device: the frames of the VMAFx API on this device
 * are never read on the host (ADR-2091). 0, or the first failure. */
static int read_device_luma(sycl::queue &q, const VmafPicture *reference,
                            const VmafPicture *comparison, void *ref_dst, void *cmp_dst, size_t row,
                            unsigned rows)
{
    const int err =
        vmaf_sycl_picture_read_plane(reference, 0, &q, ref_dst, row, row, rows, nullptr);
    return err ? err :
                 vmaf_sycl_picture_read_plane(comparison, 0, &q, cmp_dst, row, row, rows, nullptr);
}

/* float_ssim's raw luma into d_ref_raw / d_cmp_raw: packed on the host and
 * uploaded (one DMA per plane), or copied on the device for device frames. */
static int upload_raw_luma(sycl::queue &q, const SsimStateSycl *s, const VmafPicture *ref_pic,
                           const VmafPicture *dist_pic)
{
    const size_t row = (size_t)s->width * s->sample_bytes;
    if (vmaf_sycl_picture_on_device(ref_pic) || vmaf_sycl_picture_on_device(dist_pic)) {
        return read_device_luma(q, ref_pic, dist_pic, s->d_ref_raw, s->d_cmp_raw, row, s->height);
    }
    stage_raw_luma(s, ref_pic, s->h_ref_raw);
    stage_raw_luma(s, dist_pic, s->h_cmp_raw);
    const size_t raw_bytes = row * s->height;
    q.memcpy(s->d_ref_raw, s->h_ref_raw, raw_bytes);
    q.memcpy(s->d_cmp_raw, s->h_cmp_raw, raw_bytes);
    return 0;
}

} // namespace

namespace
{

static bool float_ssim_allocations_complete(const SsimStateSycl *s)
{
    const bool lcs_complete = !s->enable_lcs || (s->d_structure && s->h_structure);
    return s->h_ref_raw && s->h_cmp_raw && s->d_ref_raw && s->d_cmp_raw && s->d_ref && s->d_cmp &&
           s->d_ref_mu && s->d_cmp_mu && s->d_ref_sq && s->d_cmp_sq && s->d_refcmp && s->d_terms &&
           s->h_terms && lcs_complete;
}

} // namespace

namespace
{

static int close_fex_sycl(VmafFeatureExtractor *fex);

static int init_fex_sycl(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                         unsigned width, unsigned height)
{
    (void)pix_fmt;
    auto *s = static_cast<SsimStateSycl *>(fex->priv);
    const int config_error = configure_float_ssim(s, bpc, width, height);
    if (config_error) {
        return config_error;
    }
    if (!fex->sycl_state) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "ssim_sycl: no SYCL state\n");
        return -EINVAL;
    }
    s->sycl_state = fex->sycl_state;
    allocate_float_ssim(s);
    if (!float_ssim_allocations_complete(s)) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "ssim_sycl: USM allocation failed\n");
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

} // namespace

namespace
{

/* Pass 2 and the read-back of the per-window terms; `enable_lcs` selects the
 * L/C/S kernel and reads its structure plane back as well. */
static void enqueue_float_vertical(SsimStateSycl *s, sycl::queue &q)
{
    const FloatVertArgs vert_args{.reference_mean = s->d_ref_mu,
                                  .comparison_mean = s->d_cmp_mu,
                                  .reference_square = s->d_ref_sq,
                                  .comparison_square = s->d_cmp_sq,
                                  .cross_product = s->d_refcmp,
                                  .terms = s->d_terms,
                                  .structure = s->d_structure,
                                  .horizontal_width = s->w_horiz,
                                  .final_width = s->w_final,
                                  .final_height = s->h_final,
                                  .c1 = s->c1,
                                  .c2 = s->c2};
    const size_t windows = (size_t)s->w_final * s->h_final;
    launch_window_terms(q, vert_args, s->enable_lcs);
    if (s->enable_lcs) {
        q.memcpy(s->h_structure, s->d_structure, windows * sizeof(float));
    }
    q.memcpy(s->h_terms, s->d_terms, (s->enable_lcs ? 2U : 1U) * windows * sizeof(std::uint64_t));
}

} // namespace

namespace
{

static int submit_fex_sycl(VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                           VmafPicture *dist_pic, VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic_90;
    (void)dist_pic_90;
    auto *s = static_cast<SsimStateSycl *>(fex->priv);
    auto *qptr = static_cast<sycl::queue *>(vmaf_sycl_get_queue_ptr(s->sycl_state));
    if (!qptr)
        return -EINVAL;
    if (!ref_pic || !dist_pic) {
        /* vmaf_read_pictures_sycl() passes no host pictures; this twin
         * uploads its own luma and does not read the shared frame. */
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "ssim_sycl: needs host pictures\n");
        return -EINVAL;
    }
    sycl::queue &q = *qptr;

    /* ADR-1370: raw luma up (one DMA per plane), then picture_copy()'s
     * normalisation and ssim.c's decimation on the device. No host wait:
     * collect() waits once for the read-back. */
    if (upload_raw_luma(q, s, ref_pic, dist_pic) != 0)
        return -EIO;
    launch_decimate(q,
                    {.reference = s->d_ref_raw,
                     .comparison = s->d_cmp_raw,
                     .reference_out = s->d_ref,
                     .comparison_out = s->d_cmp,
                     .width = s->width,
                     .height = s->height,
                     .output_width = s->dec_width,
                     .output_height = s->dec_height,
                     .scale = s->scale,
                     .sample_scale = s->sample_scale,
                     .tap_weight = s->tap_weight},
                    s->sample_bytes);

    launch_horiz(q, {.reference = s->d_ref,
                     .comparison = s->d_cmp,
                     .reference_mean = s->d_ref_mu,
                     .comparison_mean = s->d_cmp_mu,
                     .reference_square = s->d_ref_sq,
                     .comparison_square = s->d_cmp_sq,
                     .cross_product = s->d_refcmp,
                     .width = s->dec_width,
                     .output_width = s->w_horiz,
                     .output_height = s->h_horiz});
    enqueue_float_vertical(s, q);

    s->pending_index = index;
    s->has_pending = true;
    return 0;
}

} // namespace

namespace
{

/* iqa_ssim()'s frame sum of one kind of term: every window's double added
 * into one double, row after row, each row left to right. The integer SSIM
 * twin below adds its terms the same way (calc_ssim()). */
static double frame_sum_of_terms(const std::uint64_t *terms, size_t count)
{
    double sum = 0.0;
    for (size_t i = 0U; i < count; i++) {
        sum += std::bit_cast<double>(terms[i]);
    }
    return sum;
}

/* iqa/ssim_tools.c::iqa_ssim returns every frame mean as fp32,
 * `(float)(sum / (double)(w * h))`; the twin rounds the same way, so a frame
 * whose mean rounds to 1 scores exactly 1 and enable_db reports the CPU's
 * +inf / clip_db ceiling for it (ADR-1370). */
static int float_ssim_frame_mean(const char *feature, double sum, double n_pixels, unsigned index,
                                 double *mean)
{
    const int err =
        vmaf_feature_finite_ratio_named("float_ssim_sycl", feature, sum, n_pixels, index, mean);
    if (!err) {
        *mean = (double)(float)*mean;
    }
    return err;
}

/* enable_lcs: the four sums become the frame means float_ssim and
 * float_ssim_{l,c,s}, published in CPU float_ssim.c order after the shared
 * SSIM validation (ADR-1302). */
static int emit_float_ssim_lcs(const SsimStateSycl *s, const SsimFrameSums &sums, double n_pixels,
                               unsigned index, VmafFeatureCollector *feature_collector)
{
    static const char *const atom_names[3] = {"float_ssim_l", "float_ssim_c", "float_ssim_s"};
    const double atom_sums[3] = {sums.luminance, sums.contrast, sums.structure};
    VmafNamedScore atoms[3];
    double score = 0.0;
    int err = float_ssim_frame_mean("float_ssim", sums.ssim, n_pixels, index, &score);
    for (unsigned k = 0; k < 3U && !err; k++) {
        atoms[k].name = atom_names[k];
        err = float_ssim_frame_mean(atom_names[k], atom_sums[k], n_pixels, index, &atoms[k].value);
    }
    if (err)
        return err;
    return vmaf_ssim_emit_scores_named(feature_collector, s->feature_name_dict, "float_ssim_sycl",
                                       "float_ssim", score, s->enable_db, s->max_db, atoms, 3U,
                                       index);
}

static int collect_fex_sycl(VmafFeatureExtractor *fex, unsigned index,
                            VmafFeatureCollector *feature_collector)
{
    auto *s = static_cast<SsimStateSycl *>(fex->priv);
    auto *qptr = static_cast<sycl::queue *>(vmaf_sycl_get_queue_ptr(s->sycl_state));
    if (!qptr)
        return -EINVAL;
    qptr->wait();

    /* The frame sums in iqa_ssim()'s order -> means over the (W'-10)·(H'-10)
     * windows of the decimated plane, rounded to fp32 like the CPU. */
    const size_t windows = (size_t)s->w_final * s->h_final;
    const double n_pixels = (double)s->w_final * (double)s->h_final;
    if (s->enable_lcs) {
        const SsimFrameSums sums =
            ssim_frame_sums(s->h_terms, s->h_terms + windows, s->h_structure, windows);
        return emit_float_ssim_lcs(s, sums, n_pixels, index, feature_collector);
    }
    double score = 0.0;
    const int err = float_ssim_frame_mean("float_ssim", frame_sum_of_terms(s->h_terms, windows),
                                          n_pixels, index, &score);
    if (err)
        return err;
    return vmaf_ssim_emit_score_named(feature_collector, s->feature_name_dict, "float_ssim_sycl",
                                      "float_ssim", score, s->enable_db, s->max_db, index);
}

} // namespace

namespace
{

/* Host copy of pass 1 for the test hook below: the kernel's taps over a
 * plain plane instead of the local-memory tile. */
static void host_horizontal_pass(const float *reference, const float *comparison, unsigned width,
                                 unsigned height, std::vector<SsimMoments> &rows)
{
    const unsigned w_out = width - (unsigned)(SSIM_K - 1);
    for (unsigned y = 0; y < height; ++y) {
        for (unsigned x = 0; x < w_out; ++x) {
            MomentPairs sums{};
            for (int tap = 0; tap < SSIM_K; ++tap) {
                const size_t index = (size_t)y * width + x + (size_t)tap;
                add_horizontal_tap(sums, reference[index], comparison[index], G[tap]);
            }
            rows[(size_t)y * w_out + x] = round_moments(sums);
        }
    }
}

/* Host copy of pass 2: the kernels' per-window terms and the host's sums.
 * `kernel_ssim` is the sum of the default kernel's terms (`lv * cv * sv`
 * formed in integers on the device), `sums` what the enable_lcs path forms
 * from lv, cv and sv on the host. */
static void host_vertical_sums(const std::vector<SsimMoments> &rows, unsigned w_out, unsigned h_out,
                               SsimFrameSums &sums, double &kernel_ssim)
{
    float c1 = 0.0f;
    float c2 = 0.0f;
    float_ssim_constants(&c1, &c2);
    for (unsigned y = 0; y < h_out; ++y) {
        for (unsigned x = 0; x < w_out; ++x) {
            MomentPairs moments{};
            for (int tap = 0; tap < SSIM_K; ++tap)
                add_vertical_tap(moments, rows[((size_t)y + (size_t)tap) * w_out + x], G[tap]);
            const SsimDoubleTerms terms =
                ssim_double_terms(ssim_float_parts(round_moments(moments), c1, c2), c1, c2);
            kernel_ssim += std::bit_cast<double>(ssim_product_bits(terms));
            accumulate_window(sums,
                              std::bit_cast<double>(vmaf_sycl_soft::signed_bits(terms.luminance)),
                              std::bit_cast<double>(vmaf_sycl_soft::signed_bits(terms.contrast)),
                              (double)terms.structure);
        }
    }
}

} // namespace

/* Test hook (core/test/test_sycl_float_ssim_parity.c): the float_ssim pipeline
 * after decimation, run on the host with the kernels' own arithmetic
 * (add_horizontal_tap, add_vertical_tap, ssim_float_parts, ssim_double_terms,
 * ssim_product_bits) and the host's own sums, so the result is the device's
 * for the same per-window values; the test compares it with the CPU
 * extractor without a device. `means` receives the fp32-rounded frame means:
 * SSIM as the default kernel forms it, L, C, S, and SSIM as the enable_lcs
 * path forms it. Returns 0, -EINVAL for a plane smaller than the 11 x 11
 * window, or -ENOMEM. */
extern "C" int vmaf_sycl_float_ssim_host_means(const float *reference, const float *comparison,
                                               unsigned width, unsigned height, double means[5])
{
    if (!reference || !comparison || !means || width < (unsigned)SSIM_K ||
        height < (unsigned)SSIM_K)
        return -EINVAL;
    const unsigned w_out = width - (unsigned)(SSIM_K - 1);
    const unsigned h_out = height - (unsigned)(SSIM_K - 1);
    SsimFrameSums sums = {};
    double kernel_ssim = 0.0;
    try {
        std::vector<SsimMoments> rows((size_t)w_out * height);
        host_horizontal_pass(reference, comparison, width, height, rows);
        host_vertical_sums(rows, w_out, h_out, sums, kernel_ssim);
    } catch (const std::bad_alloc &) {
        return -ENOMEM;
    }
    const double n_pixels = (double)w_out * (double)h_out;
    const double totals[5] = {kernel_ssim, sums.luminance, sums.contrast, sums.structure,
                              sums.ssim};
    for (unsigned k = 0; k < 5U; ++k)
        means[k] = (double)(float)(totals[k] / n_pixels);
    return 0;
}

namespace
{

template <typename T> static void release_buffer(VmafSyclState *state, T *pointer)
{
    if (pointer) {
        vmaf_sycl_free(state, pointer);
    }
}

} // namespace

namespace
{

static int close_fex_sycl(VmafFeatureExtractor *fex)
{
    auto *s = static_cast<SsimStateSycl *>(fex->priv);
    if (s->sycl_state) {
        release_buffer(s->sycl_state, s->h_ref_raw);
        release_buffer(s->sycl_state, s->h_cmp_raw);
        release_buffer(s->sycl_state, s->d_ref_raw);
        release_buffer(s->sycl_state, s->d_cmp_raw);
        release_buffer(s->sycl_state, s->d_ref);
        release_buffer(s->sycl_state, s->d_cmp);
        release_buffer(s->sycl_state, s->d_ref_mu);
        release_buffer(s->sycl_state, s->d_cmp_mu);
        release_buffer(s->sycl_state, s->d_ref_sq);
        release_buffer(s->sycl_state, s->d_cmp_sq);
        release_buffer(s->sycl_state, s->d_refcmp);
        release_buffer(s->sycl_state, s->d_terms);
        release_buffer(s->sycl_state, s->h_terms);
        release_buffer(s->sycl_state, s->d_structure);
        release_buffer(s->sycl_state, s->h_structure);
    }
    if (s->feature_name_dict) {
        vmaf_dictionary_free(&s->feature_name_dict);
    }
    return 0;
}

static const char *provided_features_ssim_sycl[] = {"float_ssim", nullptr};

} // namespace

extern "C" VmafFeatureExtractor vmaf_fex_float_ssim_sycl = {
    .name = "float_ssim_sycl",
    .init = init_fex_sycl,
    .extract = nullptr,
    .flush = nullptr,
    .close = close_fex_sycl,
    .submit = submit_fex_sycl,
    .collect = collect_fex_sycl,
    .options = options_ssim_sycl,
    .priv_size = sizeof(SsimStateSycl),
    .flags = VMAF_FEATURE_EXTRACTOR_SYCL,
    .provided_features = provided_features_ssim_sycl,
    .chars =
        {
            .n_dispatches_per_frame = 3,
            .is_reduction_only = false,
            .min_useful_frame_area = 1920U * 1080U,
            .dispatch_hint = VMAF_FEATURE_DISPATCH_AUTO,
        },
    .context_check = check_context_sycl,
    .context_fallback_name = "float_ssim",
};

/* ============================================================
 * Real integer_ssim SYCL extractor (ADR-0564; the CPU's arithmetic and
 * the CPU's sum since ADR-1443)
 *
 * int64 moments equal to integer_ssim.c's: the same 9-tap integer
 * Gaussian [2,9,28,55,68,55,28,9,2] and the same truncation at the
 * frame's edges.
 *
 * integer_ssim.c::ssim_reduce_row_range() forms each pixel's term in
 * fp64 and calc_ssim() adds every term into one double, left to right
 * and top to bottom. A kernel has no fp64 type (ADR-0220) and a sum of
 * doubles is its order, so:
 *
 *   Pass 1 (launch_issim_horiz): 9-tap int64 horizontal moment
 *     accumulation. Writes 5 x (W x H) int64 USM arrays.
 *   Pass 2 (launch_issim_terms): 9-tap int64 vertical accumulation,
 *     then the reference's fp64 operations on values held in 64-bit
 *     integers (sycl_integer_ssim_math.h). Stores the fp64 bit pattern
 *     of every pixel's term at its raster position; no reduction.
 *   Host: adds the read-back plane in index order, which is
 *     calc_ssim()'s order, and divides by the weight sum.
 *
 * The window weight is not a plane: it is the product of the two tap
 * sums, and its frame sum is the product of the two line sums.
 * ============================================================ */

namespace
{

constexpr size_t ISSIM_WG_X = 16;
static constexpr size_t ISSIM_WG_Y = 8;
/* 9-tap integer Gaussian kernel matching gaussian_filter_init(sigma=1.5, max_len=5):
 * [2, 9, 28, 55, 68, 55, 28, 9, 2], sum=256, kernel_len=4. */
static constexpr int ISSIM_HALF_K = 4;
static constexpr int ISSIM_K_SZ = 9;
constexpr int32_t ISSIM_KERNEL[ISSIM_K_SZ] = {2, 9, 28, 55, 68, 55, 28, 9, 2};

} // namespace

namespace
{

struct IssimStateSycl {
    unsigned width;
    unsigned height;
    unsigned bpc;
    /* CPU integer_ssim.c options; host-side dB conversion. */
    bool enable_db;
    bool clip_db;
    /* vmaf_ssim_max_db(): +inf unless clip_db. */
    double max_db;

    VmafSyclState *sycl_state;

    /* Staging buffers: host-pinned input (packed, no stride). */
    uint8_t *h_ref_u8;
    uint8_t *h_cmp_u8;
    uint16_t *h_ref_u16;
    uint16_t *h_cmp_u16;

    /* Device USM input planes. */
    uint8_t *d_ref_u8;
    uint8_t *d_cmp_u8;
    uint16_t *d_ref_u16;
    uint16_t *d_cmp_u16;

    /* Five int64 intermediate device arrays for horizontal pass. */
    int64_t *d_mux;
    int64_t *d_muy;
    int64_t *d_x2;
    int64_t *d_xy;
    int64_t *d_y2;

    /* The fp64 bit pattern of every pixel's term, in raster order. */
    uint64_t *d_terms;
    uint64_t *h_terms;
    /* calc_ssim()'s `ssimw`: the sum of every window's weight. */
    int64_t total_weight;
    /* fl64(sm * sm * SSIM_K1) and fl64(sm * sm * SSIM_K2). */
    vmaf_sycl_issim::Stabilisers stabilisers;

    bool has_pending;
    unsigned pending_index;

    VmafDictionary *feature_name_dict;
};

} // namespace

/* Pass 1 (8bpc): horizontal 9-tap int64 moment accumulation. */
namespace
{

static void launch_issim_horiz_8bpc(sycl::queue &q, const uint8_t *d_ref, const uint8_t *d_cmp,
                                    int64_t *d_mux, int64_t *d_muy, int64_t *d_x2, int64_t *d_xy,
                                    int64_t *d_y2, unsigned width, unsigned height)
{
    const size_t gx = ((width + ISSIM_WG_X - 1) / ISSIM_WG_X) * ISSIM_WG_X;
    const size_t gy = ((height + ISSIM_WG_Y - 1) / ISSIM_WG_Y) * ISSIM_WG_Y;
    const unsigned e_width = width;
    const unsigned e_height = height;
    q.submit([&](sycl::handler &cgh) {
        cgh.parallel_for(
            sycl::nd_range<2>{sycl::range<2>{gy, gx}, sycl::range<2>{ISSIM_WG_Y, ISSIM_WG_X}},
            [=](sycl::nd_item<2> it) {
                const unsigned x = (unsigned)it.get_global_id(1);
                const unsigned y = (unsigned)it.get_global_id(0);
                if (x >= e_width || y >= e_height)
                    return;
                const int k_min = (int)x < ISSIM_HALF_K ? ISSIM_HALF_K - (int)x : 0;
                const int k_max = ((int)x + ISSIM_HALF_K >= (int)e_width) ?
                                      ISSIM_K_SZ - ((int)x + ISSIM_HALF_K - (int)e_width + 1) :
                                      ISSIM_K_SZ;
                int64_t mux = 0LL;
                int64_t muy = 0LL;
                int64_t x2 = 0LL;
                int64_t xy = 0LL;
                int64_t y2 = 0LL;
                for (int k = k_min; k < k_max; k++) {
                    const int src_x = (int)x - ISSIM_HALF_K + k;
                    const int64_t s = (int64_t)d_ref[(size_t)y * e_width + (unsigned)src_x];
                    const int64_t d = (int64_t)d_cmp[(size_t)y * e_width + (unsigned)src_x];
                    const int64_t wk = (int64_t)ISSIM_KERNEL[k];
                    mux += wk * s;
                    muy += wk * d;
                    x2 += wk * s * s;
                    xy += wk * s * d;
                    y2 += wk * d * d;
                }
                const size_t idx = (size_t)y * e_width + x;
                d_mux[idx] = mux;
                d_muy[idx] = muy;
                d_x2[idx] = x2;
                d_xy[idx] = xy;
                d_y2[idx] = y2;
            });
    });
}

} // namespace

/* Pass 1 (>8bpc): same as above but reads uint16_t. */
namespace
{

static void launch_issim_horiz_16bpc(sycl::queue &q, const uint16_t *d_ref, const uint16_t *d_cmp,
                                     int64_t *d_mux, int64_t *d_muy, int64_t *d_x2, int64_t *d_xy,
                                     int64_t *d_y2, unsigned width, unsigned height)
{
    const size_t gx = ((width + ISSIM_WG_X - 1) / ISSIM_WG_X) * ISSIM_WG_X;
    const size_t gy = ((height + ISSIM_WG_Y - 1) / ISSIM_WG_Y) * ISSIM_WG_Y;
    const unsigned e_width = width;
    const unsigned e_height = height;
    q.submit([&](sycl::handler &cgh) {
        cgh.parallel_for(
            sycl::nd_range<2>{sycl::range<2>{gy, gx}, sycl::range<2>{ISSIM_WG_Y, ISSIM_WG_X}},
            [=](sycl::nd_item<2> it) {
                const unsigned x = (unsigned)it.get_global_id(1);
                const unsigned y = (unsigned)it.get_global_id(0);
                if (x >= e_width || y >= e_height)
                    return;
                const int k_min = (int)x < ISSIM_HALF_K ? ISSIM_HALF_K - (int)x : 0;
                const int k_max = ((int)x + ISSIM_HALF_K >= (int)e_width) ?
                                      ISSIM_K_SZ - ((int)x + ISSIM_HALF_K - (int)e_width + 1) :
                                      ISSIM_K_SZ;
                int64_t mux = 0LL;
                int64_t muy = 0LL;
                int64_t x2 = 0LL;
                int64_t xy = 0LL;
                int64_t y2 = 0LL;
                for (int k = k_min; k < k_max; k++) {
                    const int src_x = (int)x - ISSIM_HALF_K + k;
                    const int64_t s = (int64_t)d_ref[(size_t)y * e_width + (unsigned)src_x];
                    const int64_t d = (int64_t)d_cmp[(size_t)y * e_width + (unsigned)src_x];
                    const int64_t wk = (int64_t)ISSIM_KERNEL[k];
                    mux += wk * s;
                    muy += wk * d;
                    x2 += wk * s * s;
                    xy += wk * s * d;
                    y2 += wk * d * d;
                }
                const size_t idx = (size_t)y * e_width + x;
                d_mux[idx] = mux;
                d_muy[idx] = muy;
                d_x2[idx] = x2;
                d_xy[idx] = xy;
                d_y2[idx] = y2;
            });
    });
}

} // namespace

namespace
{

/* Shape of the term kernel: no required sub-group size and the large
 * register file (ADR-1501). At a required SIMD-16 it spilled 320 bytes on
 * Xe-LP (tgllp, adl-*, rpl-*), which has no large register file (measured on
 * a UHD 770); without a required size icpx compiles it at SIMD-8 there and at
 * SIMD-16 with 256 registers everywhere else. One work-item per pixel and no
 * sub-group operation: the size does not change a term. */
constexpr int ISSIM_TERM_SG = 0;
constexpr int ISSIM_TERM_GRF = 256;

struct IntegerVertArgs {
    const int64_t *reference_mean;
    const int64_t *comparison_mean;
    const int64_t *reference_square;
    const int64_t *cross_product;
    const int64_t *comparison_square;
    uint64_t *terms;
    unsigned width;
    unsigned height;
    vmaf_sycl_issim::Stabilisers stabilisers;
};

/* The taps of the window at `position` that lie inside a line of `extent`
 * samples: the reference's k_min and k_max. */
struct TapRange {
    int first;
    int last;
};

} // namespace

namespace
{

static inline TapRange tap_range(unsigned position, unsigned extent)
{
    const int at = (int)position;
    const int first = at < ISSIM_HALF_K ? ISSIM_HALF_K - at : 0;
    const int last = (at + ISSIM_HALF_K >= (int)extent) ?
                         ISSIM_K_SZ - (at + ISSIM_HALF_K - (int)extent + 1) :
                         ISSIM_K_SZ;
    return {.first = first, .last = last};
}

/* The sum of the taps of a range: a line's part of the window weight. */
static inline int64_t tap_weight(TapRange taps)
{
    int64_t weight = 0;
    for (int tap = taps.first; tap < taps.last; ++tap) {
        weight += (int64_t)ISSIM_KERNEL[tap];
    }
    return weight;
}

} // namespace

namespace
{

static inline vmaf_sycl_issim::Moments vertical_integer_moments(const IntegerVertArgs &args,
                                                                unsigned x, unsigned y)
{
    const TapRange rows = tap_range(y, args.height);
    int64_t reference_mean = 0;
    int64_t comparison_mean = 0;
    int64_t reference_square = 0;
    int64_t cross_product = 0;
    int64_t comparison_square = 0;
    int64_t row_weight = 0;
    for (int tap = rows.first; tap < rows.last; ++tap) {
        const unsigned source_y = (unsigned)((int)y - ISSIM_HALF_K + tap);
        const size_t index = (size_t)source_y * args.width + x;
        const int64_t coefficient = (int64_t)ISSIM_KERNEL[tap];
        reference_mean += coefficient * args.reference_mean[index];
        comparison_mean += coefficient * args.comparison_mean[index];
        reference_square += coefficient * args.reference_square[index];
        cross_product += coefficient * args.cross_product[index];
        comparison_square += coefficient * args.comparison_square[index];
        row_weight += coefficient;
    }
    /* Every moment is a sum of non-negative products. */
    return {.mux = (uint64_t)reference_mean,
            .muy = (uint64_t)comparison_mean,
            .x2 = (uint64_t)reference_square,
            .xy = (uint64_t)cross_product,
            .y2 = (uint64_t)comparison_square,
            .w = (uint64_t)(row_weight * tap_weight(tap_range(x, args.width)))};
}

} // namespace

namespace
{

/* The fp64 bit pattern of the term ssim_reduce_row_range() adds for the
 * pixel at (x, y). Flattened into the kernel: a call left in it takes
 * scratch memory for its frame (ADR-1395). */
__attribute__((flatten, always_inline)) static inline uint64_t
integer_ssim_term(const IntegerVertArgs &args, unsigned x, unsigned y)
{
    return vmaf_sycl_issim::term_bits(vertical_integer_moments(args, x, y), args.stabilisers);
}

/* One work-item per pixel stores that pixel's term at its raster position.
 * There is no reduction on the device: calc_ssim() adds every term into one
 * double, row after row, and those additions round. */
class IssimTermKernel : public VmafSyclKernelShape<ISSIM_TERM_SG, ISSIM_TERM_GRF>
{
  public:
    explicit IssimTermKernel(const IntegerVertArgs &args) : a_(args)
    {
    }

    void operator()(sycl::id<2> id) const
    {
        a_.terms[id[0] * (size_t)a_.width + id[1]] =
            integer_ssim_term(a_, (unsigned)id[1], (unsigned)id[0]);
    }

  private:
    IntegerVertArgs a_;
};

static void launch_issim_terms(sycl::queue &queue, const IntegerVertArgs &args)
{
    queue.submit([&](sycl::handler &handler) {
        handler.parallel_for(sycl::range<2>{args.height, args.width}, IssimTermKernel(args));
    });
}

/* The sum of the window weights along a line of `extent` samples. */
static int64_t line_weight(unsigned extent)
{
    int64_t weight = 0;
    for (unsigned position = 0u; position < extent; position++) {
        weight += tap_weight(tap_range(position, extent));
    }
    return weight;
}

} // namespace

namespace
{

struct IntegerBufferSizes {
    size_t pixels8;
    size_t pixels16;
    size_t moments;
    size_t terms;
};

static int configure_integer_ssim(IssimStateSycl *s, unsigned bpc, unsigned width, unsigned height)
{
    if (width < 1u || height < 1u) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "integer_ssim_sycl: zero-dimension input %ux%u\n", width,
                 height);
        return -EINVAL;
    }
    s->width = width;
    s->height = height;
    s->bpc = bpc;
    s->max_db = vmaf_ssim_max_db(s->clip_db, bpc, width, height);
    /* A window's weight is the product of its two tap sums, so the frame's
     * weight sum is the product of the two line sums. */
    s->total_weight = line_weight(width) * line_weight(height);
    s->stabilisers = vmaf_sycl_issim::make_stabilisers(bpc);
    return 0;
}

} // namespace

namespace
{

static IntegerBufferSizes integer_buffer_sizes(const IssimStateSycl *s)
{
    const size_t pixels = (size_t)s->width * s->height;
    return {
        .pixels8 = pixels * sizeof(uint8_t),
        .pixels16 = pixels * sizeof(uint16_t),
        .moments = pixels * sizeof(int64_t),
        .terms = pixels * sizeof(uint64_t),
    };
}

} // namespace

namespace
{

static void allocate_integer_ssim(IssimStateSycl *s, const IntegerBufferSizes &bytes)
{
    s->h_ref_u8 = allocate_host<uint8_t>(s->sycl_state, bytes.pixels8);
    s->h_cmp_u8 = allocate_host<uint8_t>(s->sycl_state, bytes.pixels8);
    s->h_ref_u16 = allocate_host<uint16_t>(s->sycl_state, bytes.pixels16);
    s->h_cmp_u16 = allocate_host<uint16_t>(s->sycl_state, bytes.pixels16);
    s->d_ref_u8 = allocate_device<uint8_t>(s->sycl_state, bytes.pixels8);
    s->d_cmp_u8 = allocate_device<uint8_t>(s->sycl_state, bytes.pixels8);
    s->d_ref_u16 = allocate_device<uint16_t>(s->sycl_state, bytes.pixels16);
    s->d_cmp_u16 = allocate_device<uint16_t>(s->sycl_state, bytes.pixels16);
    s->d_mux = allocate_device<int64_t>(s->sycl_state, bytes.moments);
    s->d_muy = allocate_device<int64_t>(s->sycl_state, bytes.moments);
    s->d_x2 = allocate_device<int64_t>(s->sycl_state, bytes.moments);
    s->d_xy = allocate_device<int64_t>(s->sycl_state, bytes.moments);
    s->d_y2 = allocate_device<int64_t>(s->sycl_state, bytes.moments);
    s->d_terms = allocate_device<uint64_t>(s->sycl_state, bytes.terms);
    s->h_terms = allocate_host<uint64_t>(s->sycl_state, bytes.terms);
}

} // namespace

namespace
{

static bool integer_ssim_allocations_complete(const IssimStateSycl *s)
{
    return s->h_ref_u8 && s->h_cmp_u8 && s->h_ref_u16 && s->h_cmp_u16 && s->d_ref_u8 &&
           s->d_cmp_u8 && s->d_ref_u16 && s->d_cmp_u16 && s->d_mux && s->d_muy && s->d_x2 &&
           s->d_xy && s->d_y2 && s->d_terms && s->h_terms;
}

} // namespace

namespace
{

static int close_fex_issim_sycl(VmafFeatureExtractor *fex);

static int init_fex_issim_sycl(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt,
                               unsigned bpc, unsigned width, unsigned height)
{
    (void)pix_fmt;
    auto *s = static_cast<IssimStateSycl *>(fex->priv);
    const int config_error = configure_integer_ssim(s, bpc, width, height);
    if (config_error) {
        return config_error;
    }
    if (!fex->sycl_state) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "integer_ssim_sycl: no SYCL state\n");
        return -EINVAL;
    }
    s->sycl_state = fex->sycl_state;
    allocate_integer_ssim(s, integer_buffer_sizes(s));
    if (!integer_ssim_allocations_complete(s)) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "integer_ssim_sycl: USM allocation failed\n");
        (void)close_fex_issim_sycl(fex);
        return -ENOMEM;
    }
    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (!s->feature_name_dict) {
        (void)close_fex_issim_sycl(fex);
        return -ENOMEM;
    }
    s->has_pending = false;
    return 0;
}

} // namespace

namespace
{

/* The luma planes into d_ref_* / d_cmp_*: packed on the host and uploaded,
 * or copied on the device for device frames (ADR-2091). 0, or a failure. */
static int upload_integer_luma(IssimStateSycl *s, sycl::queue &queue, const VmafPicture *reference,
                               const VmafPicture *comparison)
{
    const size_t pixels = (size_t)s->width * s->height;
    const bool eight = s->bpc == 8u;
    if (vmaf_sycl_picture_on_device(reference) || vmaf_sycl_picture_on_device(comparison)) {
        const size_t row = (size_t)s->width * (eight ? sizeof(uint8_t) : sizeof(uint16_t));
        return eight ? read_device_luma(queue, reference, comparison, s->d_ref_u8, s->d_cmp_u8, row,
                                        s->height) :
                       read_device_luma(queue, reference, comparison, s->d_ref_u16, s->d_cmp_u16,
                                        row, s->height);
    }
    if (eight) {
        pack_integer_plane(s->h_ref_u8, reference, s->width, s->height);
        pack_integer_plane(s->h_cmp_u8, comparison, s->width, s->height);
        queue.memcpy(s->d_ref_u8, s->h_ref_u8, pixels * sizeof(uint8_t));
        queue.memcpy(s->d_cmp_u8, s->h_cmp_u8, pixels * sizeof(uint8_t));
        return 0;
    }
    pack_integer_plane(s->h_ref_u16, reference, s->width, s->height);
    pack_integer_plane(s->h_cmp_u16, comparison, s->width, s->height);
    queue.memcpy(s->d_ref_u16, s->h_ref_u16, pixels * sizeof(uint16_t));
    queue.memcpy(s->d_cmp_u16, s->h_cmp_u16, pixels * sizeof(uint16_t));
    return 0;
}

template <typename Picture>
static int submit_integer_horizontal(IssimStateSycl *s, sycl::queue &queue, Picture *reference,
                                     Picture *comparison)
{
    if (upload_integer_luma(s, queue, reference, comparison) != 0) {
        return -EIO;
    }
    if (s->bpc == 8u) {
        launch_issim_horiz_8bpc(queue, s->d_ref_u8, s->d_cmp_u8, s->d_mux, s->d_muy, s->d_x2,
                                s->d_xy, s->d_y2, s->width, s->height);
        return 0;
    }
    launch_issim_horiz_16bpc(queue, s->d_ref_u16, s->d_cmp_u16, s->d_mux, s->d_muy, s->d_x2,
                             s->d_xy, s->d_y2, s->width, s->height);
    return 0;
}

} // namespace

namespace
{

static int submit_fex_issim_sycl(VmafFeatureExtractor *fex, VmafPicture *ref_pic,
                                 VmafPicture *ref_pic_90, VmafPicture *dist_pic,
                                 VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic_90;
    (void)dist_pic_90;
    auto *s = static_cast<IssimStateSycl *>(fex->priv);
    auto *qptr = static_cast<sycl::queue *>(vmaf_sycl_get_queue_ptr(s->sycl_state));
    if (!qptr)
        return -EINVAL;
    sycl::queue &q = *qptr;

    if (submit_integer_horizontal(s, q, ref_pic, dist_pic) != 0)
        return -EIO;
    launch_issim_terms(q, {.reference_mean = s->d_mux,
                           .comparison_mean = s->d_muy,
                           .reference_square = s->d_x2,
                           .cross_product = s->d_xy,
                           .comparison_square = s->d_y2,
                           .terms = s->d_terms,
                           .width = s->width,
                           .height = s->height,
                           .stabilisers = s->stabilisers});

    q.memcpy(s->h_terms, s->d_terms, (size_t)s->width * s->height * sizeof(uint64_t));

    s->pending_index = index;
    s->has_pending = true;
    return 0;
}

} // namespace

namespace
{

static int collect_fex_issim_sycl(VmafFeatureExtractor *fex, unsigned index,
                                  VmafFeatureCollector *feature_collector)
{
    auto *s = static_cast<IssimStateSycl *>(fex->priv);
    auto *qptr = static_cast<sycl::queue *>(vmaf_sycl_get_queue_ptr(s->sycl_state));
    if (!qptr)
        return -EINVAL;
    qptr->wait();

    /* calc_ssim(): the frame sum in the reference's order, then `ssim / ssimw`. */
    const double total_ssim = frame_sum_of_terms(s->h_terms, (size_t)s->width * s->height);
    return vmaf_ssim_emit_ratio_score_named(
        feature_collector, s->feature_name_dict, "integer_ssim_sycl", "ssim", total_ssim,
        (double)s->total_weight, s->enable_db, s->max_db, index);
}

} // namespace

namespace
{

static int close_fex_issim_sycl(VmafFeatureExtractor *fex)
{
    auto *s = static_cast<IssimStateSycl *>(fex->priv);
    if (s->sycl_state) {
        release_buffer(s->sycl_state, s->h_ref_u8);
        release_buffer(s->sycl_state, s->h_cmp_u8);
        release_buffer(s->sycl_state, s->h_ref_u16);
        release_buffer(s->sycl_state, s->h_cmp_u16);
        release_buffer(s->sycl_state, s->d_ref_u8);
        release_buffer(s->sycl_state, s->d_cmp_u8);
        release_buffer(s->sycl_state, s->d_ref_u16);
        release_buffer(s->sycl_state, s->d_cmp_u16);
        release_buffer(s->sycl_state, s->d_mux);
        release_buffer(s->sycl_state, s->d_muy);
        release_buffer(s->sycl_state, s->d_x2);
        release_buffer(s->sycl_state, s->d_xy);
        release_buffer(s->sycl_state, s->d_y2);
        release_buffer(s->sycl_state, s->d_terms);
        release_buffer(s->sycl_state, s->h_terms);
    }
    if (s->feature_name_dict) {
        (void)vmaf_dictionary_free(&s->feature_name_dict);
    }
    return 0;
}

static const VmafOption options_issim_sycl[] = {
    {
        .name = "enable_db",
        .help = "write SSIM values as dB: -10*log10(1-ssim)",
        .offset = offsetof(IssimStateSycl, enable_db),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val = {.b = false},
    },
    {
        .name = "clip_db",
        .help = "clip dB scores to a peak-derived ceiling",
        .offset = offsetof(IssimStateSycl, clip_db),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val = {.b = false},
    },
    {.name = nullptr},
};

static const char *provided_features_issim_sycl[] = {"ssim", nullptr};

} // namespace

/* Real integer_ssim SYCL extractor (ADR-0564). 9-tap int64 moments equal to
 * the CPU's, the CPU's fp64 term from 64-bit integers (fp64-free, ADR-0220)
 * and the CPU's raster-order sum on the host: the score is the CPU's bit for
 * bit (ADR-1443). Load-bearing: declared via extern in feature_extractor.c. */
extern "C" VmafFeatureExtractor vmaf_fex_integer_ssim_sycl = {
    .name = "integer_ssim_sycl",
    .init = init_fex_issim_sycl,
    .extract = nullptr,
    .flush = nullptr,
    .close = close_fex_issim_sycl,
    .submit = submit_fex_issim_sycl,
    .collect = collect_fex_issim_sycl,
    .options = options_issim_sycl,
    .priv_size = sizeof(IssimStateSycl),
    .flags = VMAF_FEATURE_EXTRACTOR_SYCL,
    .provided_features = provided_features_issim_sycl,
    .chars =
        {
            .n_dispatches_per_frame = 2,
            .is_reduction_only = false,
            .min_useful_frame_area = 1920U * 1080U,
            .dispatch_hint = VMAF_FEATURE_DISPATCH_AUTO,
        },
};
