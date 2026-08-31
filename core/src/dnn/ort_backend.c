/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  ONNX Runtime C-API wrapper. Selects CPU / CUDA / OpenVINO / ROCm execution
 *  providers per VmafDnnConfig, validates ops against op_allowlist.c, and
 *  runs single-input single-output inference for FR regressors (C1) and
 *  NR metrics (C2). Learned-filter (C3) inference reuses the same session
 *  with a larger output tensor.
 */

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ort_backend.h"
#include "ort_backend_internal.h"

#if defined(VMAF_HAVE_DNN) && VMAF_HAVE_DNN

#include <onnxruntime_c_api.h>

#include "../log.h"
#include "op_allowlist.h"

/* Maximum number of model inputs or outputs supported by vmaf_ort_run().
 * All current tiny-AI models are 1-in/1-out (NR) or 5-in/1-out (FR);
 * 8 leaves headroom without triggering VLA (banned by Power-of-10). */
#define VMAF_ORT_MAX_IO 8

struct VmafOrtSession {
    const OrtApi *api;
    OrtSession *session;
    OrtSessionOptions *opts;
    OrtAllocator *alloc;
    /* input_name / output_name mirror names[0] for the single-input legacy
     * path (vmaf_ort_infer). names/n_inputs/n_outputs cover the full graph
     * IO for vmaf_ort_run. */
    char *input_name;
    char *output_name;
    char **input_names;
    char **output_names;
    size_t n_inputs;
    size_t n_outputs;
    /* Per-IO element type (ONNXTensorElementDataType). Populated at open
     * time so vmaf_ort_infer/run can decide whether to emit fp32 or fp16
     * tensors to ORT without re-querying the model each call. */
    int *input_elem_types;
    int *output_elem_types;
    bool fp16_io;
    /* EP actually attached, for diagnostics. "CPU" when nothing else bound. */
    const char *ep_name;
    /* Cached CPU memory info — created once at open, released at close.
     * Avoids a per-frame ORT round-trip in vmaf_ort_infer / vmaf_ort_run
     * (perf audit finding F2-A / F3-A, 2026-05-16). */
    OrtMemoryInfo *cpu_mem_info;
};

/* ------------------------------------------------------------------ */
/* Portable IEEE 754 half-precision conversion.                        */
/* Avoids depending on _Float16 / F16C intrinsics so the DNN backend    */
/* still builds on hosts without hardware fp16 support (older SSE      */
/* x86_64, ARMv7). Handles inf / nan / subnormals / overflow.          */
/* ------------------------------------------------------------------ */

static uint16_t fp32_to_fp16(float f)
{
    uint32_t x;
    memcpy(&x, &f, sizeof(x));
    const uint32_t sign = (x >> 31) & 0x1u;
    const int32_t exp_f = (int32_t)((x >> 23) & 0xFFu) - 127;
    uint32_t mant = x & 0x7FFFFFu;

    if (exp_f == 128) {
        /* inf / nan */
        return (uint16_t)((sign << 15) | 0x7C00u | (mant ? 0x200u : 0u));
    }
    if (exp_f > 15) {
        /* overflow → inf */
        return (uint16_t)((sign << 15) | 0x7C00u);
    }
    if (exp_f < -24) {
        /* underflow → ±0 */
        return (uint16_t)(sign << 15);
    }
    if (exp_f < -14) {
        /* subnormal half */
        mant |= 0x800000u;
        const uint32_t shift = (uint32_t)(-exp_f - 14) + 13u;
        return (uint16_t)((sign << 15) | (mant >> shift));
    }
    /* Round to nearest-even on the dropped mantissa bits (bit 12 is the
     * round bit), matching f32_to_f16_one in tensor_io.c. A plain truncation
     * here mis-encoded values in (65504, 65520) as max-finite f16 instead of
     * +inf, diverging from the other converter. The carry from h_mant==0x3ff
     * naturally propagates into the exponent (and to 0x7c00 = inf). */
    const uint32_t round = (mant & 0x1000u) != 0u ? 1u : 0u;
    const uint32_t h = ((uint32_t)sign << 15) | ((uint32_t)(exp_f + 15) << 10) | (mant >> 13);
    return (uint16_t)(h + round);
}

static float fp16_to_fp32(uint16_t h)
{
    const uint32_t sign = (uint32_t)(h >> 15) & 0x1u;
    uint32_t exp_h = (uint32_t)(h >> 10) & 0x1Fu;
    uint32_t mant = (uint32_t)h & 0x3FFu;
    uint32_t x;

    if (exp_h == 0u) {
        if (mant == 0u) {
            x = sign << 31;
        } else {
            /* Subnormal half. Loop normalises the mantissa: each shift
             * moves the leading 1 toward bit 10. The loop body counts
             * one shift more than the leading-zero distance (the
             * iteration that *places* the implicit 1 also runs), so the
             * fp32 biased exponent is (127 - 15 - e), not (127 - 14 - e).
             * The earlier formula doubled the magnitude of every
             * subnormal (e.g. 0x03FF → 1.22e-4 instead of 6.10e-5). */
            int32_t e = -1;
            do {
                mant <<= 1;
                ++e;
            } while ((mant & 0x400u) == 0u);
            mant &= 0x3FFu;
            x = (sign << 31) | (uint32_t)((127 - 15 - e) << 23) | (mant << 13);
        }
    } else if (exp_h == 31u) {
        x = (sign << 31) | 0x7F800000u | (mant << 13);
    } else {
        x = (sign << 31) | ((exp_h + 112u) << 23) | (mant << 13);
    }

    float f;
    memcpy(&f, &x, sizeof(f));
    return f;
}

/* ------------------------------------------------------------------ */
/* Execution-provider selection.                                       */
/* ORT's generic SessionOptionsAppendExecutionProvider returns         */
/* non-null OrtStatus when the requested EP isn't registered in this   */
/* ORT build — we treat that as "EP unavailable, try next" and fall    */
/* through to the CPU EP, which is always linked.                      */
/* ------------------------------------------------------------------ */

