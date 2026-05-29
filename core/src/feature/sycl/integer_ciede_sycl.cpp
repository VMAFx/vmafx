/**
 *  Copyright 2026 Lusoris and Claude (Anthropic)
 *  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
 *
 *  ciede2000 feature extractor on the SYCL backend (T7-23 /
 *  ADR-0182, GPU long-tail batch 1c part 3). SYCL twin of
 *  ciede_vulkan (PR #136 / ADR-0187) and ciede_cuda (this PR's
 *  batch 1c part 2).
 *
 *  Self-contained submit / collect — does *not* register with
 *  vmaf_sycl_graph_register because the shared_frame buffers are
 *  luma-only and ciede needs full Y/U/V. Each submit uploads
 *  ref/dis luma + chroma (host-side upscale to luma resolution
 *  mirrors ciede.c::scale_chroma_planes), launches one kernel,
 *  and reads back a single float sum. Host applies the CPU's
 *  `45 - 20*log10(mean_dE)` transform for the final score.
 *
 *  Float per-pixel math throughout; places=4 on real hardware
 *  (Intel Arc A380 + Mesa anv → 1.0e-5 max_abs on 48 frames).
 */

#include <sycl/sycl.hpp>

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

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

struct CiedeStateSycl {
    /* Frame geometry. */
    unsigned width;
    unsigned height;
    unsigned bpc;
    enum VmafPixelFormat pix_fmt;

    /* SYCL state back-pointer. */
    VmafSyclState *sycl_state;

    /* Host-pinned staging for the 6 input planes (full luma
     * resolution per plane). Bytes per plane = w * h * bpp. */
    void *h_ref_y;
    void *h_ref_u;
    void *h_ref_v;
    void *h_dis_y;
    void *h_dis_u;
    void *h_dis_v;
    /* Device USM for the 6 input planes. */
    void *d_ref_y;
    void *d_ref_u;
    void *d_ref_v;
    void *d_dis_y;
    void *d_dis_u;
    void *d_dis_v;

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
    if (bpc == 10)
        scale = 4.0f;
    else if (bpc == 12)
        scale = 16.0f;
    else if (bpc == 16)
        scale = 256.0f;
    float y = (y_lim - 16.0f * scale) * (1.0f / (219.0f * scale));
    float u = (u_lim - 128.0f * scale) * (1.0f / (224.0f * scale));
    float v = (v_lim - 128.0f * scale) * (1.0f / (224.0f * scale));
    float r = y + 1.28033f * v;
    float g = y - 0.21482f * u - 0.38059f * v;
    float b = y + 2.12798f * u;
    r = srgb_to_linear(r);
    g = srgb_to_linear(g);
    b = srgb_to_linear(b);
    float x = r * 0.4124564390896921f + g * 0.357576077643909f + b * 0.18043748326639894f;
    float yy = r * 0.21267285140562248f + g * 0.715152155287818f + b * 0.07217499330655958f;
    float z = r * 0.019333895582329317f + g * 0.119192025881303f + b * 0.9503040785363677f;
    x *= 1.0f / 0.95047f;
    z *= 1.0f / 1.08883f;
    float lx = xyz_to_lab_map(x);
    float ly = xyz_to_lab_map(yy);
    float lz = xyz_to_lab_map(z);
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
    return h * 180.0f / 3.141592653589793f;
}

static inline float get_delta_h_prime_dev(float c1, float c2, float h1, float h2)
{
    if (c1 * c2 == 0.0f)
        return 0.0f;
    float diff = h2 - h1;
    if (sycl::fabs(diff) <= 180.0f)
        return diff * 3.141592653589793f / 180.0f;
    if (diff > 180.0f)
        return (diff - 360.0f) * 3.141592653589793f / 180.0f;
    return (diff + 360.0f) * 3.141592653589793f / 180.0f;
}

static inline float get_upcase_h_bar_prime_dev(float h1, float h2)
{
    float diff = sycl::fabs(h1 - h2);
    if (diff > 180.0f)
        return ((h1 + h2 + 360.0f) / 2.0f) * 3.141592653589793f / 180.0f;
    return ((h1 + h2) / 2.0f) * 3.141592653589793f / 180.0f;
}

