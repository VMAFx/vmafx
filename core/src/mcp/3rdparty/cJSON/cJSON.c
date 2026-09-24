/*
  Copyright (c) 2009-2017 Dave Gamble and cJSON contributors

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in
  all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
  THE SOFTWARE.
*/

/* cJSON */
/* JSON parser in C. */

/* disable warnings about old C89 functions in MSVC */
#if !defined(_CRT_SECURE_NO_DEPRECATE) && defined(_MSC_VER)
#define _CRT_SECURE_NO_DEPRECATE
#endif

#ifdef __GNUC__
#pragma GCC visibility push(default)
#endif
#if defined(_MSC_VER)
#pragma warning(push)
/* disable warning about single line comments in system headers */
#pragma warning(disable : 4001)
#endif

#include <string.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <limits.h>
#include <ctype.h>
#include <float.h>

#ifdef ENABLE_LOCALES
#include <locale.h>
#endif

#if defined(_MSC_VER)
#pragma warning(pop)
#endif
#ifdef __GNUC__
#pragma GCC visibility pop
#endif

#include "cJSON.h"

/* VMAFx fork delta against upstream cJSON 1.7.19 (ADR-0683, ADR-1061, ADR-1142): the unbounded
 * string functions the fork bans are replaced by bounded snprintf/memcpy, cJSON_GetArraySize
 * saturates at INT_MAX, and the jump-based and over-long functions are split into helpers without
 * changing behaviour. A re-vendor must re-apply this delta; see AGENTS.md next to this file. */

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `NULL` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

/* define our own boolean type */
#ifdef true
#undef true
#endif
#define true ((cJSON_bool)1)

#ifdef false
#undef false
#endif
#define false ((cJSON_bool)0)

/* define isnan and isinf for ANSI C, if in C99 or above, isnan and isinf has been defined in math.h */
#ifndef isinf
#define isinf(d) (isnan((d - d)) && !isnan(d))
#endif
#ifndef isnan
#define isnan(d) (d != d)
#endif

#ifndef NAN
#ifdef _WIN32
#define NAN sqrt(-1.0)
#else
#define NAN 0.0 / 0.0
#endif
#endif

typedef struct {
    const unsigned char *json;
    size_t position;
} error;
static error global_error = {NULL, 0};

CJSON_PUBLIC(const char *) cJSON_GetErrorPtr(void)
{
    return (const char *)(global_error.json + global_error.position);
}

CJSON_PUBLIC(char *) cJSON_GetStringValue(const cJSON *const item)
{
    if (!cJSON_IsString(item)) {
        return NULL;
    }

    return item->valuestring;
}

CJSON_PUBLIC(double) cJSON_GetNumberValue(const cJSON *const item)
{
    if (!cJSON_IsNumber(item)) {
        return (double)NAN;
    }

    return item->valuedouble;
}

/* This is a safeguard to prevent copy-pasters from using incompatible C and header files */
#if (CJSON_VERSION_MAJOR != 1) || (CJSON_VERSION_MINOR != 7) || (CJSON_VERSION_PATCH != 19)
#error cJSON.h and cJSON.c have different versions. Make sure that both have the same.
#endif

CJSON_PUBLIC(const char *) cJSON_Version(void)
{
    static char version[15];
    (void)snprintf(version, sizeof(version), "%i.%i.%i", CJSON_VERSION_MAJOR, CJSON_VERSION_MINOR,
                   CJSON_VERSION_PATCH);

    return version;
}

/* Case insensitive string comparison, doesn't consider two NULL pointers equal though */
static int case_insensitive_strcmp(const unsigned char *string1, const unsigned char *string2)
{
    if ((string1 == NULL) || (string2 == NULL)) {
        return 1;
    }

    if (string1 == string2) {
        return 0;
    }

    for (; tolower(*string1) == tolower(*string2); (void)string1++, string2++) {
        if (*string1 == '\0') {
            return 0;
        }
    }

    return tolower(*string1) - tolower(*string2);
}

typedef struct internal_hooks {
    void *(CJSON_CDECL *allocate)(size_t size);
    void(CJSON_CDECL *deallocate)(void *pointer);
    void *(CJSON_CDECL *reallocate)(void *pointer, size_t size);
} internal_hooks;

#if defined(_MSC_VER)
/* work around MSVC error C2322: '...' address of dllimport '...' is not static */
static void *CJSON_CDECL internal_malloc(size_t size)
{
    return malloc(size);
}
static void CJSON_CDECL internal_free(void *pointer)
{
    free(pointer);
}
static void *CJSON_CDECL internal_realloc(void *pointer, size_t size)
{
    return realloc(pointer, size);
}
#else
#define internal_malloc malloc
#define internal_free free
#define internal_realloc realloc
#endif

/* strlen of character literals resolved at compile time */
#define static_strlen(string_literal) (sizeof(string_literal) - sizeof(""))

static internal_hooks global_hooks = {internal_malloc, internal_free, internal_realloc};

static unsigned char *cJSON_strdup(const unsigned char *string, const internal_hooks *const hooks)
{
    size_t length = 0;
    unsigned char *copy = NULL;

    if (string == NULL) {
        return NULL;
    }

    length = strlen((const char *)string) + sizeof("");
    copy = (unsigned char *)hooks->allocate(length);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, string, length);

    return copy;
}

CJSON_PUBLIC(void) cJSON_InitHooks(cJSON_Hooks *hooks)
{
    if (hooks == NULL) {
        /* Reset hooks */
        global_hooks.allocate = malloc;
        global_hooks.deallocate = free;
        global_hooks.reallocate = realloc;
        return;
    }

    global_hooks.allocate = malloc;
    if (hooks->malloc_fn != NULL) {
        global_hooks.allocate = hooks->malloc_fn;
    }

    global_hooks.deallocate = free;
    if (hooks->free_fn != NULL) {
        global_hooks.deallocate = hooks->free_fn;
    }

    /* use realloc only if both free and malloc are used */
    global_hooks.reallocate = NULL;
    if ((global_hooks.allocate == malloc) && (global_hooks.deallocate == free)) {
        global_hooks.reallocate = realloc;
    }
}

/* Internal constructor. */
static cJSON *cJSON_New_Item(const internal_hooks *const hooks)
{
    cJSON *node = (cJSON *)hooks->allocate(sizeof(cJSON));
    if (node) {
        memset(node, '\0', sizeof(cJSON));
    }

    return node;
}

/* Delete a cJSON structure. */
CJSON_PUBLIC(void) cJSON_Delete(cJSON *item)
{
    cJSON *next = NULL;
    while (item != NULL) {
        next = item->next;
        if (!(item->type & cJSON_IsReference) && (item->child != NULL)) {
            cJSON_Delete(item->child);
        }
        if (!(item->type & cJSON_IsReference) && (item->valuestring != NULL)) {
            global_hooks.deallocate(item->valuestring);
            item->valuestring = NULL;
        }
        if (!(item->type & cJSON_StringIsConst) && (item->string != NULL)) {
            global_hooks.deallocate(item->string);
            item->string = NULL;
        }
        global_hooks.deallocate(item);
        item = next;
    }
}

/* get the decimal point character of the current locale */
static unsigned char get_decimal_point(void)
{
#ifdef ENABLE_LOCALES
    struct lconv *lconv = localeconv();
    return (unsigned char)lconv->decimal_point[0];
#else
    return '.';
#endif
}

typedef struct {
    const unsigned char *content;
    size_t length;
    size_t offset;
    size_t depth; /* How deeply nested (in arrays/objects) is the input at the current offset. */
    internal_hooks hooks;
} parse_buffer;

/* check if the given size is left to read in a given parse buffer (starting with 1) */
#define can_read(buffer, size)                                                                     \
    (((buffer) != NULL) && (((buffer)->offset + (size)) <= (buffer)->length))
/* check if the buffer can be accessed at the given index (starting with 0) */
#define can_access_at_index(buffer, index)                                                         \
    (((buffer) != NULL) && (((buffer)->offset + (index)) < (buffer)->length))
#define cannot_access_at_index(buffer, index) (!can_access_at_index(buffer, index))
/* get a pointer to the buffer at the position */
#define buffer_at_offset(buffer) ((buffer)->content + (buffer)->offset)

/* check whether a character can be part of a JSON number */
static cJSON_bool is_number_character(const unsigned char character)
{
    switch (character) {
    case '0':
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
    case '7':
    case '8':
    case '9':
    case '+':
    case '-':
    case 'e':
    case 'E':
    case '.':
        return true;

    default:
        return false;
    }
}

/* length of the run of number characters at the current offset of the buffer */
static size_t number_string_span(const parse_buffer *const input_buffer,
                                 cJSON_bool *const has_decimal_point)
{
    size_t length = 0;

    while (can_access_at_index(input_buffer, length) &&
           is_number_character(buffer_at_offset(input_buffer)[length])) {
        if (buffer_at_offset(input_buffer)[length] == '.') {
            *has_decimal_point = true;
        }
        length++;
    }

    return length;
}

/* Parse the input text to generate a number, and populate the result into item. */
static cJSON_bool parse_number(cJSON *const item, parse_buffer *const input_buffer)
{
    double number = 0;
    unsigned char *after_end = NULL;
    unsigned char *number_c_string;
    unsigned char decimal_point = get_decimal_point();
    size_t i = 0;
    size_t number_string_length = 0;
    cJSON_bool has_decimal_point = false;

    if ((input_buffer == NULL) || (input_buffer->content == NULL)) {
        return false;
    }

    /* copy the number into a temporary buffer and replace '.' with the decimal point
     * of the current locale (for strtod)
     * This also takes care of '\0' not necessarily being available for marking the end of the input */
    number_string_length = number_string_span(input_buffer, &has_decimal_point);

    /* malloc for temporary buffer, add 1 for '\0' */
    number_c_string = (unsigned char *)input_buffer->hooks.allocate(number_string_length + 1);
    if (number_c_string == NULL) {
        return false; /* allocation failure */
    }

    memcpy(number_c_string, buffer_at_offset(input_buffer), number_string_length);
    number_c_string[number_string_length] = '\0';

    if (has_decimal_point) {
        for (i = 0; i < number_string_length; i++) {
            if (number_c_string[i] == '.') {
                /* replace '.' with the decimal point of the current locale (for strtod) */
                number_c_string[i] = decimal_point;
            }
        }
    }

    number = strtod((const char *)number_c_string, (char **)&after_end);
    if (number_c_string == after_end) {
        /* free the temporary buffer */
        input_buffer->hooks.deallocate(number_c_string);
        return false; /* parse_error */
    }

    /* sets valuedouble, and valueint with saturation in case of overflow */
    (void)cJSON_SetNumberHelper(item, number);

    item->type = cJSON_Number;

    input_buffer->offset += (size_t)(after_end - number_c_string);
    /* free the temporary buffer */
    input_buffer->hooks.deallocate(number_c_string);
    return true;
}

