/* Vendored 3rd-party JSON parser (pdjson); fork changes are in docs/rebase-notes.md. */
/* Copyright: pdjson authors (https://github.com/skeeto/pdjson), Unlicense */
/* Fork integration: Copyright 2026 Lusoris */
/* SPDX-License-Identifier: Unlicense */
/* NOLINTBEGIN(modernize-use-nullptr) -- ADR-1138: preserve upstream C NULL
 * compatibility and the required Windows MSVC C build. */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef PDJSON_H
#include "pdjson.h"
#endif

#define JSON_FLAG_ERROR (1u << 0)
#define JSON_FLAG_STREAMING (1u << 1)

/*
 * ADR-1061: cap pdjson nesting depth to prevent unbounded stack/heap growth
 * on adversarially crafted JSON.  512 levels is far beyond any legitimate
 * VMAF model or MCP message structure.
 */
#ifndef PDJSON_STACK_MAX
#define PDJSON_STACK_MAX 512u
#endif

static bool json_begin_error(json_stream *json)
{
    if (json->flags & JSON_FLAG_ERROR)
        return false;
    json->flags |= JSON_FLAG_ERROR;
    return true;
}

/* Diagnostic formatting is best effort after the sticky error flag is set.
 * Keep the upstream formatter (including legacy MSVC), with an expression
 * macro so calling it inside an if/else never captures the caller's else. */
#if defined(_MSC_VER) && (_MSC_VER < 1900)
#define json_error(json, format, ...)                                                              \
    (json_begin_error(json) ? (void)_snprintf_s((json)->errmsg, sizeof((json)->errmsg), _TRUNCATE, \
                                                format, __VA_ARGS__) :                             \
                              (void)0)
#else
#define json_error(json, format, ...)                                                              \
    (json_begin_error(json) ?                                                                      \
         (void)snprintf((json)->errmsg, sizeof((json)->errmsg), format, __VA_ARGS__) :             \
         (void)0)
#endif

/* See also PDJSON_STACK_MAX below. */
#ifndef PDJSON_STACK_INC
#define PDJSON_STACK_INC 4
#endif

struct json_stack {
    enum json_type type;
    long count;
};

static enum json_type push(json_stream *json, enum json_type type)
{
    /* ADR-1061: depth counts containers, while stack_top is a zero-based
     * index. Reject before changing the accepted stack or allocating. */
    const ptrdiff_t next_top = json->stack_top + 1;
    if (next_top >= (ptrdiff_t)PDJSON_STACK_MAX) {
        json_error(json, "%s", "maximum depth of nesting reached");
        return JSON_ERROR;
    }

    if ((size_t)next_top >= json->stack_size) {
        /* Check both the addition and multiplication before either occurs. */
        if (PDJSON_STACK_INC == 0 || PDJSON_STACK_INC > SIZE_MAX / sizeof(*json->stack) ||
            json->stack_size > SIZE_MAX / sizeof(*json->stack) - PDJSON_STACK_INC) {
            json_error(json, "%s", "out of memory");
            return JSON_ERROR;
        }
        const size_t new_count = json->stack_size + PDJSON_STACK_INC;
        const size_t size = new_count * sizeof(*json->stack);
        struct json_stack *stack = json->alloc.realloc(json->stack, size);
        if (stack == NULL) {
            json_error(json, "%s", "out of memory");
            return JSON_ERROR;
        }
        json->stack_size = new_count;
        json->stack = stack;
    }

    json->stack_top = next_top;
    json->stack[next_top].type = type;
    json->stack[next_top].count = 0;
    return type;
}

static enum json_type pop(json_stream *json, int c, enum json_type expected)
{
    if (json->stack == NULL || json->stack[json->stack_top].type != expected) {
        json_error(json, "unexpected byte '%c'", c);
        return JSON_ERROR;
    }
    json->stack_top--;
    return expected == JSON_ARRAY ? JSON_ARRAY_END : JSON_OBJECT_END;
}

static int buffer_peek(struct json_source *source)
{
    if (source->position < source->source.buffer.length)
        return source->source.buffer.buffer[source->position];
    return EOF;
}

static int buffer_get(struct json_source *source)
{
    int c = source->peek(source);
    source->position++;
    return c;
}

static int stream_get(struct json_source *source)
{
    source->position++;
    return fgetc(source->source.stream.stream);
}

