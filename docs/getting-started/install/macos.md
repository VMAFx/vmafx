<!-- markdownlint-disable MD013 -->
# Installing on macOS

```bash
bash scripts/setup/macos.sh                     # CPU-only, Intel or Apple silicon
INSTALL_LINTERS=1 bash scripts/setup/macos.sh   # + Homebrew LLVM (clang-tidy/clang-format)
```

## Caveats

- **CUDA** is unsupported on macOS by NVIDIA. Use Linux or Windows for CUDA work.
- **SYCL via Intel oneAPI** is unsupported on Apple Silicon; the setup script blocks it.
- **Intel QSV** is unsupported on macOS. Intel's oneVPL / Media SDK
  drivers ship for Linux and Windows only — the QSV codec adapters
  (`h264_qsv`, `hevc_qsv`, `av1_qsv`) in
  [`tools/vmaf-tune/`](../../usage/vmaf-tune-codec-adapters.md) will
  fail the FFmpeg-encoder probe on macOS regardless of host CPU
  vendor. Use `h264_videotoolbox` / `hevc_videotoolbox` for
  hardware-accelerated encode on macOS instead. Verified 2026-05-08:
  Intel does not list macOS as a supported OS on the
  [oneVPL / VPL GPU runtime project page](https://github.com/intel/vpl-gpu-rt).
- On Apple Silicon, Apple's built-in `clang` lacks `clang-tidy`/`clang-format`.
  The setup script installs `llvm` from Homebrew and exports its bin dir into `PATH`.

## Manual install

```bash
brew install meson ninja pkg-config nasm doxygen
brew install --with-toolchain llvm     # clang-tidy, clang-format, clangd
```

Then append to your shell RC:

```bash
export PATH="$(brew --prefix llvm)/bin:$PATH"
```

## Build

```bash
cd core
meson setup ../build
ninja -C ../build
```
