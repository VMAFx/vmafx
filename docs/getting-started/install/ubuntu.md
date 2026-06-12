<!-- markdownlint-disable MD013 -->
# Installing on Ubuntu (22.04 / 24.04 / 26.04)

The [`scripts/setup/ubuntu.sh`](../../scripts/setup/ubuntu.sh) helper does
everything below in one step:

```bash
bash scripts/setup/ubuntu.sh               # CPU-only
ENABLE_CUDA=1 bash scripts/setup/ubuntu.sh # + CUDA toolkit
ENABLE_SYCL=1 bash scripts/setup/ubuntu.sh # + Intel oneAPI
INSTALL_LINTERS=1 bash scripts/setup/ubuntu.sh # + clang-tidy/cppcheck/iwyu
```

## Manual install

```bash
sudo apt-get update
sudo apt-get install -y \
    build-essential meson ninja-build pkg-config nasm \
    python3-venv python3-pip \
    clang clang-format clang-tidy cppcheck doxygen
```

### CUDA (optional)

Requires an NVIDIA GPU. Use the official NVIDIA repo — Ubuntu's `nvidia-cuda-toolkit` is often outdated:

```bash
wget https://developer.download.nvidia.com/compute/cuda/repos/ubuntu2404/x86_64/cuda-keyring_1.1-1_all.deb
sudo dpkg -i cuda-keyring_1.1-1_all.deb
sudo apt-get update
sudo apt-get install -y cuda-toolkit-12-6
echo 'export PATH=/usr/local/cuda/bin:$PATH' >> ~/.bashrc
```

### SYCL / Intel oneAPI (optional)

```bash
wget -qO- https://apt.repos.intel.com/intel-gpg-keys/GPG-PUB-KEY-INTEL-SW-PRODUCTS.PUB \
    | sudo gpg --dearmor -o /usr/share/keyrings/oneapi-archive-keyring.gpg
echo "deb [signed-by=/usr/share/keyrings/oneapi-archive-keyring.gpg] https://apt.repos.intel.com/oneapi all main" \
    | sudo tee /etc/apt/sources.list.d/oneAPI.list
sudo apt-get update
sudo apt-get install -y intel-basekit
source /opt/intel/oneapi/setvars.sh
```

### Intel QSV (optional, for `h264_qsv` / `hevc_qsv` / `av1_qsv`)

The fork's three QSV codec adapters in
[`tools/vmaf-tune/`](../../usage/vmaf-tune-codec-adapters.md) require an
FFmpeg built with the Intel oneVPL dispatcher (`libvpl`) — Intel
[archived Media SDK / `libmfx` in May 2023](https://github.com/Intel-Media-SDK/MediaSDK)
and oneVPL is the supported successor.

```bash
sudo apt-get install -y libvpl2 libvpl-dev
# 24.04 noble / 26.04 plucky  -> libvpl2 / libvpl-dev 2023.3.0 (universe).
# 22.04 jammy  -> libvpl2 / libvpl-dev 2022.1.0 (universe).
# For pre-Tiger-Lake iGPUs also: `intel-media-va-driver` /
# `intel-media-va-driver-non-free`.
```

Verified 2026-05-08 against
[Ubuntu noble libvpl-dev 2023.3.0-1build1](https://packages.ubuntu.com/noble/libvpl-dev)
and the package search for
[libvpl across all suites](https://packages.ubuntu.com/search?keywords=libvpl&searchon=names).

Ubuntu's `ffmpeg` is built with `--enable-libvpl` from FFmpeg n6.0
onward (the
[`Changelog`](https://github.com/FFmpeg/FFmpeg/blob/master/Changelog)
records "oneVPL support for QSV" under FFmpeg 6.0). 22.04 jammy ships
FFmpeg 4.4, which uses the legacy `--enable-libmfx` path — for QSV
work on jammy, install FFmpeg from a backports PPA or build from
source against `libvpl-dev`.

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
meson setup ../build \
    -Denable_cuda=true \
    -Denable_sycl=true
ninja -C ../build
```

Binary lands at `build/tools/vmaf`.

## Run the Netflix golden tests

```bash
make test-netflix-golden
```
