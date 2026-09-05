/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  C++23 implementation of the once-snapshotted GPU dispatch env helper.
 *  See gpu_dispatch_env.h for the public contract and ADR-0461 for rationale.
 *  ADR-0858 records the .c → .cpp conversion decision.
 *  ADR-1068 records the fast-path data-race fix.
 *
 *  Design: a fixed-capacity table of (var_name, value) pairs protected by a
 *  single mutex.  On the first call for a given var_name the entry is
 *  populated under the lock; subsequent calls read the cached value
 *  lock-free after a ready-flag acquire load.  The table holds at most
 *  kTableCap entries; 8 is generous for the current 3 backends (CUDA, SYCL,
 *  HIP) plus anticipated Metal.
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
 *    - std::atomic<bool> ready flag per slot: the slow-path writer stores
 *      var_name and value under the mutex, then releases the ready flag
 *      (store release); the fast-path reader acquires the ready flag first
 *      and only then reads var_name and value. This satisfies the C++
 *      publication idiom ([atomics.order]) and eliminates the data race that
 *      existed in ADR-0858's first revision, where the fast path read
 *      std::string_view and std::optional<std::string> without any
 *      synchronisation while the slow path wrote them. (ADR-1068.)
 *    - EnvRow is a proper aggregate with a constructor guard.
 */
#include "gpu_dispatch_env.h"

#include <array>
#include <atomic>
#include <cstdlib> /* std::getenv */
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace
{

constexpr std::size_t kTableCap = 8;

struct EnvRow {
    /* ready is the publication flag.  The slow-path writer sets it last with
     * memory_order_release; the fast-path reader checks it first with
     * memory_order_acquire.  All accesses to var_name and value are
     * sequenced after the acquire load of ready (C++ [atomics.order] §3). */
    std::atomic<bool> ready{false};
    /* Non-empty only when ready == true (published under lock). */
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

/* Slow path: snapshot @key into @slot under the caller-held mutex and
 * publish it. Returns the cached value (or nullptr when the variable was
 * unset OR when the value-string copy ran out of memory).
 *
 * The std::string copy inside std::optional::emplace is the only throwing
 * operation; this TU is reached through an extern "C" boundary, so letting a
 * C++ exception propagate across the C ABI is undefined behaviour (ADR-1068
 * follow-up R2-9b). We contain std::bad_alloc and map it to the header's
 * documented "unset" contract: return nullptr and leave @slot UNpopulated
 * (ready stays false) so a later call can retry. This preserves the
 * ADR-0858 / ADR-1068 invariant that var_name/value are published only with
 * the release store of ready set LAST — @slot's fields are touched only
 * after the throwing region completes successfully. */
const char *snapshot_into_slot(EnvRow *slot, std::string_view key, const char *var_name)
{
    /* ADR-0461 caller-contract: no other thread calls setenv("VMAF_*")
     * concurrently with getenv here; the lock serialises only multiple
     * vmaf_gpu_dispatch_env_get callers, not hypothetical concurrent setenv
     * from user code. */
    /* NOLINTNEXTLINE(concurrency-mt-unsafe) — ADR-0461 contract above. */
    const char *const val = std::getenv(var_name);
    std::optional<std::string> snapshot{};
    try {
        if (val)
            snapshot.emplace(val);
    } catch (const std::bad_alloc &) {
        /* Allocation failed while copying the value string. Do not cache a
         * wrong result and do not mark the slot populated; report "unset". */
        return nullptr;
    }
    /* var_name (string_view copy) and value (move of the already-built local)
     * cannot throw, so ready is the last field set — invariant preserved. */
    slot->var_name = key;
    slot->value = std::move(snapshot);
    slot->ready.store(true, std::memory_order_release);
    return slot->value ? slot->value->c_str() : nullptr;
}

} /* anonymous namespace */

extern "C" {

/* NOLINTNEXTLINE(readability-redundant-declaration) — required: public C header
 * forward-declares this without [[nodiscard]]; the attribute is additive here (ADR-0141 / ADR-0278). */
[[nodiscard]] const char *vmaf_gpu_dispatch_env_get(const char *var_name)
{
    if (!var_name)
        return nullptr;

    const std::string_view key{var_name};

    /* Fast path: scan without the mutex.  Each slot's ready flag is loaded
     * with memory_order_acquire, which synchronises-with the corresponding
     * release store in the slow path (C++ publication idiom).  Only after a
     * true acquire load are var_name and value safe to read.  The worst
     * outcome of a false-negative (seeing ready==false for a slot that is
     * concurrently being published) is a harmless fall-through to the slow
     * path, which re-checks under the mutex. */
    for (const auto &row : g_rows) {
        if (!row.ready.load(std::memory_order_acquire))
            continue;
        if (row.var_name == key)
            return row.value ? row.value->c_str() : nullptr;
    }

    /* Slow path: snapshot the variable under the lock. */
    const std::scoped_lock<std::mutex> guard{g_lock};

    /* Re-check under lock to guard against a concurrent insert.  The mutex
     * provides the necessary happens-before so plain (non-atomic) reads of
     * var_name/value are safe here. */
    for (const auto &row : g_rows) {
        if (row.ready.load(std::memory_order_relaxed) && row.var_name == key)
            return row.value ? row.value->c_str() : nullptr;
    }

    /* Find a free slot. */
    EnvRow *slot = nullptr;
    for (auto &row : g_rows) {
        if (!row.ready.load(std::memory_order_relaxed)) {
            slot = &row;
            break;
        }
    }

    if (!slot) {
        /* Table exhausted — fall back to a raw getenv.  Should never
         * happen in production (8 slots, at most 4 backends).
         * NOLINTNEXTLINE(concurrency-mt-unsafe) — ADR-0461 caller-contract. */
        return std::getenv(var_name);
    }

    return snapshot_into_slot(slot, key, var_name);
}

} /* extern "C" */
