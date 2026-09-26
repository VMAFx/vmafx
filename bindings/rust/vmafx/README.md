<!-- markdownlint-disable MD013 -->
# vmafx — safe Rust bindings to libvmaf

[![License](https://img.shields.io/badge/license-BSD--3--Clause--Plus--Patent-blue.svg)](../../../LICENSE)

Safe, idiomatic Rust API over the raw FFI exposed by [`vmafx-sys`](../vmafx-sys).

This crate is **Phase 1** of the safe-binding effort (ADR-0929): it ships the
essential scoring loop (`Context`, `Model`, `Picture`, `Score`) and a
`Result`-based error type. Advanced surfaces (model collections, per-feature
scores, output writers, dmabuf import) remain accessible through raw FFI in
`vmafx-sys` until follow-on PRs land.

## Why two crates?

Rust convention separates raw FFI (`-sys` crate) from idiomatic wrappers
(non-`-sys` crate). The split lets the FFI crate stay minimal and stable
while the safe layer is free to evolve its API without forcing every
consumer to re-link.

## Quick start

```rust
use vmafx::{Context, Model, Picture, PoolingMethod};

fn main() -> vmafx::Result<()> {
    // The model is declared first because libvmaf keeps a borrowed pointer to
    // it until context teardown completes.
    let model = Model::from_path("/usr/local/share/model/vmaf_v0.6.1.json")?;
    let mut ctx = Context::new()?;
    ctx.use_features_from_model(&model)?;

    // Push one frame; reuse for every frame in your pipeline.
    let r = Picture::new_yuv420p_8bit(576, 324)?;
    let d = Picture::new_yuv420p_8bit(576, 324)?;
    ctx.read_pictures(r, d, 0)?;
    ctx.flush()?;

    let score = ctx.score_pooled(&model, PoolingMethod::Mean, 0, 0)?;
    println!("VMAF mean = {score}");
    Ok(())
}
```

## API surface (Phase 1)

| Type                | Purpose                                                             |
| ------------------- | ------------------------------------------------------------------- |
| `Context`           | Owns a `vmaf_context`; drives the scoring loop.                     |
| `ContextBuilder`    | Optional fine-grained configuration before `Context`.               |
| `ContextCloseError` | Teardown-only context retained after a failed explicit close.       |
| `Model`             | Owns a `vmaf_model` loaded from a `.json` file.                     |
| `Picture`           | Owns a `vmaf_picture` plane allocation.                             |
| `PixelFormat`       | YUV 4:0:0 / 4:2:0 / 4:2:2 / 4:4:4 enum.                             |
| `PoolingMethod`     | Mean / Min / Max / HarmonicMean / Median / Perc5 / Perc10 / Perc20. |
| `Score`             | `(index, value)` pair for per-frame results.                        |
| `LogLevel`          | None / Error / Warning / Info / Debug.                              |
| `Error`             | Result-friendly error type with errno mapping.                      |

## Ownership model

- `Model` and `Picture` are ordinary RAII owners. Dropping an active `Context`
  makes an initial close attempt plus at most one retry. A close-retry token
  preserves the initial error; dropping it consumes the retry only if the
  caller has not already done so. After a failed explicit retry, dropping the
  token aborts without a third close call rather than ending the
  registered-model borrow while libvmaf still retains model pointers.
- `Context::close` consumes the active context. Zero from `vmaf_close`
  invalidates it; any other status returns `ContextCloseError`, which retains
  the model lifetime and exposes only the initial error plus one `retry()`.
- `Context::read_pictures` **consumes** both `Picture` values — libvmaf
  takes ownership of their plane buffers internally. Allocate fresh
  pictures per frame.
- `Model` and `Picture` are `Send` but `!Sync`. A `Context` and its close-retry
  token are also `!Send`: they carry a shared model borrow, while `Model` is
  deliberately `!Sync` because libvmaf does not document concurrent access.

## Errors

Scoring operations return `vmafx::Result<T>`. The `Error` enum maps common
libvmaf errno values (`ENOMEM`, `EINVAL`, `ENOSYS`, `EACCES`, `ENOENT`) to
dedicated variants; the long tail is preserved as `Error::Libvmaf { code }`.
`Context::close` instead returns `Result<(), ContextCloseError>` because a
failure must carry the still-owned context for a safe retry.

```rust
match ctx.close() {
    Ok(()) => {}
    Err(pending) => pending
        .retry()
        .expect("context teardown still pending after retry"),
}
```

```rust
use vmafx::{Error, Model};

match Model::from_path("/nonexistent.json") {
    Ok(_) => unreachable!(),
    Err(Error::NotFound) => eprintln!("model file missing"),
    Err(e) => eprintln!("other failure: {e}"),
}
```

## See also

- [ADR-0706](../../../docs/adr/0706-vmafx-rust-sys-bindings.md) — `vmafx-sys` FFI crate.
- [ADR-0929](../../../docs/adr/0929-rust-safe-binding-scaffold.md) — this crate (Phase 1 scope and rationale).
- [`docs/development/rust.md`](../../../docs/development/rust.md) — Rust workspace overview.
- [`vmafx-sys`](../vmafx-sys) — the raw FFI crate this one wraps.
