/* Copyright 2026 Lusoris */
/* SPDX-License-Identifier: EUPL-1.2 */

/* Behaviour pin for the vendored cJSON (core/src/mcp/3rdparty/cJSON/).
 *
 * The fork carries a delta against upstream cJSON 1.7.19: bounded snprintf /
 * memcpy in place of the banned sprintf / strcpy, an INT_MAX saturation in
 * cJSON_GetArraySize, and the goto-based and over-long functions split into
 * helpers (ADR-0683, ADR-1061, ADR-1142). Every expected value below was
 * taken from an unmodified 1.7.19 build, so this test passes against upstream
 * and must keep passing against the fork: it is what a re-vendor re-applies
 * the delta against. It deliberately does not depend on enable_mcp, so the
 * CPU build always compiles cJSON.c and the clang-tidy ratchet and cppcheck
 * see it.
 *
 * The check that the banned calls stay out is source-level and lives in
 * scripts/ci/tests/test_semgrep_vendored_scope.py. */

/* NOLINTBEGIN(modernize-use-nullptr) -- ADR-1138: retain C/upstream NULL
 * compatibility and the required Windows MSVC C build. */
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "test.h"

enum {
    CANARY_BYTES = 8,
    CANARY = 0x5A,
    PRINT_SLACK = 16,
    FAILURE_POINTS = 96,
    LONG_VALUE_LENGTH = 400
};

/* Prints an item, compares it with the expected text and releases the item. */
static bool prints_as(cJSON *item, const char *expected)
{
    char *printed = cJSON_PrintUnformatted(item);
    const bool equal = (printed != NULL) && (strcmp(printed, expected) == 0);
    cJSON_free(printed);
    cJSON_Delete(item);
    return equal;
}

/* Parsing must fail and report the failure at the given offset. */
static bool fails_at(const char *input, long offset)
{
    const char *end = NULL;
    cJSON *parsed = cJSON_ParseWithOpts(input, &end, 0);
    const bool failed = (parsed == NULL) && (end != NULL) && ((end - input) == offset);
    cJSON_Delete(parsed);
    return failed;
}

static char *test_version(void)
{
    /* A re-vendor changes this string: re-apply the fork delta first, see
     * core/src/mcp/3rdparty/cJSON/AGENTS.md. */
    mu_assert("vendored cJSON is 1.7.19", strcmp(cJSON_Version(), "1.7.19") == 0);
    return NULL;
}

static char *test_print_literals(void)
{
    mu_assert("null literal", prints_as(cJSON_CreateNull(), "null"));
    mu_assert("false literal", prints_as(cJSON_CreateFalse(), "false"));
    mu_assert("true literal", prints_as(cJSON_CreateTrue(), "true"));
    mu_assert("raw text is copied verbatim", prints_as(cJSON_CreateRaw("[1, 2]"), "[1, 2]"));
    mu_assert("a missing item prints nothing", cJSON_PrintUnformatted(NULL) == NULL);
    return NULL;
}

static char *test_print_numbers(void)
{
    mu_assert("zero", prints_as(cJSON_CreateNumber(0.0), "0"));
    mu_assert("negative integer", prints_as(cJSON_CreateNumber(-125.0), "-125"));
    mu_assert("fraction", prints_as(cJSON_CreateNumber(1.5), "1.5"));
    mu_assert("exponent", prints_as(cJSON_CreateNumber(1e300), "1e+300"));
    mu_assert("beyond int", prints_as(cJSON_CreateNumber(2147483648.0), "2147483648"));
    return NULL;
}

static char *test_print_number_precision(void)
{
    mu_assert("15 digits are enough", prints_as(cJSON_CreateNumber(0.1 + 0.2), "0.3"));
    mu_assert("17 digits when 15 do not round-trip",
              prints_as(cJSON_CreateNumber(1.0 / 3.0), "0.33333333333333331"));
    mu_assert("smallest denormal", prints_as(cJSON_CreateNumber(5e-324), "4.94065645841247e-324"));
    mu_assert("NaN prints null", prints_as(cJSON_CreateNumber((double)NAN), "null"));
    mu_assert("infinity prints null", prints_as(cJSON_CreateNumber((double)INFINITY), "null"));
    return NULL;
}

