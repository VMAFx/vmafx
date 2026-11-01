# Installing on macOS

Run the setup script from the repository root, then build. On Apple silicon the
build includes the Metal GPU backend automatically
([`scripts/setup/macos.sh`](https://github.com/VMAFx/vmafx/blob/master/scripts/setup/macos.sh)).

## Setup script

```bash
bash scripts/setup/macos.sh                       # build dependencies and linters, Intel or Apple silicon
INSTALL_LINTERS=false bash scripts/setup/macos.sh # skip shellcheck, shfmt and the Python linters
```

The script needs [Homebrew](https://brew.sh). It installs Homebrew's `llvm`,
because Apple's `clang` ships without `clang-tidy` and `clang-format`, and
prints the `PATH` line to add.

## Caveats

- **Metal** is the GPU backend on macOS. It needs Apple silicon (M1 or later);
  on an Intel Mac it reports `-ENODEV` at run time. See the
  [Metal backend guide](../../backends/metal/index.md).
- **CUDA** is not available: NVIDIA does not support macOS.
- **SYCL** is not supported on Apple silicon, and the setup script refuses
  `ENABLE_SYCL=true` there. On an Intel Mac the script only prints where to
  download oneAPI.
- **Intel QSV** is not available: Intel ships oneVPL for Linux and Windows only
  ([vpl-gpu-rt](https://github.com/intel/vpl-gpu-rt), checked 2026-05-08). The
  `vmaf-tune` QSV adapters (`h264_qsv`, `hevc_qsv`, `av1_qsv`) fail FFmpeg's
  encoder probe on macOS; use `h264_videotoolbox` or `hevc_videotoolbox`.

## Manual install

```bash
brew install meson ninja nasm pkg-config llvm cppcheck doxygen zimg
export PATH="$(brew --prefix llvm)/bin:$PATH"   # add this line to your shell profile
```

## Build

From the repository root:

```bash
meson setup build core
ninja -C build
```

`enable_metal` defaults to `auto`, which builds the Metal backend when the
Metal frameworks are present. The binary is `build/tools/vmaf`; select Metal at
run time with `--backend metal`.

To test VMAFx on a Mac without building it, use the
[native macOS tester bundle](../../usage/tester-image.md).
