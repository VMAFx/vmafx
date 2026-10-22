/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 *  SYCL side of test_sycl_ordered_sum (ADR-1446): the stages a twin builds
 *  around feature/ordered_sum.h (feature/sycl/sycl_ordered_sum.h), over an
 *  array of terms instead of a picture. The plan, the increments and the kept
 *  chunks are formed here with the header's functions, as the extractor's
 *  kernels form them; the walk runs on the host or in a kernel.
 *  core/test/meson.build compiles this TU with the SYCL feature line.
 */

#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <new>
#include <optional>
#include <vector>

#include <sycl/sycl.hpp>

#include "../src/feature/sycl/sycl_compat.h"
#include "../src/feature/sycl/sycl_ordered_sum.h"
#include "sycl_ordered_sum_probe.h"

namespace
{

using vmaf_sycl_ordsum::kChunk;
using vmaf_sycl_ordsum::kRun;
using vmaf_sycl_ordsum::kRuns;
using vmaf_sycl_ordsum::kRunUnits;
using vmaf_sycl_ordsum::kSlots;
using vmaf_sycl_ordsum::SumWalk;

/* Everything the stages before the walk produce for one sum. */
struct Staged {
    std::vector<int16_t> plan;
    std::vector<int64_t> units;
    std::vector<int32_t> slot_chunk;
    std::vector<int16_t> slot_binade;
    std::vector<uint64_t> terms;
    std::vector<int64_t> run_units;
    unsigned chunks;
};

size_t chunk_end(const VmafTestOrdsumCase &c, unsigned chunk)
{
    const size_t end = ((size_t)chunk + 1u) * kChunk;
    return end < c.count ? end : c.count;
}

/* The fp32 value of a term times 2^scale_log2, as the extractor's advice
 * has it. */
float advice_term(uint64_t bits, int scale_log2)
{
    const double value = sycl::bit_cast<double>(bits);
    return (float)std::ldexp(value, scale_log2);
}

float chunk_advice(const VmafTestOrdsumCase &c, unsigned chunk)
{
    if (c.advice == VMAF_TEST_ORDSUM_ADVICE_ZERO) {
        return 0.0f;
    }
    if (c.advice == VMAF_TEST_ORDSUM_ADVICE_CONSTANT) {
        return c.advice_factor;
    }
    if (c.advice == VMAF_TEST_ORDSUM_ADVICE_NAN) {
        return std::numeric_limits<float>::quiet_NaN();
    }
    float sum = 0.0f;
    for (size_t i = (size_t)chunk * kChunk; i < chunk_end(c, chunk); i++) {
        sum += advice_term(c.terms[i], c.scale_log2);
    }
    return sum * c.advice_factor;
}

/* Stage 3: every chunk's increment under its plan, in term order. */
void stage_units(const VmafTestOrdsumCase &c, Staged &s)
{
    for (unsigned chunk = 0; chunk < s.chunks; chunk++) {
        VmafOrdsumUnits units = vmaf_ordsum_units(0, 0);
        const int plan = vmaf_sycl_ordsum::plan_of(s.plan[chunk]);
        for (size_t i = (size_t)chunk * kChunk; i < chunk_end(c, chunk); i++) {
            units = vmaf_ordsum_then(units, vmaf_ordsum_planned_term_bits(c.terms[i], plan));
        }
        s.units[(size_t)chunk * 2u] = units.even;
        s.units[(size_t)chunk * 2u + 1u] = units.odd;
    }
}

/* Stage 3c: the terms and runs of the chunks with a slot. */
void stage_slots(const VmafTestOrdsumCase &c, Staged &s)
{
    for (unsigned slot = 0; slot < kSlots; slot++) {
        if (s.slot_chunk[slot] < 0) {
            continue;
        }
        const size_t first = (size_t)s.slot_chunk[slot] * kChunk;
        for (unsigned j = 0; j < kChunk; j++) {
            const uint64_t bits = first + j < c.count ? c.terms[first + j] : uint64_t{0};
            s.terms[(size_t)slot * kChunk + j] = bits;
            int64_t *run = &s.run_units[((size_t)slot * kRuns + j / kRun) * kRunUnits];
            vmaf_sycl_ordsum::stage_run(bits, (int)s.slot_binade[slot], run);
        }
    }
}

Staged stage(const VmafTestOrdsumCase &c)
{
    const auto chunks = (unsigned)((c.count + kChunk - 1u) / kChunk);
    Staged s = {.plan = std::vector<int16_t>(chunks),
                .units = std::vector<int64_t>((size_t)chunks * 2u),
                .slot_chunk = std::vector<int32_t>(kSlots),
                .slot_binade = std::vector<int16_t>(kSlots),
                .terms = std::vector<uint64_t>((size_t)kSlots * kChunk),
                .run_units = std::vector<int64_t>((size_t)kSlots * kRuns * kRunUnits),
                .chunks = chunks};
    std::vector<float> advice(chunks);
    for (unsigned chunk = 0; chunk < chunks; chunk++) {
        advice[chunk] = chunk_advice(c, chunk);
    }
    vmaf_sycl_ordsum::plan_chunks(advice.data(), 1u, chunks, c.scale_log2,
                                  {.plan = s.plan.data(),
                                   .slot_chunk = s.slot_chunk.data(),
                                   .slot_binade = s.slot_binade.data()},
                                  c.slot_limit);
    stage_units(c, s);
    stage_slots(c, s);
    return s;
}

/* Device allocation released on every exit, exceptions included. The copy
 * from `host` is only enqueued: the caller keeps `host` alive until it waits
 * on the queue. */
template <typename T> class DeviceBlock
{
  public:
    DeviceBlock(sycl::queue &queue, const std::vector<T> &host)
        : q_(queue), ptr_(sycl::malloc_device<T>(host.size() + 1u, queue))
    {
        if (ptr_ == nullptr) {
            throw std::bad_alloc();
        }
        if (!host.empty()) {
            q_.memcpy(ptr_, host.data(), host.size() * sizeof(T));
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

/* The walk in a kernel, shaped as the extractor's: one sum on the first lane
 * of a work-group of its own. */
class WalkKernel : public VmafSyclKernelShape<16, 0>
{
  public:
    WalkKernel(const SumWalk &walk, const uint64_t *terms, uint64_t *out)
        : walk_(walk), terms_(terms), out_(out)
    {
    }

    VMAF_SYCL_FUNCTOR_SG_SIZE(16)
    __attribute__((flatten)) void operator()(sycl::nd_item<1> it) const
    {
        if (it.get_local_id(0) != 0u) {
            return;
        }
        const uint64_t *terms = terms_;
        *out_ = vmaf_sycl_ordsum::walk_sum(walk_, [terms](size_t i) { return terms[i]; });
    }

  private:
    SumWalk walk_;
    const uint64_t *terms_;
    uint64_t *out_;
};

uint64_t walk_on_device(sycl::queue &q, const VmafTestOrdsumCase &c, const Staged &s)
{
    const DeviceBlock<int16_t> plan(q, s.plan);
    const DeviceBlock<int64_t> units(q, s.units);
    const DeviceBlock<int16_t> slot_binade(q, s.slot_binade);
    const DeviceBlock<uint64_t> kept(q, s.terms);
    const DeviceBlock<int64_t> run_units(q, s.run_units);
    /* DeviceBlock only enqueues the copy from `host`, so every source lives
     * until the wait below; a temporary here is freed before a deferred
     * (batched command list) copy runs, and the kernel then reads freed
     * heap memory. */
    const std::vector<uint64_t> host_terms(c.terms, c.terms + c.count);
    const std::vector<uint64_t> host_out(1u);
    const DeviceBlock<uint64_t> terms(q, host_terms);
    const DeviceBlock<uint64_t> out(q, host_out);
    q.wait_and_throw();
    const SumWalk walk = {.plan = plan.get(),
                          .units = units.get(),
                          .slot_binade = slot_binade.get(),
                          .terms = kept.get(),
                          .run_units = run_units.get(),
                          .chunks = s.chunks,
                          .count = c.count};
    q.parallel_for(sycl::nd_range<1>(16, 16), WalkKernel(walk, terms.get(), out.get()));
    q.wait_and_throw();
    uint64_t sum = 0u;
    q.memcpy(&sum, out.get(), sizeof(sum));
    q.wait_and_throw();
    return sum;
}

int run_on_device(VmafTestOrdsumCase *c, const Staged &s)
{
    std::optional<sycl::device> device;
    try {
        device.emplace(sycl::gpu_selector_v);
    } catch (const sycl::exception &) {
        return -ENODEV;
    }
    try {
        sycl::queue q(*device);
        c->sum = walk_on_device(q, *c, s);
    } catch (const std::exception &) {
        return -EIO;
    }
    return 0;
}

} // namespace

extern "C" int vmaf_test_sycl_ordsum(VmafTestOrdsumCase *c)
{
    if (c == nullptr || (c->terms == nullptr && c->count != 0u)) {
        return -EINVAL;
    }
    const Staged s = stage(*c);
    c->asked_terms = 0u;
    if (c->on_device != 0) {
        return run_on_device(c, s);
    }
    const SumWalk walk = {.plan = s.plan.data(),
                          .units = s.units.data(),
                          .slot_binade = s.slot_binade.data(),
                          .terms = s.terms.data(),
                          .run_units = s.run_units.data(),
                          .chunks = s.chunks,
                          .count = c->count};
    size_t asked = 0u;
    const uint64_t *terms = c->terms;
    c->sum = vmaf_sycl_ordsum::walk_sum(walk, [terms, &asked](size_t i) {
        asked++;
        return terms[i];
    });
    c->asked_terms = asked;
    return 0;
}
