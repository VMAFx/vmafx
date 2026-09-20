/* Copyright 2026 Lusoris
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 * Portable null-pointer token for C translation units.
 */

#ifndef VMAF_SRC_VMAF_NULLPTR_H_
#define VMAF_SRC_VMAF_NULLPTR_H_

/* GCC 13 and Clang 16 implemented C nullptr while the C2x draft still used
 * 202000L. Final C23 frontends advertise 202311L. MSVC C intentionally takes
 * the fallback until its required lane proves native nullptr support. */
#if defined(VMAF_FORCE_NULLPTR_FALLBACK)
#define VMAF_NULLPTR 0
#elif defined(__cplusplus)
#define VMAF_NULLPTR nullptr
#elif defined(_MSC_VER)
#define VMAF_NULLPTR 0
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 202311L
#define VMAF_NULLPTR nullptr
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ > 201710L && defined(__clang__) &&             \
    __clang_major__ >= 16
#define VMAF_NULLPTR nullptr
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ > 201710L && defined(__GNUC__) &&              \
    !defined(__clang__) && __GNUC__ >= 13
#define VMAF_NULLPTR nullptr
#else
#define VMAF_NULLPTR 0
#endif

#endif /* VMAF_SRC_VMAF_NULLPTR_H_ */
