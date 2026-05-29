/**
 *  Copyright 2026 Lusoris and Claude (Anthropic)
 *  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
 *
 *  Direct unit tests for ort_backend.c internal helpers and NULL-guard
 *  branches. Exists because many of ort_backend.c's branches are
 *  fundamentally unreachable through the public libvmaf/dnn.h surface
 *  on a CPU-only ORT CI build (EP-attach success, ORT-API failure,
 *  fp16 conversion edges) — the wrappers in ort_backend_internal.h
 *  let us drive them directly without standing up a full session.
 *  See ADR-0112.
 *
 *  Stub-build behaviour: every test short-circuits to a no-op when
 *  vmaf_dnn_available() is 0, mirroring the rest of the dnn/ test
 *  suite.
 */

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"

#include "libvmaf/dnn.h"
#include "ort_backend.h"
#include "ort_backend_internal.h"

#define SMOKE_FP32_MODEL "model/tiny/smoke_v0.onnx"
#define SMOKE_FP16_MODEL "model/tiny/smoke_fp16_v0.onnx"

/* ---------- fp32 ↔ fp16 conversion --------------------------------- */

static char *test_fp32_to_fp16_normal(void)
{
    if (!vmaf_dnn_available())
        return NULL;
    /* +1.0f → exp_f = 0, mant = 0 → h_exp = 15, h_mant = 0 → 0x3C00 */
    mu_assert("fp32→fp16: +1.0", vmaf_ort_internal_fp32_to_fp16(1.0f) == 0x3C00);
    /* -1.0f → sign bit set */
    mu_assert("fp32→fp16: -1.0", vmaf_ort_internal_fp32_to_fp16(-1.0f) == 0xBC00);
    /* 0.0f → all zero */
    mu_assert("fp32→fp16: +0.0", vmaf_ort_internal_fp32_to_fp16(0.0f) == 0x0000);
    return NULL;
}

static char *test_fp32_to_fp16_inf_nan(void)
{
    if (!vmaf_dnn_available())
        return NULL;
    /* +inf → 0x7C00 (sign 0, exp 31, mant 0). Hits L66-69 inf/nan branch. */
    mu_assert("fp32→fp16: +inf", vmaf_ort_internal_fp32_to_fp16(INFINITY) == 0x7C00);
    /* -inf → 0xFC00 */
    mu_assert("fp32→fp16: -inf", vmaf_ort_internal_fp32_to_fp16(-INFINITY) == 0xFC00);
    /* NaN → exponent 31, non-zero mantissa */
    const uint16_t nan_h = vmaf_ort_internal_fp32_to_fp16(nanf(""));
    mu_assert("fp32→fp16: nan exponent", (nan_h & 0x7C00) == 0x7C00);
    mu_assert("fp32→fp16: nan mantissa non-zero", (nan_h & 0x03FF) != 0);
    return NULL;
}

static char *test_fp32_to_fp16_overflow(void)
{
    if (!vmaf_dnn_available())
        return NULL;
    /* 1e10 > fp16 max (65504) → overflow → ±inf. Hits L70-73. */
    mu_assert("fp32→fp16: overflow → +inf", vmaf_ort_internal_fp32_to_fp16(1.0e10f) == 0x7C00);
    mu_assert("fp32→fp16: overflow → -inf", vmaf_ort_internal_fp32_to_fp16(-1.0e10f) == 0xFC00);
    return NULL;
}

static char *test_fp32_to_fp16_underflow(void)
{
    if (!vmaf_dnn_available())
        return NULL;
    /* exp_f < -24 → underflow → ±0. 1e-10f has exp_f ≈ -34. Hits L74-77. */
    mu_assert("fp32→fp16: underflow → +0", vmaf_ort_internal_fp32_to_fp16(1.0e-10f) == 0x0000);
    mu_assert("fp32→fp16: underflow → -0", vmaf_ort_internal_fp32_to_fp16(-1.0e-10f) == 0x8000);
    return NULL;
}

