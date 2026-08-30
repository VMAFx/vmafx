<!-- markdownlint-disable MD013 MD041 -->
# ADR-1000: Tech-stack badges in README and Go/Rust version pin consistency

- **Status**: Accepted
- **Date**: 2026-06-04
- **Deciders**: Lusoris
- **Tags**: `docs`, `ci`, `build`, `go`, `rust`

## Context

The VMAFx README badge row carried only CI workflow pass/fail badges and a
handful of project-meta badges (license, Conventional Commits, OpenSSF
Scorecard, ko-fi). Reviewers and integrators had no immediate visibility into
the language versions, GPU backend support, SIMD coverage, or container
distribution coordinates from the repository landing page. They had to read
`go.mod`, `core/meson.build`, `Cargo.toml`, and CI workflow files to assemble
that picture.

Additionally, the `go-ci.yml` workflow pinned `go-version: "1.23"` while
`go.mod` declared `go 1.25.0` — a two-major-version gap. Running tests against
a toolchain two versions behind the declared minimum allows regressions to
accumulate silently.

Phase A research confirmed Go 1.26.4 as the current latest stable release
(2026-05-29). The Rust workspace edition was `2021`. Rust edition 2024 (shipped
with Rust 1.85) was evaluated but is blocked by the `bindgen 0.69` dependency:
edition 2024 requires `unsafe extern "C"` blocks in generated FFI code, and
the current bindgen version emits bare `extern "C"` blocks. Bumping bindgen to
0.72+ would fix this but is out of scope for this maintenance PR.

## Decision

1. Add tech-stack badges to the README grouped into four categories:
   CI/build/test, version pins, GPU/SIMD capabilities, and
   distribution/community.
2. Bump `go.mod` minimum from `1.25.0` to `1.26.4` (current latest stable).
3. Bump `go-ci.yml` `go-version` from `"1.23"` to `"1.26.4"` to match.
4. Leave the Rust workspace edition at `2021`; document the blocker.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Keep go-ci.yml at 1.23, only add badges | No CI change required | Two-major-version gap between go.mod and CI remains; regressions can accumulate | Defeats the purpose of surfacing the actual supported version |
| Bump go.mod to 1.25 only (match current go.mod), leave CI at 1.23 | Minimal diff | Stale CI pin persists | The gap is the problem; both must move together |
| Bump Rust edition to 2024 | Modern edition, `let-else` stabilised, `impl Trait` in fn param | `bindgen 0.69` emits bare `extern "C"` blocks; edition 2024 requires `unsafe extern` — compilation fails | Revert mandated by task constraint; follow-up needed |

## Consequences

- **Positive**: Repository landing page immediately communicates language
  versions, GPU/SIMD support, and container coordinates to evaluators without
  requiring them to read individual config files. Go CI now tests against the
  same toolchain version the module declares as its minimum.
- **Negative**: Rust edition 2024 remains unresolved; a follow-up PR must
  upgrade `bindgen` to 0.72+ before bumping the edition.
- **Neutral**: go.sum was unchanged by the `go mod tidy` run — all transitive
  dependencies remain compatible with 1.26.4.

## References

- Phase A research: `go1.26.4` confirmed latest stable via `go.dev/VERSION?m=text`
  and `go.dev/dl/?mode=json` (2026-05-29).
- `bindgen` edition-2024 blocker: <https://github.com/rust-lang/rust-bindgen/issues/2574>
  (extern block unsafe wrapping, fixed in bindgen 0.71+).
- Related PR: this PR.
- ADR-0702 (vmafx-sys FFI crate, bindgen integration).
- ADR-0707 (TAD Rust pilot, cbindgen integration).