static int try_append_ep_generic(struct VmafOrtSession *sess, const char *name,
                                 const char *const *keys, const char *const *values, size_t nk)
{
    OrtStatus *st =
        sess->api->SessionOptionsAppendExecutionProvider(sess->opts, name, keys, values, nk);
    if (st != NULL) {
        const char *msg = sess->api->GetErrorMessage(st);
        if (msg && msg[0] != '\0')
            vmaf_log(VMAF_LOG_LEVEL_DEBUG, "libvmaf dnn: EP '%s' unavailable: %s\n", name, msg);
        sess->api->ReleaseStatus(st);
        return -ENOSYS;
    }
    return 0;
}

static int try_append_cuda(struct VmafOrtSession *sess, int device_index)
{
    OrtCUDAProviderOptions cuda = {0};
    cuda.device_id = device_index;
    OrtStatus *st = sess->api->SessionOptionsAppendExecutionProvider_CUDA(sess->opts, &cuda);
    if (st != NULL) {
        const char *msg = sess->api->GetErrorMessage(st);
        if (msg && msg[0] != '\0')
            vmaf_log(VMAF_LOG_LEVEL_DEBUG, "libvmaf dnn: CUDA EP unavailable: %s\n", msg);
        sess->api->ReleaseStatus(st);
        return -ENOSYS;
    }
    return 0;
}

static int try_append_openvino(struct VmafOrtSession *sess, const char *device_type, bool fp16_io)
{
    const char *keys[2];
    const char *values[2];
    size_t nk = 0u;
    keys[nk] = "device_type";
    values[nk] = device_type;
    ++nk;
    if (fp16_io) {
        keys[nk] = "precision";
        values[nk] = "FP16";
        ++nk;
    }
    return try_append_ep_generic(sess, "OpenVINOExecutionProvider", keys, values, nk);
}

static int try_append_rocm(struct VmafOrtSession *sess)
{
    return try_append_ep_generic(sess, "ROCMExecutionProvider", NULL, NULL, 0u);
}

/* Apple CoreML execution provider. Targets the Apple Neural Engine (ANE),
 * Metal-backed GPU, and CPU on M-series and Intel Macs. The CoreML EP key
 * "MLComputeUnits" pins a single compute unit; valid values are
 * "ALL" (auto-route — default when key is absent), "CPUOnly",
 * "CPUAndGPU", "CPUAndNeuralEngine".
 *
 * API surface used here is the generic
 * `SessionOptionsAppendExecutionProvider("CoreMLExecutionProvider", ...)`
 * key/value form rather than the older
 * `OrtSessionOptionsAppendExecutionProvider_CoreML(opts, uint32_t flags)`
 * factory in `coreml_provider_factory.h`. The generic form needs no extra
 * header and degrades cleanly on Linux ORT builds (no CoreML EP linked
 * → non-null OrtStatus → -ENOSYS → CPU fallback).
 *
 * Reference: ONNX Runtime CoreML Execution Provider documentation,
 *   https://onnxruntime.ai/docs/execution-providers/CoreML-ExecutionProvider.html
 *   (accessed 2026-05-09).
 *
 * @p compute_units may be NULL (omits the key — CoreML picks any unit) or
 * one of the strings above. @p fp16_io adds "ModelFormat=NeuralNetwork"
 * is intentionally NOT set; the EP defaults to ML Program format which
 * supports fp16 weight precision via the model itself, not via an EP
 * option. */
static int try_append_coreml(struct VmafOrtSession *sess, const char *compute_units)
{
    const char *keys[1];
    const char *values[1];
    size_t nk = 0u;
    if (compute_units != NULL) {
        keys[nk] = "MLComputeUnits";
        values[nk] = compute_units;
        ++nk;
    }
    return try_append_ep_generic(sess, "CoreMLExecutionProvider", keys, values, nk);
}

/* Process-wide OrtEnv singleton (iter10 TSan fix).
 *
 * ORT's documentation states that OrtEnv is a process-wide resource that
 * spawns internal background threads during CreateEnv.  Creating a fresh
 * OrtEnv per session causes TSan data races inside ORT's thread-pool
 * initialisation code.  The singleton is initialised exactly once via
 * pthread_once and lives for the process lifetime — ORT's own recommended
 * usage pattern.  It is intentionally never released (process exit cleans
 * up ORT's internal state implicitly). */
static OrtEnv *g_ort_env = NULL;
static pthread_once_t g_ort_env_once = PTHREAD_ONCE_INIT;

static void ort_env_init(void)
{
    const OrtApi *api = OrtGetApiBase()->GetApi(ORT_API_VERSION);
    if (!api)
        return;
    OrtStatus *st = api->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "libvmaf-dnn", &g_ort_env);
    if (st != NULL) {
        /* Best-effort: log and leave g_ort_env NULL; vmaf_ort_open will
         * detect the NULL and return -ENOSYS. */
        const char *msg = api->GetErrorMessage(st);
        vmaf_log(VMAF_LOG_LEVEL_WARNING, "libvmaf dnn: OrtEnv singleton init failed: %s\n",
                 (msg && msg[0] != '\0') ? msg : "(no message)");
        api->ReleaseStatus(st);
    }
}

static void ort_discard_status(const OrtApi *api, OrtStatus *st)
{
    if (st != NULL)
        api->ReleaseStatus(st);
}

/* Log the ORT error message at WARNING level, then release the status.
 * ORT guarantees a non-empty message whenever st != NULL, so the else
 * branch ("no ORT error message") was unreachable in practice; it was
 * removed to lift coverage above the 83% security-critical floor. */
static void ort_log_and_release_status(const OrtApi *api, OrtStatus *st, const char *ctx)
{
    if (st == NULL)
        return;
    const char *msg = api->GetErrorMessage(st);
    vmaf_log(VMAF_LOG_LEVEL_WARNING, "libvmaf dnn %s: %s\n", ctx ? ctx : "ORT error",
             (msg && msg[0] != '\0') ? msg : "(no ORT error message)");
    api->ReleaseStatus(st);
}

