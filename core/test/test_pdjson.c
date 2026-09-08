/* Copyright 2026 Lusoris */
/* SPDX-License-Identifier: BSD-2-Clause-Patent */

/* NOLINTBEGIN(modernize-use-nullptr) -- ADR-1138: retain C/upstream NULL
 * compatibility and the required Windows MSVC C build. */
#include <stdlib.h>
#include <string.h>

#include "pdjson.h"
#include "test.h"

static char *test_event_sequence(void)
{
    const enum json_type expected[] = {
        JSON_OBJECT, JSON_STRING,    JSON_ARRAY,  JSON_TRUE,   JSON_FALSE,      JSON_NULL,
        JSON_NUMBER, JSON_ARRAY_END, JSON_STRING, JSON_STRING, JSON_OBJECT_END, JSON_DONE,
    };
    json_stream stream;
    json_open_string(&stream, "{\"a\":[true,false,null,-1.25e2],\"b\":\"x\"}");
    json_set_streaming(&stream, false);
    for (size_t i = 0; i < sizeof(expected) / sizeof(*expected); ++i) {
        const enum json_type type = json_next(&stream);
        mu_assert("event sequence", type == expected[i]);
        if (type == JSON_NUMBER)
            mu_assert("numeric value", json_get_number(&stream) == -125.0);
    }
    mu_assert("valid document has no error", json_get_error(&stream) == NULL);
    json_close(&stream);
    return NULL;
}

static char *test_context_and_skip(void)
{
    json_stream stream;
    size_t count = 99;
    json_open_string(&stream, "{\"a\":[{},1],\"b\":2}");
    mu_assert("initial context", json_get_context(&stream, &count) == JSON_DONE);
    mu_assert("open object", json_next(&stream) == JSON_OBJECT);
    mu_assert("depth and context", json_get_depth(&stream) == 1 &&
                                       json_get_context(&stream, &count) == JSON_OBJECT &&
                                       count == 0);
    mu_assert("first key", json_next(&stream) == JSON_STRING);
    mu_assert("key event count", json_get_context(&stream, &count) == JSON_OBJECT && count == 1);
    mu_assert("skip nested array", json_skip(&stream) == JSON_ARRAY);
    mu_assert("skip until number", json_skip_until(&stream, JSON_NUMBER) == JSON_NUMBER);
    json_close(&stream);
    return NULL;
}

static char *test_streaming_peek_reset(void)
{
    json_stream stream;
    json_open_string(&stream, "[] \n true");
    mu_assert("open array", json_next(&stream) == JSON_ARRAY);
    mu_assert("peek is stable", json_peek(&stream) == JSON_ARRAY_END &&
                                    json_peek(&stream) == JSON_ARRAY_END &&
                                    json_next(&stream) == JSON_ARRAY_END);
    mu_assert("streaming completion leaves whitespace", json_next(&stream) == JSON_DONE &&
                                                            json_get_position(&stream) == 2 &&
                                                            json_source_peek(&stream) == ' ');
    const int space = json_source_get(&stream);
    const int newline = json_source_get(&stream);
    mu_assert("source separator", space == ' ' && newline == '\n' && json_get_lineno(&stream) == 2);
    json_reset(&stream);
    const enum json_type value = json_next(&stream);
    const enum json_type done = json_next(&stream);
    mu_assert("next document", value == JSON_TRUE && done == JSON_DONE);
    json_reset(&stream);
    mu_assert("stream exhausted", json_next(&stream) == JSON_DONE);
    json_close(&stream);
    return NULL;
}

static char *test_unicode_and_embedded_nul(void)
{
    const char expected[] = {'a', '\0', '\xc2', '\xa2', '\xf0', '\x9d', '\x84', '\x9e', '\0'};
    const char raw[] = {'"', '\xc2', '\xa2', '"'};
    json_stream stream;
    size_t length = 0;
    json_open_string(&stream, "\"a\\u0000\\u00a2\\ud834\\udd1e\"");
    mu_assert("escaped Unicode string", json_next(&stream) == JSON_STRING);
    const char *value = json_get_string(&stream, &length);
    mu_assert("decoded bytes include embedded and terminating NUL",
              length == sizeof(expected) && memcmp(value, expected, sizeof(expected)) == 0);
    json_close(&stream);
    json_open_buffer(&stream, raw, sizeof(raw));
    mu_assert("raw UTF-8 string", json_next(&stream) == JSON_STRING);
    value = json_get_string(&stream, &length);
    mu_assert("raw UTF-8 preserved", length == 3 && memcmp(value, raw + 1, 2) == 0);
    json_close(&stream);
    return NULL;
}

