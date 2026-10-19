<!-- markdownlint-disable MD060 -->
# SYCL Backend

The SYCL backend runs libvmaf's feature extractors on Intel GPUs (Arc, Xe
integrated graphics) through oneAPI DPC++ and Level Zero. Build it with
`-Denable_sycl=true`, select it with `--backend sycl`, and expect scores that
equal the CPU's bit for bit for 24 of the 25 gated features.

This page covers what you need to build and run it, how closely it agrees with
the CPU, and what is still open. The details live on five further pages:

| Page | What it holds |
| --- | --- |
| [AOT targets](aot.md) | Ahead-of-time device code, the default target list, sub-group sizes |
| [Device sanitizer](device-sanitizer.md) | `-Dsycl_device_asan=true`, what the DPC++ device AddressSanitizer reports on an Arc GPU and what it cannot prove |
| [Twin notes](twins.md) | How each twin reaches the CPU's bits; compile-line guarantees |
| [Zero-copy and pictures](zero-copy.md) | QSV / VA-API import, D3D11, picture pre-allocation |
| [Developer notes](developer-notes.md) | fp64-free kernels, scratch-memory rules, source layout |
| [History](history.md) | Dated change log with every before / after measurement |

Platform guides: [bundling a self-contained binary](bundling.md) and
[SYCL on Windows](windows.md).

## Hardware

The backend targets any SYCL device that Level Zero exposes.

- **Intel Arc discrete GPUs**: A-series (DG2 / ACM) and B-series
  (Battlemage).
- **Intel integrated GPUs**: Tiger Lake and later (Xe-LP, Xe-LPG, Xe2).
- **Other devices**: AMD through the HIP plugin and NVIDIA through the CUDA
  plugin work only when the DPC++ compiler is built with them. The shipped
  oneAPI binaries do not include the HIP plugin.

A device that is not in the ahead-of-time list, such as a Data Center GPU Flex
or Max card, still works: the binary also carries portable SPIR-V, which the
Level Zero runtime compiles on first use.

## Requirements

| Component | Version | Source of the pin |
| --- | --- | --- |
| oneAPI DPC++ (`icpx`) | 2026.1 | `ONEAPI_VERSION` in `build-config.env` |
| Level Zero loader | 1.34.0 | `LEVEL_ZERO_VERSION` in `build-config.env` |
| Intel compute runtime | 26.35.39758.10 | `INTEL_NEO_VERSION` in `build-config.env` |
| Intel `ocloc` | any release that knows every target | `scripts/ci/install-intel-ocloc.sh` |
| Linux render node | `/dev/dri/renderD*` | none |

`build-config.env` is the single source for these versions; the release image
and CI install exactly what it names. Older oneAPI releases may work, but only
the pinned set is tested.

