- **OpenVINO NPU execution provider wired into the tiny-AI dispatch layer
  ([ADR-0332](../docs/adr/0332-openvino-npu-ep-wiring.md),
  [Research-0031](../docs/research/0031-intel-ai-pc-applicability.md)).**
  Adds three new `--tiny-device` keywords — `openvino-npu`,
  `openvino-cpu`, and `openvino-gpu` — that pin the
  `OpenVINOExecutionProvider` to a single `device_type` (`NPU`, `CPU`,
  `GPU`) with no fallback inside the explicit-selector branches. The
  existing `--tiny-device openvino` keeps its GPU→CPU fallback chain
  unchanged. NPU is intentionally NOT added to the `--tiny-device auto`
  try-chain because of NPU power-state latency floor on small graphs.
  The public `VmafDnnDevice` enum gains `VMAF_DNN_DEVICE_OPENVINO_NPU`
  / `_CPU` / `_GPU`; `vmaf_dnn_session_attached_ep()` gains
  `"OpenVINO:NPU"` as a stable return string. Targets the Intel AI-PC
  neural processing unit on Meteor / Lunar / Arrow Lake silicon. The
  graceful CPU-EP fallback in `vmaf_ort_open()` covers hosts where the
  EP is registered but the device isn't physically present, so
  `--tiny-device=openvino-cpu` is a safe fallback selector on
  hardware-less hosts. End-to-end NPU silicon validation deferred to a
  contributor with Meteor / Lunar / Arrow Lake hardware.
