<!-- markdownlint-disable MD060 -->
# Research-0966 — dev/Containerfile libvmaf → core rename (Round 26 audit C.1)

**Date**: 2026-05-31
**Author**: lusoris
**Branch**: fix/dev-containerfile-libvmaf-rename
**Status**: Resolved (see ADR-0966)

---

## Finding

Round 26 audit identified three stale `libvmaf/` path references in
`dev/Containerfile` that survived the ADR-0700 directory rename:

| Line | Stale text | Effect |
|------|-----------|--------|
| 452  | `COPY --chown=vmaf:vmaf libvmaf/ /build/vmaf/libvmaf/` | Docker COPY fails: `file not found: /libvmaf` |
| 515  | `RUN cd libvmaf && CC=icx CXX=icpx meson setup build …` | `cd` fails if COPY had succeeded |
| 533  | `RUN cd libvmaf && ninja -C build install` | `cd` fails if prior RUN had succeeded |

All three were introduced when ADR-0700 renamed the C source root `libvmaf/` →
`core/` but the Containerfile grep was either not run or did not cover these
code paths.

## Verification

**Pre-fix behaviour** was confirmed by running:

```text
docker compose --project-directory /home/kilian/dev/vmaf \
  -f /home/kilian/dev/vmaf/dev/docker-compose.yml build dev-mcp 2>&1 | tail -20
```

Output:

```text
#25 [libvmaf-build  2/28] COPY --chown=vmaf:vmaf libvmaf/        /build/vmaf/libvmaf/
#25 ERROR: failed to calculate checksum … "/libvmaf": not found
Containerfile:452
```

**Post-fix behaviour**: running the same command against the worktree
`/tmp/wt-c1-containerfile` (with `--project-directory /tmp/wt-c1-containerfile`)
confirmed the build progressed past the COPY layer and into subsequent SDK layers
(all cached), verifying the fix is correct.

## Root cause

Rename greps after ADR-0700 did not cover `dev/Containerfile`. The memory rule
`feedback_fix_preexisting_bugs_too` (corollary: "Rename greps must be exhaustive")
specifically calls out this pattern from a prior incident where 9 stale `libvmaf/`
references survived in `ai/` tests. The same miss recurred in the Containerfile.

## Fix

Three textual substitutions in `dev/Containerfile`:

1. `COPY libvmaf/ /build/vmaf/libvmaf/` → `COPY core/ /build/vmaf/core/`
2. `RUN cd libvmaf && … meson setup` → `RUN cd core && … meson setup`
3. `RUN cd libvmaf && ninja install` → `RUN cd core && ninja install`

Additionally, the directory-list comment at line 434 was updated to reference
`core/` with an inline ADR-0700 citation.

The library install name (`libvmaf.so.3`), install prefix (`/usr/local`), and the
stage name `libvmaf-build` (which refers to the *library* not the directory) are
unaffected.

## Scope

Pure path fix — no behaviour change, no ABI change, no new dependencies. The
only impact is restoring the dev-MCP container build to a working state.

## References

- ADR-0700: `docs/adr/0700-rename-libvmaf-to-core.md`
- ADR-0966: `docs/adr/0966-dev-containerfile-libvmaf-rename.md`
- Memory: `feedback_fix_preexisting_bugs_too` — rename greps must be exhaustive