static inline float get_upcase_t_dev(float h_bar)
{
    return 1.0f - 0.17f * sycl::cos(h_bar - 3.141592653589793f / 6.0f) +
           0.24f * sycl::cos(2.0f * h_bar) +
           0.32f * sycl::cos(3.0f * h_bar + 3.141592653589793f / 30.0f) -
           0.20f * sycl::cos(4.0f * h_bar - 63.0f * 3.141592653589793f / 180.0f);
}

static inline float get_r_sub_t_dev(float c_bar, float h_bar)
{
    float exponent = -sycl::pow((h_bar * 180.0f / 3.141592653589793f - 275.0f) / 25.0f, 2.0f);
    float c7 = sycl::pow(c_bar, 7.0f);
    float r_c = 2.0f * sycl::sqrt(c7 / (c7 + sycl::pow(25.0f, 7.0f)));
    return -sycl::sin(60.0f * 3.141592653589793f / 180.0f * sycl::exp(exponent)) * r_c;
}

static inline float ciede2000_dev(float l1, float a1, float b1, float l2, float a2, float b2)
{
    const float k_l = 0.65f;
    const float k_c = 1.0f;
    const float k_h = 4.0f;
    float dl_p = l2 - l1;
    float l_bar = 0.5f * (l1 + l2);
    float c1 = sycl::sqrt(a1 * a1 + b1 * b1);
    float c2 = sycl::sqrt(a2 * a2 + b2 * b2);
    float c_bar = 0.5f * (c1 + c2);
    float c_bar_7 = sycl::pow(c_bar, 7.0f);
    float g_factor = 1.0f - sycl::sqrt(c_bar_7 / (c_bar_7 + sycl::pow(25.0f, 7.0f)));
    float a1_p = a1 + 0.5f * a1 * g_factor;
    float a2_p = a2 + 0.5f * a2 * g_factor;
    float c1_p = sycl::sqrt(a1_p * a1_p + b1 * b1);
    float c2_p = sycl::sqrt(a2_p * a2_p + b2 * b2);
    float c_bar_p = 0.5f * (c1_p + c2_p);
    float dc_p = c2_p - c1_p;
    float dl2 = (l_bar - 50.0f) * (l_bar - 50.0f);
    float s_l = 1.0f + (0.015f * dl2) / sycl::sqrt(20.0f + dl2);
    float s_c = 1.0f + 0.045f * c_bar_p;
    float h1_p = get_h_prime_dev(b1, a1_p);
    float h2_p = get_h_prime_dev(b2, a2_p);
    float dh_p = get_delta_h_prime_dev(c1, c2, h1_p, h2_p);
    float dH_p = 2.0f * sycl::sqrt(c1_p * c2_p) * sycl::sin(dh_p / 2.0f);
    float H_bar_p = get_upcase_h_bar_prime_dev(h1_p, h2_p);
    float t_term = get_upcase_t_dev(H_bar_p);
    float s_h = 1.0f + 0.015f * c_bar_p * t_term;
    float r_t = get_r_sub_t_dev(c_bar_p, H_bar_p);
    float lightness = dl_p / (k_l * s_l);
    float chroma = dc_p / (k_c * s_c);
    float hue = dH_p / (k_h * s_h);
    return sycl::sqrt(lightness * lightness + chroma * chroma + hue * hue + r_t * chroma * hue);
}

template <typename T>
static void upscale_plane(unsigned p, const VmafPicture *pic, void *dst, unsigned out_w,
                          unsigned out_h, enum VmafPixelFormat pix_fmt)
{
    const int ss_hor = (p > 0u) && (pix_fmt != VMAF_PIX_FMT_YUV444P);
    const int ss_ver = (p > 0u) && (pix_fmt == VMAF_PIX_FMT_YUV420P);
    const T *in_buf = static_cast<const T *>(pic->data[p]);
    T *out_buf = static_cast<T *>(dst);
    const ptrdiff_t in_stride_t = pic->stride[p] / static_cast<ptrdiff_t>(sizeof(T));
    for (unsigned i = 0; i < out_h; i++) {
        for (unsigned j = 0; j < out_w; j++) {
            unsigned in_x = ss_hor ? (j >> 1) : j;
            out_buf[j] = in_buf[in_x];
        }
        unsigned in_row_step = ss_ver ? (i & 1u) : 1u;
        in_buf += in_row_step * in_stride_t;
        out_buf += out_w;
    }
}

