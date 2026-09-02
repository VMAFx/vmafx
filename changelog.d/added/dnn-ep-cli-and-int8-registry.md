- **`--dnn-ep` CLI flag and `vmaf_tiny_v3_int8` registry promotion.**
  Two additive wiring fixes identified in the DNN-EP audit:
  (1) Add `--dnn-ep <ep>` to the `vmaf` CLI (`core/tools/cli_parse.c`)
  as an alias for the existing `--tiny-device` flag, using ORT
  "execution provider" terminology. Accepts the same values:
  `auto|cpu|cuda|openvino|coreml|coreml-ane|coreml-gpu|coreml-cpu|rocm`.
  Both flags write to the same `CLISettings.tiny_device` field; the
  ORT EP selection in `core/src/dnn/ort_backend.c` is unchanged.
  (2) Register `vmaf_tiny_v3.int8.onnx` as a directly-addressable
  model (`id: vmaf_tiny_v3_int8`) in `model/tiny/registry.json`
  alongside a new sidecar `model/tiny/vmaf_tiny_v3.int8.json` so users
  can load the quantised variant by ID rather than relying on the
  implicit `quant_mode: dynamic` redirect from the fp32 path.
  User docs: [`docs/usage/cli.md`](../docs/usage/cli.md) updated with
  `--dnn-ep` entry and alias note.
