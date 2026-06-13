/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Tests for VmafDnnConfig.device execution-provider selection and
 *  VmafDnnConfig.fp16_io round-trip conversion. Runs only when libvmaf
 *  was built with -Denable_dnn=enabled; otherwise the asserts collapse
 *  to stub-semantics checks.
 *
 *  EP selection is verified via vmaf_dnn_session_attached_ep(), which
 *  returns the human-readable name of the EP that actually bound. On a
 *  stock CI ORT build (no CUDA/OpenVINO/ROCm compiled in) every AUTO
 *  and explicit-device selection must fall through to "CPU" without
 *  erroring — that's the behaviour this gate locks in.
 *
 *  fp16_io is verified with model/tiny/smoke_fp16_v0.onnx, a 2x2 Identity
 *  model with FLOAT16 inputs and outputs. The test feeds fp32 values,
 *  libvmaf casts to fp16 host-side, ORT runs Identity, libvmaf casts
 *  back — output must equal input within fp16-rounding tolerance.
 */

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"

#include "libvmaf/dnn.h"

/* Skip on -ENOENT (model not in tree) or -EIO (CUDA EP .so absent; see
 * DNN_OPEN_SKIP_RC in test_dnn_session_api.c for full rationale). */
#define DNN_OPEN_SKIP_RC(rc) ((rc) == -ENOENT || (rc) == -EIO)

#define SMOKE_FP32_MODEL "model/tiny/smoke_v0.onnx"
#define SMOKE_FP16_MODEL "model/tiny/smoke_fp16_v0.onnx"

static char *test_auto_falls_through_to_cpu(void)
{
    if (!vmaf_dnn_available())
        return NULL;

#if defined(__APPLE__)
    /* macOS skip: Homebrew's onnxruntime keg and Microsoft's official
     * `onnxruntime-osx-${ARCH}-*.tgz` distributions both ship only the
     * main `libonnxruntime.dylib` (providers are statically linked).
     * The runtime provider-bridge then dlopen()s a non-existent
     * `libonnxruntime_providers_shared.dylib`, which makes
     * `vmaf_dnn_session_open` fail with the AUTO device. Until either
     * the brew formula gains the providers dylib or libvmaf-dnn
     * stops calling into the provider-bridge for the AUTO path on
     * macOS, this specific assertion is skipped on `__APPLE__`.
     * See PR #758 for the failure reproducer; rest of the DNN suite
     * (model loading, session lifecycle, FP16 EP probe) still
     * exercises the macOS build. */
    return NULL;
#endif

    VmafDnnSession *sess = NULL;
    VmafDnnConfig cfg = {.device = VMAF_DNN_DEVICE_AUTO};
    int rc = vmaf_dnn_session_open(&sess, SMOKE_FP32_MODEL, &cfg);
    if (DNN_OPEN_SKIP_RC(rc)) {
        /* Test data not present or CUDA EP .so absent — skip. */
        return NULL;
    }
    mu_assert("AUTO open succeeds", rc == 0);
    mu_assert("session is non-NULL", sess != NULL);

    const char *ep = vmaf_dnn_session_attached_ep(sess);
    mu_assert("attached_ep is non-NULL", ep != NULL);
    /* CI ORT ships CPU EP only. If the runtime happens to have CUDA or
     * OpenVINO compiled in that's fine — any named EP is acceptable so
     * long as it's a known stable string. */
    mu_assert("EP name matches known set", strcmp(ep, "CPU") == 0 || strcmp(ep, "CUDA") == 0 ||
                                               strcmp(ep, "OpenVINO:GPU") == 0 ||
                                               strcmp(ep, "OpenVINO:CPU") == 0 ||
                                               strcmp(ep, "ROCm") == 0);

    vmaf_dnn_session_close(sess);
    return NULL;
}