static int stream_peek(struct json_source *source)
{
    int c = fgetc(source->source.stream.stream);
    (void)ungetc(c, source->source.stream.stream);
    return c;
}

static void init(json_stream *json)
{
    json->lineno = 1;
    json->flags = JSON_FLAG_STREAMING;
    json->errmsg[0] = '\0';
    json->ntokens = 0;
    json->next = JSON_NONE;

    json->stack = NULL;
    json->stack_top = -1; /* No active container. */
    json->stack_size = 0;

    json->data.string = NULL;
    json->data.string_size = 0;
    json->data.string_fill = 0;
    json->source.position = 0;

    json->alloc.malloc = malloc;
    json->alloc.realloc = realloc;
    json->alloc.free = free;
}

static enum json_type is_match(json_stream *json, const char *pattern, enum json_type type)
{
    for (const char *p = pattern; *p; p++) {
        const int c = json->source.get(&json->source);
        if (*p != c) {
            json_error(json, "expected '%c' instead of byte '%c'", *p, c);
            return JSON_ERROR;
        }
    }
    return type;
}

static int pushchar(json_stream *json, int c)
{
    if (json->data.string_fill == json->data.string_size) {
        /* ADR-1061: guard the x2 doubling against size_t wrap (CERT-C INT30-C).
         * When string_size > SIZE_MAX/2 the multiply would overflow to a value
         * smaller than the current allocation, causing realloc to shrink the
         * buffer and the subsequent write to go out of bounds. */
        if (json->data.string_size > (size_t)-1 / 2u) {
            json_error(json, "%s", "out of memory");
            return -1;
        }
        size_t size = json->data.string_size * 2;
        char *buffer = (char *)json->alloc.realloc(json->data.string, size);
        if (buffer == NULL) {
            json_error(json, "%s", "out of memory");
            return -1;
        } else {
            json->data.string_size = size;
            json->data.string = buffer;
        }
    }
    json->data.string[json->data.string_fill++] = c;
    return 0;
}

static int init_string(json_stream *json)
{
    json->data.string_fill = 0;
    if (json->data.string == NULL) {
        json->data.string_size = 1024;
        json->data.string = (char *)json->alloc.malloc(json->data.string_size);
        if (json->data.string == NULL) {
            json_error(json, "%s", "out of memory");
            return -1;
        }
    }
    json->data.string[0] = '\0';
    return 0;
}

static int encode_utf8(json_stream *json, unsigned long c)
{
    if (c < 0x80UL) {
        return pushchar(json, c);
    } else if (c < 0x0800UL) {
        return !((pushchar(json, (c >> 6 & 0x1F) | 0xC0) == 0) &&
                 (pushchar(json, (c >> 0 & 0x3F) | 0x80) == 0));
    } else if (c < 0x010000UL) {
        if (c >= 0xd800 && c <= 0xdfff) {
            json_error(json, "invalid codepoint %06lx", c);
            return -1;
        }
        return !((pushchar(json, (c >> 12 & 0x0F) | 0xE0) == 0) &&
                 (pushchar(json, (c >> 6 & 0x3F) | 0x80) == 0) &&
                 (pushchar(json, (c >> 0 & 0x3F) | 0x80) == 0));
    } else if (c < 0x110000UL) {
        return !((pushchar(json, (c >> 18 & 0x07) | 0xF0) == 0) &&
                 (pushchar(json, (c >> 12 & 0x3F) | 0x80) == 0) &&
                 (pushchar(json, (c >> 6 & 0x3F) | 0x80) == 0) &&
                 (pushchar(json, (c >> 0 & 0x3F) | 0x80) == 0));
    } else {
        json_error(json, "unable to encode %06lx as UTF-8", c);
        return -1;
    }
}

static int hexchar(int c)
{
    switch (c) {
    case '0':
        return 0;
    case '1':
        return 1;
    case '2':
        return 2;
    case '3':
        return 3;
    case '4':
        return 4;
    case '5':
        return 5;
    case '6':
        return 6;
    case '7':
        return 7;
    case '8':
        return 8;
    case '9':
        return 9;
    case 'a':
    case 'A':
        return 10;
    case 'b':
    case 'B':
        return 11;
    case 'c':
    case 'C':
        return 12;
    case 'd':
    case 'D':
        return 13;
    case 'e':
    case 'E':
        return 14;
    case 'f':
    case 'F':
        return 15;
    default:
        return -1;
    }
}

