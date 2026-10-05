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

#include "vmafx/vmafx.h"

#if defined(__GNUC__) || defined(__clang__)
#define VMAFX_PRINTF_FORMAT(fmt, args) __attribute__((format(printf, fmt, args)))
#else
#define VMAFX_PRINTF_FORMAT(fmt, args)
#endif

/* Report a failure: stores a new VmafxError in `*out` when `out` is not NULL,
 * else logs the message at ERROR so no failure is silent. Returns `status`.
 * `engine_errno` is the negative errno the engine returned, 0 for none;
 * `subject` names what failed (parameter, feature, extractor), "" for none. */
VmafxStatus vmafx_fail(VmafxError **out, VmafxStatus status, int32_t engine_errno,
                       const char *subject, const char *fmt, ...) VMAFX_PRINTF_FORMAT(5, 6);

#endif /* VMAFX_ERROR_INTERNAL_H */
