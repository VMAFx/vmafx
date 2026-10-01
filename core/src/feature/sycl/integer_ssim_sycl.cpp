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
 *    2. nd_range vertical 11-tap + per-pixel SSIM combine +
 *       per-WG float partial sums via sycl::reduce_over_group.
 *
 *  Host accumulates partials in `double`, divides by
 *  (W'-10)·(H'-10) over the decimated W' x H' and emits `float_ssim`.
 *
 *  Options mirror CPU float_ssim.c. `enable_lcs` switches pass 2 to a
 *  variant that also reduces the per-pixel luminance / contrast /
 *  structure terms of iqa/ssim_tools.c (clamped variances, flat-region
 *  covariance clamp) into three more per-WG partials and emits
 *  `float_ssim_{l,c,s}`. `enable_db` / `clip_db` act on the host through
 *  the shared nonfinite_score.h SSIM helpers.
 *
 *  `scale` resolves as in ssim.c::compute_ssim (0 = auto from the short
 *  side). The ADR-1324 context check refuses only a geometry the device
 *  cannot compute exactly (decimated plane under the 11x11 Gaussian, or a
 *  scale past SSIM_MAX_EXACT_SCALE); model dispatch and `--feature` then
 *  run the CPU float_ssim, direct requests keep the -EINVAL init error.
 *  fp64-free (Intel Arc A380 lacks native fp64).
 */

#include <sycl/sycl.hpp>

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
#include "sycl_exact_fp.h"

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

/* float_ssim reduction (Research-2133): every per-pixel term goes to int64 in
 * units of 2^-52 before the work-group sum. |term| <= 2 and a work-group has
 * 128 items, so a group sum stays below 2^60, and integer addition makes the
 * sum exact and independent of the reduction order. */
constexpr float SSIM_TERM_FIXED_ONE = 0x1p52f;
constexpr double SSIM_TERM_FIXED_INV = 0x1p-52;

using vmaf_sycl_exact::div_rn;
using vmaf_sycl_exact::Ff;
using vmaf_sycl_exact::ff_add;
using vmaf_sycl_exact::ff_div;
using vmaf_sycl_exact::ff_mul;
using vmaf_sycl_exact::sqrt_rn;
using vmaf_sycl_exact::two_prod;
using vmaf_sycl_exact::two_sum;

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
    unsigned wg_count_x;
    unsigned wg_count_y;
    unsigned wg_count;

    float c1;
    float c2;

    /* SYCL state back-pointer. */
    VmafSyclState *sycl_state;

    /* Host-pinned packed raw luma staging and its device copy. */
    void *h_ref_raw;
    void *h_cmp_raw;
    void *d_ref_raw;
    void *d_cmp_raw;
    /* Device USM decimated float ref / cmp + 5 intermediates + WG partials. */
    float *d_ref;
    float *d_cmp;
    float *d_ref_mu;
    float *d_cmp_mu;
    float *d_ref_sq;
    float *d_cmp_sq;
    float *d_refcmp;
    /* Per-WG sums of the SSIM terms in units of 2^-52. */
    std::int64_t *d_partials;
    /* Host-pinned partials for D2H. */
    std::int64_t *h_partials;
    /* enable_lcs only: per-WG L / C / S sums, 3 x wg_count laid out
     * [l | c | s]; NULL otherwise. */
    std::int64_t *d_lcs_partials;
    std::int64_t *h_lcs_partials;

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
    /* Per-work-group sums of the SSIM terms in units of 2^-52. */
    std::int64_t *partials;
    /* enable_lcs kernel only: 3 x group_count sums, [l | c | s]. */
    std::int64_t *lcs_partials;
    unsigned horizontal_width;
    unsigned final_width;
    unsigned final_height;
    size_t group_columns;
    size_t group_count;
    float c1;
    float c2;
};

struct SsimMoments {
    float reference_mean;
    float comparison_mean;
    float reference_square;
    float comparison_square;
    float cross_product;
};

/* The five sums of one convolution pass, as pairs (Research-2133). */
struct MomentPairs {
    Ff reference_mean;
    Ff comparison_mean;
    Ff reference_square;
    Ff comparison_square;
    Ff cross_product;
};

/* One pixel's luminance, contrast and structure terms in the CPU's types:
 * L and C are doubles on the CPU (pairs here), S is an fp32 quotient. */
struct SsimTerms {
    Ff luminance;
    Ff contrast;
    float structure;
};

} // namespace