static char *test_fp32_to_fp16_subnormal(void)
{
    if (!vmaf_dnn_available())
        return NULL;
    /* exp_f in [-24, -15] → subnormal half. 1e-5f has exp_f ≈ -17. Hits L78-83.
     * Just assert the result is a valid subnormal (exponent bits 0, mantissa
     * non-zero) — the exact bit pattern depends on rounding. */
    const uint16_t h = vmaf_ort_internal_fp32_to_fp16(1.0e-5f);
    mu_assert("fp32→fp16: subnormal exp == 0", (h & 0x7C00) == 0x0000);
    mu_assert("fp32→fp16: subnormal mant != 0", (h & 0x03FF) != 0);
    return NULL;
}

static char *test_fp16_to_fp32_normal(void)
{
    if (!vmaf_dnn_available())
        return NULL;
    /* Round-trip identity: every normal finite fp16 maps back to its fp32
     * exact value, then back to the same fp16 bits. */
    mu_assert("fp16→fp32: +1.0", vmaf_ort_internal_fp16_to_fp32(0x3C00) == 1.0f);
    mu_assert("fp16→fp32: -1.0", vmaf_ort_internal_fp16_to_fp32(0xBC00) == -1.0f);
    return NULL;
}

static char *test_fp16_to_fp32_zero(void)
{
    if (!vmaf_dnn_available())
        return NULL;
    /* exp_h == 0, mant == 0 → ±0. Hits L96-99. */
    mu_assert("fp16→fp32: +0", vmaf_ort_internal_fp16_to_fp32(0x0000) == 0.0f);
    /* signbit check: bit pattern of -0.0f differs from +0.0f. */
    const float neg_zero = vmaf_ort_internal_fp16_to_fp32(0x8000);
    mu_assert("fp16→fp32: -0 is zero", neg_zero == 0.0f);
    mu_assert("fp16→fp32: -0 has sign bit", signbit(neg_zero));
    return NULL;
}

static char *test_fp16_to_fp32_subnormal(void)
{
    if (!vmaf_dnn_available())
        return NULL;
    /* IEEE 754 subnormal half: value = mant × 2^-24.
     * Smallest positive subnormal 0x0001 → 2^-24 ≈ 5.960e-8. */
    const float vmin = vmaf_ort_internal_fp16_to_fp32(0x0001);
    const float kSmallest = 5.9604644775390625e-8f; /* 2^-24, exact in fp32 */
    mu_assert("fp16→fp32: smallest subnormal == 2^-24", vmin == kSmallest);
    /* Largest subnormal 0x03FF → (1023/1024) × 2^-14 ≈ 6.0976e-5.
     * Earlier off-by-one returned 2× this value (1.22e-4); the exact
     * equality below regression-locks the fix. */
    const float vmax = vmaf_ort_internal_fp16_to_fp32(0x03FF);
    const float kLargest = 1023.0f * 5.9604644775390625e-8f; /* 1023 × 2^-24 */
    mu_assert("fp16→fp32: largest subnormal == 1023 × 2^-24", vmax == kLargest);
    /* Mid-range subnormal 0x0200 → 512 × 2^-24 = 2^-15 (exactly normal-edge). */
    const float vmid = vmaf_ort_internal_fp16_to_fp32(0x0200);
    const float kMid = 512.0f * 5.9604644775390625e-8f; /* 2^-15 */
    mu_assert("fp16→fp32: mid subnormal == 512 × 2^-24", vmid == kMid);
    return NULL;
}

static char *test_fp16_to_fp32_inf_nan(void)
{
    if (!vmaf_dnn_available())
        return NULL;
    /* exp_h == 31 → inf or NaN. Hits L109-110. */
    mu_assert("fp16→fp32: +inf", isinf(vmaf_ort_internal_fp16_to_fp32(0x7C00)));
    mu_assert("fp16→fp32: -inf", isinf(vmaf_ort_internal_fp16_to_fp32(0xFC00)));
    mu_assert("fp16→fp32: nan", isnan(vmaf_ort_internal_fp16_to_fp32(0x7E00)));
    return NULL;
}

/* ---------- resolve_name ------------------------------------------- */

static char *test_resolve_name_hit(void)
{
    if (!vmaf_dnn_available())
        return NULL;
    char *names[3] = {(char *)"alpha", (char *)"beta", (char *)"gamma"};
    const char *r = vmaf_ort_internal_resolve_name(names, 3u, "beta", 0u);
    mu_assert("resolve_name hit returns table entry", r == names[1]);
    return NULL;
}

