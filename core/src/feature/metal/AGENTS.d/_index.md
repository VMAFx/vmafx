<!-- markdownlint-disable MD013 MD060 -->
# `core/src/feature/metal/` — Metal feature-kernel directory

Parent: [../AGENTS.md](../../AGENTS.md). Metal backend runtime lives at
[`../../metal/AGENTS.md`](../../../metal/AGENTS.md); ADRs governing this
directory listed in "Governing ADRs" section at bottom of this file.

## Purpose

Contains one `.mm` (Objective-C++ host dispatch) + one `.metal` (Metal
Shading Language device kernel) pair per feature extractor in Metal
GPU backend, plus per-T8-1 scaffold `.c` stubs, superseded.

Only `.mm` + `.metal` pairs functional. `.c` stubs (e.g.
`float_psnr_metal.c`) replaced by `.mm` counterparts once real kernel
lands; removed from `metal_sources` in `core/src/metal/meson.build`
when conversion happens.

## Governing ADRs

- [ADR-1176](../../../../../docs/adr/1176-metal-motion-v2-mirror-closeout.md) — Metal motion_v2 mirror closeout and reflect-101 parity
- [ADR-0490](../../../../../docs/adr/0490-float-ms-ssim-metal-port.md) — T8-2b: float_ms_ssim_metal port
- [ADR-0421](../../../../../docs/adr/0421-metal-first-kernel-motion-v2.md) — T8-1c through T8-1k batch specification
- [ADR-0420](../../../../../docs/adr/0420-metal-backend-runtime-t8-1b.md) — runtime (T8-1b), prerequisite
- [ADR-0361](../../../../../docs/adr/0361-metal-compute-backend.md) — scaffold (T8-1), origin
- [ADR-0214](../../../../../docs/adr/0214-gpu-parity-ci-gate.md) — `places=4` cross-backend parity gate