namespace
{

/* One tap of iqa/convolve.c: the fp32 product `img * kernel`, added to the
 * pass's sum. The CPU adds these products in double and rounds the sum to
 * fp32 once; a pair sum carries it to about 2^-46, so the rounded pair is the
 * CPU's fp32 value except within that distance of a rounding midpoint. */
inline Ff add_tap(Ff sum, float sample, float weight)
{
    const float product = sample * weight;
    return ff_add(sum, Ff{.hi = product, .lo = 0.0f});
}

/* Horizontal pass: the CPU convolves ref, cmp and the fp32 products
 * ref * ref, cmp * cmp and ref * cmp (ssim_precompute). */
inline void add_horizontal_tap(MomentPairs &sums, float ref, float cmp, float weight)
{
    const float ref_sq = ref * ref;
    const float cmp_sq = cmp * cmp;
    const float ref_cmp = ref * cmp;
    sums.reference_mean = add_tap(sums.reference_mean, ref, weight);
    sums.comparison_mean = add_tap(sums.comparison_mean, cmp, weight);
    sums.reference_square = add_tap(sums.reference_square, ref_sq, weight);
    sums.comparison_square = add_tap(sums.comparison_square, cmp_sq, weight);
    sums.cross_product = add_tap(sums.cross_product, ref_cmp, weight);
}

/* Vertical pass over the five horizontal results. */
inline void add_vertical_tap(MomentPairs &sums, const SsimMoments &row, float weight)
{
    sums.reference_mean = add_tap(sums.reference_mean, row.reference_mean, weight);
    sums.comparison_mean = add_tap(sums.comparison_mean, row.comparison_mean, weight);
    sums.reference_square = add_tap(sums.reference_square, row.reference_square, weight);
    sums.comparison_square = add_tap(sums.comparison_square, row.comparison_square, weight);
    sums.cross_product = add_tap(sums.cross_product, row.cross_product, weight);
}

/* A pass result, `(float)(sum * scale)` with scale 1 on the CPU: ff_add keeps
 * a pair normalised, so hi is hi + lo rounded to fp32. */
inline SsimMoments round_moments(const MomentPairs &sums)
{
    return {.reference_mean = sums.reference_mean.hi,
            .comparison_mean = sums.comparison_mean.hi,
            .reference_square = sums.reference_square.hi,
            .comparison_square = sums.comparison_square.hi,
            .cross_product = sums.cross_product.hi};
}

/* iqa/ssim_tools.c for one pixel (ssim_variance_scalar, then
 * ssim_accumulate_default_scalar, which ssim_accumulate_lane.h shares with the
 * SIMD paths): fp32 variances clamped at zero, the covariance, one fp32
 * square root of their product, the fp32 denominators and S, then
 * L = (2.0 * mu_ref * mu_cmp + C1) / l_den and C = (2.0 * srsc + C2) / c_den,
 * which the CPU forms in double and this twin as pairs. Every fp32 operation
 * is the CPU's, in its order: contraction is off for this TU
 * (sycl_exact_fp_sources) and the division and square root are correctly
 * rounded. There is no identical-window shortcut: on a flat identical window
 * the fp32 l_den rounds below 2 * mu^2 + C1, and the CPU keeps that. */
inline SsimTerms ssim_terms(const SsimMoments &m, float c1, float c2)
{
    const float ref_mean_sq = m.reference_mean * m.reference_mean;
    const float cmp_mean_sq = m.comparison_mean * m.comparison_mean;
    const float mean_product = m.reference_mean * m.comparison_mean;
    const float ref_var_raw = m.reference_square - ref_mean_sq;
    const float cmp_var_raw = m.comparison_square - cmp_mean_sq;
    const float ref_var = ref_var_raw < 0.0f ? 0.0f : ref_var_raw;
    const float cmp_var = cmp_var_raw < 0.0f ? 0.0f : cmp_var_raw;
    const float covariance = m.cross_product - mean_product;
    const float var_product = ref_var * cmp_var;
    const float srsc = sqrt_rn(var_product);
    const float l_den_sum = ref_mean_sq + cmp_mean_sq;
    const float l_den = l_den_sum + c1;
    const float c_den_sum = ref_var + cmp_var;
    const float c_den = c_den_sum + c2;
    const float c3 = c2 / 2.0f;
    const float flat_covariance = (covariance < 0.0f && srsc <= 0.0f) ? 0.0f : covariance;
    const float s_num = flat_covariance + c3;
    const float s_den = srsc + c3;
    const Ff product = two_prod(m.reference_mean, m.comparison_mean);
    const Ff doubled = {.hi = 2.0f * product.hi, .lo = 2.0f * product.lo};
    const Ff l_num = ff_add(doubled, Ff{.hi = c1, .lo = 0.0f});
    const Ff c_num = two_sum(2.0f * srsc, c2);
    return {.luminance = ff_div(l_num, Ff{.hi = l_den, .lo = 0.0f}),
            .contrast = ff_div(c_num, Ff{.hi = c_den, .lo = 0.0f}),
            .structure = div_rn(s_num, s_den)};
}

/* The pixel's SSIM term, `lv * cv * sv` in double on the CPU. */
inline Ff ssim_term(const SsimTerms &t)
{
    return ff_mul(ff_mul(t.luminance, t.contrast), Ff{.hi = t.structure, .lo = 0.0f});
}

/* A pair in int64 units of 2^-52, rounded to nearest: hi * 2^52 is exact and
 * integral for |hi| >= 2^-28, lo adds its rounded share. */
inline std::int64_t term_fixed(Ff value)
{
    const float hi = sycl::rint(value.hi * SSIM_TERM_FIXED_ONE);
    const float lo = sycl::rint(value.lo * SSIM_TERM_FIXED_ONE);
    return static_cast<std::int64_t>(hi) + static_cast<std::int64_t>(lo);
}

/* Exact sum of fixed-point terms of any count (host side): each is split
 * into multiples of 2^32 and a remainder, so neither half overflows, and the
 * halves join in one double rounding. */
struct FixedSum {
    std::int64_t high = 0;
    std::int64_t low = 0;

