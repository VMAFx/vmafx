- **A cross-device parity run that compared nothing passed** (`T-TINY-AI-CROSS-DEVICE-PARITY-UNGATED-2026-09-25`).
  `vmaf_train.cross_backend.CrossBackendReport.ok` is now False when no output was compared, when a requested
  provider is missing, or when ONNX Runtime accepted a provider but ran the session on the CPU, so
  `vmaf-train cross-backend --fail-on-mismatch` no longer exits 0 on a CPU-only host. New
  `scripts/ci/tiny_ai_cross_device_parity_gate.py` checks `vmaf_tiny_v2` (1e-4) and `smoke_fp16_v0` (1e-2)
  between two providers and names the missing provider; it is not yet wired into a hardware job.
