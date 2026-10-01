<!-- markdownlint-disable MD013 MD060 -->
# Research-1407: contraction off in every HIP kernel — parity and cost per twin on a gfx1036

- **Status**: Active
- **Workstream**: RC3 HIP lane, `T-HIP-FP-CONTRACT-DEFAULT-2026-09-29`; decision in [ADR-1407](../adr/1407-hip-strict-fp-every-kernel.md)
- **Last updated**: 2026-10-01

## Question

hipcc contracts `a * b + c` into a fused multiply-add for device code by default, and all but three HIP kernels were built that way. What does each twin's output and cost do when every kernel is built with `-ffp-contract=off`, and can a test tell on the device whether a kernel was contracted?

## Sources

- `core/src/meson.build` (`hip_kernel_sources`, the former `hip_cu_extra_flags`, `hip_strict_fp_args`).
- [ADR-1367](../adr/1367-sycl-strict-fp-every-feature-tu.md) and [Research-1367](1367-sycl-strict-fp-every-feature-tu.md), the SYCL precedent.
- Host: `ryzen-4090-arc`, AMD gfx1036 iGPU, ROCm 7.2.4 (`hipcc` HIP 7.2.53211, AMD clang 22.0.0git), Linux 7.2.8.
- Builds: `meson setup build-hip core -Denable_hip=true -Denable_hipcc=true -Dhip_gfx_targets=gfx1036 -Denable_cuda=false -Denable_sycl=false --buildtype=release -Db_lto=false`, once at `origin/master` 0e4368cdb ("before") and once with the list ("after").

## Findings

### A device probe separates the builds

`test_hip_fp_arith_contract` runs one kernel that computes `a * b + c`, `a / b` and `sqrtf(|a|)` for 1 048 576 random operand triples (random sign and mantissa, exponents -20 to 20) and 2 197 boundary combinations, and compares each result with the correctly rounded host value (the operation in fp64, rounded once).

| Probe compiled with | `a * b + c` differing | `a / b` | `sqrtf` |
|---|---|---|---|
| `hip_strict_fp_args` | 0 | 0 | 0 |
| hipcc defaults | 126 577 | 0 | 0 |
| `-ffp-contract=off -fno-hip-fp32-correctly-rounded-divide-sqrt` | 0 | 303 181 | 158 765 |

So contraction is the only thing hipcc's defaults change on this device, and the division flag, though a default, is worth pinning.

### Parity per twin, before and after

Max abs diff against `--backend cpu --threads 16` at `--precision max`, with the number of frames on which every output is bit-identical. 576x324 is the Netflix pair (48 frames), 4K is BBB 3840x2160 (22 frames). `float_ssim` at 4K runs with `scale=1`, the only scale the twin implemented at this commit.

| Twin | 576x324 before | 576x324 after | 4K before | 4K after |
|---|---|---|---|---|
| `vif_hip` | 5.36e-7 (0/48) | 5.36e-7 (0/48) | 2.98e-7 (0/22) | 2.98e-7 (0/22) |
| `adm_hip` | 0 (48/48) | 0 (48/48) | 0 (22/22) | 0 (22/22) |
| `motion_hip` | 0 (48/48) | 0 (48/48) | 0 (22/22) | 0 (22/22) |
| `motion_v2_hip` | 0 (48/48) | 0 (48/48) | 0 (22/22) | 0 (22/22) |
| `psnr_hip` | 0 (48/48) | 0 (48/48) | 0 (22/22) | 0 (22/22) |
| `float_psnr_hip` | 0 (48/48) | 0 (48/48) | 0 (22/22) | 0 (22/22) |
| `float_moment_hip` | 0 (48/48) | 0 (48/48) | 0 (22/22) | 0 (22/22) |
| `integer_ssim_hip` | 2.32e-14 (0/48) | 2.32e-14 (0/48) | 5.58e-13 (0/22) | 5.58e-13 (0/22) |
| `cambi_hip` | 0 (48/48) | 0 (48/48) | 0 (22/22) | 0 (22/22) |
| `ssimulacra2_hip` | 0 (48/48) | 0 (48/48) | 0 (22/22) | 0 (22/22) |
| `speed_chroma_hip` | 1.19e-6 (47/48) | 1.19e-6 (47/48) | 1.43e-6 (20/22) | 1.43e-6 (20/22) |
| `speed_temporal_hip` | 0 (48/48) | 0 (48/48) | 0 (22/22) | 0 (22/22) |
| `float_adm_hip` | 2.50e-5 (0/48) | 2.53e-6 (0/48) | 6.04e-6 (0/22) | 1.28e-5 (0/22) |
| `float_vif_hip` | 2.72e-5 (0/48) | 3.82e-5 (0/48) | 5.74e-6 (0/22) | 7.02e-6 (0/22) |
| `float_motion_hip` | 3.01e-6 (1/48) | 3.12e-6 (1/48) | 2.37e-5 (1/22) | 2.36e-5 (1/22) |
| `float_ssim_hip` | 1.79e-7 (0/48) | 1.19e-7 (7/48) | 7.21e-6 (0/22) | 4.83e-6 (0/22) |
| `integer_ms_ssim_hip` | 6.89e-8 (0/48) | 5.53e-8 (0/48) | 5.82e-7 (0/22) | 1.22e-6 (0/22) |
| `ciede_hip` | 1.133e-5 (0/48) | 1.134e-5 (0/48) | 1.65e-6 (0/22) | 1.43e-6 (0/22) |
| `psnr_hvs_hip` | 8.37e-5 (0/48) | 8.37e-5 (0/48) | 1.10e-2 (0/22) | 1.10e-2 (0/22) |

