<!-- markdownlint-disable MD013 MD036 MD060 -->
# ADR-0517: Repair MCP `run_benchmark` tool — drop per-call args, inject VMAF_BIN, guard set -u in bench_all.sh

- **Status**: Accepted
- **Date**: 2026-05-18
- **Deciders**: lusoris, Claude Sonnet 4.6
- **Tags**: mcp, bugfix, fork-local, benchmark

## Context

The `run_benchmark` MCP tool consistently returned an error response with no benchmark
output. The E2E test matrix (v9, 2026-05-18, item 5e) captured the symptom:

> `bench_all.sh exited 1 — likely aborted by set -euo pipefail`

Three independent root causes were found:

**Root cause 1 — spurious positional arguments corrupt `$@` inside sourced setvars.sh**

`_run_benchmark()` called `bench_all.sh` with `-r ref -d dis --width W --height H`
positional arguments. `bench_all.sh` uses `set -euo pipefail` and sources Intel oneAPI
`setvars.sh` inside its body. `setvars.sh` saves `$@` at the top of its body and
interprets it as its own flag set; the unknown flags (`-r`, `-d`, `--width`, `--height`)
propagated into each per-component `env/vars.sh` script, which either hung waiting for
input or exited non-zero. Because the `source` call was inside a `set -euo pipefail`
context, the first component failure silently aborted `bench_all.sh` before any `echo`
had fired — producing empty stdout and stderr and exit code 1.

**Root cause 2 — `VMAF_BIN` not injected into subprocess environment**

`bench_all.sh` defaults `VMAF` to `core/build/tools/vmaf` (a relative in-tree path)
when `VMAF_BIN` is not set. In the `vmaf-dev-mcp` container the binary is installed at
`/usr/local/bin/vmaf` and the in-tree path does not exist. `_run_benchmark()` computed
the correct binary via `_vmaf_binary()` but did not export `VMAF_BIN` to the subprocess,
so `bench_all.sh` always attempted the missing in-tree path.

**Root cause 3 — `set -u` (nounset) aborts when setvars.sh references unset variables**

Even after removing the spurious args, `source setvars.sh >/dev/null 2>&1 || true` still
caused a silent abort. The `|| true` guards against a non-zero `return`, but `set -u` in
the calling shell aborts immediately when the sourced script references an unset variable
(e.g., `SETVARS_ARGS`, `ia32`). This exit is not a return-code event; it terminates the
shell process directly and bypasses `|| true`. The fix is to temporarily disable `-u`
with `set +u` around the `source` call, then restore it with `set -u` after.

**Root cause 4 — OUTDIR points to a relative path that does not exist in worktrees**

`bench_all.sh` creates its output JSON files at `testdata/bbb/results/` (relative to
`VMAF_ROOT`). When the MCP server is installed as an editable package from a git worktree,
`_repo_root()` resolves to the worktree directory, which shares the git objects but does
not have the large YUV fixtures or the `testdata/bbb/` directory. Additionally, in the
`vmaf-dev-mcp` container, `/workspace` is mounted read-only, so writing to the workspace
tree is not possible. The fix is to default `OUTDIR` to `/tmp/vmaf-bench-$$` (writable)
and let callers override via `VMAF_BENCH_OUTDIR`.

## Decision

We apply four targeted fixes:

1. **Remove per-call arguments from `run_benchmark`**: the MCP tool schema no longer
   accepts `ref`, `dis`, `width`, `height`. The tool takes no arguments. `bench_all.sh`
   is a fixed-fixture suite; per-pair scoring already has the `vmaf_score` tool.

2. **Inject `VMAF_BIN` into the subprocess environment**: `_run_benchmark()` adds
   `VMAF_BIN: str(_vmaf_binary())` to the environment dict so `bench_all.sh` uses the
   discovered binary path regardless of its own default.

3. **Add `set +u` / `set -u` guard around `source setvars.sh`** in `bench_all.sh`: this
   isolates any nounset references inside `setvars.sh` and its sub-scripts from the
   outer `set -euo pipefail` context.

4. **Default `OUTDIR` to `/tmp/vmaf-bench-$$`**: writable on every platform and in every
   container configuration. Callers who want persistent artefacts may set
   `VMAF_BENCH_OUTDIR`. `_run_benchmark()` also adds data-root discovery that prefers a
   root that actually contains the fixture YUVs (checked via a canary file probe).

Additionally, `run()` in `bench_all.sh` is hardened to treat missing output files as
SKIP (not abort), so unavailable backends (Vulkan without ICD, HIP scaffold-only) do not
kill the whole harness under `set -euo pipefail`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Parse `-r`/`-d` in `bench_all.sh` with getopts | Keeps per-pair flexibility | bench_all.sh is a full-suite runner; per-pair scoring is `vmaf_score`; significant script complexity | Unnecessary complexity — not the right tool |
| Source setvars.sh in a subshell `( source ... )` | Isolates exit calls completely | Env vars set by setvars.sh (oneAPI paths) are lost to the parent; breaks SYCL runs that rely on oneAPI PATH | Defeats the purpose of sourcing |
| Skip setvars.sh entirely when running as MCP subprocess | Simple | Breaks SYCL runs that genuinely need oneAPI env setup | Not robust across all backends |
| Set SETVARS_COMPLETED=1 to skip re-sourcing | Documented setvars.sh flag | Still triggers the `prep_for_exit 3; return` path which returns 3 (non-zero), and `-u` abort on that path is also possible | Fragile; `-u` problem not solved |

## Consequences

**Positive**:

- `run_benchmark` returns a complete benchmark JSON response with per-backend VMAF scores.
- Backends that are unavailable (Vulkan without ICD) produce a SKIP line rather than aborting.
- The fix is backward-compatible: existing calls with `{}` arguments work without changes.

**Negative**:

- Callers who previously passed `ref`/`dis`/`width`/`height` to `run_benchmark` will get
  a validation error ("unknown argument"). This is acceptable: those callers were receiving
  an error response anyway, so no regression in functionality.
- The output JSONs are now ephemeral (`/tmp/vmaf-bench-$$`) rather than persistent at
  `testdata/bbb/results/`. Callers that wanted to read the per-backend JSON artefacts
  after the fact must set `VMAF_BENCH_OUTDIR`.

**Neutral follow-ups**:

- The vmaf binary's "could not open file" on Vulkan fallback (exit 0, no JSON written) is
  a pre-existing issue not introduced by this fix. Tracked separately.
- `docs/mcp/run_benchmark.md` (new, this PR) documents the corrected tool surface.

## References

- req: "Fix MCP server tool `run_benchmark` which currently fails with bench_all.sh exited 1" (user task 2026-05-18)
- E2E Test Matrix v9 Finding 5e: `run_benchmark` FAIL — `.workingdir/bbb_reports/E2E_TEST_MATRIX_v9.md`
- [ADR-0009](0009-mcp-server-tool-surface.md) — MCP tool surface governance
- Intel oneAPI `setvars.sh` `set -u` interaction: confirmed by `bash -x` trace in container (2026-05-18)
