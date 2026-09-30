/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  CAMBI banding-detection feature extractor on the SYCL backend
 *  (T3-15 / ADR-0415), fully device-resident since ADR-1357.
 *
 *  Per-frame flow — every stage runs on the device, in order, on the
 *  combined in-order queue (vmaf_sycl_graph_register), reading the distorted
 *  luma plane the shared-frame upload already placed on the device:
 *
 *    1. reset         : zero the per-scale selection state and the status word.
 *    2. validate      : flag samples above the declared bit depth (only for
 *                       bpc other than 8 and 16, as cambi.c::validate_image).
 *    3. preprocess    : convert to 10 bit (and resize to enc_width x enc_height
 *                       through init-time index tables), then the anti-dither
 *                       2x2 average when enc_bitdepth < 10.
 *    4. spatial mask  : derivative + zero-padded 7x7 box sum + threshold, one
 *                       local-memory tile per work-group.
 *    5. per scale s   : decimate (s > 0 or high-res speed-up), 3-tap mode filter
 *                       horizontal then vertical — the vertical pass also emits
 *                       the compact level map Q — then the per-row run/change
 *                       bit masks, c-values (+ top-K pass 0), top-K pooling.
 *    6. readback      : one D2H copy of the five per-scale top-K sums and the
 *                       status word (post_fn); collect() weights the scales.
 *
 *  c-values (cvals_column): one work-item owns one histogram column of one
 *  row chunk and slides the (2 * pad + 1)^2 window down its rows as
 *  cambi.c::calculate_c_values slides its column histograms, so every cell it
 *  reads holds the true window count of that level (modular uint16 updates in
 *  any order give the same final count; init rejects any window above
 *  65 x 65 exactly as cambi.c does, so no count exceeds 4225). Rows
 *  whose leaving and entering segments agree are skipped and a changed
 *  segment is applied one run of equal levels at a time (bit masks from
 *  launch_row_masks). The per-pixel formula is c_value_pixel()'s, float for
 *  float, with the same reciprocal table (vmaf_cambi_reciprocal_lut).
 *
 *  Top-K pooling: cambi.c sums the k largest c-values in double after a
 *  quick-select. The device finds the k-th largest value T with a 3-pass
 *  radix select on the IEEE bit patterns (monotonic for the non-negative
 *  c-values; pass 0 is counted by the c-values kernel) and sums
 *  sum(v > T) + (k - #(v > T)) * T exactly, as a 128-bit integer in units of
 *  2^-24: every non-zero c-value is at least 0.5 and below 2^14, so it is an
 *  integer multiple of 2^-24 and the sum is exact. The host converts it to
 *  double once. That equals cambi.c's double sum whenever that sum is exact
 *  (always below 2^29, and in practice far beyond); otherwise it differs from
 *  it by the CPU's own accumulated rounding (a few ulp of the score). See
 *  ADR-1357 for the proof and the measured deltas.
 *
 *  Precision contract: integer-only mask/decimate/filter/histogram stages,
 *  fp32 c-values bit-identical to cambi.c, exact top-K sums. No kernel uses
 *  fp64 (ADR-0220).
 */

#include <sycl/sycl.hpp>

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

#include "config.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature_name.h"
#include "log.h"
#include "luminance_tools.h"
#include "picture.h"
#include "sycl/common.h"
#include "feature/cambi_internal.h"

/* ------------------------------------------------------------------ */
/* Constants (mirroring cambi.c / integer_cambi_cuda.c).               */
/* ------------------------------------------------------------------ */
namespace
{

constexpr int CAMBI_SYCL_NUM_SCALES = VMAF_CAMBI_NUM_SCALES;
constexpr double CAMBI_SYCL_DEFAULT_MAX_VAL = 1000.0;
constexpr int CAMBI_SYCL_DEFAULT_WINDOW_SIZE = 65;
constexpr double CAMBI_SYCL_DEFAULT_TOPK = 0.6;
constexpr double CAMBI_SYCL_DEFAULT_TVI = 0.019;
constexpr double CAMBI_SYCL_DEFAULT_VLT = 0.0;
constexpr int CAMBI_SYCL_DEFAULT_MAX_LOG_CONTRAST = 2;
/* `default_val.s` in `VmafOption` is declared `char *` (not `const char *`);
 * use a `char[]` so the array decays to `char *` without a const cast.
 * Mirrors the CUDA twin `CAMBI_CUDA_DEFAULT_EOTF` which uses a `#define`
 * macro for the same reason. */
char CAMBI_SYCL_DEFAULT_EOTF[] = "bt1886";

/* Work-group tile for the 2-D image kernels. */
constexpr size_t WG_X = 16;
constexpr size_t WG_Y = 16;

/* c-values: work-items per work-group along the columns of one row chunk,
 * and the shortest row chunk worth re-priming a window for. */
constexpr size_t CVALS_WG = 64;
constexpr unsigned CVALS_MIN_CHUNK_ROWS = 32U;
/* Upper bound on the per-chunk column histograms (chunks x width x levels
 * uint16 cells) — more chunks stop paying off well before this. */
constexpr size_t CVALS_HIST_BUDGET = (size_t)64U << 20U;
/* Level map value of a pixel that neither contributes to nor queries a
 * histogram: masked out, or outside the [v_band_base, v_band_base +
 * v_band_size) band calculate_c_values() keeps. */
constexpr uint16_t CAMBI_Q_INVALID = 0xFFFFU;

/* Top-K radix select: 11 + 11 + 10 bits over the IEEE pattern. */
constexpr unsigned RADIX_BINS = 2048U;
constexpr int RADIX_PASSES = 3;
constexpr uint32_t RADIX_KNOWN_MASK[RADIX_PASSES] = {0U, 0xFFE00000U, 0xFFFFFC00U};
constexpr unsigned RADIX_SHIFT[RADIX_PASSES] = {21U, 10U, 0U};
constexpr uint32_t RADIX_BIN_MASK[RADIX_PASSES] = {0x7FFU, 0x7FFU, 0x3FFU};
constexpr size_t POOL_WG = 256;
constexpr unsigned RADIX_BINS_PER_LANE = RADIX_BINS / (unsigned)POOL_WG;
constexpr unsigned POOL_MAX_GROUPS = 512U;
/* Elements per pooling work-group (before the POOL_MAX_GROUPS clamp). Even
 * clamped, a group of a 7680 x 7680 frame holds under 2^17 elements, so its
 * partial sum of fixed-point c-values (each < 2^38) stays below 2^55. */
constexpr unsigned POOL_ELEMS_PER_GROUP = 4096U;
/* c-value -> fixed point: every non-zero c-value is a multiple of 2^-24
 * (VMAF_CAMBI_TOPK_FIXED_SHIFT). */
constexpr float CAMBI_FIXED_SCALE = 16777216.0F;
static_assert(CAMBI_FIXED_SCALE == (float)(1U << VMAF_CAMBI_TOPK_FIXED_SHIFT),
              "device fixed point matches vmaf_cambi_fixed_topk_mean()");

/* Status bits read back with the per-scale sums. */
constexpr uint32_t CAMBI_STATUS_INVALID_INPUT = 1U;

} // namespace

/* ------------------------------------------------------------------ */
/* Device-shared structures                                            */
/* ------------------------------------------------------------------ */
namespace
{

/* Per-scale radix-select working state (device only). k_rem[p] is the rank
 * pass p looks for inside the bucket chosen so far; the scan of pass p writes
 * k_rem[p + 1], so no lane rewrites a word another lane still reads. */
struct CambiSyclSelect {
    uint32_t hist[RADIX_BINS];
    uint32_t prefix;
    uint32_t k_rem[RADIX_PASSES + 1];
    /* Set when pass 0 lands in bin 0: that bin holds only exact zeros (every
     * non-zero c-value is >= 0.5), so the threshold is 0 and the later passes
     * and the partial-sum pass have nothing left to do. */
    uint32_t resolved;
};

/* Everything collect() reads back, in one D2H copy. */
struct CambiSyclResults {
    uint64_t sum_lo[CAMBI_SYCL_NUM_SCALES];
    uint64_t sum_hi[CAMBI_SYCL_NUM_SCALES];
    uint32_t status;
    uint32_t reserved;
};

/* Init-time geometry of one scale. */
struct CambiScaleGeom {
    unsigned width;
    unsigned height;
    unsigned chunks;
    unsigned chunk_rows;
    unsigned cvals_groups;
    unsigned pool_groups;
    unsigned topk;
};

using GlobalCounter =
    sycl::atomic_ref<uint32_t, sycl::memory_order::relaxed, sycl::memory_scope::device,
                     sycl::access::address_space::global_space>;

/* Fixed point in units of 2^-24; exact for 0 and for every value in
 * [0.5, 2^14), the whole c-value range (ADR-1357). */
inline uint64_t cambi_fixed(float value)
{
    return (uint64_t)(value * CAMBI_FIXED_SCALE);
}

} // namespace

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */
namespace
{

struct CambiStateSycl {
    VmafSyclState *sycl_state;

    /* Image-pipeline device buffers (proc_width x proc_height, uint16). */
    uint16_t *d_image;
    uint16_t *d_mask;
    uint16_t *d_tmp;
    uint16_t *d_q;
    uint32_t *d_runs;
    uint32_t *d_change;
    float *d_cvals;
    uint16_t *d_hist;

    /* Init-time constant tables. */
    float *d_lut;
    uint16_t *d_tvi;
    int *d_weights;
    uint32_t *d_ori_x;
    uint32_t *d_ori_y;

    /* Pooling state and the readback block. */
    CambiSyclSelect *d_select;
    uint64_t *d_partials;
    CambiSyclResults *d_results;
    CambiSyclResults *h_results;

    /* Configuration options. */
    int enc_width;
    int enc_height;
    int enc_bitdepth;
    int max_log_contrast;
    int window_size;
    double topk;
    double cambi_topk;
    double tvi_threshold;
    double cambi_max_val;
    double cambi_vis_lum_threshold;
    char *eotf;
    char *cambi_eotf;
    int cambi_high_res_speedup;

    /* Resolved geometry. */
    unsigned src_width;
    unsigned src_height;
    unsigned src_bpc;
    unsigned proc_width;
    unsigned proc_height;
    unsigned num_diffs;
    unsigned levels;
    unsigned mask_index;
    uint16_t adjusted_window;
    uint16_t vlt_luma;
    uint16_t v_band_base;
    uint16_t v_band_size;
    CambiScaleGeom geom[CAMBI_SYCL_NUM_SCALES];

    bool registered;
    VmafDictionary *feature_name_dict;
};

} // namespace

/* ------------------------------------------------------------------ */
/* Geometry helpers (mirror cambi.c's static helpers).                 */
/* ------------------------------------------------------------------ */
namespace
{

/* Element offset of (row, column 0) in a plane of `pitch` elements, kept in
 * 32 bits on purpose: every plane here holds fewer than 2^32 elements (enc
 * dimensions are capped at 7680 x 7680), and 32-bit index arithmetic is
 * native on every Intel GPU while 64-bit multiplies are not. */
inline unsigned plane_offset(unsigned row, unsigned pitch)
{
    return row * pitch;
}

sycl::nd_range<2> image_range(unsigned width, unsigned height)
{
    const size_t global_x = ((size_t)width + WG_X - 1U) / WG_X * WG_X;
    const size_t global_y = ((size_t)height + WG_Y - 1U) / WG_Y * WG_Y;
    return sycl::nd_range<2>{sycl::range<2>{global_y, global_x}, sycl::range<2>{WG_Y, WG_X}};
}

} // namespace

/* ------------------------------------------------------------------ */
/* Kernel: preprocessing (cambi.c::cambi_preprocessing).               */
/* ------------------------------------------------------------------ */
namespace
{

struct PreprocArgs {
    const void *src;
    uint16_t *dst;
    const uint32_t *ori_x;
    const uint32_t *ori_y;
    unsigned in_w;
    unsigned out_w;
    unsigned out_h;
    unsigned bpc;
    bool same_size;
    bool anti_dither;
};

/* One output sample of decimate_generic_*_and_convert_to_10b. */
inline unsigned preproc_sample(const PreprocArgs &a, unsigned i, unsigned j)
{
    const unsigned row = a.same_size ? i : a.ori_y[i];
    const unsigned col = a.same_size ? j : a.ori_x[j];
    const unsigned off = row * a.in_w + col;
    if (a.bpc <= 8U) {
        return (unsigned)static_cast<const uint8_t *>(a.src)[off] << (10U - a.bpc);
    }
    const unsigned v = static_cast<const uint16_t *>(a.src)[off];
    if (a.bpc == 9U) {
        return v << 1U;
    }
    const unsigned shift = a.bpc - 10U;
    const unsigned rounding = shift == 0U ? 0U : 1U << (shift - 1U);
    return (v + rounding) >> shift;
}

/* anti_dithering_filter(): the in-place row-major pass only ever reads samples
 * it has not yet overwritten, so it equals this out-of-place 2x2 average. */
inline uint16_t preproc_pixel(const PreprocArgs &a, unsigned i, unsigned j)
{
    const unsigned here = preproc_sample(a, i, j);
    if (!a.anti_dither) {
        return (uint16_t)here;
    }
    const bool last_row = i + 1U == a.out_h;
    const bool last_col = j + 1U == a.out_w;
    if (last_row && last_col) {
        return (uint16_t)here;
    }
    if (last_row) {
        return (uint16_t)((here + preproc_sample(a, i, j + 1U)) >> 1);
    }
    if (last_col) {
        return (uint16_t)((here + preproc_sample(a, i + 1U, j)) >> 1);
    }
    const unsigned sum = here + preproc_sample(a, i, j + 1U) + preproc_sample(a, i + 1U, j) +
                         preproc_sample(a, i + 1U, j + 1U);
    return (uint16_t)(sum >> 2);
}

void launch_preprocess(sycl::queue &queue, const PreprocArgs &args)
{
    const PreprocArgs a = args;
    queue.submit([=](sycl::handler &handler) {
        handler.parallel_for(image_range(a.out_w, a.out_h), [=](sycl::nd_item<2> item) {
            const auto x = (unsigned)item.get_global_id(1);
            const auto y = (unsigned)item.get_global_id(0);
            if (x < a.out_w && y < a.out_h) {
                a.dst[y * a.out_w + x] = preproc_pixel(a, y, x);
            }
        });
    });
}

/* cambi.c::validate_image: flag any sample above (1 << bpc) - 1. */
void launch_validate(sycl::queue &queue, const void *src, unsigned width, unsigned height,
                     unsigned bpc, CambiSyclResults *results)
{
    const unsigned max_val = (1U << bpc) - 1U;
    queue.submit([=](sycl::handler &handler) {
        handler.parallel_for(image_range(width, height), [=](sycl::nd_item<2> item) {
            const auto x = (unsigned)item.get_global_id(1);
            const auto y = (unsigned)item.get_global_id(0);
            if (x >= width || y >= height) {
                return;
            }
            const unsigned off = y * width + x;
            const unsigned v = bpc <= 8U ? (unsigned)static_cast<const uint8_t *>(src)[off] :
                                           (unsigned)static_cast<const uint16_t *>(src)[off];
            if (v > max_val) {
                sycl::atomic_ref<uint32_t, sycl::memory_order::relaxed, sycl::memory_scope::device,
                                 sycl::access::address_space::global_space>(results->status)
                    .fetch_or(CAMBI_STATUS_INVALID_INPUT);
            }
        });
    });
}

} // namespace

/* ------------------------------------------------------------------ */
/* Kernel: spatial mask (cambi.c::get_spatial_mask).                   */
/* ------------------------------------------------------------------ */
namespace
{

/* One work-group computes a WG_Y x WG_X tile of the mask from a local copy
 * of the image: a (tile + 7) square of samples, the (tile + 6) square of
 * zero-derivative flags they give, and the horizontal 7-tap sums of those
 * flags. Flags outside the image are 0, exactly as the zero-padded
 * summed-area table in get_spatial_mask_for_index() counts them, so the
 * 7x7 integer sum is unchanged. */
constexpr unsigned MASK_HALO = 3U;
constexpr unsigned MASK_FLAGS_W = (unsigned)WG_X + 2U * MASK_HALO;
constexpr unsigned MASK_FLAGS_H = (unsigned)WG_Y + 2U * MASK_HALO;
constexpr unsigned MASK_PIX_W = MASK_FLAGS_W + 1U;
constexpr unsigned MASK_PIX_H = MASK_FLAGS_H + 1U;
constexpr uint32_t MASK_OUTSIDE = 0xFFFFFFFFU;

struct MaskArgs {
    const uint16_t *image;
    uint16_t *mask;
    unsigned width;
    unsigned height;
    unsigned mask_index;
};

struct MaskTile {
    sycl::local_accessor<uint32_t, 1> pixels;
    sycl::local_accessor<uint8_t, 1> flags;
    sycl::local_accessor<uint8_t, 1> row_sums;
};

/* Image sample at tile position (r, c) of the tile whose flag origin is
 * (y0, x0) in image coordinates (may be negative), or MASK_OUTSIDE. */
inline uint32_t mask_sample(const MaskArgs &a, int y0, int x0, unsigned r, unsigned c)
{
    const int y = y0 + (int)r;
    const int x = x0 + (int)c;
    if (y < 0 || x < 0 || std::cmp_greater_equal(y, a.height) ||
        std::cmp_greater_equal(x, a.width)) {
        return MASK_OUTSIDE;
    }
    return a.image[(unsigned)y * a.width + (unsigned)x];
}

/* Zero-derivative flag at flag position (r, c): equal to the right and the
 * lower neighbour, a missing neighbour (image edge) counting as equal. */
inline uint8_t mask_flag(const MaskTile &t, unsigned r, unsigned c)
{
    const uint32_t pixel = t.pixels[r * MASK_PIX_W + c];
    if (pixel == MASK_OUTSIDE) {
        return 0U;
    }
    const uint32_t right = t.pixels[r * MASK_PIX_W + c + 1U];
    const uint32_t below = t.pixels[(r + 1U) * MASK_PIX_W + c];
    return (uint8_t)((right == MASK_OUTSIDE || right == pixel) &&
                     (below == MASK_OUTSIDE || below == pixel));
}

inline void mask_tile(const MaskArgs &a, const MaskTile &t, sycl::nd_item<2> item)
{
    const auto lid = (unsigned)item.get_local_linear_id();
    const unsigned lanes = (unsigned)(WG_X * WG_Y);
    const int y0 = (int)(item.get_group(0) * WG_Y) - (int)MASK_HALO;
    const int x0 = (int)(item.get_group(1) * WG_X) - (int)MASK_HALO;
    for (unsigned i = lid; i < MASK_PIX_W * MASK_PIX_H; i += lanes) {
        t.pixels[i] = mask_sample(a, y0, x0, i / MASK_PIX_W, i % MASK_PIX_W);
    }
    sycl::group_barrier(item.get_group());
    for (unsigned i = lid; i < MASK_FLAGS_W * MASK_FLAGS_H; i += lanes) {
        t.flags[i] = mask_flag(t, i / MASK_FLAGS_W, i % MASK_FLAGS_W);
    }
    sycl::group_barrier(item.get_group());
    for (unsigned i = lid; i < (unsigned)WG_X * MASK_FLAGS_H; i += lanes) {
        const unsigned r = i / (unsigned)WG_X;
        const unsigned c = i % (unsigned)WG_X;
        unsigned sum = 0U;
        for (unsigned d = 0U; d <= 2U * MASK_HALO; ++d) {
            sum += t.flags[r * MASK_FLAGS_W + c + d];
        }
        t.row_sums[i] = (uint8_t)sum;
    }
    sycl::group_barrier(item.get_group());
    const auto x = (unsigned)item.get_global_id(1);
    const auto y = (unsigned)item.get_global_id(0);
    if (x < a.width && y < a.height) {
        const auto ly = (unsigned)item.get_local_id(0);
        const auto lx = (unsigned)item.get_local_id(1);
        unsigned sum = 0U;
        for (unsigned d = 0U; d <= 2U * MASK_HALO; ++d) {
            sum += t.row_sums[(ly + d) * (unsigned)WG_X + lx];
        }
        a.mask[y * a.width + x] = (uint16_t)(sum > a.mask_index ? 1U : 0U);
    }
}

void launch_spatial_mask(sycl::queue &queue, const MaskArgs &args)
{
    const MaskArgs a = args;
    queue.submit([=](sycl::handler &handler) {
        const MaskTile tile{
            .pixels =
                sycl::local_accessor<uint32_t, 1>{sycl::range<1>{(size_t)MASK_PIX_W * MASK_PIX_H},
                                                  handler},
            .flags =
                sycl::local_accessor<uint8_t, 1>{
                    sycl::range<1>{(size_t)MASK_FLAGS_W * MASK_FLAGS_H}, handler},
            .row_sums =
                sycl::local_accessor<uint8_t, 1>{sycl::range<1>{WG_X * MASK_FLAGS_H}, handler},
        };
        handler.parallel_for(image_range(a.width, a.height),
                             [=](sycl::nd_item<2> item) { mask_tile(a, tile, item); });
    });
}

} // namespace

/* ------------------------------------------------------------------ */
/* Kernels: 2x decimate and the 3-tap mode filter.                     */
/* ------------------------------------------------------------------ */
namespace
{

/* Strict stride-2 subsample — cambi.c::decimate (in place there; the
 * in-place walk only reads samples it has not overwritten). */
void launch_decimate(sycl::queue &queue, const uint16_t *src, uint16_t *dst, unsigned out_w,
                     unsigned out_h, unsigned src_stride)
{
    queue.submit([=](sycl::handler &handler) {
        handler.parallel_for(image_range(out_w, out_h), [=](sycl::nd_item<2> item) {
            const auto x = (unsigned)item.get_global_id(1);
            const auto y = (unsigned)item.get_global_id(0);
            if (x < out_w && y < out_h) {
                dst[y * out_w + x] = src[y * 2U * src_stride + x * 2U];
            }
        });
    });
}

/* mode3 is symmetric in its arguments, so the cyclic line order of
 * cambi.c::filter_mode's row buffer does not matter. */
inline uint16_t mode3(uint16_t first, uint16_t second, uint16_t third)
{
    if (first == second || first == third) {
        return first;
    }
    if (second == third) {
        return second;
    }
    return first < second ? (first < third ? first : third) : (second < third ? second : third);
}

/* Horizontal pass: edge columns keep their value (mode3(a, a, b) == a). */
void launch_filter_horizontal(sycl::queue &queue, const uint16_t *input, uint16_t *output,
                              unsigned width, unsigned height)
{
    queue.submit([=](sycl::handler &handler) {
        handler.parallel_for(image_range(width, height), [=](sycl::nd_item<2> item) {
            const auto x = (unsigned)item.get_global_id(1);
            const auto y = (unsigned)item.get_global_id(0);
            if (x >= width || y >= height) {
                return;
            }
            const uint16_t *row = input + plane_offset(y, width);
            const unsigned left = x > 0U ? x - 1U : 0U;
            const unsigned right = x + 1U < width ? x + 1U : width - 1U;
            output[y * width + x] = mode3(row[left], row[x], row[right]);
        });
    });
}

struct VerticalArgs {
    const uint16_t *filtered_h;
    uint16_t *image;
    const uint16_t *mask;
    uint16_t *q;
    unsigned width;
    unsigned height;
    uint16_t v_band_base;
    uint16_t v_band_size;
};

/* Vertical pass + level map. cambi.c::filter_mode writes rows 1 .. height-2
 * only, so the first and last rows keep their pre-filter value. Q is the
 * histogram row calculate_c_values() files the pixel under, or
 * CAMBI_Q_INVALID when the pixel is masked out or outside the band. */
inline void vertical_pixel(const VerticalArgs &a, unsigned x, unsigned y)
{
    const unsigned idx = y * a.width + x;
    uint16_t value = a.image[idx];
    if (y > 0U && y + 1U < a.height) {
        value = mode3(a.filtered_h[idx - a.width], a.filtered_h[idx], a.filtered_h[idx + a.width]);
        a.image[idx] = value;
    }
    const auto compact = (uint16_t)(value - a.v_band_base);
    a.q[idx] = (a.mask[idx] != 0U && compact < a.v_band_size) ? compact : CAMBI_Q_INVALID;
}

void launch_filter_vertical_and_levels(sycl::queue &queue, const VerticalArgs &args)
{
    const VerticalArgs a = args;
    queue.submit([=](sycl::handler &handler) {
        handler.parallel_for(image_range(a.width, a.height), [=](sycl::nd_item<2> item) {
            const auto x = (unsigned)item.get_global_id(1);
            const auto y = (unsigned)item.get_global_id(0);
            if (x < a.width && y < a.height) {
                vertical_pixel(a, x, y);
            }
        });
    });
}

} // namespace

/* ------------------------------------------------------------------ */
/* Kernels: c-values (cambi.c::calculate_c_values).                    */
/* ------------------------------------------------------------------ */
namespace
{

/* Per-row bit masks over the level map, 32 columns per word:
 *   runs[y]   bit x (x > 0) set where Q[y][x] != Q[y][x - 1] — run starts;
 *   change[y] bit x set where the row leaving the window of row y
 *             (y - pad - 1) and the row entering it (y + pad) differ at x,
 *             an absent row reading as CAMBI_Q_INVALID.
 * They let a work-item skip unchanged window rows and apply a row segment
 * run by run instead of pixel by pixel. */
struct RowMaskArgs {
    const uint16_t *q;
    uint32_t *runs;
    uint32_t *change;
    unsigned width;
    unsigned height;
    unsigned words;
    unsigned pad;
};

/* Work-group of ROWMASK_ROWS rows x 32 columns: every item tests one column
 * (coalesced level-map loads) and parks its two bits in local memory; the
 * first item of each row then ORs the row's 32 bits into the mask words. */
constexpr size_t ROWMASK_ROWS = 8;

struct RowMaskBits {
    sycl::local_accessor<uint32_t, 1> runs;
    sycl::local_accessor<uint32_t, 1> change;
};

inline void row_mask_item(const RowMaskArgs &a, const RowMaskBits &bits, sycl::nd_item<2> item)
{
    const auto y = (unsigned)item.get_global_id(0);
    const auto x = (unsigned)item.get_global_id(1);
    const auto slot = (unsigned)item.get_local_linear_id();
    const unsigned bit = 1U << (x & 31U);
    uint32_t runs = 0U;
    uint32_t change = 0U;
    if (x < a.width && y < a.height) {
        const uint16_t *row = a.q + plane_offset(y, a.width);
        runs = (x > 0U && row[x] != row[x - 1U]) ? bit : 0U;
        const uint16_t leaving = y > a.pad ? a.q[(y - a.pad - 1U) * a.width + x] : CAMBI_Q_INVALID;
        const uint16_t entering =
            y + a.pad < a.height ? a.q[(y + a.pad) * a.width + x] : CAMBI_Q_INVALID;
        change = leaving != entering ? bit : 0U;
    }
    bits.runs[slot] = runs;
    bits.change[slot] = change;
    sycl::group_barrier(item.get_group());
    if (item.get_local_id(1) != 0U || y >= a.height) {
        return;
    }
    uint32_t runs_word = 0U;
    uint32_t change_word = 0U;
    for (unsigned b = 0U; b < 32U; ++b) {
        runs_word |= bits.runs[slot + b];
        change_word |= bits.change[slot + b];
    }
    const auto word = (unsigned)item.get_group(1);
    a.runs[y * a.words + word] = runs_word;
    a.change[y * a.words + word] = change_word;
}

void launch_row_masks(sycl::queue &queue, const RowMaskArgs &args)
{
    const RowMaskArgs a = args;
    const size_t rows = ((size_t)a.height + ROWMASK_ROWS - 1U) / ROWMASK_ROWS * ROWMASK_ROWS;
    const sycl::nd_range<2> range{sycl::range<2>{rows, (size_t)a.words * 32U},
                                  sycl::range<2>{ROWMASK_ROWS, 32U}};
    queue.submit([=](sycl::handler &handler) {
        const RowMaskBits bits{
            .runs = sycl::local_accessor<uint32_t, 1>{sycl::range<1>{ROWMASK_ROWS * 32U}, handler},
            .change =
                sycl::local_accessor<uint32_t, 1>{sycl::range<1>{ROWMASK_ROWS * 32U}, handler},
        };
        handler.parallel_for(range, [=](sycl::nd_item<2> item) { row_mask_item(a, bits, item); });
    });
}

struct CValuesArgs {
    const uint16_t *q;
    const uint32_t *runs;
    const uint32_t *change;
    uint16_t *hist;
    float *cvals;
    CambiSyclSelect *select;
    uint64_t *partials;
    const float *lut;
    const uint16_t *tvi;
    const int *weights;
    unsigned width;
    unsigned height;
    unsigned words;
    unsigned pad;
    unsigned chunk_rows;
    unsigned levels;
    unsigned num_diffs;
    unsigned vlt_luma;
    unsigned v_band_base;
};

/* Word `w` of a mask row restricted to columns [lo, hi]. */
inline uint32_t mask_word_in_range(const uint32_t *mask_row, unsigned w, unsigned lo, unsigned hi)
{
    uint32_t bits = mask_row[w];
    if (w == lo >> 5U) {
        bits &= ~0U << (lo & 31U);
    }
    if (w == hi >> 5U && (hi & 31U) != 31U) {
        bits &= (1U << ((hi & 31U) + 1U)) - 1U;
    }
    return bits;
}

inline bool mask_any(const uint32_t *mask_row, unsigned lo, unsigned hi)
{
    uint32_t any = 0U;
    for (unsigned w = lo >> 5U; w <= hi >> 5U; ++w) {
        any |= mask_word_in_range(mask_row, w, lo, hi);
    }
    return any != 0U;
}

/* Apply `count` (+/-) to one histogram cell with uint16 wrap-around, the
 * arithmetic increment_range() / decrement_range() use. Updates commute, so
 * the cell ends at the true window count whatever the order. */
inline void hist_apply(uint16_t *col_hist, unsigned width, uint16_t level, int count)
{
    if (level == CAMBI_Q_INVALID) {
        return;
    }
    uint16_t &cell = col_hist[plane_offset(level, width)];
    cell = (uint16_t)((int)cell + count);
}

/* Add (sign +1) or remove (sign -1) row y's pixels in [lo, hi], one update
 * per run of equal levels. */
inline void hist_row_runs(const CValuesArgs &a, uint16_t *col_hist, unsigned y, unsigned lo,
                          unsigned hi, int sign)
{
    const uint16_t *row = a.q + plane_offset(y, a.width);
    const uint32_t *runs = a.runs + plane_offset(y, a.words);
    unsigned start = lo;
    if (lo < hi) {
        for (unsigned w = (lo + 1U) >> 5U; w <= hi >> 5U; ++w) {
            uint32_t bits = mask_word_in_range(runs, w, lo + 1U, hi);
            for (int visited = 0; visited < 32 && bits != 0U; ++visited) {
                const unsigned x = w * 32U + (unsigned)sycl::ctz(bits);
                bits &= bits - 1U;
                hist_apply(col_hist, a.width, row[start], sign * (int)(x - start));
                start = x;
            }
        }
    }
    hist_apply(col_hist, a.width, row[start], sign * (int)(hi + 1U - start));
}

/* Move the window of column `col` from row y - 1 to row y: remove row
 * y - pad - 1, add row y + pad; skipped when both agree over the window. */
inline void hist_slide(const CValuesArgs &a, uint16_t *col_hist, unsigned y, unsigned lo,
                       unsigned hi)
{
    if (!mask_any(a.change + plane_offset(y, a.words), lo, hi)) {
        return;
    }
    if (y > a.pad) {
        hist_row_runs(a, col_hist, y - a.pad - 1U, lo, hi, -1);
    }
    if (y + a.pad < a.height) {
        hist_row_runs(a, col_hist, y + a.pad, lo, hi, 1);
    }
}

/* cambi.c::c_value_pixel for the pixel whose level-map value is q0. */
inline float cvals_pixel(const CValuesArgs &a, const uint16_t *col_hist, uint16_t q0)
{
    if (q0 == CAMBI_Q_INVALID) {
        return 0.0F;
    }
    const unsigned value = (unsigned)q0 + a.v_band_base + a.num_diffs;
    const int p0 = col_hist[plane_offset(q0, a.width)];
    float c_value = 0.0F;
    for (unsigned d = 0U; d < a.num_diffs; ++d) {
        if (value > a.tvi[d] || value + d + 1U <= a.vlt_luma) {
            continue;
        }
        const unsigned up = (unsigned)q0 + d + 1U;
        const int p1 = up < a.levels ? col_hist[plane_offset(up, a.width)] : 0;
        const int p2 = q0 >= d + 1U ? col_hist[plane_offset(q0 - d - 1U, a.width)] : 0;
        const int pm = p1 > p2 ? p1 : p2;
        const float val = (float)(a.weights[d] * p0 * pm) * a.lut[pm + p0];
        if (val > c_value) {
            c_value = val;
        }
    }
    return c_value;
}

/* Zero the column, then load the window of the chunk's first row. */
inline void cvals_prime(const CValuesArgs &a, uint16_t *col_hist, unsigned y0, unsigned lo,
                        unsigned hi)
{
    for (unsigned level = 0U; level < a.levels; ++level) {
        col_hist[plane_offset(level, a.width)] = 0U;
    }
    const unsigned first = y0 > a.pad ? y0 - a.pad : 0U;
    const unsigned last = y0 + a.pad < a.height ? y0 + a.pad : a.height - 1U;
    for (unsigned y = first; y <= last; ++y) {
        hist_row_runs(a, col_hist, y, lo, hi, 1);
    }
}

/* A work-item's running share of top-K pass 0: the radix count of its
 * c-values (one atomic per run of equal bins) and their fixed-point sum. */
struct CvalsTally {
    uint64_t sum;
    uint32_t bin;
    uint32_t count;
};

inline void tally_flush(const CValuesArgs &a, CvalsTally &tally)
{
    if (tally.count != 0U) {
        GlobalCounter(a.select->hist[tally.bin]).fetch_add(tally.count);
        tally.count = 0U;
    }
}

inline void tally_add(const CValuesArgs &a, CvalsTally &tally, float value)
{
    const uint32_t bin = (sycl::bit_cast<uint32_t>(value) >> RADIX_SHIFT[0]) & RADIX_BIN_MASK[0];
    if (bin != tally.bin) {
        tally_flush(a, tally);
        tally.bin = bin;
    }
    ++tally.count;
    tally.sum += cambi_fixed(value);
}

/* One work-item: histogram column `col` of row chunk `chunk`. The window of
 * row y covers rows [y - pad, y + pad] and columns [col - pad, col + pad],
 * clipped to the image, as calculate_c_values()'s first-pass / top-edge /
 * middle-slide / bottom-edge walk leaves it. */
inline void cvals_column(const CValuesArgs &a, unsigned chunk, unsigned col, CvalsTally &tally)
{
    const unsigned y0 = chunk * a.chunk_rows;
    if (col >= a.width || y0 >= a.height) {
        return;
    }
    const unsigned y1 = y0 + a.chunk_rows < a.height ? y0 + a.chunk_rows : a.height;
    const unsigned lo = col > a.pad ? col - a.pad : 0U;
    const unsigned hi = col + a.pad < a.width ? col + a.pad : a.width - 1U;
    uint16_t *col_hist = a.hist + plane_offset(chunk * a.levels, a.width) + col;
    cvals_prime(a, col_hist, y0, lo, hi);
    for (unsigned y = y0; y < y1; ++y) {
        if (y > y0) {
            hist_slide(a, col_hist, y, lo, hi);
        }
        const unsigned idx = y * a.width + col;
        const float value = cvals_pixel(a, col_hist, a.q[idx]);
        a.cvals[idx] = value;
        tally_add(a, tally, value);
    }
}

/* c-values plus top-K pass 0: the radix histogram of every c-value and one
 * fixed-point partial sum per work-group (the whole top-K sum whenever the
 * threshold resolves to 0). */
void launch_c_values(sycl::queue &queue, const CValuesArgs &args, unsigned chunks)
{
    const CValuesArgs a = args;
    const size_t global_x = ((size_t)a.width + CVALS_WG - 1U) / CVALS_WG * CVALS_WG;
    const sycl::nd_range<2> range{sycl::range<2>{(size_t)chunks, global_x},
                                  sycl::range<2>{1U, CVALS_WG}};
    queue.submit([=](sycl::handler &handler) {
        handler.parallel_for(range, [=](sycl::nd_item<2> item) {
            CvalsTally tally{.sum = 0U, .bin = 0U, .count = 0U};
            cvals_column(a, (unsigned)item.get_global_id(0), (unsigned)item.get_global_id(1),
                         tally);
            tally_flush(a, tally);
            const uint64_t group_sum =
                sycl::reduce_over_group(item.get_group(), tally.sum, sycl::plus<>());
            if (item.get_local_linear_id() == 0U) {
                a.partials[item.get_group_linear_id()] = group_sum;
            }
        });
    });
}

} // namespace

/* ------------------------------------------------------------------ */
/* Kernels: exact top-K pooling (cambi.c::spatial_pooling).            */
/* ------------------------------------------------------------------ */
namespace
{

using LocalCounter =
    sycl::atomic_ref<uint32_t, sycl::memory_order::relaxed, sycl::memory_scope::work_group,
                     sycl::access::address_space::local_space>;

struct PoolArgs {
    const float *cvals;
    CambiSyclSelect *select;
    uint64_t *partials;
    CambiSyclResults *results;
    unsigned n;
    unsigned groups;
    unsigned cvals_groups;
    int scale;
};

/* Contiguous element block of work-group `group`. */
inline void pool_block(const PoolArgs &a, size_t group, unsigned &begin, unsigned &end)
{
    const unsigned per_group = (a.n + a.groups - 1U) / a.groups;
    begin = (unsigned)group * per_group;
    end = begin + per_group < a.n ? begin + per_group : a.n;
}

inline void radix_count_block(const PoolArgs &a, sycl::nd_item<1> item,
                              const sycl::local_accessor<uint32_t, 1> &local_hist, int pass)
{
    unsigned begin = 0U;
    unsigned end = 0U;
    pool_block(a, item.get_group(0), begin, end);
    const uint32_t prefix = a.select->prefix;
    uint32_t cur_bin = 0U;
    uint32_t cur_count = 0U;
    for (unsigned i = begin + (unsigned)item.get_local_id(0); i < end; i += (unsigned)POOL_WG) {
        const auto bits = sycl::bit_cast<uint32_t>(a.cvals[i]);
        if ((bits & RADIX_KNOWN_MASK[pass]) != prefix) {
            continue;
        }
        const uint32_t bin = (bits >> RADIX_SHIFT[pass]) & RADIX_BIN_MASK[pass];
        if (bin != cur_bin && cur_count != 0U) {
            LocalCounter(local_hist[cur_bin]).fetch_add(cur_count);
            cur_count = 0U;
        }
        cur_bin = bin;
        ++cur_count;
    }
    if (cur_count != 0U) {
        LocalCounter(local_hist[cur_bin]).fetch_add(cur_count);
    }
}

/* One work-group's share: count its block into a local histogram, then add
 * the non-empty bins to the scale's global histogram. The resolved flag is
 * uniform, so either every item of the group returns or none does. */
inline void radix_histogram_group(const PoolArgs &a, sycl::nd_item<1> item,
                                  const sycl::local_accessor<uint32_t, 1> &local_hist, int pass)
{
    if (a.select->resolved != 0U) {
        return;
    }
    for (size_t b = item.get_local_id(0); b < RADIX_BINS; b += POOL_WG) {
        local_hist[b] = 0U;
    }
    sycl::group_barrier(item.get_group());
    radix_count_block(a, item, local_hist, pass);
    sycl::group_barrier(item.get_group());
    for (size_t b = item.get_local_id(0); b < RADIX_BINS; b += POOL_WG) {
        if (local_hist[b] != 0U) {
            GlobalCounter(a.select->hist[b]).fetch_add(local_hist[b]);
        }
    }
}

/* Radix histogram of passes 1 and 2 (pass 0 is counted by the c-values
 * kernel), restricted to the bucket the previous passes chose. */
void launch_radix_histogram(sycl::queue &queue, const PoolArgs &args, int pass)
{
    const PoolArgs a = args;
    const sycl::nd_range<1> range{sycl::range<1>{(size_t)a.groups * POOL_WG},
                                  sycl::range<1>{POOL_WG}};
    queue.submit([=](sycl::handler &handler) {
        const sycl::local_accessor<uint32_t, 1> local_hist{sycl::range<1>{RADIX_BINS}, handler};
        handler.parallel_for(range, [=](sycl::nd_item<1> item) {
            radix_histogram_group(a, item, local_hist, pass);
        });
    });
}

/* One lane of the bucket scan: lane l owns bins [2047 - 8l - 7, 2047 - 8l],
 * visited high to low. The lane whose range holds the k_rem-th largest
 * element records its bin and the rank left inside it; every lane then
 * clears its bins for the next pass. */
inline void radix_scan_lane(const PoolArgs &a, sycl::nd_item<1> item, int pass)
{
    if (a.select->resolved != 0U) {
        return;
    }
    const auto lane = (unsigned)item.get_local_id(0);
    const unsigned top = RADIX_BINS - 1U - lane * RADIX_BINS_PER_LANE;
    const uint32_t k = a.select->k_rem[pass];
    uint32_t mine = 0U;
    for (unsigned j = 0U; j < RADIX_BINS_PER_LANE; ++j) {
        mine += a.select->hist[top - j];
    }
    const uint32_t before = sycl::exclusive_scan_over_group(item.get_group(), mine, sycl::plus<>());
    if (before < k && k <= before + mine) {
        uint32_t cum = before;
        for (unsigned j = 0U; j < RADIX_BINS_PER_LANE; ++j) {
            const uint32_t count = a.select->hist[top - j];
            if (cum + count >= k) {
                a.select->prefix |= (top - j) << RADIX_SHIFT[pass];
                a.select->k_rem[pass + 1] = k - cum;
                a.select->resolved = (pass == 0 && top - j == 0U) ? 1U : 0U;
                break;
            }
            cum += count;
        }
    }
    for (unsigned j = 0U; j < RADIX_BINS_PER_LANE; ++j) {
        a.select->hist[top - j] = 0U;
    }
}

void launch_radix_scan(sycl::queue &queue, const PoolArgs &args, int pass)
{
    const PoolArgs a = args;
    const sycl::nd_range<1> range{sycl::range<1>{POOL_WG}, sycl::range<1>{POOL_WG}};
    queue.submit([=](sycl::handler &handler) {
        handler.parallel_for(range, [=](sycl::nd_item<1> item) { radix_scan_lane(a, item, pass); });
    });
}

/* Per work-group sum of every element strictly above the threshold. Not
 * needed when the threshold resolved to 0: the c-values kernel's partials
 * already hold the sum of every element. */
void launch_topk_partials(sycl::queue &queue, const PoolArgs &args)
{
    assert(args.groups >= 1U && args.groups <= POOL_MAX_GROUPS);
    const PoolArgs a = args;
    const sycl::nd_range<1> range{sycl::range<1>{(size_t)a.groups * POOL_WG},
                                  sycl::range<1>{POOL_WG}};
    queue.submit([=](sycl::handler &handler) {
        handler.parallel_for(range, [=](sycl::nd_item<1> item) {
            if (a.select->resolved != 0U) {
                return;
            }
            unsigned begin = 0U;
            unsigned end = 0U;
            pool_block(a, item.get_group(0), begin, end);
            const uint32_t threshold = a.select->prefix;
            uint64_t sum = 0U;
            for (unsigned i = begin + (unsigned)item.get_local_id(0); i < end;
                 i += (unsigned)POOL_WG) {
                const float value = a.cvals[i];
                sum += sycl::bit_cast<uint32_t>(value) > threshold ? cambi_fixed(value) : 0U;
            }
            const uint64_t group_sum =
                sycl::reduce_over_group(item.get_group(), sum, sycl::plus<>());
            if (item.get_local_id(0) == 0U) {
                a.partials[item.get_group(0)] = group_sum;
            }
        });
    });
}

/* 128-bit accumulator: value = hi * 2^64 + lo. */
struct U128 {
    uint64_t lo;
    uint64_t hi;
};

/* hi32 * 2^32 + lo32 as a 128-bit value (both halves < 2^64). */
inline U128 u128_from_halves(uint64_t hi32_sum, uint64_t lo32_sum)
{
    const uint64_t shifted = hi32_sum << 32U;
    U128 r{.lo = shifted + lo32_sum, .hi = hi32_sum >> 32U};
    r.hi += r.lo < shifted ? 1U : 0U;
    return r;
}

inline U128 u128_add(U128 x, U128 y)
{
    U128 r{.lo = x.lo + y.lo, .hi = x.hi + y.hi};
    r.hi += r.lo < x.lo ? 1U : 0U;
    return r;
}

/* Sum the per-group partials (each < 2^64) and the k_rem copies of the
 * threshold value into the 128-bit per-scale result. */
inline void topk_final_lane(const PoolArgs &a, sycl::nd_item<1> item)
{
    const bool resolved = a.select->resolved != 0U;
    const unsigned count = resolved ? a.cvals_groups : a.groups;
    uint64_t lo32 = 0U;
    uint64_t hi32 = 0U;
    for (size_t g = item.get_local_id(0); g < count; g += POOL_WG) {
        lo32 += a.partials[g] & 0xFFFFFFFFU;
        hi32 += a.partials[g] >> 32U;
    }
    lo32 = sycl::reduce_over_group(item.get_group(), lo32, sycl::plus<>());
    hi32 = sycl::reduce_over_group(item.get_group(), hi32, sycl::plus<>());
    if (item.get_local_id(0) != 0U) {
        return;
    }
    const uint64_t t_fixed = resolved ? 0U : cambi_fixed(sycl::bit_cast<float>(a.select->prefix));
    const uint64_t k_rem = a.select->k_rem[RADIX_PASSES];
    const U128 ties = u128_from_halves((t_fixed >> 32U) * k_rem, (t_fixed & 0xFFFFFFFFU) * k_rem);
    const U128 total = u128_add(u128_from_halves(hi32, lo32), ties);
    a.results->sum_lo[a.scale] = total.lo;
    a.results->sum_hi[a.scale] = total.hi;
}

void launch_topk_final(sycl::queue &queue, const PoolArgs &args)
{
    const PoolArgs a = args;
    const sycl::nd_range<1> range{sycl::range<1>{POOL_WG}, sycl::range<1>{POOL_WG}};
    queue.submit([=](sycl::handler &handler) {
        handler.parallel_for(range, [=](sycl::nd_item<1> item) { topk_final_lane(a, item); });
    });
}

/* Everything after the c-values kernel (which counted pass 0): scan 0,
 * passes 1-2 (skipped on device once resolved), partial sums, final sum. */
void launch_topk_pooling(sycl::queue &queue, const PoolArgs &args)
{
    launch_radix_scan(queue, args, 0);
    for (int pass = 1; pass < RADIX_PASSES; ++pass) {
        launch_radix_histogram(queue, args, pass);
        launch_radix_scan(queue, args, pass);
    }
    launch_topk_partials(queue, args);
    launch_topk_final(queue, args);
}

/* Frame start: clear every scale's selection state and the readback block. */
void launch_reset(sycl::queue &queue, CambiSyclSelect *select, CambiSyclResults *results,
                  const CambiScaleGeom (&geom)[CAMBI_SYCL_NUM_SCALES])
{
    unsigned topk[CAMBI_SYCL_NUM_SCALES];
    for (int scale = 0; scale < CAMBI_SYCL_NUM_SCALES; ++scale) {
        topk[scale] = geom[scale].topk;
    }
    const sycl::range<2> range{(size_t)CAMBI_SYCL_NUM_SCALES, (size_t)RADIX_BINS};
    queue.submit([=](sycl::handler &handler) {
        handler.parallel_for(range, [=](sycl::id<2> id) {
            const size_t scale = id[0];
            const size_t bin = id[1];
            select[scale].hist[bin] = 0U;
            if (bin <= (size_t)RADIX_PASSES) {
                select[scale].k_rem[bin] = bin == 0U ? topk[scale] : 0U;
            }
            if (bin == 0U) {
                select[scale].prefix = 0U;
                select[scale].resolved = 0U;
                results->sum_lo[scale] = 0U;
                results->sum_hi[scale] = 0U;
            }
            if (scale == 0U && bin == 0U) {
                results->status = 0U;
            }
        });
    });
}

} // namespace

/* ------------------------------------------------------------------ */
/* Options (mirrors integer_cambi_cuda.c).                             */
/* ------------------------------------------------------------------ */
namespace
{

constexpr VmafOption double_option(const char *name, const char *help, const char *alias,
                                   int offset, double value, double minimum,
                                   double maximum) noexcept
{
    return {.name = name,
            .help = help,
            .alias = alias,
            .offset = offset,
            .type = VMAF_OPT_TYPE_DOUBLE,
            .default_val = {.d = value},
            .min = minimum,
            .max = maximum,
            .flags = VMAF_OPT_FLAG_FEATURE_PARAM};
}

constexpr VmafOption int_option(const char *name, const char *help, const char *alias, int offset,
                                int value, int minimum, int maximum) noexcept
{
    return {.name = name,
            .help = help,
            .alias = alias,
            .offset = offset,
            .type = VMAF_OPT_TYPE_INT,
            .default_val = {.i = value},
            .min = (double)minimum,
            .max = (double)maximum,
            .flags = VMAF_OPT_FLAG_FEATURE_PARAM};
}

constexpr VmafOption string_option(const char *name, const char *help, const char *alias,
                                   int offset) noexcept
{
    return {.name = name,
            .help = help,
            .alias = alias,
            .offset = offset,
            .type = VMAF_OPT_TYPE_STRING,
            .default_val = {.s = CAMBI_SYCL_DEFAULT_EOTF},
            .flags = VMAF_OPT_FLAG_FEATURE_PARAM};
}

} // namespace

static constexpr VmafOption options_cambi_sycl[] = {
    double_option("cambi_max_val", "maximum value allowed; larger values will be clipped", "cmxv",
                  offsetof(CambiStateSycl, cambi_max_val), CAMBI_SYCL_DEFAULT_MAX_VAL, 0.0, 1000.0),
    int_option("enc_width", "Encoding width", "encw", offsetof(CambiStateSycl, enc_width), 0, 180,
               7680),
    int_option("enc_height", "Encoding height", "ench", offsetof(CambiStateSycl, enc_height), 0,
               150, 7680),
    int_option("enc_bitdepth", "Encoding bitdepth", "encbd", offsetof(CambiStateSycl, enc_bitdepth),
               0, 6, 16),
    int_option("window_size", "Window size to compute CAMBI: 65 corresponds to ~1 degree at 4k",
               "ws", offsetof(CambiStateSycl, window_size), CAMBI_SYCL_DEFAULT_WINDOW_SIZE, 15,
               127),
    double_option("topk", "Ratio of pixels for the spatial pooling computation", nullptr,
                  offsetof(CambiStateSycl, topk), CAMBI_SYCL_DEFAULT_TOPK, 0.0001, 1.0),
    double_option("cambi_topk", "Ratio of pixels for the spatial pooling computation", "ctpk",
                  offsetof(CambiStateSycl, cambi_topk), CAMBI_SYCL_DEFAULT_TOPK, 0.0001, 1.0),
    double_option("tvi_threshold", "Visibility threshold: delta-L < tvi_threshold * L_mean", "tvit",
                  offsetof(CambiStateSycl, tvi_threshold), CAMBI_SYCL_DEFAULT_TVI, 0.0001, 1.0),
    double_option("cambi_vis_lum_threshold",
                  "Luminance value below which banding is assumed invisible", "vlt",
                  offsetof(CambiStateSycl, cambi_vis_lum_threshold), CAMBI_SYCL_DEFAULT_VLT, 0.0,
                  300.0),
    int_option("max_log_contrast", "Maximum log contrast (0 to 5, default 2)", "mlc",
               offsetof(CambiStateSycl, max_log_contrast), CAMBI_SYCL_DEFAULT_MAX_LOG_CONTRAST, 0,
               5),
    string_option("eotf", "EOTF for visibility-threshold conversion (bt1886 / pq)", nullptr,
                  offsetof(CambiStateSycl, eotf)),
    string_option("cambi_eotf", "EOTF override for cambi (defaults to eotf)", "ceot",
                  offsetof(CambiStateSycl, cambi_eotf)),
    int_option("cambi_high_res_speedup",
               "Speed up the processing by downsampling post spatial mask for resolutions >= 1080p",
               "hrs", offsetof(CambiStateSycl, cambi_high_res_speedup), 0, 0, CAMBI_4K_HEIGHT),
    {.name = nullptr},
};

/* ------------------------------------------------------------------ */
/* Init: configuration, geometry, allocation, constant tables.         */
/* ------------------------------------------------------------------ */
namespace
{

bool speedup_is_valid(int requested, int pixels)
{
    if (requested == 1080) {
        return pixels >= CAMBI_HIGH_RES_SPEEDUP_THRESHOLD_1080p;
    }
    if (requested == 1440) {
        return pixels >= CAMBI_HIGH_RES_SPEEDUP_THRESHOLD_1440p;
    }
    if (requested == 2160) {
        return pixels >= CAMBI_HIGH_RES_SPEEDUP_THRESHOLD_2160p;
    }
    return false;
}

int configure_cambi(CambiStateSycl *s, unsigned bpc, unsigned width, unsigned height)
{
    assert(s != nullptr && width > 0U && height > 0U);
    if (s->enc_bitdepth == 0) {
        s->enc_bitdepth = (int)bpc;
    }
    if (s->enc_width == 0 || s->enc_height == 0 || std::cmp_greater(s->enc_height, height) ||
        std::cmp_greater(s->enc_width, width)) {
        s->enc_width = (int)width;
        s->enc_height = (int)height;
    }
    if (!cambi_validate_dimensions((unsigned)s->enc_width, (unsigned)s->enc_height)) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "cambi_sycl: encoded resolution %dx%d below minimum %d\n",
                 s->enc_width, s->enc_height, CAMBI_MIN_WIDTH_HEIGHT);
        return -EINVAL;
    }
    if (!speedup_is_valid(s->cambi_high_res_speedup, s->enc_width * s->enc_height)) {
        s->cambi_high_res_speedup = 0;
    }
    s->src_width = width;
    s->src_height = height;
    s->src_bpc = bpc;
    s->proc_width = (unsigned)s->enc_width;
    s->proc_height = (unsigned)s->enc_height;
    /* cambi.c's own window and mask-index rounding (ADR-1378, ADR-1379). */
    s->adjusted_window = vmaf_cambi_adjust_window(s->window_size, s->proc_width, s->proc_height,
                                                  (bool)s->cambi_high_res_speedup);
    s->mask_index = vmaf_cambi_mask_index(s->proc_width, s->proc_height);
    s->num_diffs = 1U << (unsigned)s->max_log_contrast;
    return 0;
}