/* Fork delta: double -> int with saturation, defined for every input.
 *
 * Upstream 1.7.19 open-codes the two range checks at both call sites and then
 * casts. NaN compares false against both bounds, so it reached `(int)number`,
 * which is undefined behaviour (C11 6.3.1.4p1); clang's
 * -fsanitize=float-cast-overflow reports it. It is reachable in production:
 * the MCP server hands a VMAF score, which can be NaN, to
 * cJSON_AddNumberToObject(). NaN maps to 0. `valuedouble` keeps the NaN, and
 * print_number() still emits `null` for it, so printed output is unchanged. */
static int saturate_to_int(double number)
{
    if (number != number) {
        return 0;
    }
    if (number >= INT_MAX) {
        return INT_MAX;
    }
    if (number <= (double)INT_MIN) {
        return INT_MIN;
    }
    return (int)number;
}

/* don't ask me, but the original cJSON_SetNumberValue returns an integer or double */
CJSON_PUBLIC(double) cJSON_SetNumberHelper(cJSON *object, double number)
{
    object->valueint = saturate_to_int(number);

    return object->valuedouble = number;
}

/* Note: when passing a NULL valuestring, cJSON_SetValuestring treats this as an error and return NULL */
CJSON_PUBLIC(char *) cJSON_SetValuestring(cJSON *object, const char *valuestring)
{
    char *copy = NULL;
    size_t v1_len;
    size_t v2_len;
    /* if object's type is not cJSON_String or is cJSON_IsReference, it should not set valuestring */
    if ((object == NULL) || !(object->type & cJSON_String) || (object->type & cJSON_IsReference)) {
        return NULL;
    }
    /* return NULL if the object is corrupted or valuestring is NULL */
    if (object->valuestring == NULL || valuestring == NULL) {
        return NULL;
    }

    v1_len = strlen(valuestring);
    v2_len = strlen(object->valuestring);

    if (v1_len <= v2_len) {
        /* the copy does not handle overlapping strings: [X1, X2] [Y1, Y2] => X2 < Y1 or Y2 < X1 */
        if (!(valuestring + v1_len < object->valuestring ||
              object->valuestring + v2_len < valuestring)) {
            return NULL;
        }
        /* bounded: v1_len <= v2_len, so the terminator still fits the existing allocation */
        memcpy(object->valuestring, valuestring, v1_len + sizeof(""));
        return object->valuestring;
    }
    copy = (char *)cJSON_strdup((const unsigned char *)valuestring, &global_hooks);
    if (copy == NULL) {
        return NULL;
    }
    if (object->valuestring != NULL) {
        cJSON_free(object->valuestring);
    }
    object->valuestring = copy;

    return copy;
}

typedef struct {
    unsigned char *buffer;
    size_t length;
    size_t offset;
    size_t depth; /* current nesting depth (for formatted printing) */
    cJSON_bool noalloc;
    cJSON_bool format; /* is this print a formatted print */
    internal_hooks hooks;
} printbuffer;

/* move a printbuffer into a new allocation of newsize bytes; on failure the old one is released */
static unsigned char *reallocate_printbuffer(printbuffer *const p, const size_t newsize)
{
    unsigned char *newbuffer = NULL;

    if (p->hooks.reallocate != NULL) {
        /* reallocate with realloc if available */
        newbuffer = (unsigned char *)p->hooks.reallocate(p->buffer, newsize);
        if (newbuffer == NULL) {
            p->hooks.deallocate(p->buffer);
            p->length = 0;
            p->buffer = NULL;
        }

        return newbuffer;
    }

    /* otherwise reallocate manually */
    newbuffer = (unsigned char *)p->hooks.allocate(newsize);
    if (!newbuffer) {
        p->hooks.deallocate(p->buffer);
        p->length = 0;
        p->buffer = NULL;

        return NULL;
    }

    memcpy(newbuffer, p->buffer, p->offset + 1);
    p->hooks.deallocate(p->buffer);

    return newbuffer;
}

/* realloc printbuffer if necessary to have at least "needed" bytes more */
static unsigned char *ensure(printbuffer *const p, size_t needed)
{
    unsigned char *newbuffer = NULL;
    size_t newsize = 0;

    if ((p == NULL) || (p->buffer == NULL)) {
        return NULL;
    }

    if ((p->length > 0) && (p->offset >= p->length)) {
        /* make sure that offset is valid */
        return NULL;
    }

    if (needed > INT_MAX) {
        /* sizes bigger than INT_MAX are currently not supported */
        return NULL;
    }

    needed += p->offset + 1;
    if (needed <= p->length) {
        return p->buffer + p->offset;
    }

    if (p->noalloc) {
        return NULL;
    }

    /* calculate new buffer size */
    if (needed > (INT_MAX / 2)) {
        /* overflow of int, use INT_MAX if possible */
        if (needed <= INT_MAX) {
            newsize = INT_MAX;
        } else {
            return NULL;
        }
    } else {
        newsize = needed * 2;
    }

    newbuffer = reallocate_printbuffer(p, newsize);
    if (newbuffer == NULL) {
        return NULL;
    }
    p->length = newsize;
    p->buffer = newbuffer;

    return newbuffer + p->offset;
}

/* calculate the new length of the string in a printbuffer and update the offset */
static void update_offset(printbuffer *const buffer)
{
    const unsigned char *buffer_pointer = NULL;
    if ((buffer == NULL) || (buffer->buffer == NULL)) {
        return;
    }
    buffer_pointer = buffer->buffer + buffer->offset;

    buffer->offset += strlen((const char *)buffer_pointer);
}

/* securely comparison of floating-point variables */
static cJSON_bool compare_double(double a, double b)
{
    double maxVal = fabs(a) > fabs(b) ? fabs(a) : fabs(b);
    return (fabs(a - b) <= maxVal * DBL_EPSILON);
}

/* check whether the printed number converts back to the double it was printed from */
static cJSON_bool number_round_trips(const unsigned char *const number_buffer,
                                     const double expected)
{
    char *after_end = NULL;
    const double parsed = strtod((const char *)number_buffer, &after_end);

    if (after_end == (const char *)number_buffer) {
        return false;
    }

    return compare_double(parsed, expected);
}

/* Render the number nicely from the given item into a string. */
static cJSON_bool print_number(const cJSON *const item, printbuffer *const output_buffer)
{
    unsigned char *output_pointer = NULL;
    double d = item->valuedouble;
    int length = 0;
    size_t i = 0;
    unsigned char number_buffer[26] = {0}; /* temporary buffer to print the number into */
    unsigned char decimal_point = get_decimal_point();

    if (output_buffer == NULL) {
        return false;
    }

    /* This checks for NaN and Infinity */
    if (isnan(d) || isinf(d)) {
        length = snprintf((char *)number_buffer, sizeof(number_buffer), "null");
    } else if (d - (double)item->valueint == 0.0) {
        length = snprintf((char *)number_buffer, sizeof(number_buffer), "%d", item->valueint);
    } else {
        /* Try 15 decimal places of precision to avoid nonsignificant nonzero digits */
        length = snprintf((char *)number_buffer, sizeof(number_buffer), "%1.15g", d);

        /* Check whether the original double can be recovered */
        if (!number_round_trips(number_buffer, d)) {
            /* If not, print with 17 decimal places of precision */
            length = snprintf((char *)number_buffer, sizeof(number_buffer), "%1.17g", d);
        }
    }

    /* snprintf failed or the output was truncated */
    if ((length < 0) || (length > (int)(sizeof(number_buffer) - 1))) {
        return false;
    }

    /* reserve appropriate space in the output */
    output_pointer = ensure(output_buffer, (size_t)length + sizeof(""));
    if (output_pointer == NULL) {
        return false;
    }

    /* copy the printed number to the output and replace locale
     * dependent decimal point with '.' */
    for (i = 0; i < ((size_t)length); i++) {
        if (number_buffer[i] == decimal_point) {
            output_pointer[i] = '.';
            continue;
        }

        output_pointer[i] = number_buffer[i];
    }
    output_pointer[i] = '\0';

    output_buffer->offset += (size_t)length;

    return true;
}

/* parse 4 digit hexadecimal number */
static unsigned parse_hex4(const unsigned char *const input)
{
    unsigned int h = 0;
    size_t i = 0;

    for (i = 0; i < 4; i++) {
        /* parse digit */
        if ((input[i] >= '0') && (input[i] <= '9')) {
            h += (unsigned int)input[i] - '0';
        } else if ((input[i] >= 'A') && (input[i] <= 'F')) {
            h += (unsigned int)10 + input[i] - 'A';
        } else if ((input[i] >= 'a') && (input[i] <= 'f')) {
            h += (unsigned int)10 + input[i] - 'a';
        } else /* invalid */
        {
            return 0;
        }

        if (i < 3) {
            /* shift left to make place for the next nibble */
            h = h << 4;
        }
    }

    return h;
}

/* reads the codepoint of a UTF-16 literal, which can be one or two sequences of the form \uXXXX
 * returns the length of the literal, or 0 if it is invalid */
static unsigned char utf16_literal_codepoint(const unsigned char *const first_sequence,
                                             const unsigned char *const input_end,
                                             long unsigned int *const codepoint)
{
    unsigned int first_code = 0;
    unsigned int second_code = 0;
    /* Formed only once the bounds check below proves the six bytes exist:
     * first_sequence + 6 is past one-past-the-end of the parse buffer for a
     * truncated literal, and forming such a pointer is undefined even when it
     * is never dereferenced (C17 6.5.6p8; UBSan -fsanitize=pointer-overflow).
     * The pre-refactor code computed it inside the surrogate-pair branch. */
    const unsigned char *second_sequence = NULL;

    if ((input_end - first_sequence) < 6) {
        /* input ends unexpectedly */
        return 0;
    }

    /* get the first utf16 sequence */
    first_code = parse_hex4(first_sequence + 2);

    /* check that the code is valid */
    if (((first_code >= 0xDC00) && (first_code <= 0xDFFF))) {
        return 0;
    }

    if ((first_code < 0xD800) || (first_code > 0xDBFF)) {
        *codepoint = first_code;
        return 6; /* \uXXXX */
    }

    /* UTF16 surrogate pair */
    second_sequence = first_sequence + 6;
    if ((input_end - second_sequence) < 6) {
        /* input ends unexpectedly */
        return 0;
    }

    if ((second_sequence[0] != '\\') || (second_sequence[1] != 'u')) {
        /* missing second half of the surrogate pair */
        return 0;
    }

    /* get the second utf16 sequence */
    second_code = parse_hex4(second_sequence + 2);
    /* check that the code is valid */
    if ((second_code < 0xDC00) || (second_code > 0xDFFF)) {
        /* invalid second half of the surrogate pair */
        return 0;
    }

    /* calculate the unicode codepoint from the surrogate pair */
    *codepoint = 0x10000 + (((first_code & 0x3FF) << 10) | (second_code & 0x3FF));

    return 12; /* \uXXXX\uXXXX */
}

