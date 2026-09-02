/**
 *
 *  Copyright 2016-2026 Netflix, Inc.
 *
 *     Licensed under the BSD+Patent License (the "License");
 *     you may not use this file except in compliance with the License.
 *     You may obtain a copy of the License at
 *
 *         https://opensource.org/licenses/BSDplusPatent
 *
 *     Unless required by applicable law or agreed to in writing, software
 *     distributed under the License is distributed on an "AS IS" BASIS,
 *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *     See the License for the specific language governing permissions and
 *     limitations under the License.
 *
 */

/*
 * C++23 implementation of the CPU feature-flag dispatcher (ADR-0735).
 *
 * Migration note (ADR-0735 / Wave 5):
 *   - `git mv cpu.c cpu.cpp` preserves blame history.
 *   - The public C API in cpu.h is unchanged; all three functions retain
 *     their original C signatures and are declared `extern "C"` in the
 *     header, so every C caller links without modification.
 *   - `static unsigned flags` and `flags_mask` are converted to
 *     `std::atomic<unsigned>` to eliminate data-races when
 *     `vmaf_init_cpu` and `vmaf_get_cpu_flags` are called from
 *     different threads during initialisation.  The relaxed-load on the
 *     hot path preserves the near-zero overhead of the original plain read.
 *   - `flags_mask` default is `std::numeric_limits<unsigned>::max()` which
 *     is identical to the former `-1` cast but carries its intent explicitly.
 */

#include <atomic>
#include <limits>

#include "config.h"
#include "cpu.h"

namespace
{

/* ADR-0735: use std::atomic for thread-safe flag initialisation.
 * memory_order_relaxed is sufficient here because:
 *   - vmaf_init_cpu() is expected to be called once before any scoring
 *     threads start (sequential-before relationship established by the
 *     caller's thread-sync primitive, typically pthread_once or
 *     std::call_once at libvmaf.c init time).
 *   - vmaf_get_cpu_flags() is a pure read on the hot path; relaxed load
 *     avoids unnecessary fence emission on ARM/POWER while still being
 *     defined behaviour for concurrent reads after a sequenced store. */
/* constinit (C++20/23) guarantees compile-time initialisation of these atomics,
 * preventing the static-initialisation-order fiasco if another TU's static
 * initializer calls vmaf_get_cpu_flags() before this TU is constructed
 * (adversarial review 2026-05-28 finding #19). */
constinit std::atomic<unsigned> g_flags{0u};
constinit std::atomic<unsigned> g_flags_mask{std::numeric_limits<unsigned>::max()};

} // namespace

void vmaf_init_cpu(void)
{
#if ARCH_X86
    g_flags.store(vmaf_get_cpu_flags_x86(), std::memory_order_relaxed);
#if HAVE_AVX512
    if (g_flags.load(std::memory_order_relaxed) & VMAF_X86_CPU_FLAG_AVX512) {
        /* Warm up AVX-512 execution units. On Intel CPUs, the 512-bit
         * units power down after idle and take 10-20µs to reactivate.
         * Issuing a dummy instruction here avoids that latency penalty
         * on the first frame of actual computation.
         * GCC/clang inline asm only — MSVC dropped inline asm on x64.
         * On MSVC the warmup is skipped (micro-opt, not correctness). */
#if defined(__GNUC__) || defined(__clang__)
        __asm__ volatile("vpxord %%zmm0, %%zmm0, %%zmm0" ::: "zmm0");
#endif
    }
#endif
#elif ARCH_AARCH64
    g_flags.store(vmaf_get_cpu_flags_arm(), std::memory_order_relaxed);
#endif
}

void vmaf_set_cpu_flags_mask(const unsigned mask)
{
    g_flags_mask.store(mask, std::memory_order_relaxed);
}

unsigned vmaf_get_cpu_flags(void)
{
    return g_flags.load(std::memory_order_relaxed) & g_flags_mask.load(std::memory_order_relaxed);
}