/* Row chunks for the c-values pass. One work-item walks one column of one
 * chunk, row after row, so a chunk is a latency chain: enough of them keep
 * the device busy (compute_units * 512 work-items measured best on Arc B580
 * and UHD 770), and at least CVALS_MIN_CHUNK_ROWS rows each keep the
 * per-chunk window priming (2 * pad + 1 row segments) a minor cost. */
unsigned cvals_chunks(unsigned width, unsigned height, unsigned levels, unsigned compute_units)
{
    const unsigned target_items = compute_units * 512U;
    const unsigned chunks = (target_items + width - 1U) / width;
    const size_t chunk_bytes = (size_t)width * levels * sizeof(uint16_t);
    const auto by_memory = (unsigned)(std::max)((size_t)1U, CVALS_HIST_BUDGET / chunk_bytes);
    const unsigned max_chunks =
        (std::min)((std::max)(1U, height / CVALS_MIN_CHUNK_ROWS), by_memory);
    return std::clamp(chunks, 1U, max_chunks);
}

/* Per-scale dimensions (cambi_score's scaled_width / scaled_height walk),
 * c-values chunking and top-K counts (spatial_pooling's clip()). */
void compute_scale_geometry(CambiStateSycl *s, double topk, unsigned compute_units)
{
    /* The level count comes from the TVI tables, which init loads first. */
    assert(s->levels >= 1U && compute_units >= 1U);
    unsigned width = s->proc_width;
    unsigned height = s->proc_height;
    for (int scale = 0; scale < CAMBI_SYCL_NUM_SCALES; ++scale) {
        if (scale > 0 || s->cambi_high_res_speedup) {
            width = (width + 1U) >> 1;
            height = (height + 1U) >> 1;
        }
        CambiScaleGeom &g = s->geom[scale];
        g.width = width;
        g.height = height;
        g.chunks = cvals_chunks(width, height, s->levels, compute_units);
        g.chunk_rows = (height + g.chunks - 1U) / g.chunks;
        g.chunks = (height + g.chunk_rows - 1U) / g.chunk_rows;
        g.cvals_groups = g.chunks * (unsigned)((width + CVALS_WG - 1U) / CVALS_WG);
        const unsigned n = height * width;
        const auto raw = static_cast<int>(topk * (int)n);
        g.topk = (unsigned)std::clamp(raw, 1, (int)n);
        g.pool_groups =
            std::clamp((n + POOL_ELEMS_PER_GROUP - 1U) / POOL_ELEMS_PER_GROUP, 1U, POOL_MAX_GROUPS);
    }
}

