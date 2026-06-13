<!-- markdownlint-disable MD013 MD060 -->
# Bundling libvmaf_sycl for Self-Contained Deployment

## Problem

When deploying FFmpeg with `libvmaf_sycl` on a system without Intel oneAPI installed, SYCL fails with:

```text
SYCL exception: No device of requested type available
```

Even though the Intel iGPU is present and VA-API works, the SYCL runtime libraries are missing.

## Required Runtime Libraries

### Intel oneAPI Runtime (from `/opt/intel/oneapi/compiler/latest/lib/`)

| Library | Purpose |
|---------|---------|
| `libsycl.so` | SYCL runtime |
| `libze_loader.so` | Level Zero loader (GPU compute API) |
| `libsvml.so` | Intel short vector math library |
| `libirc.so` | Intel compiler runtime |
| `libpi_level_zero.so` | SYCL plugin for Level Zero backend (runtime-loaded) |

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

- `i915` or `xe` kernel module (Intel GPU driver)
- `/dev/dri/render*` device node access
- Standard glibc (`libc.so`, `libm.so`, `libpthread.so`)

## Bundling Steps

1. **Copy the `.so` files** into the FFmpeg binary directory (or a `lib/` subdirectory).

2. **Set RPATH at link time** so the binary finds them without `LD_LIBRARY_PATH`:

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
ldd /path/to/ffmpeg | grep -E 'sycl|ze_loader|svml|irc|pi_level_zero|vpl|drm|libva'
```

Any "not found" entries need to be bundled.

## Notes

- SPIR-V device code is embedded in the binary at link time via `clang-offload-wrapper`, so no extra device code files are needed.
- The kernel driver (`i915` or `xe`) must still be loaded on the target system — it cannot be bundled.
- `libva` and `libva-drm` are only needed if using the DMA-BUF zero-copy path; otherwise the CPU upload path is used.

## Toolchain versions and runtime knobs

Built and validated against:

- **Intel oneAPI DPC++ 2025.3** (package `intel-oneapi-compiler-dpcpp-cpp-2025.3`, icpx 2025.3.x).
- **Level Zero loader v1.28.0** (`oneapi-src/level-zero`, tag `v1.28.0`, Feb 2026).
- **Intel Compute Runtime 26.09+** on target systems with Xe2 / Battlemage.
- **SYCL 2020 Rev 11** spec.

oneAPI 2025.0 was an ABI-breaking release; any object files / shared libraries built
against earlier toolchains must be rebuilt. CI pins the minor meta-package
`-2025.3` rather than the unversioned `latest` to prevent silent bumps.

### Level Zero v2 adapter (enabled by default on Xe2 / Battlemage)

oneAPI 2025.3 enables the refactored Unified Runtime L0 v2 adapter by default on
Arc B-Series and other Xe2-based GPUs. On **Arc A-Series / DG2 / Flex** you may
observe a perf regression under L0 v2's immediate command lists; the escape hatch is:

```bash
export UR_L0_USE_IMMEDIATE_COMMANDLISTS=0
```

Set before running `ffmpeg` or any libvmaf-linked binary. Xe2 / Battlemage users
should leave the default (immediate command lists on).
