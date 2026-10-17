/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 *  SYCL side of test_sycl_speed_cov_math: covariance_entry() of
 *  feature/sycl/sycl_speed_cov_math.h, callable from the C test on the host
 *  and in a kernel on the default GPU, and the split form the pipeline runs
 *  (ADR-2690: differences, products and the add chain in three kernels over
 *  stored fp64 bit patterns) on the same GPU. core/test/meson.build compiles this
 *  TU with the SYCL feature line, so the kernel is built like the
 *  extractor's. Integers and fp32 only on the device (ADR-0220).
 */

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <new>
#include <optional>

#include <sycl/sycl.hpp>

#include "../src/feature/sycl/sycl_compat.h"
#include "../src/feature/sycl/sycl_speed_cov_math.h"

namespace
{

constexpr int PROBE_SG = 16;
constexpr int PROBE_GRF = 256;

struct CaseArgs {
    const float *x; /* case i: block at x + i * height * stride */
    const float *y;
    const float *mean_x; /* one per case */
    const float *mean_y;
    float *out;
    size_t stride;
    uint32_t width;
    uint32_t height;
};

__attribute__((flatten, always_inline)) inline float one_case(const CaseArgs &a, size_t i)
{
    const size_t offset = i * a.height * a.stride;
    return vmaf_sycl_speed_cov::covariance_entry(a.x + offset, a.y + offset, a.stride, a.width,
                                                 a.height, a.mean_x[i], a.mean_y[i]);
}

class CovKernel : public VmafSyclKernelShape<PROBE_SG, PROBE_GRF>
{
  public:
    explicit CovKernel(const CaseArgs &args) : a_(args)
    {
    }

    VMAF_SYCL_FUNCTOR_SG_SIZE(PROBE_SG) void operator()(sycl::id<1> i) const
    {
        a_.out[i] = one_case(a_, i[0]);
    }

  private:
    CaseArgs a_;
};

/* The split form (ADR-2690), one kernel per stage as the pipeline runs it.
 * Stage 1: dx and dy of every (case, pixel); stage 2: the term of every
 * (pixel, case), stored term-major; stage 3: the chain of every case. */
struct SplitArgs {
    CaseArgs c;
    uint64_t *dx; /* n x pixels */
    uint64_t *dy;
    uint64_t *terms; /* pixels x n */
    size_t n;
};

class SplitDifferenceKernel : public VmafSyclKernelShape<PROBE_SG, PROBE_GRF>
{
  public:
    explicit SplitDifferenceKernel(const SplitArgs &args) : a_(args)
    {
    }

    VMAF_SYCL_FUNCTOR_SG_SIZE(PROBE_SG) void operator()(sycl::id<1> id) const
    {
        const size_t pixels = static_cast<size_t>(a_.c.width) * a_.c.height;
        const size_t i = id[0] / pixels;
        const size_t k = id[0] % pixels;
        const size_t at =
            i * a_.c.height * a_.c.stride + (k / a_.c.width) * a_.c.stride + k % a_.c.width;
        namespace soft = vmaf_sycl_soft;
        a_.dx[id[0]] = soft::signed_bits(vmaf_sycl_speed_cov::covariance_difference(
            a_.c.x[at], soft::signed_from_float(a_.c.mean_x[i])));
        a_.dy[id[0]] = soft::signed_bits(vmaf_sycl_speed_cov::covariance_difference(
            a_.c.y[at], soft::signed_from_float(a_.c.mean_y[i])));
    }

  private:
    SplitArgs a_;
};

class SplitTermKernel : public VmafSyclKernelShape<PROBE_SG, PROBE_GRF>
{
  public:
    explicit SplitTermKernel(const SplitArgs &args) : a_(args)
    {
    }

    VMAF_SYCL_FUNCTOR_SG_SIZE(PROBE_SG) void operator()(sycl::id<1> id) const
    {
        const size_t pixels = static_cast<size_t>(a_.c.width) * a_.c.height;
        const size_t k = id[0] / a_.n;
        const size_t i = id[0] % a_.n;
        namespace soft = vmaf_sycl_soft;
        a_.terms[id[0]] = soft::signed_bits(
            vmaf_sycl_speed_cov::covariance_term(soft::signed_from_bits(a_.dx[i * pixels + k]),
                                                 soft::signed_from_bits(a_.dy[i * pixels + k])));
    }

  private:
    SplitArgs a_;
};

class SplitChainKernel : public VmafSyclKernelShape<PROBE_SG, PROBE_GRF>
{
  public:
    explicit SplitChainKernel(const SplitArgs &args) : a_(args)
    {
    }

    VMAF_SYCL_FUNCTOR_SG_SIZE(PROBE_SG) void operator()(sycl::id<1> id) const
    {
        const uint32_t pixels = a_.c.width * a_.c.height;
        const vmaf_sycl_soft::SoftSigned sum = vmaf_sycl_speed_cov::covariance_chain(
            vmaf_sycl_speed_cov::covariance_zero(), a_.terms + id[0], a_.n, pixels);
        a_.c.out[id[0]] = vmaf_sycl_speed_cov::covariance_store(sum, pixels);
    }