The first twelve twins produce the same output before and after on every frame (the after build against the before build: 48 of 48 and 22 of 22 identical). Their kernels are integer, or were already built strict, or have no product feeding an add. `vif_hip`'s 5.4e-7 is therefore not a contraction effect.

Mean abs diff for the seven twins whose output changes:

| Twin | 576x324 mean before -> after | 4K mean before -> after |
|---|---|---|
| `float_adm_hip` | 1.66e-7 -> 4.34e-8 | 2.03e-7 -> 1.84e-7 |
| `float_vif_hip` | 3.07e-6 -> 3.18e-6 | 1.52e-6 -> 1.39e-6 |
| `float_motion_hip` | 9.09e-7 -> 9.16e-7 | 2.07e-6 -> 2.07e-6 |
| `float_ssim_hip` | 1.18e-7 -> 5.84e-8 | 2.68e-6 -> 1.90e-6 |
| `integer_ms_ssim_hip` | 2.45e-8 -> 1.97e-8 | 3.42e-7 -> 4.03e-7 |
| `ciede_hip` | 1.02e-5 -> 1.02e-5 | 1.31e-6 -> 7.84e-7 |

`psnr_hvs_hip` moves by at most 1.3e-6 between the builds, far below its residual against the CPU's running fp32 sum (`T-PSNR-HVS-CPU-FLOAT-SUM-4K-2026-09-30`).

The picture matches SYCL's (ADR-1367): `float_adm` improves tenfold at 576x324, `float_vif`'s worst frame gets worse while its mean stays, and the rest move inside their existing residual. `cross_backend_parity_gate.py --backends cpu hip` on the Netflix pair passes its 17 runnable cells before and after; the `motion` cell aborts on a metric name in both.

### Cost at 4K

`(t(22) - t(2)) / 20` ms per frame, whole CLI. The host ran other agents' builds throughout (load average 12 to 26), so the light twins were re-measured with the two builds interleaved, seven repetitions each, and the heavier ones with five; the rest are the median of three runs per build, not interleaved.

| Twin | Before | After | Reps |
|---|---|---|---|
| `float_psnr_hip` | 4.49 | 4.44 | 7, interleaved |
| `psnr_hip` | 8.62 | 8.44 | 7, interleaved |
| `motion_hip` | 12.92 | 13.08 | 7, interleaved |
| `motion_v2_hip` | 13.98 | 13.81 | 7, interleaved |
| `float_motion_hip` | 20.38 | 19.97 | 7, interleaved |
| `adm_hip` | 76.76 | 72.96 | 5, interleaved |
| `float_adm_hip` | 77.95 | 78.75 | 5, interleaved |
| `float_vif_hip` | 82.94 | 86.49 | 5, interleaved |
| `float_ssim_hip` (`scale=1`) | 93.60 | 84.76 | 5, interleaved |
| `vif_hip` | 141.17 | 139.15 | 5, interleaved |
| `integer_ms_ssim_hip` | 164.61 | 157.06 | 5, interleaved |
| `speed_chroma_hip` | 5.46 | 5.50 | 3 |
| `float_moment_hip` | 8.76 | 8.00 | 3 |
| `speed_temporal_hip` | 14.26 | 14.60 | 3 |
| `psnr_hvs_hip` | 18.26 | 18.60 | 3 |
| `ciede_hip` | 73.60 | 74.68 | 3 |
| `cambi_hip` | 99.49 | 98.79 | 3 |
| `integer_ssim_hip` | 136.9 | 139.1 | 3 |
| `ssimulacra2_hip` | 3765 | 3649 | 3 |

`float_vif_hip` is the one consistent change: its five after samples (84.7 to 87.2) all sit above its five before samples (81.6 to 84.1), 4.3% on the median. Every other twin's difference is inside its own run-to-run spread. No twin is 10% slower, ADR-1367's bar for an exemption.

### Wrong frames during the measurement were the device, not the flag

Five runs showed frames far from the CPU: `vif_hip` 2.1e-3 on one 4K frame, `float_moment_hip` 1.5e4 on one frame, and `adm_hip` 1.5e-2 on one or two frames in three runs. A repeat of each was clean, and an interleaved repeat of `adm_hip` at 4K gave 15 of 15 clean runs with the list and 14 of 15 without. That is the gfx1036 losing a run of a stream's commands (`T-HIP-GFX1036-DROPPED-DISPATCHES-2026-10-01`), at a higher rate than that row's one frame in 10^4 while the host was loaded. The tables above are from clean runs.

## Alternatives explored

See the decision matrix in [ADR-1407](../adr/1407-hip-strict-fp-every-kernel.md).

## Open questions

- `float_vif_hip`: a systematic 3e-6 mean offset and a worst frame at 3.8e-5 remain with strict arithmetic; SYCL's ADR-1367 attributes the same offset to `log2` and to the CPU's fp64 `1 + x / y`. Not isolated here.
- The CUDA twins' counterpart is `T-CUDA-FP-CONTRACT-DEFAULT-2026-09-29`.

## Related

- [ADR-1407](../adr/1407-hip-strict-fp-every-kernel.md), [ADR-1367](../adr/1367-sycl-strict-fp-every-feature-tu.md), [ADR-0594](../adr/0594-hip-ssimulacra2-blur-fp-contract-off.md), [ADR-0214](../adr/0214-gpu-parity-ci-gate.md).
