# ADR-0856: Security Advisory Sweep — Rust and Python Dependencies (2026-05-29)

- **Status**: Accepted
- **Date**: 2026-05-29
- **Deciders**: lusoris
- **Tags**: `security`, `deps`, `rust`, `python`, `fork-local`

## Context

The project maintains a Rust workspace (`Cargo.lock`) and several Python
dependency files. No systematic advisory scan had been run since the Rust
workspace was bootstrapped with `vmafx-sys` (ADR-0702) and the TAD crate
(ADR-0707). A latent defect was also present: `Cargo.lock` was missing the
`bindgen` subtree (30 packages), which caused `cargo audit` to fail with a
parse error and left the Rust dependency surface unscanned.

## Decision

We run `cargo audit` (after repairing the lock file via `cargo generate-lockfile`)
and `pip-audit` against all in-tree requirements files as a one-time sweep, commit
the repaired `Cargo.lock`, and document the results. No version bumps are applied
for non-security reasons (Renovate owns those).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Bump all outdated deps (`bindgen` 0.69→0.72, `cbindgen` 0.27→0.29) | Reduces drift | Non-security; may introduce build changes; out of scope for a security sweep | Left to Renovate |
| Skip Cargo.lock repair and only check Python | Faster | Leaves Rust surface unaudited; defect persists in tree | Not acceptable |

## Consequences

- **Positive**: Cargo.lock is now complete and parseable by `cargo audit`. Both
  Rust and Python surfaces are confirmed free of known CVEs/RustSec advisories as
  of 2026-05-29. The incomplete lock file defect is resolved.
- **Negative**: None.
- **Neutral / follow-ups**: Renovate should pick up `bindgen` and `cbindgen`
  version bumps in the next scheduled run.

## References

- Research digest: [docs/research/security-advisory-sweep-20260529.md](../research/security-advisory-sweep-20260529.md)
- `cargo audit` advisory database: https://github.com/RustSec/advisory-db
- `pip-audit` OSV database: https://osv.dev
- ADR-0702 (vmafx-sys FFI crate), ADR-0707 (TAD Rust pilot)