#define ORT_TRY(call)                                                                              \
    do {                                                                                           \
        OrtStatus *st__ = (call);                                                                  \
        if (st__ != NULL) {                                                                        \
            ort_log_and_release_status(sess->api, st__, #call);                                    \
            vmaf_ort_close(sess);                                                                  \
            return -EIO;                                                                           \
        }                                                                                          \
    } while (0)

int vmaf_ort_open(VmafOrtSession **out, const char *onnx_path, const VmafDnnConfig *cfg)
{
    if (!out || !onnx_path)
        return -EINVAL;
    assert(out != NULL);
    assert(onnx_path != NULL);

    VmafOrtSession *sess = (VmafOrtSession *)calloc(1, sizeof(*sess));
    if (!sess)
        return -ENOMEM;

    sess->api = OrtGetApiBase()->GetApi(ORT_API_VERSION);
    if (!sess->api) {
        free(sess);
        return -ENOSYS;
    }

    /* Initialise the process-wide OrtEnv singleton on first call (iter10
     * TSan fix): per-session CreateEnv races inside ORT's thread-pool. */
    (void)pthread_once(&g_ort_env_once, ort_env_init);
    if (!g_ort_env) {
        free(sess);
        return -ENOSYS;
    }

    ORT_TRY(sess->api->CreateSessionOptions(&sess->opts));

    const int intra = (cfg && cfg->threads > 0) ? cfg->threads : 0;
    if (intra > 0) {
        ORT_TRY(sess->api->SetIntraOpNumThreads(sess->opts, intra));
    }

    const VmafDnnDevice dev = cfg ? cfg->device : VMAF_DNN_DEVICE_AUTO;
    const int idx = (cfg && cfg->device_index > 0) ? cfg->device_index : 0;
    sess->fp16_io = (cfg != NULL) && cfg->fp16_io;

    /* Execution-provider selection.
     *
     * AUTO: try CUDA → OpenVINO (GPU then CPU) → ROCm → CPU. The first EP
     * whose append call returns NULL OrtStatus wins; EPs absent from the
     * ORT build return non-null and we fall through. The CPU EP is always
     * linked, so the final fall-through never fails.
     *
     * Explicit device: try only the requested EP; on failure the session
     * silently downgrades to CPU. Callers that need to know which EP
     * actually bound can check sess->ep_name via vmaf_ort_attached_ep()
     * (exposed for diagnostics / tests).
     */
    /* OpenVINO EP option set is documented at:
     *   https://onnxruntime.ai/docs/execution-providers/OpenVINO-ExecutionProvider.html
     *   (accessed 2026-05-08).
     * device_type values understood by OpenVINOExecutionProvider include
     * "CPU", "GPU" (alias for GPU.0), "GPU.0", "GPU.1", and "NPU". The NPU
     * value targets the Intel AI-PC neural processing unit on Meteor /
     * Lunar / Arrow Lake silicon. */
    sess->ep_name = "CPU";
    switch (dev) {
    case VMAF_DNN_DEVICE_CUDA:
        if (try_append_cuda(sess, idx) == 0)
            sess->ep_name = "CUDA";
        break;
    case VMAF_DNN_DEVICE_OPENVINO:
        if (try_append_openvino(sess, "GPU", sess->fp16_io) == 0) {
            sess->ep_name = "OpenVINO:GPU";
        } else if (try_append_openvino(sess, "CPU", sess->fp16_io) == 0) {
            sess->ep_name = "OpenVINO:CPU";
        }
        break;
    case VMAF_DNN_DEVICE_OPENVINO_NPU:
        if (try_append_openvino(sess, "NPU", sess->fp16_io) == 0)
            sess->ep_name = "OpenVINO:NPU";
        break;
    case VMAF_DNN_DEVICE_OPENVINO_CPU:
        if (try_append_openvino(sess, "CPU", sess->fp16_io) == 0)
            sess->ep_name = "OpenVINO:CPU";
        break;
    case VMAF_DNN_DEVICE_OPENVINO_GPU:
        if (try_append_openvino(sess, "GPU", sess->fp16_io) == 0)
            sess->ep_name = "OpenVINO:GPU";
        break;
    case VMAF_DNN_DEVICE_ROCM:
        if (try_append_rocm(sess) == 0)
            sess->ep_name = "ROCm";
        break;
    /* CoreML EP variants. The unscoped CoreML selector lets the EP
     * auto-route across compute units; the explicit ANE/GPU/CPU
     * selectors pin a single MLComputeUnits value. On non-Apple hosts
     * (e.g. Linux CI runners) the CoreML EP is absent from the linked
     * ORT and try_append_coreml returns -ENOSYS — the session keeps
     * the default ep_name="CPU" and CreateSession runs on the CPU EP. */
    case VMAF_DNN_DEVICE_COREML:
        if (try_append_coreml(sess, NULL) == 0)
            sess->ep_name = "CoreML";
        break;
    case VMAF_DNN_DEVICE_COREML_ANE:
        if (try_append_coreml(sess, "CPUAndNeuralEngine") == 0)
            sess->ep_name = "CoreML:ANE";
        break;
    case VMAF_DNN_DEVICE_COREML_GPU:
        if (try_append_coreml(sess, "CPUAndGPU") == 0)
            sess->ep_name = "CoreML:GPU";
        break;
    case VMAF_DNN_DEVICE_COREML_CPU:
        if (try_append_coreml(sess, "CPUOnly") == 0)
            sess->ep_name = "CoreML:CPU";
        break;
    case VMAF_DNN_DEVICE_AUTO:
        if (try_append_cuda(sess, idx) == 0) {
            sess->ep_name = "CUDA";
        } else if (try_append_openvino(sess, "GPU", sess->fp16_io) == 0) {
            sess->ep_name = "OpenVINO:GPU";
        } else if (try_append_rocm(sess) == 0) {
            sess->ep_name = "ROCm";
        } else if (try_append_coreml(sess, NULL) == 0) {
            /* CoreML is last in the AUTO chain because the explicit
             * --tiny-device=coreml-ane selector is the recommended
             * Apple-silicon entry point for highest perf-per-watt;
             * AUTO picks CoreML only when no discrete-GPU EP is
             * available (typical on M-series Macs). The unscoped
             * variant lets CoreML pick any compute unit — see the
             * ANE perf note in docs/ai/inference.md. */
            sess->ep_name = "CoreML";
        }
        /* NPU is intentionally NOT in the AUTO chain. NPU has surprising
         * latency floors on small graphs (power-state-transition cost
         * dominates sub-ms inferences) and is opt-in only via the
         * explicit `--tiny-device openvino-npu` selector. */
        break;
    case VMAF_DNN_DEVICE_CPU:
    default:
        break;
    }

    /* Two-stage session creation with non-CPU → CPU fallback.
     *
     * try_append_<EP> returning success only proves ORT *registered* the EP;
     * actual hardware initialisation happens inside CreateSession. If no
     * matching device is present (CUDA EP registered without an NVIDIA GPU,
     * OpenVINO without an OV-supported runtime, etc.) CreateSession returns
     * a non-null OrtStatus. Without this fallback, that condition would
     * surface as -EIO and the entire session_open would fail — even though
     * the CPU EP is always linked and could have served the request.
     *
     * Behaviour change on the happy path: when CUDA / OpenVINO / ROCm
     * actually work, nothing changes. The fallback only fires when the
     * non-CPU EP attached but its hardware is unavailable, which previously
     * was a hard failure. Callers that need to detect the degraded mode can
     * check vmaf_ort_attached_ep() — it now returns "CPU" after fallback.
     * See ADR-0113. */
    OrtStatus *create_st =
        sess->api->CreateSession(g_ort_env, onnx_path, sess->opts, &sess->session);
    if (create_st != NULL) {
        if (strcmp(sess->ep_name, "CPU") != 0) {
            /* Log the primary-EP failure at DEBUG level (hardware absent is expected in
             * CPU-only containers; the caller sees the result via vmaf_ort_attached_ep()). */
            ort_log_and_release_status(sess->api, create_st, "CreateSession (non-CPU EP)");
            create_st = NULL;
            /* Recreate session_options with no non-CPU EPs and retry. */
            sess->api->ReleaseSessionOptions(sess->opts);
            sess->opts = NULL;
            OrtStatus *opts_st = sess->api->CreateSessionOptions(&sess->opts);
            if (opts_st != NULL) {
                ort_log_and_release_status(sess->api, opts_st, "CreateSessionOptions (CPU retry)");
                vmaf_ort_close(sess);
                return -EIO;
            }
            const int intra_retry = (cfg && cfg->threads > 0) ? cfg->threads : 0;
            if (intra_retry > 0) {
                OrtStatus *t_st = sess->api->SetIntraOpNumThreads(sess->opts, intra_retry);
                if (t_st != NULL)
                    ort_discard_status(sess->api, t_st); /* best-effort; not fatal */
            }
            sess->ep_name = "CPU";
            OrtStatus *retry_st =
                sess->api->CreateSession(g_ort_env, onnx_path, sess->opts, &sess->session);
            if (retry_st != NULL) {
                ort_log_and_release_status(sess->api, retry_st, "CreateSession (CPU fallback)");
                vmaf_ort_close(sess);
                return -EIO;
            }
        } else {
            ort_log_and_release_status(sess->api, create_st, "CreateSession");
            create_st = NULL;
            vmaf_ort_close(sess);
            return -EIO;
        }
    }
    ORT_TRY(sess->api->GetAllocatorWithDefaultOptions(&sess->alloc));

    size_t ni = 0, no = 0;
    ORT_TRY(sess->api->SessionGetInputCount(sess->session, &ni));
    ORT_TRY(sess->api->SessionGetOutputCount(sess->session, &no));
    if (ni == 0 || no == 0) {
        vmaf_ort_close(sess);
        return -EINVAL;
    }
    sess->n_inputs = ni;
    sess->n_outputs = no;
    sess->input_names = (char **)calloc(ni, sizeof(char *));
    sess->output_names = (char **)calloc(no, sizeof(char *));
    if (!sess->input_names || !sess->output_names) {
        vmaf_ort_close(sess);
        return -ENOMEM;
    }
    for (size_t i = 0; i < ni; ++i) {
        ORT_TRY(
            sess->api->SessionGetInputName(sess->session, i, sess->alloc, &sess->input_names[i]));
    }
    for (size_t i = 0; i < no; ++i) {
        ORT_TRY(
            sess->api->SessionGetOutputName(sess->session, i, sess->alloc, &sess->output_names[i]));
    }
    /* legacy single-IO pointers alias position 0 */
    sess->input_name = sess->input_names[0];
    sess->output_name = sess->output_names[0];

    /* Cache per-IO element types so the run path can decide fp32 vs fp16
     * tensor creation without re-querying the model every call. */
    sess->input_elem_types = (int *)calloc(ni, sizeof(int));
    sess->output_elem_types = (int *)calloc(no, sizeof(int));
    if (!sess->input_elem_types || !sess->output_elem_types) {
        vmaf_ort_close(sess);
        return -ENOMEM;
    }
    for (size_t i = 0; i < ni; ++i) {
        OrtTypeInfo *ti = NULL;
        ORT_TRY(sess->api->SessionGetInputTypeInfo(sess->session, i, &ti));
        const OrtTensorTypeAndShapeInfo *tinfo = NULL;
        OrtStatus *cst = sess->api->CastTypeInfoToTensorInfo(ti, &tinfo);
        if (cst == NULL && tinfo != NULL) {
            ONNXTensorElementDataType et = ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
            OrtStatus *et_st = sess->api->GetTensorElementType(tinfo, &et);
            if (et_st != NULL) {
                ort_log_and_release_status(sess->api, et_st, "GetTensorElementType (input)");
                sess->api->ReleaseTypeInfo(ti);
                vmaf_ort_close(sess);
                return -EINVAL;
            }
            sess->input_elem_types[i] = (int)et;
        } else if (cst != NULL) {
            ort_log_and_release_status(sess->api, cst, "CastTypeInfoToTensorInfo (input)");
        }
        sess->api->ReleaseTypeInfo(ti);
    }
    for (size_t i = 0; i < no; ++i) {
        OrtTypeInfo *ti = NULL;
        ORT_TRY(sess->api->SessionGetOutputTypeInfo(sess->session, i, &ti));
        const OrtTensorTypeAndShapeInfo *tinfo = NULL;
        OrtStatus *cst = sess->api->CastTypeInfoToTensorInfo(ti, &tinfo);
        if (cst == NULL && tinfo != NULL) {
            ONNXTensorElementDataType et = ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
            OrtStatus *et_st = sess->api->GetTensorElementType(tinfo, &et);
            if (et_st != NULL) {
                ort_log_and_release_status(sess->api, et_st, "GetTensorElementType (output)");
                sess->api->ReleaseTypeInfo(ti);
                vmaf_ort_close(sess);
                return -EINVAL;
            }
            sess->output_elem_types[i] = (int)et;
        } else if (cst != NULL) {
            ort_log_and_release_status(sess->api, cst, "CastTypeInfoToTensorInfo (output)");
        }
        sess->api->ReleaseTypeInfo(ti);
    }

    /* Pre-create the CPU memory info once so vmaf_ort_infer / vmaf_ort_run
     * can reuse it across all frames instead of allocating it per call. */
    OrtStatus *mi_st =
        sess->api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &sess->cpu_mem_info);
    if (mi_st != NULL) {
        ort_log_and_release_status(sess->api, mi_st, "CreateCpuMemoryInfo");
        vmaf_ort_close(sess);
        return -EIO;
    }

    *out = sess;
    return 0;
}