    void add(std::int64_t value)
    {
        const std::int64_t value_high = value / 0x100000000LL;
        high += value_high;
        low += value - value_high * 0x100000000LL;
    }

    [[nodiscard]] double value() const
    {
        return ((double)high * 0x1p32 + (double)low) * SSIM_TERM_FIXED_INV;
    }
};

/* ssim_init_args' C1 and C2 for L = 255, K1 = 0.01, K2 = 0.03, in fp32. */
inline void float_ssim_constants(float *c1, float *c2)
{
    const float range = 255.0f;
    const float k1 = 0.01f;
    const float k2 = 0.03f;
    *c1 = (k1 * range) * (k1 * range);
    *c2 = (k2 * range) * (k2 * range);
}

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

/* Exact work-group sum of one fixed-point term into `out`. Every work-item
 * of the group must call it (reduce_over_group). */
static inline void store_fixed_group(sycl::nd_item<2> item, const FloatVertArgs &args,
                                     std::int64_t *out, std::int64_t value)
{
    const std::int64_t sum =
        sycl::reduce_over_group(item.get_group(), value, sycl::plus<std::int64_t>{});
    if (item.get_local_id(0) == 0 && item.get_local_id(1) == 0) {
        out[item.get_group(0) * args.group_columns + item.get_group(1)] = sum;
    }
}

} // namespace

