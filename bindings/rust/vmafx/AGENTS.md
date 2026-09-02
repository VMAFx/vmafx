# AGENTS.md — vmafx (safe binding crate)

## Rebase-sensitive invariants

- **`unsafe` boundary**: every `unsafe { ... }` block must be at an FFI
  call site (i.e. directly invoking a `vmafx_sys::vmaf_*` symbol or
  reading/writing an opaque C struct field). Adding `unsafe fn` to a
  public method signature breaks the contract of the safe layer.
- **Ownership transfer at `Context::read_pictures`**: the method
  consumes both `Picture` values via `into_raw_owned()`, which clears
  the `owned` flag so `Drop` does not double-free buffers libvmaf has
  taken responsibility for. Any refactor that changes how pictures are
  passed across the FFI boundary must preserve this contract.
- **`Send` / `!Sync` split**: `Context`, `Model`, and `Picture` are
  marked `Send` but deliberately not `Sync`. libvmaf does not document
  thread-safe concurrent access on a single object. Do not add
  `unsafe impl Sync` without an ADR.
- **errno mapping is a stable subset**: `Error::from_libvmaf_rc` maps
  five well-known POSIX errno values (`ENOMEM=12`, `EINVAL=22`,
  `ENOSYS=38`/`ENOTSUP=95`, `EACCES=13`, `ENOENT=2`) and falls through
  to `Error::Libvmaf { code }`. Adding new mapped variants is fine;
  changing existing numeric values is a breaking change.
- **No `unsafe` re-export from `vmafx-sys`**: this crate must not
  re-export raw FFI symbols. If a user needs the escape hatch, they
  add `vmafx-sys` as a direct dep.

- **`&raw mut` for FFI out-pointers**: all `*mut T` out-pointer arguments
  to C functions use `&raw mut foo` (not `&mut foo as *mut _`) to satisfy
  `clippy::borrow_as_ptr`. The `clippy::pedantic` profile is run in CI;
  new FFI call sites must follow this pattern.
- **`Self::Variant` in `match self`**: all `match self { ... }` arms in
  `impl Foo` blocks use `Self::` (not the struct/enum name) to satisfy
  `clippy::use_self`. The same applies to `Self { field }` struct literals
  inside constructors.
- **`const fn` for pure accessor functions**: functions that only return a
  field value or do an arithmetic match with no heap allocation should be
  `const fn`.

- **`clippy::expect_used` and `clippy::unwrap_used` are warned** (ADR-1063):
  `src/lib.rs` carries `#![warn(clippy::expect_used, clippy::unwrap_used)]`.
  Library code must return `Result` rather than panic. Test modules opt back in
  with `#[allow(clippy::expect_used, clippy::unwrap_used)]` on the `mod tests`
  block. Do not add `.expect()` or `.unwrap()` to non-test library functions.

## Phase scope (Phase 1, ADR-0929)

In scope: `Context`, `Model`, `Picture`, `Score`, `Error`, lifecycle +
single-pool scoring.

Deferred: model collections, per-feature score readout, output writers
(JSON/XML/CSV), dmabuf/USM import, per-frame iteration adapters.
The raw FFI in `vmafx_sys` remains the escape hatch for these.
