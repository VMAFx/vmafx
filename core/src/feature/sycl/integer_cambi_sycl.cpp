/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  CAMBI banding-detection feature extractor on the SYCL backend
 *  (T3-15 / ADR-0371). SYCL twin of integer_cambi_cuda.c (ADR-0360)
 *  and cambi_vulkan.c (ADR-0210).
 *
 *  Strategy II hybrid — identical to the CUDA twin (ADR-0360):
 *
 *    GPU stages (three SYCL kernels):
 *      - launch_spatial_mask  : derivative + 7×7 box sum + threshold.
 *        Produces a uint16 mask buffer (0 = flat, 1 = edge).
 *        Bit-exact port of cambi_spatial_mask_kernel.
 *      - launch_decimate      : strict 2× stride-2 subsample.
 *        Bit-exact port of cambi_decimate_kernel.
 *      - launch_filter_mode   : separable 3-tap mode filter (H + V).
 *        Bit-exact port of cambi_filter_mode_kernel.
 *
 *    Host CPU stages (exact CPU code via cambi_internal.h wrappers):
 *      - vmaf_cambi_preprocessing: decimate/upcast to 10-bit.
 *      - vmaf_cambi_calculate_c_values: sliding-histogram c-value pass.
 *      - vmaf_cambi_spatial_pooling: top-K pooling → per-scale score.
 *      - vmaf_cambi_weight_scores_per_scale: inner-product scale weights.
 *
 *  Per-frame flow (event-chained GPU passes, SY-1 perf-audit 2026-05-16):
 *    1. Host preprocessing (CPU): resize/upcast dist_pic → pics[0].
 *    2. H2D upload of pics[0] luma plane → d_image (USM device).
 *       One q.wait() drains the H2D upload before GPU kernel launch.
 *    3. GPU launch_spatial_mask over d_image → d_mask.
 *       Returns a sycl::event — no q.wait() here.
 *    4. For scale = 0 .. NUM_SCALES-1:
 *         a. (scale > 0) GPU launch_decimate d_image → d_tmp, depends on
 *            prior event; GPU launch_decimate d_mask → d_tmp, depends on
 *            image-decimate event.  Both return events; no q.wait().
 *         b. GPU launch_filter_mode H: d_image → d_tmp, depends on prior
 *            event.  Returns event.
 *         c. GPU launch_filter_mode V: d_tmp → d_image, depends on H event.
 *            Returns event.
 *         d. One q.wait() to drain all GPU work before D2H.
 *            D2H memcpy → pics[0], pics[1].  q.wait() after D2H.
 *         e. Host vmaf_cambi_calculate_c_values + vmaf_cambi_spatial_pooling.
 *    5. Host vmaf_cambi_weight_scores_per_scale → final score.
 *    6. Store score; collect() emits "Cambi_feature_cambi_score".
 *
 *  Precision contract: `places=4` (ULP=0 on emitted score). All GPU
 *  stages use integer arithmetic only. The host residual runs the exact
 *  CPU code path from cambi_internal.h, so the emitted score is
 *  bit-for-bit identical to `vmaf_fex_cambi` and the CUDA twin.
 *
 *  SYCL specifics:
 *    - Buffers are USM device pointers (uint16_t *) allocated via
 *      vmaf_sycl_malloc_device / vmaf_sycl_malloc_host.
 *    - Kernels submitted with q.submit([=](sycl::handler &) { ... }).
 *    - q.wait() used only at H2D completion and before each D2H read.
 *      GPU-to-GPU dependencies use sycl::event depends_on chains so the
 *      runtime can overlap or pipeline adjacent dispatches without a full
 *      queue drain (SY-1 fix — perf-audit 2026-05-16).  The CUDA twin
 *      (ADR-0360) retains its v1 synchronous posture for now.
 *    - Does NOT use vmaf_sycl_graph_register because CAMBI's host
 *      residual is non-trivial and the per-scale CPU work serialises
 *      frames already. Same reasoning as the CUDA twin.
 *    - Supports both Intel oneAPI (icpx -fsycl) and AdaptiveCpp
 *      (acpp --acpp-targets=...) per ADR-0335. The strict-FP contract
 *      is honoured by the meson.build sycl_strict_fp_args mechanism
 *      (-fp-model=precise for icpx, -ffp-contract=off for acpp).
 *      Since all arithmetic in the SYCL kernels is integer-only,
 *      strict-FP has no effect here; the flag is inherited from the
 *      common feature build recipe.
 *
 *  Kernel mapping from CUDA → SYCL:
 *    cambi_spatial_mask_kernel → launch_spatial_mask (anonymous ns)
 *    cambi_decimate_kernel     → launch_decimate     (anonymous ns)
 *    cambi_filter_mode_kernel  → launch_filter_mode  (anonymous ns)
 */

