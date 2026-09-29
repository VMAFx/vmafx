/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/* ADR-1364: a program linked against libvmaf must register libvmaf's SYCL
 * device images. With the Windows MSVC toolchain the link is done by link.exe,
 * which never runs the SYCL image wrapper; before the explicit device link the
 * registry held no kernel and every submit failed with "No kernel named ...
 * was found". The check asks the SYCL runtime for its kernel IDs, which needs
 * no GPU, so it also guards the device-less Windows CI build. */

#include <stdio.h>

#include "config.h"
#include "test.h"

#if HAVE_SYCL

#include "sycl/common.h"

/* One kernel per SYCL feature translation unit is the floor: every
 * core/src/feature/sycl/ TU submits at least one. The build has 21. */
enum { MIN_REGISTERED_KERNELS = 21 };

static char *test_device_images_are_registered(void)
{
    const int count = vmaf_sycl_registered_kernel_count();
    (void)fprintf(stderr, "  registered SYCL kernels: %d\n", count);
    mu_assert("kernel registry query must not fail", count >= 0);
    mu_assert("no SYCL kernel is registered: the device images were not linked", count > 0);
    mu_assert("fewer SYCL kernels registered than there are SYCL feature TUs",
              count >= MIN_REGISTERED_KERNELS);
    return NULL;
}

static char *test_registry_query_is_stable(void)
{
    const int first = vmaf_sycl_registered_kernel_count();
    const int second = vmaf_sycl_registered_kernel_count();
    mu_assert("the kernel registry must not change between queries", first == second);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_device_images_are_registered);
    mu_run_test(test_registry_query_is_stable);
    return NULL;
}

#else

char *run_tests(void)
{
    return NULL;
}

#endif
