/**
 * Copyright 2026 Lusoris
 * SPDX-License-Identifier: EUPL-1.2
 *
 * The provenance record in the vmaf CLI's JSON report (#2142, #2155,
 * ADR-2044): the VMAFx provenance record of the context a run scored with,
 * as one JSON member, so the scoring server can read it into its proto
 * Provenance message. A C translation unit, so the C++ CLI sources never
 * include the generated C headers of the VMAFx API.
 */

#ifndef LIBVMAF_TOOLS_CLI_PROVENANCE_H_
#define LIBVMAF_TOOLS_CLI_PROVENANCE_H_

#include <stddef.h>

#include "libvmaf/libvmaf.h"

#ifdef __cplusplus
extern "C" {
#endif

struct VmafxProvenance;

/**
 * Format `"provenance": {...}` of @p record into @p buf (snprintf semantics):
 * one key per field of VmafxProvenance, named as the field; `active_backend`
 * is the backend's lower-case name and `version` keeps only characters that
 * need no JSON escape. Returns the length the full text needs, excluding the
 * terminating NUL; 0 for a NULL record.
 */
size_t cli_format_provenance_record(const struct VmafxProvenance *record, char *buf, size_t sz);

/**
 * The member of the VMAFx context @p vmaf is bound to, as
 * cli_format_provenance_record() formats it; 0 (and an empty @p buf) when the
 * handle has no VMAFx context or its record cannot be read.
 */
size_t cli_format_provenance_member(VmafContext *vmaf, char *buf, size_t sz);

#ifdef __cplusplus
}
#endif

#endif /* LIBVMAF_TOOLS_CLI_PROVENANCE_H_ */