/* Build an OrtValue from caller-supplied fp32 data. When the model declares
 * FLOAT16 at this slot and fp16_io is enabled, we emit a scratch fp16 tensor
 * (ownership returned via @p scratch_out so the caller can free it after
 * Run()); otherwise the caller's buffer is wrapped directly as fp32. */
static int build_input_tensor(VmafOrtSession *sess, OrtMemoryInfo *mem, size_t slot,
                              const float *data, const int64_t *shape, size_t rank,
                              OrtValue **tensor_out, void **scratch_out)
{
    /* Validate shape here so both callers (vmaf_ort_run, which already
     * pre-checks, and vmaf_ort_infer, which historically did not) are
     * guarded. Reject empty rank, non-positive dims, and an element
     * count that overflows size_t (a dim product that would wrap makes
     * the n*sizeof(...) malloc/tensor sizing below allocate too little). */
    if (rank == 0u)
        return -EINVAL;
    size_t n = 1;
    for (size_t d = 0; d < rank; ++d) {
        if (shape[d] <= 0)
            return -EINVAL;
        const size_t dim = (size_t)shape[d];
        if (n > SIZE_MAX / dim)
            return -EOVERFLOW;
        n *= dim;
    }

    const bool want_fp16 =
        sess->fp16_io && sess->input_elem_types[slot] == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16;

    if (want_fp16) {
        /* Guard the byte-count multiply too: the dim-product loop above only
         * proved the element count `n` does not overflow size_t, but
         * `n * sizeof(uint16_t)` can still wrap and under-allocate. */
        if (n > SIZE_MAX / sizeof(uint16_t))
            return -EOVERFLOW;
        uint16_t *half = (uint16_t *)malloc(n * sizeof(uint16_t));
        if (!half)
            return -ENOMEM;
        for (size_t i = 0; i < n; ++i)
            half[i] = fp32_to_fp16(data[i]);
        OrtStatus *st = sess->api->CreateTensorWithDataAsOrtValue(
            mem, half, n * sizeof(uint16_t), shape, rank, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16,
            tensor_out);
        if (st) {
            ort_log_and_release_status(sess->api, st, "CreateTensorWithDataAsOrtValue (fp16)");
            free(half);
            return -EIO;
        }
        *scratch_out = half;
        return 0;
    }

    if (n > SIZE_MAX / sizeof(float))
        return -EOVERFLOW;
    OrtStatus *st =
        sess->api->CreateTensorWithDataAsOrtValue(mem, (void *)data, n * sizeof(float), shape, rank,
                                                  ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, tensor_out);
    if (st) {
        ort_log_and_release_status(sess->api, st, "CreateTensorWithDataAsOrtValue (fp32)");
        return -EIO;
    }
    *scratch_out = NULL;
    return 0;
}

