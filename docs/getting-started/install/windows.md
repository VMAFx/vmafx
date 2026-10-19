# Installing on Windows (MSVC)

This page builds with Microsoft's compiler (MSVC). For a MinGW-w64 build under
MSYS2, see [Building on Windows](../building-on-windows.md).

## Setup script

Run the setup script from the repository root in PowerShell 7 as an
administrator
([`scripts/setup/windows.ps1`](https://github.com/VMAFx/vmafx/blob/master/scripts/setup/windows.ps1)):

```powershell
powershell -ExecutionPolicy Bypass -File scripts\setup\windows.ps1                 # toolchain and linters
powershell -ExecutionPolicy Bypass -File scripts\setup\windows.ps1 -EnableCuda     # + NVIDIA CUDA Toolkit
powershell -ExecutionPolicy Bypass -File scripts\setup\windows.ps1 -EnableSycl     # + Intel oneAPI Base Toolkit
```

The script installs with **winget**, or with **Chocolatey** when winget is
missing:

| Package | Notes |
| --- | --- |
| Python 3.12 | the Python linters are installed into it |
| Meson, Ninja, NASM | build tools |
| Git, Doxygen | |
| LLVM | `clang-tidy` and `clang-format` |
| Visual Studio 2022 Build Tools | skipped when already present; add the **Desktop development with C++** workload in the Visual Studio Installer afterwards |
| CUDA Toolkit | only with `-EnableCuda` |
| Intel oneAPI Base Toolkit | only with `-EnableSycl` |

`-InstallLinters:$false` skips the Python linters.

## Build

1. Open an **x64 Native Tools Command Prompt for VS 2022**, so that `cl.exe`
   and the Windows SDK are on `PATH`.
2. Enable C11 atomics, which MSVC refuses without this flag:

    ```bat
    set CFLAGS=/experimental:c11atomics
    set CXXFLAGS=/experimental:c11atomics
    ```

3. From the repository root, configure and build:

    ```bat
    meson setup build core --buildtype=release -Dc_std=none
    ninja -C build
    ```

    `-Dc_std=none` keeps Meson from adding `/std:c17`; the build adds `/std:clatest`
    itself, and without the option `cl.exe` prints warning D9025 ("overriding
    `/std:c17` with `/std:clatest`") for every C file.

The binary is `build\tools\vmaf.exe`.

### CUDA

Install the CUDA Toolkit (`-EnableCuda`, or the installer from the
[NVIDIA CUDA Toolkit page](https://developer.nvidia.com/cuda-downloads?target_os=Windows))
and add `-Denable_cuda=true` to the `meson setup` line. Meson finds `cl.exe`
for NVCC through `vswhere` or `PATH`;
[Native MSVC and CUDA](../building-on-windows.md#native-msvc-and-cuda) explains
the lookup.

### SYCL / oneAPI

Install the
[Intel oneAPI Base Toolkit](https://www.intel.com/content/www/us/en/developer/tools/oneapi/base-toolkit.html),
initialise it in the shell, and add
`-Denable_sycl=true`:

```bat
"C:\Program Files (x86)\Intel\oneAPI\setvars.bat"
```

The Windows SYCL build also needs the Level Zero loader, which oneAPI does not
ship for Windows, and the `icx-cl` compiler.
[SYCL on Windows](../../backends/sycl/windows.md) gives the full recipe.

### Intel QSV (optional, for vmaf-tune)

On Windows the oneVPL runtime comes with Intel's graphics driver: install the
current driver from the
[Intel Driver & Support Assistant](https://www.intel.com/content/www/us/en/support/detect.html)
or the
[Arc & Iris Xe Graphics driver package](https://www.intel.com/content/www/us/en/download/785597/intel-arc-iris-xe-graphics-windows.html).
To build FFmpeg with QSV yourself, install the
[oneVPL development headers](https://github.com/intel/libvpl/releases).
[Intel QSV](intel-qsv.md) has the hardware matrix and the FFmpeg requirements.

## Notes

- A change that builds on Linux but fails on Windows usually hits an MSVC
  difference: variable-length arrays are rejected, the 64-bit integer typedefs
  differ, and narrowing conversions are stricter.
- `scripts/dev/preflight.sh --stage msvcism` checks the most common of these
  without a Windows host ([preflight](../../development/preflight.md)).
- Windows on ARM64 has its own recipe in
  [Native MSVC on Windows ARM64](../building-on-windows.md#native-msvc-on-windows-arm64).
