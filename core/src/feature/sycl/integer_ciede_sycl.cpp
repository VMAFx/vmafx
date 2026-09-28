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
 *  pinned staging, uploads them, launches one kernel, and reads
 *  back one float partial per work-group. The kernel reads chroma
 *  at (x >> ss_hor, y >> ss_ver), the nearest-neighbour upsample
 *  of ciede.c::scale_chroma_planes, as the CUDA and HIP twins
 *  do. Host applies the CPU's `45 - 20*log10(mean_dE)` transform
 *  for the final score.
 *
 *  Float per-pixel math throughout (ADR-0220: no fp64 in the
 *  kernel); the CPU reference computes in double, so parity is
 *  places=4, not bit-exact.
 */

#include <sycl/sycl.hpp>

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <numbers>

#include "config.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature_name.h"
#include "log.h"
#include "picture.h"
#include "sycl/common.h"

namespace
{

/* Y, U, V. */
static constexpr unsigned CIEDE_SYCL_PLANES = 3U;

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

    /* Per-workgroup float partials. Tree-reducing inside each WG
     * keeps per-block sums in float7 range (~5000 max); the host
     * accumulates partials in `double`, sidestepping the fp64
     * limitations on consumer Intel iGPU/dGPU (Arc A380 lacks
     * native fp64). Same pattern as ciede_vulkan + ciede_cuda. */
    float *d_partials;
    float *h_partials;
    unsigned wg_count_x;
    unsigned wg_count_y;
    unsigned wg_count;

    /* Submit/collect plumbing. */
    bool has_pending;
    unsigned pending_index;

    VmafDictionary *feature_name_dict;
};

static inline float srgb_to_linear(float c)
{
    if (c > 10.0f / 255.0f) {
        const float A = 0.055f;
        const float D = 1.0f / 1.055f;
        return sycl::pow((c + A) * D, 2.4f);
    }
    return c / 12.92f;
}

static inline float xyz_to_lab_map(float t)
{
    if (t > 0.008856f)
        return sycl::cbrt(t);
    return 7.787f * t + (16.0f / 116.0f);
}

static inline void yuv_to_lab(float y_lim, float u_lim, float v_lim, unsigned bpc, float &L,
                              float &A, float &B)
{
    float scale = 1.0f;
    if (bpc == 10) {
        scale = 4.0f;
    } else if (bpc == 12) {
        scale = 16.0f;
    } else if (bpc == 16) {
        scale = 256.0f;
    }
    float const y = (y_lim - 16.0f * scale) * (1.0f / (219.0f * scale));
    float const u = (u_lim - 128.0f * scale) * (1.0f / (224.0f * scale));
    float const v = (v_lim - 128.0f * scale) * (1.0f / (224.0f * scale));
    float r = y + 1.28033f * v;
    float g = y - 0.21482f * u - 0.38059f * v;
    float b = y + 2.12798f * u;
    r = srgb_to_linear(r);
    g = srgb_to_linear(g);
    b = srgb_to_linear(b);
    float x = r * 0.4124564390896921f + g * 0.357576077643909f + b * 0.18043748326639894f;
    float const yy = r * 0.21267285140562248f + g * 0.715152155287818f + b * 0.07217499330655958f;
    float z = r * 0.019333895582329317f + g * 0.119192025881303f + b * 0.9503040785363677f;
    x *= 1.0f / 0.95047f;
    z *= 1.0f / 1.08883f;
    float const lx = xyz_to_lab_map(x);
    float const ly = xyz_to_lab_map(yy);
    float const lz = xyz_to_lab_map(z);
    L = 116.0f * ly - 16.0f;
    A = 500.0f * (lx - ly);
    B = 200.0f * (ly - lz);
}

static inline float get_h_prime_dev(float b, float a)
{
    if (b == 0.0f && a == 0.0f)
        return 0.0f;
    float h = sycl::atan2(b, a);
    if (h < 0.0f)
        h += 6.283185307179586f;
    return h * 180.0f / std::numbers::pi_v<float>;
}

