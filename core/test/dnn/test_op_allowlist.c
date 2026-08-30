/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 */

#include "test.h"

#include "dnn/op_allowlist.h"

static char *test_common_ops_allowed(void)
{
    mu_assert("Conv should be allowed", vmaf_dnn_op_allowed("Conv"));
    mu_assert("Gemm should be allowed", vmaf_dnn_op_allowed("Gemm"));
    mu_assert("Relu should be allowed", vmaf_dnn_op_allowed("Relu"));
    mu_assert("BatchNormalization should be allowed", vmaf_dnn_op_allowed("BatchNormalization"));
    mu_assert("GlobalAveragePool should be allowed", vmaf_dnn_op_allowed("GlobalAveragePool"));
    mu_assert("QuantizeLinear should be allowed", vmaf_dnn_op_allowed("QuantizeLinear"));
    mu_assert("DequantizeLinear should be allowed", vmaf_dnn_op_allowed("DequantizeLinear"));
    return NULL;
}

static char *test_control_flow_ops_allowed(void)
{
    /* ADR-0169 / T6-5: Loop + If joined the allowlist. Subgraph contents
     * are validated recursively by the onnx_scan.c walker, so a forbidden
     * op nested in a Loop body is still rejected at load time. Scan
     * stays off-list — its variant-typed input/output binding makes
     * static bound-checking impractical (see ADR-0169 § Alternatives). */
    mu_assert("Loop should be allowed", vmaf_dnn_op_allowed("Loop"));
    mu_assert("If should be allowed", vmaf_dnn_op_allowed("If"));
    mu_assert("Scan must remain rejected", !vmaf_dnn_op_allowed("Scan"));
    return NULL;
}

static char *test_custom_ops_rejected(void)
{
    mu_assert("unknown should be rejected", !vmaf_dnn_op_allowed("custom_op_xyz"));
    mu_assert("NULL should be rejected", !vmaf_dnn_op_allowed(NULL));
    mu_assert("empty string should be rejected", !vmaf_dnn_op_allowed(""));
    return NULL;
}

static char *test_resize_op_allowed(void)
{
    /* ADR-0258 / T7-32: Resize joined the allowlist to unblock U-2-Net,
     * mobilesal, and the wider saliency / segmentation surface. ORT
     * executes whatever `mode` the model declares — the consumer is
     * expected to ship `mode in ("nearest", "linear")`. */
    mu_assert("Resize should be allowed", vmaf_dnn_op_allowed("Resize"));
    /* Sanity guard against a typo regression — case sensitivity matters,
     * the ONNX spec spells the op exactly "Resize". */
    mu_assert("resize lowercase must remain rejected", !vmaf_dnn_op_allowed("resize"));
    mu_assert("RESIZE uppercase must remain rejected", !vmaf_dnn_op_allowed("RESIZE"));
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_common_ops_allowed);
    mu_run_test(test_control_flow_ops_allowed);
    mu_run_test(test_resize_op_allowed);
    mu_run_test(test_custom_ops_rejected);
    return NULL;
}