static long read_unicode_cp(json_stream *json)
{
    long cp = 0;
    int shift = 12;

    for (size_t i = 0; i < 4; i++) {
        const int c = json->source.get(&json->source);
        const int hc = hexchar(c);

        if (c == EOF) {
            json_error(json, "%s", "unterminated string literal in Unicode");
            return -1;
        } else if (hc == -1) {
            json_error(json, "invalid escape Unicode byte '%c'", c);
            return -1;
        }

        cp += (long)hc * (1 << shift);
        shift -= 4;
    }

    return cp;
}

static int read_unicode(json_stream *json)
{
    long cp = read_unicode_cp(json);
    if (cp == -1) {
        return -1;
    }

    if (cp >= 0xd800 && cp <= 0xdbff) {
        /* This is the high portion of a surrogate pair; we need to read the
         * lower portion to get the codepoint
         */
        const long h = cp;

        int c = json->source.get(&json->source);
        if (c == EOF) {
            json_error(json, "%s", "unterminated string literal in Unicode");
            return -1;
        } else if (c != '\\') {
            json_error(json,
                       "invalid continuation for surrogate pair '%c', "
                       "expected '\\'",
                       c);
            return -1;
        }

        c = json->source.get(&json->source);
        if (c == EOF) {
            json_error(json, "%s", "unterminated string literal in Unicode");
            return -1;
        } else if (c != 'u') {
            json_error(json,
                       "invalid continuation for surrogate pair '%c', "
                       "expected 'u'",
                       c);
            return -1;
        }

        const long l = read_unicode_cp(json);
        if (l == -1) {
            return -1;
        }

        if (l < 0xdc00 || l > 0xdfff) {
            json_error(json,
                       "surrogate pair continuation \\u%04lx out "
                       "of range (dc00-dfff)",
                       l);
            return -1;
        }

        cp = ((h - 0xd800) * 0x400) + ((l - 0xdc00) + 0x10000);
    } else if (cp >= 0xdc00 && cp <= 0xdfff) {
        json_error(json, "dangling surrogate \\u%04lx", cp);
        return -1;
    }

    return encode_utf8(json, cp);
}

static int read_escaped(json_stream *json)
{
    int c = json->source.get(&json->source);
    if (c == EOF) {
        json_error(json, "%s", "unterminated string literal in escape");
        return -1;
    } else if (c == 'u') {
        if (read_unicode(json) != 0)
            return -1;
    } else {
        switch (c) {
        case '\\':
        case 'b':
        case 'f':
        case 'n':
        case 'r':
        case 't':
        case '/':
        case '"': {
            const char *codes = "\\bfnrt/\"";
            const char *p = strchr(codes, c);
            if (pushchar(json, "\\\b\f\n\r\t/\""[p - codes]) != 0)
                return -1;
        } break;
        default:
            json_error(json, "invalid escaped byte '%c'", c);
            return -1;
        }
    }
    return 0;
}

static int char_needs_escaping(int c)
{
    if ((c >= 0) && (c < 0x20 || c == 0x22 || c == 0x5c)) {
        return 1;
    }

    return 0;
}

static int utf8_seq_length(char byte)
{
    unsigned char u = (unsigned char)byte;
    if (u < 0x80)
        return 1;

    if (u <= 0xC1 || u > 0xF4) {
        /* Continuation bytes, overlong ASCII, or prefixes beyond Unicode. */
        return 0;
    } else if (u <= 0xDF) {
        /* u >= 0xC2 is guaranteed by the preceding branches; lower bound removed for
         * CodeQL cpp/constant-comparison. */
        // 2-byte sequence
        return 2;
    } else if (u <= 0xEF) {
        /* u >= 0xE0 is guaranteed by the preceding branch (u <= 0xDF);
         * redundant lower bound removed for CodeQL cpp/constant-comparison. */
        // 3-byte sequence
        return 3;
    }
    return 4; /* 0xF0..0xF4: four-byte sequence. */
}