namespace
{

static sycl::nd_range<2> vert_combine_range(const FloatVertArgs &args)
{
    const size_t global_x = ((args.final_width + SSIM_WG_X - 1) / SSIM_WG_X) * SSIM_WG_X;
    const size_t global_y = ((args.final_height + SSIM_WG_Y - 1) / SSIM_WG_Y) * SSIM_WG_Y;
    return sycl::nd_range<2>{sycl::range<2>{global_y, global_x},
                             sycl::range<2>{SSIM_WG_Y, SSIM_WG_X}};
}

/* Pass 2: vertical moments, the CPU's per-pixel SSIM term and its exact
 * work-group sum; out-of-frame work-items contribute zero. */
static void launch_vert_combine(sycl::queue &queue, const FloatVertArgs &args)
{
    sycl::nd_range<2> const range = vert_combine_range(args);
    queue.submit([=](sycl::handler &handler) {
        handler.parallel_for(range, [=](sycl::nd_item<2> item) {
            const size_t x = item.get_global_id(1);
            const size_t y = item.get_global_id(0);
            std::int64_t value = 0;
            if (x < args.final_width && y < args.final_height) {
                const SsimTerms terms = ssim_terms(vertical_moments(args, x, y), args.c1, args.c2);
                value = term_fixed(ssim_term(terms));
            }
            store_fixed_group(item, args, args.partials, value);
        });
    });
}

/* enable_lcs variant: the same terms feed the SSIM sum and the L / C / S
 * sums, stored as [l | c | s] rows of group_count each. */
static void launch_vert_combine_lcs(sycl::queue &queue, const FloatVertArgs &args)
{
    sycl::nd_range<2> const range = vert_combine_range(args);
    queue.submit([=](sycl::handler &handler) {
        handler.parallel_for(range, [=](sycl::nd_item<2> item) {
            const size_t x = item.get_global_id(1);
            const size_t y = item.get_global_id(0);
            std::int64_t value = 0;
            std::int64_t luminance = 0;
            std::int64_t contrast = 0;
            std::int64_t structure = 0;
            if (x < args.final_width && y < args.final_height) {
                const SsimTerms terms = ssim_terms(vertical_moments(args, x, y), args.c1, args.c2);
                value = term_fixed(ssim_term(terms));
                luminance = term_fixed(terms.luminance);
                contrast = term_fixed(terms.contrast);
                structure = term_fixed(Ff{.hi = terms.structure, .lo = 0.0f});
            }
            store_fixed_group(item, args, args.partials, value);
            store_fixed_group(item, args, args.lcs_partials, luminance);
            store_fixed_group(item, args, args.lcs_partials + args.group_count, contrast);
            store_fixed_group(item, args, args.lcs_partials + 2U * args.group_count, structure);
        });
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
    s->wg_count_x = (s->w_final + (unsigned)SSIM_WG_X - 1) / (unsigned)SSIM_WG_X;
    s->wg_count_y = (s->h_final + (unsigned)SSIM_WG_Y - 1) / (unsigned)SSIM_WG_Y;
    s->wg_count = s->wg_count_x * s->wg_count_y;
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
    const size_t partials_bytes = (size_t)s->wg_count * sizeof(std::int64_t);
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
    s->d_partials = allocate_device<std::int64_t>(s->sycl_state, partials_bytes);
    s->h_partials = allocate_host<std::int64_t>(s->sycl_state, partials_bytes);
    if (s->enable_lcs) {
        s->d_lcs_partials = allocate_device<std::int64_t>(s->sycl_state, 3U * partials_bytes);
        s->h_lcs_partials = allocate_host<std::int64_t>(s->sycl_state, 3U * partials_bytes);
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

} // namespace

namespace
{

static bool float_ssim_allocations_complete(const SsimStateSycl *s)
{
    const bool lcs_complete = !s->enable_lcs || (s->d_lcs_partials && s->h_lcs_partials);
    return s->h_ref_raw && s->h_cmp_raw && s->d_ref_raw && s->d_cmp_raw && s->d_ref && s->d_cmp &&
           s->d_ref_mu && s->d_cmp_mu && s->d_ref_sq && s->d_cmp_sq && s->d_refcmp &&
           s->d_partials && s->h_partials && lcs_complete;
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

/* Pass 2 and the partials read-back; `enable_lcs` selects the L/C/S kernel
 * and reads its three extra partial rows back as well. */
static void enqueue_float_vertical(SsimStateSycl *s, sycl::queue &q)
{
    const FloatVertArgs vert_args{.reference_mean = s->d_ref_mu,
                                  .comparison_mean = s->d_cmp_mu,
                                  .reference_square = s->d_ref_sq,
                                  .comparison_square = s->d_cmp_sq,
                                  .cross_product = s->d_refcmp,
                                  .partials = s->d_partials,
                                  .lcs_partials = s->d_lcs_partials,
                                  .horizontal_width = s->w_horiz,
                                  .final_width = s->w_final,
                                  .final_height = s->h_final,
                                  .group_columns = s->wg_count_x,
                                  .group_count = s->wg_count,
                                  .c1 = s->c1,
                                  .c2 = s->c2};
    const size_t partials_bytes = (size_t)s->wg_count * sizeof(std::int64_t);
    if (s->enable_lcs) {
        launch_vert_combine_lcs(q, vert_args);
        q.memcpy(s->h_lcs_partials, s->d_lcs_partials, 3U * partials_bytes);
    } else {
        launch_vert_combine(q, vert_args);
    }
    q.memcpy(s->h_partials, s->d_partials, partials_bytes);
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
    stage_raw_luma(s, ref_pic, s->h_ref_raw);
    stage_raw_luma(s, dist_pic, s->h_cmp_raw);
    const size_t raw_bytes = (size_t)s->width * s->height * s->sample_bytes;
    q.memcpy(s->d_ref_raw, s->h_ref_raw, raw_bytes);
    q.memcpy(s->d_cmp_raw, s->h_cmp_raw, raw_bytes);
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

/* The exact sum of the per-WG fixed-point partials, as a double. */
static double sum_partials(const std::int64_t *partials, unsigned count)
{
    FixedSum total;
    for (unsigned i = 0; i < count; i++)
        total.add(partials[i]);
    return total.value();
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

/* enable_lcs: the three per-WG L / C / S partial rows become the frame means
 * float_ssim_{l,c,s}, published with the score in CPU float_ssim.c order
 * after the shared SSIM validation (ADR-1302). */
static int emit_float_ssim_lcs(const SsimStateSycl *s, double score, double n_pixels,
                               unsigned index, VmafFeatureCollector *feature_collector)
{
    static const char *const atom_names[3] = {"float_ssim_l", "float_ssim_c", "float_ssim_s"};
    VmafNamedScore atoms[3];
    int err = 0;
    for (unsigned k = 0; k < 3U && !err; k++) {
        const double sum = sum_partials(s->h_lcs_partials + (size_t)k * s->wg_count, s->wg_count);
        atoms[k].name = atom_names[k];
        err = float_ssim_frame_mean(atom_names[k], sum, n_pixels, index, &atoms[k].value);
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

    /* Exact sum of the per-WG fixed-point partials -> mean SSIM over
     * (W'-10)·(H'-10) decimated pixels, rounded to fp32 like the CPU. */
    const double total = sum_partials(s->h_partials, s->wg_count);
    const double n_pixels = (double)s->w_final * (double)s->h_final;
    double score = 0.0;
    const int err = float_ssim_frame_mean("float_ssim", total, n_pixels, index, &score);
    if (err)
        return err;
    if (!s->enable_lcs) {
        return vmaf_ssim_emit_score_named(feature_collector, s->feature_name_dict,
                                          "float_ssim_sycl", "float_ssim", score, s->enable_db,
                                          s->max_db, index);
    }
    return emit_float_ssim_lcs(s, score, n_pixels, index, feature_collector);
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

/* Host copy of pass 2 and the fixed-point sums of SSIM, L, C and S. */
static void host_vertical_sums(const std::vector<SsimMoments> &rows, unsigned w_out, unsigned h_out,
                               FixedSum totals[4])
{
    float c1 = 0.0f;
    float c2 = 0.0f;
    float_ssim_constants(&c1, &c2);
    for (unsigned y = 0; y < h_out; ++y) {
        for (unsigned x = 0; x < w_out; ++x) {
            MomentPairs sums{};
            for (int tap = 0; tap < SSIM_K; ++tap)
                add_vertical_tap(sums, rows[((size_t)y + (size_t)tap) * w_out + x], G[tap]);
            const SsimTerms terms = ssim_terms(round_moments(sums), c1, c2);
            totals[0].add(term_fixed(ssim_term(terms)));
            totals[1].add(term_fixed(terms.luminance));
            totals[2].add(term_fixed(terms.contrast));
            totals[3].add(term_fixed(Ff{.hi = terms.structure, .lo = 0.0f}));
        }
    }
}

} // namespace

/* Test hook (core/test/test_sycl_float_ssim_math.c): the float_ssim pipeline
 * after decimation, run on the host with the kernels' own arithmetic
 * (add_horizontal_tap, add_vertical_tap, ssim_terms, term_fixed). The group
 * sums on the device are exact integer sums, so the host result is the
 * device's for the same per-pixel values; the test compares it with
 * iqa_ssim() without a device. `means` receives the fp32-rounded frame means
 * of SSIM, L, C and S. Returns 0, -EINVAL for a plane smaller than the
 * 11 x 11 window, or -ENOMEM. */
extern "C" int vmaf_sycl_float_ssim_host_means(const float *reference, const float *comparison,
                                               unsigned width, unsigned height, double means[4])
{
    if (!reference || !comparison || !means || width < (unsigned)SSIM_K ||
        height < (unsigned)SSIM_K)
        return -EINVAL;
    const unsigned w_out = width - (unsigned)(SSIM_K - 1);
    const unsigned h_out = height - (unsigned)(SSIM_K - 1);
    FixedSum totals[4];
    try {
        std::vector<SsimMoments> rows((size_t)w_out * height);
        host_horizontal_pass(reference, comparison, width, height, rows);
        host_vertical_sums(rows, w_out, h_out, totals);
    } catch (const std::bad_alloc &) {
        return -ENOMEM;
    }
    const double n_pixels = (double)w_out * (double)h_out;
    for (unsigned k = 0; k < 4U; ++k)
        means[k] = (double)(float)(totals[k].value() / n_pixels);
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
        release_buffer(s->sycl_state, s->d_partials);
        release_buffer(s->sycl_state, s->h_partials);
        release_buffer(s->sycl_state, s->d_lcs_partials);
        release_buffer(s->sycl_state, s->h_lcs_partials);
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
 * Real integer_ssim SYCL extractor (ADR-0564)
 *
 * Bit-exact int64 moment accumulation matching the CPU integer_ssim.c.
 * Uses the same 9-tap integer Gaussian [2,9,28,55,68,55,28,9,2] and
 * boundary-truncation as the CPU.
 *
 * fp64 constraint (ADR-0220 / AGENTS.md): no double inside kernel
 * lambdas. The SSIM formula is computed in float32 per-pixel, then
 * float partials are accumulated on the host in double.  This gives
 * places=4-5 vs CPU (not places=6), documented in ADR-0564.
 *
 * Two-pass design:
 *   Pass 1 (launch_issim_horiz): 9-tap int64 horizontal moment
 *     accumulation. Writes 6 x (W x H) int64 USM arrays.
 *   Pass 2 (launch_issim_vert_combine): 9-tap int64 vertical
 *     accumulation from horiz arrays, then float SSIM formula,
 *     then float per-WG partial sum + int64 per-WG weight sum.
 * Host: ssim = sum(float_partials * wgt_partials) / sum(wgt_partials).
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

    unsigned wg_count_x;
    unsigned wg_count_y;
    unsigned wg_count;

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

    /* Six int64 intermediate device arrays for horizontal pass. */
    int64_t *d_mux;
    int64_t *d_muy;
    int64_t *d_x2;
    int64_t *d_xy;
    int64_t *d_y2;
    int64_t *d_w;

    /* float per-WG ssim partial sum. */
    float *d_partials;
    float *h_partials;
    /* int64 per-WG weight partial sum. */
    int64_t *d_wgt;
    int64_t *h_wgt;

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
                                    int64_t *d_y2, int64_t *d_w, unsigned width, unsigned height)
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
                int64_t w = 0LL;
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
                    w += wk;
                }
                const size_t idx = (size_t)y * e_width + x;
                d_mux[idx] = mux;
                d_muy[idx] = muy;
                d_x2[idx] = x2;
                d_xy[idx] = xy;
                d_y2[idx] = y2;
                d_w[idx] = w;
            });
    });
}

} // namespace

/* Pass 1 (>8bpc): same as above but reads uint16_t. */
namespace
{

static void launch_issim_horiz_16bpc(sycl::queue &q, const uint16_t *d_ref, const uint16_t *d_cmp,
                                     int64_t *d_mux, int64_t *d_muy, int64_t *d_x2, int64_t *d_xy,
                                     int64_t *d_y2, int64_t *d_w, unsigned width, unsigned height)
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
                int64_t w = 0LL;
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
                    w += wk;
                }
                const size_t idx = (size_t)y * e_width + x;
                d_mux[idx] = mux;
                d_muy[idx] = muy;
                d_x2[idx] = x2;
                d_xy[idx] = xy;
                d_y2[idx] = y2;
                d_w[idx] = w;
            });
    });
}

} // namespace

