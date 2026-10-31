# Installing on Arch Linux

Run the setup script from the repository root, then build. The script also
covers Manjaro, CachyOS and EndeavourOS
([`scripts/setup/arch.sh`](https://github.com/VMAFx/vmafx/blob/master/scripts/setup/arch.sh)).

## Setup script

```bash
bash scripts/setup/arch.sh                       # CPU build dependencies and linters
ENABLE_CUDA=true bash scripts/setup/arch.sh      # + cuda and cuda-tools from extra
ENABLE_SYCL=true bash scripts/setup/arch.sh      # prints the oneAPI install steps
INSTALL_LINTERS=false bash scripts/setup/arch.sh # skip shellcheck, shfmt and the Python linters
```

The switches take the value `true`; any other value, `1` included, leaves the
option off. `INSTALL_LINTERS` is on unless you set it to `false`.
`ENABLE_SYCL=true` installs nothing: it prints the commands of the SYCL section
below.

## Manual install

```bash
sudo pacman -S --needed \
    base-devel clang cppcheck \
    meson ninja nasm pkgconf \
    python python-pip python-virtualenv \
    doxygen zimg
```

### CUDA (optional)

```bash
sudo pacman -S --needed cuda cuda-tools
export PATH=/opt/cuda/bin:$PATH
```

`extra/cuda` follows the current CUDA release; the project builds and tests
with CUDA 13.4. See the [CUDA backend guide](../../backends/cuda/overview.md).

### SYCL / oneAPI (optional)

The oneAPI compiler is in the official `extra` repository:

```bash
sudo pacman -S --needed intel-oneapi-dpcpp-cpp
source /opt/intel/oneapi/setvars.sh
```

`intel-oneapi-toolkit` installs the whole toolkit instead. See the
[SYCL backend guide](../../backends/sycl/overview.md).

### Intel QSV (optional, for vmaf-tune)

```bash
sudo pacman -S --needed libvpl vpl-gpu-rt
```

`vpl-gpu-rt` is the runtime for Tiger Lake and newer GPUs, Arc included; older
GPUs (Skylake to Comet Lake) need `intel-media-sdk` instead. `extra/ffmpeg` is
built with oneVPL. [Intel QSV](intel-qsv.md) has the hardware matrix and the
FFmpeg requirements.

## Build

From the repository root:

```bash
meson setup build core -Denable_cuda=true -Denable_sycl=true
ninja -C build
```

Leave out the `-Denable_*` options you did not install SDKs for. The binary is
`build/tools/vmaf`.
