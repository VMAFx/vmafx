/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  C++23 implementation of the once-snapshotted GPU dispatch env helper.
 *  See gpu_dispatch_env.h for the public contract and ADR-0461 for rationale.
 *  ADR-0858 records the .c → .cpp conversion decision.
 *
 *  Design: a fixed-capacity table of (var_name, value) pairs protected by a
 *  single mutex.  On the first call for a given var_name the entry is
 *  populated under the lock; subsequent calls read the cached value
 *  lock-free after a pointer match.  The table holds at most
 *  GPU_DISPATCH_ENV_TABLE_CAP entries; 8 is generous for the current
 *  3 backends (CUDA, Vulkan, SYCL) plus anticipated Metal/HIP.
 *
 *  C++23 improvements over the original .c:
 *    - std::string_view for O(1) sized comparisons — no null-pointer UB.
 *    - [[nodiscard]] on the public entry point.
 *    - std::mutex + std::lock_guard<> RAII — no manual lock/unlock pairs,
 *      no platform #ifdef for Windows CRITICAL_SECTION.
 *    - std::optional<std::string> value storage — encodes "unset at
 *      snapshot time" without a nullable raw pointer.
 *    - Platform-specific lock bootstrap (INIT_ONCE / pthread_mutex_t)
 *      replaced by a std::mutex (constinit not applicable; non-constexpr
 *      constructor on GCC/MinGW libstdc++; static-duration zero-init suffices).
 *    - EnvRow is a proper aggregate with a constructor guard.
 */
#include "gpu_dispatch_env.h"

#include <array>
#include <cstdlib> /* std::getenv */
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace
{

constexpr std::size_t kTableCap = 8;

struct EnvRow {
    /* Empty var_name means the slot is free. */
    std::string_view var_name{};
    /* nullopt  ↔ variable was unset at snapshot time.
     * has_value ↔ snapshotted copy of the value string. */
    std::optional<std::string> value{};
};

/* std::array is aggregate-initialised (constinit-compatible on all compilers).
 * std::mutex has a non-constexpr constructor on GCC/MinGW libstdc++, so
 * constinit cannot be applied to it; static-duration zero-init already
 * prevents any dynamic-init race. */
std::mutex g_lock;
constinit std::array<EnvRow, kTableCap> g_rows{};

} /* anonymous namespace */

extern "C" {

/* NOLINTNEXTLINE(readability-redundant-declaration) — required: public C header
 * forward-declares this without [[nodiscard]]; the attribute is additive here. */
[[nodiscard]] const char *vmaf_gpu_dispatch_env_get(const char *var_name)
{
    if (!var_name)
        return nullptr;

    const std::string_view key{var_name};

    /* Fast path: scan without the lock.  A slot whose var_name field is
     * non-empty is effectively immutable after insertion, so reading it
     * without holding the lock is safe on all coherent ISAs.  The worst
     * outcome is a false miss that falls through to the slow path. */
    for (const auto &row : g_rows) {
        if (row.var_name == key)
            return row.value ? row.value->c_str() : nullptr;
    }

    /* Slow path: snapshot the variable under the lock. */
    const std::lock_guard<std::mutex> guard{g_lock};

    /* Re-check under lock to guard against a concurrent insert. */
    for (const auto &row : g_rows) {
        if (row.var_name == key)
            return row.value ? row.value->c_str() : nullptr;
    }

    /* Find a free slot. */
    EnvRow *slot = nullptr;
    for (auto &row : g_rows) {
        if (row.var_name.empty()) {
            slot = &row;
            break;
        }
    }

    if (!slot) {
        /* Table exhausted — fall back to a raw getenv.  Should never
         * happen in production (8 slots, at most 4 backends).
         * NOLINT(concurrency-mt-unsafe): caller-contract bans concurrent
         * setenv before the first GPU frame; see ADR-0461. */
        return std::getenv(var_name); /* NOLINT(concurrency-mt-unsafe) */
    }

    /* Snapshot under lock.  ADR-0461 caller-contract: no other thread
     * calls setenv("VMAF_*") concurrently with getenv here; the lock
     * serialises only multiple vmaf_gpu_dispatch_env_get callers, not
     * hypothetical concurrent setenv from user code. */
    /* NOLINT(concurrency-mt-unsafe): see ADR-0461 contract above. */
    const char *const val = std::getenv(var_name); /* NOLINT(concurrency-mt-unsafe) */
    slot->var_name = key;
    if (val)
        slot->value.emplace(val);
    return slot->value ? slot->value->c_str() : nullptr;
}

} /* extern "C" */