static char *test_invalid_documents(void)
{
    const char *const invalid[] = {
        "{",
        "[1,]",
        "{\"x\" 1}",
        "{\"x\":1,}",
        "{1:2}",
        "[1 2]",
        "[}",
        "tru",
        "nulx",
        "-",
        "1.",
        "1e",
        "1e+",
        "01",
        "true false",
        "\"\\x\"",
        "\"\\ud800\"",
        "\"\\ud800\\u0000\"",
        "\"\\udc00\"",
        "\"\\u0x00\"",
        "\"\xc0\x80\"",
        "\"\xed\xa0\x80\"",
        "\"\xf4\x90\x80\x80\"",
        "\"\xe2\x82",
        "\"\n\"",
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i) {
        json_stream stream;
        json_open_string(&stream, invalid[i]);
        json_set_streaming(&stream, false);
        enum json_type type = JSON_DONE;
        for (size_t token = 0; token < 32; ++token) {
            type = json_next(&stream);
            if (type == JSON_DONE || type == JSON_ERROR)
                break;
        }
        mu_assert("malformed document rejected", type == JSON_ERROR);
        const char *error = json_get_error(&stream);
        mu_assert("error is sticky", error != NULL && error[0] != '\0' &&
                                         json_next(&stream) == JSON_ERROR &&
                                         json_get_error(&stream) == error);
        json_close(&stream);
    }
    return NULL;
}

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

static char *test_user_source(void)
{
    struct input_cursor cursor = {"[false]", 0, 0};
    json_stream stream;
    json_open_user(&stream, cursor_get, cursor_peek, &cursor);
    mu_assert("user source array", json_next(&stream) == JSON_ARRAY);
    mu_assert("user source value", json_next(&stream) == JSON_FALSE);
    const enum json_type end = json_next(&stream);
    const enum json_type done = json_next(&stream);
    mu_assert("user source completion", end == JSON_ARRAY_END && done == JSON_DONE &&
                                            cursor.position == 7 && cursor.peek_calls >= 7);
    json_close(&stream);
    return NULL;
}

static char *test_file_source(void)
{
    FILE *file = tmpfile();
    mu_assert("temporary input", file != NULL);
    mu_assert("write temporary input", fputs("[true]", file) >= 0 && fseek(file, 0, SEEK_SET) == 0);
    json_stream stream;
    json_open_stream(&stream, file);
    mu_assert("file source array", json_next(&stream) == JSON_ARRAY);
    mu_assert("file source peek retains input", json_source_peek(&stream) == 't' &&
                                                    json_get_position(&stream) == 1 &&
                                                    json_next(&stream) == JSON_TRUE);
    const enum json_type end = json_next(&stream);
    const enum json_type done = json_next(&stream);
    mu_assert("file source completion", end == JSON_ARRAY_END && done == JSON_DONE);
    json_close(&stream);
    mu_assert("close temporary input", fclose(file) == 0);
    return NULL;
}

static size_t allocation_calls;
static size_t fail_at;

static void *controlled_malloc(size_t size)
{
    return ++allocation_calls == fail_at ? NULL : malloc(size);
}

static void *controlled_realloc(void *pointer, size_t size)
{
    return ++allocation_calls == fail_at ? NULL : realloc(pointer, size);
}

static char *test_allocation_failures(void)
{
    char document[2105];
    memset(document, 'a', sizeof(document));
    document[0] = '[';
    document[1] = '"';
    document[2102] = '"';
    document[2103] = ']';
    document[2104] = '\0';
    json_allocator allocator = {controlled_malloc, controlled_realloc, free};
    for (size_t failure = 1; failure <= 5; ++failure) {
        allocation_calls = 0;
        fail_at = failure;
        json_stream stream;
        json_open_string(&stream, document);
        json_set_allocator(&stream, &allocator);
        enum json_type type = JSON_DONE;
        for (size_t token = 0; token < 5; ++token) {
            type = json_next(&stream);
            if (type == JSON_ERROR || type == JSON_DONE)
                break;
        }
        mu_assert("each allocator failure is reported",
                  failure == 5 ? type == JSON_DONE :
                                 type == JSON_ERROR &&
                                     strstr(json_get_error(&stream), "out of memory") != NULL);
        json_close(&stream);
    }
    return NULL;
}

static char *test_nesting_boundary(void)
{
    for (size_t depth = 511; depth <= 513; ++depth) {
        char document[1027];
        memset(document, '[', depth);
        memset(document + depth, ']', depth);
        document[depth * 2] = '\0';
        json_stream stream;
        json_open_string(&stream, document);
        enum json_type type = JSON_DONE;
        for (size_t token = 0; token <= depth * 2; ++token) {
            type = json_next(&stream);
            if (type == JSON_ERROR || type == JSON_DONE)
                break;
        }
        const size_t accepted_depth = json_get_depth(&stream);
        const char *error = json_get_error(&stream);
        const bool nesting_error = error && strcmp(error, "maximum depth of nesting reached") == 0;
        json_close(&stream);
        if (depth <= 512) {
            mu_assert("documented nesting limit accepts 511 and 512", type == JSON_DONE);
        } else {
            mu_assert("documented nesting limit rejects 513", type == JSON_ERROR);
            mu_assert("rejected nesting keeps accepted depth", accepted_depth == 512);
            mu_assert("nesting diagnostic", nesting_error);
        }
    }
    return NULL;
}

static char *run_input_tests(void)
{
    mu_run_test(test_unicode_and_embedded_nul);
    mu_run_test(test_invalid_documents);
    mu_run_test(test_user_source);
    mu_run_test(test_file_source);
    mu_run_test(test_allocation_failures);
    mu_run_test(test_nesting_boundary);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_event_sequence);
    mu_run_test(test_context_and_skip);
    mu_run_test(test_streaming_peek_reset);
    return run_input_tests();
}
/* NOLINTEND(modernize-use-nullptr) */