/* encodes a codepoint as UTF-8 and advances the output, returns 0 if the codepoint is invalid
 * takes at maximum 4 bytes to encode:
 * 11110xxx 10xxxxxx 10xxxxxx 10xxxxxx */
static unsigned char utf8_encode_codepoint(long unsigned int codepoint,
                                           unsigned char **output_pointer)
{
    unsigned char utf8_length = 0;
    unsigned char utf8_position = 0;
    unsigned char first_byte_mark = 0;

    if (codepoint < 0x80) {
        /* normal ascii, encoding 0xxxxxxx */
        utf8_length = 1;
    } else if (codepoint < 0x800) {
        /* two bytes, encoding 110xxxxx 10xxxxxx */
        utf8_length = 2;
        first_byte_mark = 0xC0; /* 11000000 */
    } else if (codepoint < 0x10000) {
        /* three bytes, encoding 1110xxxx 10xxxxxx 10xxxxxx */
        utf8_length = 3;
        first_byte_mark = 0xE0; /* 11100000 */
    } else if (codepoint <= 0x10FFFF) {
        /* four bytes, encoding 1110xxxx 10xxxxxx 10xxxxxx 10xxxxxx */
        utf8_length = 4;
        first_byte_mark = 0xF0; /* 11110000 */
    } else {
        /* invalid unicode codepoint */
        return 0;
    }

    /* encode as utf8 */
    for (utf8_position = (unsigned char)(utf8_length - 1); utf8_position > 0; utf8_position--) {
        /* 10xxxxxx */
        (*output_pointer)[utf8_position] = (unsigned char)((codepoint | 0x80) & 0xBF);
        codepoint >>= 6;
    }
    /* encode first byte */
    if (utf8_length > 1) {
        (*output_pointer)[0] = (unsigned char)((codepoint | first_byte_mark) & 0xFF);
    } else {
        (*output_pointer)[0] = (unsigned char)(codepoint & 0x7F);
    }

    *output_pointer += utf8_length;

    return utf8_length;
}

/* converts a UTF-16 literal to UTF-8
 * A literal can be one or two sequences of the form \uXXXX */
static unsigned char utf16_literal_to_utf8(const unsigned char *const input_pointer,
                                           const unsigned char *const input_end,
                                           unsigned char **output_pointer)
{
    long unsigned int codepoint = 0;
    const unsigned char sequence_length =
        utf16_literal_codepoint(input_pointer, input_end, &codepoint);

    if (sequence_length == 0) {
        return 0;
    }

    if (utf8_encode_codepoint(codepoint, output_pointer) == 0) {
        return 0;
    }

    return sequence_length;
}

/* find the closing quote of the string literal at the current offset and count the bytes that
 * unescaping will drop; returns NULL if the literal is not terminated inside the buffer */
static const unsigned char *find_string_end(const parse_buffer *const input_buffer,
                                            size_t *const skipped_bytes)
{
    const unsigned char *input_end = buffer_at_offset(input_buffer) + 1;

    while (((size_t)(input_end - input_buffer->content) < input_buffer->length) &&
           (*input_end != '\"')) {
        /* is escape sequence */
        if (input_end[0] == '\\') {
            if ((size_t)(input_end + 1 - input_buffer->content) >= input_buffer->length) {
                /* prevent buffer overflow when last input character is a backslash */
                return NULL;
            }
            (*skipped_bytes)++;
            input_end++;
        }
        input_end++;
    }
    if (((size_t)(input_end - input_buffer->content) >= input_buffer->length) ||
        (*input_end != '\"')) {
        return NULL; /* string ended unexpectedly */
    }

    return input_end;
}

/* unescape the sequence at *input_pointer; the input only advances if the sequence is valid */
static cJSON_bool unescape_sequence(const unsigned char **const input_pointer,
                                    const unsigned char *const input_end,
                                    unsigned char **const output_pointer)
{
    unsigned char sequence_length = 2;

    if ((input_end - *input_pointer) < 1) {
        return false;
    }

    switch ((*input_pointer)[1]) {
    case 'b':
        *(*output_pointer)++ = '\b';
        break;
    case 'f':
        *(*output_pointer)++ = '\f';
        break;
    case 'n':
        *(*output_pointer)++ = '\n';
        break;
    case 'r':
        *(*output_pointer)++ = '\r';
        break;
    case 't':
        *(*output_pointer)++ = '\t';
        break;
    case '\"':
    case '\\':
    case '/':
        *(*output_pointer)++ = (*input_pointer)[1];
        break;

    /* UTF-16 literal */
    case 'u':
        sequence_length = utf16_literal_to_utf8(*input_pointer, input_end, output_pointer);
        if (sequence_length == 0) {
            /* failed to convert UTF16-literal to UTF-8 */
            return false;
        }
        break;

    default:
        return false;
    }
    *input_pointer += sequence_length;

    return true;
}

/* copy the string literal into output while unescaping it; on failure *input_pointer is left at
 * the sequence that could not be unescaped */
static cJSON_bool unescape_string(const unsigned char **const input_pointer,
                                  const unsigned char *const input_end, unsigned char *const output)
{
    unsigned char *output_pointer = output;

    /* loop through the string literal */
    while (*input_pointer < input_end) {
        if (**input_pointer != '\\') {
            *output_pointer++ = *(*input_pointer)++;
        }
        /* escape sequence */
        else if (!unescape_sequence(input_pointer, input_end, &output_pointer)) {
            return false;
        }
    }

    /* zero terminate the output */
    *output_pointer = '\0';

    return true;
}

/* Parse the input text into an unescaped cinput, and populate item. */
static cJSON_bool parse_string(cJSON *const item, parse_buffer *const input_buffer)
{
    const unsigned char *input_pointer = buffer_at_offset(input_buffer) + 1;
    const unsigned char *input_end = NULL;
    unsigned char *output = NULL;
    size_t skipped_bytes = 0;
    cJSON_bool unescaped = false;

    /* not a string unless it opens with a quote */
    if (buffer_at_offset(input_buffer)[0] == '\"') {
        input_end = find_string_end(input_buffer, &skipped_bytes);
    }

    if (input_end != NULL) {
        /* This is at most how much we need for the output (overestimate) */
        const size_t allocation_length =
            (size_t)(input_end - buffer_at_offset(input_buffer)) - skipped_bytes;
        output = (unsigned char *)input_buffer->hooks.allocate(allocation_length + sizeof(""));
    }

    if (output != NULL) {
        unescaped = unescape_string(&input_pointer, input_end, output);
    }

    if (!unescaped) {
        if (output != NULL) {
            input_buffer->hooks.deallocate(output);
            output = NULL;
        }

        if (input_pointer != NULL) {
            input_buffer->offset = (size_t)(input_pointer - input_buffer->content);
        }

        return false;
    }

    item->type = cJSON_String;
    item->valuestring = (char *)output;

    input_buffer->offset = (size_t)(input_end - input_buffer->content);
    input_buffer->offset++;

    return true;
}

/* numbers of additional characters needed for escaping the string, also reports its length */
static size_t count_escape_characters(const unsigned char *const input, size_t *const input_length)
{
    const unsigned char *input_pointer = NULL;
    size_t escape_characters = 0;

    for (input_pointer = input; *input_pointer; input_pointer++) {
        switch (*input_pointer) {
        case '\"':
        case '\\':
        case '\b':
        case '\f':
        case '\n':
        case '\r':
        case '\t':
            /* one character escape sequence */
            escape_characters++;
            break;
        default:
            if (*input_pointer < 32) {
                /* UTF-16 escape sequence uXXXX */
                escape_characters += 5;
            }
            break;
        }
    }
    *input_length = (size_t)(input_pointer - input);

    return escape_characters;
}

/* write the escape sequence for a character after the backslash, output_end bounds the write;
 * returns the last character that was written, NULL if the output did not fit */
static unsigned char *print_escape_sequence(const unsigned char character,
                                            unsigned char *const output_pointer,
                                            const unsigned char *const output_end)
{
    switch (character) {
    case '\\':
        *output_pointer = '\\';
        return output_pointer;
    case '\"':
        *output_pointer = '\"';
        return output_pointer;
    case '\b':
        *output_pointer = 'b';
        return output_pointer;
    case '\f':
        *output_pointer = 'f';
        return output_pointer;
    case '\n':
        *output_pointer = 'n';
        return output_pointer;
    case '\r':
        *output_pointer = 'r';
        return output_pointer;
    case '\t':
        *output_pointer = 't';
        return output_pointer;
    default:
        break;
    }

    /* escape and print as unicode codepoint, bounded by the space that ensure() reserved */
    if (snprintf((char *)output_pointer, (size_t)(output_end - output_pointer), "u%04x",
                 character) != 5) {
        return NULL;
    }

    return output_pointer + 4;
}

/* copy the string between quotes and escape what needs to be escaped */
static cJSON_bool print_escaped_string(const unsigned char *const input,
                                       unsigned char *const output, const size_t output_length)
{
    /* ensure() reserved the escaped string, both quotes and the terminator */
    const unsigned char *const output_end = output + output_length + sizeof("\"\"");
    const unsigned char *input_pointer = NULL;
    unsigned char *output_pointer = output + 1;

    output[0] = '\"';
    for (input_pointer = input; *input_pointer != '\0'; (void)input_pointer++, output_pointer++) {
        if ((*input_pointer > 31) && (*input_pointer != '\"') && (*input_pointer != '\\')) {
            /* normal character, copy */
            *output_pointer = *input_pointer;
            continue;
        }

        /* character needs to be escaped */
        *output_pointer++ = '\\';
        output_pointer = print_escape_sequence(*input_pointer, output_pointer, output_end);
        if (output_pointer == NULL) {
            return false;
        }
    }
    output[output_length + 1] = '\"';
    output[output_length + 2] = '\0';

    return true;
}

/* Render the cstring provided to an escaped version that can be printed. */
static cJSON_bool print_string_ptr(const unsigned char *const input,
                                   printbuffer *const output_buffer)
{
    unsigned char *output = NULL;
    size_t input_length = 0;
    size_t output_length = 0;
    /* numbers of additional characters needed for escaping */
    size_t escape_characters = 0;

    if (output_buffer == NULL) {
        return false;
    }

    /* empty string */
    if (input == NULL) {
        output = ensure(output_buffer, sizeof("\"\""));
        if (output == NULL) {
            return false;
        }
        memcpy(output, "\"\"", sizeof("\"\""));

        return true;
    }

    escape_characters = count_escape_characters(input, &input_length);
    output_length = input_length + escape_characters;

    output = ensure(output_buffer, output_length + sizeof("\"\""));
    if (output == NULL) {
        return false;
    }

    /* no characters have to be escaped */
    if (escape_characters == 0) {
        output[0] = '\"';
        memcpy(output + 1, input, output_length);
        output[output_length + 1] = '\"';
        output[output_length + 2] = '\0';

        return true;
    }

    return print_escaped_string(input, output, output_length);
}

