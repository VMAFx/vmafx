# CI Workflow Staleness Sweep — 2026-05-29

Audit of all `.github/workflows/*.yml` files for three staleness categories:
stale `libvmaf/` directory paths (post-ADR-0700 rename to `core/`), references to
removed jobs, and deprecated action version tags (v1/v2/v3).

## Findings

### Stale `libvmaf/` directory paths — 1 found

| File | Line | Stale value | Correct value | Disposition |
|---|---|---|---|---|
| `supply-chain.yml` | 50 | `build/libvmaf/libvmaf.so*` | `build/src/libvmaf.so*` | **Fixed in this PR** |

The `meson setup build core` command (repo root, no `working-directory`) places the
shared library at `build/src/libvmaf.so*`. All other workflows that run meson use
`working-directory: core` and reference the library at `build/src/` relative to that
directory, which is correct.

Other occurrences of the string `libvmaf/` in workflow files are either:
- Header include paths (`core/include/libvmaf/`) — correct post-rename
- Comment prose explaining the old path (no-op)
- Library name patterns (`enable_libvmaf`, `vf_libvmaf`, etc.) — not directory paths

### Deprecated action versions — none found

All 11 actions used across the 25 workflow files are pinned to full 40-character commit
SHAs (e.g. `actions/checkout@de0fac2e…`). No `@v1` / `@v2` / `@v3` tags were found.

### Committed conflict markers — already resolved (PR #174)

`security-scans.yml` and `libvmaf-build-matrix.yml` contained merge conflict markers from
commit `24bb5daf89`. These were resolved by PR #174 (2026-05-29) before this sweep.
No conflict markers remain in the clean `origin/master` tree.

### References to removed jobs — none found

All `needs:` job references were verified against their local workflow file. No dangling
cross-workflow job references were detected.

## Conclusion

One trivial one-line fix applied. No non-trivial issues requiring follow-up in state.md.
