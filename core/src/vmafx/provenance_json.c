/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Canonical JSON text (RFC 8785) for the provenance record; see
 * provenance_json.h. core/test/test_vmafx_provenance_json.c checks the
 * escapes, the member order and the digest against fixed texts.
 */

#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "provenance_json.h"
#include "sha256.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

/* Largest text the record may grow to (HISS-02): far above a record with
 * thousands of features. */
#define JSON_TEXT_MAX ((size_t)256u << 20)
/* Longest string a record value may be (a path, an annotation). */
#define JSON_STRING_MAX ((size_t)1u << 20)

static bool text_reserve(VmafxJsonText *text, size_t more)
{
    if (text->failed) {
        return false;
    }
    if (more <= text->cap - text->len) {
        return true;
    }
    size_t cap = text->cap ? text->cap : 256u;
    while (cap - text->len < more && cap <= JSON_TEXT_MAX) {
        cap *= 2u;
    }
    char *const data = cap <= JSON_TEXT_MAX ? realloc(text->data, cap) : NULL;
    if (!data) {
        text->failed = true;
        return false;
    }
    text->data = data;
    text->cap = cap;
    return true;
}

void vmafx_json_text_put(VmafxJsonText *text, const char *bytes, size_t len)
{
    if (len && text_reserve(text, len + 1u)) {
        memcpy(text->data + text->len, bytes, len);
        text->len += len;
    }
}

void vmafx_json_text_puts(VmafxJsonText *text, const char *s)
{
    vmafx_json_text_put(text, s, strlen(s));
}

/* Bytes of the valid UTF-8 sequence at `s` (at most `left`), or 0. */
static size_t utf8_length(const unsigned char *s, size_t left)
{
    const unsigned char c = s[0];
    size_t n = 0;
    unsigned char lo = 0x80u;
    unsigned char hi = 0xbfu;
    if (c >= 0xc2u && c <= 0xdfu) {
        n = 2;
    } else if (c >= 0xe0u && c <= 0xefu) {
        n = 3;
        lo = c == 0xe0u ? 0xa0u : 0x80u; /* no overlong form */
        hi = c == 0xedu ? 0x9fu : 0xbfu; /* no surrogate */
    } else if (c >= 0xf0u && c <= 0xf4u) {
        n = 4;
        lo = c == 0xf0u ? 0x90u : 0x80u;
        hi = c == 0xf4u ? 0x8fu : 0xbfu; /* at most U+10FFFF */
    }
    if (n == 0 || n > left || s[1] < lo || s[1] > hi) {
        return 0;
    }
    for (size_t i = 2; i < n; i++) {
        if (s[i] < 0x80u || s[i] > 0xbfu) {
            return 0;
        }
    }
    return n;
}

/* The escape of an ASCII byte RFC 8785 escapes, or NULL. */
static const char *short_escape(unsigned char c)
{
    switch (c) {
    case '"':
        return "\\\"";
    case '\\':
        return "\\\\";
    case '\b':
        return "\\b";
    case '\t':
        return "\\t";
    case '\n':
        return "\\n";
    case '\f':
        return "\\f";
    case '\r':
        return "\\r";
    default:
        return NULL;
    }
}

/* One character of `s` at `*at`; advances `*at`. */
static void put_char(VmafxJsonText *text, const unsigned char *s, size_t len, size_t *at)
{
    const unsigned char c = s[*at];
    const char *const escape = short_escape(c);
    if (escape) {
        vmafx_json_text_puts(text, escape);
        *at += 1u;
    } else if (c < 0x20u) {
        char hex[8];
        (void)snprintf(hex, sizeof(hex), "\\u%04x", (unsigned)c);
        vmafx_json_text_puts(text, hex);
        *at += 1u;
    } else if (c < 0x80u) {
        vmafx_json_text_put(text, (const char *)&s[*at], 1u);
        *at += 1u;
    } else {
        const size_t n = utf8_length(s + *at, len - *at);
        /* An invalid byte is U+FFFD (EF BF BD), one per byte. */
        vmafx_json_text_put(text, n ? (const char *)&s[*at] : "\xef\xbf\xbd", n ? n : 3u);
        *at += n ? n : 1u;
    }
}

void vmafx_json_text_string(VmafxJsonText *text, const char *s)
{
    const unsigned char *const bytes = (const unsigned char *)(s ? s : "");
    const size_t len = strnlen((const char *)bytes, JSON_STRING_MAX);
    vmafx_json_text_put(text, "\"", 1u);
    for (size_t at = 0; at < len && !text->failed;) {
        put_char(text, bytes, len, &at);
    }
    vmafx_json_text_put(text, "\"", 1u);
}

