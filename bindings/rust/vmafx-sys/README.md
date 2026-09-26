<!-- markdownlint-disable MD013 MD060 -->
# vmafx-sys

Raw FFI bindings to **libvmaf** (the [VMAFX](https://github.com/VMAFx/vmafx) fork of
[Netflix/vmaf](https://github.com/Netflix/vmaf)).

The crate exposes two layers:

| Module | Description |
|--------|-------------|
| `vmafx_sys::*` | Auto-generated raw bindings (C types and functions). Use these when full control is needed. |
| `vmafx_sys::safe` | Thin safe Rust wrappers returning `Result<T, VmafxError>`. Start here. |

## Quick start

```toml
# Cargo.toml
[dependencies]
vmafx-sys = { path = "bindings/rust/vmafx-sys" }
```

```rust
use vmafx_sys::safe::{VmafContext, VmafModel};

let model = VmafModel::from_path("/path/to/vmaf_v0.6.1.json")?;
let mut ctx = VmafContext::new()?;
ctx.use_features_from_model(&model)?;

// ... queue frames with ctx.read_pictures() ...

ctx.flush()?;
let score = ctx.score_pooled(&model, 0, n_frames - 1)?;
println!("Mean VMAF: {score:.4}");
ctx.close().expect("vmaf_close failed");
```

`VmafContext::close` consumes the active wrapper. Exactly zero invalidates the
context; any other status returns a `VmafContextCloseError` that exposes only
the initial error and one `retry()`. The context and retry token carry the
registered model lifetime, so safe Rust cannot destroy a model while libvmaf
still references it. Dropping an active context makes an initial close attempt
plus at most one retry. Dropping a close-retry token consumes that retry only
if it has not happened; after a failed explicit retry, drop aborts without a
third close call rather than ending the registered-model borrow while libvmaf
retains the pointer.

## Building

### Prerequisites

1. Build or install `libvmaf`:

   ```bash
   # From the repo root:
   meson setup build -Denable_cuda=false -Denable_sycl=false
   ninja -C build install     # installs to /usr/local by default
   ```

2. (Optional) Set `LIBVMAF_PREFIX` if you installed to a non-standard prefix:

   ```bash
   export LIBVMAF_PREFIX=$HOME/.local
   ```

3. Make sure the linker can find `libvmaf.so` at runtime:

   ```bash
   export LD_LIBRARY_PATH=$LIBVMAF_PREFIX/lib:$LD_LIBRARY_PATH
   # or add $LIBVMAF_PREFIX/lib to /etc/ld.so.conf.d/ and run ldconfig
   ```

### Build

```bash
cargo build -p vmafx-sys
```

### Run the smoke test

```bash
VMAFX_REPO=$(git rev-parse --show-toplevel) \
    cargo run --example score
# Expected output: Mean VMAF score: 76.6680 (or very close)
```

### Run integration tests

```bash
VMAFX_REPO=$(git rev-parse --show-toplevel) \
    cargo test -p vmafx-sys
```

## Environment variables

| Variable | Default | Description |
|----------|---------|-------------|
| `LIBVMAF_PREFIX` | `/usr/local` | Install prefix used by `build.rs` to find headers and the shared library. |
| `VMAFX_REPO` | auto-detected | Repository root; used by tests and examples to resolve YUV and model paths. |
| `VMAFX_YUV_REF` | `<repo>/python/test/resource/yuv/src01_hrc00_576x324.yuv` | Override reference YUV. |
| `VMAFX_YUV_DIST` | `<repo>/python/test/resource/yuv/src01_hrc01_576x324.yuv` | Override distorted YUV. |
| `VMAFX_MODEL` | `<repo>/model/vmaf_v0.6.1.json` | Override model path. |

## API split: `safe` vs raw

`vmafx-sys` deliberately exposes two layers:

**Raw layer** (`use vmafx_sys::*`): bindgen-generated types and `extern "C"` function
declarations. Use this if you need access to collection scoring
(`vmaf_score_pooled_model_collection`), feature-level scores (`vmaf_feature_score_at_index`),
or output file writing (`vmaf_write_output`), which the safe layer does not yet wrap.

**Safe layer** (`use vmafx_sys::safe::*`): thin RAII wrappers that manage object lifetime
and convert negative C error codes to `Result<_, VmafxError>`. `unsafe` is confined to the
actual FFI call sites. `VmafModel` is `Send` but `!Sync`; a `VmafContext` and
its close-retry token are `!Send` because they carry a shared model borrow.

A higher-level `vmafx` crate (in progress) will be built on top of this one.

## License

BSD-3-Clause — see [LICENSE](../../../LICENSE).