static char *test_explicit_openvino_graceful_fallback(void)
{
    if (!vmaf_dnn_available())
        return NULL;

    /* Requesting OpenVINO on a CPU-only ORT build must silently fall
     * back to CPU rather than failing the open. This is the
     * "accepted-but-ignored" behaviour turning into "accepted-and-
     * honoured-or-logged-fallback". */
    VmafDnnSession *sess = NULL;
    VmafDnnConfig cfg = {.device = VMAF_DNN_DEVICE_OPENVINO};
    int rc = vmaf_dnn_session_open(&sess, SMOKE_FP32_MODEL, &cfg);
    if (DNN_OPEN_SKIP_RC(rc))
        return NULL;
    mu_assert("OPENVINO request does not fail open", rc == 0);

    const char *ep = vmaf_dnn_session_attached_ep(sess);
    mu_assert("EP is reported", ep != NULL);
    /* Will be CPU on stock CI ORT; OpenVINO:* if DPC++/OV-enabled build. */

    vmaf_dnn_session_close(sess);
    return NULL;
}

static char *test_explicit_coreml_graceful_fallback(void)
{
    if (!vmaf_dnn_available())
        return NULL;

    /* `--tiny-device coreml` (VMAF_DNN_DEVICE_COREML) requests the
     * CoreMLExecutionProvider with no MLComputeUnits pin (CoreML
     * auto-routes across ANE / GPU / CPU). On Linux CI runners the
     * CoreML EP is not compiled into the linked ORT — the EP register
     * call returns non-null OrtStatus, the session keeps ep_name="CPU",
     * and CreateSession runs on the CPU EP. On macOS hosts with a
     * CoreML-enabled ORT build the open succeeds with ep="CoreML".
     * Either way the session must open without erroring; this test
     * locks in the graceful-fallback behaviour. */
    VmafDnnSession *sess = NULL;
    VmafDnnConfig cfg = {.device = VMAF_DNN_DEVICE_COREML};
    int rc = vmaf_dnn_session_open(&sess, SMOKE_FP32_MODEL, &cfg);
    if (DNN_OPEN_SKIP_RC(rc))
        return NULL;
    mu_assert("COREML request does not fail open", rc == 0);

    const char *ep = vmaf_dnn_session_attached_ep(sess);
    mu_assert("EP is reported", ep != NULL);
    mu_assert("EP name matches known set", strcmp(ep, "CPU") == 0 || strcmp(ep, "CoreML") == 0);

    vmaf_dnn_session_close(sess);
    return NULL;
}

static char *test_explicit_openvino_npu_graceful_fallback(void)
{
    if (!vmaf_dnn_available())
        return NULL;

    /* `--tiny-device openvino-npu` (VMAF_DNN_DEVICE_OPENVINO_NPU) requests
     * the OpenVINOExecutionProvider with `device_type=NPU`. On hosts
     * without Intel NPU silicon (e.g. Ryzen / Arc-only / AMD-only dev
     * machines) the EP either is not compiled into the linked ORT or
     * fails CreateSession with no NPU device — the two-stage fallback in
     * vmaf_ort_open() must downgrade to the CPU EP rather than failing
     * the open. This is the "skip-when-no-NPU" behaviour the test
     * locks in: the session opens, attached_ep is reported, and
     * cleanup is clean. The exact returned EP string is host-dependent
     * (CPU when ORT is stubbed or has no OpenVINO; "OpenVINO:NPU" only
     * when both the EP and the silicon are present). */
    VmafDnnSession *sess = NULL;
    VmafDnnConfig cfg = {.device = VMAF_DNN_DEVICE_OPENVINO_NPU};
    int rc = vmaf_dnn_session_open(&sess, SMOKE_FP32_MODEL, &cfg);
    if (DNN_OPEN_SKIP_RC(rc))
        return NULL;
    mu_assert("OPENVINO_NPU request does not fail open", rc == 0);

    const char *ep = vmaf_dnn_session_attached_ep(sess);
    mu_assert("EP is reported", ep != NULL);
    mu_assert("EP name matches known set",
              strcmp(ep, "CPU") == 0 || strcmp(ep, "OpenVINO:NPU") == 0);

    vmaf_dnn_session_close(sess);
    return NULL;
}

