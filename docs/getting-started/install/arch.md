# Installing on Arch Linux

```bash
bash scripts/setup/arch.sh                     # CPU-only
ENABLE_CUDA=1 bash scripts/setup/arch.sh       # + CUDA toolkit from extra
ENABLE_SYCL=1 bash scripts/setup/arch.sh       # + intel-oneapi-basekit (AUR)
INSTALL_LINTERS=1 bash scripts/setup/arch.sh   # + clang-tidy/cppcheck/iwyu
```

## Manual install

```bash
sudo pacman -S --needed \
    base-devel meson ninja pkgconf nasm \
    python python-pip \
    clang cppcheck doxygen
```

### CUDA (optional)

```bash
sudo pacman -S --needed cuda cuda-tools
```

### SYCL (optional, AUR)

```bash
yay -S intel-oneapi-basekit
source /opt/intel/oneapi/setvars.sh
```

### Intel QSV (optional, for `h264_qsv` / `hevc_qsv` / `av1_qsv`)

The fork's three QSV codec adapters in
[`tools/vmaf-tune/`](../../usage/vmaf-tune-codec-adapters.md) require an
FFmpeg built with the Intel oneVPL dispatcher (`libvpl`) — Intel
[archived Media SDK / `libmfx` in May 2023](https://github.com/Intel-Media-SDK/MediaSDK)
and oneVPL is the supported successor.

```bash
sudo pacman -S --needed libvpl vpl-gpu-rt
# vpl-gpu-rt is the runtime for Tiger Lake and newer iGPUs / Arc;
# legacy iGPUs (Skylake … Comet Lake) need `intel-media-sdk` instead.
```

Verified 2026-05-08 against
[`extra/libvpl 2.16.0-2`](https://archlinux.org/packages/extra/x86_64/libvpl/)
and
[`extra/vpl-gpu-rt 26.1.5-1`](https://archlinux.org/packages/extra/x86_64/vpl-gpu-rt/).

The FFmpeg in `extra` is built with `--enable-libvpl` from FFmpeg
[n6.0 onward](https://github.com/FFmpeg/FFmpeg/blob/master/Changelog);
no extra build step is needed if you use the distro FFmpeg. If you
build FFmpeg yourself, see
[Hardware capability matrix](#intel-quick-sync-hardware-capability-matrix)
below for codec / generation gating.

#### Intel Quick Sync hardware capability matrix

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

## Build

```bash
cd core
meson setup ../build -Denable_cuda=true -Denable_sycl=true
ninja -C ../build
```
