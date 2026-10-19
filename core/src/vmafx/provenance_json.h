/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * JSON text in the canonical form of RFC 8785 (JSON Canonicalization Scheme)
 * for the provenance record (#2142, ADR-2073, RC4 WP5). The record only holds
 * strings, integers below 2^53 and 64-bit integers written as strings (the
 * proto JSON mapping), so the form reduces to: members in byte order of their
 * keys (every key is ASCII, where byte order is RFC 8785's UTF-16 order), no
 * whitespace, integers in plain decimal, and strings that escape only `"`,
 * `\` and the control characters (`\b \t \n \f \r`, else `\u00xx` in lower
 * case). Invalid UTF-8 in a string becomes U+FFFD, so the text is valid JSON.
 */

#ifndef VMAFX_PROVENANCE_JSON_H
#define VMAFX_PROVENANCE_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Members one object holds (HISS-02); the record has 39. */
#define VMAFX_JSON_MEMBERS_MAX 64u

/* A growable text; `failed` once an allocation failed (then nothing grows). */
typedef struct VmafxJsonText {
    char *data;
    size_t len;
    size_t cap;
    bool failed;
} VmafxJsonText;

void vmafx_json_text_put(VmafxJsonText *text, const char *bytes, size_t len);
void vmafx_json_text_puts(VmafxJsonText *text, const char *s);
/* `s` as a canonical JSON string (quotes included); NULL is "". */
void vmafx_json_text_string(VmafxJsonText *text, const char *s);
/* The NUL-terminated text, or NULL (failed); the text gives it up. */
char *vmafx_json_text_take(VmafxJsonText *text);

typedef struct VmafxJsonMember {
    const char *key; /* static ASCII */
    char *value;     /* JSON text, owned */
} VmafxJsonMember;

/* An object built member by member and written with its keys sorted. */
typedef struct VmafxJsonObject {
    VmafxJsonMember member[VMAFX_JSON_MEMBERS_MAX];
    uint32_t n;
    bool failed;
} VmafxJsonObject;

void vmafx_json_object_init(VmafxJsonObject *object);
/* Add `key` with JSON text `value`, which the object takes (NULL: failed). */
void vmafx_json_object_take(VmafxJsonObject *object, const char *key, char *value);
void vmafx_json_object_string(VmafxJsonObject *object, const char *key, const char *value);
void vmafx_json_object_u32(VmafxJsonObject *object, const char *key, uint32_t value);
void vmafx_json_object_i32(VmafxJsonObject *object, const char *key, int32_t value);
/* A 64-bit integer as a decimal string (the proto JSON mapping). */
void vmafx_json_object_u64(VmafxJsonObject *object, const char *key, uint64_t value);
/* The object's text with its members in key order, the members named in the
 * NULL-terminated `skip` (may be NULL) left out; NULL on failure. The object
 * keeps its members: finish again, then vmafx_json_object_release(). */
char *vmafx_json_object_finish(const VmafxJsonObject *object, const char *const *skip);
void vmafx_json_object_release(VmafxJsonObject *object);

/* `[item,item,...]` of `n` JSON texts, which it frees; NULL when one is NULL
 * or on failure. */
char *vmafx_json_array(char **items, uint32_t n);

/* `sha256:` and the SHA-256 of `data[0..len)` as lower-case hex. */
void vmafx_digest_text(const void *data, size_t len, char out[72]);

#endif /* VMAFX_PROVENANCE_JSON_H */