static inline float get_delta_h_prime_dev(float c1, float c2, float h1, float h2)
{
    if (c1 * c2 == 0.0f)
        return 0.0f;
    float const diff = h2 - h1;
    if (sycl::fabs(diff) <= 180.0f)
        return diff * std::numbers::pi_v<float> / 180.0f;
    if (diff > 180.0f)
        return (diff - 360.0f) * std::numbers::pi_v<float> / 180.0f;
    return (diff + 360.0f) * std::numbers::pi_v<float> / 180.0f;
}

static inline float get_upcase_h_bar_prime_dev(float h1, float h2)
{
    float const diff = sycl::fabs(h1 - h2);
    if (diff > 180.0f)
        return ((h1 + h2 + 360.0f) / 2.0f) * std::numbers::pi_v<float> / 180.0f;
    return ((h1 + h2) / 2.0f) * std::numbers::pi_v<float> / 180.0f;
}

static inline float get_upcase_t_dev(float h_bar)
{
    return 1.0f - 0.17f * sycl::cos(h_bar - std::numbers::pi_v<float> / 6.0f) +
           0.24f * sycl::cos(2.0f * h_bar) +
           0.32f * sycl::cos(3.0f * h_bar + std::numbers::pi_v<float> / 30.0f) -
           0.20f * sycl::cos(4.0f * h_bar - 63.0f * std::numbers::pi_v<float> / 180.0f);
}

static inline float get_r_sub_t_dev(float c_bar, float h_bar)
{
    float const exponent =
        -sycl::pow((h_bar * 180.0f / std::numbers::pi_v<float> - 275.0f) / 25.0f, 2.0f);
    float const c7 = sycl::pow(c_bar, 7.0f);
    float const r_c = 2.0f * sycl::sqrt(c7 / (c7 + sycl::pow(25.0f, 7.0f)));
    return -sycl::sin(60.0f * std::numbers::pi_v<float> / 180.0f * sycl::exp(exponent)) * r_c;
}

static inline float ciede2000_dev(float l1, float a1, float b1, float l2, float a2, float b2)
{
    const float k_l = 0.65f;
    const float k_c = 1.0f;
    const float k_h = 4.0f;
    float const dl_p = l2 - l1;
    float const l_bar = 0.5f * (l1 + l2);
    float const c1 = sycl::sqrt(a1 * a1 + b1 * b1);
    float const c2 = sycl::sqrt(a2 * a2 + b2 * b2);
    float const c_bar = 0.5f * (c1 + c2);
    float const c_bar_7 = sycl::pow(c_bar, 7.0f);
    float const g_factor = 1.0f - sycl::sqrt(c_bar_7 / (c_bar_7 + sycl::pow(25.0f, 7.0f)));
    float const a1_p = a1 + 0.5f * a1 * g_factor;
    float const a2_p = a2 + 0.5f * a2 * g_factor;
    float const c1_p = sycl::sqrt(a1_p * a1_p + b1 * b1);
    float const c2_p = sycl::sqrt(a2_p * a2_p + b2 * b2);
    float const c_bar_p = 0.5f * (c1_p + c2_p);
    float const dc_p = c2_p - c1_p;
    float const dl2 = (l_bar - 50.0f) * (l_bar - 50.0f);
    float const s_l = 1.0f + (0.015f * dl2) / sycl::sqrt(20.0f + dl2);
    float const s_c = 1.0f + 0.045f * c_bar_p;
    float const h1_p = get_h_prime_dev(b1, a1_p);
    float const h2_p = get_h_prime_dev(b2, a2_p);
    float const dh_p = get_delta_h_prime_dev(c1, c2, h1_p, h2_p);
    float const dH_p = 2.0f * sycl::sqrt(c1_p * c2_p) * sycl::sin(dh_p / 2.0f);
    float const H_bar_p = get_upcase_h_bar_prime_dev(h1_p, h2_p);
    float const t_term = get_upcase_t_dev(H_bar_p);
    float const s_h = 1.0f + 0.015f * c_bar_p * t_term;
    float const r_t = get_r_sub_t_dev(c_bar_p, H_bar_p);
    float const lightness = dl_p / (k_l * s_l);
    float const chroma = dc_p / (k_c * s_c);
    float const hue = dH_p / (k_h * s_h);
    return sycl::sqrt(lightness * lightness + chroma * chroma + hue * hue + r_t * chroma * hue);
}

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