size_t hist_elements(const CambiStateSycl *s)
{
    size_t most = 0U;
    for (const CambiScaleGeom &g : s->geom) {
        most = (std::max)(most, (size_t)g.chunks * g.width);
    }
    return most * s->levels;
}

/* Partial-sum slots: one per c-values work-group or per pooling group. */
size_t partial_elements(const CambiStateSycl *s)
{
    size_t most = POOL_MAX_GROUPS;
    for (const CambiScaleGeom &g : s->geom) {
        most = (std::max)(most, (size_t)g.cvals_groups);
    }
    return most;
}

template <typename T> T *device_alloc(const CambiStateSycl *s, size_t count)
{
    return static_cast<T *>(vmaf_sycl_malloc_device(s->sycl_state, count * sizeof(T)));
}

int allocate_cambi_buffers(CambiStateSycl *s)
{
    assert(s->proc_width > 0U && s->proc_height > 0U && s->levels >= 1U);
    const size_t pixels = (size_t)s->proc_width * s->proc_height;
    s->d_image = device_alloc<uint16_t>(s, pixels);
    s->d_mask = device_alloc<uint16_t>(s, pixels);
    s->d_tmp = device_alloc<uint16_t>(s, pixels);
    s->d_q = device_alloc<uint16_t>(s, pixels);
    const size_t mask_words = (size_t)s->proc_height * ((s->proc_width + 31U) / 32U);
    s->d_runs = device_alloc<uint32_t>(s, mask_words);
    s->d_change = device_alloc<uint32_t>(s, mask_words);
    s->d_cvals = device_alloc<float>(s, pixels);
    s->d_hist = device_alloc<uint16_t>(s, hist_elements(s));
    s->d_select = device_alloc<CambiSyclSelect>(s, CAMBI_SYCL_NUM_SCALES);
    s->d_partials = device_alloc<uint64_t>(s, partial_elements(s));
    s->d_results = device_alloc<CambiSyclResults>(s, 1U);
    s->h_results = static_cast<CambiSyclResults *>(
        vmaf_sycl_malloc_host(s->sycl_state, sizeof(CambiSyclResults)));
    const bool ok = s->d_image && s->d_mask && s->d_tmp && s->d_q && s->d_runs && s->d_change &&
                    s->d_cvals && s->d_hist && s->d_select && s->d_partials && s->d_results &&
                    s->h_results;
    return ok ? 0 : -ENOMEM;
}