/* Invoke print_string_ptr (which is useful) on an item. */
static cJSON_bool print_string(const cJSON *const item, printbuffer *const p)
{
    return print_string_ptr((unsigned char *)item->valuestring, p);
}

/* Predeclare these prototypes. */
static cJSON_bool parse_value(cJSON *const item, parse_buffer *const input_buffer);
static cJSON_bool print_value(const cJSON *const item, printbuffer *const output_buffer);
static cJSON_bool parse_array(cJSON *const item, parse_buffer *const input_buffer);
static cJSON_bool print_array(const cJSON *const item, printbuffer *const output_buffer);
static cJSON_bool parse_object(cJSON *const item, parse_buffer *const input_buffer);
static cJSON_bool print_object(const cJSON *const item, printbuffer *const output_buffer);

/* Utility to jump whitespace and cr/lf */
static parse_buffer *buffer_skip_whitespace(parse_buffer *const buffer)
{
    if ((buffer == NULL) || (buffer->content == NULL)) {
        return NULL;
    }

    if (cannot_access_at_index(buffer, 0)) {
        return buffer;
    }

    while (can_access_at_index(buffer, 0) && (buffer_at_offset(buffer)[0] <= 32)) {
        buffer->offset++;
    }

    if (buffer->offset == buffer->length) {
        buffer->offset--;
    }

    return buffer;
}

/* skip the UTF-8 BOM (byte order mark) if it is at the beginning of a buffer */
static parse_buffer *skip_utf8_bom(parse_buffer *const buffer)
{
    if ((buffer == NULL) || (buffer->content == NULL) || (buffer->offset != 0)) {
        return NULL;
    }

    if (can_access_at_index(buffer, 4) &&
        (strncmp((const char *)buffer_at_offset(buffer), "\xEF\xBB\xBF", 3) == 0)) {
        buffer->offset += 3;
    }

    return buffer;
}

CJSON_PUBLIC(cJSON *)
cJSON_ParseWithOpts(const char *value, const char **return_parse_end,
                    cJSON_bool require_null_terminated)
{
    size_t buffer_length;

    if (NULL == value) {
        return NULL;
    }

    /* Adding null character size due to require_null_terminated. */
    buffer_length = strlen(value) + sizeof("");

    return cJSON_ParseWithLengthOpts(value, buffer_length, return_parse_end,
                                     require_null_terminated);
}

/* parse the root value and, if required, check that only a null terminator follows it */
static cJSON_bool parse_root(cJSON *const item, parse_buffer *const buffer,
                             const cJSON_bool require_null_terminated)
{
    if (!parse_value(item, buffer_skip_whitespace(skip_utf8_bom(buffer)))) {
        /* parse failure. ep is set. */
        return false;
    }

    /* if we require null-terminated JSON without appended garbage, skip and then check for a null terminator */
    if (require_null_terminated) {
        buffer_skip_whitespace(buffer);
        if ((buffer->offset >= buffer->length) || buffer_at_offset(buffer)[0] != '\0') {
            return false;
        }
    }

    return true;
}

/* record where parsing failed, for cJSON_GetErrorPtr and return_parse_end */
static void record_parse_error(const char *const value, const parse_buffer *const buffer,
                               const char **const return_parse_end)
{
    error local_error;
    local_error.json = (const unsigned char *)value;
    local_error.position = 0;

    if (buffer->offset < buffer->length) {
        local_error.position = buffer->offset;
    } else if (buffer->length > 0) {
        local_error.position = buffer->length - 1;
    }

    if (return_parse_end != NULL) {
        *return_parse_end = (const char *)local_error.json + local_error.position;
    }

    global_error = local_error;
}

/* Parse an object - create a new root, and populate. */
CJSON_PUBLIC(cJSON *)
cJSON_ParseWithLengthOpts(const char *value, size_t buffer_length, const char **return_parse_end,
                          cJSON_bool require_null_terminated)
{
    parse_buffer buffer = {0, 0, 0, 0, {0, 0, 0}};
    cJSON *item = NULL;
    cJSON_bool parsed = false;

    /* reset error position */
    global_error.json = NULL;
    global_error.position = 0;

    if ((value != NULL) && (0 != buffer_length)) {
        buffer.content = (const unsigned char *)value;
        buffer.length = buffer_length;
        buffer.offset = 0;
        buffer.hooks = global_hooks;

        item = cJSON_New_Item(&global_hooks);
    }

    if (item != NULL) {
        parsed = parse_root(item, &buffer, require_null_terminated);
    }

    if (!parsed) {
        if (item != NULL) {
            cJSON_Delete(item);
        }

        if (value != NULL) {
            record_parse_error(value, &buffer, return_parse_end);
        }

        return NULL;
    }

    if (return_parse_end) {
        *return_parse_end = (const char *)buffer_at_offset(&buffer);
    }

    return item;
}

/* Default options for cJSON_Parse */
CJSON_PUBLIC(cJSON *) cJSON_Parse(const char *value)
{
    return cJSON_ParseWithOpts(value, 0, 0);
}

CJSON_PUBLIC(cJSON *) cJSON_ParseWithLength(const char *value, size_t buffer_length)
{
    return cJSON_ParseWithLengthOpts(value, buffer_length, 0, 0);
}

#define cjson_min(a, b) (((a) < (b)) ? (a) : (b))

/* hand the printed text over to an allocation of exactly the needed size; on success the
 * printbuffer no longer owns a buffer, on failure it still does */
static unsigned char *detach_printbuffer(printbuffer *const buffer,
                                         const internal_hooks *const hooks)
{
    unsigned char *printed = NULL;

    /* check if reallocate is available */
    if (hooks->reallocate != NULL) {
        printed = (unsigned char *)hooks->reallocate(buffer->buffer, buffer->offset + 1);
        if (printed == NULL) {
            return NULL;
        }
        buffer->buffer = NULL;

        return printed;
    }

    /* otherwise copy the JSON over to a new buffer */
    printed = (unsigned char *)hooks->allocate(buffer->offset + 1);
    if (printed == NULL) {
        return NULL;
    }
    memcpy(printed, buffer->buffer, cjson_min(buffer->length, buffer->offset + 1));
    printed[buffer->offset] = '\0'; /* just to be sure */

    /* free the buffer */
    hooks->deallocate(buffer->buffer);
    buffer->buffer = NULL;

    return printed;
}

static unsigned char *print(const cJSON *const item, cJSON_bool format,
                            const internal_hooks *const hooks)
{
    static const size_t default_buffer_size = 256;
    printbuffer buffer[1];
    unsigned char *printed = NULL;

    memset(buffer, 0, sizeof(buffer));

    /* create buffer */
    buffer->buffer = (unsigned char *)hooks->allocate(default_buffer_size);
    buffer->length = default_buffer_size;
    buffer->format = format;
    buffer->hooks = *hooks;

    /* print the value */
    if ((buffer->buffer != NULL) && print_value(item, buffer)) {
        update_offset(buffer);
        printed = detach_printbuffer(buffer, hooks);
    }

    /* the printbuffer only still owns a buffer if printing failed */
    if (buffer->buffer != NULL) {
        hooks->deallocate(buffer->buffer);
        buffer->buffer = NULL;
    }

    return printed;
}

/* Render a cJSON item/entity/structure to text. */
CJSON_PUBLIC(char *) cJSON_Print(const cJSON *item)
{
    return (char *)print(item, true, &global_hooks);
}

CJSON_PUBLIC(char *) cJSON_PrintUnformatted(const cJSON *item)
{
    return (char *)print(item, false, &global_hooks);
}

CJSON_PUBLIC(char *) cJSON_PrintBuffered(const cJSON *item, int prebuffer, cJSON_bool fmt)
{
    printbuffer p = {0, 0, 0, 0, 0, 0, {0, 0, 0}};

    if (prebuffer < 0) {
        return NULL;
    }

    p.buffer = (unsigned char *)global_hooks.allocate((size_t)prebuffer);
    if (!p.buffer) {
        return NULL;
    }

    p.length = (size_t)prebuffer;
    p.offset = 0;
    p.noalloc = false;
    p.format = fmt;
    p.hooks = global_hooks;

    if (!print_value(item, &p)) {
        global_hooks.deallocate(p.buffer);
        p.buffer = NULL;
        return NULL;
    }

    return (char *)p.buffer;
}

CJSON_PUBLIC(cJSON_bool)
cJSON_PrintPreallocated(cJSON *item, char *buffer, const int length, const cJSON_bool format)
{
    printbuffer p = {0, 0, 0, 0, 0, 0, {0, 0, 0}};

    if ((length < 0) || (buffer == NULL)) {
        return false;
    }

    p.buffer = (unsigned char *)buffer;
    p.length = (size_t)length;
    p.offset = 0;
    p.noalloc = true;
    p.format = format;
    p.hooks = global_hooks;

    return print_value(item, &p);
}

/* Parser core - when encountering text, process appropriately. */
static cJSON_bool parse_value(cJSON *const item, parse_buffer *const input_buffer)
{
    if ((input_buffer == NULL) || (input_buffer->content == NULL)) {
        return false; /* no input */
    }

    /* parse the different types of values */
    /* null */
    if (can_read(input_buffer, 4) &&
        (strncmp((const char *)buffer_at_offset(input_buffer), "null", 4) == 0)) {
        item->type = cJSON_NULL;
        input_buffer->offset += 4;
        return true;
    }
    /* false */
    if (can_read(input_buffer, 5) &&
        (strncmp((const char *)buffer_at_offset(input_buffer), "false", 5) == 0)) {
        item->type = cJSON_False;
        input_buffer->offset += 5;
        return true;
    }
    /* true */
    if (can_read(input_buffer, 4) &&
        (strncmp((const char *)buffer_at_offset(input_buffer), "true", 4) == 0)) {
        item->type = cJSON_True;
        item->valueint = 1;
        input_buffer->offset += 4;
        return true;
    }
    /* string */
    if (can_access_at_index(input_buffer, 0) && (buffer_at_offset(input_buffer)[0] == '\"')) {
        return parse_string(item, input_buffer);
    }
    /* number */
    if (can_access_at_index(input_buffer, 0) && ((buffer_at_offset(input_buffer)[0] == '-') ||
                                                 ((buffer_at_offset(input_buffer)[0] >= '0') &&
                                                  (buffer_at_offset(input_buffer)[0] <= '9')))) {
        return parse_number(item, input_buffer);
    }
    /* array */
    if (can_access_at_index(input_buffer, 0) && (buffer_at_offset(input_buffer)[0] == '[')) {
        return parse_array(item, input_buffer);
    }
    /* object */
    if (can_access_at_index(input_buffer, 0) && (buffer_at_offset(input_buffer)[0] == '{')) {
        return parse_object(item, input_buffer);
    }

    return false;
}