char *vmafx_json_text_take(VmafxJsonText *text)
{
    if (!text_reserve(text, 1u)) {
        free(text->data);
        text->data = NULL;
        return NULL;
    }
    text->data[text->len] = '\0';
    char *const data = text->data;
    text->data = NULL;
    text->len = 0;
    text->cap = 0;
    return data;
}

void vmafx_json_object_init(VmafxJsonObject *object)
{
    memset(object, 0, sizeof(*object));
}

void vmafx_json_object_take(VmafxJsonObject *object, const char *key, char *value)
{
    if (!value || object->n >= VMAFX_JSON_MEMBERS_MAX) {
        free(value);
        object->failed = true;
        return;
    }
    object->member[object->n].key = key;
    object->member[object->n].value = value;
    object->n++;
}

void vmafx_json_object_string(VmafxJsonObject *object, const char *key, const char *value)
{
    VmafxJsonText text = {0};
    vmafx_json_text_string(&text, value);
    vmafx_json_object_take(object, key, vmafx_json_text_take(&text));
}

static void object_number(VmafxJsonObject *object, const char *key, int64_t value)
{
    char buf[32];
    /* A literal format: cppcheck reads "%" PRId64 as a lone '%' and reports no argument. */
    (void)snprintf(buf, sizeof(buf), "%lld", (long long)value);
    VmafxJsonText text = {0};
    vmafx_json_text_puts(&text, buf);
    vmafx_json_object_take(object, key, vmafx_json_text_take(&text));
}

void vmafx_json_object_u32(VmafxJsonObject *object, const char *key, uint32_t value)
{
    object_number(object, key, (int64_t)value);
}

void vmafx_json_object_i32(VmafxJsonObject *object, const char *key, int32_t value)
{
    object_number(object, key, (int64_t)value);
}

void vmafx_json_object_u64(VmafxJsonObject *object, const char *key, uint64_t value)
{
    char buf[32];
    (void)snprintf(buf, sizeof(buf), "%" PRIu64, value);
    vmafx_json_object_string(object, key, buf);
}

static bool skipped(const char *key, const char *const *skip)
{
    for (size_t i = 0; skip && i < VMAFX_JSON_MEMBERS_MAX && skip[i]; i++) {
        if (strcmp(key, skip[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* Indices of the members in key order (insertion sort; at most 64). */
static void sorted_order(const VmafxJsonObject *object, uint32_t order[VMAFX_JSON_MEMBERS_MAX])
{
    for (uint32_t i = 0; i < object->n; i++) {
        uint32_t j = i;
        for (; j > 0 && strcmp(object->member[order[j - 1u]].key, object->member[i].key) > 0; j--) {
            order[j] = order[j - 1u];
        }
        order[j] = i;
    }
}

char *vmafx_json_object_finish(const VmafxJsonObject *object, const char *const *skip)
{
    assert(object && object->n <= VMAFX_JSON_MEMBERS_MAX);
    if (object->failed) {
        return NULL;
    }
    uint32_t order[VMAFX_JSON_MEMBERS_MAX];
    sorted_order(object, order);
    VmafxJsonText text = {0};
    vmafx_json_text_put(&text, "{", 1u);
    bool first = true;
    for (uint32_t i = 0; i < object->n; i++) {
        const VmafxJsonMember *const member = &object->member[order[i]];
        if (skipped(member->key, skip)) {
            continue;
        }
        vmafx_json_text_puts(&text, first ? "" : ",");
        vmafx_json_text_string(&text, member->key);
        vmafx_json_text_put(&text, ":", 1u);
        vmafx_json_text_puts(&text, member->value);
        first = false;
    }
    vmafx_json_text_put(&text, "}", 1u);
    return vmafx_json_text_take(&text);
}

void vmafx_json_object_release(VmafxJsonObject *object)
{
    for (uint32_t i = 0; i < object->n; i++) {
        free(object->member[i].value);
    }
    object->n = 0;
}

char *vmafx_json_array(char **items, uint32_t n)
{
    assert(items || n == 0);
    VmafxJsonText text = {0};
    bool ok = true;
    vmafx_json_text_put(&text, "[", 1u);
    for (uint32_t i = 0; i < n; i++) {
        ok = ok && items[i] != NULL;
        if (ok) {
            vmafx_json_text_puts(&text, i ? "," : "");
            vmafx_json_text_puts(&text, items[i]);
        }
        free(items[i]);
        items[i] = NULL;
    }
    vmafx_json_text_put(&text, "]", 1u);
    char *const out = vmafx_json_text_take(&text);
    if (!ok) {
        free(out);
        return NULL;
    }
    return out;
}

void vmafx_digest_text(const void *data, size_t len, char out[72])
{
    char hex[VMAFX_SHA256_HEX_CHARS];
    vmafx_sha256_hex(data, len, hex);
    (void)snprintf(out, 72u, "sha256:%s", hex);
}

/* NOLINTEND(modernize-use-nullptr) */