static char *test_explicit_coreml_ane_graceful_fallback(void)
{
    if (!vmaf_dnn_available())
        return NULL;

    /* `--tiny-device coreml-ane` pins CoreML's MLComputeUnits to
     * CPUAndNeuralEngine — the highest-perf path on M-series silicon.
     * On non-Apple hosts the EP is absent and the open downgrades to
     * CPU; on macOS without ANE the EP loads but routes work to CPU+GPU
     * (CoreML's ANE-or-fallback policy). The exact attached_ep string
     * is host-dependent; we only assert open + a known EP name. */
    VmafDnnSession *sess = NULL;
    VmafDnnConfig cfg = {.device = VMAF_DNN_DEVICE_COREML_ANE};
    int rc = vmaf_dnn_session_open(&sess, SMOKE_FP32_MODEL, &cfg);
    if (DNN_OPEN_SKIP_RC(rc))
        return NULL;
    mu_assert("COREML_ANE request does not fail open", rc == 0);

    const char *ep = vmaf_dnn_session_attached_ep(sess);
    mu_assert("EP is reported", ep != NULL);
    mu_assert("EP name matches known set", strcmp(ep, "CPU") == 0 || strcmp(ep, "CoreML:ANE") == 0);

    vmaf_dnn_session_close(sess);
    return NULL;
}

static char *test_explicit_openvino_cpu_fallback_ep(void)
{
    if (!vmaf_dnn_available())
        return NULL;

    /* `--tiny-device openvino-cpu` pins OpenVINO's CPU device type. On a
     * stock CI ORT build (no OpenVINO compiled in) we still expect the
     * open to succeed via the two-stage CPU-EP fallback; the attached
     * EP will be either "OpenVINO:CPU" (when OV is present) or "CPU"
     * (stub / no-OV ORT). This is the recommended NPU-absent fallback
     * and the test that runs end-to-end on the hardware-less host. */
    VmafDnnSession *sess = NULL;
    VmafDnnConfig cfg = {.device = VMAF_DNN_DEVICE_OPENVINO_CPU};
    int rc = vmaf_dnn_session_open(&sess, SMOKE_FP32_MODEL, &cfg);
    if (DNN_OPEN_SKIP_RC(rc))
        return NULL;
    mu_assert("OPENVINO_CPU request does not fail open", rc == 0);

    const char *ep = vmaf_dnn_session_attached_ep(sess);
    mu_assert("EP is reported", ep != NULL);
    mu_assert("EP name matches known set",
              strcmp(ep, "CPU") == 0 || strcmp(ep, "OpenVINO:CPU") == 0);

    vmaf_dnn_session_close(sess);
    return NULL;
}

static char *test_explicit_coreml_cpu_graceful_fallback(void)
{
    if (!vmaf_dnn_available())
        return NULL;

    /* `--tiny-device coreml-cpu` pins CoreML's MLComputeUnits to
     * CPUOnly — universal fallback, exercised end-to-end on the
     * hardware-less Linux CI host (the EP is not compiled in but
     * the two-stage CPU-EP fallback in vmaf_ort_open keeps the
     * open succeeding). On macOS the EP loads and runs on the CPU
     * compute unit with attached_ep="CoreML:CPU". */
    VmafDnnSession *sess = NULL;
    VmafDnnConfig cfg = {.device = VMAF_DNN_DEVICE_COREML_CPU};
    int rc = vmaf_dnn_session_open(&sess, SMOKE_FP32_MODEL, &cfg);
    if (DNN_OPEN_SKIP_RC(rc))
        return NULL;
    mu_assert("COREML_CPU request does not fail open", rc == 0);

    const char *ep = vmaf_dnn_session_attached_ep(sess);
    mu_assert("EP is reported", ep != NULL);
    mu_assert("EP name matches known set", strcmp(ep, "CPU") == 0 || strcmp(ep, "CoreML:CPU") == 0);

    vmaf_dnn_session_close(sess);
    return NULL;
}

static char *test_explicit_openvino_gpu_graceful_fallback(void)
{
    if (!vmaf_dnn_available())
        return NULL;

    /* `--tiny-device openvino-gpu` pins OpenVINO's GPU device type
     * (`GPU.0` alias). Without an Intel iGPU/dGPU plus the OpenVINO
     * intel_gpu plugin the open downgrades to CPU. */
    VmafDnnSession *sess = NULL;
    VmafDnnConfig cfg = {.device = VMAF_DNN_DEVICE_OPENVINO_GPU};
    int rc = vmaf_dnn_session_open(&sess, SMOKE_FP32_MODEL, &cfg);
    if (DNN_OPEN_SKIP_RC(rc))
        return NULL;
    mu_assert("OPENVINO_GPU request does not fail open", rc == 0);

    const char *ep = vmaf_dnn_session_attached_ep(sess);
    mu_assert("EP is reported", ep != NULL);
    mu_assert("EP name matches known set",
              strcmp(ep, "CPU") == 0 || strcmp(ep, "OpenVINO:GPU") == 0);

    vmaf_dnn_session_close(sess);
    return NULL;
}