static int is_legal_utf8(const unsigned char *bytes, int length)
{
    if (0 == bytes || 0 == length)
        return 0;

    if (length < 1 || length > 4)
        return 0;
    for (int i = length - 1; i >= 2; --i) {
        if (bytes[i] < 0x80 || bytes[i] > 0xBF)
            return 0;
    }
    if (length >= 2) {
        const unsigned char a = bytes[1];
        switch (*bytes) {
        case 0xE0:
            if (a < 0xA0 || a > 0xBF)
                return 0;
            break;
        case 0xED:
            if (a < 0x80 || a > 0x9F)
                return 0;
            break;
        case 0xF0:
            if (a < 0x90 || a > 0xBF)
                return 0;
            break;
        case 0xF4:
            if (a < 0x80 || a > 0x8F)
                return 0;
            break;
        default:
            if (a < 0x80 || a > 0xBF)
                return 0;
            break;
        }
    }
    if (*bytes >= 0x80 && *bytes < 0xC2)
        return 0;
    return *bytes <= 0xF4;
}

static int read_utf8(json_stream *json, int next_char)
{
    int count = utf8_seq_length(next_char);
    if (!count) {
        json_error(json, "%s", "invalid UTF-8 character");
        return -1;
    }

    char buffer[4];
    buffer[0] = next_char;
    int i;
    for (i = 1; i < count; ++i) {
        buffer[i] = json->source.get(&json->source);
    }

    if (!is_legal_utf8((unsigned char *)buffer, count)) {
        json_error(json, "%s", "invalid UTF-8 text");
        return -1;
    }

    for (i = 0; i < count; ++i) {
        if (pushchar(json, buffer[i]) != 0)
            return -1;
    }
    return 0;
}

static enum json_type read_string(json_stream *json)
{
    if (init_string(json) != 0)
        return JSON_ERROR;
    while (1) {
        int c = json->source.get(&json->source);
        if (c == EOF) {
            json_error(json, "%s", "unterminated string literal");
            return JSON_ERROR;
        } else if (c == '"') {
            if (pushchar(json, '\0') == 0)
                return JSON_STRING;
            return JSON_ERROR;
        } else if (c == '\\') {
            if (read_escaped(json) != 0)
                return JSON_ERROR;
        } else if ((unsigned)c >= 0x80) {
            if (read_utf8(json, c) != 0)
                return JSON_ERROR;
        } else {
            if (char_needs_escaping(c)) {
                json_error(json, "%s", "unescaped control character in string");
                return JSON_ERROR;
            }

            if (pushchar(json, c) != 0)
                return JSON_ERROR;
        }
    }
    return JSON_ERROR;
}

static int is_digit(int c)
{
    return c >= 48 /*0*/ && c <= 57 /*9*/;
}

static int read_digits(json_stream *json)
{
    int c;
    unsigned nread = 0;
    while (is_digit(c = json->source.peek(&json->source))) {
        if (pushchar(json, json->source.get(&json->source)) != 0)
            return -1;

        nread++;
    }

    if (nread == 0) {
        json_error(json, "expected digit instead of byte '%c'", c);
        return -1;
    }

    return 0;
}

static int read_fraction(json_stream *json)
{
    const int c = json->source.peek(&json->source);
    if (c != '.')
        return 0;
    (void)json->source.get(&json->source);
    if (pushchar(json, c) != 0)
        return -1;
    return read_digits(json);
}

static int read_exponent(json_stream *json)
{
    int c = json->source.peek(&json->source);
    if (c != 'e' && c != 'E')
        return 0;
    (void)json->source.get(&json->source);
    if (pushchar(json, c) != 0)
        return -1;
    c = json->source.peek(&json->source);
    if (c == '+' || c == '-') {
        (void)json->source.get(&json->source);
        if (pushchar(json, c) != 0)
            return -1;
    } else if (!is_digit(c)) {
        json_error(json, "unexpected byte '%c' in number", c);
        return -1;
    }
    return read_digits(json);
}

static enum json_type read_number(json_stream *json, int c)
{
    if (pushchar(json, c) != 0)
        return JSON_ERROR;
    if (c == '-') {
        c = json->source.get(&json->source);
        if (!is_digit(c)) {
            json_error(json, "unexpected byte '%c' in number", c);
            return JSON_ERROR;
        }
        return read_number(json, c);
    }
    if (c >= '1' && c <= '9' && is_digit(json->source.peek(&json->source))) {
        if (read_digits(json) != 0)
            return JSON_ERROR;
    }
    if (read_fraction(json) != 0 || read_exponent(json) != 0 || pushchar(json, '\0') != 0)
        return JSON_ERROR;
    return JSON_NUMBER;
}

bool json_isspace(int c)
{
    switch (c) {
    case 0x09:
    case 0x0a:
    case 0x0d:
    case 0x20:
        return true;
    default:
        return false;
    }
}

