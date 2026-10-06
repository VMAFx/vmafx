/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * vmafx_report_write() (#2142, ADR-2073, RC4 WP5): the engine's report
 * writers (core/src/output.cpp), which embed the provenance record in JSON
 * and XML, and the `<path>.provenance.json` sidecar.
 */

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compat/path_utf8.h"
#include "error_internal.h"
#include "internal.h"
#include "libvmaf/libvmaf.h"
#include "provenance_record.h"
#include "status_gen.h"
#include "vmafx/vmafx.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

#define SIDECAR_SUFFIX ".provenance.json"
#define REPORT_PATH_MAX 4096u

static VmafxStatus write_text(const VmafxReport *report, const char *path, const char *text)
{
    FILE *const file = vmaf_fopen_utf8(path, "wb");
    if (!file) {
        return VMAFX_FAIL(report, VMAFX_E_IO, -EIO, VMAFX_SUBJECT_PATH, path, "cannot open");
    }
    const size_t len = strlen(text);
    const size_t written = fwrite(text, 1u, len, file);
    const int newline = fputc('\n', file);
    const int closed = fclose(file);
    if (written != len || newline == EOF || closed != 0) {
        return VMAFX_FAIL(report, VMAFX_E_IO, -EIO, VMAFX_SUBJECT_PATH, path, "cannot write");
    }
    return VMAFX_OK;
}

static VmafxStatus write_sidecar(const VmafxReport *report, VmafxContext *context, const char *path)
{
    char sidecar[REPORT_PATH_MAX];
    const int n = snprintf(sidecar, sizeof(sidecar), "%s%s", path, SIDECAR_SUFFIX);
    if (n < 0 || (size_t)n >= sizeof(sidecar)) {
        return VMAFX_FAIL(report, VMAFX_E_RANGE, 0, VMAFX_SUBJECT_PATH, path,
                          "the sidecar path is longer than %u bytes", REPORT_PATH_MAX - 1u);
    }
    VmafxProvenanceState *const state = &context->provenance;
    (void)pthread_mutex_lock(&state->lock);
    char *json = NULL;
    VmafxStatus status = vmafx_provenance_render(report, context, 0u, &json, NULL);
    (void)pthread_mutex_unlock(&state->lock);
    if (status == VMAFX_OK) {
        status = write_text(report, sidecar, json);
    }
    free(json);
    return status;
}

/* The engine's output format of a checked VmafxReportFormat. */
static enum VmafOutputFormat engine_format(uint32_t format)
{
    switch (format) {
    case VMAFX_REPORT_FORMAT_XML:
        return VMAF_OUTPUT_FORMAT_XML;
    case VMAFX_REPORT_FORMAT_JSON:
        return VMAF_OUTPUT_FORMAT_JSON;
    case VMAFX_REPORT_FORMAT_CSV:
        return VMAF_OUTPUT_FORMAT_CSV;
    default:
        return VMAF_OUTPUT_FORMAT_SUB;
    }
}

static VmafxStatus check_write(const VmafxReport *report, const VmafxContext *context,
                               const char *path, uint32_t format, uint32_t flags)
{
    if (!context || !path) {
        return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER,
                          context ? "path" : "context", "NULL argument");
    }
    if (format < VMAFX_REPORT_FORMAT_XML || format > VMAFX_REPORT_FORMAT_SUB) {
        return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER, "format",
                          "%u is not a VmafxReportFormat", (unsigned)format);
    }
    if (flags & ~(uint32_t)VMAFX_REPORT_PROVENANCE_SIDECAR) {
        return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER, "flags",
                          "unknown flag bits 0x%x", (unsigned)flags);
    }
    return VMAFX_OK;
}

VmafxStatus vmafx_report_write(VmafxContext *context, const char *path, uint32_t format,
                               uint32_t flags, const char *score_format, VmafxError **error)
{
    const VmafxReport report = VMAFX_REPORT(context, error);
    VmafxStatus status = check_write(&report, context, path, format, flags);
    if (status != VMAFX_OK) {
        return status;
    }
    const VmafLogSink *const previous = vmafx_engine_enter(context);
    const int err =
        vmaf_write_output_with_format(context->engine, path, engine_format(format), score_format);
    vmafx_engine_leave(context, previous);
    if (err) {
        return VMAFX_FAIL(&report, vmafx_status_from_errno(err), err, VMAFX_SUBJECT_PATH, path,
                          "cannot write the report (%d)", err);
    }
    if (flags & VMAFX_REPORT_PROVENANCE_SIDECAR) {
        status = write_sidecar(&report, context, path);
    }
    return status;
}

/* NOLINTEND(modernize-use-nullptr) */