/* nd_range with 16x16 work-groups; each WG sums its 256 ΔE
 * contributions in float (per-WG max ~5000, fits cleanly in
 * float7), then writes one float to partials[wg_idx]. Host then
 * accumulates the WG totals in `double`. This is the same
 * precision pattern as ciede_cuda, and necessary because
 * Intel Arc A380 lacks native fp64 (so sycl::reduction<double>
 * fails at runtime). */
static constexpr size_t CIEDE_SYCL_WG_X = 16;
static constexpr size_t CIEDE_SYCL_WG_Y = 16;

/* Everything the kernel reads, captured by value as one struct so the
 * output pointer travels with its geometry (same shape as
 * PsnrKernelArgs / FpsnrOutput). Planes are packed at native size. */
struct CiedeKernelArgs {
    const void *ref[CIEDE_SYCL_PLANES];
    const void *dis[CIEDE_SYCL_PLANES];
    float *partials;
    unsigned width;
    unsigned height;
    unsigned chroma_w;
    unsigned ss_hor;
    unsigned ss_ver;
    unsigned bpc;
    unsigned wg_count_x;
};

/* Fetch one pixel's Y/U/V as float from native-resolution planes. */
template <typename T>
static inline void load_yuv(const void *const planes[CIEDE_SYCL_PLANES], size_t off_y, size_t off_c,
                            float yuv[CIEDE_SYCL_PLANES])
{
    yuv[0] = (float)static_cast<const T *>(planes[0])[off_y];
    yuv[1] = (float)static_cast<const T *>(planes[1])[off_c];
    yuv[2] = (float)static_cast<const T *>(planes[2])[off_c];
}

/* ΔE2000 of the pixel at (x, y). Chroma is read at
 * (x >> ss_hor, y >> ss_ver): output row i of scale_chroma_planes
 * reads input row i >> 1 on 4:2:0 and column j >> 1 whenever the
 * format is not 4:4:4. */
static inline float ciede_pixel(const CiedeKernelArgs &a, size_t x, size_t y)
{
    const size_t cx = a.ss_hor ? (x >> 1) : x;
    const size_t cy = a.ss_ver ? (y >> 1) : y;
    const size_t off_y = y * (size_t)a.width + x;
    const size_t off_c = cy * (size_t)a.chroma_w + cx;
    float r[CIEDE_SYCL_PLANES];
    float d[CIEDE_SYCL_PLANES];
    if (a.bpc <= 8) {
        load_yuv<uint8_t>(a.ref, off_y, off_c, r);
        load_yuv<uint8_t>(a.dis, off_y, off_c, d);
    } else {
        load_yuv<uint16_t>(a.ref, off_y, off_c, r);
        load_yuv<uint16_t>(a.dis, off_y, off_c, d);
    }
    float l1;
    float a1;
    float b1;
    float l2;
    float a2;
    float b2;
    yuv_to_lab(r[0], r[1], r[2], a.bpc, l1, a1, b1);
    yuv_to_lab(d[0], d[1], d[2], a.bpc, l2, a2, b2);
    return ciede2000_dev(l1, a1, b1, l2, a2, b2);
}

