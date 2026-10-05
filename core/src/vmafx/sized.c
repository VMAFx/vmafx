/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Struct size negotiation of the VMAFx API (ADR-1852, design section 2.6).
 * Every struct that crosses the ABI starts with uint32_t struct_size and only
 * grows at the end, so a caller compiled against older headers passes a
 * shorter struct and one compiled against newer headers a longer one.
 */

#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "error_internal.h"
#include "internal.h"

/* The caller's struct_size; the struct starts with it. */
static uint32_t caller_size(const void *sized)
{
    uint32_t size = 0;
    memcpy(&size, sized, sizeof(size));
    return size;
}

VmafxStatus vmafx_read_sized(const VmafxReport *report, void *local, uint32_t full, const void *in,
                             uint32_t min, const char *subject)
{
    assert(local && in && min >= sizeof(uint32_t) && min <= full);
    const uint32_t size = caller_size(in);
    if (size < min) {
        return VMAFX_FAIL(report, VMAFX_E_ABI, 0, VMAFX_SUBJECT_PARAMETER, subject,
                          "struct_size %u is below %u, the size this struct had when it was "
                          "introduced",
                          (unsigned)size, (unsigned)min);
    }
    /* Fields past the caller's struct keep the defaults `local` holds; fields
     * past ours (a newer caller) are not read. */
    const uint32_t known = size < full ? size : full;
    memcpy(local, in, known);
    memcpy(local, &full, sizeof(full));
    return VMAFX_OK;
}

VmafxStatus vmafx_write_sized(const VmafxReport *report, void *out, const void *record,
                              uint32_t full, const char *subject)
{
    assert(out && record && full >= sizeof(uint32_t));
    const uint32_t size = caller_size(out);
    if (size < sizeof(uint32_t)) {
        return VMAFX_FAIL(report, VMAFX_E_ABI, 0, VMAFX_SUBJECT_PARAMETER, subject,
                          "struct_size %u cannot hold struct_size itself", (unsigned)size);
    }
    vmafx_store_sized(out, record, full);
    return VMAFX_OK;
}

void vmafx_store_sized(void *out, const void *record, uint32_t full)
{
    assert(out && record && full >= sizeof(uint32_t));
    const uint32_t size = caller_size(out);
    const uint32_t written = size < full ? size : full;
    memcpy(out, record, written);
    memcpy(out, &written, sizeof(written));
}
