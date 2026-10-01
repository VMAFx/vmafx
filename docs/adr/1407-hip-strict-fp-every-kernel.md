<!-- markdownlint-disable MD013 MD060 -->
# ADR-1407: Every HIP kernel compiles with contraction off and correctly rounded fp32 division and square root

- **Status**: Accepted
- **Date**: 2026-10-01
- **Deciders**: lusoris
- **Tags**: hip, gpu, numerics, build, rc3, fork-local

## Context

hipcc (amdclang++) compiles device code with `-ffp-contract=fast`: `a * b + c` becomes one fused multiply-add, across statements too. The CPU reference build (gcc/clang, x86-64 baseline, no `-mfma` in scalar TUs) does not contract, so a contracted kernel rounds differently from the extractor it mirrors. `core/src/meson.build` turned contraction off for three kernels only, through a per-kernel table (`hip_cu_extra_flags`, [ADR-0594](0594-hip-ssimulacra2-blur-fp-contract-off.md)): `ssimulacra2_blur`, `integer_ssim_score` and `speed_pipeline`. The other eighteen kernels contracted, and `float_ssim` carries `#pragma clang fp contract(off)` in the part that mirrors the CPU ([ADR-1382](1382-hip-twin-cpu-option-parity.md)). Two floating-point behaviours for one kind of kernel is a HISS-19 defect, and every new exact kernel had to remember to join the table (`T-HIP-FP-CONTRACT-DEFAULT-2026-09-29`).

[ADR-1367](1367-sycl-strict-fp-every-feature-tu.md) settled the same question for SYCL: one strict line for every feature TU, a device test, and an exemption only for a twin that gets more than 10% slower at 4K with no parity gain. The row asks for the HIP measurement on an AMD device, which ADR-1367's host did not have.

## Decision

Every HIP kernel is compiled with one list, defined once in `core/src/meson.build` between the `VMAF HIP strict FP policy` markers:

`hip_strict_fp_args = ['-ffp-contract=off', '-fhip-fp32-correctly-rounded-divide-sqrt']`

The second flag is hipcc's default; it is passed explicitly so that a toolchain default cannot move a score. The per-kernel table is removed: a kernel listed in `hip_kernel_sources` gets the policy, and there is no way to exempt one. A kernel that needs a fused operation writes `fmaf()` / `fma()`. The existing `#pragma clang fp contract(off)` lines stay; they are redundant now and harmless.

Two tests pin it. `test_hip_strict_fp_policy.py` reads the build files without a device: the list holds both flags, every HSACO compile gets it, no per-kernel table exists, no kernel source turns contraction back on, and the device probe is compiled with the same list. `test_hip_fp_arith_contract` runs a probe kernel compiled with that list on the device and compares 1 048 576 random and 2 197 boundary `a * b + c`, `a / b` and `sqrtf(|a|)` results with correctly rounded host values. Its host side is the SYCL test's source (`test_sycl_fp_arith_contract.c`) built for the HIP probe, so both backends are held to one set of operands and references.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep the per-kernel table | No other twin's output changes | Two FP behaviours for one kind of kernel; every new exact kernel must remember to join it | HISS-19; the row asks for one shared list |
| Contraction off only, leave division and square root to the toolchain default | One flag | The default is correct today, but the probe shows what a changed default costs: 303 181 of 1 048 576 divisions and 158 765 square roots differ with `-fno-hip-fp32-correctly-rounded-divide-sqrt` | Pinning the default is free |
| Exempt `float_vif_hip`, whose worst frame gets worse and which is 4% slower | Keeps its old numbers | Its mean barely moves (3.07e-6 to 3.18e-6 at 576x324, better at 4K), it stays inside the 5e-5 gate, and 4% is under ADR-1367's 10% bar | Nothing crosses the bar |
| `#pragma clang fp contract(off)` in every kernel source | No build change | Eighteen files to edit and every future one to remember; a pragma covers only the block it sits in | The flag covers everything |
| **One strict list for every kernel (chosen)** | One behaviour; a device test enforces it; the build test forbids exemptions | Seven twins' outputs change (each inside its ADR-0214 tolerance) | Chosen |

## Consequences

Measured on `ryzen-4090-arc` (gfx1036, ROCm 7.2.4, hipcc 7.2.53211, AMD clang 22.0.0git), `origin/master` 0e4368cdb against the same tree with the list.

