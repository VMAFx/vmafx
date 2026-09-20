/* Copyright 2026 Lusoris
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 * Portable null-pointer token regression coverage.
 */

#include <string.h>

#include "test.h"

#define VMAF_STRINGIZE_INNER(token) #token
#define VMAF_STRINGIZE(token) VMAF_STRINGIZE_INNER(token)

static void callable_fixture(void)
{
}

static char *test_macro_expansion(void)
{
    const char *const spelling = VMAF_STRINGIZE(VMAF_NULLPTR);

#if defined(VMAF_FORCE_NULLPTR_FALLBACK)
    mu_assert("forced fallback must expand to the integer null-pointer constant",
              strcmp(spelling, "0") == 0);
#else
    mu_assert("native selection must expand to a supported null-pointer token",
              strcmp(spelling, "nullptr") == 0 || strcmp(spelling, "0") == 0);
#endif
    return VMAF_NULLPTR;
}

static char *test_object_and_function_pointers(void)
{
    const int *object_pointer = VMAF_NULLPTR;
    void (*function_pointer)(void) = VMAF_NULLPTR;

    mu_assert("VMAF_NULLPTR must initialize an object pointer", object_pointer == VMAF_NULLPTR);
    mu_assert("VMAF_NULLPTR must initialize a function pointer", function_pointer == VMAF_NULLPTR);

    const int object = 1;
    object_pointer = &object;
    function_pointer = callable_fixture;
    mu_assert("object pointer control must be non-null", object_pointer != VMAF_NULLPTR);
    mu_assert("function pointer control must be non-null", function_pointer != VMAF_NULLPTR);

    return VMAF_NULLPTR;
}

char *run_tests(void)
{
    mu_run_test(test_macro_expansion);
    mu_run_test(test_object_and_function_pointers);
    return VMAF_NULLPTR;
}
