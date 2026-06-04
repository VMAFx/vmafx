# Installing on Windows

Use PowerShell as an administrator:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\setup\windows.ps1
```

The script uses **winget** (falling back to **Chocolatey**) to install:

- Visual Studio 2022 Build Tools (with the Desktop C++ workload)
- `meson`, `ninja`, `nasm`, `python` (3.11+ for core use;
  3.14.5+ required for `ai/`), `llvm`
- Optional: `CUDA 12.6`, `Intel oneAPI Base Toolkit`

## Environment

After install, open a **x64 Native Tools Command Prompt for VS 2022** so
that `cl.exe` and the Windows SDK are on `PATH`. From there:

```cmd
cd core
meson setup ..\build --buildtype=release
ninja -C ..\build
```

## CUDA

Download the installer from the
[NVIDIA CUDA Toolkit page](https://developer.nvidia.com/cuda-downloads?target_os=Windows).
Re-run meson with `-Denable_cuda=true` after install.

## oneAPI / SYCL

Install the [Intel oneAPI Base Toolkit](https://www.intel.com/content/www/us/en/developer/tools/oneapi/base-toolkit.html),
then initialize in the shell via:

```cmd
"C:\Program Files (x86)\Intel\oneAPI\setvars.bat"
```

Re-run meson with `-Denable_sycl=true`.

## Intel QSV (optional, for `h264_qsv` / `hevc_qsv` / `av1_qsv`)

The fork's three QSV codec adapters in
[`tools/vmaf-tune/`](../../usage/vmaf-tune-codec-adapters.md) require an
FFmpeg built with the Intel oneVPL dispatcher (`libvpl`) — Intel
[archived Media SDK / `libmfx` in May 2023](https://github.com/Intel-Media-SDK/MediaSDK).

On Windows, the oneVPL runtime is bundled with Intel's standard
graphics drivers — install the latest driver for your iGPU / Arc card
from the
[Intel Driver & Support Assistant](https://www.intel.com/content/www/us/en/support/detect.html)
or grab the
[Arc & Iris Xe Graphics driver package](https://www.intel.com/content/www/us/en/download/785597/intel-arc-iris-xe-graphics-windows.html)
directly. No additional SDK install is needed for runtime use.

To build FFmpeg yourself with QSV support, install the
[oneVPL development headers](https://github.com/intel/libvpl/releases)
and pass `--enable-libvpl` (FFmpeg
[n6.0+](https://github.com/FFmpeg/FFmpeg/blob/master/Changelog) — the
"oneVPL support for QSV" line in the FFmpeg 6.0 changelog) or the
legacy `--enable-libmfx` for older FFmpeg builds. Pre-built FFmpeg
binaries from
[BtbN's gyan.dev / FFmpeg Builds](https://github.com/BtbN/FFmpeg-Builds)
ship with `--enable-libvpl` enabled.

### Intel Quick Sync hardware capability matrix

Verified 2026-05-08 against
[Wikipedia: Intel Quick Sync Video — Hardware decoding and encoding](https://en.wikipedia.org/wiki/Intel_Quick_Sync_Video#Hardware_decoding_and_encoding).

| CPU / GPU generation                          | H.264 enc/dec | HEVC 8-bit enc/dec | HEVC 10-bit enc/dec | AV1 decode | AV1 encode |
|-----------------------------------------------|---------------|--------------------|---------------------|------------|------------|
| Skylake / Kaby Lake / Coffee Lake (Gen 9)     | yes           | yes                | decode only         | no         | no         |
| Ice Lake (Gen 11)                             | yes           | yes                | yes                 | no         | no         |
| Tiger Lake / Alder Lake / Raptor Lake (Xe LP) | yes           | yes                | yes                 | yes        | no         |
| Arc Alchemist (Xe HPG, A-series, 2022)        | yes           | yes                | yes                 | yes        | yes        |
| Arc Battlemage (Xe2, B-series)                | yes           | yes                | yes                 | yes        | yes        |

`av1_qsv` therefore requires Arc Alchemist or newer; `hevc_qsv` 10-bit
requires Ice Lake or newer.

## Notes

- Windows CI is Linux's canary for MSVC quirks (forbidden VLAs, different
  64-bit typedefs, narrowing-conversion strictness). If your change builds
  on Linux but fails on Windows, look for one of those first.
