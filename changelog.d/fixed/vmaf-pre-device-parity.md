- `vmaf_pre` FFmpeg filter `device=` option now accepts all 12 `VmafDnnDevice` strings
  (`openvino-npu`, `openvino-cpu`, `openvino-gpu`, `coreml`, `coreml-ane`, `coreml-gpu`,
  `coreml-cpu` were previously silently rejected with `AVERROR(EINVAL)`). Parity with the
  main `libvmaf` filter's `tiny_device=` option is now complete (ADR-0482).
