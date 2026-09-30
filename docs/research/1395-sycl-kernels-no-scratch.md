<!-- markdownlint-disable MD013 MD060 -->
# Research-1395: Scratch memory in the SYCL kernels on an Arc A380 under the xe driver

- **Status**: Active
- **Workstream**: [ADR-1395](../adr/1395-sycl-kernels-no-scratch.md), [ADR-0220](../adr/0220-sycl-fp64-fallback.md)
- **Last updated**: 2026-10-01

## Question

On the RC3 host's Arc A380, 16 SYCL tests fail on master and on every branch
built from it, including #1630, which changes no kernel's control flow. Is the
cause in vmafx or in the platform? Which kernels are affected, how can a build
catch a new one, and how can a user on the affected platform find out?

## Sources

- Host: Arc A380 (`dg2-g11`, PCI ID 56a5) on Linux 7.2.8 with
  `xe.force_probe=56a5` and `i915.force_probe=!56a5`; Intel compute runtime
  26.35.39758.10 and IGC 2.41.5 (both installed 2026-09-20); icpx 2026.0;
  `ONEAPI_DEVICE_SELECTOR=level_zero:0`. The boots up to 2026-09-27 bound the
  A380 to i915 (`i915 0000:03:00.0: [drm] Found dg2/g11`); the boot of
  2026-09-30 binds it to xe.
- SYCL 2020 `info::kernel_device_specific::private_mem_size`; the oneAPI
  extension `ext::intel::info::kernel_device_specific::spill_memory_size`
  (`sycl/info/ext_intel_kernel_info_traits.def`) and aspect
  `ext_intel_spill_memory_size`; the `sycl_ext_intel_grf_size` kernel property
  (`sycl/ext/intel/experimental/grf_size_properties.hpp`, `grf_size<128|256>`),
  all read from the installed oneAPI 2026.0 headers.
- The meson test log of 2026-09-25 23:07 in the `agy-gpu-adm-aim` worktree
  (i915, same runtime and compiler).
- Measurement commands in the Findings below; all builds were
  `--buildtype=release -Db_lto=false`, SPIR-V JIT unless noted.

## Findings

### The defect needs no vmafx code

Standalone SYCL kernels on the A380 under xe, each checked against the same
function on the host, in work-groups of 256 work-items, 1, 8, 64 and 1024 of
them per run:

| Probe | Scratch per thread | Wrong work-items |
|---|---|---|
| private array of 256 floats, SIMD-16 | 16384 B private | all, at every size |
| private array of 110 floats, SIMD-16 / SIMD-32 | 7040 / 14080 B private | all 256 in one work-group; 336 / 64 of 2048 with 8; 37746 / 4660 of 262144 with 1024 |
| private array of 32 floats, SIMD-16 | 2048 B private | none up to 8 work-groups; 16089 of 16384 with 64 |
| 96 live floats, SIMD-32 | 4096 B spill | all 256 in one work-group; 261888 of 262144 with 1024 |
| 8 floats; 48 or 160 live floats at SIMD-16; 96 at SIMD-8 | none | none |
| 96 live floats, SIMD-32, `grf_size<256>` (65536 work-items) | none | none |

The failures are the same for JIT and `dg2-g11` AOT images, on Level Zero and on
OpenCL, and every probe is correct on the OpenCL CPU device. The self-test's
integer versions (`core/src/sycl/scratch_check.cpp`) give 256 of 256 wrong for
the private-array probe (16384 B) and 256 of 256 for the SIMD-32 spill probe
(4096 B).

### Which kernels use scratch

A kernel audit (every `sycl::get_kernel_ids()` entry of `libvmaf.so` built for
the A380 and queried for both sizes) finds 25 of 109 kernels with scratch on
master `10f27efe2`:

| Extractor | Kernels | Private / spill bytes per thread |
|---|---|---|
| `vif_sycl` (SIMD-32 only) | `launch_vif_hori_impl<0..3, 32>`, `launch_vif_fused_impl<0..3, 32>` | spill 96 to 8832 |
| `float_vif_sycl` | `launch_compute<0>`, `launch_decimate<1>` | private 14080, 2432 |
| `float_adm_sycl` | `launch_aim_cm`, `launch_csf_cm` | private 896 each |
| `adm_sycl` | `launch_csf_den_cm` | spill 864 |
| `psnr_hvs_sycl` | `launch_psnr_hvs` | private 2432 |
| `speed_chroma_sycl`, `speed_temporal_sycl` | `launch_scale` and `launch_decimate` for 8 and 16 bits, each also as a rounded-range wrapper (8 kernels) | private 832 to 3584 |
| `motion_sycl`, `motion_v2_sycl` | `submit_sad<int64_t>` (above 15 bits per component) | spill 768 |
| `cambi_sycl` | `launch_reset` and its rounded-range wrapper | private 1280, 896 |

