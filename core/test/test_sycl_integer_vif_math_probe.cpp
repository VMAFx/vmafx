/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 *  SYCL side of test_sycl_integer_vif_math (ADR-1432): the gain terms
 *  integer_vif_sycl.cpp's kernels compute
 *  (feature/sycl/sycl_integer_vif_math.h), callable from the C test on the
 *  host and on the device. core/test/meson.build compiles this TU with the
 *  SYCL feature line and links it through sycl_dependency, so the kernel below
 *  is built exactly like the extractor's. Integers and fp32 only on the
 *  device (ADR-0220).
 */

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <new>
#include <optional>

#include <sycl/sycl.hpp>

#include "../src/feature/sycl/sycl_compat.h"
#include "../src/feature/sycl/sycl_integer_vif_math.h"

namespace
{

using vmaf_sycl_ivif::GainLimit;
using vmaf_sycl_ivif::GainResult;
using vmaf_sycl_ivif::GainTerms;

/* Device allocation released on every exit, exceptions included. */
template <typename T> class DeviceBlock
{
  public:
    DeviceBlock(sycl::queue &queue, size_t count)
        : q_(queue), ptr_(sycl::malloc_device<T>(count, queue))
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

struct ProbeBuffers {
    const uint32_t *sigmas; /* sigma1_sq, sigma2_sq, sigma12 per sample */
    uint32_t *sv_sq;
    int64_t *gg_sigma;
    size_t n;
};

/* The gain terms of every sample, one work-item each at sub-group size 16,
 * the size the extractor's kernels run at by default. */
void run_kernel(sycl::queue &q, const ProbeBuffers &host, const GainLimit &limit)
{
    const size_t n = host.n;
    const DeviceBlock<uint32_t> words(q, 4 * n);
    const DeviceBlock<int64_t> wide(q, n);
    uint32_t *sigmas = words.get();
    uint32_t *sv_sq = sigmas + 3 * n;
    int64_t *gg_sigma = wide.get();
    q.memcpy(sigmas, host.sigmas, 3 * n * sizeof(uint32_t));
    q.wait_and_throw();
    q.parallel_for(sycl::range<1>(n), [=](sycl::id<1> i) [[VMAF_SYCL_REQD_SG_ATTR(16)]] {
        const uint32_t *s = sigmas + 3 * i[0];
        const GainTerms terms = vmaf_sycl_ivif::gain_terms(s[0], s[1], s[2], limit);
        sv_sq[i] = terms.sv_sq;
        gg_sigma[i] = terms.gg_sigma;
    });
    q.wait_and_throw();
    q.memcpy(host.sv_sq, sv_sq, n * sizeof(uint32_t));
    q.memcpy(host.gg_sigma, gg_sigma, n * sizeof(int64_t));
    q.wait_and_throw();
}

} // namespace

/* The gain terms of n samples on the host, three ways. `path` 0: as the
 * kernels select; 1: the integer evaluation alone; 2: the replay alone.
 * `replays`, when not null, counts the samples the integer evaluation does
 * not decide. */
extern "C" void vmaf_test_sycl_ivif_host(const uint32_t *sigmas, size_t n, double gain_limit,
                                         int path, uint32_t *sv_sq, int64_t *gg_sigma,
                                         size_t *replays)
{
    const GainLimit limit = vmaf_sycl_ivif::make_gain_limit(gain_limit);
    size_t undecided = 0;
    for (size_t i = 0; i < n; i++) {
        const uint32_t *s = sigmas + 3 * i;
        const GainResult fast = vmaf_sycl_ivif::gain_terms_integer(s[0], s[1], s[2], limit);
        undecided += fast.replay ? 1u : 0u;
        GainTerms terms = fast.terms;
        if (path == 2 || (path == 0 && fast.replay)) {
            terms = vmaf_sycl_ivif::gain_terms_replayed(s[0], s[1], s[2], limit);
        }
        sv_sq[i] = terms.sv_sq;
        gg_sigma[i] = terms.gg_sigma;
    }
    if (replays != nullptr) {
        *replays = undecided;
    }
}

/* The same, as the kernels select, on the default GPU. 0, -ENODEV without a
 * device, -EIO on a SYCL error. */
extern "C" int vmaf_test_sycl_ivif_device(const uint32_t *sigmas, size_t n, double gain_limit,
                                          uint32_t *sv_sq, int64_t *gg_sigma)
{
    if (sigmas == nullptr || sv_sq == nullptr || gg_sigma == nullptr) {
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
        run_kernel(q, ProbeBuffers{.sigmas = sigmas, .sv_sq = sv_sq, .gg_sigma = gg_sigma, .n = n},
                   vmaf_sycl_ivif::make_gain_limit(gain_limit));
    } catch (const std::exception &) {
        return -EIO;
    }
    return 0;
}
