<!-- markdownlint-disable MD013 MD060 -->
# ADR-0966: Fix dev/Containerfile post-ADR-0700 libvmaf → core paths (Round 26 audit C.1)

- **Status**: Accepted
- **Date**: 2026-05-31
- **Deciders**: lusoris
- **Tags**: `dev`, `docker`, `containerfile`, `rename`, `adr-0700`, `fork-local`

## Context

ADR-0700 renamed the C library source root from `libvmaf/` to `core/` several
weeks before this fix. The rename was correctly applied in most files but missed
three load-bearing references in `dev/Containerfile`:

1. `COPY --chown=vmaf:vmaf libvmaf/ /build/vmaf/libvmaf/` (line 452) — Docker
   COPY fails with `file not found` because the host path `libvmaf/` no longer
   exists at repo root.
2. `RUN cd libvmaf && CC=icx CXX=icpx meson setup build ...` (line 515) —
   `cd` would fail with "no such file or directory" if the COPY had succeeded.
3. `RUN cd libvmaf && ninja -C build install` (line 533) — same class of failure.

These three stale references caused all `docker compose build dev-mcp` invocations
to fail at the very first `COPY` step, making the development container unbuildable.
The error was confirmed by running the docker build against the main working tree:
`COPY failed: file not found: /libvmaf`.

The library output name `libvmaf.so` and the install prefix `/usr/local` are unchanged
by ADR-0700 — only the source directory name moved. Comments referencing `libvmaf`
as a product/library name (e.g., `--enable-libvmaf`, stage name `libvmaf-build`,
`libvmaf.so.3`) are correct and were not modified.

Round 26 audit (Research-0966) categorised this as bug C.1 — a critical build
blocker for the dev-MCP container workflow mandated by CLAUDE.md §12 r15.

## Decision

Replace the three stale `libvmaf/` path references with `core/` in
`dev/Containerfile`:

- `COPY --chown=vmaf:vmaf libvmaf/ /build/vmaf/libvmaf/` →
  `COPY --chown=vmaf:vmaf core/ /build/vmaf/core/`
- `RUN cd libvmaf && ... meson setup build` → `RUN cd core && ...`
- `RUN cd libvmaf && ninja -C build install` → `RUN cd core && ...`

Also update the directory-list comment (line 434) to reflect the post-rename name,
with a parenthetical citation of ADR-0700.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Create a `libvmaf/` symlink at repo root pointing to `core/` | Zero Containerfile change | Symlinks in Docker build contexts behave inconsistently; adds repo-root noise; hides the real bug under a compatibility shim; violates the spirit of ADR-0700 | Not chosen |
| Revert ADR-0700 (rename `core/` back to `libvmaf/`) | Restores old state | ADR-0700 was a deliberate rebrand decision; reverting breaks all other consumers that already use `core/` | Not chosen |

## Consequences

- **Positive**: `docker compose build dev-mcp` succeeds past the COPY layer; the
  dev-MCP container is buildable again. Build-time verification: the docker build
  progressed past stage `[libvmaf-build 2/28]` to subsequent layers after the fix.
- **Negative**: none — the install paths and library names are unchanged.
- **Neutral / follow-ups**: The `dev/AGENTS.md` invariant note (added in this PR)
  documents the rename-grep discipline so future agents catch similar staleness
  before it reaches a PR.

## References

- [ADR-0700](0700-rename-libvmaf-to-core.md) — original rename decision.
- Research-0966 (this PR's companion digest): `docs/research/0966-dev-containerfile-libvmaf-rename-2026-05-31.md`
- Memory rule: `feedback_fix_preexisting_bugs_too` — "Rename greps must be exhaustive"
  (added as corollary after a large rename left 9 stale `libvmaf/` refs in `ai/` tests).
- Round 26 audit C.1 finding.
