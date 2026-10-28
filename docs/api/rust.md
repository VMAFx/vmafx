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
vmafx-sys: libvmaf not found (looked in: /path/to/install, /usr/local, /usr); set LIBVMAF_PREFIX or install libvmaf development files
```

Install matching development files or correct `LIBVMAF_PREFIX`. If an
installation is found but its header lacks symbols used by the wrappers,
the existing stale-header check instead names that header and the missing
symbols. Install the library from your checkout; changing version macros
does not fix an older header. See [stale-header troubleshooting](../development/rust.md#an-older-install-under-the-prefix).

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
[docs.rs build environment documentation](https://docs.rs/about/builds#detecting-docsrs).

This mode currently requires the source checkout. The explicit Cargo
`include` list ships crate sources, tests, examples and README, and omits
native builds, video fixtures and the out-of-crate C headers. Preparing those
headers for a standalone registry upload is separate publication work;
an extracted crate alone cannot use this docs.rs mode yet.

## Version policy

The Rust crates are independently versioned, as specified by
[ADR-1127](../adr/1127-single-semver-release-stream.md). Their manifests
currently declare `0.1.0`; the product's release stream does not reset or
automatically publish them. Match the native library and bindings revision
when using a checkout. For wrapper ownership rules, see
[context close and retry](rust-context-close.md).