static char *test_resolve_name_miss(void)
{
    if (!vmaf_dnn_available())
        return NULL;
    char *names[2] = {(char *)"a", (char *)"b"};
    const char *r = vmaf_ort_internal_resolve_name(names, 2u, "no-such-name", 0u);
    mu_assert("resolve_name miss returns NULL", r == NULL);
    return NULL;
}

static char *test_resolve_name_positional(void)
{
    if (!vmaf_dnn_available())
        return NULL;
    char *names[2] = {(char *)"x", (char *)"y"};
    /* NULL name → positional fallback at @p pos. Hits L551-554 happy branch. */
    mu_assert("resolve_name NULL→pos[0]",
              vmaf_ort_internal_resolve_name(names, 2u, NULL, 0u) == names[0]);
    mu_assert("resolve_name NULL→pos[1]",
              vmaf_ort_internal_resolve_name(names, 2u, NULL, 1u) == names[1]);
    return NULL;
}

static char *test_resolve_name_positional_out_of_range(void)
{
    if (!vmaf_dnn_available())
        return NULL;
    char *names[2] = {(char *)"x", (char *)"y"};
    /* NULL name + pos >= count → NULL. Hits L552-553 (uncovered before). */
    mu_assert("resolve_name pos >= count → NULL",
              vmaf_ort_internal_resolve_name(names, 2u, NULL, 5u) == NULL);
    return NULL;
}

/* ---------- ort_backend NULL-guard branches ------------------------ */
/* These guards short-circuit before any ORT call, so they're safe to
 * exercise without a session. They reach lines uncovered through the
 * public dnn.h API because libvmaf/src/dnn/dnn_api.c validates inputs
 * one layer up and never passes NULLs through. */

static char *test_ort_attached_ep_null_session(void)
{
    if (!vmaf_dnn_available())
        return NULL;
    mu_assert("vmaf_ort_attached_ep(NULL) → NULL", vmaf_ort_attached_ep(NULL) == NULL);
    return NULL;
}

static char *test_ort_close_null_session(void)
{
    if (!vmaf_dnn_available())
        return NULL;
    /* Must be a no-op, not a crash. */
    vmaf_ort_close(NULL);
    return NULL;
}

static char *test_ort_io_count_null_args(void)
{
    if (!vmaf_dnn_available())
        return NULL;
    size_t a = 0;
    size_t b = 0;
    mu_assert("io_count NULL sess", vmaf_ort_io_count(NULL, &a, &b) == -EINVAL);
    /* Need a real session for the other NULL-arg paths. Open one cheaply. */
    VmafOrtSession *sess = NULL;
    int rc = vmaf_ort_open(&sess, SMOKE_FP32_MODEL, NULL);
    if (rc == -ENOENT)
        return NULL;
    mu_assert("io_count: open succeeds", rc == 0);
    mu_assert("io_count NULL n_inputs", vmaf_ort_io_count(sess, NULL, &b) == -EINVAL);
    mu_assert("io_count NULL n_outputs", vmaf_ort_io_count(sess, &a, NULL) == -EINVAL);
    /* Valid call to cover the success path */
    rc = vmaf_ort_io_count(sess, &a, &b);
    mu_assert("io_count valid", rc == 0);
    mu_assert("io_count: at least 1 input", a >= 1u);
    mu_assert("io_count: at least 1 output", b >= 1u);
    vmaf_ort_close(sess);
    return NULL;
}

static char *test_ort_input_shape_null_args(void)
{
    if (!vmaf_dnn_available())
        return NULL;
    int64_t shape[4] = {0};
    size_t rank = 0;
    /* All NULL-guard combinations. Hits L455. */
    mu_assert("input_shape NULL sess", vmaf_ort_input_shape(NULL, shape, 4u, &rank) == -EINVAL);
    /* Open a session for the other NULL-arg paths. */
    VmafOrtSession *sess = NULL;
    int rc = vmaf_ort_open(&sess, SMOKE_FP32_MODEL, NULL);
    if (rc == -ENOENT)
        return NULL;
    mu_assert("input_shape: open succeeds", rc == 0);
    mu_assert("input_shape NULL out_shape", vmaf_ort_input_shape(sess, NULL, 4u, &rank) == -EINVAL);
    mu_assert("input_shape NULL out_rank", vmaf_ort_input_shape(sess, shape, 4u, NULL) == -EINVAL);
    mu_assert("input_shape max_rank=0", vmaf_ort_input_shape(sess, shape, 0u, &rank) == -EINVAL);
    vmaf_ort_close(sess);
    return NULL;
}

