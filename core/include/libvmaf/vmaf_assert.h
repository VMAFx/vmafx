/**
 *
 *  Copyright 2026 Lusoris
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

#ifndef LIBVMAF_VMAF_ASSERT_H
#define LIBVMAF_VMAF_ASSERT_H

/*
 * INTERNAL HEADER — not installed; not part of the public libvmaf API.
 *
 * This header is used exclusively by libvmaf's own translation units.
 * External consumers must not include or depend on it; its contents
 * (particularly the VMAF_DEBUG flag) are build-system internals.
 */

/*
 * VMAF_ASSERT_DEBUG(expr)
 *
 * Invariant assertion that is cheap to enable in development but zero-cost in
 * release. Use inside frame-loop / per-pixel hot paths where a standard
 * assert() would perturb benchmarks or inflate code size.
 *
 * Semantics:
 *   - With VMAF_DEBUG defined (debug builds, ASan/UBSan builds, test builds):
 *     behaves identically to assert().
 *   - Otherwise: compiles to a statement that the compiler can fully elide,
 *     while still tokenizing 'expr' so typos remain compile errors.
 *
 * Rationale: Power of 10 rule #5 requires ≥ 2 assertions per function on
 * average. In hot kernels we want those assertions during development without
 * paying for them in release binaries.
 */

#ifdef VMAF_DEBUG
#include <assert.h>
#define VMAF_ASSERT_DEBUG(expr) assert(expr)
#else
#define VMAF_ASSERT_DEBUG(expr) ((void)sizeof(expr))
#endif

#endif /* LIBVMAF_VMAF_ASSERT_H */
