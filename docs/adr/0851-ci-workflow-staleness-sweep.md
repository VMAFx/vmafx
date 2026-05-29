# ADR-0851: CI workflow staleness sweep — fix stale `build/libvmaf/` artifact path in supply-chain.yml

- **Status**: Accepted
- **Date**: 2026-05-29
- **Deciders**: lusoris
- **Tags**: `ci`, `build`, `fork-local`

## Context

ADR-0700 renamed the source tree from `libvmaf/` to `core/`. The Meson build was updated
to use `meson setup build core` (or `working-directory: core` in CI). The library output
path therefore moved from `build/libvmaf/libvmaf.so*` to `build/src/libvmaf.so*`.

A post-rename audit of all `.github/workflows/*.yml` files found one surviving stale
path: `supply-chain.yml` line 50 still copied `build/libvmaf/libvmaf.so*` into the SLSA
provenance artifact bundle. This would cause the supply-chain release job to fail with
"no such file or directory" the first time a release tag fires after the rename.

No deprecated action versions (v1/v2/v3 tags) were found — all actions are pinned to
full commit SHAs. No removed jobs were referenced. The previously committed conflict
markers in `security-scans.yml` and `libvmaf-build-matrix.yml` were already resolved by
PR #174.

## Decision

Replace `build/libvmaf/libvmaf.so*` with `build/src/libvmaf.so*` in
`.github/workflows/supply-chain.yml` — the correct post-ADR-0700 output path.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Add `working-directory: core` and use `build/libvmaf/libvmaf.so*` | Mirrors how other workflows address this | Requires splitting the build job into two steps with the new working-directory | More invasive than a one-line path fix; the existing `meson setup build core` form works correctly from the repo root |
| Leave until first release fires and fix then | Zero churn now | Would fail the first real release run | Unacceptable |

## Consequences

- **Positive**: supply-chain release job will correctly stage `libvmaf.so*` into the SLSA
  artifact bundle; `sha256sum` step and `cosign sign-blob` loop will cover the library.
- **Negative**: none.
- **Neutral / follow-ups**: A full post-ADR-0700 path sweep of all CI YAML was performed
  as part of this PR; no other stale `libvmaf/` directory references were found in
  executable workflow steps.

## References

- [ADR-0700](0700-vmafx-repo-layout.md) — the rename decision.
- PR #174 — resolved committed conflict markers in `security-scans.yml` and
  `libvmaf-build-matrix.yml` (predecessor cleanup).
- Source: user direction (2026-05-29 sweep request).
