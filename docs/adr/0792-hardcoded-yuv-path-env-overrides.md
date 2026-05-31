<!-- markdownlint-disable MD060 -->
# ADR-0792: Env-var overrides for hardcoded YUV and testdata paths

- **Status**: Accepted
- **Date**: 2026-05-29
- **Deciders**: lusoris
- **Tags**: `workspace`, `ci`, `testdata`

## Context

Several testdata-layer scripts and one AI test carried absolute paths that were
valid only on a single developer's machine
(`/home/kilian/dev/libvmaf_vulkan/...`).  Running them from any other checkout,
worktree, or CI container silently broke or required manual editing.  The same
pattern was already resolved for `testdata/benchmark_netflix.py` (which honours
`VMAF_YUVDIR`) and for `testdata/bench_all.sh` (which honours `VMAF_ROOT`
and `VMAF_BIN`); this ADR extends that pattern to the remaining offenders.

An additional stale path was found in `ai/tests/test_e2e_frame_to_score.py`:
it hard-coded `libvmaf/build-cpu/` which ceased to exist when `libvmaf/` was
renamed to `core/` in ADR-0700.

## Decision

The following env-var overrides are introduced, consistent with the existing
`VMAF_ROOT` / `VMAF_BIN` / `VMAF_YUVDIR` convention:

| Script | Env var added | Previous hardcoded value |
|---|---|---|
| `testdata/test_all_backends.sh` | `VMAF_BIN`, `VMAF_YUVDIR`, `VMAF_TESTDATA` | `/home/kilian/dev/libvmaf_vulkan/...` |
| `testdata/bench_quick.py` | `VMAF_BIN`, `VMAF_TESTDATA` | `/home/kilian/dev/libvmaf_vulkan/testdata` |
| `testdata/compare_combined.py` | `VMAF_TESTDATA` | `/home/kilian/dev/libvmaf_vulkan/testdata` |
| `ai/tests/test_e2e_frame_to_score.py` | `VMAF_BIN`, `VMAF_YUVDIR` | `REPO_ROOT/libvmaf/build-cpu/...` (stale rename) |

Each script falls back to a computed default derived from `git rev-parse
--show-toplevel` (or `__file__` for Python) so existing no-arg invocations
from the repo root continue to work unchanged.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Leave as-is with a comment | Zero code change | Breaks every non-lusoris checkout and every CI worktree | Unacceptable |
| Replace with a single `VMAF_FIXTURE_ROOT` var | One var to set | Doesn't align with the established `VMAF_YUVDIR` / `VMAF_TESTDATA` split already in use | Consistency wins |

## Consequences

- **Positive**: any worktree, CI container, or external contributor can run
  these scripts without editing source files.
- **Negative**: none — fallbacks preserve old behaviour for the common case.
- **Neutral**: `VMAF_BIN`, `VMAF_YUVDIR`, and `VMAF_TESTDATA` should be
  documented in `docs/development/dev-mcp.md` and the repo-level README as
  standard knobs.

## References

- ADR-0700: `libvmaf/` → `core/` rename (adds stale `build-cpu` path).
- PR #122: established `VMAF_YUVDIR` pattern in `benchmark_netflix.py`.
- `testdata/bench_all.sh`: reference implementation for `VMAF_ROOT` / `VMAF_BIN`.
- Source: user direction (paraphrased) — audit `testdata/` and Netflix golden
  YUV references for hardcoded paths; follow the `.corpus` env-var override
  pattern from PR #122; fix easy cases; open a ready PR.