/* Upload `count` elements of `host` into a fresh device buffer. */
template <typename T> int upload_table(CambiStateSycl *s, T *&device, const T *host, size_t count)
{
    device = device_alloc<T>(s, count);
    if (!device) {
        return -ENOMEM;
    }
    return vmaf_sycl_memcpy_h2d(s->sycl_state, device, host, count * sizeof(T));
}

/* c_value_pixel()'s reciprocal table, verbatim. check_window_fits_lut()
 * guarantees every index p0 + pm (at most window^2) lies inside it. */
int upload_reciprocal_lut(CambiStateSycl *s)
{
    unsigned table_size = 0U;
    const float *table = vmaf_cambi_reciprocal_lut(&table_size);
    return upload_table(s, s->d_lut, table, (size_t)table_size);
}

} // namespace

namespace
{

/* tvi_for_diff / vlt_luma / v_band (vmaf_cambi_init_tvi_and_vlt) and the
 * contrast weights (vmaf_cambi_contrast_weights), uploaded once. */
int upload_contrast_tables(CambiStateSycl *s)
{
    /* max_log_contrast is capped at 5 by the option table. */
    assert(s->num_diffs >= 1U && s->num_diffs <= 32U);
    std::vector<uint16_t> diffs(s->num_diffs);
    std::vector<uint16_t> tvi(s->num_diffs);
    for (unsigned d = 0U; d < s->num_diffs; ++d) {
        diffs[d] = (uint16_t)(d + 1U);
    }
    int err = vmaf_cambi_init_tvi_and_vlt(
        (int)s->num_diffs, diffs.data(), s->tvi_threshold, s->cambi_vis_lum_threshold,
        s->cambi_eotf, s->eotf, tvi.data(), &s->vlt_luma, &s->v_band_base, &s->v_band_size);
    if (err) {
        return err;
    }
    s->levels = s->v_band_size;
    err = upload_table(s, s->d_tvi, tvi.data(), tvi.size());
    if (!err) {
        err = upload_table(s, s->d_weights, vmaf_cambi_contrast_weights(nullptr), s->num_diffs);
    }
    return err;
}

/* decimate_generic_*_and_convert_to_10b's resize walk, evaluated once on the
 * host by cambi.c itself: output index -> source index (ADR-1378, ADR-1379). */
std::vector<uint32_t> resize_indices(unsigned in_len, unsigned out_len)
{
    std::vector<uint32_t> idx(out_len);
    vmaf_cambi_resize_source_indices(in_len, out_len, idx.data());
    return idx;
}

int upload_resize_tables(CambiStateSycl *s)
{
    if (s->proc_width == s->src_width && s->proc_height == s->src_height) {
        return 0;
    }
    const std::vector<uint32_t> ori_x = resize_indices(s->src_width, s->proc_width);
    const std::vector<uint32_t> ori_y = resize_indices(s->src_height, s->proc_height);
    int err = upload_table(s, s->d_ori_x, ori_x.data(), ori_x.size());
    if (!err) {
        err = upload_table(s, s->d_ori_y, ori_y.data(), ori_y.size());
    }
    return err;
}

/* Everything after the contrast tables: they fix the level count that the
 * scale geometry (c-values chunking) depends on. */
int setup_device_state(CambiStateSycl *s)
{
    int err = upload_reciprocal_lut(s);
    if (!err) {
        err = upload_resize_tables(s);
    }
    if (!err) {
        err = allocate_cambi_buffers(s);
    }
    return err;
}

template <typename T> void release_sycl_buffer(VmafSyclState *state, T *&pointer)
{
    if (pointer) {
        vmaf_sycl_free(state, pointer);
        pointer = nullptr;
    }
}

void release_cambi_resources(CambiStateSycl *s)
{
    assert(s != nullptr);
    if (s->sycl_state) {
        if (s->registered) {
            (void)vmaf_sycl_graph_unregister(s->sycl_state, s);
            s->registered = false;
        }
        release_sycl_buffer(s->sycl_state, s->d_image);
        release_sycl_buffer(s->sycl_state, s->d_mask);
        release_sycl_buffer(s->sycl_state, s->d_tmp);
        release_sycl_buffer(s->sycl_state, s->d_q);
        release_sycl_buffer(s->sycl_state, s->d_runs);
        release_sycl_buffer(s->sycl_state, s->d_change);
        release_sycl_buffer(s->sycl_state, s->d_cvals);
        release_sycl_buffer(s->sycl_state, s->d_hist);
        release_sycl_buffer(s->sycl_state, s->d_lut);
        release_sycl_buffer(s->sycl_state, s->d_tvi);
        release_sycl_buffer(s->sycl_state, s->d_weights);
        release_sycl_buffer(s->sycl_state, s->d_ori_x);
        release_sycl_buffer(s->sycl_state, s->d_ori_y);
        release_sycl_buffer(s->sycl_state, s->d_select);
        release_sycl_buffer(s->sycl_state, s->d_partials);
        release_sycl_buffer(s->sycl_state, s->d_results);
        release_sycl_buffer(s->sycl_state, s->h_results);
    }
    if (s->feature_name_dict) {
        (void)vmaf_dictionary_free(&s->feature_name_dict);
    }
}

} // namespace