static char *test_explicit_cuda_graceful_fallback(void)
{
    if (!vmaf_dnn_available())
        return NULL;

    VmafDnnSession *sess = NULL;
    VmafDnnConfig cfg = {.device = VMAF_DNN_DEVICE_CUDA};
    int rc = vmaf_dnn_session_open(&sess, SMOKE_FP32_MODEL, &cfg);
    if (DNN_OPEN_SKIP_RC(rc))
        return NULL;
    mu_assert("CUDA request does not fail open", rc == 0);

    const char *ep = vmaf_dnn_session_attached_ep(sess);
    mu_assert("EP is reported", ep != NULL);

    vmaf_dnn_session_close(sess);
    return NULL;
}

static char *test_fp16_io_round_trip(void)
{
    if (!vmaf_dnn_available())
        return NULL;

    VmafDnnSession *sess = NULL;
    VmafDnnConfig cfg = {.device = VMAF_DNN_DEVICE_CPU, .fp16_io = true};
    int rc = vmaf_dnn_session_open(&sess, SMOKE_FP16_MODEL, &cfg);
    if (DNN_OPEN_SKIP_RC(rc))
        return NULL;
    mu_assert("fp16 session open succeeds", rc == 0);

    /* Identity model: output must equal input within fp16 rounding
     * error. Values chosen to stay exactly representable in fp16 so the
     * assertion is tight. Non-exact values would introduce ±0.5 ULP of
     * fp16 precision on top — still detectable as a bug but a looser
     * tolerance would mask real cast-direction errors. */
    const float in_data[4] = {0.0f, 1.0f, -0.5f, 256.0f};
    float out_data[4] = {9999.f, 9999.f, 9999.f, 9999.f};
    const int64_t shape[4] = {1, 1, 2, 2};

    VmafDnnInput in = {.name = "x", .data = in_data, .shape = shape, .rank = 4};
    VmafDnnOutput out = {.name = "y", .data = out_data, .capacity = 4, .written = 0};

    rc = vmaf_dnn_session_run(sess, &in, 1, &out, 1);
    mu_assert("fp16 Identity run succeeds", rc == 0);
    mu_assert("output element count is 4", out.written == 4);

    for (size_t i = 0; i < 4; ++i) {
        const float diff = fabsf(out_data[i] - in_data[i]);
        mu_assert("fp16 round-trip matches input exactly", diff == 0.0f);
    }

    vmaf_dnn_session_close(sess);
    return NULL;
}

