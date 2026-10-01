/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 *  Scratch-memory self-test and kernel audit for the SYCL backend
 *  (ADR-1395). See scratch_check.h.
 */
#include "config.h"

#include <sycl/sycl.hpp>

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#if __has_include(<cxxabi.h>)
#include <cxxabi.h>
#define VMAF_SYCL_HAVE_CXXABI 1
#else
#define VMAF_SYCL_HAVE_CXXABI 0
#endif

#include "feature/sycl/sycl_compat.h"
#include "gpu_dispatch_env.h"
#include "log.h"
#include "scratch_check.h"

namespace
{
/* Extractors that still run a kernel listed in scratch_ratchet.txt. Keep in
 * step with that file; test_sycl_kernel_scratch compares the two. */
constexpr const char *kScratchExtractors =
    "float_adm_sycl, float_vif_sycl, motion_sycl, motion_v2_sycl, "
    "speed_chroma_sycl, speed_temporal_sycl";

bool selftest_disabled()
{
    const char *env = vmaf_gpu_dispatch_env_get("VMAF_SYCL_SCRATCH_SELFTEST");
    return env != nullptr && std::strcmp(env, "0") == 0;
}
} // namespace

/* The audit and the probes query kernel bundles and the Intel spill-size
 * extension, which only the oneAPI toolchain provides. */
#if defined(__INTEL_LLVM_COMPILER)

/* Kernel names of the two probes. They use scratch memory on purpose, so the
 * audit leaves them out. */
class VmafSyclScratchProbePrivate;
class VmafSyclScratchProbeSpill;