/* ------------------------------------------------------------------ */
/* Per-frame enqueue (graph-safe: every argument is init-time state).  */
/* ------------------------------------------------------------------ */
namespace
{

struct ScaleBuffers {
    uint16_t *image;
    uint16_t *mask;
    uint16_t *scratch;
    unsigned width;
    unsigned height;
};

void enqueue_preprocess(sycl::queue &queue, const CambiStateSycl *s, const void *dist)
{
    assert(dist != nullptr && s->d_image != nullptr);
    if (s->src_bpc != 8U && s->src_bpc != 16U) {
        launch_validate(queue, dist, s->src_width, s->src_height, s->src_bpc, s->d_results);
    }
    const PreprocArgs args{
        .src = dist,
        .dst = s->d_image,
        .ori_x = s->d_ori_x,
        .ori_y = s->d_ori_y,
        .in_w = s->src_width,
        .out_w = s->proc_width,
        .out_h = s->proc_height,
        .bpc = s->src_bpc,
        .same_size = s->proc_width == s->src_width && s->proc_height == s->src_height,
        .anti_dither = s->enc_bitdepth < 10,
    };
    launch_preprocess(queue, args);
    const MaskArgs mask{
        .image = s->d_image,
        .mask = s->d_mask,
        .width = s->proc_width,
        .height = s->proc_height,
        .mask_index = s->mask_index,
    };
    launch_spatial_mask(queue, mask);
}

/* cambi_score's per-scale decimate + filter_mode on the device buffers. */
void enqueue_scale_image(sycl::queue &queue, const CambiStateSycl *s, ScaleBuffers &b, int scale)
{
    assert(scale >= 0 && scale < CAMBI_SYCL_NUM_SCALES);
    const CambiScaleGeom &g = s->geom[scale];
    if (scale > 0 || s->cambi_high_res_speedup) {
        launch_decimate(queue, b.image, b.scratch, g.width, g.height, b.width);
        std::swap(b.image, b.scratch);
        launch_decimate(queue, b.mask, b.scratch, g.width, g.height, b.width);
        std::swap(b.mask, b.scratch);
        b.width = g.width;
        b.height = g.height;
    }
    launch_filter_horizontal(queue, b.image, b.scratch, b.width, b.height);
    const VerticalArgs vertical{
        .filtered_h = b.scratch,
        .image = b.image,
        .mask = b.mask,
        .q = s->d_q,
        .width = b.width,
        .height = b.height,
        .v_band_base = s->v_band_base,
        .v_band_size = s->v_band_size,
    };
    launch_filter_vertical_and_levels(queue, vertical);
}

void enqueue_scale_score(sycl::queue &queue, const CambiStateSycl *s, int scale)
{
    assert(scale >= 0 && scale < CAMBI_SYCL_NUM_SCALES);
    const CambiScaleGeom &g = s->geom[scale];
    /* The row chunks must cover every row of the scale. */
    assert(g.chunks >= 1U && g.chunks * g.chunk_rows >= g.height);
    const unsigned words = (g.width + 31U) / 32U;
    const unsigned pad = (unsigned)s->adjusted_window >> 1;
    const RowMaskArgs masks{
        .q = s->d_q,
        .runs = s->d_runs,
        .change = s->d_change,
        .width = g.width,
        .height = g.height,
        .words = words,
        .pad = pad,
    };
    launch_row_masks(queue, masks);
    const CValuesArgs cvals{
        .q = s->d_q,
        .runs = s->d_runs,
        .change = s->d_change,
        .hist = s->d_hist,
        .cvals = s->d_cvals,
        .select = s->d_select + scale,
        .partials = s->d_partials,
        .lut = s->d_lut,
        .tvi = s->d_tvi,
        .weights = s->d_weights,
        .width = g.width,
        .height = g.height,
        .words = words,
        .pad = pad,
        .chunk_rows = g.chunk_rows,
        .levels = s->levels,
        .num_diffs = s->num_diffs,
        .vlt_luma = s->vlt_luma,
        .v_band_base = s->v_band_base,
    };
    launch_c_values(queue, cvals, g.chunks);
    const PoolArgs pool{
        .cvals = s->d_cvals,
        .select = s->d_select + scale,
        .partials = s->d_partials,
        .results = s->d_results,
        .n = g.width * g.height,
        .groups = g.pool_groups,
        .cvals_groups = g.cvals_groups,
        .scale = scale,
    };
    launch_topk_pooling(queue, pool);
}

/* VmafSyclGraphEnqueueFn: the whole frame, graph-recordable. */
void enqueue_cambi_work(void *queue_ptr, void *priv, void *shared_ref, void *shared_dis)
{
    (void)shared_ref;
    sycl::queue &queue = *static_cast<sycl::queue *>(queue_ptr);
    const auto *s = static_cast<const CambiStateSycl *>(priv);
    launch_reset(queue, s->d_select, s->d_results, s->geom);
    enqueue_preprocess(queue, s, shared_dis);
    ScaleBuffers buffers{
        .image = s->d_image,
        .mask = s->d_mask,
        .scratch = s->d_tmp,
        .width = s->proc_width,
        .height = s->proc_height,
    };
    for (int scale = 0; scale < CAMBI_SYCL_NUM_SCALES; ++scale) {
        enqueue_scale_image(queue, s, buffers, scale);
        enqueue_scale_score(queue, s, scale);
    }
}

/* VmafSyclGraphPostFn: the single D2H copy of the frame's results. */
void cambi_post_graph(void *queue_ptr, void *priv)
{
    sycl::queue &queue = *static_cast<sycl::queue *>(queue_ptr);
    const auto *s = static_cast<const CambiStateSycl *>(priv);
    queue.memcpy(s->h_results, s->d_results, sizeof(CambiSyclResults));
}

} // namespace

