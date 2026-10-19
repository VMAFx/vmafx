/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Lookup over the generated exactness table (exactness_gen.c): a bounded
 * scan of its rows (one per twin and backend, about a hundred).
 */

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "vmafx/exactness.h"
#include "vmafx/types.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

const VmafxExactnessRow *vmafx_exactness_find(const char *extractor, uint32_t backend)
{
    assert(vmafx_exactness_row_count > 0u);
    if (extractor == NULL || backend == (uint32_t)VMAFX_BACKEND_CPU) {
        return NULL;
    }
    for (uint32_t i = 0; i < vmafx_exactness_row_count; i++) {
        const VmafxExactnessRow *row = &vmafx_exactness_rows[i];
        if (row->backend == backend && strcmp(row->extractor, extractor) == 0) {
            return row;
        }
    }
    return NULL;
}

const char *vmafx_exactness_text(const char *extractor, uint32_t backend)
{
    if (backend == (uint32_t)VMAFX_BACKEND_CPU) {
        return "cpu-reference";
    }
    const VmafxExactnessRow *row = vmafx_exactness_find(extractor, backend);
    return row ? row->text : "unclassified";
}

size_t vmafx_exactness_class(const char *extractor, uint32_t backend, char *buf, size_t size)
{
    assert(buf != NULL || size == 0u);
    const int n = snprintf(buf, size, "%s", vmafx_exactness_text(extractor, backend));
    return n > 0 ? (size_t)n : 0u;
}

/* NOLINTEND(modernize-use-nullptr) */
