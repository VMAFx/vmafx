<!-- markdownlint-disable MD013 MD060 -->
# ARM NEON / SVE2 backend

On aarch64, libvmaf runs ARMv8-A NEON kernels by default and upgrades to
ARMv9-A SVE2 at runtime on CPUs that advertise it. No build option is needed:
NEON is always built when the compiler targets aarch64.

## Overview

- Kernels live under
  [`core/src/feature/arm64/`](../../../core/src/feature/arm64/) and are
  dispatched at runtime through `vmaf_get_cpu_flags()`.
- There is no `-Denable_neon` switch. The only build switch that affects NEON
  code generation is the global `enable_asm`.
- SVE2 is purely additive. It covers SSIMULACRA 2 and `float_moment` today
  ([ADR-0213](../../adr/0213-ssimulacra2-sve2.md),
  [ADR-0584](../../adr/0584-moment-sve2-port.md)). Further ports follow the
  same pattern.

## Build

Run from the repository root. NEON sources compile automatically on aarch64:

```bash
meson setup build core
ninja -C build
```

`-Denable_asm=false` disables every SIMD path (NEON included) and falls back
to scalar C. See [build flags](../../development/build-flags.md).

SVE2 sister translation units compile alongside the NEON ones when the build
probe `cc.compiles(... -march=armv9-a+sve2)` succeeds. The runtime dispatch
then picks SVE2 if `getauxval(AT_HWCAP2) & HWCAP2_SVE2` is set; otherwise the
binary keeps the NEON entries.

## Runtime control

`--cpumask` takes the ISA bits to mask out, as `vmaf_get_cpu_flags()` returns
them (`core/src/arm/cpu.h`: NEON is 1, SVE2 is 2). `0`, the default, leaves
every ISA enabled.

```bash
vmaf --cpumask 3 ...        # mask out NEON (1) and SVE2 (2): scalar only
vmaf --cpumask 2 ...        # keep NEON, drop SVE2
```

There is no per-feature NEON disable flag: an extractor with a NEON kernel
uses it whenever `--cpumask` allows. To bisect a suspected NEON regression,
use `--cpumask 3` to drop to scalar across all extractors at once, then
compare individual `--feature` runs.

## Per-feature coverage

The table tracks which extractors have a NEON kernel; it matches the
`Backends` column of [feature metrics](../../metrics/features.md). Longer
explanations are in the notes below the table.

| Feature | NEON | SVE2 | Notes |
|---------|------|------|-------|
| `vif` | yes | no | matches the AVX2 path bit for bit |
| `adm` | yes | no | scalar bits at every stage, see note 6 |
| `motion` | yes | no | fixed-point legacy `motion` |
| `motion_v2` | yes | no | pipelined fused-blur variant |
| `float_moment` | yes | yes | scalar bits, see note 1 |
| `float_motion` | yes | no | float-pipeline twin |
| `float_adm` | yes | no | float-pipeline twin |
| `float_psnr` | yes | no | per-plane float PSNR |
| `ciede` | yes | no | YUV to CIELAB delta E |
| `psnr` | yes | no | fixed-point per plane |
| `psnr_hvs` | yes | no | scalar bits, see note 2 |
| `ssim` / `float_ssim` | yes | no | shared decimate kernel |
| `float_ms_ssim` | yes | no | 9-tap 9/7 wavelet decimate via `ms_ssim_decimate_neon` |
| `ssimulacra2` | yes | yes | bit-identical to scalar, see note 3 |
| `cambi` | yes | no | see note 4 |
| `speed_chroma` / `speed_temporal` | yes | no | see note 5 |

1. `float_moment`: the NEON and SVE2 kernels return the scalar function's
   bits on every input and every SVE vector length; values are added in
   raster order
   ([ADR-1500](../../adr/1500-arm-float-moment-scalar-order.md)).
