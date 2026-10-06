/**
 * Copyright 2026 Lusoris
 * SPDX-License-Identifier: EUPL-1.2
 *
 * The provenance record in the vmaf CLI's JSON report (#2142, #2155,
 * ADR-2044); see cli_provenance.h.
 */

#include "cli_provenance.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cli_feature_backend.h"
#include "vmafx/libvmaf_bridge.h"
#include "vmafx/provenance.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

#define VERSION_MAX 256u /* longest version text kept; build versions are short */

/* `version` with every character JSON would need escaped replaced by '_',
 * as the backend receipt does with registry names. */
static void json_safe(const char *text, char *out, size_t sz)
{
    size_t n = 0;
    for (; text && n + 1 < sz && n < VERSION_MAX && text[n]; n++) {
        const char ch = text[n];
        const int plain = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                          (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.' ||
                          ch == '+';
        out[n] = plain ? ch : '_';
    }
    out[n] = '\0';
}

size_t cli_format_provenance_record(const struct VmafxProvenance *record, char *buf, size_t sz)
{
    if (!record) {
        if (buf && sz > 0) {
            buf[0] = '\0';
        }
        return 0;
    }
    char version[VERSION_MAX + 1];
    json_safe(record->version, version, sizeof(version));
    const char *const backend = cli_backend_label((enum VmafBackend)record->active_backend);
    const int n =
        snprintf(buf, buf ? sz : 0,
                 "\"provenance\": {\"abi_major\": %u, \"abi_minor\": %u, "
                 "\"abi_patch\": %u, \"active_backend\": \"%s\", "
                 "\"n_extractors\": %u, \"version\": \"%s\"}",
                 (unsigned)record->abi_major, (unsigned)record->abi_minor,
                 (unsigned)record->abi_patch, backend, (unsigned)record->n_extractors, version);
    return n > 0 ? (size_t)n : 0u;
}

size_t cli_format_provenance_member(VmafContext *vmaf, char *buf, size_t sz)
{
    VmafxProvenance record = VMAFX_PROVENANCE_INIT;
    VmafxContext *const context = vmaf ? vmafx_context_from_libvmaf(vmaf) : NULL;
    if (!context || vmafx_context_provenance(context, &record, NULL) != VMAFX_OK) {
        return cli_format_provenance_record(NULL, buf, sz);
    }
    return cli_format_provenance_record(&record, buf, sz);
}

/* NOLINTEND(modernize-use-nullptr) */