static char *test_fp16_io_edge_values(void)
{
    if (!vmaf_dnn_available())
        return NULL;

    /* Round-trip edge values through the fp16 IO path: subnormal, overflow
     * (→ inf), underflow (→ ±0), and a value that's exactly representable
     * but outside the small-integer range. Drives the inf/nan, overflow,
     * underflow, and subnormal branches of fp32_to_fp16 and fp16_to_fp32.
     * The Identity model gives us output == input modulo fp16 representation,
     * so we assert direction-preserving properties (sign, finiteness,
     * magnitude bracket), not bit-exact equality. */
    VmafDnnSession *sess = NULL;
    VmafDnnConfig cfg = {.device = VMAF_DNN_DEVICE_CPU, .fp16_io = true};
    int rc = vmaf_dnn_session_open(&sess, SMOKE_FP16_MODEL, &cfg);
    if (DNN_OPEN_SKIP_RC(rc))
        return NULL;
    mu_assert("fp16 edge open succeeds", rc == 0);

    /* 1.0e-7f underflows fp16 (smallest normal ≈ 6.1e-5), 1.0e-5f is
     * subnormal in fp16, 1.0e10f overflows fp16 (max ≈ 65504). */
    const float in_data[4] = {1.0e-7f, 1.0e-5f, 1.0e10f, -1.0e10f};
    float out_data[4] = {9999.f, 9999.f, 9999.f, 9999.f};
    const int64_t shape[4] = {1, 1, 2, 2};

    VmafDnnInput in = {.name = "x", .data = in_data, .shape = shape, .rank = 4};
    VmafDnnOutput out = {.name = "y", .data = out_data, .capacity = 4, .written = 0};

    rc = vmaf_dnn_session_run(sess, &in, 1, &out, 1);
    mu_assert("fp16 edge run succeeds", rc == 0);
    mu_assert("fp16 edge output count is 4", out.written == 4);

    /* 1e-7 → underflow → ±0 (assert magnitude near zero) */
    mu_assert("fp16 underflow → near zero", fabsf(out_data[0]) < 1.0e-6f);
    /* 1e-5 → subnormal in fp16 → small positive value */
    mu_assert("fp16 subnormal → small positive", out_data[1] > 0.0f && out_data[1] < 1.0e-3f);
    /* 1e10 → overflow → +inf */
    mu_assert("fp16 +overflow → +inf", isinf(out_data[2]) && out_data[2] > 0.0f);
    /* -1e10 → overflow → -inf */
    mu_assert("fp16 -overflow → -inf", isinf(out_data[3]) && out_data[3] < 0.0f);

    vmaf_dnn_session_close(sess);
    return NULL;
}

static char *test_fp16_model_rejects_fp32_config(void)
{
    if (!vmaf_dnn_available())
        return NULL;

    /* Opening a FLOAT16-typed model with fp16_io=false: the session
     * still opens (ORT accepts the graph), but a run with fp32 input
     * buffers must fail because we hand ORT a FLOAT-typed tensor for a
     * FLOAT16-declared input slot. We assert the run failure, not the
     * open failure — see ADR-0102 rationale. */
    VmafDnnSession *sess = NULL;
    VmafDnnConfig cfg = {.device = VMAF_DNN_DEVICE_CPU, .fp16_io = false};
    int rc = vmaf_dnn_session_open(&sess, SMOKE_FP16_MODEL, &cfg);
    if (DNN_OPEN_SKIP_RC(rc))
        return NULL;
    mu_assert("fp16 model opens under fp16_io=false", rc == 0);

    const float in_data[4] = {0.f, 1.f, 2.f, 3.f};
    float out_data[4] = {0.f, 0.f, 0.f, 0.f};
    const int64_t shape[4] = {1, 1, 2, 2};
    VmafDnnInput in = {.name = "x", .data = in_data, .shape = shape, .rank = 4};
    VmafDnnOutput out = {.name = "y", .data = out_data, .capacity = 4, .written = 0};

    rc = vmaf_dnn_session_run(sess, &in, 1, &out, 1);
    mu_assert("fp16 model rejects fp32 tensor when fp16_io=false", rc < 0);

    vmaf_dnn_session_close(sess);
    return NULL;
}

static char *test_stub_attached_ep_returns_null(void)
{
    if (vmaf_dnn_available())
        return NULL;
    /* When DNN is disabled, the accessor is defined but returns NULL for
     * any session (which can only be NULL via the stub path anyway). */
    mu_assert("stub attached_ep returns NULL", vmaf_dnn_session_attached_ep(NULL) == NULL);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_auto_falls_through_to_cpu);
    mu_run_test(test_explicit_openvino_graceful_fallback);
    mu_run_test(test_explicit_coreml_graceful_fallback);
    mu_run_test(test_explicit_coreml_ane_graceful_fallback);
    mu_run_test(test_explicit_coreml_cpu_graceful_fallback);
    mu_run_test(test_explicit_openvino_npu_graceful_fallback);
    mu_run_test(test_explicit_openvino_cpu_fallback_ep);
    mu_run_test(test_explicit_openvino_gpu_graceful_fallback);
    mu_run_test(test_explicit_cuda_graceful_fallback);
    mu_run_test(test_fp16_io_round_trip);
    mu_run_test(test_fp16_io_edge_values);
    mu_run_test(test_fp16_model_rejects_fp32_config);
    mu_run_test(test_stub_attached_ep_returns_null);
    return NULL;
}