/* Convert @p out_n ORT output elements of type @p et from @p raw into the
 * caller's fp32 @p dst buffer. A blind memcpy-as-float corrupts every score
 * when the model declares a non-float output (e.g. a DOUBLE regressor head, or
 * an INT class index): the raw bytes would be reinterpreted as IEEE-754 float
 * garbage. Branch per dtype instead; reject unsupported types with -ENOTSUP. */
static int convert_output_elems(ONNXTensorElementDataType et, const void *raw, float *dst,
                                size_t out_n)
{
    switch (et) {
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT:
        memcpy(dst, raw, out_n * sizeof(float));
        return 0;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16: {
        const uint16_t *src = (const uint16_t *)raw;
        for (size_t i = 0; i < out_n; ++i)
            dst[i] = fp16_to_fp32(src[i]);
        return 0;
    }
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_DOUBLE: {
        const double *src = (const double *)raw;
        for (size_t i = 0; i < out_n; ++i)
            dst[i] = (float)src[i];
        return 0;
    }
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64: {
        const int64_t *src = (const int64_t *)raw;
        for (size_t i = 0; i < out_n; ++i)
            dst[i] = (float)src[i];
        return 0;
    }
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32: {
        const int32_t *src = (const int32_t *)raw;
        for (size_t i = 0; i < out_n; ++i)
            dst[i] = (float)src[i];
        return 0;
    }
    default:
        /* Unsupported output dtype — fail loudly rather than emit garbage. */
        vmaf_log(VMAF_LOG_LEVEL_WARNING, "libvmaf dnn: unsupported ORT output element type %d\n",
                 (int)et);
        return -ENOTSUP;
    }
}

/* Copy out an OrtValue into the caller's fp32 buffer. Detects the actual
 * tensor element type from the OrtValue — if fp16, casts back to fp32. */