#include <sycl/sycl.hpp>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <utility>

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
/* Constants (mirroring integer_cambi_cuda.c). */
/* ------------------------------------------------------------------ */
namespace
{

constexpr int CAMBI_SYCL_NUM_SCALES = 5;
static constexpr int CAMBI_SYCL_MIN_WIDTH_HEIGHT = CAMBI_MIN_WIDTH_HEIGHT;
static constexpr unsigned CAMBI_SYCL_MASK_FILTER_SIZE = 7U;
static constexpr double CAMBI_SYCL_DEFAULT_MAX_VAL = 1000.0;
static constexpr int CAMBI_SYCL_DEFAULT_WINDOW_SIZE = 65;
static constexpr double CAMBI_SYCL_DEFAULT_TOPK = 0.6;
static constexpr double CAMBI_SYCL_DEFAULT_TVI = 0.019;
static constexpr double CAMBI_SYCL_DEFAULT_VLT = 0.0;
static constexpr int CAMBI_SYCL_DEFAULT_MAX_LOG_CONTRAST = 2;
/* `default_val.s` in `VmafOption` is declared `char *` (not `const char *`);
 * use a `char[]` so the array decays to `char *` without a const cast.
 * Mirrors the CUDA twin `CAMBI_CUDA_DEFAULT_EOTF` which uses a `#define`
 * macro for the same reason. */
char CAMBI_SYCL_DEFAULT_EOTF[] = "bt1886";

/* Work-group tile size. */
static constexpr size_t WG_X = 16;
constexpr size_t WG_Y = 16;

} // namespace

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */
namespace
{

struct CambiStateSycl {
    VmafSyclState *sycl_state;

    /* USM device buffers (flat uint16 arrays). */
    uint16_t *d_image;
    uint16_t *d_mask;
    uint16_t *d_tmp;

    /* USM host staging buffers for D2H. */
    uint16_t *h_image;
    uint16_t *h_mask;

    /* Host VmafPicture pair for the CPU residual. */
    VmafPicture pics[2]; /* [0] = image, [1] = mask */

    /* Host scratch buffers for the CPU residual. */
    VmafCambiHostBuffers buffers;

    /* Callbacks (scalar; mirrors CUDA twin). */
    VmafCambiRangeUpdater inc_range_callback;
    VmafCambiRangeUpdater dec_range_callback;
    VmafCambiDerivativeCalculator derivative_callback;

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

    /* Resolved per-frame geometry. */
    unsigned src_width;
    unsigned src_height;
    unsigned src_bpc;
    unsigned proc_width;
    unsigned proc_height;

    uint16_t adjusted_window;
    uint16_t vlt_luma;

    /* Pre-computed per-scale score storage. */
    double score; /* final weighted score stored by submit, emitted by collect */

    bool has_pending;
    unsigned pending_index;

    VmafDictionary *feature_name_dict;
};

} // namespace

/* ------------------------------------------------------------------ */
/* Helpers (mirrors integer_cambi_cuda.c's static helpers). */
/* ------------------------------------------------------------------ */
namespace
{

static uint16_t cambi_sycl_adjust_window(int window_size, unsigned w, unsigned h,
                                         bool cambi_high_res_speedup)
{
    unsigned adjusted = (unsigned)(window_size) * (w + h) / (unsigned)CAMBI_WINDOW_DIVISOR;
    adjusted >>= 4;
    if (cambi_high_res_speedup) {
        adjusted = (adjusted + 1u) >> 1;
    }
    if (adjusted < 1u)
        adjusted = 1u;
    if ((adjusted & 1u) == 0u)
        adjusted++;
    return (uint16_t)adjusted;
}

} // namespace

