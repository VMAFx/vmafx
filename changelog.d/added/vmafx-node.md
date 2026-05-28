### Added: vmafx-node Go worker binary (Phase 4b.2)

`vmafx-node` is the data-plane worker for the VMAFX distributed platform.

- Single static Go binary at `cmd/vmafx-node/`.
- Connects to `vmafx-controller` via gRPC `VmafxController` service (ADR-0711).
- GPU vendor auto-detection: NVIDIA (nvidia-smi), AMD (rocm-smi), Intel (clinfo),
  Apple Metal (system_profiler), CPU fallback (`pkg/gpu/`).
- libvmaf VMAF scoring via cgo at `pkg/libvmaf/` (existing wrapper, direct
  `libvmaf.so` link).
- ffmpeg encode pipeline at `pkg/encoder/` extended with hardware encoders:
  h264_nvenc, hevc_nvenc, h264_qsv, hevc_qsv, h264_amf, hevc_amf, libsvtav1,
  libaom-av1; codec availability probed at startup.
- ONNX Runtime inference registry at `pkg/ai/` (Stage 1 subprocess path;
  Stage 2 will use `github.com/yalue/onnxruntime_go` directly).
- VmafxController gRPC client stubs vendored at `gen/go/controller/`
  (mirrors `cmd/vmafx-controller/proto/controller.proto` from ADR-0711).
- Prometheus metrics on `:9090/metrics`.
- SIGTERM graceful shutdown with 30 s drain timeout.
- Multi-variant Docker images at `docker/Dockerfile.node{,-cuda12,-rocm6,-sycl-oneapi2026,-cpu}`.
- Helm `templates/node.yaml` — worker pool Deployment + metrics Service
  gated on `.Values.node.enabled`; GPU nodeSelector + tolerations configurable.
- 12-factor config: `VMAFX_CONTROLLER_ADDR`, `VMAFX_NODE_ID`, `VMAFX_BACKEND`,
  `VMAFX_GPU_DEVICE`, `VMAFX_LOG_LEVEL`, `VMAFX_MODEL_DIR`.

ADR: [0713](../docs/adr/0713-vmafx-node-impl.md)
