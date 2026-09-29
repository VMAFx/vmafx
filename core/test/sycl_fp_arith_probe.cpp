/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 *  Device side of test_sycl_fp_arith_contract (ADR-1367). core/test/meson.build
 *  compiles this TU with the SYCL feature line (sycl_toolchain_args +
 *  sycl_feature_tail_args) and links it through sycl_dependency, so the kernel
 *  below is built exactly like an extractor's: AOT images at compile time,
 *  the SPIR-V JIT image at the link. fp32 only (ADR-0220).
 */

#include <cerrno>
#include <cstddef>
#include <exception>
#include <new>
#include <optional>

#include <sycl/sycl.hpp>

namespace
{

struct ProbeBuffers {
    const float *a;
    const float *b;
    const float *c;
    float *mad;
    float *quot;
    float *root;
    size_t n;
};

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

/* One multiply-add written as a single expression, one division and one square
 * root per element: the three operations icpx changes on the device when the
 * strict FP line is incomplete. */
void run_kernel(sycl::queue &q, const ProbeBuffers &host)
{
    const size_t n = host.n;
    const size_t bytes = n * sizeof(float);
    const DeviceBlock block(q, 6 * n);
    float *dev = block.get();
    float *da = dev;
    float *db = dev + n;
    float *dc = dev + (2 * n);
    float *dmad = dev + (3 * n);
    float *dquot = dev + (4 * n);
    float *droot = dev + (5 * n);
    q.memcpy(da, host.a, bytes);
    q.memcpy(db, host.b, bytes);
    q.memcpy(dc, host.c, bytes);
    q.wait_and_throw();
    q.parallel_for(sycl::range<1>(n), [=](sycl::id<1> i) {
        dmad[i] = da[i] * db[i] + dc[i];
        dquot[i] = da[i] / db[i];
        droot[i] = sycl::sqrt(sycl::fabs(da[i]));
    });
    q.wait_and_throw();
    q.memcpy(host.mad, dmad, bytes);
    q.memcpy(host.quot, dquot, bytes);
    q.memcpy(host.root, droot, bytes);
    q.wait_and_throw();
}

} // namespace

extern "C" int vmaf_test_sycl_fp_arith(const float *a, const float *b, const float *c, size_t n,
                                       float *mad, float *quot, float *root)
{
    if (a == nullptr || b == nullptr || c == nullptr || mad == nullptr || quot == nullptr ||
        root == nullptr) {
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
        run_kernel(q, ProbeBuffers{
                          .a = a, .b = b, .c = c, .mad = mad, .quot = quot, .root = root, .n = n});
    } catch (const std::exception &) {
        return -EIO;
    }
    return 0;
}