/* print a literal, literal_size counts its terminator */
static cJSON_bool print_literal(printbuffer *const output_buffer, const char *const literal,
                                const size_t literal_size)
{
    unsigned char *output = ensure(output_buffer, literal_size);
    if (output == NULL) {
        return false;
    }
    memcpy(output, literal, literal_size);

    return true;
}

/* print the raw JSON an item holds */
static cJSON_bool print_raw(const cJSON *const item, printbuffer *const output_buffer)
{
    unsigned char *output = NULL;
    size_t raw_length = 0;

    if (item->valuestring == NULL) {
        return false;
    }

    raw_length = strlen(item->valuestring) + sizeof("");
    output = ensure(output_buffer, raw_length);
    if (output == NULL) {
        return false;
    }
    memcpy(output, item->valuestring, raw_length);

    return true;
}

/* Render a value to text. */
static cJSON_bool print_value(const cJSON *const item, printbuffer *const output_buffer)
{
    if ((item == NULL) || (output_buffer == NULL)) {
        return false;
    }

    switch ((item->type) & 0xFF) {
    case cJSON_NULL:
        return print_literal(output_buffer, "null", sizeof("null"));

    case cJSON_False:
        return print_literal(output_buffer, "false", sizeof("false"));

    case cJSON_True:
        return print_literal(output_buffer, "true", sizeof("true"));

    case cJSON_Number:
        return print_number(item, output_buffer);

    case cJSON_Raw:
        return print_raw(item, output_buffer);

    case cJSON_String:
        return print_string(item, output_buffer);

    case cJSON_Array:
        return print_array(item, output_buffer);

    case cJSON_Object:
        return print_object(item, output_buffer);

    default:
        return false;
    }
}

/* allocate an item and append it to the list that is being parsed, returns the new tail */
static cJSON *append_new_item(cJSON **const head, cJSON *const current_item,
                              const internal_hooks *const hooks)
{
    cJSON *new_item = cJSON_New_Item(hooks);
    if (new_item == NULL) {
        return NULL; /* allocation failure */
    }

    /* attach next item to list */
    if (*head == NULL) {
        /* start the linked list */
        *head = new_item;
    } else {
        /* add to the end and advance */
        current_item->next = new_item;
        new_item->prev = current_item;
    }

    return new_item;
}

/* parse the comma separated elements of a non-empty array up to its closing bracket */
static cJSON_bool parse_array_elements(parse_buffer *const input_buffer, cJSON **const head,
                                       cJSON **const tail)
{
    /* step back to character in front of the first element */
    input_buffer->offset--;
    /* loop through the comma separated array elements */
    do {
        /* allocate next item */
        cJSON *new_item = append_new_item(head, *tail, &(input_buffer->hooks));
        if (new_item == NULL) {
            return false; /* allocation failure */
        }
        *tail = new_item;

        /* parse next value */
        input_buffer->offset++;
        buffer_skip_whitespace(input_buffer);
        if (!parse_value(new_item, input_buffer)) {
            return false; /* failed to parse value */
        }
        buffer_skip_whitespace(input_buffer);
    } while (can_access_at_index(input_buffer, 0) && (buffer_at_offset(input_buffer)[0] == ','));

    /* expected end of array */
    return can_access_at_index(input_buffer, 0) && (buffer_at_offset(input_buffer)[0] == ']');
}

/* Build an array from input text. */
static cJSON_bool parse_array(cJSON *const item, parse_buffer *const input_buffer)
{
    cJSON *head = NULL; /* head of the linked list */
    cJSON *current_item = NULL;
    cJSON_bool parsed = false;

    if (input_buffer == NULL) {
        return false; /* no input */
    }

    if (input_buffer->depth >= CJSON_NESTING_LIMIT) {
        return false; /* to deeply nested */
    }
    input_buffer->depth++;

    /* not an array unless it opens with a bracket */
    if (buffer_at_offset(input_buffer)[0] == '[') {
        input_buffer->offset++;
        buffer_skip_whitespace(input_buffer);
        if (can_access_at_index(input_buffer, 0) && (buffer_at_offset(input_buffer)[0] == ']')) {
            /* empty array */
            parsed = true;
        } else if (cannot_access_at_index(input_buffer, 0)) {
            /* we skipped to the end of the buffer */
            input_buffer->offset--;
        } else {
            parsed = parse_array_elements(input_buffer, &head, &current_item);
        }
    }

    if (!parsed) {
        if (head != NULL) {
            cJSON_Delete(head);
        }

        return false;
    }

    input_buffer->depth--;

    if (head != NULL) {
        head->prev = current_item;
    }

    item->type = cJSON_Array;
    item->child = head;

    input_buffer->offset++;

    return true;
}

/* Render an array to text */
static cJSON_bool print_array(const cJSON *const item, printbuffer *const output_buffer)
{
    unsigned char *output_pointer = NULL;
    size_t length = 0;
    cJSON *current_element = item->child;

    if (output_buffer == NULL) {
        return false;
    }

    /* Compose the output array. */
    /* opening square bracket */
    output_pointer = ensure(output_buffer, 1);
    if (output_pointer == NULL) {
        return false;
    }

    *output_pointer = '[';
    output_buffer->offset++;
    output_buffer->depth++;

    while (current_element != NULL) {
        if (!print_value(current_element, output_buffer)) {
            return false;
        }
        update_offset(output_buffer);
        if (current_element->next) {
            length = (size_t)(output_buffer->format ? 2 : 1);
            output_pointer = ensure(output_buffer, length + 1);
            if (output_pointer == NULL) {
                return false;
            }
            *output_pointer++ = ',';
            if (output_buffer->format) {
                *output_pointer++ = ' ';
            }
            *output_pointer = '\0';
            output_buffer->offset += length;
        }
        current_element = current_element->next;
    }

    output_pointer = ensure(output_buffer, 2);
    if (output_pointer == NULL) {
        return false;
    }
    *output_pointer++ = ']';
    *output_pointer = '\0';
    output_buffer->depth--;

    return true;
}

/* parse the name and the value of one object member into current_item */
static cJSON_bool parse_object_member(cJSON *const current_item, parse_buffer *const input_buffer)
{
    if (cannot_access_at_index(input_buffer, 1)) {
        return false; /* nothing comes after the comma */
    }

    /* parse the name of the child */
    input_buffer->offset++;
    buffer_skip_whitespace(input_buffer);
    if (!parse_string(current_item, input_buffer)) {
        return false; /* failed to parse name */
    }
    buffer_skip_whitespace(input_buffer);

    /* swap valuestring and string, because we parsed the name */
    current_item->string = current_item->valuestring;
    current_item->valuestring = NULL;

    if (cannot_access_at_index(input_buffer, 0) || (buffer_at_offset(input_buffer)[0] != ':')) {
        return false; /* invalid object */
    }

    /* parse the value */
    input_buffer->offset++;
    buffer_skip_whitespace(input_buffer);
    if (!parse_value(current_item, input_buffer)) {
        return false; /* failed to parse value */
    }
    buffer_skip_whitespace(input_buffer);

    return true;
}

/* parse the comma separated members of a non-empty object up to its closing brace */
static cJSON_bool parse_object_members(parse_buffer *const input_buffer, cJSON **const head,
                                       cJSON **const tail)
{
    /* step back to character in front of the first element */
    input_buffer->offset--;
    /* loop through the comma separated array elements */
    do {
        /* allocate next item */
        cJSON *new_item = append_new_item(head, *tail, &(input_buffer->hooks));
        if (new_item == NULL) {
            return false; /* allocation failure */
        }
        *tail = new_item;

        if (!parse_object_member(new_item, input_buffer)) {
            return false;
        }
    } while (can_access_at_index(input_buffer, 0) && (buffer_at_offset(input_buffer)[0] == ','));

    /* expected end of object */
    return can_access_at_index(input_buffer, 0) && (buffer_at_offset(input_buffer)[0] == '}');
}

/* Build an object from the text. */
static cJSON_bool parse_object(cJSON *const item, parse_buffer *const input_buffer)
{
    cJSON *head = NULL; /* linked list head */
    cJSON *current_item = NULL;
    cJSON_bool parsed = false;

    if (input_buffer == NULL) {
        return false; /* no input */
    }

    if (input_buffer->depth >= CJSON_NESTING_LIMIT) {
        return false; /* to deeply nested */
    }
    input_buffer->depth++;

    /* not an object unless it opens with a brace */
    if (can_access_at_index(input_buffer, 0) && (buffer_at_offset(input_buffer)[0] == '{')) {
        input_buffer->offset++;
        buffer_skip_whitespace(input_buffer);
        if (can_access_at_index(input_buffer, 0) && (buffer_at_offset(input_buffer)[0] == '}')) {
            /* empty object */
            parsed = true;
        } else if (cannot_access_at_index(input_buffer, 0)) {
            /* we skipped to the end of the buffer */
            input_buffer->offset--;
        } else {
            parsed = parse_object_members(input_buffer, &head, &current_item);
        }
    }

    if (!parsed) {
        if (head != NULL) {
            cJSON_Delete(head);
        }

        return false;
    }

    input_buffer->depth--;

    if (head != NULL) {
        head->prev = current_item;
    }

    item->type = cJSON_Object;
    item->child = head;

    input_buffer->offset++;
    return true;
}

/* print one member of an object: indentation, key, separator, value and the trailing comma */
static cJSON_bool print_object_member(const cJSON *const current_item,
                                      printbuffer *const output_buffer)
{
    unsigned char *output_pointer = NULL;
    size_t length = 0;

    if (output_buffer->format) {
        size_t i;
        output_pointer = ensure(output_buffer, output_buffer->depth);
        if (output_pointer == NULL) {
            return false;
        }
        for (i = 0; i < output_buffer->depth; i++) {
            *output_pointer++ = '\t';
        }
        output_buffer->offset += output_buffer->depth;
    }

    /* print key */
    if (!print_string_ptr((unsigned char *)current_item->string, output_buffer)) {
        return false;
    }
    update_offset(output_buffer);

    length = (size_t)(output_buffer->format ? 2 : 1);
    output_pointer = ensure(output_buffer, length);
    if (output_pointer == NULL) {
        return false;
    }
    *output_pointer++ = ':';
    if (output_buffer->format) {
        *output_pointer++ = '\t';
    }
    output_buffer->offset += length;

    /* print value */
    if (!print_value(current_item, output_buffer)) {
        return false;
    }
    update_offset(output_buffer);

    /* print comma if not last */
    length = ((size_t)(output_buffer->format ? 1 : 0) + (size_t)(current_item->next ? 1 : 0));
    output_pointer = ensure(output_buffer, length + 1);
    if (output_pointer == NULL) {
        return false;
    }
    if (current_item->next) {
        *output_pointer++ = ',';
    }

    if (output_buffer->format) {
        *output_pointer++ = '\n';
    }
    *output_pointer = '\0';
    output_buffer->offset += length;

    return true;
}

