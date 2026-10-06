/**
 * Copyright 2026 Lusoris
 * SPDX-License-Identifier: EUPL-1.2
 *
 * The provenance member of the vmaf CLI's JSON report (#2155, ADR-2044): one
 * key per field of VmafxProvenance, named as the field, so the scoring server
 * reads it into its proto Provenance message without loss. Failing first:
 * without cli_provenance.c the report has no `provenance` and the server's
 * scoreProvenance() refuses it.
 */

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "cli_provenance.h"
#include "libvmaf/libvmaf.h"
#include "mu_table.h"
#include "test.h"
#include "vmafx/provenance.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. ADR-1138. */

static char *test_record_member(void)
{
    VmafxProvenance record = VMAFX_PROVENANCE_INIT;
    record.abi_major = 0U;
    record.abi_minor = 1U;
    record.abi_patch = 4U;
    record.active_backend = VMAFX_BACKEND_SYCL;
    record.n_extractors = 3U;
    record.version = "v1.0.0-rc.4-7-gabc";
    char json[256];
    const size_t len = cli_format_provenance_record(&record, json, sizeof(json));
    const char *const want = "\"provenance\": {\"abi_major\": 0, \"abi_minor\": 1, "
                             "\"abi_patch\": 4, \"active_backend\": \"sycl\", "
                             "\"n_extractors\": 3, \"version\": \"v1.0.0-rc.4-7-gabc\"}";
    mu_assert("provenance member differs", !strcmp(json, want));
    mu_assert("sizing pass disagrees", cli_format_provenance_record(&record, NULL, 0) == len);
    return NULL;
}

static char *test_version_is_json_safe_and_null_is_empty(void)
{
    VmafxProvenance record = VMAFX_PROVENANCE_INIT;
    record.version = "v1\"x\\y";
    char json[256];
    (void)cli_format_provenance_record(&record, json, sizeof(json));
    mu_assert("a quote or backslash reached the JSON", strstr(json, "\"version\": \"v1_x_y\"}"));
    record.version = NULL;
    (void)cli_format_provenance_record(&record, json, sizeof(json));
    mu_assert("a missing version is not an empty string", strstr(json, "\"version\": \"\"}"));
    mu_assert("a NULL record formats something",
              cli_format_provenance_record(NULL, json, sizeof(json)) == 0 && json[0] == '\0');
    return NULL;
}

static char *test_member_of_a_libvmaf_context(void)
{
    VmafConfiguration cfg;
    memset(&cfg, 0, sizeof(cfg));
    VmafContext *vmaf = NULL;
    mu_assert("vmaf_init", vmaf_init(&vmaf, cfg) == 0);
    char json[512];
    const size_t len = cli_format_provenance_member(vmaf, json, sizeof(json));
    const bool ok = len > 0 && len < sizeof(json) && strstr(json, "\"active_backend\": \"cpu\"") &&
                    !strstr(json, "\"version\": \"\"");
    mu_assert("vmaf_close", vmaf_close(vmaf) == 0);
    mu_assert("a libvmaf context has no provenance member", ok);
    mu_assert("a NULL handle formats something", cli_format_provenance_member(NULL, json, 8) == 0);
    return NULL;
}

char *run_tests(void)
{
    static const MuTest tests[] = {
        MU_TEST(test_record_member),
        MU_TEST(test_version_is_json_safe_and_null_is_empty),
        MU_TEST(test_member_of_a_libvmaf_context),
    };
    return mu_run_table(tests, MU_TABLE_LEN(tests));
}

/* NOLINTEND(modernize-use-nullptr) */
