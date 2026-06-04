# Installing on Fedora (40 / 41)

```bash
bash scripts/setup/fedora.sh
ENABLE_CUDA=1 bash scripts/setup/fedora.sh      # + CUDA (RPMFusion + NVIDIA repo)
ENABLE_SYCL=1 bash scripts/setup/fedora.sh      # + intel-basekit
INSTALL_LINTERS=1 bash scripts/setup/fedora.sh  # + clang-tools-extra/cppcheck/iwyu
```

## Manual install

```bash
sudo dnf install -y \
    @development-tools meson ninja-build pkgconf-pkg-config nasm \
    python3 python3-pip \
    clang clang-tools-extra cppcheck doxygen
```

### CUDA (optional)

```bash
sudo dnf config-manager --add-repo \
    https://developer.download.nvidia.com/compute/cuda/repos/fedora40/x86_64/cuda-fedora40.repo
sudo dnf install -y cuda-toolkit-12-6
```

### SYCL / oneAPI (optional)

```bash
tee /tmp/oneAPI.repo <<'EOF'
[oneAPI]
name=Intel(R) oneAPI repository
baseurl=https://yum.repos.intel.com/oneapi
enabled=1
gpgcheck=1
gpgkey=https://yum.repos.intel.com/intel-gpg-keys/GPG-PUB-KEY-INTEL-SW-PRODUCTS.PUB
EOF
sudo mv /tmp/oneAPI.repo /etc/yum.repos.d/
sudo dnf install -y intel-basekit
source /opt/intel/oneapi/setvars.sh
```

### Intel QSV (optional, for `h264_qsv` / `hevc_qsv` / `av1_qsv`)

The fork's three QSV codec adapters in
[`tools/vmaf-tune/`](../../usage/vmaf-tune-codec-adapters.md) require an
FFmpeg built with the Intel oneVPL dispatcher (`libvpl`) — Intel
[archived Media SDK / `libmfx` in May 2023](https://github.com/Intel-Media-SDK/MediaSDK).

```bash
sudo dnf install -y libvpl libvpl-tools
# libvpl-tools ships sample_multi_transcode and the VPL probe utilities.
# For pre-Tiger-Lake iGPUs you also need `intel-mediasdk`.
```

Verified 2026-05-08 against
[Fedora packages: libvpl + libvpl-tools](https://packages.fedoraproject.org/pkgs/oneVPL/)
(shipped on Fedora 42, 43, Rawhide, and EPEL 9 / 10).

Fedora's `ffmpeg` from RPMFusion is built with `--enable-libvpl` from
FFmpeg n6.0 onward; the
[`Changelog`](https://github.com/FFmpeg/FFmpeg/blob/master/Changelog)
records the "oneVPL support for QSV" entry under FFmpeg 6.0. If you
build FFmpeg yourself, pass `--enable-libvpl` (newer) or
`--enable-libmfx` (legacy, pre-n6.0).

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
