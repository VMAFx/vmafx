/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
 */

/**
 * @file onnx_scan.h
 * @brief Minimal ONNX protobuf scanner — extracts NodeProto.op_type strings
 *        from a ModelProto buffer and checks each against the op allowlist.
 *
 * Rationale (ADR D39):
 *   ONNX Runtime 1.22's public C API does not expose per-node op-type
 *   iteration. Pulling in `libprotobuf-c` for one field scan is
 *   disproportionate. This scanner walks the wire format directly for the
 *   four fields that matter:
 *
 *       ModelProto.graph    = 7 (wire 2, length-delimited)
 *       GraphProto.node     = 1 (wire 2, length-delimited, repeated)
 *       NodeProto.op_type   = 4 (wire 2, length-delimited string)
 *       NodeProto.domain    = 7 (wire 2, length-delimited string)
 *
 * The domain field must be the empty string or "ai.onnx" (both denote the
 * standard built-in op set). Any other domain is rejected because ORT
 * dispatches via the (domain, op_type) tuple — a custom domain can shadow
 * an allowlisted op_type name with arbitrary custom-op code (ADR-1089).
 *
 * Everything else is skipped by wire-type. No recursive message parsing is
 * performed beyond the fixed three-level descent above, keeping the scanner
 * bounded and auditable.
 */

#ifndef LIBVMAF_DNN_ONNX_SCAN_H_
#define LIBVMAF_DNN_ONNX_SCAN_H_

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Scan an ONNX ModelProto buffer and reject any node whose `op_type` is
 * not in the allowlist (see op_allowlist.h).
 *
 * @param buf         pointer to raw ONNX-protobuf file contents.
 * @param len         size of @p buf in bytes.
 * @param first_bad   optional out-pointer. On rejection, set to a heap-
 *                    allocated copy of the first disallowed op name; caller
 *                    must free(). Pass NULL to skip this reporting.
 *
 * @return  0 on success (all node op_types permitted).
 * @return -EINVAL if @p buf is NULL or @p len is 0.
 * @return -EPERM  if any node op_type is not in the allowlist.
 * @return -ENOENT if the buffer contains no ModelProto.graph field.
 * @return -EBADMSG if the protobuf wire format is malformed (truncated
 *                  input, varint overflow, length-delimited field overruns
 *                  the buffer, unsupported wire type).
 * @return -ENOMEM if allocating @p first_bad fails.
 */
int vmaf_dnn_scan_onnx(const unsigned char *buf, size_t len, char **first_bad);

#ifdef __cplusplus
}
#endif

#endif /* LIBVMAF_DNN_ONNX_SCAN_H_ */
