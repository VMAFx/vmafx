<!-- markdownlint-disable MD013 MD060 -->
# ADR-0102: DNN execution-provider selection is ordered + graceful, fp16_io does a host-side cast

- **Status**: Accepted
- **Date**: 2026-04-18
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: ai, dnn, api

## Context

Before this ADR, `core/src/dnn/ort_backend.c` exposed a three-field
`VmafDnnConfig` (`device`, `device_index`, `threads`, `fp16_io`) and
wired exactly one of those fields: `threads`. The other three were
accepted at the public surface and then silently dropped by the backend:

- `device` was switched on, but the CUDA case was gated behind a
  `#ifdef ORT_API_HAS_CUDA` macro that is not defined anywhere in the
  Meson build — so even the CUDA branch was dead code. `OPENVINO` and
  `ROCM` enum values existed but had no wiring at all.
- `fp16_io` was a documented `bool` with no code path ever reading it.

Issue [#30](https://github.com/VMAFx/vmafx/issues/30) flagged this as
an API correctness bug: callers that set `device = VMAF_DNN_DEVICE_CUDA`
or `fp16_io = true` got a CPU fp32 session, with no log, no error, and
no way to detect the silent downgrade. The same applies to
`VMAF_DNN_DEVICE_OPENVINO` (the field the Intel-GPU path is meant to
target) and `VMAF_DNN_DEVICE_ROCM`.

## Decision

### EP selection

1. **Use the generic `SessionOptionsAppendExecutionProvider` C-API**
   (`const char *provider_name`, keys/values) for OpenVINO and ROCm
   instead of per-EP struct calls. The generic call returns non-null
   `OrtStatus` when the named EP isn't registered in this ORT build —
   that's what we treat as "EP unavailable, try next". Keeping CUDA on
   the `_CUDA(OrtCUDAProviderOptions)` entry point preserves
   compatibility with older ORT builds that predate the generic API
   for CUDA.
2. **`VMAF_DNN_DEVICE_AUTO` tries an ordered chain**: CUDA → OpenVINO
   (GPU then CPU) → ROCm → CPU. The first EP whose append call returns
   `NULL` OrtStatus wins; absent EPs fall through. CPU is always
   linked, so the final fall-through never fails.
3. **Explicit device requests degrade gracefully**: if CUDA / OpenVINO
   / ROCm is requested but the ORT build doesn't carry that EP, the
   session still opens against the CPU EP rather than erroring.
   Diagnostics are surfaced via the new accessor
   `vmaf_dnn_session_attached_ep()` (public) / `vmaf_ort_attached_ep()`
   (internal) — stable strings `"CPU"`, `"CUDA"`, `"OpenVINO:GPU"`,
   `"OpenVINO:CPU"`, `"ROCm"`. Callers that want a hard failure on
   missing EP can assert `strcmp(ep, "CPU") != 0` at the call site.

### fp16_io

`VmafDnnConfig.fp16_io` now gates a **host-side fp32 ↔ fp16 round-trip
cast**, triggered per input/output slot based on the model's declared
element type:

- At open time we cache each input's and output's `ONNXTensorElementDataType`.
- When `fp16_io == true` and slot type is `ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16`,
  inputs are cast fp32 → fp16 into a scratch buffer and an fp16 OrtValue
  is handed to Run(). Outputs are cast fp16 → fp32 back into the
  caller's fp32 buffer. The public data types stay fp32 — callers don't
  need to know anything about IEEE-754 half precision.
- When `fp16_io == true` but the model's slot type is FLOAT32, the
  cast is skipped (no-op). This is the natural "request, when
  supported" reading of the field.
- When `fp16_io == false` and the model declares FLOAT16, the session
  still opens (ORT accepts the graph), but the first run fails because
  a FLOAT-typed tensor is handed to a FLOAT16-declared input.

OpenVINO additionally receives `precision=FP16` as a provider option
when `fp16_io == true`, so intermediate compute also runs at half
precision rather than just I/O.

The portable conversion code (`fp32_to_fp16` / `fp16_to_fp32`) avoids
`_Float16` and F16C intrinsics so the DNN backend still builds on
hosts without hardware fp16.

## Alternatives considered

| Alternative | Why rejected |
|---|---|
| Keep the existing `#ifdef ORT_API_HAS_CUDA` gate and add parallel `ORT_API_HAS_OPENVINO` / `ORT_API_HAS_ROCM` gates | The macros aren't defined anywhere in our build system and never were. Adding two more dead-code gates extends the bug. Runtime detection via `OrtStatus` is what ORT itself recommends and what every serious ORT consumer uses. |
| Hard-fail when the requested EP isn't available | Breaks users who set `device = CUDA` hopefully on laptops and expect a CPU fallback. The public `VmafDnnConfig` doc already says "device hint", not "device requirement", and the `AUTO` default implies best-effort selection. Callers that truly need the EP assertion can check `vmaf_dnn_session_attached_ep()`. |
| Require fp16 inputs/outputs in the public API | Forces every caller to ship their own fp16 converter. The internal cast is ~8 lines of arithmetic per direction; centralising it keeps the API uniform. |
| Use `_Float16` when the compiler supports it, fall back to bitfiddling otherwise | Two code paths to test, inconsistent rounding behaviour between compilers. Current pure-integer path is deterministic and fast enough for typical tiny-model I/O sizes (a 1-Mpixel luma frame converts in ~1ms on a modern x86). |
| Log the fallback through ORT's verbose logger | Requires callers to attach a log sink and parse EP registration strings out of ORT's free-form output. The accessor is one string comparison and doesn't depend on ORT internals. |

## Consequences

- Callers that set `device = VMAF_DNN_DEVICE_OPENVINO` on an Intel-GPU
  host with an OpenVINO-enabled ORT build get the OpenVINO EP.
  Previously this was indistinguishable from CPU.
- Callers on stock CPU-only ORT builds (including CI) see no behaviour
  change: every EP request still resolves to CPU. The change is that
  this is now **observable** via `vmaf_dnn_session_attached_ep()`.
- `fp16_io = true` on a FLOAT16-typed model now runs the model; on a
  FLOAT32 model it's a silent no-op (documented).
- The registry gains `model/tiny/smoke_fp16_v0.onnx` — a 98-byte
  Identity model that exercises the fp16 cast path under CI. Not a
  quality model.
- New internal symbol: `vmaf_ort_attached_ep`. New public symbol:
  `vmaf_dnn_session_attached_ep`.
- Downstream consumers that previously relied on the
  `accepted-but-ignored` behaviour get the same observable result
  unless they explicitly set a non-default `device` or `fp16_io`, at
  which point they opt into the new semantics.

## References

- Issue [#30 — VmafDnnDevice OPENVINO/ROCM + fp16_io accepted-but-ignored](https://github.com/VMAFx/vmafx/issues/30)
- [ONNX Runtime C API — execution providers](https://onnxruntime.ai/docs/execution-providers/)
- [ADR-0040](decisions-log.md) — multi-input DNN session API (the same `VmafDnnConfig` this ADR extends)
- [ADR-0042 / ADR-0100](decisions-log.md) — docs-in-same-PR rules (this PR updates `docs/api/dnn.md` accordingly)
