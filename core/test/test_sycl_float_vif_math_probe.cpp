/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 *  SYCL side of test_sycl_float_vif_math (ADR-1422): the arithmetic
 *  float_vif_sycl.cpp's kernels run (feature/sycl/sycl_float_vif_math.h),
 *  callable from the C test on the host and on the device. core/test/meson.build
 *  compiles this TU with the SYCL feature line (sycl_toolchain_args +
 *  sycl_feature_tail_args) and links it through sycl_dependency, so the kernel
 *  below is built exactly like the extractor's. fp32 and integers only on the
 *  device (ADR-0220).
 */

#include <cerrno>
#include <cstddef>
#include <exception>
#include <new>
#include <optional>

#include <sycl/sycl.hpp>

#include "../src/feature/sycl/sycl_compat.h"
#include "../src/feature/sycl/sycl_float_vif_math.h"

namespace
{

using vmaf_sycl_fvif::Denominator;
using vmaf_sycl_fvif::NoiseVariance;
using vmaf_sycl_fvif::StatisticParams;
using vmaf_sycl_fvif::Term;

constexpr size_t kMomentFloats = 5;

Term statistic_of(const float *moments, const StatisticParams &params)
{
    return vmaf_sycl_fvif::pixel_statistic(vmaf_sycl_fvif::pixel_sigmas({.mu1 = moments[0],
                                                                         .mu2 = moments[1],
                                                                         .xx = moments[2],
                                                                         .yy = moments[3],
                                                                         .xy = moments[4]}),
                                           params);
}

/* Device allocation released on every exit, exceptions included. */
class DeviceBlock
{
  public:
    DeviceBlock(sycl::queue &queue, size_t count)
        : q_(queue), ptr_(sycl::malloc_device<float>(count, queue))
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
    [[nodiscard]] float *get() const
    {
        return ptr_;
    }

  private:
    sycl::queue &q_;
    float *ptr_;
};

struct ProbeBuffers {
    const float *moments;
    float *num;
    float *den;
    size_t n;
};

/* The statistic of every sample, in a kernel shaped like the extractor's
 * statistic kernel: one work-item per pixel, sub-group size 16. */
void run_kernel(sycl::queue &q, const ProbeBuffers &host, const StatisticParams &params)
{
    const size_t n = host.n;
    const DeviceBlock block(q, (kMomentFloats + 2) * n);
    float *moments = block.get();
    float *num = moments + kMomentFloats * n;
    float *den = num + n;
    q.memcpy(moments, host.moments, kMomentFloats * n * sizeof(float));
    q.wait_and_throw();
    q.parallel_for(sycl::range<1>(n), [=](sycl::id<1> i) [[VMAF_SYCL_REQD_SG_ATTR(16)]] {
        const Term term = statistic_of(moments + kMomentFloats * i[0], params);
        num[i] = term.num;
        den[i] = term.den;
    });
    q.wait_and_throw();
    q.memcpy(host.num, num, n * sizeof(float));
    q.memcpy(host.den, den, n * sizeof(float));
    q.wait_and_throw();
}

} // namespace

/* vif_pixel_statistic_s() of n samples (five moments each) on the host. */
extern "C" void vmaf_test_sycl_fvif_host(const float *moments, size_t n, double sigma_nsq,
                                         double gain_limit, float *num, float *den)
{
    const StatisticParams params = vmaf_sycl_fvif::make_statistic_params(sigma_nsq, gain_limit);
    for (size_t i = 0; i < n; i++) {
        const Term term = statistic_of(moments + kMomentFloats * i, params);
        num[i] = term.num;
        den[i] = term.den;
    }
}

/* The same on the default GPU. 0, -ENODEV without a device, -EIO on a SYCL
 * error. */
extern "C" int vmaf_test_sycl_fvif_device(const float *moments, size_t n, double sigma_nsq,
                                          double gain_limit, float *num, float *den)
{
    if (moments == nullptr || num == nullptr || den == nullptr) {
        return -EINVAL;
    }
    if (n == 0) {
        return 0;
    }
    std::optional<sycl::device> device;
    try {
        device.emplace(sycl::gpu_selector_v);
    } catch (const sycl::exception &) {
        return -ENODEV;
    }
    try {
        sycl::queue q(*device);
        run_kernel(q, ProbeBuffers{.moments = moments, .num = num, .den = den, .n = n},
                   vmaf_sycl_fvif::make_statistic_params(sigma_nsq, gain_limit));
    } catch (const std::exception &) {
        return -EIO;
    }
    return 0;
}

/* 1.0f + numerator / (addend + sigma_nsq) on the host, three ways:
 * out[0] as the kernels select, out[1] by the pair alone, out[2] by the
 * integer replay alone. `addend` is sv_sq for the numerator term and absent
 * (has_addend 0) for the denominator term. numerator and the denominator
 * are positive and normal. Returns 1 when the kernels select the replay. */
extern "C" int vmaf_test_sycl_fvif_ratio(float numerator, int has_addend, float addend,
                                         double sigma_nsq, float out[3])
{
    const NoiseVariance noise = vmaf_sycl_fvif::make_noise_variance(sigma_nsq);
    const Denominator denominator = has_addend != 0 ? vmaf_sycl_fvif::noise_plus(addend, noise) :
                                                      vmaf_sycl_fvif::noise_denominator(noise);
    const vmaf_sycl_fvif::Ff sum = vmaf_sycl_fvif::one_plus_ratio_pair(numerator, denominator.pair);
    out[0] = vmaf_sycl_fvif::one_plus_ratio(numerator, denominator);
    out[1] = sum.hi;
    out[2] = vmaf_sycl_fvif::one_plus_ratio_replayed(numerator, denominator.exact);
    return vmaf_sycl_fvif::needs_replay(numerator, sum, denominator) ? 1 : 0;
}