namespace
{

static uint16_t cambi_sycl_ceil_log2(uint32_t num)
{
    if (num == 0u)
        return 0u;
    uint32_t tmp = num - 1u;
    uint16_t shift = 0;
    while (tmp > 0u) {
        tmp >>= 1;
        shift++;
    }
    return shift;
}

} // namespace

namespace
{

static uint16_t cambi_sycl_get_mask_index(unsigned w, unsigned h, unsigned filter_size)
{
    uint32_t const shifted_wh = (w >> 6) * (h >> 6);
    return (uint16_t)((filter_size * filter_size + 3u * (cambi_sycl_ceil_log2(shifted_wh) - 11u) -
                       1u) >>
                      1u);
}

} // namespace

/* ------------------------------------------------------------------ */
/* SYCL kernel 1: Spatial mask                                         */
/* Port of cambi_spatial_mask_kernel from cambi_score.cu.              */
/* ------------------------------------------------------------------ */
namespace
{

static inline bool is_zero_derivative(const uint16_t *image, int x, int y, unsigned width,
                                      unsigned height, unsigned stride)
{
    const uint16_t pixel = image[(size_t)(unsigned)y * stride + (unsigned)x];
    const int right_x = x == (int)width - 1 ? x : x + 1;
    const int below_y = y == (int)height - 1 ? y : y + 1;
    const uint16_t right = image[(size_t)(unsigned)y * stride + (unsigned)right_x];
    const uint16_t below = image[(size_t)(unsigned)below_y * stride + (unsigned)x];
    return (x == (int)width - 1 || pixel == right) && (y == (int)height - 1 || pixel == below);
}

} // namespace

namespace
{

static inline unsigned spatial_box_sum(const uint16_t *image, int x, int y, unsigned width,
                                       unsigned height, unsigned stride)
{
    unsigned sum = 0u;
    for (int delta_y = -3; delta_y <= 3; ++delta_y) {
        const int row = y + delta_y;
        if (row < 0 || std::cmp_greater_equal(row, height)) {
            continue;
        }
        for (int delta_x = -3; delta_x <= 3; ++delta_x) {
            const int column = x + delta_x;
            if (column >= 0 && std::cmp_less(column, width)) {
                sum += (unsigned)is_zero_derivative(image, column, row, width, height, stride);
            }
        }
    }
    return sum;
}

} // namespace

namespace
{

static sycl::event launch_spatial_mask(sycl::queue &queue, const uint16_t *image, uint16_t *mask,
                                       unsigned width, unsigned height, unsigned stride,
                                       unsigned mask_index)
{
    const size_t global_x = ((size_t)width + WG_X - 1u) / WG_X * WG_X;
    const size_t global_y = ((size_t)height + WG_Y - 1u) / WG_Y * WG_Y;
    sycl::nd_range<2> const range{sycl::range<2>{global_y, global_x}, sycl::range<2>{WG_Y, WG_X}};
    return queue.submit([=](sycl::handler &handler) {
        handler.parallel_for(range, [=](sycl::nd_item<2> item) {
            const int x = (int)item.get_global_id(1);
            const int y = (int)item.get_global_id(0);
            if (std::cmp_less(x, width) && std::cmp_less(y, height)) {
                const unsigned sum = spatial_box_sum(image, x, y, width, height, stride);
                mask[(size_t)(unsigned)y * stride + (unsigned)x] =
                    (uint16_t)(sum > mask_index ? 1u : 0u);
            }
        });
    });
}

} // namespace

/* ------------------------------------------------------------------ */
/* SYCL kernel 2: 2× decimate                                          */
/* Port of cambi_decimate_kernel from cambi_score.cu.                  */
/* ------------------------------------------------------------------ */
/* Returns the submit event; dep is a prerequisite event (use a default-constructed
 * sycl::event{} when there is no explicit dependency). */
