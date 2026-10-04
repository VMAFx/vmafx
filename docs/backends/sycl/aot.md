# SYCL AOT targets

The default SYCL build compiles device code ahead of time (AOT) for 19 Intel
GPU micro-architectures, so the first launch of a short run does not pay a JIT
compile. This page lists the targets, says what the build needs and checks, and
explains the sub-group sizes the targets accept. Back to the
[SYCL overview](overview.md).

## Why AOT is the default

Without AOT, `icpx -fsycl` emits portable SPIR-V that the Level Zero / IGC
runtime compiles to native code on first use. That compile typically takes
several seconds and is paid again after a driver upgrade or a reinstall. For a
short VMAF run (a handful of frames) the JIT cost dominates the wall time. The
HIP analogue (`hip_gfx_targets`) learned the same lesson in PR #1329, and
making AOT the default removes the silent first-run penalty
([ADR-0568](../../adr/0568-sycl-icpx-aot-targets-default.md)).

## Default target list

<!-- markdownlint-capture -->
<!-- markdownlint-disable MD013 -->
The default `sycl_icpx_aot_targets` value covers these Intel GPU
micro-architectures:

| Target | Silicon |
| --- | --- |
| `dg2-g10` | Arc A770 / A750 (DG2-G10) |
| `dg2-g11` | Arc A380 / Arc Pro A30M (DG2-G11) |
| `acm-g10` | Arc A770M / A730M (ACM-G10) — mobile |
| `acm-g11` | Arc A550M / A370M (ACM-G11) — mobile |
| `acm-g12` | Arc A350M (ACM-G12) — mobile thin |
| `tgllp` | Tiger Lake integrated (TGL-LP) |
| `adl-s` | Alder Lake-S integrated (desktop) |
| `adl-p` | Alder Lake-P integrated (mobile 28W) |
| `adl-n` | Alder Lake-N integrated (N-series) |
| `rpl-s` | Raptor Lake-S integrated (desktop) |
| `rpl-p` | Raptor Lake-P integrated (mobile) |
| `mtl-h` | Meteor Lake-H integrated (high-performance mobile) |
| `mtl-u` | Meteor Lake-U integrated (ultra-mobile) |
| `arl-h` | Arrow Lake-H integrated (high-performance mobile) |
| `arl-s` | Arrow Lake-S integrated (desktop) |
| `arl-u` | Arrow Lake-U integrated (ultra-mobile) |
| `lnl-m` | Lunar Lake-M integrated (requires icpx 2025.0+) |
| `bmg-g21` | Battlemage G21 dGPU (requires icpx 2025.1+) |
| `bmg-g31` | Battlemage G31 dGPU (requires icpx 2025.1+) |
<!-- markdownlint-restore -->

The fat binary also embeds a SPIR-V JIT fallback (`spir64`) for any device not
in the list, so an unlisted or future device still works; it just pays the
cold-start cost.

The build forwards `-device <list>` with `-Xsycl-target-backend=spir64_gen`.
The target qualifier is required: an unqualified `-Xs` also reaches the
portable `spir64` target, and oneAPI then reports the device selector as
unused.

## What the build needs and checks

