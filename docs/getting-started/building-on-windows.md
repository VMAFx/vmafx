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

The fork's GPU backends (`-Denable_cuda=true`, `-Denable_sycl=true`) are not
supported on Windows via MSYS2. For GPU-accelerated builds on Windows,
follow the vendor SDKs directly (CUDA Toolkit for MSVC, oneAPI DPC++).