namespace
{

struct IntegerVertArgs {
    const int64_t *reference_mean;
    const int64_t *comparison_mean;
    const int64_t *reference_square;
    const int64_t *cross_product;
    const int64_t *comparison_square;
    const int64_t *weight;
    float *partials;
    int64_t *weight_partials;
    unsigned width;
    unsigned height;
    float sample_max;
    size_t group_columns;
};

struct IntegerMoments {
    int64_t reference_mean;
    int64_t comparison_mean;
    int64_t reference_square;
    int64_t cross_product;
    int64_t comparison_square;
    int64_t weight;
};

struct IssimContribution {
    float weighted_score;
    float weight;
};

} // namespace

namespace
{

static inline IntegerMoments vertical_integer_moments(const IntegerVertArgs &args, unsigned x,
                                                      unsigned y)
{
    const int first = (int)y < ISSIM_HALF_K ? ISSIM_HALF_K - (int)y : 0;
    const int last = ((int)y + ISSIM_HALF_K >= (int)args.height) ?
                         ISSIM_K_SZ - ((int)y + ISSIM_HALF_K - (int)args.height + 1) :
                         ISSIM_K_SZ;
    IntegerMoments result{};
    for (int tap = first; tap < last; ++tap) {
        const unsigned source_y = (unsigned)((int)y - ISSIM_HALF_K + tap);
        const size_t index = (size_t)source_y * args.width + x;
        const int64_t coefficient = (int64_t)ISSIM_KERNEL[tap];
        result.reference_mean += coefficient * args.reference_mean[index];
        result.comparison_mean += coefficient * args.comparison_mean[index];
        result.reference_square += coefficient * args.reference_square[index];
        result.cross_product += coefficient * args.cross_product[index];
        result.comparison_square += coefficient * args.comparison_square[index];
        result.weight += coefficient * args.weight[index];
    }
    return result;
}

} // namespace