static void launch_ciede(sycl::queue &q, const CiedeKernelArgs &args)
{
    /* Round work-item count up to WG-multiples; out-of-range
     * threads contribute 0.0 to the WG sum. */
    const size_t global_x =
        ((args.width + CIEDE_SYCL_WG_X - 1) / CIEDE_SYCL_WG_X) * CIEDE_SYCL_WG_X;
    const size_t global_y =
        ((args.height + CIEDE_SYCL_WG_Y - 1) / CIEDE_SYCL_WG_Y) * CIEDE_SYCL_WG_Y;
    sycl::nd_range<2> const ndr{sycl::range<2>{global_y, global_x},
                                sycl::range<2>{CIEDE_SYCL_WG_Y, CIEDE_SYCL_WG_X}};
    const CiedeKernelArgs a = args;

    q.submit([=](sycl::handler &h) {
        h.parallel_for(ndr, [=](sycl::nd_item<2> it) {
            const size_t x = it.get_global_id(1);
            const size_t y = it.get_global_id(0);
            float my_de = 0.0f;
            if (x < (size_t)a.width && y < (size_t)a.height)
                my_de = ciede_pixel(a, x, y);
            /* Reduce within the WG via reduce_over_group. */
            float const wg_sum =
                sycl::reduce_over_group(it.get_group(), my_de, sycl::plus<float>{});
            if (it.get_local_id(0) == 0 && it.get_local_id(1) == 0) {
                const size_t wg_idx = it.get_group(0) * (size_t)a.wg_count_x + it.get_group(1);
                a.partials[wg_idx] = wg_sum;
            }
        });
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
    s->wg_count_x = (unsigned)((w + CIEDE_SYCL_WG_X - 1) / CIEDE_SYCL_WG_X);
    s->wg_count_y = (unsigned)((h + CIEDE_SYCL_WG_Y - 1) / CIEDE_SYCL_WG_Y);
    s->wg_count = s->wg_count_x * s->wg_count_y;
}

/* Allocate staging, device planes and partials. Stops at the first
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
    const size_t partials_bytes = (size_t)s->wg_count * sizeof(float);
    s->d_partials = static_cast<float *>(vmaf_sycl_malloc_device(state, partials_bytes));
    s->h_partials = static_cast<float *>(vmaf_sycl_malloc_host(state, partials_bytes));
    return s->d_partials != nullptr && s->h_partials != nullptr;
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

    CiedeKernelArgs args = {};
    /* Stage and enqueue plane by plane so the DMA of one plane overlaps
     * the host packing the next. */
    for (unsigned p = 0; p < CIEDE_SYCL_PLANES; p++) {
        const size_t bytes = s->row_bytes[p] * s->plane_h[p];
        stage_plane(ref_pic, p, s->h_ref[p], s->row_bytes[p], s->plane_h[p]);
        q.memcpy(s->d_ref[p], s->h_ref[p], bytes);
        stage_plane(dist_pic, p, s->h_dis[p], s->row_bytes[p], s->plane_h[p]);
        q.memcpy(s->d_dis[p], s->h_dis[p], bytes);
        args.ref[p] = s->d_ref[p];
        args.dis[p] = s->d_dis[p];
    }
    args.partials = s->d_partials;
    args.width = s->width;
    args.height = s->height;
    args.chroma_w = s->plane_w[1];
    args.ss_hor = s->ss_hor;
    args.ss_ver = s->ss_ver;
    args.bpc = s->bpc;
    args.wg_count_x = s->wg_count_x;
    launch_ciede(q, args);

    q.memcpy(s->h_partials, s->d_partials, (size_t)s->wg_count * sizeof(float));

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

    /* Per-WG float partials → double accumulation on host. */
    double total = 0.0;
    for (unsigned i = 0; i < s->wg_count; i++)
        total += (double)s->h_partials[i];
    const double n_pixels = (double)s->width * (double)s->height;
    const double mean_de = total / n_pixels;
    const double score = 45.0 - 20.0 * std::log10(mean_de);

    return vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "ciede2000", score, index);
}

static int close_fex_sycl(VmafFeatureExtractor *fex)
{
    auto *s = static_cast<CiedeStateSycl *>(fex->priv);
    if (s->sycl_state) {
        void *const buffers[] = {
            s->h_ref[0], s->h_ref[1], s->h_ref[2],   s->h_dis[0],   s->h_dis[1],
            s->h_dis[2], s->d_ref[0], s->d_ref[1],   s->d_ref[2],   s->d_dis[0],
            s->d_dis[1], s->d_dis[2], s->d_partials, s->h_partials,
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
