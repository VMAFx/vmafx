/* Copyright 2026 Lusoris */
/* SPDX-License-Identifier: BSD-2-Clause-Patent */

/* NOLINTBEGIN(modernize-use-nullptr) -- ADR-1138: retain upstream C NULL
 * compatibility and the required Windows MSVC C build. */
#include <stdlib.h>
#include <string.h>

#include "pdjson.h"
#include "test.h"

static size_t allocation_calls;

static void *observed_realloc(void *pointer, size_t size)
{
    ++allocation_calls;
    return realloc(pointer, size);
}

static char *test_invalid_stack_increment(void)
{
    /* Meson builds this once with increment zero and once with SIZE_MAX. */
    json_stream stream;
    json_allocator allocator = {malloc, observed_realloc, free};
    json_open_string(&stream, "[]");
    json_set_allocator(&stream, &allocator);
    const enum json_type type = json_next(&stream);
    const size_t depth = json_get_depth(&stream);
    const char *error = json_get_error(&stream);
    const bool out_of_memory = error && strcmp(error, "out of memory") == 0;
    json_close(&stream);
    mu_assert("invalid growth rejected", type == JSON_ERROR);
    mu_assert("invalid growth never allocates", allocation_calls == 0);
    mu_assert("failed growth leaves depth unchanged", depth == 0);
    mu_assert("existing allocation error preserved", out_of_memory);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_invalid_stack_increment);
    return NULL;
}
/* NOLINTEND(modernize-use-nullptr) */