/* Render an object to text. */
static cJSON_bool print_object(const cJSON *const item, printbuffer *const output_buffer)
{
    unsigned char *output_pointer = NULL;
    size_t length = 0;
    cJSON *current_item = item->child;

    if (output_buffer == NULL) {
        return false;
    }

    /* Compose the output: */
    length = (size_t)(output_buffer->format ? 2 : 1); /* fmt: {\n */
    output_pointer = ensure(output_buffer, length + 1);
    if (output_pointer == NULL) {
        return false;
    }

    *output_pointer++ = '{';
    output_buffer->depth++;
    if (output_buffer->format) {
        *output_pointer++ = '\n';
    }
    output_buffer->offset += length;

    while (current_item) {
        if (!print_object_member(current_item, output_buffer)) {
            return false;
        }

        current_item = current_item->next;
    }

    output_pointer = ensure(output_buffer, output_buffer->format ? (output_buffer->depth + 1) : 2);
    if (output_pointer == NULL) {
        return false;
    }
    if (output_buffer->format) {
        size_t i;
        for (i = 0; i < (output_buffer->depth - 1); i++) {
            *output_pointer++ = '\t';
        }
    }
    *output_pointer++ = '}';
    *output_pointer = '\0';
    output_buffer->depth--;

    return true;
}

/* Get Array size/item / object item. */
CJSON_PUBLIC(int) cJSON_GetArraySize(const cJSON *array)
{
    cJSON *child = NULL;
    size_t size = 0;

    if (array == NULL) {
        return 0;
    }

    child = array->child;

    while (child != NULL) {
        size++;
        child = child->next;
    }

    /* The API returns int, so saturate instead of wrapping to a negative size (CERT INT31-C),
     * like ensure() does for buffer sizes. ADR-1061. */
    if (size > (size_t)INT_MAX) {
        return INT_MAX;
    }

    return (int)size;
}

static cJSON *get_array_item(const cJSON *array, size_t index)
{
    cJSON *current_child = NULL;

    if (array == NULL) {
        return NULL;
    }

    current_child = array->child;
    while ((current_child != NULL) && (index > 0)) {
        index--;
        current_child = current_child->next;
    }

    return current_child;
}

CJSON_PUBLIC(cJSON *) cJSON_GetArrayItem(const cJSON *array, int index)
{
    if (index < 0) {
        return NULL;
    }

    return get_array_item(array, (size_t)index);
}

static cJSON *get_object_item(const cJSON *const object, const char *const name,
                              const cJSON_bool case_sensitive)
{
    cJSON *current_element = NULL;

    if ((object == NULL) || (name == NULL)) {
        return NULL;
    }

    current_element = object->child;
    if (case_sensitive) {
        while ((current_element != NULL) && (current_element->string != NULL) &&
               (strcmp(name, current_element->string) != 0)) {
            current_element = current_element->next;
        }
    } else {
        while ((current_element != NULL) &&
               (case_insensitive_strcmp((const unsigned char *)name,
                                        (const unsigned char *)(current_element->string)) != 0)) {
            current_element = current_element->next;
        }
    }

    if ((current_element == NULL) || (current_element->string == NULL)) {
        return NULL;
    }

    return current_element;
}

CJSON_PUBLIC(cJSON *) cJSON_GetObjectItem(const cJSON *const object, const char *const string)
{
    return get_object_item(object, string, false);
}

CJSON_PUBLIC(cJSON *)
cJSON_GetObjectItemCaseSensitive(const cJSON *const object, const char *const string)
{
    return get_object_item(object, string, true);
}

CJSON_PUBLIC(cJSON_bool) cJSON_HasObjectItem(const cJSON *object, const char *string)
{
    return cJSON_GetObjectItem(object, string) ? 1 : 0;
}

/* Utility for array list handling. */
static void suffix_object(cJSON *prev, cJSON *item)
{
    prev->next = item;
    item->prev = prev;
}

/* Utility for handling references. */
static cJSON *create_reference(const cJSON *item, const internal_hooks *const hooks)
{
    cJSON *reference = NULL;
    if (item == NULL) {
        return NULL;
    }

    reference = cJSON_New_Item(hooks);
    if (reference == NULL) {
        return NULL;
    }

    memcpy(reference, item, sizeof(cJSON));
    reference->string = NULL;
    reference->type |= cJSON_IsReference;
    reference->next = reference->prev = NULL;
    return reference;
}

static cJSON_bool add_item_to_array(cJSON *array, cJSON *item)
{
    cJSON *child = NULL;

    if ((item == NULL) || (array == NULL) || (array == item)) {
        return false;
    }

    child = array->child;
    /*
     * To find the last item in array quickly, we use prev in array
     */
    if (child == NULL) {
        /* list is empty, start new one */
        array->child = item;
        item->prev = item;
        item->next = NULL;
    } else {
        /* append to the end */
        if (child->prev) {
            suffix_object(child->prev, item);
            array->child->prev = item;
        }
    }

    return true;
}

/* Add item to array/object. */
CJSON_PUBLIC(cJSON_bool) cJSON_AddItemToArray(cJSON *array, cJSON *item)
{
    return add_item_to_array(array, item);
}

#if defined(__clang__) ||                                                                          \
    (defined(__GNUC__) && ((__GNUC__ > 4) || ((__GNUC__ == 4) && (__GNUC_MINOR__ > 5))))
#pragma GCC diagnostic push
#endif
#ifdef __GNUC__
#pragma GCC diagnostic ignored "-Wcast-qual"
#endif
/* helper function to cast away const */
static void *cast_away_const(const void *string)
{
    return (void *)string;
}
#if defined(__clang__) ||                                                                          \
    (defined(__GNUC__) && ((__GNUC__ > 4) || ((__GNUC__ == 4) && (__GNUC_MINOR__ > 5))))
#pragma GCC diagnostic pop
#endif

static cJSON_bool add_item_to_object(cJSON *const object, const char *const string,
                                     cJSON *const item, const internal_hooks *const hooks,
                                     const cJSON_bool constant_key)
{
    char *new_key = NULL;
    int new_type = cJSON_Invalid;

    if ((object == NULL) || (string == NULL) || (item == NULL) || (object == item)) {
        return false;
    }

    if (constant_key) {
        new_key = (char *)cast_away_const(string);
        new_type = item->type | cJSON_StringIsConst;
    } else {
        new_key = (char *)cJSON_strdup((const unsigned char *)string, hooks);
        if (new_key == NULL) {
            return false;
        }

        new_type = item->type & ~cJSON_StringIsConst;
    }

    if (!(item->type & cJSON_StringIsConst) && (item->string != NULL)) {
        hooks->deallocate(item->string);
    }

    item->string = new_key;
    item->type = new_type;

    return add_item_to_array(object, item);
}

CJSON_PUBLIC(cJSON_bool) cJSON_AddItemToObject(cJSON *object, const char *string, cJSON *item)
{
    return add_item_to_object(object, string, item, &global_hooks, false);
}

/* Add an item to an object with constant string as key */
CJSON_PUBLIC(cJSON_bool) cJSON_AddItemToObjectCS(cJSON *object, const char *string, cJSON *item)
{
    return add_item_to_object(object, string, item, &global_hooks, true);
}

CJSON_PUBLIC(cJSON_bool) cJSON_AddItemReferenceToArray(cJSON *array, cJSON *item)
{
    if (array == NULL) {
        return false;
    }

    return add_item_to_array(array, create_reference(item, &global_hooks));
}

CJSON_PUBLIC(cJSON_bool)
cJSON_AddItemReferenceToObject(cJSON *object, const char *string, cJSON *item)
{
    if ((object == NULL) || (string == NULL)) {
        return false;
    }

    return add_item_to_object(object, string, create_reference(item, &global_hooks), &global_hooks,
                              false);
}

CJSON_PUBLIC(cJSON *) cJSON_AddNullToObject(cJSON *const object, const char *const name)
{
    cJSON *null = cJSON_CreateNull();
    if (add_item_to_object(object, name, null, &global_hooks, false)) {
        return null;
    }

    cJSON_Delete(null);
    return NULL;
}

CJSON_PUBLIC(cJSON *) cJSON_AddTrueToObject(cJSON *const object, const char *const name)
{
    cJSON *true_item = cJSON_CreateTrue();
    if (add_item_to_object(object, name, true_item, &global_hooks, false)) {
        return true_item;
    }

    cJSON_Delete(true_item);
    return NULL;
}

CJSON_PUBLIC(cJSON *) cJSON_AddFalseToObject(cJSON *const object, const char *const name)
{
    cJSON *false_item = cJSON_CreateFalse();
    if (add_item_to_object(object, name, false_item, &global_hooks, false)) {
        return false_item;
    }

    cJSON_Delete(false_item);
    return NULL;
}

CJSON_PUBLIC(cJSON *)
cJSON_AddBoolToObject(cJSON *const object, const char *const name, const cJSON_bool boolean)
{
    cJSON *bool_item = cJSON_CreateBool(boolean);
    if (add_item_to_object(object, name, bool_item, &global_hooks, false)) {
        return bool_item;
    }

    cJSON_Delete(bool_item);
    return NULL;
}

CJSON_PUBLIC(cJSON *)
cJSON_AddNumberToObject(cJSON *const object, const char *const name, const double number)
{
    cJSON *number_item = cJSON_CreateNumber(number);
    if (add_item_to_object(object, name, number_item, &global_hooks, false)) {
        return number_item;
    }

    cJSON_Delete(number_item);
    return NULL;
}

CJSON_PUBLIC(cJSON *)
cJSON_AddStringToObject(cJSON *const object, const char *const name, const char *const string)
{
    cJSON *string_item = cJSON_CreateString(string);
    if (add_item_to_object(object, name, string_item, &global_hooks, false)) {
        return string_item;
    }

    cJSON_Delete(string_item);
    return NULL;
}

CJSON_PUBLIC(cJSON *)
cJSON_AddRawToObject(cJSON *const object, const char *const name, const char *const raw)
{
    cJSON *raw_item = cJSON_CreateRaw(raw);
    if (add_item_to_object(object, name, raw_item, &global_hooks, false)) {
        return raw_item;
    }

    cJSON_Delete(raw_item);
    return NULL;
}

CJSON_PUBLIC(cJSON *) cJSON_AddObjectToObject(cJSON *const object, const char *const name)
{
    cJSON *object_item = cJSON_CreateObject();
    if (add_item_to_object(object, name, object_item, &global_hooks, false)) {
        return object_item;
    }

    cJSON_Delete(object_item);
    return NULL;
}

CJSON_PUBLIC(cJSON *) cJSON_AddArrayToObject(cJSON *const object, const char *const name)
{
    cJSON *array = cJSON_CreateArray();
    if (add_item_to_object(object, name, array, &global_hooks, false)) {
        return array;
    }

    cJSON_Delete(array);
    return NULL;
}

