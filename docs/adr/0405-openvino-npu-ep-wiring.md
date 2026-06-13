<!-- markdownlint-disable MD060 -->
# ADR-0405: Wire OpenVINO NPU execution provider into the tiny-AI dispatch layer

- **Status**: Accepted
- **Date**: 2026-05-08
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: ai, dnn, openvino, intel-ai-pc, fork-local

## Context

[Research-0031](../research/0031-intel-ai-pc-applicability.md) surveyed
the Intel AI-PC NPU surface in 2026-04 and recommended **defer** until a
maintainer with Meteor / Lunar / Arrow Lake silicon could validate the
path. SYCL audit research-0086 (action item A.5) revisited the question
under the oneAPI 2025.3.1 bump: the bundled ONNX Runtime now reliably
exposes the OpenVINO NPU plugin as a `device_type=NPU` value on the
existing `OpenVINOExecutionProvider`, and the dispatch-layer wiring is
the last piece needed to expose `--tiny-device=openvino-npu` to users.
Hardware coverage on the maintainer's primary box (Ryzen 9 9950X3D +
Arc A380 + RTX 4090 + Granite Ridge iGPU) is *still* NPU-less, so
end-to-end NPU silicon validation remains pending; what *is* in scope
here is the runtime plumbing plus a smoke-test that exercises the
selector and asserts graceful fallback when no NPU is present.

## Decision

Add three explicit OpenVINO device-type selectors to the public
`VmafDnnDevice` enum and the `--tiny-device` CLI grammar:
`VMAF_DNN_DEVICE_OPENVINO_NPU` / `_CPU` / `_GPU`, mapping to
`device_type=NPU` / `CPU` / `GPU` on the OpenVINO EP with **no** fallback
chain inside the explicit-selector branches. Keep the existing
`VMAF_DNN_DEVICE_OPENVINO` (`--tiny-device openvino`) entry as the
"GPU device type with CPU fallback" path it has always been. Do NOT add
NPU to the `VMAF_DNN_DEVICE_AUTO` try-chain — NPU has surprising
performance characteristics on small graphs (latency floor dominated
by power-state transitions) and is opt-in only.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Add `--tiny-device=npu` only (no `openvino-cpu`/`openvino-gpu`) | Minimal grammar growth | Hides which EP is bound; ambiguous on systems with multiple NPU vendors (future Qualcomm / AMD NPUs) | Rejected — Research-0031 §4 explicitly proposes the `openvino-*` namespace for disambiguation |
| Reuse `VMAF_DNN_DEVICE_OPENVINO` and add an NPU sub-flag | One enum value | Forces a runtime-flag check inside the dispatch switch; harder to test EP selection per device type in isolation | Rejected |
| Extend the AUTO chain to include NPU after CUDA/OV-GPU | Zero-config NPU on AI-PC laptops | NPU power-state latency floor would surprise users running short-clip inference; the int8-first precision policy doesn't match the fork's fp32 default | Rejected — opt-in only, per Research-0031 §4 |
| Defer indefinitely (Research-0031 verdict) | Zero risk | research-0086 GO recommendation supersedes; users have requested an explicit NPU selector for AI-PC laptops | Rejected — superseded |

## Consequences

- **Positive**: external users with Meteor / Lunar / Arrow Lake silicon
  can target the NPU explicitly with `--tiny-device=openvino-npu`. The
  `--tiny-device=openvino-cpu` form gives a stable test path on hosts
  without an NPU (covers the maintainer's Ryzen workstation). The
  graceful CPU-EP fallback in `vmaf_ort_open()` already handles missing
  silicon, so the new selectors degrade safely on hosts where the EP
  is registered but the device isn't physically present.
- **Negative**: per-host validation against NPU silicon is still
  pending hardware access. The fork ships the wiring un-validated on
  silicon, mitigated by (a) graceful CPU fallback and (b) the
  hardware-less smoke-test which exercises the selector path and
  asserts `attached_ep` is one of the known set.
- **Neutral / follow-ups**: when a contributor with NPU hardware runs
  the int8 + fp16 measurement matrix from
  [Research-0006](../research/0006-tinyai-ptq-accuracy-targets.md)
  against `device_type=NPU`, append a `### Status update YYYY-MM-DD`
  section to this ADR with the findings; do not edit the body
  (per ADR-0028 immutability).

## References

- Source: research-0086 SYCL audit #464 action item A.5 (GO recommendation).
- Related ADRs: [ADR-0028](0028-adr-maintenance-rule.md) (immutability),
  [ADR-0042](0042-tinyai-docs-required-per-pr.md) (tiny-AI doc bar),
  [ADR-0108](0108-deep-dive-deliverables-rule.md) (six-deliverable rule),
  [ADR-0221](0221-changelog-adr-fragment-pattern.md) (fragment files).
- Related research:
  [Research-0031](../research/0031-intel-ai-pc-applicability.md)
  (predecessor; verdict was DEFER, this ADR is the GO follow-up).
- Touched files:
  `core/include/libvmaf/dnn.h`,
  `core/src/dnn/ort_backend.{c,h}`,
  `core/tools/vmaf.c`,
  `core/tools/cli_parse.{c,h}`,
  `core/test/dnn/test_ep_fp16.c`,
  `core/test/dnn/test_cli.sh`,
  `docs/ai/inference.md`,
  `docs/usage/cli.md`,
  `docs/development/oneapi-install.md`.
- ORT OpenVINO EP option set:
  <https://onnxruntime.ai/docs/execution-providers/OpenVINO-ExecutionProvider.html>
  (accessed 2026-05-08).
