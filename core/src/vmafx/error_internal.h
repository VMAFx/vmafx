/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/* Construction of VmafxError values inside the library (ADR-1852). */

#ifndef VMAFX_ERROR_INTERNAL_H
#define VMAFX_ERROR_INTERNAL_H

#include <stdint.h>
#include <stdio.h>

#include "internal.h"
#include "vmafx/vmafx.h"

/* vmafx_fail_report() formats with the C runtime's vsnprintf(). On MinGW, GCC's
 * `printf` archetype is the MSVCRT one, which rejects %zu (the Windows UCRT64
 * build failed on model.c with -Werror=format); <stdio.h> names the archetype
 * the runtime's own declarations use in __MINGW_PRINTF_FORMAT (gnu_printf
 * under UCRT or __USE_MINGW_ANSI_STDIO, ms_printf for the old MSVCRT). */
#if defined(__MINGW_PRINTF_FORMAT)
#define VMAFX_PRINTF_FORMAT(fmt, args) __attribute__((format(__MINGW_PRINTF_FORMAT, fmt, args)))
#elif defined(__GNUC__) || defined(__clang__)
#define VMAFX_PRINTF_FORMAT(fmt, args) __attribute__((format(printf, fmt, args)))
#else
#define VMAFX_PRINTF_FORMAT(fmt, args)
#endif

/* What failed: the status, the negative errno the engine returned (0 for
 * none), what the subject names (a VmafxSubjectKind) and the subject itself
 * (parameter, feature, extractor, path, ...; "" for none). */
typedef struct VmafxFailure {
    VmafxStatus status;
    int32_t engine_errno;
    uint32_t kind;
    const char *subject;
} VmafxFailure;

/* Report a failure: stores a new VmafxError in `*report->error` when the
 * caller passed an error out-parameter, else delivers the message at ERROR to
 * the report's log sink or, without one, to stderr, whatever the log level:
 * no failure is silent (design section 2.5). Returns the status. */
VmafxStatus vmafx_fail_report(const VmafxReport *report, VmafxFailure failure, const char *fmt, ...)
    VMAFX_PRINTF_FORMAT(3, 4);

/* VMAFX_FAIL(report, status, engine_errno, kind, subject, fmt, ...) */
#define VMAFX_FAIL(report, status, engine_errno, kind, subject, ...)                               \
    vmafx_fail_report((report), (VmafxFailure){(status), (engine_errno), (kind), (subject)},       \
                      __VA_ARGS__)

#endif /* VMAFX_ERROR_INTERNAL_H */
