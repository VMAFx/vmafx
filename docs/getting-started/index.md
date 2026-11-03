<!-- markdownlint-disable MD013 MD060 -->
# Getting started

VMAFx scores how a distorted video compares with its reference, the way a
viewer would judge it. You can run it as a container image, as a downloaded
binary, or from a source build. Pick one below, then
[score your first pair](first-score.md).

## Choose how to get VMAFx

Each platform has its own table: the route, the command, whether it is
available today, and the GPU backends that route ships.

### Linux x86-64

| Route | Command | Status | GPU backends |
| --- | --- | --- | --- |
| Container | `docker run --rm ghcr.io/vmafx/vmafx:<tag> --version` | available | CUDA, HIP, SYCL |
| Release download | download `vmaf`, `libvmaf.so.3`, `libvmafx.so.1` (glibc 2.41 or later) | available | none |
| Homebrew | `brew install Vidiko/tap/vmafx` | after the 0.1.0 tag | none |
| conda-forge | `conda install -c conda-forge vmafx` | after 0.1.0, pending acceptance | none |
| winget | — | not offered | — |
| APT / RPM | `apt install vmafx` / `dnf install vmafx` | planned 0.2.0 ([#3115](https://github.com/VMAFx/vmafx/issues/3115)) | see manifest |
| Source build | `meson setup build core && ninja -C build` | available | CUDA, HIP, SYCL |

- **Container:** Available via multi-arch CPU and dedicated GPU container images (`ghcr.io/vmafx/vmafx:<tag>`, `<tag>-cuda13`, `<tag>-rocm10`, `<tag>-oneapi2026`) built by [`.github/workflows/docker-publish-production.yml`](https://github.com/VMAFx/vmafx/blob/master/.github/workflows/docker-publish-production.yml).
- **Release download:** Available via artifact `native-linux-x86_64` in [`packaging/manifest.toml`](https://github.com/VMAFx/vmafx/blob/master/packaging/manifest.toml), built by [`.github/workflows/supply-chain.yml`](https://github.com/VMAFx/vmafx/blob/master/.github/workflows/supply-chain.yml) (`Build Linux artifacts`). Backends: CPU only.
- **Homebrew:** Formula in `Vidiko/homebrew-tap` published after the 0.1.0 tag (channel `homebrew`, tracking [#2319](https://github.com/VMAFx/vmafx/issues/2319), [ADR-3387](../adr/3387-homebrew-tap-publish.md)). Builds from source with CPU backend.
- **conda-forge:** Recipe submitted after the 0.1.0 tag (channel `conda-forge`, tracking [#2319](https://github.com/VMAFx/vmafx/issues/2319)). Builds from source with CPU backend.
- **winget:** Not offered on Linux (Windows package manager).
- **APT / RPM:** Native distribution packages planned for 0.2.0 (tracking [#3115](https://github.com/VMAFx/vmafx/issues/3115)).
- **Source build:** Available via source archive (`artifacts.source`) or Git repository. Configurable with `-Denable_cuda=true`, `-Denable_hip=true`, and `-Denable_sycl=true`.

### Linux arm64

| Route | Command | Status | GPU backends |
| --- | --- | --- | --- |
| Container | `docker run --rm ghcr.io/vmafx/vmafx:<tag> --version` | available | none |
| Release download | — | not offered | — |
| Homebrew | `brew install Vidiko/tap/vmafx` | after the 0.1.0 tag | none |
| conda-forge | `conda install -c conda-forge vmafx` | after 0.1.0, pending acceptance | none |
| winget | — | not offered | — |
| APT / RPM | `apt install vmafx` / `dnf install vmafx` | planned 0.2.0 ([#3115](https://github.com/VMAFx/vmafx/issues/3115)) | see manifest |
| Source build | `meson setup build core && ninja -C build` | available | none |

- **Container:** Available via multi-arch CPU container image (`ghcr.io/vmafx/vmafx:<tag>`, platform `linux/arm64`) built by [`.github/workflows/docker-publish-production.yml`](https://github.com/VMAFx/vmafx/blob/master/.github/workflows/docker-publish-production.yml). Backends: CPU only.
- **Release download:** Not offered (no native Linux arm64 binary artifact in [`packaging/manifest.toml`](https://github.com/VMAFx/vmafx/blob/master/packaging/manifest.toml)).
- **Homebrew:** Formula in `Vidiko/homebrew-tap` published after the 0.1.0 tag (channel `homebrew`, tracking [#2319](https://github.com/VMAFx/vmafx/issues/2319)). Builds from source with CPU backend.
- **conda-forge:** Recipe submitted after the 0.1.0 tag (channel `conda-forge`, tracking [#2319](https://github.com/VMAFx/vmafx/issues/2319)). Builds from source with CPU backend.
- **winget:** Not offered on Linux.
- **APT / RPM:** Native distribution packages planned for 0.2.0 (tracking [#3115](https://github.com/VMAFx/vmafx/issues/3115)).
- **Source build:** Available via source archive (`artifacts.source`) or Git repository with NEON SIMD CPU backend.

### macOS Arm

| Route | Command | Status | GPU backends |
| --- | --- | --- | --- |
| Container | — | not offered | — |
| Release download | download `vmafx-tester-macos-arm64-v<version>.tar.xz` | available | Metal |
| Homebrew | `brew install Vidiko/tap/vmafx` | after the 0.1.0 tag | none |
| conda-forge | `conda install -c conda-forge vmafx` | after 0.1.0, pending acceptance | none |
| winget | — | not offered | — |
| APT / RPM | — | not offered | — |
| Source build | `meson setup build core && ninja -C build` | available | Metal |

- **Container:** Not offered (native macOS containers are not supported).
- **Release download:** Available via tester bundle artifact `tester-macos-arm64` in [`packaging/manifest.toml`](https://github.com/VMAFx/vmafx/blob/master/packaging/manifest.toml), built by [`.github/workflows/macos-tester-bundle.yml`](https://github.com/VMAFx/vmafx/blob/master/.github/workflows/macos-tester-bundle.yml) (`Build and test the bundle (macOS arm64)`). Includes Metal GPU backend.
- **Homebrew:** Formula in `Vidiko/homebrew-tap` published after the 0.1.0 tag (channel `homebrew`, tracking [#2319](https://github.com/VMAFx/vmafx/issues/2319), [ADR-3387](../adr/3387-homebrew-tap-publish.md)). Builds from source with CPU backend.
- **conda-forge:** Recipe submitted after the 0.1.0 tag (channel `conda-forge`, tracking [#2319](https://github.com/VMAFx/vmafx/issues/2319)). Builds from source with CPU backend.
- **winget / APT / RPM:** Not offered on macOS.
- **Source build:** Available via source archive (`artifacts.source`) or Git repository. Metal GPU backend is automatically enabled on Apple silicon.

### macOS x86-64

| Route | Command | Status | GPU backends |
| --- | --- | --- | --- |
| Container | — | not offered | — |
| Release download | — | not offered | — |
| Homebrew | `brew install Vidiko/tap/vmafx` | after the 0.1.0 tag | none |
| conda-forge | `conda install -c conda-forge vmafx` | after 0.1.0, pending acceptance | none |
| winget | — | not offered | — |
| APT / RPM | — | not offered | — |
| Source build | `meson setup build core && ninja -C build` | available | none |

- **Container:** Not offered (native macOS containers are not supported).
- **Release download:** Not offered (no Intel macOS binary artifact in [`packaging/manifest.toml`](https://github.com/VMAFx/vmafx/blob/master/packaging/manifest.toml)).
- **Homebrew:** Formula in `Vidiko/homebrew-tap` published after the 0.1.0 tag (channel `homebrew`, tracking [#2319](https://github.com/VMAFx/vmafx/issues/2319), [ADR-3387](../adr/3387-homebrew-tap-publish.md)). Builds from source with CPU backend.
- **conda-forge:** Recipe submitted after the 0.1.0 tag (channel `conda-forge`, tracking [#2319](https://github.com/VMAFx/vmafx/issues/2319)). Builds from source with CPU backend.
- **winget / APT / RPM:** Not offered on macOS.
- **Source build:** Available via source archive (`artifacts.source`) or Git repository with CPU backend (Metal requires Apple silicon).

### Windows x86-64

| Route | Command | Status | GPU backends |
| --- | --- | --- | --- |
| Container | — | not offered | — |
| Release download | download `vmafx-tester-windows-x64-v<version>.zip` (or the `-cuda` / `-sycl` bundle) | available | CUDA, SYCL |
| Homebrew | — | not offered | — |
| conda-forge | — | not offered | — |
| winget | `winget install VMAFx.VMAFX` | after 0.1.0, pending acceptance | none |
| APT / RPM | — | not offered | — |
| Source build | `meson setup build core && ninja -C build` | available | CUDA, SYCL |

- **Container:** Not offered (no native Windows container images published).
- **Release download:** Available via tester bundle artifacts `tester-windows-x64` (CPU), `tester-windows-x64-cuda` (CUDA), and `tester-windows-x64-sycl` (SYCL) in [`packaging/manifest.toml`](https://github.com/VMAFx/vmafx/blob/master/packaging/manifest.toml), built by [`.github/workflows/windows-tester-bundle.yml`](https://github.com/VMAFx/vmafx/blob/master/.github/workflows/windows-tester-bundle.yml).
- **Homebrew / APT / RPM:** Not offered on Windows.
- **conda-forge:** Not offered on Windows: the generated recipe ([`scripts/release/distribution.py`](https://github.com/VMAFx/vmafx/blob/master/scripts/release/distribution.py)) builds with a POSIX shell script.
- **winget:** Manifests generated for package `VMAFx.VMAFX` carrying artifact `tester-windows-x64` (CPU backend), submitted after the 0.1.0 tag (channel `winget`, tracking [#2319](https://github.com/VMAFx/vmafx/issues/2319)).
- **Source build:** Available via source archive (`artifacts.source`) or Git repository. Configurable for CPU, CUDA, and SYCL backends.

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
3. Use the [CLI reference](../usage/cli.md), the [VMAFx C API](../api/vmafx/index.md) or
   the [FFmpeg filter](../usage/ffmpeg.md).
4. To report results from hardware the project does not own, use the
   [tester image](../usage/tester-image.md).