static int copy_output_tensor(VmafOrtSession *sess, OrtValue *tensor, float *dst, size_t capacity,
                              size_t *written)
{
    OrtTensorTypeAndShapeInfo *info = NULL;
    OrtStatus *st = sess->api->GetTensorTypeAndShape(tensor, &info);
    if (st) {
        ort_log_and_release_status(sess->api, st, "GetTensorTypeAndShape");
        return -EIO;
    }
    size_t out_n = 0;
    st = sess->api->GetTensorShapeElementCount(info, &out_n);
    if (st) {
        ort_log_and_release_status(sess->api, st, "GetTensorShapeElementCount");
        sess->api->ReleaseTensorTypeAndShapeInfo(info);
        return -EIO;
    }
    ONNXTensorElementDataType et = ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT;
    st = sess->api->GetTensorElementType(info, &et);
    if (st) {
        ort_log_and_release_status(sess->api, st, "GetTensorElementType (output tensor)");
        sess->api->ReleaseTensorTypeAndShapeInfo(info);
        return -EIO;
    }
    sess->api->ReleaseTensorTypeAndShapeInfo(info);

    if (written)
        *written = out_n;
    if (out_n > capacity)
        return -ENOSPC;

    void *raw = NULL;
    st = sess->api->GetTensorMutableData(tensor, &raw);
    if (st) {
        ort_log_and_release_status(sess->api, st, "GetTensorMutableData");
        return -EIO;
    }
    return convert_output_elems(et, raw, dst, out_n);
}

int vmaf_ort_infer(VmafOrtSession *sess, const float *input, const int64_t *input_shape,
                   size_t input_rank, float *output, size_t output_capacity, size_t *output_written)
{
    if (!sess || !input || !input_shape || !output)
        return -EINVAL;

    /* Use the session-level cached OrtMemoryInfo (created once at vmaf_ort_open)
     * to avoid a per-frame ORT allocation round-trip (perf audit F2-A). */
    OrtMemoryInfo *mem = sess->cpu_mem_info;
    if (!mem)
        return -EINVAL;

    OrtValue *in_tensor = NULL;
    void *in_scratch = NULL;
    int rc =
        build_input_tensor(sess, mem, 0u, input, input_shape, input_rank, &in_tensor, &in_scratch);
    if (rc != 0)
        return rc;

    const char *in_names[1] = {sess->input_name};
    const char *out_names[1] = {sess->output_name};
    OrtValue *out_tensor = NULL;

    OrtStatus *st =
        sess->api->Run(sess->session, NULL, in_names, (const OrtValue *const *)&in_tensor, 1,
                       out_names, 1, &out_tensor);
    sess->api->ReleaseValue(in_tensor);
    free(in_scratch);
    if (st) {
        ort_log_and_release_status(sess->api, st, "Run (infer)");
        return -EIO;
    }

    size_t produced = 0;
    rc = copy_output_tensor(sess, out_tensor, output, output_capacity, &produced);
    sess->api->ReleaseValue(out_tensor);
    if (output_written)
        *output_written = produced;
    return rc;
}

int vmaf_ort_input_shape_at(VmafOrtSession *sess, size_t slot, int64_t *out_shape, size_t max_rank,
                            size_t *out_rank)
{
    if (!sess || !out_shape || !out_rank || max_rank == 0)
        return -EINVAL;
    if (slot >= sess->n_inputs)
        return -ERANGE;

    OrtTypeInfo *type_info = NULL;
    OrtStatus *st = sess->api->SessionGetInputTypeInfo(sess->session, slot, &type_info);
    if (st) {
        ort_log_and_release_status(sess->api, st, "SessionGetInputTypeInfo");
        return -EIO;
    }

    const OrtTensorTypeAndShapeInfo *tinfo = NULL;
    st = sess->api->CastTypeInfoToTensorInfo(type_info, &tinfo);
    if (st) {
        ort_log_and_release_status(sess->api, st, "CastTypeInfoToTensorInfo");
        sess->api->ReleaseTypeInfo(type_info);
        return -EIO;
    }

    size_t rank = 0;
    st = sess->api->GetDimensionsCount(tinfo, &rank);
    if (st) {
        ort_log_and_release_status(sess->api, st, "GetDimensionsCount");
        sess->api->ReleaseTypeInfo(type_info);
        return -EIO;
    }
    if (rank == 0 || rank > max_rank) {
        sess->api->ReleaseTypeInfo(type_info);
        return -ERANGE;
    }

    st = sess->api->GetDimensions(tinfo, out_shape, rank);
    sess->api->ReleaseTypeInfo(type_info);
    if (st) {
        ort_log_and_release_status(sess->api, st, "GetDimensions");
        return -EIO;
    }

    *out_rank = rank;
    return 0;
}

int vmaf_ort_input_shape(VmafOrtSession *sess, int64_t *out_shape, size_t max_rank,
                         size_t *out_rank)
{
    if (!sess || !out_shape || !out_rank || max_rank == 0)
        return -EINVAL;

    OrtTypeInfo *type_info = NULL;
    OrtStatus *st = sess->api->SessionGetInputTypeInfo(sess->session, 0, &type_info);
    if (st) {
        ort_log_and_release_status(sess->api, st, "SessionGetInputTypeInfo");
        return -EIO;
    }

    const OrtTensorTypeAndShapeInfo *tinfo = NULL;
    st = sess->api->CastTypeInfoToTensorInfo(type_info, &tinfo);
    if (st) {
        ort_log_and_release_status(sess->api, st, "CastTypeInfoToTensorInfo");
        sess->api->ReleaseTypeInfo(type_info);
        return -EIO;
    }

    size_t rank = 0;
    st = sess->api->GetDimensionsCount(tinfo, &rank);
    if (st) {
        ort_log_and_release_status(sess->api, st, "GetDimensionsCount");
        sess->api->ReleaseTypeInfo(type_info);
        return -EIO;
    }
    if (rank == 0 || rank > max_rank) {
        sess->api->ReleaseTypeInfo(type_info);
        return -ERANGE;
    }

    st = sess->api->GetDimensions(tinfo, out_shape, rank);
    sess->api->ReleaseTypeInfo(type_info);
    if (st) {
        ort_log_and_release_status(sess->api, st, "GetDimensions");
        return -EIO;
    }

    *out_rank = rank;
    return 0;
}

