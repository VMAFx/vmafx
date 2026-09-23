/* Copyright 2026 Lusoris */
/* SPDX-License-Identifier: EUPL-1.2 */

/* NOLINTBEGIN(modernize-use-nullptr) -- ADR-1138: retain upstream C NULL
 * compatibility and the required Windows MSVC C build. */
#include <stdlib.h>
#include <string.h>

#include "pdjson.h"
#include "test.h"

struct json_stack {
    enum json_type type;
    long count;
};

struct input_cursor {
    const char *text;
    size_t position;
    size_t peek_calls;
};

static int cursor_peek(void *opaque)
{
    struct input_cursor *cursor = opaque;
    ++cursor->peek_calls;
    const unsigned char byte = (unsigned char)cursor->text[cursor->position];
    return byte ? byte : EOF;
}

static int cursor_get(void *opaque)
{
    struct input_cursor *cursor = opaque;
    const int byte = cursor_peek(cursor);
    if (byte != EOF)
        ++cursor->position;
    return byte;
}

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

static char *test_scalar_string(void)
{
    json_stream stream;
    json_open_string(&stream, "\"val\\u0041\\n\"");
    mu_assert("open string scalar", json_next(&stream) == JSON_STRING);
    size_t length = 0;
    const char *str = json_get_string(&stream, &length);
    mu_assert("string content decoded", length == 6 && strcmp(str, "valA\n") == 0);
    mu_assert("string scalar done", json_next(&stream) == JSON_DONE);
    json_close(&stream);
    return NULL;
}

static char *test_scalar_number(void)
{
    json_stream stream;
    const char num_raw[] = "-1.25e2";
    json_open_buffer(&stream, num_raw, sizeof(num_raw) - 1);
    mu_assert("open number scalar", json_next(&stream) == JSON_NUMBER);
    mu_assert("number value decoded", json_get_number(&stream) == -125.0);
    mu_assert("number scalar done", json_next(&stream) == JSON_DONE);
    json_close(&stream);
    return NULL;
}

static char *test_scalar_streaming(void)
{
    json_stream stream;
    json_open_string(&stream, "true false null");
    json_set_streaming(&stream, true);
    mu_assert("boolean true scalar", json_next(&stream) == JSON_TRUE);
    mu_assert("true scalar done", json_next(&stream) == JSON_DONE);
    json_reset(&stream);
    mu_assert("boolean false scalar", json_next(&stream) == JSON_FALSE);
    mu_assert("false scalar done", json_next(&stream) == JSON_DONE);
    json_reset(&stream);
    mu_assert("null scalar", json_next(&stream) == JSON_NULL);
    mu_assert("null scalar done", json_next(&stream) == JSON_DONE);
    json_reset(&stream);
    mu_assert("streaming exhausted", json_next(&stream) == JSON_DONE);
    json_close(&stream);
    return NULL;
}

static char *test_user_source(void)
{
    struct input_cursor cursor = {"true", 0, 0};
    json_stream stream;
    json_open_user(&stream, cursor_get, cursor_peek, &cursor);
    mu_assert("user source value", json_next(&stream) == JSON_TRUE);
    mu_assert("user source done", json_next(&stream) == JSON_DONE);
    json_close(&stream);
    return NULL;
}

static char *test_file_source(void)
{
    FILE *file = tmpfile();
    mu_assert("temporary input created", file != NULL);
    mu_assert("temporary input written", fputs("42", file) >= 0 && fseek(file, 0, SEEK_SET) == 0);
    json_stream stream;
    json_open_stream(&stream, file);
    mu_assert("stream source value", json_next(&stream) == JSON_NUMBER);
    mu_assert("stream source done", json_next(&stream) == JSON_DONE);
    json_close(&stream);
    mu_assert("temporary input closed", fclose(file) == 0);
    return NULL;
}

static char *test_preallocated_container_success(void)
{
    json_stream stream;
    json_open_string(&stream, "[{\"a\":1}]");
    stream.stack = malloc(2 * sizeof(struct json_stack));
    mu_assert("pre-allocated stack memory", stream.stack != NULL);
    stream.stack_size = 2;

    const enum json_type t1 = json_next(&stream);
    const enum json_type t2 = json_next(&stream);
    const enum json_type t3 = json_next(&stream);
    const enum json_type t4 = json_next(&stream);
    const enum json_type t5 = json_next(&stream);
    const enum json_type t6 = json_next(&stream);
    const enum json_type t7 = json_next(&stream);
    mu_assert("container open sequence", t1 == JSON_ARRAY && t2 == JSON_OBJECT);
    mu_assert("container entries sequence", t3 == JSON_STRING && t4 == JSON_NUMBER);
    mu_assert("container close sequence", t5 == JSON_OBJECT_END && t6 == JSON_ARRAY_END);
    mu_assert("document completed", t7 == JSON_DONE);
    json_close(&stream);
    return NULL;
}

static char *test_preallocated_container_overflow(void)
{
    json_stream stream;
    json_open_string(&stream, "[[]]");
    stream.stack = malloc(1 * sizeof(struct json_stack));
    mu_assert("pre-allocated shallow stack", stream.stack != NULL);
    stream.stack_size = 1;
    mu_assert("first level within capacity", json_next(&stream) == JSON_ARRAY);
    mu_assert("exceeding capacity triggers growth error", json_next(&stream) == JSON_ERROR);
    const char *error = json_get_error(&stream);
    mu_assert("growth failure reports out of memory", error && strcmp(error, "out of memory") == 0);
    json_close(&stream);
    return NULL;
}

static char *run_container_tests(void)
{
    mu_run_test(test_invalid_stack_increment);
    mu_run_test(test_preallocated_container_success);
    mu_run_test(test_preallocated_container_overflow);
    return NULL;
}

static char *run_scalar_tests(void)
{
    mu_run_test(test_scalar_string);
    mu_run_test(test_scalar_number);
    mu_run_test(test_scalar_streaming);
    mu_run_test(test_user_source);
    mu_run_test(test_file_source);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(run_container_tests);
    mu_run_test(run_scalar_tests);
    return NULL;
}
/* NOLINTEND(modernize-use-nullptr) */