namespace
{

static inline IssimContribution integer_ssim_contribution(const IntegerVertArgs &args, unsigned x,
                                                          unsigned y)
{
    const IntegerMoments moments = vertical_integer_moments(args, x, y);
    const float weight = (float)moments.weight;
    const float c1 = args.sample_max * args.sample_max * 0.0001f * weight * weight;
    const float c2 = args.sample_max * args.sample_max * 0.0009f * weight * weight;
    const float reference_mean = (float)moments.reference_mean;
    const float comparison_mean = (float)moments.comparison_mean;
    /* The CPU integer_ssim.c::ssim_reduce_row_range groups the term as
     * ((weight * a) * b) / denominator: on border pixels, where weight is
     * not a power of two, the two round differently. */
    const float reference_mean_sq = reference_mean * reference_mean;
    const float comparison_mean_sq = comparison_mean * comparison_mean;
    const float mean_product = reference_mean * comparison_mean;
    const float reference_weighted = (float)moments.reference_square * weight;
    const float comparison_weighted = (float)moments.comparison_square * weight;
    const float cross_weighted = (float)moments.cross_product * weight;
    const float reference_variance = reference_weighted - reference_mean_sq;
    const float comparison_variance = comparison_weighted - comparison_mean_sq;
    const float covariance = cross_weighted - mean_product;
    const float a = 2.0f * mean_product + c1;
    const float b = 2.0f * covariance + c2;
    const float denominator = (reference_mean_sq + comparison_mean_sq + c1) *
                              (reference_variance + comparison_variance + c2);
    if (denominator == 0.0f || moments.weight <= 0LL) {
        return {};
    }
    const float weighted_score = ((weight * a) * b) / denominator;
    return {.weighted_score = weighted_score, .weight = weight};
}

} // namespace

