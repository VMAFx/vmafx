- **`vmaf-tune --score-backend=vulkan` — vendor-neutral GPU scoring
  ([ADR-0314](../docs/adr/0314-vmaf-tune-score-backend-vulkan.md)).**
  Adds `vulkan` as a `--score-backend` choice (alongside `cuda` /
  `sycl` / `cpu`) so AMD, Intel Arc, and Apple-MoltenVK hosts can run
  GPU-accelerated VMAF scoring without the NVIDIA-only CUDA path. The
  auto-detection chain becomes `cuda > vulkan > sycl > cpu`; existing
  NVIDIA boxes see no behaviour change. Strict-mode failures stay
  strict per ADR-0299 — no silent CPU downgrade. The CLI flag, the
  detection plumbing in `score_backend.py`, and the libvmaf Vulkan
  backend (ADR-0127 / 0175 / 0186) all shipped earlier; this entry
  captures the operator-facing flip.