/* `valueint` is defined for every double. Upstream cast NaN to int, which is
 * undefined behaviour; the fork maps it to 0 (see saturate_to_int in cJSON.c).
 * The sanitizer lane is what turns a regression here into a failure. */
static int valueint_of(double number)
{
    cJSON *item = cJSON_CreateNumber(number);
    if (item == NULL) {
        return -1;
    }
    const int value = item->valueint;
    cJSON_Delete(item);
    return value;
}

static char *test_number_valueint_non_finite(void)
{
    mu_assert("NaN maps to 0", valueint_of((double)NAN) == 0);
    mu_assert("negative NaN maps to 0", valueint_of(-(double)NAN) == 0);
    mu_assert("+infinity saturates", valueint_of((double)INFINITY) == INT_MAX);
    mu_assert("-infinity saturates", valueint_of(-(double)INFINITY) == INT_MIN);
    return NULL;
}

static char *test_number_valueint_range(void)
{
    mu_assert("above INT_MAX saturates", valueint_of(2147483648.0) == INT_MAX);
    mu_assert("below INT_MIN saturates", valueint_of(-2147483649.0) == INT_MIN);
    mu_assert("INT_MAX itself", valueint_of(2147483647.0) == INT_MAX);
    mu_assert("INT_MIN itself", valueint_of(-2147483648.0) == INT_MIN);
    mu_assert("truncates toward zero", valueint_of(-1.9) == -1);
    return NULL;
}

static char *test_number_setter_maps_nan_to_zero(void)
{
    cJSON *item = cJSON_CreateNumber(1.0);
    mu_assert("number item", item != NULL);
    (void)cJSON_SetNumberHelper(item, (double)NAN);
    const int after_set = item->valueint;
    cJSON_Delete(item);
    mu_assert("the setter maps NaN to 0 too", after_set == 0);
    return NULL;
}

static char *test_print_strings(void)
{
    cJSON *without_value = cJSON_CreateString("x");
    mu_assert("string item", without_value != NULL);
    cJSON_free(without_value->valuestring);
    without_value->valuestring = NULL;

    mu_assert("a missing value prints an empty string", prints_as(without_value, "\"\""));
    mu_assert("empty string", prints_as(cJSON_CreateString(""), "\"\""));
    mu_assert("nothing to escape", prints_as(cJSON_CreateString("plain"), "\"plain\""));
    mu_assert("one control character", prints_as(cJSON_CreateString("\x01"), "\"\\u0001\""));
    mu_assert("every escape form, UTF-8 passes through",
              prints_as(cJSON_CreateString("q\" b\\ \b\f\n\r\t \x01\x1f \xc3\xa9"),
                        "\"q\\\" b\\\\ \\b\\f\\n\\r\\t \\u0001\\u001f \xc3\xa9\""));
    return NULL;
}

/* Prints into a buffer of `size` bytes that is followed by canary bytes. */
static char *check_preallocated(cJSON *root, const char *expected, size_t size)
{
    const size_t length = strlen(expected);
    unsigned char *buffer = malloc(size + CANARY_BYTES);
    mu_assert("buffer", buffer != NULL);
    memset(buffer, CANARY, size + CANARY_BYTES);

    const cJSON_bool printed = cJSON_PrintPreallocated(root, (char *)buffer, (int)size, 0);
    bool intact = true;
    for (size_t i = 0; i < (size_t)CANARY_BYTES; i++) {
        intact = intact && (buffer[size + i] == CANARY);
    }
    const bool exact = !printed || (strcmp((const char *)buffer, expected) == 0);
    free(buffer);

    mu_assert("never writes past the buffer it was given", intact);
    mu_assert("a successful print is the complete text", exact);
    mu_assert("a buffer without room for the terminator is refused", !printed || size > length);
    mu_assert("the documented five spare bytes always suffice", printed || size < length + 5);
    return NULL;
}