`ocloc` is needed only for ahead-of-time builds (the default); see
[AOT targets](aot.md#what-the-build-needs-and-checks). AdaptiveCpp is an
alternative compiler, chosen with `-Dsycl_compiler=acpp` (see the table
below).

## Build

1. Configure from the repository root. The Meson source directory is `core/`:

    ```bash
    meson setup build core -Denable_sycl=true
    ```

2. Build:

    ```bash
    ninja -C build
    ```

The options that shape a SYCL build:

| Option | Default | Effect |
| --- | --- | --- |
| `enable_sycl` | `false` | Compile the SYCL backend and its kernels |
| `sycl_compiler` | `icpx` | Compiler: Intel `icpx`, or AdaptiveCpp `acpp` / `syclcc` (ADR-0335) |
| `sycl_icpx_aot_targets` | 19 Intel targets | Ahead-of-time device list; empty string selects SPIR-V JIT only. Ignored unless the compiler is `icpx` |
| `sycl_acpp_targets` | `generic` | AdaptiveCpp `--acpp-targets` value. Ignored when the compiler is `icpx` |
| `enable_cuda` | `false` | May be set together with `enable_sycl`; both backends then live in one binary |

!!! note
    A SYCL build is an `icx` / `icpx` build for its host code as well. Its
    CPU extractors take `log10`, `pow`, `powf` and the other math functions
    from glibc's `libm`, not from Intel's `libimf`, because the build passes
    `-no-intel-lib=libimf` to every link
    ([ADR-1495](../../adr/1495-icx-system-libm.md),
    [build flags](../../development/build-flags.md#icx-builds-use-glibcs-math-library)).
    The CPU half of a SYCL build therefore returns a GCC build's scores, and
    a SYCL twin that equals its own build's CPU extractor equals a GCC
    build's too. The option applies to the host link only; the device code
    and its math libraries do not change.

## Run

```bash
./build/tools/vmaf ...                   # SYCL used automatically if it initialises
./build/tools/vmaf --backend sycl ...    # exit 100 if SYCL cannot initialise
./build/tools/vmaf --no_sycl ...         # force the CPU path
./build/tools/vmaf --sycl_device 1 ...   # pick device index 1
```

| Flag | Meaning |
| --- | --- |
| `--backend sycl` | Use SYCL exclusively; an init failure exits with code `100` |
| `--no_sycl` | Disable the SYCL backend |
| `--sycl_device N` | Pick the device by index; without it SYCL's default selector decides, and `--backend sycl` uses index 0 |

Index `0` is whichever device SYCL's default selector picks, usually the first
discrete GPU. Pin an integrated GPU or a specific Arc card with
`--sycl_device`, or with the oneAPI runtime variable
`ONEAPI_DEVICE_SELECTOR=level_zero:N`.

Automatic selection falls back to the CPU when SYCL cannot initialise, for
example when a Level Zero or Unified Runtime library fails to load or a
container has no render node. The run still prints a score and exits 0; only a
stderr line such as `problem during vmaf_sycl_state_init, using CPU` shows it.
`--backend sycl` makes the failure explicit instead. See
[how selection works](../index.md#how-selection-works).

### Choosing a twin from `--feature`

With `--backend sycl`, a `--feature` that names a CPU extractor runs on that
extractor's SYCL twin
([ADR-1359](../../adr/1359-cli-feature-backend-twin.md)):

```bash
vmaf ... --backend sycl --feature cambi          # runs cambi_sycl
vmaf ... --backend sycl --feature psnr=enable_mse=true
vmaf ... --backend sycl --feature float_motion_sycl=motion_max_val=4
```

The twin runs only when it can honour the options and the frame size and bit
depth you gave. Otherwise the CPU extractor runs and `vmaf` prints one warning
that names the reason; the
[`feature_backends` receipt](../../usage/cli.md#backend-receipt-in-json-output)
in the JSON output lists which extractor ran where. The rules and the warning
texts are in
[CLI: feature extractors on a GPU backend](../../usage/cli.md#feature-extractors-on-a-gpu-backend).
Naming the twin (`adm_sycl`) always selects it, and it fails when it cannot
run.

A model's feature list resolves the same way, so the default model
`vmaf_v1.0.16_3d0h` runs its VIF, ADM, motion and CAMBI features on the
device.

## Environment variables

| Variable | Effect |
| --- | --- |
| `VMAF_SYCL_DISPATCH` | Per-feature dispatch strategy, `direct` or `graph` ([below](#dispatch-strategy)) |
| `VMAF_SYCL_USE_GRAPH` | `1` forces graph replay for every feature |
| `VMAF_SYCL_NO_GRAPH` | Deprecated: `1` forces direct submission and prints a warning |
| `VMAF_SYCL_IMPORT_DEBUG` | `1` logs shared frame-buffer addresses and each VA import at `INFO` |
| `VMAF_SYCL_PROFILE` | `1` enables queue profiling events |
| `VMAF_SYCL_TIMING` | `1` records per-extractor timing with a queue wait |
| `VMAF_SYCL_CHECKSUM` | `1` logs a CRC of the uploaded ref and dis frame buffers at `INFO` |
| `VMAF_SYCL_SCRATCH_SELFTEST` | `0` skips the start-up scratch-memory probes ([below](#scratch-memory-on-intel-gpus-adr-1395)) |
| `VMAF_SYCL_AOT_JOBS` | Parallel compiles of the `sycl-aot` test suite (default 4) |
| `ONEAPI_DEVICE_SELECTOR` | oneAPI runtime device filter, for example `level_zero:0` |

`VMAF_SYCL_DISPATCH` is also listed in the
[env-var reference](../../usage/env-vars.md#sycl-dispatch).

### Dispatch strategy

`VMAF_SYCL_DISPATCH` chooses how kernels reach the device:

| Value | Behaviour |
| --- | --- |
| `direct` | Submit kernels to an in-order queue. Lower per-frame overhead at small resolutions |
| `graph` | Replay a recorded SYCL graph ([ADR-0483](../../adr/0483-gpu-dispatch-parse-dedup.md)). Cuts kernel-launch overhead at 720p and above |

When nothing is set, an area threshold decides: `graph` from 1280 x 720 pixels
upward, `direct` below. The zero-copy VA-import path of the `libvmaf_sycl`
FFmpeg filter is the exception and defaults to `direct` at every resolution:
there the graph's output is byte-identical, but the per-frame de-tile import
plus the graph's compute barrier serialise decode and compute, which costs 15
to 25 % at 4K
([ADR-1121](../../adr/1121-sycl-qsv-zerocopy-p010-normalization.md)).
Set `VMAF_SYCL_USE_GRAPH=1` or `VMAF_SYCL_DISPATCH=<feature>:graph` to force
the graph there anyway.

`VMAF_SYCL_NO_GRAPH` is deprecated
([ADR-0841](../../adr/0841-env-var-consolidation.md)): it still works,
prints a one-shot warning, and the warning names v4.0 as the removal release.
`VMAF_SYCL_IMPORT_DEBUG` is read once at init, so changing it mid-run has no
effect.

## Numerical agreement with the CPU

The SYCL twins return the CPU extractors' bits. The cross-backend gate
compares 25 features; 24 have an exact-twin declaration in
`scripts/ci/exact_twins.d/*.sycl` and are held to tolerance 0. Only `ciede` is
bounded, at `1e-9` (measured 1.4e-11). The generated list is
[the exact-twin table](../../development/cross-backend-exact-twins.md) and the
procedure is in [the gate guide](../../development/cross-backend-gate.md).

Measured on an Intel Arc A380 (2026-10-02) with the default model, the VMAF
score of every frame equals `--backend cpu` on the Netflix `src01` pair (48
frames) and on 50 frames of BBB 3840x2160.

| CPU extractor (gate feature) | SYCL twin | Agreement | ADR |
| --- | --- | --- | --- |
| `adm` | `adm_sycl` | exact | 1362, 1451 |
| `float_adm` | `float_adm_sycl` | exact | 1434 |
| `vif` | `vif_sycl` | exact | 1432 |
| `float_vif` | `float_vif_sycl` | exact | 1422 |
| `motion`, `motion_debug`, `motion_mffw` | `motion_sycl` | exact | 1371, 1451, 1491 |
| `motion_v2`, `motion_v2_mffw` | `motion_v2_sycl` | exact | 1451, 1491 |
| `float_motion` | `float_motion_sycl` | exact | 1411 |
| `psnr` | `psnr_sycl` | exact | 1451 |
| `float_psnr` | `float_psnr_sycl` | exact | 1450 |
| `psnr_hvs` | `psnr_hvs_sycl` | exact | 1401 |
| `ssim` | `integer_ssim_sycl` | exact | 1443 |
| `float_ssim`, `float_ssim_lcs` | `float_ssim_sycl` | exact | 1463 |
| `float_ms_ssim`, `float_ms_ssim_lcs`, `float_ms_ssim_chroma` | `float_ms_ssim_sycl` | exact | 1414, 1466 |
| `ssimulacra2` | `ssimulacra2_sycl` | exact | 1446 |
| `float_moment` | `float_moment_sycl` | exact | 1449, 1497 |
| `cambi` | `cambi_sycl` | exact while the CPU's own top-K double sum is exact; otherwise within that sum's rounding (2.2e-15 at most over 50 frames of BBB 4K) | 1357 |
| `speed_chroma`, `speed_temporal` | `speed_chroma_sycl`, `speed_temporal_sycl` | exact | 1358, 1477 |
| `ciede` | `ciede_sycl` | bounded at `1e-9`; measured within 1.4e-11 (the host's `powf` differs) | 1436 |

`float_ansnr` has no twin; it was removed
([ADR-0865](../../adr/0865-ansnr-sunset-pre-vmaf-metric-drop.md)). The
extractor-by-extractor coverage across all backends is in
[metrics/features.md](../../metrics/features.md).

Four rules about device code make the table possible. Each is enforced by a
test, and [Twin notes](twins.md) explains how the twins meet them:

| Rule | Meaning | Where |
| --- | --- | --- |
| No fp64 in any kernel | Arc A-series has no fp64; a single fp64 instruction rejects the whole module | [ADR-0220](../../adr/0220-sycl-fp64-fallback.md), [developer notes](developer-notes.md#fp64-less-device-contract-t7-17) |
| Strict FP line | `-fp-model=precise -ffp-contract=off` plus correctly rounded `/` and `sqrt` | [ADR-1367](../../adr/1367-sycl-strict-fp-every-feature-tu.md), [twin notes](twins.md#what-the-sycl-compile-line-guarantees) |
| No scratch memory | Kernels that spill return wrong values on Arc A-series under the xe driver | [ADR-1395](../../adr/1395-sycl-kernels-no-scratch.md), [below](#scratch-memory-on-intel-gpus-adr-1395) |
| Sub-group size 16 or 32 | Xe2 targets reject a kernel that requires 8 | [ADR-1468](../../adr/1468-sycl-sub-group-sizes-every-aot-target.md), [AOT targets](aot.md#sub-group-sizes-and-the-aot-targets-adr-1468) |

The Netflix golden-data gate is CPU-only
([principles](../../principles.md#31-netflix-golden-data-gate)). The SYCL
backend's numerics are pinned by fork-added tests such as
`test_sycl_exact_twins`, not by the Netflix goldens.

## AOT targets (default, ADR-0568)

The default build compiles device code ahead of time for 19 Intel targets, so
the first launch does not pay a JIT compile. A device outside the list falls
back to SPIR-V JIT.

| Family | Targets |
| --- | --- |
| Arc A-series (DG2, ACM) | `dg2-g10`, `dg2-g11`, `acm-g10`, `acm-g11`, `acm-g12` |
| Xe-LP integrated (Tiger, Alder, Raptor Lake) | `tgllp`, `adl-s`, `adl-p`, `adl-n`, `rpl-s`, `rpl-p` |
| Xe-LPG integrated (Meteor, Arrow Lake) | `mtl-h`, `mtl-u`, `arl-h`, `arl-s`, `arl-u` |
| Xe2 (Lunar Lake, Battlemage) | `lnl-m`, `bmg-g21`, `bmg-g31` |

Override the list with `-Dsycl_icpx_aot_targets=dg2-g11,mtl-h`, or disable AOT
with `-Dsycl_icpx_aot_targets=''`. [AOT targets](aot.md) has the silicon behind
each name, the build and image checks, the target-list recipes and the
sub-group-size table.

## Scratch memory on Intel GPUs (ADR-1395)

A kernel uses scratch memory when the Intel graphics compiler puts a private
array in memory or spills registers there. On an Arc A-series GPU under the
Linux `xe` kernel driver, such kernels return wrong values with no error. This
was measured on an Arc A380 with compute runtime 26.35.39758.10 and IGC 2.41.5,
where the same kernels were correct under `i915`.

The fork's rule is that SYCL kernels use no scratch memory
([ADR-1395](../../adr/1395-sycl-kernels-no-scratch.md)). Since 2026-10-01 none
does: the list of kernels that still did,
[`core/src/sycl/scratch_ratchet.txt`](../../../core/src/sycl/scratch_ratchet.txt),
is empty. `float_adm_sycl` was the last extractor on it; until then it returned
NaN on an Arc A380 under xe and the run stopped with `problem reading
pictures`.

### What you see

At the first SYCL initialisation on each device, libvmaf runs two small probe
kernels, one with a private array and one that spills. When either returns
wrong values it logs a warning like this:

```text
libvmaf WARNING SYCL: Intel(R) Arc(TM) A380 Graphics returns wrong values from kernels that use
scratch memory (private-array probe: 256 of 256 work-items wrong; register-spill probe: 256 of 256).
Seen on Arc A-series GPUs under the Linux xe kernel driver, not under i915. No libvmaf SYCL
extractor uses scratch memory, so its scores are not affected. See
docs/backends/sycl/overview.md (ADR-1395).
```

The device is still used and every libvmaf extractor gives correct scores on
it. The warning stays because it describes the device: a kernel from outside
libvmaf, or one added without the audit in the
[developer notes](developer-notes.md#scratch-memory-writing-and-checking-kernels),
would be affected.

The probes cost 3 to 5 ms with a warm compute-runtime kernel cache and about
0.4 s on the first run. `VMAF_SYCL_SCRATCH_SELFTEST=0` skips them.

!!! note
    A build older than 2026-10-01 names, in this message, the extractors that
    still used scratch memory. On such a build compute those features with
    their CPU extractors (`--feature adm` rather than `adm_sycl`, or
    `--no_sycl`), or run the GPU under `i915`.

### Checking a device

`test_sycl_kernel_scratch` (in `--suite sycl`) builds every kernel libvmaf
registers for the default GPU and fails on each one that uses scratch memory:

```bash
ONEAPI_DEVICE_SELECTOR=level_zero:0 \
  python3 scripts/ci/run_meson_test.py -- -C build -v test_sycl_kernel_scratch
```

The ratchet list that used to exempt known kernels is empty and stays empty;
`test_sycl_kernel_source_contract.py` rejects a new entry without a device. The
test skips without a GPU, so CI, which has none, does not run it.

It audited 128 kernels on an Arc A380 at the revision of
[ADR-1488](../../adr/1488-psnr-hvs-upstream-mask-product.md) (2026-10-02) and
found none. On an Arc B580 and an Arc Pro B60 (Xe2, xe driver) it audited 127
kernels and found none (2026-10-03). Both Xe2 cards return correct values from
the two probes, so they log no warning. Until 2026-10-03 the term kernel of
`float_adm_sycl` used 128 bytes of scratch memory there
([ADR-1501](../../adr/1501-sycl-float-adm-terms-large-grf-xe2.md)).

### VIF kernels run at SIMD-16

`vif_sycl` runs its kernels at sub-group size 16 on every device
([ADR-1830](../../adr/1830-sycl-vif-simd16-only.md)); a SYCL device without
SIMD-16 sub-groups (no Intel GPU) gets `-ENOTSUP` from it. The SIMD-32 kernels
and the `VMAF_SYCL_VIF_SUBGROUP_SIZE` variable that forced them were removed on
2026-10-05: they needed a 256-entry register file that Xe-LP integrated GPUs
(UHD 770 and similar) do not have, spilled to scratch memory there, and were
never faster than SIMD-16 on an Arc A380 (21.19 against 21.20 ms per frame at
3840x2160, 33.85 against 23.50 ms with `vif_fused=true`). Setting the variable
now has no effect.

## Profiling

- Intel VTune (`vtune-gui`) with the GPU Compute analysis type for kernel
  occupancy and EU utilization.
- `onetrace` from the [pti-gpu](https://github.com/intel/pti-gpu) project for
  Level Zero API-level tracing.
- `VMAF_SYCL_PROFILE=1` gives the queues `enable_profiling`; with
  `VMAF_SYCL_NO_GRAPH=1` graph extractors submit directly, so every kernel has
  its own event
  ([Research-1369](../../research/1369-sycl-shared-planes-light-twins.md)
  describes the event-timing build).
- For end-to-end wall-time comparisons against the CUDA and CPU paths, use
  `make test-netflix-golden`, which records per-backend scores and timings.
- Programmatic profiling through `VmafSyclState.enable_profiling`; see
  [api/gpu.md](../../api/gpu.md#profiling-helpers) for the queue-event query
  API.

## Known gaps

Only open items are listed. A closed gap is recorded in [History](history.md)
and in [`docs/state.md`](../../state.md), which is the authority on status.

| Gap | Effect | Tracked as |
| --- | --- | --- |
| No CI lane executes a SYCL kernel | Device parity is checked by hand on Arc hardware; the isolated Arc workflow has no registered runner | `T-SYCL-NO-CI-KERNEL-EXECUTION-2026-09-04` |
| The six row kernels moved to sub-group 16 are verified on Xe2 and Arc A-series only | Scratch freedom and exactness at 16 are unmeasured on Xe-LP and Xe-LPG integrated GPUs | `T-SYCL-ROW-KERNELS-SG16-OTHER-DEVICES-2026-10-02` |
| Exact twins cost throughput | Scores are correct; `float_ssim`, `float_ms_ssim`, `ssim`, `ssimulacra2`, `ciede`, `float_vif`, `float_moment` and `psnr_hvs` take longer than before exactness, and `float_ms_ssim` at 4K is slower than a 16-thread CPU run (`--backend cpu` is faster there) | `T-SYCL-FLOAT-SSIM-RASTER-SUM-THROUGHPUT-2026-10-02`, `T-SYCL-FLOAT-MS-SSIM-RASTER-SUM-THROUGHPUT-2026-10-02`, `T-SYCL-SSIM-EXACT-THROUGHPUT-2026-10-02`, `T-SYCL-SSIMULACRA2-EXACT-THROUGHPUT-2026-10-02`, `T-SYCL-CIEDE-EXACT-THROUGHPUT-2026-10-01`, `T-SYCL-FLOAT-VIF-EXACT-THROUGHPUT-2026-10-01`, `T-SYCL-FLOAT-MOMENT-PER-PIXEL-ATOMICS-2026-10-02`, `T-SYCL-HIP-PSNR-HVS-EXACT-SUM-THROUGHPUT-2026-10-01` (RC8) |
| `psnr_hvs_sycl` is slow on Xe-LP | About 49 ms of kernel time per 3840x2160 frame on a UHD 770, against about 7 ms for 16 CPU threads | `T-SYCL-PSNR-HVS-XE-LP-THROUGHPUT-2026-09-29` (RC8) |
| `float_motion_sycl` lacks three CPU options | `motion_add_scale1`, `motion_add_uv` and `motion_filter_size` are not declared, so a request that sets one runs `float_motion` on the CPU ([ADR-1183](../../adr/1183-model-options-gate-gpu-twin-selection.md)). `motion_sycl` has its own `motion_add_uv` | none (see [the motion page](../../metrics/motion.md)) |
| AdaptiveCpp builds are JIT only | `sycl_acpp_targets` defaults to `generic`, AdaptiveCpp's portable JIT path; ahead-of-time `intel_gpu_<arch>` strings (AdaptiveCpp 23.10 and later) are not wired | follow-up task, no state row |
| HIP and CUDA devices through SYCL need a custom DPC++ | The shipped oneAPI compiler includes the Level Zero, OpenCL CPU and CUDA plugins, not HIP | build limitation |
| dmabuf import is Linux-only | `vmaf_sycl_dmabuf_import` and `vmaf_sycl_import_va_surface` return `-ENOSYS` on Windows, where callers use the D3D11 staging path; DMA-BUF is a Linux kernel interface | by design (`T-SYCL-DMABUF-IMPORT-WIN32-ENOSYS`) |
| Windows MSVC device link is not measured on a GPU | The one explicit device link of the MSVC build gets the strict FP flags, but that path has not run on a GPU ([ADR-1364](../../adr/1364-windows-sycl-msvc-device-link.md)) | none |

See [metrics/features.md](../../metrics/features.md) for the per-extractor
coverage matrix and [api/gpu.md](../../api/gpu.md#sycl) for the programmatic
surface.

## References

- [SYCL 2020
  Specification](https://registry.khronos.org/SYCL/specs/sycl-2020/html/sycl-2020.html)
- [Intel oneAPI DPC++ Compiler](https://github.com/intel/llvm)
- [Level Zero Specification](https://spec.oneapi.io/level-zero/latest/)
- [Intel oneAPI Programming
  Guide](https://www.intel.com/content/www/us/en/docs/oneapi/programming-guide/current/overview.html)

## Licensing of the SYCL kernels (ADR-1250)

As with the other backends, a SYCL kernel implementing an upstream Netflix
metric keeps that code's terms and copyright notice, while fork-original SYCL
code is EUPL-1.2. Four files in `core/src/sycl/` additionally carry an outside
contributor's work and stay on their current terms until that contributor
agrees to a change. See [ADR-1250](../../adr/1250-eupl-fork-relicense.md).

## Former section names

ADRs and research digests link to these headings; each points to the section
that now holds its content.

### Numerical tolerance vs the CPU scalar path

Now under [numerical agreement with the CPU](#numerical-agreement-with-the-cpu).