static char *test_ort_input_shape_at_bounds_and_success(void)
{
    if (!vmaf_dnn_available())
        return NULL;
    int64_t shape[4] = {0};
    size_t rank = 0;
    VmafOrtSession *sess = NULL;
    int rc = vmaf_ort_open(&sess, SMOKE_FP32_MODEL, NULL);
    if (rc == -ENOENT)
        return NULL;
    mu_assert("input_shape_at: open succeeds", rc == 0);

    mu_assert("input_shape_at NULL sess",
              vmaf_ort_input_shape_at(NULL, 0u, shape, 4u, &rank) == -EINVAL);
    mu_assert("input_shape_at NULL out_shape",
              vmaf_ort_input_shape_at(sess, 0u, NULL, 4u, &rank) == -EINVAL);
    mu_assert("input_shape_at NULL out_rank",
              vmaf_ort_input_shape_at(sess, 0u, shape, 4u, NULL) == -EINVAL);
    mu_assert("input_shape_at max_rank=0",
              vmaf_ort_input_shape_at(sess, 0u, shape, 0u, &rank) == -EINVAL);
    mu_assert("input_shape_at slot out of range",
              vmaf_ort_input_shape_at(sess, 99u, shape, 4u, &rank) == -ERANGE);
    mu_assert("input_shape_at max_rank too small",
              vmaf_ort_input_shape_at(sess, 0u, shape, 1u, &rank) == -ERANGE);

    rc = vmaf_ort_input_shape_at(sess, 0u, shape, 4u, &rank);
    mu_assert("input_shape_at success", rc == 0);
    mu_assert("input_shape_at rank 4", rank == 4u);
    mu_assert("input_shape_at dims match smoke model",
              shape[0] == 1 && shape[1] == 1 && shape[2] == 4 && shape[3] == 4);

    rank = 0;
    memset(shape, 0, sizeof(shape));
    rc = vmaf_ort_input_shape(sess, shape, 4u, &rank);
    mu_assert("input_shape success", rc == 0);
    mu_assert("input_shape rank 4", rank == 4u);
    mu_assert("input_shape max_rank too small",
              vmaf_ort_input_shape(sess, shape, 1u, &rank) == -ERANGE);

    vmaf_ort_close(sess);
    return NULL;
}

static char *test_ort_infer_guards_and_smoke_paths(void)
{
    if (!vmaf_dnn_available())
        return NULL;
    float input[16] = {0.0f};
    float output[16] = {0.0f};
    int64_t shape[4] = {1, 1, 4, 4};
    size_t written = 0;

    mu_assert("infer NULL sess",
              vmaf_ort_infer(NULL, input, shape, 4u, output, 16u, &written) == -EINVAL);

    VmafOrtSession *sess = NULL;
    int rc = vmaf_ort_open(&sess, SMOKE_FP32_MODEL, NULL);
    if (rc == -ENOENT)
        return NULL;
    mu_assert("infer: open succeeds", rc == 0);
    mu_assert("infer NULL input",
              vmaf_ort_infer(sess, NULL, shape, 4u, output, 16u, &written) == -EINVAL);
    mu_assert("infer NULL shape",
              vmaf_ort_infer(sess, input, NULL, 4u, output, 16u, &written) == -EINVAL);
    mu_assert("infer NULL output",
              vmaf_ort_infer(sess, input, shape, 4u, NULL, 16u, &written) == -EINVAL);

    rc = vmaf_ort_infer(sess, input, shape, 4u, output, 16u, &written);
    mu_assert("infer fp32 smoke succeeds", rc == 0);
    mu_assert("infer fp32 written count", written == 16u);
    written = 0;
    rc = vmaf_ort_infer(sess, input, shape, 4u, output, 1u, &written);
    mu_assert("infer undersized output reports ENOSPC", rc == -ENOSPC);
    mu_assert("infer undersized output required count", written == 16u);

    vmaf_ort_close(sess);
    return NULL;
}