static char *test_print_preallocated(void)
{
    const char *expected = "{\"k\":\"\\u0001v\",\"n\":[null,false,true,-125,0.5]}";
    cJSON *root = cJSON_Parse(expected);
    mu_assert("document", root != NULL);

    char *failure = NULL;
    for (size_t size = 1; (failure == NULL) && (size <= strlen(expected) + PRINT_SLACK); size++) {
        failure = check_preallocated(root, expected, size);
    }
    cJSON_Delete(root);
    return failure;
}

static char *test_set_valuestring_in_place(void)
{
    cJSON *item = cJSON_CreateString("abcdef");
    mu_assert("item", item != NULL);
    char *original = item->valuestring;

    mu_assert("a shorter value is copied in place", cJSON_SetValuestring(item, "xyz") == original);
    mu_assert("in-place copy is terminated", strcmp(original, "xyz") == 0);
    mu_assert("an equally long value is copied in place",
              cJSON_SetValuestring(item, "uvw") == original);
    mu_assert("an empty value is copied in place", cJSON_SetValuestring(item, "") == original);
    mu_assert("in-place empty value", original[0] == '\0');
    mu_assert("an overlapping value is refused", cJSON_SetValuestring(item, original) == NULL);

    cJSON_Delete(item);
    return NULL;
}

static char *test_set_valuestring_reallocates_or_refuses(void)
{
    cJSON *item = cJSON_CreateString("abc");
    cJSON *number = cJSON_CreateNumber(1.0);
    mu_assert("items", (item != NULL) && (number != NULL));

    mu_assert("a longer value is reallocated",
              strcmp(cJSON_SetValuestring(item, "a longer value"), "a longer value") == 0);
    mu_assert("the item owns the new value", strcmp(item->valuestring, "a longer value") == 0);
    mu_assert("a missing value is refused", cJSON_SetValuestring(item, NULL) == NULL);
    mu_assert("a missing item is refused", cJSON_SetValuestring(NULL, "x") == NULL);
    mu_assert("a number has no string value", cJSON_SetValuestring(number, "x") == NULL);

    cJSON_Delete(item);
    cJSON_Delete(number);
    return NULL;
}

static char *test_parse_containers(void)
{
    cJSON *root = cJSON_Parse("{\"a\":[true,false,null,-1.25e2],\"b\":\"x\",\"c\":{\"d\":1.5}}");
    mu_assert("document", root != NULL);
    const cJSON *array = cJSON_GetObjectItemCaseSensitive(root, "a");
    const cJSON *nested = cJSON_GetObjectItemCaseSensitive(root, "c");

    mu_assert("array size", cJSON_GetArraySize(array) == 4);
    mu_assert("object size", cJSON_GetArraySize(root) == 3);
    mu_assert("a missing array has no size", cJSON_GetArraySize(NULL) == 0);
    mu_assert("true", cJSON_IsTrue(cJSON_GetArrayItem(array, 0)));
    mu_assert("null", cJSON_IsNull(cJSON_GetArrayItem(array, 2)));
    mu_assert("nested", cJSON_GetObjectItemCaseSensitive(nested, "d")->valuedouble == 1.5);

    cJSON_Delete(root);
    return NULL;
}

static char *test_parse_scalars(void)
{
    cJSON *root = cJSON_Parse("{\"n\":-1.25e2,\"s\":\"x\",\"big\":2147483648,"
                              "\"small\":-2147483649}");
    mu_assert("document", root != NULL);
    const cJSON *number = cJSON_GetObjectItemCaseSensitive(root, "n");

    mu_assert("number", cJSON_IsNumber(number) && (number->valuedouble == -125.0));
    mu_assert("integer view", number->valueint == -125);
    mu_assert("string", strcmp(cJSON_GetObjectItemCaseSensitive(root, "s")->valuestring, "x") == 0);
    mu_assert("integer view saturates upwards",
              cJSON_GetObjectItemCaseSensitive(root, "big")->valueint == INT_MAX);
    mu_assert("integer view saturates downwards",
              cJSON_GetObjectItemCaseSensitive(root, "small")->valueint == INT_MIN);

    cJSON_Delete(root);
    return NULL;
}