/* ------------------------------------------------------------------ */
/* Extractor callbacks.                                                */
/* ------------------------------------------------------------------ */
namespace
{

unsigned device_compute_units(VmafSyclState *state)
{
    const auto *queue = static_cast<sycl::queue *>(vmaf_sycl_get_queue_ptr(state));
    if (!queue) {
        return 1U;
    }
    return (std::max)(1U, queue->get_device().get_info<sycl::info::device::max_compute_units>());
}

/* cambi.c::setup_contrast_and_luminance()'s guard, in the same place in the
 * init sequence (after the TVI tables), through the same routine: both the
 * encode-resolution window and the source-resolution window (the full input
 * here, as cambi.c's default src_width / src_height), adjusted with the
 * high-res speed-up, must satisfy window^2 < the reciprocal table size, so
 * the largest accepted window is 65 x 65 (ADR-1357, ADR-1378, ADR-1379). */
int check_window_fits_lut(const CambiStateSycl *s)
{
    const uint16_t src_window = vmaf_cambi_adjust_window(
        s->window_size, s->src_width, s->src_height, (bool)s->cambi_high_res_speedup);
    return vmaf_cambi_check_window_fits_lut(s->adjusted_window, src_window);
}

int init_cambi_device(CambiStateSycl *s, unsigned bpc, unsigned width, unsigned height)
{
    assert(s->sycl_state != nullptr);
    int error = configure_cambi(s, bpc, width, height);
    if (!error) {
        error = upload_contrast_tables(s);
    }
    if (!error) {
        error = check_window_fits_lut(s);
    }
    if (!error) {
        const double topk = s->topk != CAMBI_SYCL_DEFAULT_TOPK ? s->topk : s->cambi_topk;
        compute_scale_geometry(s, topk, device_compute_units(s->sycl_state));
        error = vmaf_sycl_shared_frame_init(s->sycl_state, width, height, bpc);
    }
    if (!error) {
        error = setup_device_state(s);
    }
    if (!error) {
        error = vmaf_sycl_graph_register(s->sycl_state, enqueue_cambi_work, nullptr,
                                         cambi_post_graph, nullptr, s, "cambi_sycl");
        s->registered = error == 0;
    }
    return error;
}

int init_fex_sycl(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                  unsigned width, unsigned height)
{
    (void)pix_fmt;
    auto *s = static_cast<CambiStateSycl *>(fex->priv);
    if (!fex->sycl_state) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "cambi_sycl: no SYCL state\n");
        return -EINVAL;
    }
    s->sycl_state = fex->sycl_state;
    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    int error = s->feature_name_dict ? 0 : -ENOMEM;
    try {
        if (!error) {
            error = init_cambi_device(s, bpc, width, height);
        }
    } catch (const sycl::exception &e) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "cambi_sycl: init failed: %s\n", e.what());
        error = -EIO;
    }
    if (error) {
        release_cambi_resources(s);
    }
    return error;
}