CJSON_PUBLIC(cJSON *) cJSON_DetachItemViaPointer(cJSON *parent, cJSON *const item)
{
    if ((parent == NULL) || (item == NULL) || (item != parent->child && item->prev == NULL)) {
        return NULL;
    }

    if (item != parent->child) {
        /* not the first element */
        item->prev->next = item->next;
    }
    if (item->next != NULL) {
        /* not the last element */
        item->next->prev = item->prev;
    }

    if (item == parent->child) {
        /* first element */
        parent->child = item->next;
    } else if (item->next == NULL) {
        /* last element */
        parent->child->prev = item->prev;
    }

    /* make sure the detached item doesn't point anywhere anymore */
    item->prev = NULL;
    item->next = NULL;

    return item;
}

CJSON_PUBLIC(cJSON *) cJSON_DetachItemFromArray(cJSON *array, int which)
{
    if (which < 0) {
        return NULL;
    }

    return cJSON_DetachItemViaPointer(array, get_array_item(array, (size_t)which));
}

CJSON_PUBLIC(void) cJSON_DeleteItemFromArray(cJSON *array, int which)
{
    cJSON_Delete(cJSON_DetachItemFromArray(array, which));
}

CJSON_PUBLIC(cJSON *) cJSON_DetachItemFromObject(cJSON *object, const char *string)
{
    cJSON *to_detach = cJSON_GetObjectItem(object, string);

    return cJSON_DetachItemViaPointer(object, to_detach);
}

CJSON_PUBLIC(cJSON *) cJSON_DetachItemFromObjectCaseSensitive(cJSON *object, const char *string)
{
    cJSON *to_detach = cJSON_GetObjectItemCaseSensitive(object, string);

    return cJSON_DetachItemViaPointer(object, to_detach);
}

CJSON_PUBLIC(void) cJSON_DeleteItemFromObject(cJSON *object, const char *string)
{
    cJSON_Delete(cJSON_DetachItemFromObject(object, string));
}

CJSON_PUBLIC(void) cJSON_DeleteItemFromObjectCaseSensitive(cJSON *object, const char *string)
{
    cJSON_Delete(cJSON_DetachItemFromObjectCaseSensitive(object, string));
}

/* Replace array/object items with new ones. */
CJSON_PUBLIC(cJSON_bool) cJSON_InsertItemInArray(cJSON *array, int which, cJSON *newitem)
{
    cJSON *after_inserted = NULL;

    if (which < 0 || newitem == NULL) {
        return false;
    }

    after_inserted = get_array_item(array, (size_t)which);
    if (after_inserted == NULL) {
        return add_item_to_array(array, newitem);
    }

    if (after_inserted != array->child && after_inserted->prev == NULL) {
        /* return false if after_inserted is a corrupted array item */
        return false;
    }

    newitem->next = after_inserted;
    newitem->prev = after_inserted->prev;
    after_inserted->prev = newitem;
    if (after_inserted == array->child) {
        array->child = newitem;
    } else {
        newitem->prev->next = newitem;
    }
    return true;
}

CJSON_PUBLIC(cJSON_bool)
cJSON_ReplaceItemViaPointer(cJSON *const parent, cJSON *const item, cJSON *replacement)
{
    if ((parent == NULL) || (parent->child == NULL) || (replacement == NULL) || (item == NULL)) {
        return false;
    }

    if (replacement == item) {
        return true;
    }

    replacement->next = item->next;
    replacement->prev = item->prev;

    if (replacement->next != NULL) {
        replacement->next->prev = replacement;
    }
    if (parent->child == item) {
        if (parent->child->prev == parent->child) {
            replacement->prev = replacement;
        }
        parent->child = replacement;
    } else { /*
         * To find the last item in array quickly, we use prev in array.
         * We can't modify the last item's next pointer where this item was the parent's child
         */
        if (replacement->prev != NULL) {
            replacement->prev->next = replacement;
        }
        if (replacement->next == NULL) {
            parent->child->prev = replacement;
        }
    }

    item->next = NULL;
    item->prev = NULL;
    cJSON_Delete(item);

    return true;
}

CJSON_PUBLIC(cJSON_bool) cJSON_ReplaceItemInArray(cJSON *array, int which, cJSON *newitem)
{
    if (which < 0) {
        return false;
    }

    return cJSON_ReplaceItemViaPointer(array, get_array_item(array, (size_t)which), newitem);
}

static cJSON_bool replace_item_in_object(cJSON *object, const char *string, cJSON *replacement,
                                         cJSON_bool case_sensitive)
{
    if ((replacement == NULL) || (string == NULL)) {
        return false;
    }

    /* replace the name in the replacement */
    if (!(replacement->type & cJSON_StringIsConst) && (replacement->string != NULL)) {
        cJSON_free(replacement->string);
    }
    replacement->string = (char *)cJSON_strdup((const unsigned char *)string, &global_hooks);
    if (replacement->string == NULL) {
        return false;
    }

    replacement->type &= ~cJSON_StringIsConst;

    return cJSON_ReplaceItemViaPointer(object, get_object_item(object, string, case_sensitive),
                                       replacement);
}

CJSON_PUBLIC(cJSON_bool)
cJSON_ReplaceItemInObject(cJSON *object, const char *string, cJSON *newitem)
{
    return replace_item_in_object(object, string, newitem, false);
}

CJSON_PUBLIC(cJSON_bool)
cJSON_ReplaceItemInObjectCaseSensitive(cJSON *object, const char *string, cJSON *newitem)
{
    return replace_item_in_object(object, string, newitem, true);
}

/* Create basic types: */
CJSON_PUBLIC(cJSON *) cJSON_CreateNull(void)
{
    cJSON *item = cJSON_New_Item(&global_hooks);
    if (item) {
        item->type = cJSON_NULL;
    }

    return item;
}

CJSON_PUBLIC(cJSON *) cJSON_CreateTrue(void)
{
    cJSON *item = cJSON_New_Item(&global_hooks);
    if (item) {
        item->type = cJSON_True;
    }

    return item;
}

CJSON_PUBLIC(cJSON *) cJSON_CreateFalse(void)
{
    cJSON *item = cJSON_New_Item(&global_hooks);
    if (item) {
        item->type = cJSON_False;
    }

    return item;
}

CJSON_PUBLIC(cJSON *) cJSON_CreateBool(cJSON_bool boolean)
{
    cJSON *item = cJSON_New_Item(&global_hooks);
    if (item) {
        item->type = boolean ? cJSON_True : cJSON_False;
    }

    return item;
}

CJSON_PUBLIC(cJSON *) cJSON_CreateNumber(double num)
{
    cJSON *item = cJSON_New_Item(&global_hooks);
    if (item) {
        item->type = cJSON_Number;
        item->valuedouble = num;
        item->valueint = saturate_to_int(num);
    }

    return item;
}

CJSON_PUBLIC(cJSON *) cJSON_CreateString(const char *string)
{
    cJSON *item = cJSON_New_Item(&global_hooks);
    if (item) {
        item->type = cJSON_String;
        item->valuestring = (char *)cJSON_strdup((const unsigned char *)string, &global_hooks);
        if (!item->valuestring) {
            cJSON_Delete(item);
            return NULL;
        }
    }

    return item;
}

CJSON_PUBLIC(cJSON *) cJSON_CreateStringReference(const char *string)
{
    cJSON *item = cJSON_New_Item(&global_hooks);
    if (item != NULL) {
        item->type = cJSON_String | cJSON_IsReference;
        item->valuestring = (char *)cast_away_const(string);
    }

    return item;
}

CJSON_PUBLIC(cJSON *) cJSON_CreateObjectReference(const cJSON *child)
{
    cJSON *item = cJSON_New_Item(&global_hooks);
    if (item != NULL) {
        item->type = cJSON_Object | cJSON_IsReference;
        item->child = (cJSON *)cast_away_const(child);
    }

    return item;
}

CJSON_PUBLIC(cJSON *) cJSON_CreateArrayReference(const cJSON *child)
{
    cJSON *item = cJSON_New_Item(&global_hooks);
    if (item != NULL) {
        item->type = cJSON_Array | cJSON_IsReference;
        item->child = (cJSON *)cast_away_const(child);
    }

    return item;
}

CJSON_PUBLIC(cJSON *) cJSON_CreateRaw(const char *raw)
{
    cJSON *item = cJSON_New_Item(&global_hooks);
    if (item) {
        item->type = cJSON_Raw;
        item->valuestring = (char *)cJSON_strdup((const unsigned char *)raw, &global_hooks);
        if (!item->valuestring) {
            cJSON_Delete(item);
            return NULL;
        }
    }

    return item;
}

CJSON_PUBLIC(cJSON *) cJSON_CreateArray(void)
{
    cJSON *item = cJSON_New_Item(&global_hooks);
    if (item) {
        item->type = cJSON_Array;
    }

    return item;
}

CJSON_PUBLIC(cJSON *) cJSON_CreateObject(void)
{
    cJSON *item = cJSON_New_Item(&global_hooks);
    if (item) {
        item->type = cJSON_Object;
    }

    return item;
}

/* Create Arrays: */
CJSON_PUBLIC(cJSON *) cJSON_CreateIntArray(const int *numbers, int count)
{
    size_t i = 0;
    cJSON *n = NULL;
    cJSON *p = NULL;
    cJSON *a = NULL;

    if ((count < 0) || (numbers == NULL)) {
        return NULL;
    }

    a = cJSON_CreateArray();

    for (i = 0; a && (i < (size_t)count); i++) {
        n = cJSON_CreateNumber(numbers[i]);
        if (!n) {
            cJSON_Delete(a);
            return NULL;
        }
        if (!i) {
            a->child = n;
        } else {
            suffix_object(p, n);
        }
        p = n;
    }

    if (a && a->child) {
        a->child->prev = n;
    }

    return a;
}

CJSON_PUBLIC(cJSON *) cJSON_CreateFloatArray(const float *numbers, int count)
{
    size_t i = 0;
    cJSON *n = NULL;
    cJSON *p = NULL;
    cJSON *a = NULL;

    if ((count < 0) || (numbers == NULL)) {
        return NULL;
    }

    a = cJSON_CreateArray();

    for (i = 0; a && (i < (size_t)count); i++) {
        n = cJSON_CreateNumber((double)numbers[i]);
        if (!n) {
            cJSON_Delete(a);
            return NULL;
        }
        if (!i) {
            a->child = n;
        } else {
            suffix_object(p, n);
        }
        p = n;
    }

    if (a && a->child) {
        a->child->prev = n;
    }

    return a;
}

CJSON_PUBLIC(cJSON *) cJSON_CreateDoubleArray(const double *numbers, int count)
{
    size_t i = 0;
    cJSON *n = NULL;
    cJSON *p = NULL;
    cJSON *a = NULL;

    if ((count < 0) || (numbers == NULL)) {
        return NULL;
    }

    a = cJSON_CreateArray();

    for (i = 0; a && (i < (size_t)count); i++) {
        n = cJSON_CreateNumber(numbers[i]);
        if (!n) {
            cJSON_Delete(a);
            return NULL;
        }
        if (!i) {
            a->child = n;
        } else {
            suffix_object(p, n);
        }
        p = n;
    }

    if (a && a->child) {
        a->child->prev = n;
    }

    return a;
}