  private:
    SplitArgs a_;
};

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

void run_cases(sycl::queue &q, const CaseArgs &host, size_t n)
{
    const size_t plane = n * host.height * host.stride;
    const DeviceBlock<float> block(q, 2 * plane + 3 * n);
    float *d_x = block.get();
    float *d_y = d_x + plane;
    float *d_mx = d_y + plane;
    float *d_my = d_mx + n;
    float *d_out = d_my + n;
    q.memcpy(d_x, host.x, plane * sizeof(float));
    q.memcpy(d_y, host.y, plane * sizeof(float));
    q.memcpy(d_mx, host.mean_x, n * sizeof(float));
    q.memcpy(d_my, host.mean_y, n * sizeof(float));
    q.wait_and_throw();
    CaseArgs dev = host;
    dev.x = d_x;
    dev.y = d_y;
    dev.mean_x = d_mx;
    dev.mean_y = d_my;
    dev.out = d_out;
    q.parallel_for(sycl::range<1>(n), CovKernel(dev));
    q.wait_and_throw();
    q.memcpy(host.out, d_out, n * sizeof(float));
    q.wait_and_throw();
}

/* The split form of every case on `q`: the three stages in order. */
void run_split_cases(sycl::queue &q, const CaseArgs &host, size_t n)
{
    const size_t plane = n * host.height * host.stride;
    const size_t pixels = static_cast<size_t>(host.width) * host.height;
    const DeviceBlock<float> block(q, 2 * plane + 3 * n);
    const DeviceBlock<uint64_t> words(q, 3 * n * pixels);
    float *d_x = block.get();
    q.memcpy(d_x, host.x, plane * sizeof(float));
    q.memcpy(d_x + plane, host.y, plane * sizeof(float));
    q.memcpy(d_x + 2 * plane, host.mean_x, n * sizeof(float));
    q.memcpy(d_x + 2 * plane + n, host.mean_y, n * sizeof(float));
    q.wait_and_throw();
    SplitArgs a{.c = host,
                .dx = words.get(),
                .dy = words.get() + n * pixels,
                .terms = words.get() + 2 * n * pixels,
                .n = n};
    a.c.x = d_x;
    a.c.y = d_x + plane;
    a.c.mean_x = d_x + 2 * plane;
    a.c.mean_y = d_x + 2 * plane + n;
    a.c.out = d_x + 2 * plane + 2 * n;
    q.parallel_for(sycl::range<1>(n * pixels), SplitDifferenceKernel(a));
    q.wait_and_throw();
    q.parallel_for(sycl::range<1>(n * pixels), SplitTermKernel(a));
    q.wait_and_throw();
    q.parallel_for(sycl::range<1>(n), SplitChainKernel(a));
    q.wait_and_throw();
    q.memcpy(host.out, a.c.out, n * sizeof(float));
    q.wait_and_throw();
}

} // namespace

/* out[i] = covariance_entry() of case i, on the host. */
extern "C" void vmaf_test_sycl_cov_host(const float *x, const float *y, const float *mean_x,
                                        const float *mean_y, size_t n, size_t stride,
                                        uint32_t width, uint32_t height, float *out)
{
    const CaseArgs a{.x = x,
                     .y = y,
                     .mean_x = mean_x,
                     .mean_y = mean_y,
                     .out = out,
                     .stride = stride,
                     .width = width,
                     .height = height};
    for (size_t i = 0; i < n; i++) {
        out[i] = one_case(a, i);
    }
}

/* The same in a kernel on the default GPU: 0, -ENODEV without a device, -EIO
 * on a SYCL error. */
extern "C" int vmaf_test_sycl_cov_device(const float *x, const float *y, const float *mean_x,
                                         const float *mean_y, size_t n, size_t stride,
                                         uint32_t width, uint32_t height, float *out)
{
    if (x == nullptr || y == nullptr || mean_x == nullptr || mean_y == nullptr || out == nullptr) {
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
        const CaseArgs a{.x = x,
                         .y = y,
                         .mean_x = mean_x,
                         .mean_y = mean_y,
                         .out = out,
                         .stride = stride,
                         .width = width,
                         .height = height};
        run_cases(q, a, n);
    } catch (const std::exception &) {
        return -EIO;
    }
    return 0;
}

/* The split form of every case in kernels on the default GPU: 0, -ENODEV
 * without a device, -EIO on a SYCL error. */
extern "C" int vmaf_test_sycl_cov_split_device(const float *x, const float *y, const float *mean_x,
                                               const float *mean_y, size_t n, size_t stride,
                                               uint32_t width, uint32_t height, float *out)
{
    if (x == nullptr || y == nullptr || mean_x == nullptr || mean_y == nullptr || out == nullptr) {
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
        const CaseArgs a{.x = x,
                         .y = y,
                         .mean_x = mean_x,
                         .mean_y = mean_y,
                         .out = out,
                         .stride = stride,
                         .width = width,
                         .height = height};
        run_split_cases(q, a, n);
    } catch (const std::exception &) {
        return -EIO;
    }
    return 0;
}
