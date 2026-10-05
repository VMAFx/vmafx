/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * VmafxError: the failure record of the VMAFx API (ADR-1852). One allocation
 * per failure, never on the success path; the caller releases it with
 * vmafx_error_free().
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "error_internal.h"
#include "log.h"
#include "vmafx/vmafx.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

struct VmafxError {
    VmafxStatus status;
    int32_t engine_errno;
    char subject[96];
    char message[384];
};

/* Copy `text` into `dst` (size `size` > 0), truncating and always terminating. */
static void copy_text(char *dst, size_t size, const char *text)
{
    const size_t len = text ? strnlen(text, size - 1) : 0;
    if (len)
        memcpy(dst, text, len);
    dst[len] = '\0';
}

VmafxStatus vmafx_fail(VmafxError **out, VmafxStatus status, int32_t engine_errno,
                       const char *subject, const char *fmt, ...)
{
    char message[sizeof(((VmafxError *)0)->message)];
    va_list args;
#if defined(__clang__) && defined(__STDC_VERSION__) && __STDC_VERSION__ >= 202311L
    /* As in core/src/log.c: Clang lowers the C23 va_start macro to
     * __builtin_c23_va_start, which its VAList analyzer does not model yet. The
     * traditional builtin has identical initialization semantics in C23. */
    __builtin_va_start(args, fmt);
#else
    va_start(args, fmt);
#endif
    const int written = vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);
    if (written < 0)
        copy_text(message, sizeof(message), "unformattable error message");
    VmafxError *const error = out ? malloc(sizeof(*error)) : NULL;
    if (!error) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "vmafx: %s: %s%s%s\n", vmafx_status_name(status), message,
                 subject && *subject ? " " : "", subject ? subject : "");
        return status;
    }
    error->status = status;
    error->engine_errno = engine_errno;
    copy_text(error->subject, sizeof(error->subject), subject);
    copy_text(error->message, sizeof(error->message), message);
    *out = error;
    return status;
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

int32_t vmafx_error_errno(const VmafxError *error)
{
    return error ? error->engine_errno : 0;
}

void vmafx_error_free(VmafxError *error)
{
    free(error);
}

/* NOLINTEND(modernize-use-nullptr) */
