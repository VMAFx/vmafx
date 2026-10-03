/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 *  SYCL side of test_sycl_float_adm_math (ADR-1434): what
 *  float_adm_sycl.cpp's decouple, term and row-sum kernels run per work-item
 *  (feature/sycl/sycl_float_adm_math.h), callable from the C test on the host
 *  and on the device. core/test/meson.build compiles this TU with the SYCL
 *  feature line and links it through sycl_dependency, so the kernels below
 *  are built exactly like the extractor's and launched in its shapes. fp32
 *  and integers only on the device (ADR-0220); the host entry points may use
 *  double.
 */

#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <new>
#include <optional>

#include <sycl/sycl.hpp>

#include "../src/feature/sycl/sycl_compat.h"
#include "../src/feature/sycl/sycl_float_adm_math.h"
#include "sycl_float_adm_math_probe.h"

namespace
{

using vmaf_sycl_fadm::Bands;
using vmaf_sycl_fadm::DecoupleArgs;
using vmaf_sycl_fadm::GainLimit;
using vmaf_sycl_fadm::kOneBy15;
using vmaf_sycl_fadm::kOneBy30;
using vmaf_sycl_fadm::kTermSlots;
using vmaf_sycl_fadm::RowArgs;
using vmaf_sycl_fadm::TermArgs;

constexpr size_t kTile = 16;

/* Device allocation released on every exit, exceptions included. */
template <typename T> class DeviceBlock
{
  public:
    DeviceBlock(sycl::queue &queue, size_t count)
        : q_(queue), ptr_(sycl::malloc_device<T>(count == 0 ? 1 : count, queue))
    {
        if (ptr_ == nullptr) {
            throw std::bad_alloc();
        }
    }
    DeviceBlock(const DeviceBlock &) = delete;
    DeviceBlock &operator=(const DeviceBlock &) = delete;
    DeviceBlock(DeviceBlock &&) = delete;
    DeviceBlock &operator=(DeviceBlock &&) = delete;
    ~DeviceBlock()
    {
        sycl::free(ptr_, q_);
    }
    [[nodiscard]] T *get() const
    {
        return ptr_;
    }

  private:
    sycl::queue &q_;
    T *ptr_;
};

/* The sizes of one scale's buffers. */
struct ScaleSizes {
    size_t plane;    /* one sub-band */
    size_t region_w; /* the reduced region */
    size_t region_h;
};

ScaleSizes sizes_of(const VmafTestFadmScale &s)
{
    return {.plane = (size_t)s.w * (size_t)s.h,
            .region_w = (size_t)(s.right - s.left),
            .region_h = (size_t)(s.bottom - s.top)};
}

/* The buffers of one scale wherever they live: the test's on the host, device
 * allocations in a kernel. */
struct ScaleBuffers {
    const float *ref;
    const float *dis;
    float *csf_a;
    float *csf_fa;
    float *csf_r;
    float *csf_fr;
    float *terms;
    float *rows;
};

Bands bands_of(const VmafTestFadmScale &s, const ScaleBuffers &b)
{
    return {.ref_band = b.ref,
            .dis_band = b.dis,
            .csf_a = b.csf_a,
            .csf_fa = b.csf_fa,
            .csf_r = b.csf_r,
            .csf_fr = b.csf_fr,
            .half_w = s.w,
            .half_h = s.h,
            .buf_stride = s.w,
            .rfactor_h = s.rfactor[0],
            .rfactor_v = s.rfactor[1],
            .rfactor_d = s.rfactor[2]};
}

DecoupleArgs decouple_args(const VmafTestFadmScale &s, const ScaleBuffers &b)
{
    return {.bands = bands_of(s, b),
            .limit = vmaf_sycl_fadm::make_gain_limit(s.gain_limit),
            .cos_1deg_sq = s.cos_1deg_sq};
}

TermArgs term_args(const VmafTestFadmScale &s, const ScaleBuffers &b)
{
    const ScaleSizes n = sizes_of(s);
    return {.bands = bands_of(s, b),
            .terms = b.terms,
            .left = s.left,
            .top = s.top,
            .region_w = (unsigned)n.region_w,
            .region_h = (unsigned)n.region_h,
            .p_norm = (float)s.p_norm,
            .is_cube = s.p_norm == 3.0,
            .bypass_cm = s.bypass_cm != 0};
}

RowArgs row_args(const VmafTestFadmScale &s, const ScaleBuffers &b)
{
    const ScaleSizes n = sizes_of(s);
    return {.terms = b.terms,
            .rows = b.rows,
            .region_w = (unsigned)n.region_w,
            .region_h = (unsigned)n.region_h};
}

/* The three kernels' work-items in host loops. */
void run_scale_host(const VmafTestFadmScale &s, float *terms)
{
    const ScaleBuffers b = {.ref = s.ref,
                            .dis = s.dis,
                            .csf_a = s.csf[0],
                            .csf_fa = s.csf[1],
                            .csf_r = s.csf[2],
                            .csf_fr = s.csf[3],
                            .terms = terms,
                            .rows = s.rows};
    const ScaleSizes n = sizes_of(s);
    const DecoupleArgs decouple = decouple_args(s, b);
    for (int y = 0; y < s.h; y++) {
        for (int x = 0; x < s.w; x++) {
            vmaf_sycl_fadm::decouple_sample(decouple, y, x);
        }
    }
    const TermArgs terms_of = term_args(s, b);
    for (size_t ry = 0; ry < n.region_h; ry++) {
        for (size_t rx = 0; rx < n.region_w; rx++) {
            vmaf_sycl_fadm::terms_sample(terms_of, (unsigned)ry, (unsigned)rx);
        }
    }
    const RowArgs rows = row_args(s, b);
    for (size_t id = 0; id < (size_t)kTermSlots * n.region_h; id++) {
        vmaf_sycl_fadm::row_item(rows, id);
    }
}

/* The term kernel in the extractor's shape (sycl_float_adm_math.h). */
class TermsKernel
    : public VmafSyclKernelShape<vmaf_sycl_fadm::kTermsSubGroup, vmaf_sycl_fadm::kTermsGrf>
{
  public:
    explicit TermsKernel(const TermArgs &args) : args_(args)
    {
    }

    void operator()(sycl::id<2> region) const
    {
        vmaf_sycl_fadm::terms_sample(args_, (unsigned)region[0], (unsigned)region[1]);
    }

  private:
    TermArgs args_;
};

/* The three kernels in the extractor's launch shapes: 16x16 tiles for the
 * decouple, the term kernel's shape, sub-group size 16 for the rows. Each
 * reads what the one before wrote, so the queue must be in order
 * (on_default_gpu()), as libvmaf's is. */
void launch_scale(sycl::queue &q, const VmafTestFadmScale &s, const ScaleBuffers &b)
{
    const ScaleSizes n = sizes_of(s);
    const DecoupleArgs decouple = decouple_args(s, b);
    const size_t global_x = ((size_t)s.w + kTile - 1) / kTile * kTile;
    const size_t global_y = ((size_t)s.h + kTile - 1) / kTile * kTile;
    q.parallel_for(
        sycl::nd_range<2>(sycl::range<2>(global_y, global_x), sycl::range<2>(kTile, kTile)),
        [=](sycl::nd_item<2> item) {
            const int x = (int)item.get_global_id(1);
            const int y = (int)item.get_global_id(0);
            if (x < decouple.bands.half_w && y < decouple.bands.half_h) {
                vmaf_sycl_fadm::decouple_sample(decouple, y, x);
            }
        });
    const TermArgs terms_of = term_args(s, b);
    q.parallel_for(sycl::range<2>(n.region_h, n.region_w), TermsKernel(terms_of));
    const RowArgs rows = row_args(s, b);
    q.parallel_for(sycl::range<1>((size_t)kTermSlots * n.region_h),
                   [=](sycl::id<1> id)
                       VMAF_SYCL_REQD_SG_SIZE(16) { vmaf_sycl_fadm::row_item(rows, id[0]); });
    q.wait_and_throw();
}

void run_scale_device(sycl::queue &q, const VmafTestFadmScale &s)
{
    const ScaleSizes n = sizes_of(s);
    const size_t csf_floats = 3 * n.plane;
    const size_t row_floats = (size_t)kTermSlots * n.region_h;
    const DeviceBlock<float> bands(q, 8 * n.plane);
    const DeviceBlock<float> csf(q, 4 * csf_floats);
    const DeviceBlock<float> terms(q, (size_t)kTermSlots * n.region_w * n.region_h);
    const DeviceBlock<float> rows(q, row_floats);
    const ScaleBuffers b = {.ref = bands.get(),
                            .dis = bands.get() + 4 * n.plane,
                            .csf_a = csf.get(),
                            .csf_fa = csf.get() + csf_floats,
                            .csf_r = csf.get() + 2 * csf_floats,
                            .csf_fr = csf.get() + 3 * csf_floats,
                            .terms = terms.get(),
                            .rows = rows.get()};
    q.memcpy(bands.get(), s.ref, 4 * n.plane * sizeof(float));
    q.memcpy(bands.get() + 4 * n.plane, s.dis, 4 * n.plane * sizeof(float));
    q.wait_and_throw();
    launch_scale(q, s, b);
    for (size_t k = 0; k < 4; k++) {
        q.memcpy(s.csf[k], csf.get() + k * csf_floats, csf_floats * sizeof(float));
    }
    q.memcpy(s.rows, rows.get(), row_floats * sizeof(float));
    q.wait_and_throw();
}

bool scale_is_valid(const VmafTestFadmScale *s)
{
    if (s == nullptr || s->ref == nullptr || s->dis == nullptr || s->rows == nullptr) {
        return false;
    }
    for (const float *buffer : s->csf) {
        if (buffer == nullptr) {
            return false;
        }
    }
    return s->w > 0 && s->h > 0 && s->left >= 0 && s->top >= 0 && s->right > s->left &&
           s->bottom > s->top && s->right <= s->w && s->bottom <= s->h;
}

bool spots_are_valid(const VmafTestFadmSpots *p)
{
    return p != nullptr && p->a != nullptr && p->sum != nullptr && p->rst != nullptr &&
           p->t != nullptr && p->flt != nullptr && p->centre != nullptr && p->gain != nullptr;
}

/* add_scaled() with the pair's decision replaced by the replay. */
float add_scaled_replay_only(float sum, float a)
{
    if (a == 0.0f || sum == 0.0f || !std::isfinite(a) || !std::isfinite(sum)) {
        return vmaf_sycl_fadm::add_scaled(sum, a, kOneBy15);
    }
    return vmaf_sycl_fadm::add_scaled_replayed(sum, a, kOneBy15);
}

/* times_constant() with the pair's decision replaced by the replay. */
float times_constant_replay_only(float a)
{
    if (a == 0.0f || !std::isfinite(a)) {
        return vmaf_sycl_fadm::times_constant(a, kOneBy30);
    }
    return vmaf_sycl_fadm::times_constant_replayed(a, kOneBy30);
}

void run_spots_device(sycl::queue &q, const VmafTestFadmSpots &p)
{
    const size_t n = p.n;
    const DeviceBlock<float> block(q, 7 * n);
    float *a = block.get();
    float *sum = a + n;
    float *rst = a + 2 * n;
    float *t = a + 3 * n;
    float *flt = a + 4 * n;
    float *centre = a + 5 * n;
    float *gain = a + 6 * n;
    q.memcpy(a, p.a, n * sizeof(float));
    q.memcpy(sum, p.sum, n * sizeof(float));
    q.memcpy(rst, p.rst, n * sizeof(float));
    q.memcpy(t, p.t, n * sizeof(float));
    q.wait_and_throw();
    const GainLimit limit = vmaf_sycl_fadm::make_gain_limit(p.gain_limit);
    q.parallel_for(sycl::range<1>(n), [=](sycl::id<1> i) {
        flt[i] = vmaf_sycl_fadm::csf_flt(a[i]);
        centre[i] = vmaf_sycl_fadm::add_scaled(sum[i], sycl::fabs(a[i]), kOneBy15);
        gain[i] = vmaf_sycl_fadm::gain_limited(rst[i], t[i], limit);
    });
    q.wait_and_throw();
    q.memcpy(p.flt, flt, n * sizeof(float));
    q.memcpy(p.centre, centre, n * sizeof(float));
    q.memcpy(p.gain, gain, n * sizeof(float));
    q.wait_and_throw();
}

/* Run `body` on a queue of the default GPU. */
template <typename Body> int on_default_gpu(Body body)
{
    std::optional<sycl::device> device;
    try {
        device.emplace(sycl::gpu_selector_v);
    } catch (const sycl::exception &) {
        return -ENODEV;
    }
    try {
        /* In order: launch_scale() submits three kernels that each read the
         * one before. On an out-of-order queue an Arc B580 runs them
         * concurrently and the row sums read terms not yet written. */
        sycl::queue q(*device, sycl::property::queue::in_order{});
        body(q);
    } catch (const std::exception &) {
        return -EIO;
    }
    return 0;
}

} // namespace