CJSON_PUBLIC(cJSON *) cJSON_CreateStringArray(const char *const *strings, int count)
{
    size_t i = 0;
    cJSON *n = NULL;
    cJSON *p = NULL;
    cJSON *a = NULL;

    if ((count < 0) || (strings == NULL)) {
        return NULL;
    }

    a = cJSON_CreateArray();

    for (i = 0; a && (i < (size_t)count); i++) {
        n = cJSON_CreateString(strings[i]);
        if (!n) {
            cJSON_Delete(a);
            return NULL;
        }
        if (!i) {
            a->child = n;
        } else {
            suffix_object(p, n);
        }
        p = n;
    }

    if (a && a->child) {
        a->child->prev = n;
    }

    return a;
}

/* Duplication */
static cJSON *cJSON_Duplicate_rec(const cJSON *item, size_t depth, cJSON_bool recurse);

CJSON_PUBLIC(cJSON *) cJSON_Duplicate(const cJSON *item, cJSON_bool recurse)
{
    return cJSON_Duplicate_rec(item, 0, recurse);
}

/* duplicate an item without its children */
static cJSON *duplicate_without_children(const cJSON *const item)
{
    /* Create new item */
    cJSON *newitem = cJSON_New_Item(&global_hooks);
    if (!newitem) {
        return NULL;
    }
    /* Copy over all vars */
    newitem->type = item->type & (~cJSON_IsReference);
    newitem->valueint = item->valueint;
    newitem->valuedouble = item->valuedouble;
    if (item->valuestring) {
        newitem->valuestring =
            (char *)cJSON_strdup((unsigned char *)item->valuestring, &global_hooks);
        if (!newitem->valuestring) {
            cJSON_Delete(newitem);
            return NULL;
        }
    }
    if (item->string) {
        newitem->string = (item->type & cJSON_StringIsConst) ?
                              item->string :
                              (char *)cJSON_strdup((unsigned char *)item->string, &global_hooks);
        if (!newitem->string) {
            cJSON_Delete(newitem);
            return NULL;
        }
    }

    return newitem;
}

/* duplicate the children of item into newitem */
static cJSON_bool duplicate_children(const cJSON *const item, cJSON *const newitem,
                                     const size_t depth)
{
    const cJSON *child = item->child;
    cJSON *next = NULL;
    cJSON *newchild = NULL;

    /* Walk the ->next chain for the child. */
    while (child != NULL) {
        if (depth >= CJSON_CIRCULAR_LIMIT) {
            return false;
        }
        newchild = cJSON_Duplicate_rec(
            child, depth + 1, true); /* Duplicate (with recurse) each item in the ->next chain */
        if (!newchild) {
            return false;
        }
        if (next != NULL) {
            /* If newitem->child already set, then crosswire ->prev and ->next and move on */
            next->next = newchild;
            newchild->prev = next;
            next = newchild;
        } else {
            /* Set newitem->child and move to it */
            newitem->child = newchild;
            next = newchild;
        }
        child = child->next;
    }
    if (newitem->child) {
        newitem->child->prev = newchild;
    }

    return true;
}

static cJSON *cJSON_Duplicate_rec(const cJSON *item, size_t depth, cJSON_bool recurse)
{
    cJSON *newitem = NULL;

    /* Bail on bad ptr */
    if (!item) {
        return NULL;
    }

    newitem = duplicate_without_children(item);
    /* If non-recursive, then we're done! */
    if ((newitem == NULL) || !recurse) {
        return newitem;
    }

    if (!duplicate_children(item, newitem, depth)) {
        cJSON_Delete(newitem);
        return NULL;
    }

    return newitem;
}

static void skip_oneline_comment(char **input)
{
    *input += static_strlen("//");

    for (; (*input)[0] != '\0'; ++(*input)) {
        if ((*input)[0] == '\n') {
            *input += static_strlen("\n");
            return;
        }
    }
}

static void skip_multiline_comment(char **input)
{
    *input += static_strlen("/*");

    for (; (*input)[0] != '\0'; ++(*input)) {
        if (((*input)[0] == '*') && ((*input)[1] == '/')) {
            *input += static_strlen("*/");
            return;
        }
    }
}

static void minify_string(char **input, char **output)
{
    (*output)[0] = (*input)[0];
    *input += static_strlen("\"");
    *output += static_strlen("\"");

    for (; (*input)[0] != '\0'; (void)++(*input), ++(*output)) {
        (*output)[0] = (*input)[0];

        if ((*input)[0] == '\"') {
            (*output)[0] = '\"';
            *input += static_strlen("\"");
            *output += static_strlen("\"");
            return;
        } else if (((*input)[0] == '\\') && ((*input)[1] == '\"')) {
            (*output)[1] = (*input)[1];
            *input += static_strlen("\"");
            *output += static_strlen("\"");
        }
    }
}

CJSON_PUBLIC(void) cJSON_Minify(char *json)
{
    char *into = json;

    if (json == NULL) {
        return;
    }

    while (json[0] != '\0') {
        switch (json[0]) {
        case ' ':
        case '\t':
        case '\r':
        case '\n':
            json++;
            break;

        case '/':
            if (json[1] == '/') {
                skip_oneline_comment(&json);
            } else if (json[1] == '*') {
                skip_multiline_comment(&json);
            } else {
                json++;
            }
            break;

        case '\"':
            minify_string(&json, (char **)&into);
            break;

        default:
            into[0] = json[0];
            json++;
            into++;
        }
    }

    /* and null-terminate. */
    *into = '\0';
}

CJSON_PUBLIC(cJSON_bool) cJSON_IsInvalid(const cJSON *const item)
{
    if (item == NULL) {
        return false;
    }

    return (item->type & 0xFF) == cJSON_Invalid;
}

CJSON_PUBLIC(cJSON_bool) cJSON_IsFalse(const cJSON *const item)
{
    if (item == NULL) {
        return false;
    }

    return (item->type & 0xFF) == cJSON_False;
}

CJSON_PUBLIC(cJSON_bool) cJSON_IsTrue(const cJSON *const item)
{
    if (item == NULL) {
        return false;
    }

    return (item->type & 0xff) == cJSON_True;
}

CJSON_PUBLIC(cJSON_bool) cJSON_IsBool(const cJSON *const item)
{
    if (item == NULL) {
        return false;
    }

    return (item->type & (cJSON_True | cJSON_False)) != 0;
}
CJSON_PUBLIC(cJSON_bool) cJSON_IsNull(const cJSON *const item)
{
    if (item == NULL) {
        return false;
    }

    return (item->type & 0xFF) == cJSON_NULL;
}

CJSON_PUBLIC(cJSON_bool) cJSON_IsNumber(const cJSON *const item)
{
    if (item == NULL) {
        return false;
    }

    return (item->type & 0xFF) == cJSON_Number;
}

CJSON_PUBLIC(cJSON_bool) cJSON_IsString(const cJSON *const item)
{
    if (item == NULL) {
        return false;
    }

    return (item->type & 0xFF) == cJSON_String;
}

CJSON_PUBLIC(cJSON_bool) cJSON_IsArray(const cJSON *const item)
{
    if (item == NULL) {
        return false;
    }

    return (item->type & 0xFF) == cJSON_Array;
}

CJSON_PUBLIC(cJSON_bool) cJSON_IsObject(const cJSON *const item)
{
    if (item == NULL) {
        return false;
    }

    return (item->type & 0xFF) == cJSON_Object;
}

CJSON_PUBLIC(cJSON_bool) cJSON_IsRaw(const cJSON *const item)
{
    if (item == NULL) {
        return false;
    }

    return (item->type & 0xFF) == cJSON_Raw;
}

/* check if the type of an item is one that can be compared */
static cJSON_bool is_comparable_type(const int type)
{
    switch (type & 0xFF) {
    case cJSON_False:
    case cJSON_True:
    case cJSON_NULL:
    case cJSON_Number:
    case cJSON_String:
    case cJSON_Raw:
    case cJSON_Array:
    case cJSON_Object:
        return true;

    default:
        return false;
    }
}

static cJSON_bool compare_arrays(const cJSON *const a, const cJSON *const b,
                                 const cJSON_bool case_sensitive)
{
    cJSON *a_element = a->child;
    cJSON *b_element = b->child;

    for (; (a_element != NULL) && (b_element != NULL);) {
        if (!cJSON_Compare(a_element, b_element, case_sensitive)) {
            return false;
        }

        a_element = a_element->next;
        b_element = b_element->next;
    }

    /* one of the arrays is longer than the other */
    return a_element == b_element;
}

/* check that every member of a has an equal member of the same name in b */
static cJSON_bool object_is_subset(const cJSON *const a, const cJSON *const b,
                                   const cJSON_bool case_sensitive)
{
    cJSON *a_element = NULL;

    cJSON_ArrayForEach(a_element, a)
    {
        /* TODO This has O(n^2) runtime, which is horrible! */
        const cJSON *b_element = get_object_item(b, a_element->string, case_sensitive);
        if (b_element == NULL) {
            return false;
        }

        if (!cJSON_Compare(a_element, b_element, case_sensitive)) {
            return false;
        }
    }

    return true;
}

CJSON_PUBLIC(cJSON_bool)
cJSON_Compare(const cJSON *const a, const cJSON *const b, const cJSON_bool case_sensitive)
{
    if ((a == NULL) || (b == NULL) || ((a->type & 0xFF) != (b->type & 0xFF))) {
        return false;
    }

    /* check if type is valid */
    if (!is_comparable_type(a->type)) {
        return false;
    }

    /* identical objects are equal */
    if (a == b) {
        return true;
    }

    switch (a->type & 0xFF) {
    /* in these cases and equal type is enough */
    case cJSON_False:
    case cJSON_True:
    case cJSON_NULL:
        return true;

    case cJSON_Number:
        return compare_double(a->valuedouble, b->valuedouble);

    case cJSON_String:
    case cJSON_Raw:
        if ((a->valuestring == NULL) || (b->valuestring == NULL)) {
            return false;
        }

        return strcmp(a->valuestring, b->valuestring) == 0;

    case cJSON_Array:
        return compare_arrays(a, b, case_sensitive);

    case cJSON_Object:
        /* doing this twice, once on a and b to prevent true comparison if a subset of b
         * TODO: Do this the proper way, this is just a fix for now */
        return object_is_subset(a, b, case_sensitive) && object_is_subset(b, a, case_sensitive);

    default:
        return false;
    }
}

CJSON_PUBLIC(void *) cJSON_malloc(size_t size)
{
    return global_hooks.allocate(size);
}

CJSON_PUBLIC(void) cJSON_free(void *object)
{
    global_hooks.deallocate(object);
}

/* NOLINTEND(modernize-use-nullptr) */
