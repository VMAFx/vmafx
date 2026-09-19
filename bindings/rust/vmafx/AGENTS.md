# AGENTS.md — vmafx (safe binding crate)

## Rebase-sensitive invariants

- **`unsafe` boundary**: every `unsafe { ... }` block must be at FFI call site
  (direct `vmafx_sys::vmaf_*` invocation or read/write of opaque C struct
  field). adding `unsafe fn` to public method signature breaks safe-layer
  contract.
- **Ownership transfer at `Context::read_pictures`**: method consumes both
  `Picture` values via `into_raw_owned()`; clears `owned` flag so `Drop` does
  not double-free buffers libvmaf owns. any refactor changing picture passing
  across FFI boundary must preserve this contract.
- **`Send` / `!Sync` split**: `Context`, `Model`, `Picture` marked `Send`,
  deliberately not `Sync`. libvmaf does not document thread-safe concurrent
  access on single object. Do not add `unsafe impl Sync` without ADR.
- **errno mapping is a stable subset**: `Error::from_libvmaf_rc` maps 5 POSIX
  errno values (`ENOMEM=12`, `EINVAL=22`, `ENOSYS=38`/`ENOTSUP=95`,
  `EACCES=13`, `ENOENT=2`); falls through to `Error::Libvmaf { code }`. new
  mapped variants fine; changing existing numeric values = breaking change.
- **No `unsafe` re-export from `vmafx-sys`**: crate must not re-export raw FFI
  symbols. escape hatch: user adds `vmafx-sys` as direct dep.

- **`&raw mut` for FFI out-pointers**: all `*mut T` out-pointer arguments to C
  functions use `&raw mut foo` (not `&mut foo as *mut _`) for
  `clippy::borrow_as_ptr`. CI runs `clippy::pedantic` profile; new FFI call
  sites must follow this pattern.
- **`Self::Variant` in `match self`**: all `match self { ... }` arms in
  `impl Foo` blocks use `Self::` (not struct/enum name) for `clippy::use_self`.
  same applies to `Self { field }` struct literals in constructors.
- **`const fn` for pure accessor functions**: functions returning only field
  value or arithmetic match with no heap allocation should be `const fn`.

- **`clippy::expect_used` and `clippy::unwrap_used` are warned** (ADR-1063):
  `src/lib.rs` carries `#![warn(clippy::expect_used, clippy::unwrap_used)]`.
  library code must return `Result` rather than panic. test modules opt back in
  via `#[allow(clippy::expect_used, clippy::unwrap_used)]` on `mod tests`
  block. Do not add `.expect()` or `.unwrap()` to non-test library functions.

## Phase scope (Phase 1, ADR-0929)

In scope: `Context`, `Model`, `Picture`, `Score`, `Error`, lifecycle +
single-pool scoring.

Deferred: model collections, per-feature score readout, output writers
(JSON/XML/CSV), dmabuf/USM import, per-frame iteration adapters.
raw FFI in `vmafx_sys` = escape hatch.