static char *test_ort_infer_fp16_input_output_path(void)
{
    if (!vmaf_dnn_available())
        return NULL;
    float input[4] = {0.0f};
    float output[4] = {0.0f};
    int64_t shape[4] = {1, 1, 2, 2};
    size_t written = 0;
    VmafOrtSession *sess = NULL;
    VmafDnnConfig cfg = {.device = VMAF_DNN_DEVICE_CPU, .fp16_io = true};
    int rc = vmaf_ort_open(&sess, SMOKE_FP16_MODEL, &cfg);
    if (rc == -ENOENT)
        return NULL;
    mu_assert("fp16 smoke model open succeeds", rc == 0);

    for (size_t i = 0; i < 4u; ++i)
        input[i] = (float)i / 16.0f;
    rc = vmaf_ort_infer(sess, input, shape, 4u, output, 4u, &written);
    mu_assert("fp16 infer succeeds", rc == 0);
    mu_assert("fp16 infer written count", written == 4u);

    vmaf_ort_close(sess);
    return NULL;
}

static char *test_ort_run_null_guards(void)
{
    if (!vmaf_dnn_available())
        return NULL;
    /* All NULL-guard combinations on vmaf_ort_run. Hits L566-567.
     * VmafOrtSession is opaque, so we open a real session for the cases
     * that pass a non-NULL sess. The early-return on bad args fires
     * before any sess deref, so the session is never actually used. */
    VmafOrtTensorIn ti = {0};
    VmafOrtTensorOut to = {0};
    mu_assert("run NULL sess", vmaf_ort_run(NULL, &ti, 1u, &to, 1u) == -EINVAL);

    VmafOrtSession *sess = NULL;
    int rc = vmaf_ort_open(&sess, SMOKE_FP32_MODEL, NULL);
    if (rc == -ENOENT)
        return NULL;
    mu_assert("run-guards: open succeeds", rc == 0);
    mu_assert("run NULL inputs", vmaf_ort_run(sess, NULL, 1u, &to, 1u) == -EINVAL);
    mu_assert("run NULL outputs", vmaf_ort_run(sess, &ti, 1u, NULL, 1u) == -EINVAL);
    mu_assert("run zero n_inputs", vmaf_ort_run(sess, &ti, 0u, &to, 1u) == -EINVAL);
    mu_assert("run zero n_outputs", vmaf_ort_run(sess, &ti, 1u, &to, 0u) == -EINVAL);
    vmaf_ort_close(sess);
    return NULL;
}

static char *test_ort_open_null_args(void)
{
    if (!vmaf_dnn_available())
        return NULL;
    /* Hits L185 (NULL out / NULL onnx_path). */
    VmafOrtSession *sess = NULL;
    mu_assert("open NULL out", vmaf_ort_open(NULL, SMOKE_FP32_MODEL, NULL) == -EINVAL);
    mu_assert("open NULL onnx_path", vmaf_ort_open(&sess, NULL, NULL) == -EINVAL);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_fp32_to_fp16_normal);
    mu_run_test(test_fp32_to_fp16_inf_nan);
    mu_run_test(test_fp32_to_fp16_overflow);
    mu_run_test(test_fp32_to_fp16_underflow);
    mu_run_test(test_fp32_to_fp16_subnormal);
    mu_run_test(test_fp16_to_fp32_normal);
    mu_run_test(test_fp16_to_fp32_zero);
    mu_run_test(test_fp16_to_fp32_subnormal);
    mu_run_test(test_fp16_to_fp32_inf_nan);
    mu_run_test(test_resolve_name_hit);
    mu_run_test(test_resolve_name_miss);
    mu_run_test(test_resolve_name_positional);
    mu_run_test(test_resolve_name_positional_out_of_range);
    mu_run_test(test_ort_attached_ep_null_session);
    mu_run_test(test_ort_close_null_session);
    mu_run_test(test_ort_io_count_null_args);
    mu_run_test(test_ort_input_shape_null_args);
    mu_run_test(test_ort_input_shape_at_bounds_and_success);
    mu_run_test(test_ort_infer_guards_and_smoke_paths);
    mu_run_test(test_ort_infer_fp16_input_output_path);
    mu_run_test(test_ort_run_null_guards);
    mu_run_test(test_ort_open_null_args);
    return NULL;
}