2. `psnr_hvs`: the scalar function's bits on every plane and frame
   ([ADR-0160](../../adr/0160-psnr-hvs-neon-bitexact.md); see
   [`psnr_hvs`](../../metrics/psnr-hvs.md#cpu-instruction-sets)).
3. `ssimulacra2`: NEON and SVE2 produce byte-equal output
   ([ADR-0161](../../adr/0161-ssimulacra2-simd-bitexact.md),
   [ADR-0162](../../adr/0162-ssimulacra2-iir-blur-simd.md),
   [ADR-0163](../../adr/0163-ssimulacra2-ptlr-simd.md),
   [ADR-0213](../../adr/0213-ssimulacra2-sve2.md)).
4. `cambi`: every stage except the mask row and the mode filter, which the
   compilers already vectorise
   ([CAMBI CPU SIMD paths](../../metrics/cambi.md#cpu-simd-paths)).
5. `speed_chroma` / `speed_temporal`: the covariance row kernel
   (`speed_cov_row_neon`) uses one lane per covariance sum and is
   bit-identical to scalar
   ([ADR-1459](../../adr/1459-speed-cov-kernel-exact.md)). The anti-alias
   filter stays scalar C on aarch64 and is evaluated only at the samples the
   16x decimation keeps (`vif_filter1d_dec16_s()`, Netflix/vmaf `76ea5f03`;
   x86 runs the same function with an AVX2 vertical pass, `9cb9479f`).
   Upstream's NEON covariance kernel (`15297286`) is not used: it splits one
   sum over eight lanes and does not return the scalar's bits.
6. `adm` (integer): NEON runs the 8-bit scale-0 DWT, the DWT of scales 1 to
   3, the decouple of every scale and the contrast masking of every scale
   (Netflix/vmaf `9e48141b`, `8bc5a5c6a`, `b41d2340a`). Each returns the
   scalar kernel's bits. The scale 1-3 decouple vectorises an enhancement
   gain limit of 1 (the `neg` models) and runs the scalar kernel at other
   limits. The contrast masking runs the scalar kernel when the CSF weights
   need the wide fixed-point range (`adm_csf_mode` 1, Barten), as on x86.
   The masking threshold keeps the scale-0 centre tap in int32 and forms the
   excess in int64 ([ADR-1402](../../adr/1402-adm-cm-centre-tap-int32.md)),
   where upstream's NEON narrows the tap to int16.

## Bit-exactness

NEON outputs are byte-identical to the scalar C reference for the features
below. Other extractors are numerically equivalent to their scalar twins
within `places=4` of the snapshot tolerance but carry no byte-identity
contract. The Netflix golden CPU gate (`make test-netflix-golden`) is the
cross-architecture correctness check.

| Feature | Contract | Tests | How it holds |
|---------|----------|-------|--------------|
| `psnr_hvs` | ADR-0160 | `test_psnr_hvs_dispatch_invariance`, `test_psnr_hvs_neon` | the first scores the extractor with NEON and with every flag masked and compares the four outputs bit for bit; the second compares the integer DCT alone. The masking threshold uses the scalar's `float` product ([`psnr_hvs`](../../metrics/psnr-hvs.md#cpu-instruction-sets)) |
| `ssimulacra2` | ADR-0161, 0162, 0163, 0213 | cross-file `build-aux/aarch64-linux-gnu-sve2.ini` under `qemu-aarch64-static -cpu max` | cross-host determinism via `vmaf_ss2_cbrtf` and the sRGB-EOTF LUT; the SVE2 TU is locked to a fixed 4-lane predicate (`svwhilelt_b32(0, 4)`) so its arithmetic order matches NEON at any runtime vector length |
| `float_moment` | ADR-1500 | `core/test/test_moment_simd.c` (asserts `==`) | each sample, or its float square, is added into one `double` in raster order, as the scalar loop and the x86 kernels do, so a 16-bit frame whose sum of squares passes $2^{53}$ units gets the scalar's bits |
| `ms_ssim_decimate` | ADR-0125 | decimate tests | per-lane `vfmaq_n_f32` with broadcast coefficients matches the scalar `fmaf` chain exactly |
| `speed_chroma` / `speed_temporal` | ADR-1459 | `core/test/test_speed_simd.c` | every covariance sum of the NEON row kernel has the bits of `compute_cov_kernel_scalar()`; run under `qemu-aarch64` with GCC and with clang |
| `adm` (integer) | ADR-1402, ADR-1413, ADR-1167 | `core/test/test_integer_adm_simd.c`, `core/test/test_adm_dwt2_neon.c` | every vector kernel against the scalar kernels of `integer_adm_kernels.h`: contrast masking of every scale on fills that reach negative thresholds and saturating excesses, decouple at gain limits 1 to 100, DWT of every scale on full-range samples with a guard band; integer lanes widened where the scalar widens, rows folded once (ADR-1167); run under `qemu-aarch64` with GCC and with clang |

Notes on running these:

- The `float_moment` test runs under `qemu-aarch64` with `-cpu max,sve=off`
  and with `sve128=on`, `sve256=on`, `sve512=on` and
  `sve2048=on,sve-default-vector-length=256`, which covers NEON and each SVE
  vector length.
- The `speed` kernel's speed on hardware is not measured: it has only run
  under emulation.

### GCC and clang builds return the same scores

Every C and C++ file of the library, the tools and the tests is built without
FP contraction
([ADR-1461](../../adr/1461-strict-fp-every-translation-unit.md)). A fused
multiply-add is a baseline aarch64 instruction, and a compiler may use it for
`a * b + c` on its own, which rounds once where the source rounds twice.
Kernels that want a fused multiply-add ask for it in the source and still get
it.

Measured result: the aarch64 clang and GCC builds agree on 3350 of 3355 values
(five `ciede2000` values differ by up to 1.1e-12, as they do between the two
compilers on x86-64), and the aarch64 GCC build agrees with an x86-64 GCC
build on `psnr_hvs`
([ADR-1488](../../adr/1488-psnr-hvs-upstream-mask-product.md)). x86-64 builds
did
not change.

## CI matrix

| Job | Runner | What it checks |
|-----|--------|----------------|
| `Ubuntu ARM clang` ([`libvmaf-build-matrix.yml`][libvmaf-build-matrix]) | `ubuntu-26.04-arm`, clang | the full unit-test and tox suite on real aarch64 hardware, not qemu |
| `Windows ARM64 MSVC` (same workflow) | `windows-11-vs2026-arm` | NEON tree built natively with the ARM64-hosted MSVC toolset, meson `fast` suite ([ADR-1260](../../adr/1260-windows-arm64-cpu-lane.md)); advisory |

[libvmaf-build-matrix]: ../../../.github/workflows/libvmaf-build-matrix.yml

`make test-netflix-golden` runs on aarch64 in the same matrix and must stay
green (see the Netflix golden-data gate in
[`docs/principles.md`](../../principles.md#31-netflix-golden-data-gate)).

On the Windows ARM64 job, MSVC gets `/fp:precise` where GCC and clang get
`-ffp-contract=off` (`vmaf_strict_fp_args` in `core/src/meson.build`). MSVC
has no `<arm_sve.h>` and the SVE2 runtime probe is Linux-only, so a Windows
build dispatches NEON.

## Running the golden gate for aarch64 on an x86 host

Two commands cross-build and run the gate under emulation:

```bash
make test-netflix-golden-arm64                         # aarch64 GCC
make test-netflix-golden-arm64 GOLDEN_ARM64_CC=clang   # aarch64 clang
```

The target cross-builds the `vmaf` tool with the golden gate's build profile
into `core/build-golden-arm64-<compiler>` and runs the same Python assertions
as `make test-netflix-golden` against it. The kernel's `binfmt_misc` handler
passes the aarch64 binary to `qemu-aarch64`, which loads it against the
aarch64 C library under `QEMU_LD_PREFIX` (default `/usr/aarch64-linux-gnu`).

### Prerequisites

The target needs a cross compiler (`aarch64-linux-gnu-gcc`, or clang with lld
and the cross binutils), the aarch64 C library and a registered, enabled
`qemu-aarch64` handler. It stops with one line per missing piece before it
configures anything.

| Distribution | Packages |
|--------------|----------|
| Arch Linux | `aarch64-linux-gnu-gcc`, `aarch64-linux-gnu-glibc`, `qemu-user-static`, `qemu-user-static-binfmt` |
| Debian / Ubuntu | `gcc-aarch64-linux-gnu`, `g++-aarch64-linux-gnu`, `libc6-arm64-cross`, `qemu-user-static`, `binfmt-support` (names not checked on a Debian host) |

### Variables

| Variable | Default | Meaning |
| --- | --- | --- |
| `GOLDEN_ARM64_CC` | `gcc` | `gcc` or `clang` |
| `GOLDEN_ARM64_CROSS_FILE` | `build-aux/aarch64-linux-gnu.ini`, `-clang.ini` for clang | Meson cross file. The clang file names the Arch sysroot layout; another distribution needs its own copy |
| `GOLDEN_ARM64_BUILD_DIR` | `core/build-golden-arm64-$(GOLDEN_ARM64_CC)` | Build directory, one per compiler |
| `QEMU_LD_PREFIX` | `$(AARCH64_SYSROOT)` = `/usr/aarch64-linux-gnu` | Where qemu finds the aarch64 loader and C library |

!!! note
    Emulation checks numbers, not speed. Emulated NEON takes the same code
    path and gives the same bits as hardware, but its timing says nothing
    about a real core. The run takes about two hours on a busy 32-thread host,
    where the native gate takes four minutes.

## Limitations

- No per-feature override: every NEON kernel runs whenever `--cpumask`
  permits.
- Windows on ARM64 is NEON-only: SVE2 is neither built by MSVC nor probed
  outside Linux. Building there is described in
  [Building libvmaf on Windows](../../getting-started/building-on-windows.md#native-msvc-on-windows-arm64).
- No discrete GPU path on aarch64 yet. The CUDA, SYCL and HIP backends
  compile for x86_64 only in the current matrix; on Apple Silicon the Metal
  backend ([metal/index.md](../metal/index.md)) is the aarch64 GPU surface.
  The Vulkan backend was removed in ADR-0726.

## Related

- [Backend overview](../index.md): backend dispatch rules.
- [x86 SIMD twin reference](../x86/avx512.md).
- [Feature metrics](../../metrics/features.md): full per-feature Backends
  column.
- [ADR-0125](../../adr/0125-ms-ssim-decimate-simd.md): MS-SSIM decimate
  bit-exactness contract.
- [ADR-0160](../../adr/0160-psnr-hvs-neon-bitexact.md): `psnr_hvs` NEON
  bit-exactness.
- [ADR-0161](../../adr/0161-ssimulacra2-simd-bitexact.md),
  [ADR-0162](../../adr/0162-ssimulacra2-iir-blur-simd.md),
  [ADR-0163](../../adr/0163-ssimulacra2-ptlr-simd.md),
  [ADR-0213](../../adr/0213-ssimulacra2-sve2.md): SSIMULACRA 2 SIMD ports
  including NEON and SVE2.
- [`build-aux/aarch64-linux-gnu-sve2.ini`](../../../build-aux/aarch64-linux-gnu-sve2.ini):
  qemu cross-file driving `qemu-aarch64-static -cpu max` for SVE2 validation
  runs without native ARMv9 hardware.

## History

### 2026-10-03: float_moment lanes

Until 2026-10-03 the `float_moment` NEON and SVE2 kernels added in lanes and
differed from the scalar function on 16-bit frames whose sum of squares
passes $2^{53}$ units (ADR-1500).

### 2026-10-02: FP contraction (ADR-1461)

Until 2026-10-02 an aarch64 clang build and an aarch64 GCC build returned
different scores. clang contracts by default and GCC contracts C++, each in
different places. The two differed on 680 of 3355 measured values (17
extractors and the default model on the Netflix 576x324 pair at 8 and 10
bits, both 1080p checkerboard pairs and 4 frames of BBB 3840x2160), by up to
6.2e-5 for `speed_chroma` and 3.5e-5 for a `float_vif` scale. A build made
before that date differs from a current one by those amounts.

On 2026-10-02 the golden gate's tests gave 271 passed and 12 skipped against
aarch64 GCC 16.1 and against aarch64 clang 22.1.8 builds, the same counts as
the x86-64 gate.

### 2026-10-02: psnr_hvs masking product (ADR-0160, ADR-1488)

The NEON function returned other bits than the scalar one on about one 8x8
block in twenty because it multiplied the two factors of the masking
threshold as `float` where the scalar reference then multiplied them as
`double`. With the scalar's product an aarch64 build returned the scalar and
x86-64 scores on 708 of 708 measured values (679 before); the defect is
closed in `docs/state.md`.

ADR-1488 then moved every form, the scalar
reference included, to the `double` root of a `float` product, as Netflix's
source writes it; all forms return the same bits.