void vmaf_ort_close(VmafOrtSession *sess)
{
    if (!sess)
        return;
    assert(sess != NULL);
    if (sess->api) {
        assert(sess->api != NULL);
        if (sess->cpu_mem_info)
            sess->api->ReleaseMemoryInfo(sess->cpu_mem_info);
        if (sess->alloc && sess->input_names) {
            assert(sess->n_inputs > 0u);
            for (size_t i = 0; i < sess->n_inputs; ++i) {
                if (sess->input_names[i])
                    ort_discard_status(sess->api,
                                       sess->api->AllocatorFree(sess->alloc, sess->input_names[i]));
            }
        }
        if (sess->alloc && sess->output_names) {
            assert(sess->n_outputs > 0u);
            for (size_t i = 0; i < sess->n_outputs; ++i) {
                if (sess->output_names[i])
                    ort_discard_status(
                        sess->api, sess->api->AllocatorFree(sess->alloc, sess->output_names[i]));
            }
        }
        if (sess->session)
            sess->api->ReleaseSession(sess->session);
        if (sess->opts)
            sess->api->ReleaseSessionOptions(sess->opts);
        /* g_ort_env is a process-wide singleton; never released here. */
    }
    free(sess->input_names);
    free(sess->output_names);
    free(sess->input_elem_types);
    free(sess->output_elem_types);
    free(sess);
}

const char *vmaf_ort_attached_ep(const VmafOrtSession *sess)
{
    if (!sess)
        return NULL;
    return sess->ep_name;
}

int vmaf_ort_io_count(VmafOrtSession *sess, size_t *n_inputs, size_t *n_outputs)
{
    if (!sess || !n_inputs || !n_outputs)
        return -EINVAL;
    *n_inputs = sess->n_inputs;
    *n_outputs = sess->n_outputs;
    return 0;
}

const char *vmaf_ort_output_name_at(VmafOrtSession *sess, size_t slot)
{
    if (!sess)
        return NULL;
    if (slot >= sess->n_outputs)
        return NULL;
    return sess->output_names[slot];
}

/* Resolve a user-supplied input/output name against the session's name
 * table. NULL name → positional fallback at @p pos. Returns the const
 * char* used by ORT (owned by the session) or NULL on lookup failure. */
static const char *resolve_name(char **table, size_t count, const char *name, size_t pos)
{
    if (name == NULL) {
        if (pos >= count)
            return NULL;
        return table[pos];
    }
    for (size_t i = 0; i < count; ++i) {
        if (table[i] && strcmp(table[i], name) == 0)
            return table[i];
    }
    return NULL;
}

int vmaf_ort_run(VmafOrtSession *sess, const VmafOrtTensorIn *inputs, size_t n_inputs,
                 VmafOrtTensorOut *outputs, size_t n_outputs)
{
    if (!sess || !inputs || !outputs || n_inputs == 0u || n_outputs == 0u)
        return -EINVAL;
    assert(sess != NULL);
    assert(inputs != NULL);
    assert(outputs != NULL);
    if (n_inputs != sess->n_inputs || n_outputs != sess->n_outputs)
        return -EINVAL;
    /* Guard: n_inputs/n_outputs are already validated equal to sess->n_{in,out}
     * which were set at open time from the model graph.  VMAF_ORT_MAX_IO (8)
     * exceeds every model shipped to date (max observed: 6).  Stack arrays
     * avoid 5 per-call calloc/free pairs on the per-frame hot path (F3-B). */
    if (n_inputs > VMAF_ORT_MAX_IO || n_outputs > VMAF_ORT_MAX_IO)
        return -EINVAL;

    /* Reuse the session-level cached OrtMemoryInfo (perf audit F2-A / F3-A). */
    OrtMemoryInfo *mem = sess->cpu_mem_info;
    if (!mem)
        return -EINVAL;

    /* Stack-allocated IO arrays — no heap allocation on the hot path. */
    const char *in_names[VMAF_ORT_MAX_IO];
    const char *out_names[VMAF_ORT_MAX_IO];
    OrtValue *in_vals[VMAF_ORT_MAX_IO];
    OrtValue *out_vals[VMAF_ORT_MAX_IO];
    void *in_scratch[VMAF_ORT_MAX_IO];
    memset(in_names, 0, n_inputs * sizeof(in_names[0]));
    memset(out_names, 0, n_outputs * sizeof(out_names[0]));
    memset(in_vals, 0, n_inputs * sizeof(in_vals[0]));
    memset(out_vals, 0, n_outputs * sizeof(out_vals[0]));
    memset(in_scratch, 0, n_inputs * sizeof(in_scratch[0]));

    int rc = 0;
    for (size_t i = 0; i < n_inputs; ++i) {
        if (!inputs[i].data || !inputs[i].shape || inputs[i].rank == 0u) {
            rc = -EINVAL;
            goto cleanup;
        }
        in_names[i] = resolve_name(sess->input_names, sess->n_inputs, inputs[i].name, i);
        if (!in_names[i]) {
            rc = -EINVAL;
            goto cleanup;
        }
        for (size_t d = 0; d < inputs[i].rank; ++d) {
            if (inputs[i].shape[d] <= 0) {
                rc = -EINVAL;
                goto cleanup;
            }
        }
        int brc = build_input_tensor(sess, mem, i, inputs[i].data, inputs[i].shape, inputs[i].rank,
                                     &in_vals[i], &in_scratch[i]);
        if (brc != 0) {
            rc = brc;
            goto cleanup;
        }
    }
    for (size_t i = 0; i < n_outputs; ++i) {
        if (!outputs[i].data) {
            rc = -EINVAL;
            goto cleanup;
        }
        out_names[i] = resolve_name(sess->output_names, sess->n_outputs, outputs[i].name, i);
        if (!out_names[i]) {
            rc = -EINVAL;
            goto cleanup;
        }
    }

    OrtStatus *st_run =
        sess->api->Run(sess->session, NULL, in_names, (const OrtValue *const *)in_vals, n_inputs,
                       out_names, n_outputs, out_vals);
    if (st_run) {
        ort_log_and_release_status(sess->api, st_run, "Run");
        rc = -EIO;
        goto cleanup;
    }

    for (size_t i = 0; i < n_outputs; ++i) {
        size_t produced = 0;
        int cpy =
            copy_output_tensor(sess, out_vals[i], outputs[i].data, outputs[i].capacity, &produced);
        outputs[i].written = produced;
        if (cpy == -ENOSPC) {
            /* Short buffer — record the required count and keep going so all
             * outputs get their `written` populated, but propagate -ENOSPC. */
            rc = -ENOSPC;
        } else if (cpy != 0) {
            rc = cpy;
            goto cleanup;
        }
    }

cleanup:
    for (size_t i = 0; i < n_inputs; ++i) {
        if (in_vals[i])
            sess->api->ReleaseValue(in_vals[i]);
        free(in_scratch[i]);
    }
    for (size_t i = 0; i < n_outputs; ++i) {
        if (out_vals[i])
            sess->api->ReleaseValue(out_vals[i]);
    }
    /* mem is session-owned (cpu_mem_info); released in vmaf_ort_close.
     * Stack arrays (in_names, out_names, in_vals, out_vals, in_scratch)
     * are automatically reclaimed — no free() needed. */
    return rc;
}

