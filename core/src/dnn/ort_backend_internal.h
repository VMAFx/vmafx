/**
 *  Copyright 2026 Lusoris and Claude (Anthropic)
 *  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
 *
 *  Internal helpers exposed for unit testing only. Production callers
 *  must use ort_backend.h. Helpers declared here have zero ORT-API
 *  dependencies (fp16 conversion + name resolution) so the test binary
 *  can exercise them without standing up a full ORT session.
 *
 *  Rationale: many of ort_backend.c's static helpers (fp16 edge cases,
 *  resolve_name out-of-range branch) are unreachable via the public
 *  libvmaf/dnn.h surface on a CPU-only ORT CI build, leaving the
 *  coverage gate stuck below 85%. Exposing the helpers via this
 *  internal header — kept out of libvmaf/include/ — is the minimum
 *  surface change needed to drive direct unit tests. See ADR-0112.
 */

#ifndef LIBVMAF_DNN_ORT_BACKEND_INTERNAL_H_
#define LIBVMAF_DNN_ORT_BACKEND_INTERNAL_H_

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Declarations are unconditional — both the real-ORT and stub branches of
 * ort_backend.c provide implementations so test_ort_internals.c links on
 * either build. The test bodies short-circuit via vmaf_dnn_available()
 * before invoking these on the stub path. */

/** IEEE 754 single → half. Handles inf/nan, overflow, underflow, subnormals. */
uint16_t vmaf_ort_internal_fp32_to_fp16(float f);

/** IEEE 754 half → single. Handles inf/nan, subnormals, zero. */
float vmaf_ort_internal_fp16_to_fp32(uint16_t h);

/**
 * Resolve a user-supplied input/output name against a session name table.
 * NULL @p name → positional fallback at @p pos. Returns the table entry
 * (owned by the session) or NULL on lookup failure / out-of-range pos.
 */
const char *vmaf_ort_internal_resolve_name(char **table, size_t count, const char *name,
                                           size_t pos);

#ifdef __cplusplus
}
#endif

#endif /* LIBVMAF_DNN_ORT_BACKEND_INTERNAL_H_ */
