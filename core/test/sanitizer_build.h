/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * VMAF_TEST_SANITIZER_BUILD is 1 in a build with AddressSanitizer,
 * ThreadSanitizer or MemorySanitizer (GCC's __SANITIZE_*__ macros, clang's
 * __has_feature), else 0. The one definition for every test that adapts to an
 * instrumented build: a skip the sanitizer runtime makes meaningless, a
 * latency budget it cannot keep, a sweep it runs many times slower.
 */

#ifndef VMAF_TEST_SANITIZER_BUILD_H
#define VMAF_TEST_SANITIZER_BUILD_H

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__) || defined(__SANITIZE_MEMORY__)
#define VMAF_TEST_SANITIZER_BUILD 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) ||                         \
    __has_feature(memory_sanitizer)
#define VMAF_TEST_SANITIZER_BUILD 1
#endif
#endif
#ifndef VMAF_TEST_SANITIZER_BUILD
#define VMAF_TEST_SANITIZER_BUILD 0
#endif

/*
 * VMAF_TEST_INSTRUMENTED_BUILD is 1 where VMAF_TEST_SANITIZER_BUILD is, and in a
 * build with UndefinedBehaviorSanitizer alone, else 0. UBSan checks every
 * arithmetic operation, so a dense sweep runs many times slower under it too
 * (test_integer_adm_simd: 31 s on the clang UBSan leg against its 30 s budget,
 * T-ADM-SIMD-UBSAN-SWEEP-TIMEOUT-2026-10-09). It maps no shadow memory, so a
 * test that skips for the sanitizer runtime's address space keeps reading
 * VMAF_TEST_SANITIZER_BUILD. clang and GCC report UBSan through
 * __has_feature(undefined_behavior_sanitizer) (checked with clang 23 and GCC
 * 16); a compiler without __has_feature reads as uninstrumented here and runs
 * the dense sweeps.
 */
#if VMAF_TEST_SANITIZER_BUILD
#define VMAF_TEST_INSTRUMENTED_BUILD 1
#elif defined(__has_feature)
#if __has_feature(undefined_behavior_sanitizer)
#define VMAF_TEST_INSTRUMENTED_BUILD 1
#endif
#endif
#ifndef VMAF_TEST_INSTRUMENTED_BUILD
#define VMAF_TEST_INSTRUMENTED_BUILD 0
#endif

#endif /* VMAF_TEST_SANITIZER_BUILD_H */
