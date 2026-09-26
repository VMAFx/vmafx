# Building libvmaf on Windows from source

This guide builds `libvmaf` on Windows with MSYS2 + MinGW-w64. Works from
either `cmd.exe` or PowerShell.

> **Prefer the automated path.** The fork ships a Windows setup script at
> [`scripts/setup/windows.ps1`](../../scripts/setup/windows.ps1) (invoked via
> [install/windows.md](install/windows.md)) that handles toolchain + deps
> through `winget` / Chocolatey. Use this manual guide only when you need a
> custom toolchain or are adapting the build for CI.

This covers only `libvmaf` (the C library). The Python bindings work the
same on all platforms — set up a virtualenv and `pip install python/` from
the repo root.

## Prerequisites

1. Install [MSYS2](https://www.msys2.org/).
2. From an MSYS2 shell, install the MinGW-w64 toolchain and build tools:

   ```bash
   pacman -S --noconfirm --needed \
     mingw-w64-x86_64-nasm \
     mingw-w64-x86_64-gcc \
     mingw-w64-x86_64-meson \
     mingw-w64-x86_64-ninja
   ```

## Build and install

Assumes you want the installed artefacts at `C:/vmaf-install` — change the
`--prefix` if you want a different location.

```bash
cd <vmaf-repo-root>
mkdir C:/vmaf-install
meson setup core core\build \
  --buildtype release \
  --default-library static \
  --prefix C:/vmaf-install
meson install -C core/build
```

## Native MSVC and CUDA

The native MSVC CUDA build is exercised by the `Windows MSVC+CUDA` CI job
in `.github/workflows/libvmaf-build-matrix.yml`. It compiles, links and installs;
the hosted Windows runner does not execute GPU scoring tests.

CUDA needs Visual Studio Build Tools and the Windows SDK even when the host
library uses MinGW. The Meson build first discovers `cl.exe` through `vswhere`.
If that search is empty or fails, it uses `cl.exe` from `PATH`, such as the one
exposed by an x64 Native Tools Command Prompt. Both routes use the selected
compiler path for NVCC's `-ccbin` and MSVC header discovery. If neither route
finds a compiler, configuration reports that Visual Studio Build Tools are required.

The discovery regression runs on POSIX build hosts without a Windows SDK or GPU:

```sh
python3 core/test/test_windows_cuda_compiler_discovery.py -v
```

It also runs in the Meson `fast` suite. It verifies Meson control flow with
stubbed compiler-discovery responses; native compilation and GPU runtime
validation remain separate checks.

## Native MSVC on Windows ARM64

libvmaf builds natively on Windows on ARM64 with the ARM64-hosted MSVC
toolset; the NEON kernels under `core/src/feature/arm64/` are compiled and
used, exactly as on Linux and macOS AArch64. CI exercises this recipe on the
`windows-11-vs2026-arm` runner as the `Windows ARM64 MSVC` job
([ADR-1260](../adr/1260-windows-arm64-cpu-lane.md)).

Prerequisites: Visual Studio 2022 17.4 or later (2026 is what CI uses) with
the "MSVC ARM64/ARM64EC build tools" component, Python 3.11 or later, and
`pip install meson ninja`. No nasm is needed on ARM64.

From an *ARM64 Native Tools Command Prompt*, or after running
`vcvarsall.bat arm64` from the Visual Studio `VC\Auxiliary\Build` directory:

```bat
cd <vmaf-repo-root>
set CFLAGS=/experimental:c11atomics
set CXXFLAGS=/experimental:c11atomics
meson setup core core\build --buildtype release ^
  --prefix %CD%\install ^
  --default-library=static ^
  -Denable_cuda=false -Denable_sycl=false ^
  -Denable_float=true
ninja -C core\build install
python scripts\ci\run_meson_test.py -- -C core\build --suite fast
```

`/experimental:c11atomics` is required on every MSVC build: libvmaf uses C11
atomics and MSVC's `<stdatomic.h>` refuses them without it. `cl.exe` prints
`for ARM64` in its banner when the ARM64-hosted toolset is active; the x64
cross toolset (`vcvarsall.bat amd64_arm64`) also produces ARM64 binaries, but
runs the compiler under emulation. `install\bin\vmaf.exe` is an ARM64 image
(PE machine `0xAA64`).

What is different from the other AArch64 builds:

- Strict floating point: where GCC and clang are given `-ffp-contract=off`
  for the float NEON carve-outs, MSVC is given `/fp:precise`, which
  generates no fused multiply-adds by default (`arm64_strict_fp_args` in
  `core/src/meson.build`).
- SVE2 is not built (MSVC has no `<arm_sve.h>`) and not probed at runtime
  outside Linux, so the binary dispatches NEON.
- CUDA is not available: CUDA 13.3.1 has no Windows ARM64 packages.