namespace
{

static inline void store_integer_group(sycl::nd_item<2> item, const IntegerVertArgs &args,
                                       IssimContribution contribution)
{
    const float score =
        sycl::reduce_over_group(item.get_group(), contribution.weighted_score, sycl::plus<float>{});
    const float weight =
        sycl::reduce_over_group(item.get_group(), contribution.weight, sycl::plus<float>{});
    if (item.get_local_id(0) == 0 && item.get_local_id(1) == 0) {
        const size_t index = item.get_group(0) * args.group_columns + item.get_group(1);
        args.partials[index] = score;
        args.weight_partials[index] = (int64_t)weight;
    }
}

} // namespace

namespace
{

static void launch_issim_vert_combine(sycl::queue &queue, const IntegerVertArgs &args)
{
    const size_t global_x = ((args.width + ISSIM_WG_X - 1) / ISSIM_WG_X) * ISSIM_WG_X;
    const size_t global_y = ((args.height + ISSIM_WG_Y - 1) / ISSIM_WG_Y) * ISSIM_WG_Y;
    sycl::nd_range<2> const range{sycl::range<2>{global_y, global_x},
                                  sycl::range<2>{ISSIM_WG_Y, ISSIM_WG_X}};
    queue.submit([=](sycl::handler &handler) {
        handler.parallel_for(range, [=](sycl::nd_item<2> item) {
            const unsigned x = (unsigned)item.get_global_id(1);
            const unsigned y = (unsigned)item.get_global_id(0);
            IssimContribution contribution{};
            if (x < args.width && y < args.height) {
                contribution = integer_ssim_contribution(args, x, y);
            }
            store_integer_group(item, args, contribution);
        });
    });
}

} // namespace

namespace
{

struct IntegerBufferSizes {
    size_t pixels8;
    size_t pixels16;
    size_t moments;
    size_t partials;
    size_t weights;
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
    s->wg_count_x = (unsigned)((width + ISSIM_WG_X - 1) / ISSIM_WG_X);
    s->wg_count_y = (unsigned)((height + ISSIM_WG_Y - 1) / ISSIM_WG_Y);
    s->wg_count = s->wg_count_x * s->wg_count_y;
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
        .partials = (size_t)s->wg_count * sizeof(float),
        .weights = (size_t)s->wg_count * sizeof(int64_t),
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
    s->d_w = allocate_device<int64_t>(s->sycl_state, bytes.moments);
    s->d_partials = allocate_device<float>(s->sycl_state, bytes.partials);
    s->h_partials = allocate_host<float>(s->sycl_state, bytes.partials);
    s->d_wgt = allocate_device<int64_t>(s->sycl_state, bytes.weights);
    s->h_wgt = allocate_host<int64_t>(s->sycl_state, bytes.weights);
}

} // namespace

