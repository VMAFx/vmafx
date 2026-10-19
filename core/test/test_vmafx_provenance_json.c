/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * The canonical JSON of the provenance record (#2142, ADR-2073, RC4 WP5):
 * RFC 8785 string escapes, members in key order, 64-bit integers as strings,
 * invalid UTF-8 as U+FFFD; the digest of a fixed text; a build id that moves
 * with every part of the build description but the commit; the backend
 * receipt the JSON report carries (it was the CLI's, ADR-1359).
 *
 * Failing first: none of these functions exists on the WP8 base. Measured with
 * planted defects on this branch: an escape table without `\b` fails
 * test_string_escapes; a build id that leaves out the compiler fails
 * test_build_id_follows_the_build.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mu_table.h"
#include "test.h"
#include "vmafx/internal.h"
#include "vmafx/provenance_json.h"
#include "vmafx/provenance_record.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

static bool string_is(const char *in, const char *expected)
{
    VmafxJsonText text = {0};
    vmafx_json_text_string(&text, in);
    char *const out = vmafx_json_text_take(&text);
    const bool same = out && strcmp(out, expected) == 0;
    free(out);
    return same;
}

typedef struct EscapeCase {
    const char *in;
    const char *out;
    const char *what;
} EscapeCase;

static const EscapeCase escape_cases[] = {
    {"a\"b\\c", "\"a\\\"b\\\\c\"", "quote and backslash"},
    {"\b\t\n\f\r", "\"\\b\\t\\n\\f\\r\"", "short escapes"},
    {"\x01\x1f", "\"\\u0001\\u001f\"", "other control characters"},
    {"/\x7f", "\"/\x7f\"", "slash and DEL stay"},
    {"\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80", "\"\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80\"",
     "UTF-8 stays"},
    {"a\xff"
     "b",
     "\"a\xef\xbf\xbd"
     "b\"",
     "invalid byte"},
    {"\xc0\xaf", "\"\xef\xbf\xbd\xef\xbf\xbd\"", "overlong form"},
    {"\xed\xa0\x80", "\"\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd\"", "surrogate"},
    {"\xe2\x82", "\"\xef\xbf\xbd\xef\xbf\xbd\"", "truncated sequence"},
    {NULL, "\"\"", "NULL is empty"},
};

static char *test_string_escapes(void)
{
    for (size_t i = 0; i < sizeof(escape_cases) / sizeof(escape_cases[0]); i++) {
        const bool escaped = string_is(escape_cases[i].in, escape_cases[i].out);
        if (!escaped) {
            (void)fprintf(stderr, "escape case: %s\n", escape_cases[i].what);
        }
        mu_assert("string escape", escaped);
    }
    return NULL;
}

static char *test_members_in_key_order(void)
{
    VmafxJsonObject object;
    vmafx_json_object_init(&object);
    vmafx_json_object_u32(&object, "zeta", 7u);
    vmafx_json_object_i32(&object, "alpha", -1);
    vmafx_json_object_u64(&object, "big", UINT64_MAX);
    vmafx_json_object_string(&object, "Beta", "x");
    vmafx_json_object_string(&object, "_under", "y");
    char *const full = vmafx_json_object_finish(&object, NULL);
    static const char *const skip[] = {"big", NULL};
    char *const skipped = vmafx_json_object_finish(&object, skip);
    vmafx_json_object_release(&object);
    mu_assert("key order",
              full && strcmp(full, "{\"Beta\":\"x\",\"_under\":\"y\",\"alpha\":-1,"
                                   "\"big\":\"18446744073709551615\",\"zeta\":7}") == 0);
    mu_assert("skip", skipped && strcmp(skipped, "{\"Beta\":\"x\",\"_under\":\"y\",\"alpha\":-1,"
                                                 "\"zeta\":7}") == 0);
    free(full);
    free(skipped);

    char *items[] = {NULL, NULL};
    VmafxJsonText a = {0};
    vmafx_json_text_puts(&a, "1");
    items[0] = vmafx_json_text_take(&a);
    VmafxJsonText b = {0};
    vmafx_json_text_puts(&b, "{}");
    items[1] = vmafx_json_text_take(&b);
    char *const array = vmafx_json_array(items, 2u);
    mu_assert("array", array && strcmp(array, "[1,{}]") == 0);
    free(array);
    char *const empty = vmafx_json_array(items, 0u);
    mu_assert("empty array", empty && strcmp(empty, "[]") == 0);
    free(empty);
    return NULL;
}

static char *test_digest_text(void)
{
    char digest[VMAFX_DIGEST_TEXT_SIZE];
    vmafx_digest_text("abc", 3u, digest);
    mu_assert(
        "digest of abc",
        strcmp(digest, "sha256:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") ==
            0);
    return NULL;
}

static bool id_differs(const VmafxBuildInfo *changed, const char *reference)
{
    char id[VMAFX_DIGEST_TEXT_SIZE];
    return vmafx_build_id_of(changed, id) == 0 && strcmp(id, reference) != 0;
}

static const VmafxBuildInfo build_base = {
    .commit = "0000000000000000000000000000000000000000",
    .compiler = "c=gcc 16.2.1 cpp=gcc 16.2.1",
    .flags = "buildtype=release",
    .fp_policy = "-ffp-contract=off",
    .backends = "cpu",
    .arch = "x86_64",
    .rust_twins = 0,
};

/* The base build with part `which` changed (0..4 covered by the id, 5 the
 * commit and the architecture, which are not). */
static VmafxBuildInfo build_variant(unsigned which)
{
    VmafxBuildInfo info = build_base;
    switch (which) {
    case 0:
        info.compiler = "c=clang 22.1.8 cpp=clang 22.1.8";
        break;
    case 1:
        info.flags = "buildtype=debug";
        break;
    case 2:
        info.fp_policy = "";
        break;
    case 3:
        info.backends = "cpu,cuda";
        break;
    case 4:
        info.rust_twins = 1;
        break;
    default:
        info.commit = "1111111111111111111111111111111111111111";
        info.arch = "aarch64";
        break;
    }
    return info;
}

static char *test_build_id_follows_the_build(void)
{
    char reference[VMAFX_DIGEST_TEXT_SIZE];
    mu_assert("id", vmafx_build_id_of(&build_base, reference) == 0);
    for (unsigned which = 0; which < 5u; which++) {
        const VmafxBuildInfo other = build_variant(which);
        mu_assert("two builds, one id (compiler, flags, FP policy, backends or Rust)",
                  id_differs(&other, reference));
    }
    const VmafxBuildInfo moved = build_variant(5u);
    mu_assert("the commit and arch moved the id", !id_differs(&moved, reference));
    mu_assert("this build has an id", strncmp(vmafx_build_id(), "sha256:", 7) == 0);
    return NULL;
}

static char *receipt(const char *const *names, const uint32_t *backends, unsigned n)
{
    VmafxJsonText text = {0};
    vmafx_backend_receipt(&text, names, backends, n);
    return vmafx_json_text_take(&text);
}

static bool receipt_is(const char *const *names, const uint32_t *backends, unsigned n,
                       const char *expected)
{
    char *const text = receipt(names, backends, n);
    const bool same = text && strcmp(text, expected) == 0;
    free(text);
    return same;
}

static bool receipt_has(const char *const *names, const uint32_t *backends, unsigned n,
                        const char *part)
{
    char *const text = receipt(names, backends, n);
    const bool has = text && strstr(text, part) != NULL;
    free(text);
    return has;
}

static char *test_backend_receipt(void)
{
    const char *const names[] = {"vif_sycl", "ciede", "motion_sycl"};
    const uint32_t mixed[] = {VMAFX_BACKEND_SYCL, VMAFX_BACKEND_CPU, VMAFX_BACKEND_SYCL};
    mu_assert("mixed run", receipt_is(names, mixed, 3u,
                                      ",\n  \"feature_backends\": ["
                                      "{\"extractor\": \"vif_sycl\", \"backend\": \"sycl\"}, "
                                      "{\"extractor\": \"ciede\", \"backend\": \"cpu\"}, "
                                      "{\"extractor\": \"motion_sycl\", \"backend\": \"sycl\"}],\n"
                                      "  \"backend_used\": \"sycl\""));
    const uint32_t cpu[] = {VMAFX_BACKEND_CPU};
    mu_assert("cpu run", receipt_has(names + 1, cpu, 1u, "\"backend_used\": \"cpu\""));
    mu_assert(
        "no extractor",
        receipt_is(names, cpu, 0u, ",\n  \"feature_backends\": [],\n  \"backend_used\": \"cpu\""));
    const char *const odd[] = {"odd\"name\\"};
    const uint32_t cuda[] = {VMAFX_BACKEND_CUDA};
    mu_assert("names are escaped", receipt_has(odd, cuda, 1u, "\"odd\\\"name\\\\\""));
    return NULL;
}

char *run_tests(void)
{
    static const MuTest tests[] = {
        MU_TEST(test_string_escapes),  MU_TEST(test_members_in_key_order),
        MU_TEST(test_digest_text),     MU_TEST(test_build_id_follows_the_build),
        MU_TEST(test_backend_receipt),
    };
    return mu_run_table(tests, MU_TABLE_LEN(tests));
}

/* NOLINTEND(modernize-use-nullptr) */
