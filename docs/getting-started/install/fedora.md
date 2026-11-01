# Installing on Fedora and the RHEL 9 family

Run the setup script from the repository root, then build. The script supports
Fedora 40 and later, RHEL 9, Rocky Linux 9 and AlmaLinux 9; on the RHEL family
it enables EPEL first
([`scripts/setup/fedora.sh`](https://github.com/VMAFx/vmafx/blob/master/scripts/setup/fedora.sh)).

## Setup script

```bash
bash scripts/setup/fedora.sh                       # CPU build dependencies and linters
ENABLE_CUDA=true bash scripts/setup/fedora.sh      # + CUDA toolkit from NVIDIA's repository
ENABLE_SYCL=true bash scripts/setup/fedora.sh      # + Intel oneAPI DPC++ compiler
INSTALL_LINTERS=false bash scripts/setup/fedora.sh # skip shfmt and the Python linters
```

The switches take the value `true`; any other value, `1` included, leaves the
option off. `INSTALL_LINTERS` is on unless you set it to `false`.

## Manual install

```bash
sudo dnf groupinstall -y "Development Tools"
sudo dnf install -y \
    clang clang-tools-extra cppcheck \
    meson ninja-build nasm pkgconf-pkg-config \
    python3 python3-pip python3-virtualenv \
    doxygen shellcheck zimg-devel
```

`core/meson.build` needs Meson 1.4.0 or later. If your release ships an older
`meson`, install it from the hash-pinned lock instead:

```bash
python3 -m pip install --user --require-hashes -r requirements/locks/build.txt
```

### CUDA (optional)

The project builds and tests with CUDA 13.4 (`CUDA_VERSION` in
[`build-config.env`](https://github.com/VMAFx/vmafx/blob/master/build-config.env)).
Use NVIDIA's repository for your Fedora release (`fedora40`, `fedora41`, ...):

```bash
sudo dnf config-manager --add-repo \
    https://developer.download.nvidia.com/compute/cuda/repos/fedora41/x86_64/cuda-fedora41.repo
sudo dnf install -y cuda-toolkit
export PATH=/usr/local/cuda/bin:$PATH
```

See the [CUDA backend guide](../../backends/cuda/overview.md).

### SYCL / oneAPI (optional)

```bash
sudo tee /etc/yum.repos.d/oneAPI.repo <<'EOF'
[oneAPI]
name=Intel(R) oneAPI repository
baseurl=https://yum.repos.intel.com/oneapi
enabled=1
gpgcheck=1
repo_gpgcheck=1
gpgkey=https://yum.repos.intel.com/intel-gpg-keys/GPG-PUB-KEY-INTEL-SW-PRODUCTS.PUB
EOF
sudo dnf install -y intel-oneapi-compiler-dpcpp-cpp intel-oneapi-runtime-libs \
    level-zero-devel libva-devel
source /opt/intel/oneapi/setvars.sh
```

These are the packages the setup script installs; `intel-basekit` works as
well. See the [SYCL backend guide](../../backends/sycl/overview.md).

### Intel QSV (optional, for vmaf-tune)

```bash
sudo dnf install -y libvpl libvpl-tools
```

`libvpl-tools` adds `sample_multi_transcode` and the oneVPL probe utilities.
GPUs older than Tiger Lake also need `intel-mediasdk`. Fedora's `ffmpeg` from
RPM Fusion is built with oneVPL. [Intel QSV](intel-qsv.md) has the hardware
matrix and the FFmpeg requirements.

## Build

From the repository root:

```bash
meson setup build core -Denable_cuda=true -Denable_sycl=true
ninja -C build
```

Leave out the `-Denable_*` options you did not install SDKs for. The binary is
`build/tools/vmaf`.
