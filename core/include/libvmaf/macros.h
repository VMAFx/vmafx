/**
 *
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
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

#ifndef LIBVMAF_MACROS_H
#define LIBVMAF_MACROS_H

/**
 * VMAF_EXPORT — marks a public API symbol as visible in the shared library.
 *
 * libvmaf is compiled with -fvisibility=hidden so that only explicitly
 * annotated symbols appear in the dynamic symbol table of libvmaf.so.3.
 * Every function declared in core/include/libvmaf/ that is part of the
 * public C API must carry this attribute so that consumers can resolve the
 * symbol at link time.
 *
 * See ADR-0379 and Research-0092 for the full symbol-visibility audit and
 * rationale.
 */
#if defined(_MSC_VER)
#define VMAF_EXPORT __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#define VMAF_EXPORT __attribute__((visibility("default")))
#else
#define VMAF_EXPORT
#endif

/**
 * VMAF_DEPRECATED(msg) — marks a libvmaf function that has a VMAFx successor
 * (ADR-1852 decision D7, docs/api/vmafx/compat.md). In this release the
 * warning is opt-in: define VMAF_ENABLE_DEPRECATION_WARNINGS before including
 * a libvmaf header to have the compiler name the vmafx_ replacement of every
 * call. It becomes the default in 1.1, and the functions go in 2.0. The
 * library's own translation units (VMAF_BUILDING_LIBVMAF) never warn.
 */
#if defined(VMAF_ENABLE_DEPRECATION_WARNINGS) && !defined(VMAF_BUILDING_LIBVMAF)
#if defined(_MSC_VER)
#define VMAF_DEPRECATED(msg) __declspec(deprecated(msg))
#elif defined(__GNUC__) || defined(__clang__)
#define VMAF_DEPRECATED(msg) __attribute__((deprecated(msg)))
#else
#define VMAF_DEPRECATED(msg)
#endif
#else
#define VMAF_DEPRECATED(msg)
#endif

#endif /* LIBVMAF_MACROS_H */