namespace
{
constexpr uint32_t kProbeItems = 256;   /* one work-group per probe */
constexpr uint32_t kPrivateWords = 256; /* 1 KiB per work-item: never fits in registers */
constexpr uint32_t kPrivateTouches = 16;
constexpr uint32_t kSpillWords = 96; /* at SIMD-32 more than 128 registers hold */
constexpr uint32_t kSpillRounds = 4;

/* The private-array probe: a data-dependent index keeps the array in private
 * memory. Integer arithmetic makes host and device results exact. */
uint32_t private_probe_value(uint32_t gid, uint32_t j)
{
    uint32_t a[kPrivateWords];
    for (uint32_t k = 0; k < kPrivateWords; ++k)
        a[k] = (gid * 2654435761U) + k;
    for (uint32_t r = 0; r < kPrivateTouches; ++r)
        a[(j + r) % kPrivateWords] += 1U;
    uint32_t s = 0;
    for (uint32_t k = 0; k < kPrivateWords; ++k)
        s += a[((k * 7U) + j) % kPrivateWords] * ((k & 3U) + 1U);
    return s;
}

/* The register-spill probe: kSpillWords values stay live across every round,
 * which at SIMD-32 needs more registers than a hardware thread has. */
uint32_t spill_probe_value(const uint32_t *src, uint32_t gid, uint32_t stride)
{
    uint32_t v[kSpillWords];
#pragma unroll
    for (uint32_t k = 0; k < kSpillWords; ++k)
        v[k] = src[(k * stride) + gid];
#pragma unroll
    for (uint32_t r = 0; r < kSpillRounds; ++r) {
#pragma unroll
        for (uint32_t k = 0; k < kSpillWords; ++k)
            v[k] = (v[k] * 3U) + (v[(k + 1U + r) % kSpillWords] >> 2U);
    }
    uint32_t s = 0;
#pragma unroll
    for (uint32_t k = 0; k < kSpillWords; ++k)
        s += v[k] * (k + 1U);
    return s;
}

unsigned count_wrong(const std::vector<uint32_t> &got, const std::vector<uint32_t> &want)
{
    unsigned wrong = 0;
    for (size_t i = 0; i < got.size() && i < want.size(); ++i)
        wrong += (got[i] != want[i]) ? 1U : 0U;
    return wrong;
}

struct KernelScratch {
    size_t private_bytes;
    size_t spill_bytes;
};

KernelScratch kernel_scratch(const sycl::kernel &k, const sycl::device &dev)
{
    KernelScratch s{
        .private_bytes = k.get_info<sycl::info::kernel_device_specific::private_mem_size>(dev),
        .spill_bytes = 0,
    };
    if (dev.has(sycl::aspect::ext_intel_spill_memory_size))
        s.spill_bytes =
            k.get_info<sycl::ext::intel::info::kernel_device_specific::spill_memory_size>(dev);
    return s;
}

template <typename Name> KernelScratch probe_scratch(const sycl::queue &q)
{
    const sycl::device dev = q.get_device();
    const sycl::kernel_id id = sycl::get_kernel_id<Name>();
    const auto bundle =
        sycl::get_kernel_bundle<sycl::bundle_state::executable>(q.get_context(), {dev}, {id});
    return kernel_scratch(bundle.get_kernel(id), dev);
}

void run_private_probe(sycl::queue &q, VmafSyclScratchProbe &out)
{
    assert(out.work_items == kProbeItems);
    std::vector<uint32_t> idx(kProbeItems);
    std::vector<uint32_t> want(kProbeItems);
    std::vector<uint32_t> got(kProbeItems, 0U);
    for (uint32_t i = 0; i < kProbeItems; ++i) {
        idx[i] = (i * 40503U) % kPrivateWords;
        want[i] = private_probe_value(i, idx[i]);
    }
    {
        sycl::buffer<uint32_t, 1> idx_buf(idx.data(), sycl::range<1>(kProbeItems));
        sycl::buffer<uint32_t, 1> out_buf(got.data(), sycl::range<1>(kProbeItems));
        q.submit([&](sycl::handler &cgh) {
            const sycl::accessor in(idx_buf, cgh, sycl::read_only);
            const sycl::accessor res(out_buf, cgh, sycl::write_only, sycl::no_init);
            cgh.parallel_for<VmafSyclScratchProbePrivate>(
                sycl::nd_range<1>(kProbeItems, kProbeItems), [=](sycl::nd_item<1> it) {
                    const auto gid = static_cast<uint32_t>(it.get_global_linear_id());
                    res[gid] = private_probe_value(gid, in[gid]);
                });
        });
    } /* the buffers write back here */
    out.private_wrong = count_wrong(got, want);
    assert(out.private_wrong <= out.work_items);
    out.private_bytes = probe_scratch<VmafSyclScratchProbePrivate>(q).private_bytes;
}

bool has_sub_group_size(const sycl::device &dev, size_t size)
{
    const auto sizes = dev.get_info<sycl::info::device::sub_group_sizes>();
    return std::ranges::find(sizes, size) != sizes.end();
}

void run_spill_probe(sycl::queue &q, VmafSyclScratchProbe &out)
{
    assert(out.work_items == kProbeItems);
    if (!has_sub_group_size(q.get_device(), 32))
        return;
    std::vector<uint32_t> src(static_cast<size_t>(kSpillWords) * kProbeItems);
    for (size_t i = 0; i < src.size(); ++i)
        src[i] = static_cast<uint32_t>(i * 2654435761U);
    std::vector<uint32_t> want(kProbeItems);
    std::vector<uint32_t> got(kProbeItems, 0U);
    for (uint32_t i = 0; i < kProbeItems; ++i)
        want[i] = spill_probe_value(src.data(), i, kProbeItems);
    {
        sycl::buffer<uint32_t, 1> src_buf(src.data(), sycl::range<1>(src.size()));
        sycl::buffer<uint32_t, 1> out_buf(got.data(), sycl::range<1>(kProbeItems));
        q.submit([&](sycl::handler &cgh) {
            const sycl::accessor in(src_buf, cgh, sycl::read_only);
            const sycl::accessor res(out_buf, cgh, sycl::write_only, sycl::no_init);
            cgh.parallel_for<VmafSyclScratchProbeSpill>(
                sycl::nd_range<1>(kProbeItems, kProbeItems),
                [=](sycl::nd_item<1> it) VMAF_SYCL_REQD_SG_SIZE(32) {
                    const auto gid = static_cast<uint32_t>(it.get_global_linear_id());
                    res[gid] = spill_probe_value(
                        in.get_multi_ptr<sycl::access::decorated::no>().get(), gid, kProbeItems);
                });
        });
    }
    out.spill_ran = 1;
    out.spill_wrong = count_wrong(got, want);
    assert(out.spill_wrong <= out.work_items);
    out.spill_bytes = probe_scratch<VmafSyclScratchProbeSpill>(q).spill_bytes;
}

/* The name the audit reports next to the mangled one: "typeinfo name for X"
 * becomes "X". */
std::string pretty_kernel_name(const char *name)
{
#if VMAF_SYCL_HAVE_CXXABI
    int status = 0;
    char *demangled = abi::__cxa_demangle(name, nullptr, nullptr, &status);
    if (status == 0 && demangled != nullptr) {
        std::string pretty(demangled);
        std::free(demangled);
        const std::string prefix = "typeinfo name for ";
        if (pretty.starts_with(prefix))
            pretty.erase(0, prefix.size());
        return pretty;
    }
    std::free(demangled);
#endif
    return name;
}

using ScratchVisit = void (*)(const VmafSyclKernelScratch *kernel, void *user);

void report_kernel(const sycl::context &ctx, const sycl::device &dev, const sycl::kernel_id &id,
                   ScratchVisit fn, void *user)
{
    const auto bundle = sycl::get_kernel_bundle<sycl::bundle_state::executable>(ctx, {dev}, {id});
    const KernelScratch s = kernel_scratch(bundle.get_kernel(id), dev);
    const std::string pretty = pretty_kernel_name(id.get_name());
    const VmafSyclKernelScratch k{
        .name = id.get_name(),
        .pretty = pretty.c_str(),
        .private_bytes = s.private_bytes,
        .spill_bytes = s.spill_bytes,
    };
    fn(&k, user);
}

/* True the first time a device is seen. Hashes, not devices, are kept: a
 * static sycl::device would be destroyed after the SYCL runtime at exit. */
bool first_visit(const sycl::device &dev)
{
    static std::vector<size_t> seen;
    const size_t key = std::hash<sycl::device>{}(dev);
    if (std::ranges::find(seen, key) != seen.end())
        return false;
    seen.push_back(key);
    return true;
}

void log_selftest_result(const sycl::device &dev, const VmafSyclScratchProbe &r)
{
    const std::string name = dev.get_info<sycl::info::device::name>();
    if (r.private_wrong == 0 && r.spill_wrong == 0) {
        vmaf_log(VMAF_LOG_LEVEL_DEBUG,
                 "SYCL: scratch self-test passed on %s (private array %zu B%s, spill %zu B)\n",
                 name.c_str(), r.private_bytes, r.spill_ran ? "" : ", no SIMD-32 spill probe",
                 r.spill_bytes);
        return;
    }
    vmaf_log(VMAF_LOG_LEVEL_WARNING,
             "SYCL: %s returns wrong values from kernels that use scratch memory "
             "(private-array probe: %u of %u work-items wrong; register-spill probe: %u of %u). "
             "Seen on Arc A-series GPUs under the Linux xe kernel driver, not under i915. SYCL "
             "extractors that still use scratch memory may report wrong scores on this device: "
             "%s. See docs/backends/sycl/overview.md (ADR-1395).\n",
             name.c_str(), r.private_wrong, r.work_items, r.spill_wrong,
             r.spill_ran ? r.work_items : 0U, kScratchExtractors);
}
} // namespace

