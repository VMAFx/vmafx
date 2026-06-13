<!-- markdownlint-disable MD060 -->
# ADR-0929: Rust `vmafx` safe binding crate — Phase 1 scaffold

- **Status**: Accepted
- **Date**: 2026-05-31
- **Deciders**: lusoris
- **Tags**: `rust`, `bindings`, `ffi`, `phase4`, `workspace`, `fork-local`

## Context

ADR-0706 introduced `vmafx-sys` — the bindgen-generated raw FFI crate — and
tucked a thin safe wrapper inside it (`vmafx_sys::safe`). That worked as a
prototype, but the placement violates Rust convention: by long-standing
ecosystem practice, raw FFI lives in an `-sys` crate and idiomatic Rust
wrappers live in the matching non-`-sys` crate. Keeping the safe layer
inside `vmafx-sys` also forces every consumer of the safe API to pull in
`bindgen` build dependencies transitively.

Phase 4 of the VMAFX modernisation plan (ADR-0702) calls for a first-class
Rust surface so downstream tooling (vmafx-tune, the controller, future MCP
work) can call into libvmaf without writing `unsafe` themselves. The
existing `vmafx_sys::safe` module covers only ~30 % of the surface area
that downstream callers need; it is also harder to evolve in place without
breaking `vmafx-sys` consumers who do not want the safe layer.

This ADR promotes the safe layer to its own crate, **`vmafx`**, sitting on
top of `vmafx-sys`. The first PR ships a scaffold (Phase 1) covering the
essential scoring loop only; advanced surfaces are deferred to follow-on PRs.

## Decision

Create `bindings/rust/vmafx/` as a new workspace member with the following
design:

- `Cargo.toml` declares a single runtime dependency on the path-local
  `vmafx-sys` crate. No `bindgen`, no `cc`, no `pkg-config`.
- `src/lib.rs` is the module index; it re-exports `Context`, `ContextBuilder`,
  `LogLevel`, `Model`, `Picture`, `PixelFormat`, `Score`, `PoolingMethod`,
  `Error`, and `Result`.
- `src/context.rs`, `src/model.rs`, `src/picture.rs`, `src/score.rs`,
  `src/error.rs` carry the wrappers, one file per concept.
- `src/error.rs` exposes an `Error` enum mapping the five well-known POSIX
  errno values libvmaf actually returns (`ENOMEM`, `EINVAL`, `ENOSYS`/`ENOTSUP`,
  `EACCES`, `ENOENT`) to dedicated variants; anything else falls through
  to `Error::Libvmaf { code }`.
- `Context`, `Model`, `Picture` are all `Send` but **not** `Sync`.
  libvmaf's C API does not document thread-safe concurrent access on a
  single object — promising more than that would be unsound.
- `Context::read_pictures` **consumes** both `Picture` values. libvmaf
  takes ownership of the plane buffers internally; the safe wrapper
  enforces this in the type system via `Picture::into_raw_owned()`,
  which clears the internal `owned` flag so `Drop` does not double-free.
- The legacy `vmafx_sys::safe` module remains in place for now to avoid a
  flag-day migration; a follow-on PR will deprecate it once `vmafx`
  reaches feature parity.
- The crate is added as a workspace member in the root `Cargo.toml`.
  Existing `vmafx-sys` API is preserved verbatim.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|--------|------|------|----------------|
| Keep the safe layer inside `vmafx-sys` | No new crate; smaller workspace | Breaks Rust `-sys`/wrapper convention; forces bindgen transitive dep on every safe-API consumer; harder to evolve without breaking `vmafx-sys` consumers | Convention exists for a reason; cost of the split is one extra `Cargo.toml` |
| Ship the full surface in Phase 1 (model collections, per-feature scores, output writers) | Single PR; complete API | ~10× the scope; harder to review; harder to revert if a design choice turns out wrong | Phase-gating keeps each PR reviewable and lets the API mature in slices |
| Build on `thiserror` / `anyhow` for the error type | Less boilerplate | Pulls dependencies into the safe surface that downstream consumers may not want; surfaces in public API force the choice on every caller | Hand-written `Error` enum is ~80 lines and keeps the public surface dependency-free |
| Expose libvmaf errno codes as numeric constants (no mapping) | Trivial implementation; no risk of mis-mapping | Forces every caller to learn libvmaf's errno surface; ergonomic regression vs. the stub already in `vmafx-sys::safe` | The curated 5-variant mapping plus catch-all is the same trade-off the stub made; we preserve it. |
| Promote the stub by `pub use`-re-exporting from `vmafx-sys` with no new crate | Zero file moves | Same module, same crate, same convention violation; just hides it | Same root problem as option 1 |

## Consequences

- **Positive**: Downstream Rust code (vmafx-tune, controller, MCP) can
  depend on `vmafx` alone for the safe API. No transitive `bindgen` cost
  if all you want is `Context::score_pooled`.
- **Positive**: The crate ships with `#![deny(unsafe_op_in_unsafe_fn)]`
  and `#![warn(missing_docs)]`, which `vmafx-sys` cannot do (the generated
  FFI is rife with unsafe and undoc'd C symbols).
- **Positive**: Future safe-API evolution (adding per-feature scores,
  output writers, async adapters) lands in `vmafx` without touching the
  FFI surface. CI keeps the FFI crate stable.
- **Negative**: One more workspace member to keep `cargo fmt` and
  `cargo clippy -D warnings` clean.
- **Negative**: Two ways to spell the same thing during the transition
  (`vmafx_sys::safe::VmafContext` vs. `vmafx::Context`). The follow-on
  PR that deprecates the legacy module closes this.
- **Neutral**: Phase 1 covers the essential scoring loop only. Advanced
  callers fall through to raw FFI in `vmafx-sys`. Subsequent ADRs will
  cover the per-feature score surface, model collections, and output
  writers in separate PRs.

## References

- ADR-0702: VMAFX Phase 4 language modernisation umbrella.
- ADR-0706: `vmafx-sys` FFI crate (the layer this one sits on).
- ADR-0707: Rust feature-extractor pilot (TAD) — sibling Rust crate.
- `docs/development/rust.md`: Rust workspace overview.
- `bindings/rust/vmafx-sys/src/safe.rs`: the stub being promoted (kept in
  tree for backward compatibility).
- req: "Phase 4 modernization #11: Rust safe binding crate `vmafx` over
  `vmafx-sys`" (2026-05-31 session).