namespace
{

static sycl::event launch_decimate(sycl::queue &q, const uint16_t *src, uint16_t *dst,
                                   unsigned out_w, unsigned out_h, unsigned src_stride_words,
                                   unsigned dst_stride_words, const sycl::event &dep)
{
    const size_t global_x = ((size_t)out_w + WG_X - 1u) / WG_X * WG_X;
    const size_t global_y = ((size_t)out_h + WG_Y - 1u) / WG_Y * WG_Y;
    sycl::nd_range<2> const ndr{sycl::range<2>{global_y, global_x}, sycl::range<2>{WG_Y, WG_X}};

    const unsigned e_out_w = out_w;
    const unsigned e_out_h = out_h;
    const unsigned e_src_stride = src_stride_words;
    const unsigned e_dst_stride = dst_stride_words;
    const uint16_t *e_src = src;
    uint16_t *e_dst = dst;

    return q.submit([=](sycl::handler &h) {
        h.depends_on(dep);
        h.parallel_for(ndr, [=](sycl::nd_item<2> it) {
            const unsigned x = (unsigned)it.get_global_id(1);
            const unsigned y = (unsigned)it.get_global_id(0);
            if (x >= e_out_w || y >= e_out_h)
                return;
            /* Strict stride-2 subsample — bit-exact with cambi.c::decimate. */
            e_dst[(size_t)y * e_dst_stride + x] =
                e_src[(size_t)y * 2u * e_src_stride + (size_t)x * 2u];
        });
    });
}

} // namespace

/* ------------------------------------------------------------------ */
/* SYCL kernel 3: Separable 3-tap mode filter                          */
/* Port of cambi_filter_mode_kernel from cambi_score.cu.               */
/* axis=0 → horizontal, axis=1 → vertical.                             */
/* ------------------------------------------------------------------ */
/* Returns the submit event; dep is a prerequisite event. */
namespace
{

static inline uint16_t mode3(uint16_t first, uint16_t second, uint16_t third)
{
    if (first == second || first == third) {
        return first;
    }
    if (second == third) {
        return second;
    }
    return first < second ? (first < third ? first : third) : (second < third ? second : third);
}

} // namespace

namespace
{

static inline uint16_t filter_mode_pixel(const uint16_t *input, int x, int y, unsigned width,
                                         unsigned height, unsigned stride, int axis)
{
    if (axis == 0) {
        const int left = x > 0 ? x - 1 : 0;
        const int right = x < (int)width - 1 ? x + 1 : (int)width - 1;
        return mode3(input[(size_t)(unsigned)y * stride + (unsigned)left],
                     input[(size_t)(unsigned)y * stride + (unsigned)x],
                     input[(size_t)(unsigned)y * stride + (unsigned)right]);
    }
    const int above = y > 0 ? y - 1 : 0;
    const int below = y < (int)height - 1 ? y + 1 : (int)height - 1;
    return mode3(input[(size_t)(unsigned)above * stride + (unsigned)x],
                 input[(size_t)(unsigned)y * stride + (unsigned)x],
                 input[(size_t)(unsigned)below * stride + (unsigned)x]);
}

} // namespace

namespace
{

static sycl::event launch_filter_mode(sycl::queue &queue, const uint16_t *input, uint16_t *output,
                                      unsigned width, unsigned height, unsigned stride, int axis,
                                      const sycl::event &dependency)
{
    const size_t global_x = ((size_t)width + WG_X - 1u) / WG_X * WG_X;
    const size_t global_y = ((size_t)height + WG_Y - 1u) / WG_Y * WG_Y;
    sycl::nd_range<2> const range{sycl::range<2>{global_y, global_x}, sycl::range<2>{WG_Y, WG_X}};
    return queue.submit([=](sycl::handler &handler) {
        handler.depends_on(dependency);
        handler.parallel_for(range, [=](sycl::nd_item<2> item) {
            const int x = (int)item.get_global_id(1);
            const int y = (int)item.get_global_id(0);
            if (std::cmp_greater_equal(x, width) || std::cmp_greater_equal(y, height)) {
                return;
            }
            if (axis == 1 && (y == 0 || y >= (int)height - 1)) {
                return;
            }
            output[(size_t)(unsigned)y * stride + (unsigned)x] =
                filter_mode_pixel(input, x, y, width, height, stride, axis);
        });
    });
}

} // namespace

