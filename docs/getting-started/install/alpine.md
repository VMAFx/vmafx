# Installing on Alpine Linux (3.20+)

Alpine builds a CPU-only VMAFx: Alpine uses musl libc, which the CUDA and Intel
oneAPI toolchains do not support. Run the setup script from the repository
root, then build
([`scripts/setup/alpine.sh`](https://github.com/VMAFx/vmafx/blob/master/scripts/setup/alpine.sh)).

## Setup script

```bash
bash scripts/setup/alpine.sh                       # CPU build dependencies and linters
INSTALL_LINTERS=false bash scripts/setup/alpine.sh # skip shellcheck, shfmt and the Python linters
```

`ENABLE_CUDA=true` or `ENABLE_SYCL=true` stops the script with an error and
points to the Ubuntu, Fedora or Arch setup.

## Manual install

```bash
apk add --no-cache \
    build-base clang clang-extra-tools cppcheck \
    meson ninja nasm pkgconf \
    python3 py3-pip py3-virtualenv \
    doxygen zimg-dev
```

## Build

From the repository root:

```bash
meson setup build core -Denable_cuda=false -Denable_sycl=false
ninja -C build
```

The binary is `build/tools/vmaf`.

## Why Alpine

A musl build is a portability check: if libvmaf compiles and passes its tests
under musl, the code does not depend on glibc-specific behaviour. No CI job
runs it; run it by hand in an `alpine` container when a change touches
platform code.
