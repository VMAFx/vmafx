/**
 *
 * Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/* Force-included (-include / /FI) into every translation unit of the
 * test_framesync_init_failure target so framesync.c's calls to the four
 * pthread entry points land on the deterministic wrappers in
 * test_framesync_init_failure.c.
 *
 * Why a force-included header rather than four -D flags on the command line:
 * a command-line -D is in force before <pthread.h> is read, so the rename
 * also lands on whatever <pthread.h> *declares*. Where the entry points are
 * declared extern -- glibc, macOS libSystem, MinGW's winpthreads -- that is
 * harmless. Where <pthread.h> instead *defines* them inline it is not:
 * core/src/compat/win32/pthread.h is a header-only SRWLOCK /
 * CONDITION_VARIABLE shim whose four entry points are `static inline`
 * (core/meson.build wires it in whenever cc.check_header('pthread.h') fails,
 * i.e. on every MSVC and clang-cl build). Renaming a definition just gives
 * the real implementation the wrapper's name inside framesync.c's own
 * translation unit, where it out-scopes the wrapper: framesync.c then calls
 * the genuine primitives, no injected failure ever occurs, and
 * test_init_failure_unwinds_initialized_primitives fails. That is why the
 * target passed on Linux, macOS and MinGW but failed on Windows ARM64 MSVC --
 * the only MSVC lane that runs `meson test` rather than a named subset.
 *
 * Ordering the two halves inside one header removes the precondition instead
 * of steering around it: <pthread.h> is read first (via
 * test_framesync_init_failure.h), so its declarations and any inline
 * definitions keep their own names, and the renames below then apply to
 * nothing but the call sites compiled afterwards. test_framesync_init_failure.c
 * undefines them again before its own first include, so the wrappers still
 * see the real entry points to delegate to.
 */

#ifndef VMAFX_TEST_FRAMESYNC_INTERPOSE_H_
#define VMAFX_TEST_FRAMESYNC_INTERPOSE_H_

#include "test_framesync_init_failure.h"

#define pthread_mutex_init vmafx_test_pthread_mutex_init
#define pthread_cond_init vmafx_test_pthread_cond_init
#define pthread_mutex_destroy vmafx_test_pthread_mutex_destroy
#define pthread_cond_destroy vmafx_test_pthread_cond_destroy

#endif /* VMAFX_TEST_FRAMESYNC_INTERPOSE_H_ */
