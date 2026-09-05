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
 * C++23 implementation of the atomic reference-count helpers (ADR-0735).
 *
 * Migration note (ADR-0735 / Wave 5):
 *   - `git mv ref.c ref.cpp` preserves blame history.
 *   - The public C API in ref.h is unchanged; all five functions retain
 *     their original C signatures and are declared with C linkage so
 *     every C caller (picture_pool.c, framesync.c, etc.) links without
 *     modification.
 *   - `vmaf_ref_init` uses `malloc` + placement-new (value-init) so
 *     `vmaf_ref_close` can use `free()`, matching the legacy C allocator
 *     contract and the test harness in ref.c (adversarial review PR #78 fixup).
 *   - `vmaf_ref_close` uses `free()` — it is THE ONLY valid deallocator;
 *     C callers must never call free() on a VmafRef* directly (see ref.h).
 *   - The C11 `atomic_*` macros in ref.h remain because the header must
 *     be includable from both C and C++ translation units (the MSVC bridge
 *     already handles this — see the comment in ref.h).
 */

#include <cerrno>
#include <cstdlib>
#include <new>

#include "ref.h"

int vmaf_ref_init(VmafRef **ref)
{
    if (!ref)
        return -EINVAL;

    /* Use malloc + value-init so vmaf_ref_close can use free().
     * This keeps the allocator pair consistent with the C test harness
     * (ref.c) and eliminates the make_unique/delete vs free() mismatch
     * identified as a CRITICAL finding in adversarial review PR #78.
     * VmafRef is trivially destructible (atomic_int has no destructor
     * side effects), so placement-new + free() is well-defined. */
    // NOLINTNEXTLINE(cppcoreguidelines-no-malloc) — ADR-0141: C ABI; freed in vmaf_ref_close
    auto *r = static_cast<VmafRef *>(malloc(sizeof(VmafRef)));
    if (!r)
        return -ENOMEM;
    new (r) VmafRef{}; /* value-init: zero-initialises atomic_int cnt */

    atomic_init(&r->cnt, 1);
    *ref = r;
    return 0;
}

void vmaf_ref_fetch_increment(VmafRef *ref)
{
    atomic_fetch_add(&ref->cnt, 1);
}

long vmaf_ref_fetch_decrement(VmafRef *ref)
{
    /* acq_rel ordering for the last-decrementer pattern: the release side
     * ensures the decrementing thread's prior stores are visible to other
     * threads, and the acquire side ensures the last decrementer (the one
     * that sees old_cnt == 1) loads the most recent writes before it runs
     * the destructor (e.g. picture.c:230-233 dereferencing priv).
     * Using the default seq_cst form is safe but unnecessarily strong; this
     * matches the standard reference-counting idiom in C11 §7.17 / C++20
     * [atomics.ref.ops]. */
    return atomic_fetch_sub_explicit(&ref->cnt, 1, memory_order_acq_rel);
}

long vmaf_ref_load(VmafRef *ref)
{
    return atomic_load(&ref->cnt);
}

int vmaf_ref_close(VmafRef *ref)
{
    /* free() pairs with the malloc() in vmaf_ref_init (see allocation comment
     * there). VmafRef is trivially destructible; no explicit destructor call
     * is needed. C callers MUST use vmaf_ref_close() exclusively — never
     * call free(VmafRef*) directly (see ref.h). */
    free(ref); // NOLINT(cppcoreguidelines-no-malloc) — ADR-0141: pairs with vmaf_ref_init
    return 0;
}
