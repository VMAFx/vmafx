/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 */

#include <string.h>

#include "op_allowlist.h"

static const char *const ALLOWED_OPS[] = {
    /* structural / shape */
    "Identity",
    "Reshape",
    "Flatten",
    "Squeeze",
    "Unsqueeze",
    "Transpose",
    "Concat",
    "Slice",
    "Gather",
    "Cast",
    "Shape",
    "Expand",
    /* arithmetic */
    "Add",
    "Sub",
    "Mul",
    "Div",
    "Neg",
    "Abs",
    "Sqrt",
    "Pow",
    "Exp",
    "Log",
    "Clip",
    "Min",
    "Max",
    "Sum",
    "Mean",
    /* reductions */
    "ReduceMean",
    "ReduceSum",
    "ReduceMax",
    "ReduceMin",
    "GlobalAveragePool",
    "GlobalMaxPool",
    /* dense */
    "Gemm",
    "MatMul",
    /* convolutional */
    "Conv",
    "ConvTranspose",
    "MaxPool",
    "AveragePool",
    /* spatial sampling (ADR-0258 / T7-32) — admitted for saliency,
     * segmentation, and feature-pyramid models. `Resize` itself has no
     * filesystem / network side effects; the wire scanner gates op-type,
     * not attributes. ORT executes whatever `mode` the model declares
     * (`nearest`, `linear`, `cubic`). Consumers shipping their own ONNX
     * are expected to keep `mode in ("nearest", "linear")` — `cubic` is
     * numerically less stable on quantised inputs and not exercised by
     * any in-tree consumer. Unblocks U-2-Net (PR #341 follow-up) and
     * the wider saliency / segmentation surface (mobilesal, BASNet,
     * PiDiNet, FPN-style detectors). */
    "Resize",
    /* normalization */
    "BatchNormalization",
    "LayerNormalization",
    "InstanceNormalization",
    /* activations */
    "Relu",
    "LeakyRelu",
    "Sigmoid",
    "Tanh",
    "Softmax",
    "Elu",
    "Selu",
    "Softplus",
    "Softsign",
    "Gelu",
    "Erf",
    "HardSigmoid",
    "HardSwish",
    "PRelu",
    "Clip",
    /* dropout (no-op in inference) */
    "Dropout",
    /* INT8 post-training quantization — two flavours:
     *
     * QDQ format (static PTQ): the underlying Gemm / Conv / MatMul stay on
     * the list above; QuantizeLinear / DequantizeLinear carry the per-channel
     * scale + zero-point tensors that wrap each quantized operator.
     *
     * Dynamic PTQ format: DynamicQuantizeLinear computes per-tensor scale and
     * zero-point at runtime; MatMulInteger and ConvInteger execute integer
     * matrix-multiply and integer convolution respectively.  None of these
     * three ops have filesystem or network side-effects — they are pure
     * arithmetic on integer tensors.  Required for vmaf_tiny_v3.int8,
     * vmaf_tiny_v4.int8, and nr_metric_v1.int8 (all dynamic-PTQ exports).
     */
    "QuantizeLinear",
    "DequantizeLinear",
    "DynamicQuantizeLinear",
    "MatMulInteger",
    "ConvInteger",
    /* misc, safe */
    "Constant",
    "ConstantOfShape",
    "Pad",
    "Reciprocal",
    "ReduceProd",
    "GatherND",
    "ScatterND",
    "BitShift",
    /* control flow (ADR-0169 / T6-5) — accepted, but every op inside the
     * Loop.body / If.then_branch / If.else_branch subgraph must also pass
     * the allowlist (recursive scan in onnx_scan.c). `Scan` is deliberately
     * NOT on this list — its variant-typed input/output binding makes
     * static bound-checking impractical; revisit if a concrete consumer
     * appears. */
    "Loop",
    "If",
    NULL,
};

bool vmaf_dnn_op_allowed(const char *op_type)
{
    if (!op_type) {
        return false;
    }
    for (size_t i = 0; ALLOWED_OPS[i] != NULL; ++i) {
        if (strcmp(ALLOWED_OPS[i], op_type) == 0) {
            return true;
        }
    }
    return false;
}
