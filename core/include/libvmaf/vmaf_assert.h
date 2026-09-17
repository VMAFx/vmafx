/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
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
