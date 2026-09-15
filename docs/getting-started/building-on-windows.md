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
