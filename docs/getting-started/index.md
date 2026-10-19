<!-- markdownlint-disable MD013 MD060 -->
# Getting started

VMAFx scores how a distorted video compares with its reference, the way a
viewer would judge it. You can run it as a container image, as a downloaded
binary, or from a source build. Pick one below, then
[score your first pair](first-score.md).

## Choose how to get VMAFx

| Route | What you get | Runs on | Start here |
| --- | --- | --- | --- |
| Container image | the `vmaf` CLI and `libvmaf`, CPU or one GPU backend per image | any Docker host; GPU images on x86-64 | [Container image](#container-image) |
| Release download | the `vmaf` CLI, `libvmaf.so.3` and `libvmafx.so.1`, CPU only | Linux x86-64 with glibc 2.41 or later | [Release download](#release-download) |
| Source build | every backend your hardware and SDKs support, the tests, the tools | Linux, macOS, Windows | [Build from source](#build-from-source-any-platform) |

Each release is listed on the
[releases page](https://github.com/VMAFx/vmafx/releases). Until `v1.0.0`, the
releases are candidates (`v1.0.0-rc.N`); the [roadmap](../roadmap.md) says what
each candidate covers.

## Container image

The images are published as `ghcr.io/vmafx/vmafx:<tag>`, where `<tag>` is a
release tag such as `v1.0.0-rc.2`. The image's entry point is `vmaf`, so the
arguments after the image name go straight to the CLI:

Images published after `v1.0.0-rc.2` have zstd layers and need Docker Engine 23.0 or
later, Docker Desktop 4.19 or later, Podman or containerd 1.5 or later ([what can pull
them](../usage/docker.md#what-can-pull-the-images)).

```bash
docker run --rm ghcr.io/vmafx/vmafx:v1.0.0-rc.2 --version
```

| Tag | Backend | Platforms |
| --- | --- | --- |
| `<tag>` | CPU | `linux/amd64`, `linux/arm64` |
| `<tag>-cuda13` | CUDA | `linux/amd64` |
| `<tag>-rocm10` | HIP (AMD ROCm) | `linux/amd64` |
| `<tag>-oneapi2026` | SYCL (Intel oneAPI) | `linux/amd64` |

!!! note
    Releases up to `v1.0.0-rc.2` carry the SYCL image as `<tag>-oneapi2025`
    only. Later releases publish both names
    ([`docker-publish-production.yml`](https://github.com/VMAFx/vmafx/blob/master/.github/workflows/docker-publish-production.yml)).

[Docker](../usage/docker.md) shows how to mount your videos and how to give a
GPU image access to its device.

## Release download

Each release attaches a CPU build of the CLI for Linux x86-64:

| Asset | Contents |
| --- | --- |
| `vmaf` | the command-line tool |
| `libvmaf.so`, `libvmaf.so.3`, `libvmaf.so.3.0.0` | the libvmaf API library the tool loads |
| `libvmafx.so`, `libvmafx.so.1`, `libvmafx.so.1.0.0` | the VMAFx engine library; the tool and `libvmaf.so.3` both load it |
| `models.tar.gz` | the VMAF model files |
| `*.bundle` | a Sigstore signature for each file |

1. Download `vmaf`, the three `libvmaf.so*` files and the three `libvmafx.so*`
   files into one directory. The tool finds both libraries next to itself, so
   no `LD_LIBRARY_PATH` is needed.
2. Restore the executable bit, which a download does not keep:

    ```bash
    chmod +x vmaf
    ./vmaf --version
    ```

3. Optional: verify the signatures with the
   [consumer verification recipes](../development/release.md#consumer-verification-recipes).

The binary is built with AVX-512 enabled and without the ONNX runtime, so the
`--tiny-*` options are not available in it
([`build-native-release-artifacts.sh`](https://github.com/VMAFx/vmafx/blob/master/scripts/release/build-native-release-artifacts.sh)).

## Build from source (any platform)

A source build gives you the GPU backends, the tests and the companion tools.

1. Install the build dependencies for your platform:

    | Platform | Package manager | Guide |
    | --- | --- | --- |
    | Ubuntu 22.04 / 24.04 / 26.04 | apt | [Ubuntu](install/ubuntu.md) |
    | Fedora, RHEL 9 family | dnf | [Fedora](install/fedora.md) |
    | Arch Linux | pacman | [Arch](install/arch.md) |
    | Alpine (musl, CPU only) | apk | [Alpine](install/alpine.md) |
    | macOS (Apple silicon or Intel) | Homebrew | [macOS](install/macos.md) |
    | Windows, MSVC | winget or Chocolatey | [Windows](install/windows.md) |
    | Windows, MSYS2 / MinGW-w64 | pacman (MSYS2) | [Building on Windows](building-on-windows.md) |

2. From the repository root, configure a CPU build. Meson's source directory is
   `core/`:

    ```bash
    meson setup build core \
      -Denable_cuda=false -Denable_sycl=false -Denable_hip=false \
      -Denable_metal=disabled -Denable_dnn=disabled
    ```

3. Compile and run the unit tests:

    ```bash
    ninja -C build
    python3 scripts/ci/run_meson_test.py -- -C build
    ```

The CLI is `build/tools/vmaf` (`build/tools/vmaf.exe` on Windows), with
`build/tools/vmafx` as a second name for the same program.

Use a fresh build directory for each backend configuration. The
[backend guide](../backends/index.md) lists the SDK and the Meson options each
backend needs, and [build flags](../development/build-flags.md) lists every
option. For a container with every toolchain installed, see
[the dev-MCP container](../development/dev-mcp.md).

### Compilers

GCC and clang are the reference compilers: the Netflix golden-score gate builds
with one of them
([ADR-1317](../adr/1317-golden-gate-build-isolation.md)). An Intel `icx` /
`icpx` build on Linux links glibc's math library, so its CPU scores equal a GCC
build's ([ADR-1495](../adr/1495-icx-system-libm.md)). No C or C++ file is
compiled with floating-point contraction on any compiler
([ADR-1461](../adr/1461-strict-fp-every-translation-unit.md)).

## Next steps

1. [Score your first pair](first-score.md) and read the output.
2. [Choose a backend](../backends/index.md) for your hardware.
3. Use the [CLI reference](../usage/cli.md), the [C API](../api/index.md) or
   the [FFmpeg filter](../usage/ffmpeg.md).
4. To report results from hardware the project does not own, use the
   [tester image](../usage/tester-image.md).
