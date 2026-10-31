<!-- Copyright 2026 Lusoris -->
<!-- SPDX-License-Identifier: EUPL-1.2 -->

# Rust bindings

VMAFx extends Netflix's Video Multi-Method Assessment Fusion (VMAF) video
quality library. `vmafx` provides Rust wrappers; `vmafx-sys` generates the raw
libvmaf FFI with bindgen. This build work does not publish either crate.
Use the bindings from a source checkout of your fork:

```toml
[dependencies]
vmafx = { path = "/path/to/your-fork/bindings/rust/vmafx" }
```

## Install the native library

Install libvmaf development headers and a linkable library from the same
revision as the bindings. Follow the [source build guide](../getting-started/index.md#build-from-source-any-platform)
and install to your chosen prefix. Rust and libclang are needed for bindgen;
the Rust build does not compile the C core or require GPU SDKs itself.

`vmafx-sys` searches `LIBVMAF_PREFIX` first when set, then `/usr/local`, then
`/usr`, without duplicate prefixes. A prefix must contain
`include/libvmaf/libvmaf.h` and a library in `lib`, `lib64`, or a multiarch
subdirectory of `lib`. Supported linker filenames are `libvmaf.so`,
`libvmaf.dylib`, `libvmaf.a`, `libvmaf.dll.a`, and `vmaf.lib`.

```bash
LIBVMAF_PREFIX=/path/to/install cargo build -p vmafx -j 8
```

For shared-library builds, configure your platform's runtime library loader
to find that installation too; `LIBVMAF_PREFIX` controls build-time discovery.

### pkg-config installations

The pkg-config module is **`libvmaf`**, supplied by `libvmaf.pc`. Inspect the
installation with `pkg-config --modversion libvmaf` and
`pkg-config --variable=prefix libvmaf`. Pass that prefix as `LIBVMAF_PREFIX`
when it is outside the default search paths.

The workspace lockfile does not contain the `pkg-config` Rust crate, so this
build uses the dependency-free prefix fallback. It does not invoke pkg-config
or consume `PKG_CONFIG_PATH` automatically. The prefix still needs both the
headers and the linkable library described above.

### Build errors

An absent or incomplete development installation fails with this named error;
the list contains the prefixes actually searched, in order:

```text
vmafx-sys: libvmaf and libvmafx not found (looked in: /path/to/install, /usr/local, /usr); set LIBVMAF_PREFIX, install libvmaf development files, or enable the vendored feature
```

Install matching development files or correct `LIBVMAF_PREFIX`. If an
installation is found but its header lacks symbols used by the wrappers,
the existing stale-header check instead names that header and the missing
symbols. Install the library from your checkout; changing version macros
does not fix an older header. See [stale-header troubleshooting](../development/rust.md#an-older-install-under-the-prefix).

## System library vs vendored build

`vmafx-sys` supports two build modes: linking against an existing system
installation (default), or building the vendored C core from source.

- **System** (default, `vendored` off): discovers headers and links both
  `libvmaf` and `libvmafx` dynamically through `LIBVMAF_PREFIX` and the
  standard prefixes. Uses what the installed library was built with.
- **Vendored** (`features = ["vendored"]`): compiles the CPU-only core with
  Meson and Ninja into `OUT_DIR` and links it statically. Needs no GPU
  toolchain and no runtime library.

System mode requires both `libvmaf` and `libvmafx` development libraries
installed under searched prefixes.
Vendored mode compiles and statically links the C core from `OUT_DIR`; binaries
built with `vendored` require no runtime library installation.

To enable the vendored build, activate the `vendored` feature:

```toml
[dependencies]
vmafx-sys = { path = "/path/to/your-fork/bindings/rust/vmafx-sys", features = ["vendored"] }
```

When `vendored` is active, `build.rs` invokes Meson and Ninja to compile the C
core
with CPU-only options, disabling every GPU backend (`-Denable_cuda=false`,
`-Denable_sycl=false`, `-Denable_hip=false`, `-Denable_metal=disabled`), the
ONNX Runtime tiny-model
runtime (`-Denable_dnn=disabled`, so the result does not depend on what the host
has installed), as well as test suites, documentation, and tools
(`-Denable_tests=false`, `-Denable_docs=false`,
`-Denable_tools=false`). The resulting static libraries (`libvmaf.a`,
`libvmafx.a`, and
feature archives) are placed in `OUT_DIR/build/src` and linked statically.
Transitive
system dependencies (`pthread`, `m`, C++ runtime `stdc++`/`c++`, and optional
dependencies
such as `dl`) are parsed directly from Meson pkg-config metadata and linked.

There is no silent fallback to system libraries when `vendored` is enabled. If
the
source directory cannot be located or if Meson/Ninja fail, the build fails
immediately.

### Vendored build errors

An absent or incomplete vendored source directory fails with one of these named
errors:

```text
vmafx-sys: vendored source directory not found; set VMAFX_VENDOR_DIR or build from a checkout with core/
```

When `VMAFX_VENDOR_DIR` points to an invalid directory:

```text
vmafx-sys: VMAFX_VENDOR_DIR does not exist: /path/to/source
```

When Meson or Ninja cannot be executed from `PATH`:

```text
vmafx-sys: failed to execute 'meson' (...); ensure meson is installed and in PATH: command was '...'
```

When Meson or Ninja exits with a non-zero status:

```text
vmafx-sys: meson failed with exit status exit status: 1: command was '...'
```

### Environment variables

- `LIBVMAF_PREFIX`: First installation prefix searched for system development
  headers and libraries.
- `VMAFX_VENDOR_DIR`: Source directory containing `meson.build` for `vendored`
  builds (defaults to repository `core/`).
- `DOCS_RS`: When set, generates bindings from checkout headers without probing
  native libraries or invoking Meson/Ninja.

## docs.rs mode

When `DOCS_RS` is set (including an empty value), `build.rs` skips native
library discovery and emits no native link directives. It generates bindings
from `../../../core/include/libvmaf/libvmaf.h`, resolved relative to
`CARGO_MANIFEST_DIR`, and retains the allowlist and stale-header check.
No installed libvmaf or GPU SDK is needed; bindgen still needs libclang.

```bash
DOCS_RS=1 cargo doc -p vmafx-sys --no-deps -j 8
```

Missing checkout headers fail with `vmafx-sys: DOCS_RS header not found: ...`;
there is no fallback to unrelated system headers. The
[`package.metadata.docs.rs` settings](https://docs.rs/about/metadata) select
only `x86_64-unknown-linux-gnu`. See also the
[docs.rs build environment
documentation](https://docs.rs/about/builds#detecting-docsrs).

This mode currently requires the source checkout. The crate uses Cargo's
default packaging, which does not include the C headers under `core/`,
because they lie outside the crate directory. Preparing those headers for a
standalone registry upload is separate publication work; an extracted crate
alone cannot use this docs.rs mode yet.

## Version policy

The Rust crates are independently versioned, as specified by
[ADR-1127](../adr/1127-single-semver-release-stream.md). Their manifests
currently declare `0.1.0`; the product's release stream does not reset or
automatically publish them. Match the native library and bindings revision
when using a checkout. For wrapper ownership rules, see
[context close and retry](rust-context-close.md).