/* Returns the next non-whitespace character in the stream. */
static int next_nonspace(json_stream *json)
{
    int c;
    while (json_isspace(c = json->source.get(&json->source))) {
        if (c == '\n')
            json->lineno++;
    }
    return c;
}

static enum json_type read_value(json_stream *json, int c)
{
    json->ntokens++;
    switch (c) {
    case EOF:
        json_error(json, "%s", "unexpected end of text");
        return JSON_ERROR;
    case '{':
        return push(json, JSON_OBJECT);
    case '[':
        return push(json, JSON_ARRAY);
    case '"':
        return read_string(json);
    case 'n':
        return is_match(json, "ull", JSON_NULL);
    case 'f':
        return is_match(json, "alse", JSON_FALSE);
    case 't':
        return is_match(json, "rue", JSON_TRUE);
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
    case '-':
        if (init_string(json) != 0)
            return JSON_ERROR;
        return read_number(json, c);
    default:
        json_error(json, "unexpected byte '%c' in value", c);
        return JSON_ERROR;
    }
}

enum json_type json_peek(json_stream *json)
{
    if (json->next == JSON_NONE)
        json->next = json_next(json);
    return json->next;
}

static enum json_type read_array_item(json_stream *json, int c)
{
    struct json_stack *frame = &json->stack[json->stack_top];
    if (frame->count == 0) {
        if (c == ']')
            return pop(json, c, JSON_ARRAY);
        frame->count++;
        return read_value(json, c);
    }
    if (c == ',') {
        frame->count++;
        return read_value(json, next_nonspace(json));
    }
    if (c == ']')
        return pop(json, c, JSON_ARRAY);
    json_error(json, "unexpected byte '%c'", c);
    return JSON_ERROR;
}

static enum json_type read_member_name(json_stream *json, int c, const char *error)
{
    const enum json_type value = read_value(json, c);
    if (value != JSON_STRING) {
        if (value != JSON_ERROR)
            json_error(json, "%s", error);
        return JSON_ERROR;
    }
    json->stack[json->stack_top].count++;
    return value;
}

static enum json_type read_object_item(json_stream *json, int c)
{
    struct json_stack *frame = &json->stack[json->stack_top];
    if (frame->count == 0) {
        if (c == '}')
            return pop(json, c, JSON_OBJECT);
        return read_member_name(json, c, "expected member name or '}'");
    }
    if ((frame->count % 2) == 0) {
        if (c == '}')
            return pop(json, c, JSON_OBJECT);
        if (c != ',') {
            json_error(json, "%s", "expected ',' or '}' after member value");
            return JSON_ERROR;
        }
        return read_member_name(json, next_nonspace(json), "expected member name");
    }
    if (c != ':') {
        json_error(json, "%s", "expected ':' after member name");
        return JSON_ERROR;
    }
    frame->count++;
    return read_value(json, next_nonspace(json));
}

static enum json_type finish_document(json_stream *json)
{
    /* Streaming mode intentionally leaves trailing whitespace for the caller. */
    if (!(json->flags & JSON_FLAG_STREAMING)) {
        int c;
        do {
            c = json->source.peek(&json->source);
            if (json_isspace(c))
                c = json->source.get(&json->source);
        } while (json_isspace(c));
        if (c != EOF) {
            json_error(json, "expected end of text instead of byte '%c'", c);
            return JSON_ERROR;
        }
    }
    return JSON_DONE;
}

enum json_type json_next(json_stream *json)
{
    if (json->flags & JSON_FLAG_ERROR)
        return JSON_ERROR;
    if (json->next != JSON_NONE) {
        const enum json_type cached = json->next;
        json->next = JSON_NONE;
        return cached;
    }
    if (json->ntokens > 0 && json->stack_top == -1)
        return finish_document(json);
    const int c = next_nonspace(json);
    if (json->stack_top == -1) {
        if (c == EOF && (json->flags & JSON_FLAG_STREAMING))
            return JSON_DONE;
        return read_value(json, c);
    }
    if (json->stack[json->stack_top].type == JSON_ARRAY)
        return read_array_item(json, c);
    if (json->stack[json->stack_top].type == JSON_OBJECT)
        return read_object_item(json, c);
    json_error(json, "%s", "invalid parser state");
    return JSON_ERROR;
}