/* nd_range with 16x16 work-groups; each WG sums its 256 ΔE
 * contributions in float (per-WG max ~5000, fits cleanly in
 * float7), then writes one float to partials[wg_idx]. Host then
 * accumulates the WG totals in `double`. This is the same
 * precision pattern as ciede_vulkan, and necessary because
 * Intel Arc A380 lacks native fp64 (so sycl::reduction<double>
 * fails at runtime). */
static constexpr size_t CIEDE_SYCL_WG_X = 16;
static constexpr size_t CIEDE_SYCL_WG_Y = 16;

static void launch_ciede(sycl::queue &q, void *ref_y, void *ref_u, void *ref_v, void *dis_y,
                         void *dis_u, void *dis_v, float *d_partials, unsigned width,
                         unsigned height, unsigned bpc)
{
    /* Round work-item count up to WG-multiples; out-of-range
     * threads contribute 0.0 to the WG sum. */
    const size_t global_x = ((width + CIEDE_SYCL_WG_X - 1) / CIEDE_SYCL_WG_X) * CIEDE_SYCL_WG_X;
    const size_t global_y = ((height + CIEDE_SYCL_WG_Y - 1) / CIEDE_SYCL_WG_Y) * CIEDE_SYCL_WG_Y;
    const size_t wg_count_x = global_x / CIEDE_SYCL_WG_X;
    sycl::nd_range<2> ndr{sycl::range<2>{global_y, global_x},
                          sycl::range<2>{CIEDE_SYCL_WG_Y, CIEDE_SYCL_WG_X}};
    const unsigned e_w = width;
    const unsigned e_h = height;
    const unsigned e_bpc = bpc;
    const size_t e_wg_count_x = wg_count_x;
    void *e_ref_y = ref_y;
    void *e_ref_u = ref_u;
    void *e_ref_v = ref_v;
    void *e_dis_y = dis_y;
    void *e_dis_u = dis_u;
    void *e_dis_v = dis_v;

    q.submit([=](sycl::handler &h) {
        h.parallel_for(ndr, [=](sycl::nd_item<2> it) {
            const size_t x = it.get_global_id(1);
            const size_t y = it.get_global_id(0);
            float my_de = 0.0f;
            if (x < (size_t)e_w && y < (size_t)e_h) {
                const size_t off = y * (size_t)e_w + x;
                float r_y, r_u, r_v, d_y, d_u, d_v;
                if (e_bpc <= 8) {
                    r_y = (float)static_cast<const uint8_t *>(e_ref_y)[off];
                    r_u = (float)static_cast<const uint8_t *>(e_ref_u)[off];
                    r_v = (float)static_cast<const uint8_t *>(e_ref_v)[off];
                    d_y = (float)static_cast<const uint8_t *>(e_dis_y)[off];
                    d_u = (float)static_cast<const uint8_t *>(e_dis_u)[off];
                    d_v = (float)static_cast<const uint8_t *>(e_dis_v)[off];
                } else {
                    r_y = (float)static_cast<const uint16_t *>(e_ref_y)[off];
                    r_u = (float)static_cast<const uint16_t *>(e_ref_u)[off];
                    r_v = (float)static_cast<const uint16_t *>(e_ref_v)[off];
                    d_y = (float)static_cast<const uint16_t *>(e_dis_y)[off];
                    d_u = (float)static_cast<const uint16_t *>(e_dis_u)[off];
                    d_v = (float)static_cast<const uint16_t *>(e_dis_v)[off];
                }
                float l1, a1, b1, l2, a2, b2;
                yuv_to_lab(r_y, r_u, r_v, e_bpc, l1, a1, b1);
                yuv_to_lab(d_y, d_u, d_v, e_bpc, l2, a2, b2);
                my_de = ciede2000_dev(l1, a1, b1, l2, a2, b2);
            }
            /* Reduce within the WG via reduce_over_group. */
            float wg_sum = sycl::reduce_over_group(it.get_group(), my_de, sycl::plus<float>{});
            if (it.get_local_id(0) == 0 && it.get_local_id(1) == 0) {
                const size_t wg_idx = it.get_group(0) * e_wg_count_x + it.get_group(1);
                d_partials[wg_idx] = wg_sum;
            }
        });
    });
}

} /* anonymous namespace */

