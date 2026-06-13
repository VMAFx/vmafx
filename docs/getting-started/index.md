<!-- markdownlint-disable MD013 MD060 -->
# Getting started

Choose your platform below, or see [Building on Windows](building-on-windows.md) for a source build.

| Platform | Package manager | Guide |
|----------|----------------|-------|
| Ubuntu 22.04 / 24.04 / 26.04 | apt | [Install](install/ubuntu.md) |
| Fedora | dnf | [Install](install/fedora.md) |
| Arch Linux | pacman | [Install](install/arch.md) |
| Alpine | apk | [Install](install/alpine.md) |
| macOS | Homebrew | [Install](install/macos.md) |
| Windows | MSYS2 / MinGW | [Install](install/windows.md) |

## Build from source (any platform)

```bash
# CPU only
meson setup build core -Denable_cuda=false -Denable_sycl=false
ninja -C build

# With CUDA
meson setup build core -Denable_cuda=true -Denable_sycl=false
ninja -C build

# With SYCL (Intel oneAPI)
meson setup build core -Denable_cuda=false -Denable_sycl=true
ninja -C build
```

See [Engineering principles](../principles.md) for coding standards and the [Backends](../backends/index.md) section for GPU/SIMD details.