int submit_fex_sycl(VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                    VmafPicture *dist_pic, VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic;
    (void)ref_pic_90;
    (void)dist_pic;
    (void)dist_pic_90;
    (void)index;
    return vmaf_sycl_graph_submit(fex->sycl_state);
}

/* Per-scale mean of the top-K c-values: the exact fixed-point sum, converted
 * to double once, then spatial_pooling()'s division (cambi.c, ADR-1378, ADR-1379). */
double scale_score(const CambiSyclResults &r, int scale, unsigned topk)
{
    return vmaf_cambi_fixed_topk_mean(r.sum_hi[scale], r.sum_lo[scale], topk);
}

int collect_fex_sycl(VmafFeatureExtractor *fex, unsigned index,
                     VmafFeatureCollector *feature_collector)
{
    auto *s = static_cast<CambiStateSycl *>(fex->priv);
    const int err = vmaf_sycl_graph_wait(s->sycl_state);
    if (err) {
        return err;
    }
    const CambiSyclResults &r = *s->h_results;
    if (r.status & CAMBI_STATUS_INVALID_INPUT) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "cambi_sycl: frame %u holds samples above the %u-bit maximum\n", index,
                 s->src_bpc);
        return -EINVAL;
    }
    double scores[CAMBI_SYCL_NUM_SCALES]{};
    for (int scale = 0; scale < CAMBI_SYCL_NUM_SCALES; ++scale) {
        scores[scale] = scale_score(r, scale, s->geom[scale].topk);
    }
    const uint16_t pixels = vmaf_cambi_get_pixels_in_window(s->adjusted_window);
    const double raw_score = vmaf_cambi_weight_scores_per_scale(scores, pixels);
    double score = raw_score > s->cambi_max_val ? s->cambi_max_val : raw_score;
    if (score < 0.0) {
        score = 0.0;
    }
    return vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "Cambi_feature_cambi_score", score, index);
}