extern "C" {

static const VmafOption options_ciede_sycl[] = {{0}};

static int init_fex_sycl(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                         unsigned w, unsigned h)
{
    if (pix_fmt == VMAF_PIX_FMT_YUV400P)
        return -EINVAL;

    auto *s = static_cast<CiedeStateSycl *>(fex->priv);
    s->width = w;
    s->height = h;
    s->bpc = bpc;
    s->pix_fmt = pix_fmt;

    if (!fex->sycl_state) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "ciede_sycl: no SYCL state\n");
        return -EINVAL;
    }

    VmafSyclState *state = fex->sycl_state;
    s->sycl_state = state;

    const size_t bpp = (bpc <= 8) ? 1u : 2u;
    const size_t plane_bytes = (size_t)w * h * bpp;

    s->h_ref_y = vmaf_sycl_malloc_host(state, plane_bytes);
    s->h_ref_u = vmaf_sycl_malloc_host(state, plane_bytes);
    s->h_ref_v = vmaf_sycl_malloc_host(state, plane_bytes);
    s->h_dis_y = vmaf_sycl_malloc_host(state, plane_bytes);
    s->h_dis_u = vmaf_sycl_malloc_host(state, plane_bytes);
    s->h_dis_v = vmaf_sycl_malloc_host(state, plane_bytes);
    s->d_ref_y = vmaf_sycl_malloc_device(state, plane_bytes);
    s->d_ref_u = vmaf_sycl_malloc_device(state, plane_bytes);
    s->d_ref_v = vmaf_sycl_malloc_device(state, plane_bytes);
    s->d_dis_y = vmaf_sycl_malloc_device(state, plane_bytes);
    s->d_dis_u = vmaf_sycl_malloc_device(state, plane_bytes);
    s->d_dis_v = vmaf_sycl_malloc_device(state, plane_bytes);
    s->wg_count_x = (unsigned)((w + CIEDE_SYCL_WG_X - 1) / CIEDE_SYCL_WG_X);
    s->wg_count_y = (unsigned)((h + CIEDE_SYCL_WG_Y - 1) / CIEDE_SYCL_WG_Y);
    s->wg_count = s->wg_count_x * s->wg_count_y;
    const size_t partials_bytes = (size_t)s->wg_count * sizeof(float);
    s->d_partials = static_cast<float *>(vmaf_sycl_malloc_device(state, partials_bytes));
    s->h_partials = static_cast<float *>(vmaf_sycl_malloc_host(state, partials_bytes));
    if (!s->h_ref_y || !s->h_ref_u || !s->h_ref_v || !s->h_dis_y || !s->h_dis_u || !s->h_dis_v ||
        !s->d_ref_y || !s->d_ref_u || !s->d_ref_v || !s->d_dis_y || !s->d_dis_u || !s->d_dis_v ||
        !s->d_partials || !s->h_partials) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "ciede_sycl: USM allocation failed\n");
        return -ENOMEM;
    }

    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (!s->feature_name_dict)
        return -ENOMEM;

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
    sycl::queue &q = *qptr;

    /* Host-side chroma upscale into pinned staging. */
    if (s->bpc <= 8) {
        upscale_plane<uint8_t>(0, ref_pic, s->h_ref_y, s->width, s->height, s->pix_fmt);
        upscale_plane<uint8_t>(1, ref_pic, s->h_ref_u, s->width, s->height, s->pix_fmt);
        upscale_plane<uint8_t>(2, ref_pic, s->h_ref_v, s->width, s->height, s->pix_fmt);
        upscale_plane<uint8_t>(0, dist_pic, s->h_dis_y, s->width, s->height, s->pix_fmt);
        upscale_plane<uint8_t>(1, dist_pic, s->h_dis_u, s->width, s->height, s->pix_fmt);
        upscale_plane<uint8_t>(2, dist_pic, s->h_dis_v, s->width, s->height, s->pix_fmt);
    } else {
        upscale_plane<uint16_t>(0, ref_pic, s->h_ref_y, s->width, s->height, s->pix_fmt);
        upscale_plane<uint16_t>(1, ref_pic, s->h_ref_u, s->width, s->height, s->pix_fmt);
        upscale_plane<uint16_t>(2, ref_pic, s->h_ref_v, s->width, s->height, s->pix_fmt);
        upscale_plane<uint16_t>(0, dist_pic, s->h_dis_y, s->width, s->height, s->pix_fmt);
        upscale_plane<uint16_t>(1, dist_pic, s->h_dis_u, s->width, s->height, s->pix_fmt);
        upscale_plane<uint16_t>(2, dist_pic, s->h_dis_v, s->width, s->height, s->pix_fmt);
    }

    const size_t bpp = (s->bpc <= 8) ? 1u : 2u;
    const size_t plane_bytes = (size_t)s->width * s->height * bpp;
    q.memcpy(s->d_ref_y, s->h_ref_y, plane_bytes);
    q.memcpy(s->d_ref_u, s->h_ref_u, plane_bytes);
    q.memcpy(s->d_ref_v, s->h_ref_v, plane_bytes);
    q.memcpy(s->d_dis_y, s->h_dis_y, plane_bytes);
    q.memcpy(s->d_dis_u, s->h_dis_u, plane_bytes);
    q.memcpy(s->d_dis_v, s->h_dis_v, plane_bytes);

    launch_ciede(q, s->d_ref_y, s->d_ref_u, s->d_ref_v, s->d_dis_y, s->d_dis_u, s->d_dis_v,
                 s->d_partials, s->width, s->height, s->bpc);

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
        if (s->h_ref_y)
            vmaf_sycl_free(s->sycl_state, s->h_ref_y);
        if (s->h_ref_u)
            vmaf_sycl_free(s->sycl_state, s->h_ref_u);
        if (s->h_ref_v)
            vmaf_sycl_free(s->sycl_state, s->h_ref_v);
        if (s->h_dis_y)
            vmaf_sycl_free(s->sycl_state, s->h_dis_y);
        if (s->h_dis_u)
            vmaf_sycl_free(s->sycl_state, s->h_dis_u);
        if (s->h_dis_v)
            vmaf_sycl_free(s->sycl_state, s->h_dis_v);
        if (s->d_ref_y)
            vmaf_sycl_free(s->sycl_state, s->d_ref_y);
        if (s->d_ref_u)
            vmaf_sycl_free(s->sycl_state, s->d_ref_u);
        if (s->d_ref_v)
            vmaf_sycl_free(s->sycl_state, s->d_ref_v);
        if (s->d_dis_y)
            vmaf_sycl_free(s->sycl_state, s->d_dis_y);
        if (s->d_dis_u)
            vmaf_sycl_free(s->sycl_state, s->d_dis_u);
        if (s->d_dis_v)
            vmaf_sycl_free(s->sycl_state, s->d_dis_v);
        if (s->d_partials)
            vmaf_sycl_free(s->sycl_state, s->d_partials);
        if (s->h_partials)
            vmaf_sycl_free(s->sycl_state, s->h_partials);
    }
    if (s->feature_name_dict)
        vmaf_dictionary_free(&s->feature_name_dict);
    return 0;
}

static const char *provided_features_ciede_sycl[] = {"ciede2000", NULL};

extern "C" VmafFeatureExtractor vmaf_fex_ciede_sycl = {
    .name = "ciede_sycl",
    .init = init_fex_sycl,
    .extract = NULL,
    .flush = NULL,
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
