/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * The transport-less build of vmafx_mcp_start_sse(): it must refuse with
 * VMAFX_E_NOTSUP, name the backend, and still define its `port` output
 * (stores 0, no bound port). Failing first: against the stub that left `port`
 * untouched, test_port_is_defined_on_refusal reads back the sentinel and fails.
 * Skipped, with the reason, in a build that has the MCP server compiled in.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "mu_table.h"
#include "test.h"
#include "vmafx/vmafx.h"

#define SENTINEL 0xDEADBEEFu

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

static char *test_port_is_defined_on_refusal(void)
{
    if (vmafx_mcp_available()) {
        return NULL; /* skipped: this build has an MCP server, the stub is not compiled */
    }
    uint32_t port = SENTINEL;
    VmafxError *error = NULL;
    const VmafxStatus status = vmafx_mcp_start_sse(NULL, NULL, &port, &error);
    mu_assert("refused with NOTSUP", status == VMAFX_E_NOTSUP);
    mu_assert("port output defined (0), not left as the sentinel", port == 0u);
    mu_assert("error reported", error != NULL);
    mu_assert("error status", vmafx_error_status(error) == VMAFX_E_NOTSUP);
    mu_assert("error names the backend", strcmp(vmafx_error_subject(error), "mcp") == 0);
    vmafx_error_free(error);
    return NULL;
}

static char *test_null_port_is_accepted(void)
{
    if (vmafx_mcp_available()) {
        return NULL;
    }
    VmafxError *error = NULL;
    mu_assert("NULL port: NOTSUP, no crash",
              vmafx_mcp_start_sse(NULL, NULL, NULL, &error) == VMAFX_E_NOTSUP);
    vmafx_error_free(error);
    return NULL;
}

static char *test_null_error_pointer(void)
{
    if (vmafx_mcp_available()) {
        return NULL;
    }
    uint32_t port = SENTINEL;
    mu_assert("NULL error pointer: NOTSUP",
              vmafx_mcp_start_sse(NULL, NULL, &port, NULL) == VMAFX_E_NOTSUP);
    mu_assert("port defined without an error pointer", port == 0u);
    return NULL;
}

char *run_tests(void)
{
    static const MuTest tests[] = {
        MU_TEST(test_port_is_defined_on_refusal),
        MU_TEST(test_null_port_is_accepted),
        MU_TEST(test_null_error_pointer),
    };
    return mu_run_table(tests, MU_TABLE_LEN(tests));
}

/* NOLINTEND(modernize-use-nullptr) */
