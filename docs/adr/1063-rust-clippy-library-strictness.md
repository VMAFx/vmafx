<!-- markdownlint-disable MD013 -->
# ADR-1063: Rust clippy strictness — scoped bindings suppression, unsafe_op lint, no panicking Default

- **Status**: Accepted
- **Date**: 2026-06-06
- **Deciders**: Lusoris
- **Tags**: `rust`, `lint`, `safety`, `workspace`

## Context

The VMAFX Rust workspace (`bindings/rust/vmafx-sys`, `bindings/rust/vmafx`,
`core/src/feature/rust/tad`) accumulated three categories of clippy /
coding-standard debt that a round-11 audit surfaced:

1. **Panicking `Default` impl in library code** (`vmafx-sys/src/safe.rs`):
   `VmafContext::Default` called `Self::new().expect(...)`, meaning any downstream
   code that calls `Default::default()` could panic on an out-of-memory or
   misconfigured system. In library crates, hidden panic paths violate the
   principle of giving callers error-handling control.

2. **Blanket `#![allow(clippy::all)]` covering hand-written code**
   (`vmafx-sys/src/lib.rs`): The crate root applied `#![allow(clippy::all)]`
   at the crate level to suppress C-naming noise from the bindgen-generated
   output. This suppression bled into `pub mod safe;`, silencing all clippy
   diagnostics on the hand-written safe wrapper.

3. **Missing `#![deny(unsafe_op_in_unsafe_fn)]`** (`vmafx-sys/src/safe.rs`
   and `core/src/feature/rust/tad/src/lib.rs`): Without this lint, an
   `unsafe fn` body is implicitly allowed to perform any unsafe operation
   without an explicit `unsafe {}` block, making auditing significantly harder.

4. **No `clippy::expect_used` / `clippy::unwrap_used` warning** in the
   idiomatic bindings crate (`bindings/rust/vmafx`): The `vmafx` crate is
   the public-facing API layer; undeclared `unwrap`/`expect` calls in library
   code (outside test modules) should be surfaced at compile time.

## Decision

We will apply the following four targeted fixes:

1. **Remove `VmafContext::Default`** from `vmafx-sys/src/safe.rs`. Callers
   that want a default-configured context must call `VmafContext::new()`
   directly and handle the `Result`. A `// NOTE:` comment explains the
   deliberate omission.

2. **Scope `#![allow(clippy::all)]` to the generated bindings sub-module**
   in `vmafx-sys/src/lib.rs`. The generated output is wrapped in a private
   `mod bindings` carrying the lint suppressions; `pub use bindings::*`
   re-exports the symbols at the crate root so existing callers require no
   changes. The `pub mod safe;` declaration is now at crate root and NOT
   covered by the allow.

3. **Add `#![deny(unsafe_op_in_unsafe_fn)]`** to `vmafx-sys/src/safe.rs`
   (as a module-level inner attribute) and to
   `core/src/feature/rust/tad/src/lib.rs` (as a crate-level inner
   attribute).

4. **Add `#![warn(clippy::expect_used)]` and `#![warn(clippy::unwrap_used)]`**
   to `bindings/rust/vmafx/src/lib.rs`. Test modules that legitimately call
   `.expect()` and `.unwrap_err()` carry
   `#[allow(clippy::expect_used, clippy::unwrap_used)]` on the `mod tests`
   block.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Keep `Default` but add `# Panics` doc | Documents the hazard | Does not remove it; callers still trigger it inadvertently | Cosmetic fix, not structural |
| `#[path]` attribute for generated file | Same scoping | Syntactically awkward | `include!` inside a module is idiomatic for generated code |
| Deny `clippy::expect_used` at workspace level | Maximum strictness | Requires blanket allows in build.rs and integration test files | Crate-level is the right granularity |

## Consequences

- **Positive**: `unsafe_op_in_unsafe_fn` deny makes each unsafe operation
  auditable in isolation. Removing the panicking `Default` eliminates a hidden
  abort path. Scoping the clippy suppression means future edits to `safe.rs`
  are checked by the full clippy suite.
- **Negative**: Downstream callers that relied on `VmafContext::default()`
  (none identified in-tree) must switch to `VmafContext::new()`.
- **Neutral / follow-ups**: The `vmafx` crate's `#![warn(clippy::expect_used)]`
  will flag any future `expect()` added outside a test module — this is
  intentional and acts as an ongoing gate.

## References

- ADR-0702: `vmafx-sys` FFI crate introduction.
- ADR-0707: TAD feature extractor (`vmafx-tad` cbindgen pilot).
- ADR-0929: `vmafx` safe bindings crate introduction.
- Round-11 Rust clippy audit (r11-rust-clippy parallel push agent).