static char *test_parse_container_errors(void)
{
    mu_assert("unterminated array", fails_at("[1,2", 4));
    mu_assert("trailing comma in array", fails_at("[1,]", 3));
    mu_assert("missing comma", fails_at("[1 2]", 3));
    mu_assert("trailing comma in object", fails_at("{\"a\":1,}", 8));
    mu_assert("missing colon", fails_at("{\"a\" 1}", 5));
    mu_assert("missing value", fails_at("{\"a\":}", 5));
    return NULL;
}

static char *test_parse_scalar_errors(void)
{
    mu_assert("unterminated string", fails_at("\"abc", 1));
    mu_assert("unknown escape", fails_at("\"a\\x\"", 2));
    mu_assert("truncated literal", fails_at("nul", 0));
    mu_assert("sign without digits", fails_at("-", 0));
    mu_assert("empty input", fails_at("", 0));
    mu_assert("missing input", cJSON_Parse(NULL) == NULL);
    return NULL;
}

static char *test_parse_utf16(void)
{
    cJSON *parsed = cJSON_Parse("\"\\u0041\\u00e9\\u20ac\\ud83d\\ude00\"");
    mu_assert("UTF-16 literals", parsed != NULL);
    const bool converted =
        strcmp(parsed->valuestring, "A\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80") == 0;
    cJSON_Delete(parsed);

    mu_assert("one to four byte UTF-8 sequences", converted);
    mu_assert("lone high surrogate", fails_at("\"\\ud83d\"", 1));
    mu_assert("high surrogate without a low one", fails_at("\"\\ud83d\\u0041\"", 1));
    mu_assert("lone low surrogate", fails_at("\"\\ude00\"", 1));
    mu_assert("truncated literal", fails_at("\"\\u00\"", 1));
    return NULL;
}

/* Parses `depth` nested arrays. */
static bool parses_nested(size_t depth)
{
    static char text[2 * (CJSON_NESTING_LIMIT + 1) + 1];
    memset(text, '[', depth);
    memset(text + depth, ']', depth);
    text[2 * depth] = '\0';

    cJSON *parsed = cJSON_Parse(text);
    const bool accepted = parsed != NULL;
    cJSON_Delete(parsed);
    return accepted;
}

static char *test_nesting_limit(void)
{
    mu_assert("the nesting limit is accepted", parses_nested(CJSON_NESTING_LIMIT));
    mu_assert("one level more is refused", !parses_nested(CJSON_NESTING_LIMIT + 1));
    return NULL;
}

static char *test_duplicate(void)
{
    cJSON *root = cJSON_Parse("{\"A\":[1,{\"b\":\"x\"}],\"c\":null}");
    cJSON *deep = cJSON_Duplicate(root, 1);
    cJSON *shallow = cJSON_Duplicate(root, 0);
    mu_assert("document", root != NULL);
    mu_assert("duplicates", (deep != NULL) && (shallow != NULL));

    mu_assert("a deep duplicate is equal", cJSON_Compare(root, deep, 1));
    mu_assert("a deep duplicate is a copy", deep->child != root->child);
    mu_assert("a shallow duplicate has no children", shallow->child == NULL);
    mu_assert("a shallow duplicate is not equal", !cJSON_Compare(root, shallow, 1));
    mu_assert("nothing to duplicate", cJSON_Duplicate(NULL, 1) == NULL);

    cJSON_Delete(root);
    cJSON_Delete(deep);
    cJSON_Delete(shallow);
    return NULL;
}

static char *test_compare(void)
{
    cJSON *root = cJSON_Parse("{\"A\":[1,{\"b\":\"x\"}],\"c\":null}");
    cJSON *other_case = cJSON_Parse("{\"a\":[1,{\"B\":\"x\"}],\"C\":null}");
    cJSON *subset = cJSON_Parse("{\"A\":[1,{\"b\":\"x\"}]}");
    mu_assert("documents", (root != NULL) && (other_case != NULL) && (subset != NULL));

    mu_assert("keys differ by case", !cJSON_Compare(root, other_case, 1));
    mu_assert("keys match without case", cJSON_Compare(root, other_case, 0));
    mu_assert("a subset is not equal", !cJSON_Compare(root, subset, 1));
    mu_assert("a superset is not equal", !cJSON_Compare(subset, root, 1));
    mu_assert("a missing item is not equal", !cJSON_Compare(root, NULL, 1));

    cJSON_Delete(root);
    cJSON_Delete(other_case);
    cJSON_Delete(subset);
    return NULL;
}