/* ------------------------------------------------------------------ */
/* Options (mirrors integer_cambi_cuda.c). */
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

} // namespace

namespace
{

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

} // namespace

namespace
{

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

namespace
{

static bool speedup_is_valid(int requested, int pixels)
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

} // namespace

namespace
{

static int configure_cambi(CambiStateSycl *s, unsigned bpc, unsigned width, unsigned height)
{
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
    s->adjusted_window = cambi_sycl_adjust_window(s->window_size, s->proc_width, s->proc_height,
                                                  (bool)s->cambi_high_res_speedup);
    return 0;
}

} // namespace

namespace
{

template <typename T> static void release_sycl_buffer(VmafSyclState *state, T *&pointer)
{
    if (pointer) {
        vmaf_sycl_free(state, pointer);
        pointer = nullptr;
    }
}

template <typename T> static void release_host_buffer(T *&pointer)
{
    free(pointer);
    pointer = nullptr;
}

} // namespace

namespace
{

static void release_cambi_resources(CambiStateSycl *s)
{
    if (s->sycl_state) {
        release_sycl_buffer(s->sycl_state, s->d_image);
        release_sycl_buffer(s->sycl_state, s->d_mask);
        release_sycl_buffer(s->sycl_state, s->d_tmp);
        release_sycl_buffer(s->sycl_state, s->h_image);
        release_sycl_buffer(s->sycl_state, s->h_mask);
    }
    (void)vmaf_picture_unref(&s->pics[0]);
    (void)vmaf_picture_unref(&s->pics[1]);
    release_host_buffer(s->buffers.diffs_to_consider);
    release_host_buffer(s->buffers.diff_weights);
    release_host_buffer(s->buffers.all_diffs);
    release_host_buffer(s->buffers.tvi_for_diff);
    release_host_buffer(s->buffers.c_values);
    release_host_buffer(s->buffers.c_values_histograms);
    release_host_buffer(s->buffers.mask_dp);
    release_host_buffer(s->buffers.filter_mode_buffer);
    release_host_buffer(s->buffers.derivative_buffer);
    if (s->feature_name_dict) {
        (void)vmaf_dictionary_free(&s->feature_name_dict);
    }
}

} // namespace

namespace
{

static int allocate_cambi_core(CambiStateSycl *s)
{
    const size_t bytes = (size_t)s->proc_width * s->proc_height * sizeof(uint16_t);
    s->d_image = static_cast<uint16_t *>(vmaf_sycl_malloc_device(s->sycl_state, bytes));
    s->d_mask = static_cast<uint16_t *>(vmaf_sycl_malloc_device(s->sycl_state, bytes));
    s->d_tmp = static_cast<uint16_t *>(vmaf_sycl_malloc_device(s->sycl_state, bytes));
    s->h_image = static_cast<uint16_t *>(vmaf_sycl_malloc_host(s->sycl_state, bytes));
    s->h_mask = static_cast<uint16_t *>(vmaf_sycl_malloc_host(s->sycl_state, bytes));
    if (!s->d_image || !s->d_mask || !s->d_tmp || !s->h_image || !s->h_mask) {
        return -ENOMEM;
    }
    int error =
        vmaf_picture_alloc(&s->pics[0], VMAF_PIX_FMT_YUV400P, 10, s->proc_width, s->proc_height);
    if (!error) {
        error = vmaf_picture_alloc(&s->pics[1], VMAF_PIX_FMT_YUV400P, 10, s->proc_width,
                                   s->proc_height);
    }
    return error;
}

} // namespace

namespace
{

static int allocate_cambi_differences(CambiStateSycl *s, int differences)
{
    static const int weights[32] = {1, 2, 3, 4, 4, 5, 5, 6, 6, 6, 6, 7, 7, 7, 7, 8,
                                    8, 8, 8, 8, 8, 8, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9};
    s->buffers.diffs_to_consider =
        static_cast<uint16_t *>(malloc(sizeof(uint16_t) * (size_t)differences));
    s->buffers.diff_weights = static_cast<int *>(malloc(sizeof(int) * (size_t)differences));
    s->buffers.all_diffs = static_cast<int *>(malloc(sizeof(int) * (size_t)(2 * differences + 1)));
    if (!s->buffers.diffs_to_consider || !s->buffers.diff_weights || !s->buffers.all_diffs) {
        return -ENOMEM;
    }
    for (int difference = 0; difference < differences; ++difference) {
        s->buffers.diffs_to_consider[difference] = (uint16_t)(difference + 1);
        s->buffers.diff_weights[difference] = weights[difference];
    }
    for (int difference = -differences; difference <= differences; ++difference) {
        s->buffers.all_diffs[difference + differences] = difference;
    }
    return 0;
}

} // namespace

namespace
{

static int allocate_cambi_thresholds(CambiStateSycl *s, int differences)
{
    s->buffers.tvi_for_diff =
        static_cast<uint16_t *>(malloc(sizeof(uint16_t) * (size_t)differences));
    if (!s->buffers.tvi_for_diff) {
        return -ENOMEM;
    }
    return vmaf_cambi_init_tvi_and_vlt(differences, s->buffers.diffs_to_consider, s->tvi_threshold,
                                       s->cambi_vis_lum_threshold, s->cambi_eotf, s->eotf,
                                       s->buffers.tvi_for_diff, &s->vlt_luma,
                                       &s->buffers.v_band_base, &s->buffers.v_band_size);
}

} // namespace

namespace
{

static int allocate_cambi_analysis(CambiStateSycl *s, int differences)
{
    const size_t final_difference = (size_t)differences * 2u;
    const uint16_t bins = (uint16_t)(1024u + (unsigned)(s->buffers.all_diffs[final_difference] -
                                                        s->buffers.all_diffs[0]));
    const size_t histogram_bins =
        s->buffers.v_band_size > bins ? (size_t)s->buffers.v_band_size : (size_t)bins;
    const int padding = (int)(CAMBI_SYCL_MASK_FILTER_SIZE / 2u);
    const int dp_width = (int)s->proc_width + 2 * padding + 1;
    const int dp_height = 2 * padding + 2;
    s->buffers.c_values =
        static_cast<float *>(malloc(sizeof(float) * s->proc_width * s->proc_height));
    s->buffers.c_values_histograms =
        static_cast<uint16_t *>(malloc(sizeof(uint16_t) * (size_t)s->proc_width * histogram_bins));
    s->buffers.mask_dp =
        static_cast<uint32_t *>(malloc(sizeof(uint32_t) * (size_t)dp_width * (size_t)dp_height));
    s->buffers.filter_mode_buffer =
        static_cast<uint16_t *>(malloc(sizeof(uint16_t) * 3u * s->proc_width));
    s->buffers.derivative_buffer =
        static_cast<uint16_t *>(malloc(sizeof(uint16_t) * s->proc_width));
    return s->buffers.c_values && s->buffers.c_values_histograms && s->buffers.mask_dp &&
                   s->buffers.filter_mode_buffer && s->buffers.derivative_buffer ?
               0 :
               -ENOMEM;
}

} // namespace

namespace
{

static int allocate_cambi_scratch(CambiStateSycl *s)
{
    const int differences = 1 << s->max_log_contrast;
    int error = allocate_cambi_differences(s, differences);
    if (!error) {
        error = allocate_cambi_thresholds(s, differences);
    }
    if (!error) {
        error = allocate_cambi_analysis(s, differences);
    }
    return error;
}

} // namespace

namespace
{

static int init_fex_sycl(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
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
    int error = s->feature_name_dict ? configure_cambi(s, bpc, width, height) : -ENOMEM;
    if (!error) {
        error = allocate_cambi_core(s);
    }
    if (!error) {
        error = allocate_cambi_scratch(s);
    }
    if (error) {
        release_cambi_resources(s);
        return error;
    }
    vmaf_cambi_default_callbacks(&s->inc_range_callback, &s->dec_range_callback,
                                 &s->derivative_callback);
    s->has_pending = false;
    return 0;
}

} // namespace

namespace
{

struct CambiScaleState {
    uint16_t *image = nullptr;
    uint16_t *mask = nullptr;
    uint16_t *scratch = nullptr;
    unsigned width = 0;
    unsigned height = 0;
    sycl::event previous{};
};

template <typename Picture>
static int upload_cambi_image(CambiStateSycl *s, sycl::queue &queue, Picture *distorted)
{
    const int error = vmaf_cambi_preprocessing(distorted, &s->pics[0], (int)s->proc_width,
                                               (int)s->proc_height, s->enc_bitdepth);
    if (error) {
        return error;
    }
    const auto *source = static_cast<const uint8_t *>(s->pics[0].data[0]);
    const size_t row_bytes = (size_t)s->proc_width * sizeof(uint16_t);
    for (unsigned row = 0; row < s->proc_height; ++row) {
        queue.memcpy(s->d_image + (size_t)row * s->proc_width,
                     source + (size_t)row * (size_t)s->pics[0].stride[0], row_bytes);
    }
    queue.wait();
    return 0;
}

} // namespace

namespace
{

static void decimate_cambi_scale(sycl::queue &queue, CambiScaleState &state)
{
    const unsigned new_width = (state.width + 1u) >> 1;
    const unsigned new_height = (state.height + 1u) >> 1;
    const sycl::event image_event =
        launch_decimate(queue, state.image, state.scratch, new_width, new_height, state.width,
                        new_width, state.previous);
    std::swap(state.image, state.scratch);
    const sycl::event mask_event =
        launch_decimate(queue, state.mask, state.scratch, new_width, new_height, state.width,
                        new_width, state.previous);
    std::swap(state.mask, state.scratch);
    state.width = new_width;
    state.height = new_height;
    state.previous = queue.submit([&](sycl::handler &handler) {
        handler.depends_on({image_event, mask_event});
        handler.single_task([=]() {});
    });
}

} // namespace

namespace
{

static void filter_cambi_scale(sycl::queue &queue, CambiScaleState &state)
{
    const sycl::event horizontal =
        launch_filter_mode(queue, state.image, state.scratch, state.width, state.height,
                           state.width, 0, state.previous);
    state.previous = launch_filter_mode(queue, state.scratch, state.image, state.width,
                                        state.height, state.width, 1, horizontal);
}

} // namespace

namespace
{

static void copy_cambi_plane(VmafPicture *picture, const uint16_t *source, unsigned width,
                             unsigned height)
{
    auto *destination = static_cast<uint8_t *>(picture->data[0]);
    const size_t row_bytes = (size_t)width * sizeof(uint16_t);
    for (unsigned row = 0; row < height; ++row) {
        (void)memcpy(destination + (size_t)row * (size_t)picture->stride[0],
                     source + (size_t)row * width, row_bytes);
    }
}

} // namespace

namespace
{

static void download_cambi_scale(CambiStateSycl *s, sycl::queue &queue, CambiScaleState &state)
{
    state.previous.wait();
    const size_t bytes = (size_t)state.width * state.height * sizeof(uint16_t);
    queue.memcpy(s->h_image, state.image, bytes);
    queue.memcpy(s->h_mask, state.mask, bytes);
    queue.wait();
    copy_cambi_plane(&s->pics[0], s->h_image, state.width, state.height);
    copy_cambi_plane(&s->pics[1], s->h_mask, state.width, state.height);
}

} // namespace

namespace
{

static double score_cambi_scale(CambiStateSycl *s, unsigned width, unsigned height, int differences,
                                double topk)
{
    vmaf_cambi_calculate_c_values(&s->pics[0], &s->pics[1], s->buffers.c_values,
                                  s->buffers.c_values_histograms, s->adjusted_window,
                                  (uint16_t)differences, s->buffers.tvi_for_diff, s->vlt_luma,
                                  s->buffers.diff_weights, s->buffers.all_diffs, (int)width,
                                  (int)height, s->inc_range_callback, s->dec_range_callback);
    return vmaf_cambi_spatial_pooling(s->buffers.c_values, topk, width, height);
}

} // namespace

namespace
{

static double process_cambi_scale(CambiStateSycl *s, sycl::queue &queue, CambiScaleState &state,
                                  int scale, int differences, double topk)
{
    if (scale > 0 || s->cambi_high_res_speedup) {
        decimate_cambi_scale(queue, state);
    }
    filter_cambi_scale(queue, state);
    download_cambi_scale(s, queue, state);
    return score_cambi_scale(s, state.width, state.height, differences, topk);
}

} // namespace

namespace
{

static int submit_fex_sycl(VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                           VmafPicture *dist_pic, VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic;
    (void)ref_pic_90;
    (void)dist_pic_90;
    auto *s = static_cast<CambiStateSycl *>(fex->priv);
    auto *queue = static_cast<sycl::queue *>(vmaf_sycl_get_queue_ptr(s->sycl_state));
    if (!queue) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "cambi_sycl: null queue pointer\n");
        return -EINVAL;
    }
    const int upload_error = upload_cambi_image(s, *queue, dist_pic);
    if (upload_error) {
        return upload_error;
    }
    const unsigned mask_index = (unsigned)cambi_sycl_get_mask_index(s->proc_width, s->proc_height,
                                                                    CAMBI_SYCL_MASK_FILTER_SIZE);
    CambiScaleState state = {
        .image = s->d_image,
        .mask = s->d_mask,
        .scratch = s->d_tmp,
        .width = s->proc_width,
        .height = s->proc_height,
        .previous = launch_spatial_mask(*queue, s->d_image, s->d_mask, s->proc_width,
                                        s->proc_height, s->proc_width, mask_index),
    };
    const int differences = 1 << s->max_log_contrast;
    const double topk = s->topk != CAMBI_SYCL_DEFAULT_TOPK ? s->topk : s->cambi_topk;
    double scores[CAMBI_SYCL_NUM_SCALES]{};
    for (int scale = 0; scale < CAMBI_SYCL_NUM_SCALES; ++scale) {
        scores[scale] = process_cambi_scale(s, *queue, state, scale, differences, topk);
    }
    const uint16_t pixels = vmaf_cambi_get_pixels_in_window(s->adjusted_window);
    const double raw_score = vmaf_cambi_weight_scores_per_scale(scores, pixels);
    s->score = raw_score > s->cambi_max_val ? s->cambi_max_val : raw_score;
    if (s->score < 0.0) {
        s->score = 0.0;
    }
    s->pending_index = index;
    s->has_pending = true;
    return 0;
}

} // namespace