extern "C" int vmaf_sycl_scratch_probe(void *queue, VmafSyclScratchProbe *out)
{
    if (queue == nullptr || out == nullptr)
        return -EINVAL;
    *out = VmafSyclScratchProbe{};
    out->work_items = kProbeItems;
    try {
        auto &q = *static_cast<sycl::queue *>(queue);
        run_private_probe(q, *out);
        run_spill_probe(q, *out);
        return 0;
    } catch (const sycl::exception &e) {
        vmaf_log(VMAF_LOG_LEVEL_DEBUG, "SYCL: scratch probe failed: %s\n", e.what());
        return -EIO;
    }
}

extern "C" void vmaf_sycl_scratch_selftest(void *queue)
{
    if (queue == nullptr || selftest_disabled())
        return;
    static std::mutex lock;
    const std::scoped_lock guard(lock);
    try {
        const sycl::device dev = static_cast<sycl::queue *>(queue)->get_device();
        if (!first_visit(dev))
            return;
        VmafSyclScratchProbe r{};
        const int err = vmaf_sycl_scratch_probe(queue, &r);
        if (err != 0) {
            vmaf_log(VMAF_LOG_LEVEL_DEBUG, "SYCL: scratch self-test skipped (%d)\n", err);
            return;
        }
        log_selftest_result(dev, r);
    } catch (const sycl::exception &e) {
        vmaf_log(VMAF_LOG_LEVEL_DEBUG, "SYCL: scratch self-test skipped: %s\n", e.what());
    }
}

extern "C" int vmaf_sycl_kernel_scratch_audit(ScratchVisit fn, void *user)
{
    if (fn == nullptr)
        return -EINVAL;
    try {
        if (sycl::device::get_devices(sycl::info::device_type::gpu).empty())
            return -ENODEV;
        const sycl::device dev{sycl::gpu_selector_v};
        const sycl::context ctx{dev};
        const sycl::kernel_id probe_private = sycl::get_kernel_id<VmafSyclScratchProbePrivate>();
        const sycl::kernel_id probe_spill = sycl::get_kernel_id<VmafSyclScratchProbeSpill>();
        int reported = 0;
        for (const sycl::kernel_id &id : sycl::get_kernel_ids()) {
            if (id == probe_private || id == probe_spill || !sycl::is_compatible({id}, dev))
                continue;
            report_kernel(ctx, dev, id, fn, user);
            ++reported;
        }
        return reported;
    } catch (const sycl::exception &e) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "SYCL: kernel scratch audit failed: %s\n", e.what());
        return -EIO;
    }
}

#else /* !__INTEL_LLVM_COMPILER */

extern "C" int vmaf_sycl_scratch_probe(void *queue, VmafSyclScratchProbe *out)
{
    return (queue == nullptr || out == nullptr) ? -EINVAL : -ENOSYS;
}

extern "C" void vmaf_sycl_scratch_selftest(void *queue)
{
    if (queue != nullptr && !selftest_disabled())
        vmaf_log(VMAF_LOG_LEVEL_DEBUG, "SYCL: scratch self-test needs the oneAPI toolchain\n");
}

extern "C" int vmaf_sycl_kernel_scratch_audit(void (*fn)(const VmafSyclKernelScratch *, void *),
                                              void *user)
{
    (void)user;
    return (fn == nullptr) ? -EINVAL : -ENOSYS;
}

#endif /* __INTEL_LLVM_COMPILER */

extern "C" const char *vmaf_sycl_scratch_extractors()
{
    return kScratchExtractors;
}