static size_t allocations;
static size_t failing_allocation;
static size_t live_allocations;

static void *failing_malloc(size_t size)
{
    if (allocations++ == failing_allocation) {
        return NULL;
    }
    void *memory = malloc(size);
    if (memory != NULL) {
        live_allocations++;
    }
    return memory;
}

static void counting_free(void *memory)
{
    if (memory != NULL) {
        live_allocations--;
    }
    free(memory);
}

/* Parses, prints and duplicates with the n-th allocation failing. Returns
 * whether everything succeeded; every path has to release what it took. */
static bool survives_failure_at(const char *document, size_t failure_point)
{
    allocations = 0;
    failing_allocation = failure_point;

    cJSON *root = cJSON_Parse(document);
    char *printed = cJSON_Print(root);
    cJSON *duplicate = cJSON_Duplicate(root, 1);
    const bool complete = (root != NULL) && (printed != NULL) && (duplicate != NULL) &&
                          cJSON_Compare(root, duplicate, 1);

    cJSON_free(printed);
    cJSON_Delete(duplicate);
    cJSON_Delete(root);
    return complete;
}

static char *test_allocation_failures(void)
{
    /* Custom hooks have no realloc, so this also covers the copying growth
     * path; the long value forces the print buffer to grow. */
    static char long_value[LONG_VALUE_LENGTH + 1];
    static char document[LONG_VALUE_LENGTH + 64];
    cJSON_Hooks hooks = {failing_malloc, counting_free};
    size_t completed = 0;

    memset(long_value, 'v', LONG_VALUE_LENGTH);
    long_value[LONG_VALUE_LENGTH] = '\0';
    const int written = snprintf(document, sizeof(document),
                                 "{\"k\":[1,2.5,\"\\u0001\"],\"long\":\"%s\"}", long_value);
    mu_assert("document fits", (written > 0) && ((size_t)written < sizeof(document)));

    cJSON_InitHooks(&hooks);
    for (size_t point = 0; point < (size_t)FAILURE_POINTS; point++) {
        completed += survives_failure_at(document, point) ? 1 : 0;
        if (live_allocations != 0) {
            break;
        }
    }
    cJSON_InitHooks(NULL);

    mu_assert("every failure path releases what it allocated", live_allocations == 0);
    mu_assert("some allocation failures were injected", completed < (size_t)FAILURE_POINTS);
    mu_assert("the document completes once allocations stop failing", completed > 0);
    return NULL;
}

static char *run_number_valueint_tests(void)
{
    mu_run_test(test_number_valueint_non_finite);
    mu_run_test(test_number_valueint_range);
    mu_run_test(test_number_setter_maps_nan_to_zero);
    return NULL;
}

static char *run_print_tests(void)
{
    mu_run_test(test_print_literals);
    mu_run_test(test_print_numbers);
    mu_run_test(test_print_number_precision);
    mu_run_test(test_print_strings);
    mu_run_test(test_print_preallocated);
    return run_number_valueint_tests();
}

static char *run_parse_tests(void)
{
    mu_run_test(test_parse_containers);
    mu_run_test(test_parse_scalars);
    mu_run_test(test_parse_container_errors);
    mu_run_test(test_parse_scalar_errors);
    mu_run_test(test_parse_utf16);
    mu_run_test(test_nesting_limit);
    return run_print_tests();
}

char *run_tests(void)
{
    mu_run_test(test_version);
    mu_run_test(test_set_valuestring_in_place);
    mu_run_test(test_set_valuestring_reallocates_or_refuses);
    mu_run_test(test_duplicate);
    mu_run_test(test_compare);
    mu_run_test(test_allocation_failures);
    return run_parse_tests();
}
/* NOLINTEND(modernize-use-nullptr) */
