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

Run commands from the repository root after installing your platform's
build dependencies. For native Windows setup, use the
[Windows build guide](building-on-windows.md).

For a CPU build, explicitly disable the optional GPU backends and DNN runtime:

```bash
meson setup build core \
  -Denable_cuda=false -Denable_sycl=false -Denable_hip=false \
  -Denable_metal=disabled -Denable_dnn=disabled
ninja -C build
meson test -C build
```

The CLI is `build/tools/vmaf` (or `build/tools/vmaf.exe` on Windows).
Use a fresh build directory for each backend configuration; the
[backend guides](../backends/index.md) describe the required SDK setup and
configuration options. For the shared development container, see
[the dev-MCP guide](../development/dev-mcp.md).

Continue with the [CLI reference](../usage/cli.md) for scoring examples and
[engineering principles](../principles.md) for contribution standards.