AOT needs Intel's `ocloc` offline compiler on `PATH`. The Linux oneAPI compiler
does not ship it. Either install it with `scripts/ci/install-intel-ocloc.sh`
(see [the oneAPI install
guide](../../development/oneapi-install.md#the-ocloc-offline-compiler))
or configure with `-Dsycl_icpx_aot_targets=''`. Without `ocloc`, `meson setup`
stops with an error that says so.

Each SYCL source file is compiled to native code for every listed target at
compile time (`-fno-sycl-rdc`). Every binary that links a SYCL object carries
the images: `libvmaf.so`, static consumers of `libvmaf.a`, and the test
executables. With `compress_device_code` (on by default) every image is stored
zstd-compressed at level 22 (`--offload-compress
--offload-compression-level=22`): the native images on the compile line, the
SPIR-V fallback on the link that generates it. The build checks
`libvmaf.so` for a device image stored raw
([`compress_device_code`](../../development/build-flags.md#compress_device_code)).

After linking `libvmaf.so` on Linux, the build runs
`core/src/sycl/check_aot_image.py`. It fails the build unless the library holds
a native image for every listed target. `ocloc` writes the images of one source
file in one of two forms:

- For two or more listed targets, even when some share a GPU IP version, a fat
  binary: an `ar` archive with one member per target.
- For exactly one target, as in `-Dsycl_icpx_aot_targets=dg2-g11`, a bare
  native binary with no archive.

The check accepts both. It reads the GPU IP version of a bare binary from the
`IntelGT` product-config note in its `.note.intelgt.compat` section, which is
the value `ocloc ids <target>` prints for that target, and it rejects a binary
built for a GPU IP version that no listed target uses. The test
`core/test/test_sycl_aot_image_check.py` covers both forms without a GPU or
oneAPI.

To check a library yourself:

```bash
clang-offload-bundler --list --type=o --input=build/src/libvmaf.so
```

The output lists one bundle per kind, among them:

```text
sycl-spir64_gen
sycl-spir64
```

`sycl-spir64_gen` holds the native images and `sycl-spir64` the SPIR-V
fallback.

Windows MSVC builds work differently, because Meson links them with
`link.exe`, which cannot handle device code. The translation units keep
relocatable device code, and one `icpx -fsycl -fsycl-link` step generates the
native images for all of them and the object that registers them
([ADR-1364](../../adr/1364-windows-sycl-msvc-device-link.md),
[SYCL on Windows](windows.md)). The image check above does not run there.

### Cost and benefit

Measured with the default 19 targets on a 22-thread host at `-j6`:

| Quantity | SPIR-V only | Default AOT |
| --- | --- | --- |
| Clean build time | 82 s | 162 s |
| `libvmaf.so` size | 4.6 MB | 7.7 MB |
| First frame of the default model, cold compiler cache, Arc B580 | 524 ms | 201 ms |
| First frame of the default model, cold compiler cache, UHD 770 | 609 ms | 245 ms |

Per-frame speed and scores are unchanged: with a warm cache the SPIR-V build
starts as fast, and AOT and JIT scores are bit-identical.

## Sub-group sizes and the AOT targets (ADR-1468)

A kernel can require a sub-group size (how many work-items one hardware
thread runs as SIMD lanes). Not every GPU generation offers every size, and a
kernel that requires one a target lacks does not compile for it. Because all
kernels of a source file are compiled together, that one kernel fails the
file, and with the default target list the build:

```text
[lnl-m] error: in kernel '...': Kernel compiled with required subgroup size 8,
which is unsupported on this platform
```

What the targets of the default list accept, measured with `ocloc`:

| Targets | 8 | 16 | 32 |
| --- | --- | --- | --- |
| `tgllp`, `adl-*`, `rpl-*` (Xe-LP) | yes | yes | yes |
| `dg2-*`, `acm-*` (Arc A-series) | yes | yes | yes |
| `mtl-*`, `arl-*` (Xe-LPG) | yes | yes | yes |
| `lnl-m`, `bmg-g21`, `bmg-g31` (Xe2: Lunar Lake, Arc B-series) | no | yes | yes |

So a kernel of this backend requires 16 or 32, nothing else
([ADR-1468](../../adr/1468-sycl-sub-group-sizes-every-aot-target.md)).
`sycl_compat.h` enforces it at compile time: `VMAF_SYCL_REQD_SG_SIZE(8)` or
`VmafSyclKernelShape<8, ...>` is a compile error in every configuration,
including a build for a single device or with an empty target list.

Every kernel requires 16 or 32 since 2026-10-02. The six that used to require 8
now require 16; their scores are unchanged on an Arc A380. Whether they are
scratch-free and exact at 16 on integrated GPUs has not been measured
(`T-SYCL-ROW-KERNELS-SG16-OTHER-DEVICES-2026-10-02` in
[`state.md`](../../state.md)). The measurements and the failure the old sizes
caused are in [History](history.md#sub-group-size-8-removed-2026-10-02).

A build configured for one device does not compile for the others, so two
tests stand in for the default build:

```bash
# No compiler, no device (suite fast): every size the sources require is one
# every default target accepts.
python3 core/test/test_sycl_sub_group_size_contract.py

# Needs ocloc, no device (suite sycl-aot): compiles every SYCL source file of
# this build for all 19 default targets, whatever list the build was
# configured with. Minutes; VMAF_SYCL_AOT_JOBS sets the parallel compiles.
meson test -C build --suite sycl-aot
```

!!! note
    Run the second test before pushing a change to a SYCL kernel from a build
    that is not configured with the default list.

## Adjusting the target list

Override the default at configure time with `-Dsycl_icpx_aot_targets=`:

```bash
# Single-target fleet (Arc A380 only) — smallest binary:
meson setup build core -Denable_sycl=true -Dsycl_icpx_aot_targets=dg2-g11

# JIT-only (SPIR-V, no AOT blobs) — smallest binary, first-run penalty:
meson setup build core -Denable_sycl=true -Dsycl_icpx_aot_targets=''

# Dev machine with Arc A380 + Meteor Lake iGPU:
meson setup build core -Denable_sycl=true -Dsycl_icpx_aot_targets='dg2-g11,mtl-h'
```

The option is ignored when `sycl_compiler` is not `icpx`, so AdaptiveCpp
builds are unaffected.

## Known toolchain version constraints

The target names are resolved by `ocloc`, so what counts is the ocloc
(compute-runtime) release, not the icpx version. The release pinned as
`INTEL_NEO_VERSION` in `build-config.env` knows every default target.

- `lnl-m` (Lunar Lake) and `bmg-g21`, `bmg-g31` (Battlemage) are the targets
  an older ocloc is most likely not to know; `ocloc ids <target>` tells you.
- An ocloc that does not know a listed target fails the compile, and a target
  that goes missing from the images fails the build-time image check. Narrow
  the list to what your ocloc supports, or set `sycl_icpx_aot_targets=''` to
  disable AOT.

## AdaptiveCpp (acpp) side

The `sycl_acpp_targets` option currently defaults to `"generic"`, which is
AdaptiveCpp's portable SPIR-V / SSCP JIT path — the same cold-start trap as
the old icpx default. AOT under AdaptiveCpp requires `intel_gpu_<arch>` target
strings (supported in AdaptiveCpp 23.10+); that broadening is tracked as a
follow-up task (see [Known gaps](overview.md#known-gaps)).