int close_fex_sycl(VmafFeatureExtractor *fex)
{
    auto *s = static_cast<CambiStateSycl *>(fex->priv);
    release_cambi_resources(s);
    return 0;
}

const char *provided_features_cambi_sycl[] = {"Cambi_feature_cambi_score", nullptr};

} // namespace

extern "C" VmafFeatureExtractor vmaf_fex_cambi_sycl = {
    .name = "cambi_sycl",
    .init = init_fex_sycl,
    .extract = nullptr,
    .flush = nullptr,
    .close = close_fex_sycl,
    .submit = submit_fex_sycl,
    .collect = collect_fex_sycl,
    .options = options_cambi_sycl,
    .priv_size = sizeof(CambiStateSycl),
    .flags = VMAF_FEATURE_EXTRACTOR_SYCL,
    .provided_features = provided_features_cambi_sycl,
    /* Device-resident (ADR-1357): ~65 kernels per frame on the combined
     * in-order queue — reset, preprocess, mask, and per scale decimate,
     * filter, c-values and 8 top-K pooling kernels — and one readback. No
     * host stage remains, so the extractor rides the combined graph like
     * the other registered SYCL extractors (dispatch_hint AUTO). */
    .chars =
        {
            .n_dispatches_per_frame = 65,
            .is_reduction_only = false,
            .min_useful_frame_area = 1920U * 1080U,
            .dispatch_hint = VMAF_FEATURE_DISPATCH_AUTO,
        },
};