namespace
{

static bool integer_ssim_allocations_complete(const IssimStateSycl *s)
{
    return s->h_ref_u8 && s->h_cmp_u8 && s->h_ref_u16 && s->h_cmp_u16 && s->d_ref_u8 &&
           s->d_cmp_u8 && s->d_ref_u16 && s->d_cmp_u16 && s->d_mux && s->d_muy && s->d_x2 &&
           s->d_xy && s->d_y2 && s->d_w && s->d_partials && s->h_partials && s->d_wgt && s->h_wgt;
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

template <typename Picture>
static void submit_integer_horizontal(IssimStateSycl *s, sycl::queue &queue, Picture *reference,
                                      Picture *comparison)
{
    const size_t pixels = (size_t)s->width * s->height;
    if (s->bpc == 8u) {
        pack_integer_plane(s->h_ref_u8, reference, s->width, s->height);
        pack_integer_plane(s->h_cmp_u8, comparison, s->width, s->height);
        queue.memcpy(s->d_ref_u8, s->h_ref_u8, pixels * sizeof(uint8_t));
        queue.memcpy(s->d_cmp_u8, s->h_cmp_u8, pixels * sizeof(uint8_t));
        launch_issim_horiz_8bpc(queue, s->d_ref_u8, s->d_cmp_u8, s->d_mux, s->d_muy, s->d_x2,
                                s->d_xy, s->d_y2, s->d_w, s->width, s->height);
        return;
    }
    pack_integer_plane(s->h_ref_u16, reference, s->width, s->height);
    pack_integer_plane(s->h_cmp_u16, comparison, s->width, s->height);
    queue.memcpy(s->d_ref_u16, s->h_ref_u16, pixels * sizeof(uint16_t));
    queue.memcpy(s->d_cmp_u16, s->h_cmp_u16, pixels * sizeof(uint16_t));
    launch_issim_horiz_16bpc(queue, s->d_ref_u16, s->d_cmp_u16, s->d_mux, s->d_muy, s->d_x2,
                             s->d_xy, s->d_y2, s->d_w, s->width, s->height);
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

    submit_integer_horizontal(s, q, ref_pic, dist_pic);
    launch_issim_vert_combine(q, {.reference_mean = s->d_mux,
                                  .comparison_mean = s->d_muy,
                                  .reference_square = s->d_x2,
                                  .cross_product = s->d_xy,
                                  .comparison_square = s->d_y2,
                                  .weight = s->d_w,
                                  .partials = s->d_partials,
                                  .weight_partials = s->d_wgt,
                                  .width = s->width,
                                  .height = s->height,
                                  .sample_max = (float)((1u << s->bpc) - 1u),
                                  .group_columns = s->wg_count_x});

    q.memcpy(s->h_partials, s->d_partials, (size_t)s->wg_count * sizeof(float));
    q.memcpy(s->h_wgt, s->d_wgt, (size_t)s->wg_count * sizeof(int64_t));

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

    double total_ssim = 0.0;
    int64_t total_wgt = 0LL;
    for (unsigned i = 0; i < s->wg_count; i++) {
        total_ssim += (double)s->h_partials[i];
        total_wgt += s->h_wgt[i];
    }
    return vmaf_ssim_emit_ratio_score_named(feature_collector, s->feature_name_dict,
                                            "integer_ssim_sycl", "ssim", total_ssim,
                                            (double)total_wgt, s->enable_db, s->max_db, index);
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
        release_buffer(s->sycl_state, s->d_w);
        release_buffer(s->sycl_state, s->d_partials);
        release_buffer(s->sycl_state, s->h_partials);
        release_buffer(s->sycl_state, s->d_wgt);
        release_buffer(s->sycl_state, s->h_wgt);
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

/* Real integer_ssim SYCL extractor (ADR-0564). Uses 9-tap int64 moments
 * matching the CPU algorithm. The SSIM formula is computed in float32
 * (fp64-free constraint, ADR-0220); expected precision is places=4-5 vs
 * CPU. Load-bearing: declared via extern in feature_extractor.c. */
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