extern "C" int vmaf_test_sycl_fadm_scale(const VmafTestFadmScale *scale, int on_device)
{
    if (!scale_is_valid(scale)) {
        return -EINVAL;
    }
    if (on_device != 0) {
        return on_default_gpu([scale](sycl::queue &q) { run_scale_device(q, *scale); });
    }
    const ScaleSizes n = sizes_of(*scale);
    const std::unique_ptr<float[]> terms(
        new (std::nothrow) float[(size_t)kTermSlots * n.region_w * n.region_h]);
    if (!terms) {
        return -ENOMEM;
    }
    run_scale_host(*scale, terms.get());
    return 0;
}

extern "C" void vmaf_test_sycl_fadm_spots_host(const VmafTestFadmSpots *spots, int replay_only,
                                               size_t *undecided)
{
    size_t count = 0;
    if (spots_are_valid(spots)) {
        const GainLimit limit = vmaf_sycl_fadm::make_gain_limit(spots->gain_limit);
        for (size_t i = 0; i < spots->n; i++) {
            const float a = std::fabs(spots->a[i]);
            const bool replay = replay_only != 0;
            spots->flt[i] =
                replay ? times_constant_replay_only(a) : vmaf_sycl_fadm::csf_flt(spots->a[i]);
            spots->centre[i] = replay ? add_scaled_replay_only(spots->sum[i], a) :
                                        vmaf_sycl_fadm::add_scaled(spots->sum[i], a, kOneBy15);
            spots->gain[i] = vmaf_sycl_fadm::gain_limited(spots->rst[i], spots->t[i], limit);
            if (a > 0.0f && std::isfinite(a) &&
                vmaf_sycl_fadm::undecided(vmaf_sycl_fadm::scaled_pair(a, kOneBy30))) {
                count++;
            }
        }
    }
    if (undecided != nullptr) {
        *undecided = count;
    }
}

extern "C" int vmaf_test_sycl_fadm_spots_device(const VmafTestFadmSpots *spots)
{
    if (!spots_are_valid(spots)) {
        return -EINVAL;
    }
    if (spots->n == 0) {
        return 0;
    }
    return on_default_gpu([spots](sycl::queue &q) { run_spots_device(q, *spots); });
}

extern "C" void vmaf_test_sycl_fadm_constants(double out[4])
{
    out[0] = std::ldexp((double)kOneBy30.mant, kOneBy30.exp);
    out[1] = std::ldexp((double)kOneBy15.mant, kOneBy15.exp);
    out[2] = (double)kOneBy30.hi + (double)kOneBy30.lo;
    out[3] = (double)kOneBy15.hi + (double)kOneBy15.lo;
}
