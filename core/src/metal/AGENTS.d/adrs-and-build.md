---
paths:
  - core/src/metal/meson.build
  - core/meson_options.txt
invariant: Governing Metal backend ADRs and macOS versus non-macOS Meson build commands.
---
# Metal governing ADRs and build configuration

## Governing ADRs

- [ADR-0361](../../../../docs/adr/0361-metal-compute-backend.md) —
  Metal scaffold-only audit-first PR (T8-1, this PR).
- [ADR-0212](../../../../docs/adr/0212-hip-backend-scaffold.md) — HIP
  scaffold-only audit-first PR (T7-10); precedent this PR mirrors.
- [ADR-0175](../../../../docs/adr/0175-vulkan-backend-scaffold.md) —
  Vulkan scaffold (T5-1); original audit-first GPU-backend
  precedent.
- [ADR-0246](../../../../docs/adr/0246-gpu-kernel-template.md) — GPU
  kernel-template decision; source Metal mirror tracks (via HIP
  twin that mirrors CUDA twin).
- [ADR-0214](../../../../docs/adr/0214-gpu-parity-ci-gate.md) —
  `places=4` cross-backend gate; runtime PR's incoming numerics
  gate.
- [ADR-0145](../../../../docs/adr/0145-motion-v2-neon-bitexact.md) —
  motion_v2 NEON twin on Apple Silicon CPU. Coordinates with this
  ADR: NEON stays CPU-side path; Metal is GPU-side path.

## Build

```bash
# On macOS:
meson setup build -Denable_metal=enabled
ninja -C build
python3 "$(git rev-parse --show-toplevel)/scripts/ci/run_meson_test.py" -- -C build test_metal_smoke

# On Linux / Windows: -Denable_metal=auto resolves to disabled
# (no Metal frameworks); -Denable_metal=enabled fails the meson
# setup with a clear missing-framework error from the dependency()
# probe.
```

Scaffold has zero hard runtime dependencies on non-macOS hosts. On
macOS meson `dependency('Metal') / dependency('MetalKit')` probes
resolve to system frameworks; `Build — macOS Metal
(T8-1 scaffold)` in `.github/workflows/libvmaf-build-matrix.yml`
runs this exact configuration on every PR.