/* Internal-test entry points (declared in ort_backend_internal.h). Thin
 * wrappers preserve the static qualifier on the originals so production
 * call sites remain fully inlinable while the test binary can still
 * exercise the helpers directly. See ADR-0112. */
uint16_t vmaf_ort_internal_fp32_to_fp16(float f)
{
    return fp32_to_fp16(f);
}

float vmaf_ort_internal_fp16_to_fp32(uint16_t h)
{
    return fp16_to_fp32(h);
}

const char *vmaf_ort_internal_resolve_name(char **table, size_t count, const char *name, size_t pos)
{
    return resolve_name(table, count, name, pos);
}

int vmaf_ort_internal_convert_output_elems(VmafOrtElemType elem_type, const void *raw, float *dst,
                                           size_t count)
{
    return convert_output_elems((ONNXTensorElementDataType)elem_type, raw, dst, count);
}

VmafOrtElemType vmaf_ort_internal_input_elem_type(const VmafOrtSession *sess, size_t slot)
{
    if (!sess || slot >= sess->n_inputs)
        return ELEM_TYPE_UNDEFINED;
    return (VmafOrtElemType)sess->input_elem_types[slot];
}

VmafOrtElemType vmaf_ort_internal_output_elem_type(const VmafOrtSession *sess, size_t slot)
{
    if (!sess || slot >= sess->n_outputs)
        return ELEM_TYPE_UNDEFINED;
    return (VmafOrtElemType)sess->output_elem_types[slot];
}

#else /* !VMAF_HAVE_DNN */

struct VmafOrtSession {
    int _unused;
};

int vmaf_ort_open(VmafOrtSession **out, const char *onnx_path, const VmafDnnConfig *cfg)
{
    (void)out;
    (void)onnx_path;
    (void)cfg;
    return -ENOSYS;
}

/* NOLINTBEGIN(readability-non-const-parameter)
 * Stub signatures must match the real-ORT path declared in the header (ADR-0040). */
int vmaf_ort_infer(VmafOrtSession *sess, const float *input, const int64_t *input_shape,
                   size_t input_rank, float *output, size_t output_capacity, size_t *output_written)
{
    (void)sess;
    (void)input;
    (void)input_shape;
    (void)input_rank;
    (void)output;
    (void)output_capacity;
    (void)output_written;
    return -ENOSYS;
}

int vmaf_ort_input_shape(VmafOrtSession *sess, int64_t *out_shape, size_t max_rank,
                         size_t *out_rank)
{
    (void)sess;
    (void)out_shape;
    (void)max_rank;
    (void)out_rank;
    return -ENOSYS;
}

int vmaf_ort_input_shape_at(VmafOrtSession *sess, size_t slot, int64_t *out_shape, size_t max_rank,
                            size_t *out_rank)
{
    (void)sess;
    (void)slot;
    (void)out_shape;
    (void)max_rank;
    (void)out_rank;
    return -ENOSYS;
}

int vmaf_ort_io_count(VmafOrtSession *sess, size_t *n_inputs, size_t *n_outputs)
{
    (void)sess;
    (void)n_inputs;
    (void)n_outputs;
    return -ENOSYS;
}

const char *vmaf_ort_output_name_at(VmafOrtSession *sess, size_t slot)
{
    (void)sess;
    (void)slot;
    return NULL;
}

int vmaf_ort_run(VmafOrtSession *sess, const VmafOrtTensorIn *inputs, size_t n_inputs,
                 VmafOrtTensorOut *outputs, size_t n_outputs)
{
    (void)sess;
    (void)inputs;
    (void)n_inputs;
    (void)outputs;
    (void)n_outputs;
    return -ENOSYS;
}
/* NOLINTEND(readability-non-const-parameter) */

void vmaf_ort_close(VmafOrtSession *sess)
{
    (void)sess;
}

const char *vmaf_ort_attached_ep(const VmafOrtSession *sess)
{
    (void)sess;
    return NULL;
}

/* Internal-test stubs for the DNN-disabled build. The real wrappers live
 * in the VMAF_HAVE_DNN branch above; here they exist purely so
 * test_ort_internals.c links cleanly on stub builds. The test bodies
 * short-circuit via vmaf_dnn_available() before invoking these. */
uint16_t vmaf_ort_internal_fp32_to_fp16(float f)
{
    (void)f;
    return 0u;
}

float vmaf_ort_internal_fp16_to_fp32(uint16_t h)
{
    (void)h;
    return 0.0f;
}

const char *vmaf_ort_internal_resolve_name(char **table, size_t count, const char *name, size_t pos)
{
    (void)table;
    (void)count;
    (void)name;
    (void)pos;
    return NULL;
}

/* NOLINTNEXTLINE(readability-non-const-parameter)
 * Internal stub signature must match the real-ORT test seam (ADR-0112). */
int vmaf_ort_internal_convert_output_elems(VmafOrtElemType elem_type, const void *raw, float *dst,
                                           size_t count)
{
    (void)elem_type;
    (void)raw;
    (void)dst;
    (void)count;
    return -ENOSYS;
}

VmafOrtElemType vmaf_ort_internal_input_elem_type(const VmafOrtSession *sess, size_t slot)
{
    (void)sess;
    (void)slot;
    return ELEM_TYPE_UNDEFINED;
}

VmafOrtElemType vmaf_ort_internal_output_elem_type(const VmafOrtSession *sess, size_t slot)
{
    (void)sess;
    (void)slot;
    return ELEM_TYPE_UNDEFINED;
}

#endif /* VMAF_HAVE_DNN */