void json_reset(json_stream *json)
{
    json->stack_top = -1; /* No active container. */
    json->ntokens = 0;
    json->flags &= ~JSON_FLAG_ERROR;
    json->errmsg[0] = '\0';
}

enum json_type json_skip(json_stream *json)
{
    enum json_type type = json_next(json);
    size_t cnt_arr = 0;
    size_t cnt_obj = 0;

    for (enum json_type skip = type;; skip = json_next(json)) {
        if (skip == JSON_ERROR || skip == JSON_DONE)
            return skip;

        if (skip == JSON_ARRAY) {
            ++cnt_arr;
        } else if (skip == JSON_ARRAY_END && cnt_arr > 0) {
            --cnt_arr;
        } else if (skip == JSON_OBJECT) {
            ++cnt_obj;
        } else if (skip == JSON_OBJECT_END && cnt_obj > 0) {
            --cnt_obj;
        }

        if (!cnt_arr && !cnt_obj)
            break;
    }

    return type;
}

enum json_type json_skip_until(json_stream *json, enum json_type type)
{
    while (1) {
        enum json_type skip = json_skip(json);

        if (skip == JSON_ERROR || skip == JSON_DONE)
            return skip;

        if (skip == type)
            break;
    }

    return type;
}

const char *json_get_string(const json_stream *json, size_t *length)
{
    if (length != NULL)
        *length = json->data.string_fill;
    if (json->data.string == NULL)
        return "";
    return json->data.string;
}

double json_get_number(const json_stream *json)
{
    const char *p = json->data.string;
    return p == NULL ? 0 : strtod(p, NULL);
}

const char *json_get_error(const json_stream *json)
{
    return (json->flags & JSON_FLAG_ERROR) ? json->errmsg : NULL;
}

size_t json_get_lineno(const json_stream *json)
{
    return json->lineno;
}

size_t json_get_position(const json_stream *json)
{
    return json->source.position;
}

size_t json_get_depth(const json_stream *json)
{
    return json->stack_top + 1;
}

/* Return the current parsing context, that is, JSON_OBJECT if we are inside
   an object, JSON_ARRAY if we are inside an array, and JSON_DONE if we are
   not yet/anymore in either.

   Additionally, for the first two cases, also return the number of parsing
   events that have already been observed at this level with json_next/peek().
   In particular, inside an object, an odd number would indicate that the just
   observed JSON_STRING event is a member name.
*/
enum json_type json_get_context(const json_stream *json, size_t *count)
{
    if (json->stack_top == -1)
        return JSON_DONE;

    if (count != NULL)
        *count = json->stack[json->stack_top].count;

    return json->stack[json->stack_top].type;
}

int json_source_get(json_stream *json)
{
    int c = json->source.get(&json->source);
    if (c == '\n')
        json->lineno++;
    return c;
}

int json_source_peek(json_stream *json)
{
    return json->source.peek(&json->source);
}

void json_open_buffer(json_stream *json, const void *buffer, size_t size)
{
    init(json);
    json->source.get = buffer_get;
    json->source.peek = buffer_peek;
    json->source.source.buffer.buffer = (const char *)buffer;
    json->source.source.buffer.length = size;
}

void json_open_string(json_stream *json, const char *string)
{
    json_open_buffer(json, string, strlen(string));
}

void json_open_stream(json_stream *json, FILE *stream)
{
    init(json);
    json->source.get = stream_get;
    json->source.peek = stream_peek;
    json->source.source.stream.stream = stream;
}

static int user_get(struct json_source *json)
{
    return json->source.user.get(json->source.user.ptr);
}

static int user_peek(struct json_source *json)
{
    return json->source.user.peek(json->source.user.ptr);
}

void json_open_user(json_stream *json, json_user_io get, json_user_io peek, void *user)
{
    init(json);
    json->source.get = user_get;
    json->source.peek = user_peek;
    json->source.source.user.ptr = user;
    json->source.source.user.get = get;
    json->source.source.user.peek = peek;
}

void json_set_allocator(json_stream *json, const json_allocator *a)
{
    json->alloc = *a;
}

void json_set_streaming(json_stream *json, bool streaming)
{
    if (streaming) {
        json->flags |= JSON_FLAG_STREAMING;
    } else {
        json->flags &= ~JSON_FLAG_STREAMING;
    }
}

void json_close(json_stream *json)
{
    json->alloc.free(json->stack);
    json->alloc.free(json->data.string);
}

/* NOLINTEND(modernize-use-nullptr) */
