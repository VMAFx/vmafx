<!-- markdownlint-disable MD013 MD060 -->
# Bundling libvmaf_sycl for Self-Contained Deployment

Bundle the Intel oneAPI runtime libraries listed below next to the binary so
FFmpeg with `libvmaf_sycl` runs on a machine without oneAPI installed.

## Problem

Without the runtime, SYCL fails on a system that has no Intel oneAPI with:

```text
SYCL exception: No device of requested type available
```

Even though the Intel iGPU is present and VA-API works, the SYCL runtime
libraries are missing.

## Required Runtime Libraries

### Intel oneAPI Runtime (from `/opt/intel/oneapi/compiler/latest/lib/`)

`libumf.so.1` lives under `/opt/intel/oneapi/umf/latest/lib/`, and
`libze_loader.so` comes from the Level Zero loader package, not the compiler
directory. The oneAPI 2025 and later runtimes load Unified Runtime (UR)
adapters; the older `libpi_level_zero.so` plugin no longer exists.

| Library | Purpose |
|---------|---------|
| `libsycl.so` | SYCL runtime |
| `libze_loader.so` | Level Zero loader (GPU compute API) |
| `libsvml.so` | Intel short vector math library |
| `libirc.so` | Intel compiler runtime |
| `libur_loader.so.0` | Unified Runtime loader that `libsycl.so` links against |
| `libur_adapter_level_zero.so.0` (and `libur_adapter_level_zero_v2.so.0`) | Unified Runtime adapter for the Level Zero backend (runtime-loaded) |
| `libumf.so.1` | Unified Memory Framework, from the `intel-oneapi-umf` package |

### FFmpeg Integration

| Library | Purpose |
|---------|---------|
| `libvpl.so.2` | Intel VPL dispatcher (QSV interop for zero-copy decode→VMAF) |

### System Libraries (may be missing in minimal/container environments)

| Library | Purpose |
|---------|---------|
| `libdrm.so.2` | DRM access (required by Level Zero) |
| `libva.so` | VA-API (required for DMA-BUF zero-copy path) |
| `libva-drm.so` | VA-API DRM backend |

### Transitive Dependencies

Check the build machine for additional transitive deps:

```bash
ldd /opt/intel/oneapi/compiler/latest/lib/libze_loader.so
ldd /opt/intel/oneapi/compiler/latest/lib/libsycl.so
```

Any non-standard deps (e.g. `libspdlog`, `libfmt`) also need bundling.

## Cannot Be Bundled (must exist on target)

- `i915` or `xe` kernel module (Intel GPU driver); it must be loaded on the
  target system
- `/dev/dri/render*` device node access
- Standard glibc (`libc.so`, `libm.so`, `libpthread.so`)
- The Intel GPU compute runtime (user-space driver) matching the hardware

## Bundling Steps

1. **Copy the `.so` files** into the FFmpeg binary directory (or a `lib/`
   subdirectory).

2. **Set RPATH at link time** so the binary finds them without
   `LD_LIBRARY_PATH`:

   ```bash
   # Same directory as binary
   -Wl,-rpath,'$ORIGIN'
   # Or a lib/ subdirectory
   -Wl,-rpath,'$ORIGIN/lib'
   ```

3. **Alternatively**, have users set `LD_LIBRARY_PATH` at runtime:

   ```bash
   export LD_LIBRARY_PATH=/path/to/bundled/libs:$LD_LIBRARY_PATH
   ```

## Verifying

Check which libraries are missing at runtime:

```bash
ldd /path/to/ffmpeg | grep -E 'sycl|ze_loader|svml|irc|ur_|umf|vpl|drm|libva'
```

Any "not found" entries need to be bundled.

## Device code and the DMA-BUF path

- The default build embeds ahead-of-time device code for the 19 targets of
  `sycl_icpx_aot_targets` plus a SPIR-V image for JIT, so no separate device
  code files ship with the binary (see the
  [SYCL overview](overview.md#aot-targets-default-adr-0568)).
- `libva` and `libva-drm` are only needed for the DMA-BUF zero-copy path;
  otherwise the CPU upload path is used.

## Toolchain versions and runtime knobs

The release pins live in `build-config.env`; read them there instead of
trusting a copy in prose:

| Component | Pin in `build-config.env` | Current value |
|-----------|---------------------------|---------------|
| Intel oneAPI DPC++ (Linux) | `ONEAPI_VERSION`, package `intel-oneapi-compiler-dpcpp-cpp-<version>` | 2026.1 (apt build 2026.1.1-325) |
| Intel oneAPI (Windows CI) | `ONEAPI_WINDOWS_VERSION` | 2025.3.0.372 |
| Level Zero loader | `LEVEL_ZERO_VERSION` | 1.34.0 |
| Intel Compute Runtime (NEO) | `INTEL_NEO_VERSION` | 26.35.39758.10 |

- The SYCL 2020 Rev 11 specification is the language level.
- oneAPI 2025.0 was an ABI-breaking release; rebuild any object files or
  shared libraries built against an earlier toolchain.
- CI pins the minor meta-package (`-<ONEAPI_VERSION>`) and the exact apt build
  rather than the unversioned `latest`, to prevent silent bumps.

### Level Zero v2 adapter (Xe2 / Battlemage default)

The refactored Unified Runtime Level Zero v2 adapter is the default on Arc
B-Series and other Xe2-based GPUs. On Arc A-Series, DG2 and Flex you may see
a performance regression under its immediate command lists. Set this before
running `ffmpeg` or any libvmaf-linked binary:

```bash
export UR_L0_USE_IMMEDIATE_COMMANDLISTS=0
```

Xe2 / Battlemage users should keep the default (immediate command lists on).

One queue is exempt. libvmaf creates its primary SYCL queue, the one that
imports QSV / VA-API surfaces for the `libvmaf_sycl` zero-copy path, with
immediate command lists whatever this variable says
([ADR-3359](../../adr/3359-sycl-primary-queue-immediate-cmdlist.md)). With batched
command lists an Arc A380 (compute-runtime 26.35) silently dropped the
per-frame surface import from a random frame on, so zero-copy scores were
computed on stale frames and differed from run to run. The copy queue, the
graph queue and the per-extractor queues still follow the variable. The
property is a DPC++ extension: another SYCL implementation builds without it,
and a backend other than Level Zero ignores it.