IGC dumps of each test on master (`IGC_ShaderDumpEnable`) tie the 16 failing
tests to these kernels: 15 of them build at least one, and the 16th,
`psnr_hvs_parity_simd32`, is `psnr_hvs_parity`'s executable run with
`IGC_ForceOCLSIMDWidth=32`, so it runs the same `launch_psnr_hvs` kernel. No test that
builds only scratch-free kernels fails. Ten passing tests also build a scratch
kernel but either do not run it (`motion`'s `submit_sad<int64_t>` only runs
above 15 bits per component) or run it in small dispatches, where the probes
above often pass too (`cambi`'s `launch_reset` covers 2048 work-items;
`speed_temporal_parity` passes and `speed_temporal_parity_large` fails). Under
i915 on 2026-09-25, 13 of the 16 passed (`psnr_hvs_parity_simd32`,
`shared_planes` and `motion_tiny_frames` did not exist yet).

### The large register file

Rebuilding master's JIT image with `SYCL_PROGRAM_COMPILE_OPTIONS=-ze-opt-large-register-file`
(256 registers per thread for every kernel) leaves 14 of the 25: all spills and
`float_vif`'s `launch_compute<0>` array go, the other private arrays stay.

Per kernel, `grf_size<256>` on the eight `vif` SIMD-32 kernels removes their
spills; the audit then finds 15 kernels with scratch (with `adm_sycl` and
`psnr_hvs_sycl` cleared in #1656 and #1657), the ratchet list.
`icpx -fsycl -fno-sycl-rdc -fsycl-targets=spir64_gen,spir64` with the default
19-target `sycl_icpx_aot_targets` list builds a kernel with `grf_size<256>` for
every target.

### `vif_sycl` at SIMD-32, before and after

Forced with `VMAF_SYCL_VIF_SUBGROUP_SIZE`. Max abs diff of the four
`integer_vif_scale*` outputs against the CPU `vif` extractor at
`--precision max` (Netflix pair, 48 frames; BBB 3840x2160, 50 frames), and
ms/frame as `(t(22) - t(2)) / 20`, median of 3, load average 9 to 30 from other
jobs:

| Mode | Before | After |
|---|---|---|
| SIMD-32, separate passes | frame 0: numerator = denominator = 0, run aborts | scale 0..3: 4.99e-8 / 1.04e-7 / 1.60e-7 / 3.87e-7 (576x324), 7.02e-8 / 8.86e-8 / 1.05e-7 / 1.47e-7 (4K); 23.43 ms/frame at 4K |
| SIMD-16, separate passes (default) | same diffs; 22.99 ms/frame | same diffs; 23.22 ms/frame |
| SIMD-32, `vif_fused=true` | frame 0: 0 / 0, run aborts | 576x324 as above; 4K scales 1..3 up to 6.4e-4; 67.06 ms/frame |
| SIMD-16, `vif_fused=true` | 576x324 as above; 4K scales 1..3 up to 6.6e-4; 25.38 ms/frame | same diffs; 24.57 ms/frame |

SIMD-16 and SIMD-32 outputs are bit-identical in the separate-pass mode (every
value of 48 and 50 frames). The 4K `vif_fused` gap is older than this change and
also occurs at SIMD-16; it varies between SIMD-32 runs. See the open questions.

### Cost of the self-test

On the A380, `vmaf --backend sycl` with a two-frame `psnr_sycl` run takes 29 to
32 ms with `VMAF_SYCL_SCRATCH_SELFTEST=0` and 33 to 37 ms with the self-test,
with the compute runtime's kernel cache warm; the first run with a cold cache
took 0.42 s.

## Alternatives explored

- **Refusing the device, or routing extractors to the CPU, when the self-test
  fails.** Both hide the extractors that are correct on xe. See the ADR's
  alternatives table.
- **The large register file for every kernel.** Measured above: it removes the
  spills but leaves 14 kernels with private arrays, and it halves occupancy for
  kernels that do not need it.
- **A static zeinfo check of the AOT images.** IGC's zeinfo records
  `private_size` and `spill_size` per kernel, but only for the AOT targets, not
  for the SPIR-V image that other devices compile at run time.

## Open questions

- Whether the B580 and the UHD 770 list other scratch kernels: their register
  files differ, so the audit can find more there.
- `vif_fused=true` reads each scale's input from `rd_ref` / `rd_dis` and writes
  the next scale's input into the same buffers in the same kernel. At 4K scales
  1 to 3 are up to 6.6e-4 from the CPU on both sub-group sizes, and SIMD-32 runs
  differ from each other: this looks like a read/write race between work-groups.
  It is outside this change.
- The defect has not been reported to Intel; a reduced reproducer is the
  private-array probe above.

## Related

- ADRs: [ADR-1395](../adr/1395-sycl-kernels-no-scratch.md)
- PRs: #1630 (records the A380 results on master and with its change)
- Issues: #1641 (RC3 handoff)
