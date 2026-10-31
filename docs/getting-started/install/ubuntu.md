<!-- markdownlint-disable MD013 -->
# Installing on Ubuntu (22.04 / 24.04 / 26.04)

Run the setup script from the repository root, then build. The script also
works on Debian 12 and Linux Mint 21
([`scripts/setup/ubuntu.sh`](https://github.com/VMAFx/vmafx/blob/master/scripts/setup/ubuntu.sh)).

## Setup script

```bash
bash scripts/setup/ubuntu.sh                       # CPU build dependencies and linters
ENABLE_CUDA=true bash scripts/setup/ubuntu.sh      # + Ubuntu's CUDA toolkit packages
ENABLE_SYCL=true bash scripts/setup/ubuntu.sh      # + Intel oneAPI DPC++ compiler
INSTALL_LINTERS=false bash scripts/setup/ubuntu.sh # skip shellcheck, shfmt and the Python linters
```

The switches take the value `true`; any other value, `1` included, leaves the
option off. `INSTALL_LINTERS` is on unless you set it to `false`.

The script installs Meson and Ninja from PyPI with pinned hashes
(`requirements/locks/build.txt`), because Ubuntu 24.04's `meson` 1.3.2 is older
than the 1.4.0 that `core/meson.build` requires.

## Manual install

1. Install the compilers and tools:

    ```bash
    sudo apt-get update
    sudo apt-get install -y \
        build-essential clang clang-format clang-tidy cppcheck \
        ninja-build nasm pkg-config xxd doxygen libzimg-dev \
        python3 python3-pip python3-venv
    ```

2. Install Meson 1.4.0 or later and Ninja from the hash-pinned lock:

    ```bash
    python3 -m pip install --user --require-hashes -r requirements/locks/build.txt
    ```

### CUDA (optional)

Requires an NVIDIA GPU. Ubuntu's `nvidia-cuda-toolkit` is usually behind; the
project builds and tests with CUDA 13.4 (`CUDA_VERSION` in
[`build-config.env`](https://github.com/VMAFx/vmafx/blob/master/build-config.env)).
Install it from NVIDIA's repository, using `ubuntu2404` or `ubuntu2604` in the
URL to match your release:

```bash
wget https://developer.download.nvidia.com/compute/cuda/repos/ubuntu2404/x86_64/cuda-keyring_1.1-1_all.deb
sudo dpkg -i cuda-keyring_1.1-1_all.deb
sudo apt-get update
sudo apt-get install -y cuda-toolkit-13-4
echo 'export PATH=/usr/local/cuda/bin:$PATH' >> ~/.bashrc
```

The [CUDA backend guide](../../backends/cuda/overview.md) lists the supported
GPUs and the build options.

### SYCL / Intel oneAPI (optional)

```bash
wget -qO- https://apt.repos.intel.com/intel-gpg-keys/GPG-PUB-KEY-INTEL-SW-PRODUCTS.PUB \
    | sudo gpg --dearmor -o /usr/share/keyrings/oneapi-archive-keyring.gpg
echo "deb [signed-by=/usr/share/keyrings/oneapi-archive-keyring.gpg] https://apt.repos.intel.com/oneapi all main" \
    | sudo tee /etc/apt/sources.list.d/oneAPI.list
sudo apt-get update
sudo apt-get install -y intel-oneapi-compiler-dpcpp-cpp intel-oneapi-runtime-libs \
    libva-dev libva-drm2 level-zero-dev
source /opt/intel/oneapi/setvars.sh
```

These are the packages the setup script installs. The full `intel-basekit`
meta-package works as well. See the
[SYCL backend guide](../../backends/sycl/overview.md).

### Intel QSV (optional, for `vmaf-tune`)

The `h264_qsv`, `hevc_qsv` and `av1_qsv` codec adapters of `vmaf-tune` need the
oneVPL dispatcher:

```bash
sudo apt-get install -y libvpl2 libvpl-dev
```

24.04 and 26.04 ship `libvpl` 2023.3.0 in `universe`, 22.04 ships 2022.1.0.
22.04's FFmpeg 4.4 predates oneVPL support, so QSV work on 22.04 needs a newer
FFmpeg. [Intel QSV](intel-qsv.md) has the hardware matrix and the FFmpeg
requirements.

## Build

From the repository root:

```bash
meson setup build core -Denable_cuda=true -Denable_sycl=true
ninja -C build
```

Leave out the `-Denable_*` options you did not install SDKs for. The binary is
`build/tools/vmaf`.

## Run the Netflix golden tests

```bash
make test-netflix-golden
```

The target builds its own GCC or clang configuration in `core/build-golden`
and checks the three Netflix reference pairs
([ADR-1317](../../adr/1317-golden-gate-build-isolation.md)). It needs `pytest`,
and the test clips that `scripts/test/fetch-test-yuvs.sh` downloads
([test fixtures](../../development/test-fixtures.md)).
