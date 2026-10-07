/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * VmafxError: the failure record of the VMAFx API (ADR-1852). One allocation
 * per failure, never on the success path; the caller releases it with
 * vmafx_error_free(). A caller that passes no error out-parameter gets the
 * message at ERROR in its context's log callback, or on stderr (design
 * section 2.5: no failure is silent).
 */

#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "error_internal.h"
#include "internal.h"
#include "log.h"
#include "vmafx/vmafx.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

struct VmafxError {
    VmafxStatus status;
    int32_t engine_errno;
    uint32_t subject_kind;
    char function[64];
    char subject[1024]; /* a path is a subject: room for one (was 96, truncated long paths) */
    char message[1024]; /* RC4 WP3: the import rule names an import and its refusals */
};

/* Copy `text` into `dst` (size `size` > 0), truncating and always terminating. */
static void copy_text(char *dst, size_t size, const char *text)
{
    const size_t len = text ? strnlen(text, size - 1) : 0;
    if (len) {
        memcpy(dst, text, len);
    }
    dst[len] = '\0';
}

/* No error out-parameter: deliver one line at ERROR, ignoring the log level. */
static void deliver_unclaimed(const VmafxReport *report, const VmafxError *error)
{
    char line[sizeof(error->function) + sizeof(error->subject) + sizeof(error->message) + 64];
    const char *const separator = error->subject[0] ? " [" : "";
    const char *const close = error->subject[0] ? "]" : "";
    const int written = snprintf(line, sizeof(line), "vmafx: %s: %s: %s%s%s%s", error->function,
                                 vmafx_status_name(error->status), error->message, separator,
                                 error->subject, close);
    if (written < 0) {
        copy_text(line, sizeof(line), "vmafx: unformattable failure");
    }
    if (report->sink) {
        report->sink->deliver(VMAF_LOG_LEVEL_ERROR, line, report->sink->user);
        return;
    }
    (void)fprintf(stderr, "libvmaf ERROR %s\n", line);
}

VmafxStatus vmafx_fail_report(const VmafxReport *report, VmafxFailure failure, const char *fmt, ...)
{
    assert(report != NULL);
    VmafxError record;
    record.status = failure.status;
    record.engine_errno = failure.engine_errno;
    record.subject_kind = failure.kind;
    copy_text(record.function, sizeof(record.function), report->function);
    copy_text(record.subject, sizeof(record.subject), failure.subject);
    va_list args;
#if defined(__clang__) && defined(__STDC_VERSION__) && __STDC_VERSION__ >= 202311L
    /* As in core/src/log.c: Clang lowers the C23 va_start macro to
     * __builtin_c23_va_start, which its VAList analyzer does not model yet. The
     * traditional builtin has identical initialization semantics in C23. */
    __builtin_va_start(args, fmt);
#else
    va_start(args, fmt);
#endif
    const int written = vsnprintf(record.message, sizeof(record.message), fmt, args);
    va_end(args);
    if (written < 0) {
        copy_text(record.message, sizeof(record.message), "unformattable error message");
    }
    VmafxError *const error = report->error ? malloc(sizeof(*error)) : NULL;
    if (!error) {
        deliver_unclaimed(report, &record);
        return failure.status;
    }
    *error = record;
    *report->error = error;
    return failure.status;
}

VmafxStatus vmafx_error_status(const VmafxError *error)
{
    return error ? error->status : VMAFX_E_INVALID;
}

const char *vmafx_error_message(const VmafxError *error)
{
    return error ? error->message : "";
}

const char *vmafx_error_subject(const VmafxError *error)
{
    return error ? error->subject : "";
}

uint32_t vmafx_error_subject_kind(const VmafxError *error)
{
    return error ? error->subject_kind : (uint32_t)VMAFX_SUBJECT_NONE;
}

const char *vmafx_error_function(const VmafxError *error)
{
    return error ? error->function : "";
}

int32_t vmafx_error_errno(const VmafxError *error)
{
    return error ? error->engine_errno : 0;
}

void vmafx_error_free(VmafxError *error)
{
    free(error);
}

/* NOLINTEND(modernize-use-nullptr) */