- **The device test separates the three builds.** With the list: 0 mismatches. With hipcc's defaults: 126 577 of 1 048 576 multiply-adds differ. With `-ffp-contract=off -fno-hip-fp32-correctly-rounded-divide-sqrt`: 303 181 divisions and 158 765 square roots differ.
- **Per-twin max abs diff against `--backend cpu` at `--precision max`**, before -> after (576x324: Netflix pair, 48 frames; 4K: BBB 3840x2160, 22 frames; mean in brackets):
  - better: `float_adm` 2.50e-5 -> 2.53e-6 (1.66e-7 -> 4.34e-8) at 576x324; `float_ssim` 1.79e-7 -> 1.19e-7, 7 of 48 frames now bit-identical, and at 4K `scale=1` 7.21e-6 -> 4.83e-6; `ciede` at 4K 1.65e-6 -> 1.43e-6 (1.31e-6 -> 7.84e-7); `float_ms_ssim` 6.89e-8 -> 5.53e-8 at 576x324; `float_motion` at 4K 2.37e-5 -> 2.36e-5.
  - unchanged output, bit for bit: `vif`, `adm`, `motion`, `motion_v2`, `psnr`, `float_psnr`, `float_moment`, `ssim`, `cambi`, `ssimulacra2`, `speed_chroma`, `speed_temporal`. Of these `adm`, `motion`, `motion_v2`, `psnr`, `float_psnr`, `float_moment`, `cambi`, `ssimulacra2` and `speed_temporal` equal the CPU on every frame before and after.
  - worse max, inside tolerance: `float_vif` 2.72e-5 -> 3.82e-5 at 576x324 (mean 3.07e-6 -> 3.18e-6) and 5.74e-6 -> 7.02e-6 at 4K (mean 1.52e-6 -> 1.39e-6); `float_adm` at 4K 6.04e-6 -> 1.28e-5 (mean 2.03e-7 -> 1.84e-7); `float_ms_ssim` at 4K 5.82e-7 -> 1.22e-6 (mean 3.42e-7 -> 4.03e-7); `float_motion` at 576x324 3.01e-6 -> 3.12e-6; `ciede` at 576x324 1.133e-5 -> 1.134e-5. `psnr_hvs` changes below its own residual (8.37e-5 and 1.10e-2 against the CPU's running fp32 sum, before and after). The same twins moved the same way on SYCL (ADR-1367: `float_adm` 2.50e-5 -> 2.53e-6, `float_vif` 2.71e-5 -> 3.81e-5): what is left is not in the operations the list controls (transcendentals, summation order, fp32 where the CPU uses fp64).
  - ADR-0214 gate (`cross_backend_parity_gate.py --backends cpu hip`, Netflix pair): the 17 runnable cells pass before and after. The `motion` cell aborts on a metric name before and after (`T-CI-PARITY-GATE-MOTION-DEBUG-DEFAULT-2026-09-29`).
- **4K cost**, ms per frame, `(t(22) - t(2)) / 20`, runs of the two builds interleaved, load average 12 to 26 from other jobs: `float_psnr` 4.49 -> 4.44, `motion` 12.92 -> 13.08, `motion_v2` 13.98 -> 13.81, `float_motion` 20.38 -> 19.97, `psnr` 8.62 -> 8.44 (median of 7); `float_vif` 82.94 -> 86.49, `adm` 76.76 -> 72.96, `vif` 141.17 -> 139.15, `float_ms_ssim` 164.61 -> 157.06, `float_ssim` at `scale=1` 93.60 -> 84.76, `float_adm` 77.95 -> 78.75 (median of 5); `ciede` 73.60 -> 74.68, `psnr_hvs` 18.26 -> 18.60, `ssimulacra2` 3765 -> 3649, `cambi` 99.49 -> 98.79, `speed_chroma` 5.46 -> 5.50, `speed_temporal` 14.26 -> 14.60, `ssim` 136.9 -> 139.1, `float_moment` 8.76 -> 8.00 (median of 3, not interleaved). `float_vif` is 4.3% slower in every interleaved sample; no other twin moves outside its run-to-run spread, and none is 10% slower.
- **Negative**: seven twins' outputs change (`float_adm`, `float_vif`, `float_motion`, `float_ssim`, `float_ms_ssim`, `ciede`, `psnr_hvs`), so a stored per-build HIP output comparison needs a re-run; no fork snapshot under `testdata/` is a HIP output. The gfx1036 dropped a run of commands in at least five of about 150 runs during the measurement (one `vif`, one `float_moment` and three `adm` runs, each with one or two wrong frames, on both builds: in an interleaved repeat 15 of 15 `adm` runs with the list and 14 of 15 without were clean); that is `T-HIP-GFX1036-DROPPED-DISPATCHES-2026-10-01`, and those runs were repeated.
- **Neutral / follow-ups**: the CUDA twins still contract in all but two kernels (`T-CUDA-FP-CONTRACT-DEFAULT-2026-09-29`, its own policy ADR in review). `float_vif_hip`'s residual against the CPU is the next thing to look at once both backends compile strict.

## References

- req: RC3 HIP lane brief (2026-10-01): "T-HIP-FP-CONTRACT-DEFAULT-2026-09-29: HIP kernels contract a*b+c into an FMA everywhere except two; turn contraction off through one shared flag list (the SYCL precedent is ADR-1367 ...), pin it with a test, measure parity and ms/frame before/after per twin."
- [ADR-1367](1367-sycl-strict-fp-every-feature-tu.md) — the SYCL policy and the 10% bar.
- [ADR-0594](0594-hip-ssimulacra2-blur-fp-contract-off.md) — the per-kernel table this replaces; [ADR-0564](0564-integer-ssim-gpu-real-kernels.md), [ADR-1384](1384-hip-speed-device-resident.md) — its other two entries.
- [ADR-0214](0214-gpu-parity-ci-gate.md) — the tolerances.
- [Research-1407](../research/1407-hip-strict-fp-every-kernel.md) — the full before / after table.