/* ------------------------------------------------------------------ */
/* collect_fex_sycl — emit the pre-computed score. */
/* ------------------------------------------------------------------ */
namespace
{

static int collect_fex_sycl(VmafFeatureExtractor *fex, unsigned index,
                            VmafFeatureCollector *feature_collector)
{
    auto *s = static_cast<CambiStateSycl *>(fex->priv);
    return vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "Cambi_feature_cambi_score", s->score, index);
}

} // namespace

/* ------------------------------------------------------------------ */
/* close_fex_sycl */
/* ------------------------------------------------------------------ */
namespace
{

static int close_fex_sycl(VmafFeatureExtractor *fex)
{
    auto *s = static_cast<CambiStateSycl *>(fex->priv);
    release_cambi_resources(s);
    return 0;
}

static const char *provided_features_cambi_sycl[] = {"Cambi_feature_cambi_score", nullptr};

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
    /* 15 GPU dispatches/frame (5 scales × 3 kernels: mask + filter_H + filter_V).
     * dispatch_hint = DIRECT (matches CUDA twin and Vulkan twin): the per-frame
     * CPU residual (calculate_c_values) serialises frames already.
     * is_reduction_only = false: the GPU phases are not pure reductions. */
    .chars =
        {
            .n_dispatches_per_frame = 15,
            .is_reduction_only = false,
            .min_useful_frame_area = 1920U * 1080U,
            .dispatch_hint = VMAF_FEATURE_DISPATCH_DIRECT,
        },
};
