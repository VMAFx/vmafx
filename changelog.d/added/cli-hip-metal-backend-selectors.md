# CLI: HIP and Metal backend selectors (`--no_hip`, `--hip_device`, `--no_metal`, `--metal_device`, `--backend hip|metal`)

The standalone `vmaf` CLI now exposes the HIP (AMD ROCm) and Metal (Apple
Silicon) backends via the same flag pairs used by CUDA, SYCL, and Vulkan:

- `--no_hip` — suppress HIP dispatch even when the binary was built with
  `-Denable_hip=true`.
- `--hip_device <N>` — activate the HIP backend and pick device by ordinal
  (0 = first AMD GPU).
- `--no_metal` — suppress Metal dispatch (macOS only).
- `--metal_device <N>` — activate the Metal backend and pick device by ordinal
  (0 = first Metal device, typically the integrated Apple GPU on Apple Silicon).
- `--backend hip` / `--backend metal` — exclusive selectors that disable all
  other backends and default the device index to 0. `--backend cpu` now also
  disables HIP and Metal alongside CUDA/SYCL/Vulkan.

Activation follows the Vulkan opt-in model: the device flag must be explicitly
set (or `--backend hip|metal` passed) for the backend to engage; simply building
with HIP/Metal enabled is not sufficient to trigger it at runtime.

ADR: [ADR-0422](../../docs/adr/0422-cli-hip-metal-backend-selectors.md).
