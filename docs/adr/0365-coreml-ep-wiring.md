<!-- markdownlint-disable MD013 MD060 -->
# ADR-0365: Wire the CoreML execution provider into tiny-AI ORT dispatch

- **Status**: Accepted
- **Date**: 2026-05-09
- **Deciders**: lusoris, Claude (Anthropic)
- **Tags**: ai, dnn, coreml, apple-silicon, fork-local

## Context

Apple Silicon (M1+) carries a dedicated on-die NPU — the **Apple Neural
Engine** — that runs ML models at substantially higher tokens/sec/watt
than the CPU or Metal-backed GPU on the same chip. The vendor-supplied
path from ONNX Runtime to the Neural Engine is the **CoreML execution
provider** (`CoreMLExecutionProvider`). Until this PR the fork's
`--tiny-device` grammar accepted `auto|cpu|cuda|openvino|rocm` only;
Apple users running tiny-AI inference (saliency, fr_regressor, MOS
head) silently degraded to the CPU EP regardless of available silicon.

This is the Apple-side parallel to ADR-0332's OpenVINO NPU wiring for
Intel AI-PC silicon: same wiring shape, different EP, different
hardware-routing options. Both ADRs share the explicit-EP-downgrade
pattern from ADR-0102 (graceful CPU fallback when the requested EP
is absent from the linked ORT build).

## Decision

Add four CoreML device-type selectors to the public `VmafDnnDevice`
enum and the `--tiny-device` CLI grammar:

- `--tiny-device=coreml`     → CoreML EP, no `MLComputeUnits` pin
  (CoreML auto-routes per op).
- `--tiny-device=coreml-ane` → `MLComputeUnits=CPUAndNeuralEngine`
  (Apple Neural Engine + CPU fallback).
- `--tiny-device=coreml-gpu` → `MLComputeUnits=CPUAndGPU` (Metal +
  CPU fallback).
- `--tiny-device=coreml-cpu` → `MLComputeUnits=CPUOnly` (universal
  fallback).

The CoreML EP is added to the AUTO try-chain at the **last position**
(after CUDA / OpenVINO-GPU / ROCm). Rationale: the recommended
Apple-silicon entry point is the explicit `coreml-ane` selector; AUTO
picks CoreML only when no discrete-GPU EP is available, which is the
typical M-series-Mac configuration.

The wiring uses the generic
`SessionOptionsAppendExecutionProvider("CoreMLExecutionProvider", keys, values, n)`
key/value form rather than the older
`OrtSessionOptionsAppendExecutionProvider_CoreML(opts, uint32_t flags)`
factory in `coreml_provider_factory.h`. The generic form needs no
extra header (the factory header is only present on macOS ORT
builds), and degrades cleanly on Linux ORT builds (the EP is not
linked → non-null `OrtStatus` from
`SessionOptionsAppendExecutionProvider` → `try_append_coreml` returns
-ENOSYS → session keeps `ep_name="CPU"` and `CreateSession` runs on
the CPU EP). Reference: ONNX Runtime CoreML Execution Provider
documentation,
<https://onnxruntime.ai/docs/execution-providers/CoreML-ExecutionProvider.html>
(accessed 2026-05-09).

End-to-end Apple-silicon validation pending hardware access — the
maintainer's primary box is Ryzen 9 9950X3D + Arc A380 + RTX 4090,
no Mac. The graceful CPU-EP fallback in `vmaf_ort_open()` covers
silicon absence safely; the Linux CI lane verifies the open-and-
fallback path on every push.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Generic-EP key/value form (chosen) | No extra header dependency; clean Linux fallback; matches existing OpenVINO wiring shape | Requires CoreML EP version that recognises the `MLComputeUnits` key (ORT ≥ 1.17). | — |
| `OrtSessionOptionsAppendExecutionProvider_CoreML` factory | Works against older ORT releases (pre-1.17); explicit fp16-mode flag via `COREML_FLAG_USE_CPU_AND_GPU` | Header `coreml_provider_factory.h` only ships in macOS ORT distributions; Linux build would need conditional compilation around the include. | Rejected — adds platform-specific include conditionals to a code path that already handles missing-EP gracefully via the generic form. |
| Single `--tiny-device=coreml` selector with no sub-device variants | Simplest grammar | Loses the operator-controlled choice between ANE / GPU / CPU; CoreML's auto-routing is per-op and can spread inference across all three units, defeating the perf-per-watt motivation for ANE pinning. | Rejected — the user-visible reason to wire CoreML at all is ANE access; the explicit selectors are the load-bearing surface. |
| Add CoreML to AUTO chain ahead of CUDA | Apple-silicon Macs hit ANE in AUTO mode | Cross-platform AUTO behaviour becomes platform-dependent in non-obvious ways; dev machines with both an eGPU (CUDA) and CoreML would silently prefer CoreML. | Rejected — AUTO last-position keeps the explicit selector as the recommended Apple path. |

## Consequences

- **Positive**: Apple-silicon users now have an opt-in path to the
  Neural Engine for tiny-AI inference; no more silent degradation to
  the CPU EP. The four-variant grammar makes ANE / GPU / CPU pinning
  explicit and diagnosable via `vmaf_dnn_session_attached_ep()`.
- **Positive**: The wiring shape is identical to ADR-0332's OpenVINO
  NPU pattern, so future reviewers reading either ADR can map the
  other one-to-one.
- **Negative**: `VmafDnnDevice` enum gains 4 new values
  (`COREML`/`_ANE`/`_GPU`/`_CPU` = 5..8). Append-only — the existing
  values `0..4` are unchanged so the public ABI is stable.
- **Negative**: End-to-end ANE silicon validation is deferred until
  the fork has access to an Apple-silicon Mac. The Linux CI lane
  exercises only the fallback path; macOS validation is the next
  blocker for closing this work-stream.
- **Neutral**: Coordinates with ADR-0332 (OpenVINO NPU EP, draft).
  Both PRs touch `ort_backend.c`, `cli_parse.c`, `vmaf.c`, `dnn.h`,
  `test_ep_fp16.c`, `test_cli.sh`, and `docs/ai/inference.md`.
  Whichever lands first, the other rebases — the conflicts are
  mechanical (adjacent enum values, adjacent switch cases, adjacent
  CLI keyword strings) with no semantic interaction.

## References

- req (PR task brief, 2026-05-09): "Apple Silicon (M1+) has the
  Apple Neural Engine — a dedicated NPU on-die that runs ML models
  at much higher tokens/sec/watt than the CPU or GPU. Apple's path
  to it from ONNX Runtime is the CoreML EP. Once wired,
  `--tiny-device=coreml` routes tiny-AI inference (saliency,
  fr_regressor, MOS head) through CoreML, which Apple's runtime
  auto-routes to ANE / GPU / CPU based on the op set."
- [ADR-0102](0102-dnn-ep-selection-and-fp16-io.md) — base EP
  selection and graceful-fallback design.
- [ADR-0332](0405-openvino-npu-ep-wiring.md) — OpenVINO NPU EP
  wiring (Apple-side parallel; same shape).
- [ADR-0042](0042-tinyai-docs-required-per-pr.md) — tiny-AI docs
  required-per-PR rule.
- ONNX Runtime CoreML Execution Provider documentation,
  <https://onnxruntime.ai/docs/execution-providers/CoreML-ExecutionProvider.html>
  (accessed 2026-05-09).
