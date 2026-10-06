/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * The provenance record in the engine's report writers (#2142, ADR-2073,
 * RC4 WP5): core/src/output.cpp asks core/src/vmafx/provenance_render.c for
 * the text of the record of the VMAFx context a libvmaf handle is bound to.
 */

#ifndef VMAFX_REPORT_FRAGMENTS_H
#define VMAFX_REPORT_FRAGMENTS_H

#include "libvmaf/libvmaf.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The JSON report's members `"provenance": {...}`, `"backend_used": "..."`
 * and `"feature_backends": [...]` (indented two spaces, separated by `,\n`,
 * no leading or trailing separator); NULL when the handle has no VMAFx
 * context or on failure. The caller frees it. */
char *vmafx_report_json_members(VmafContext *vmaf);

/* The XML report's `<provenance ...>` element with its `<model>`, `<feature>`
 * and `<annotation>` children, one line each; NULL as above. */
char *vmafx_report_xml_element(VmafContext *vmaf);

#ifdef __cplusplus
}
#endif

#endif /* VMAFX_REPORT_FRAGMENTS_H */
