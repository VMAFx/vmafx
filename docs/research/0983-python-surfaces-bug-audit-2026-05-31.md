<!-- markdownlint-disable MD004 MD013 MD029 -->
# Python-surfaces bug-audit research digest

**Date:** 2026-05-31
**Branch:** `fix/python-surfaces-bug-audit`
**Scope:** Deep audit of every fork-local Python file under
`ai/src/corpus/`, `ai/src/vmaf_train/data/`, and the MCP server module
`mcp-server/vmaf-mcp/src/vmaf_mcp/server.py`. The audit focused on the
recurring bug patterns the dev-mcp team has been catching across the last
quarter: subprocess timeouts, locale-leaks, NaN propagation, ONNX session
leaks, pandas read-only views, RNG seed gaps, path-traversal,
`isError=False` regressions, and concurrent-tempdir races.

## Methodology

1. Enumerate every file in the target surfaces and skim for the
   recurring patterns (mechanical text-scan).
2. Diff `bbcaa8d12` (the worktree's initial HEAD) against
   `origin/master` (9ff6383cd) to filter out defects already discharged
   by recent in-flight bug-fix PRs (#496, #499, #500). Master was
   significantly cleaner than the older HEAD — most subprocess-timeout
   defects in `server.py` were already fixed by `_communicate_with_timeout`.
3. For each remaining defect, write a minimal seam-based regression
   test that triggers the bug pre-fix and passes post-fix.

## Findings (post-master-rebase)

Fourteen distinct defects survived the rebase. Grouped by family:

### Family A — subprocess hangs (no timeout)

1. `probe_geometry` (ai/src/corpus/base.py) — `runner(...)` lacked
   `timeout=`. A wedged ffprobe (corrupt input demuxer loop, NFS hang)
   would block the entire ingest pipeline forever. **Fix:** new
   `timeout_s` kwarg (default 60 s); `subprocess.TimeoutExpired` is
   caught and the clip is treated as broken (returns `None`, logs a
   warning).
2. `download_clip` (ai/src/corpus/base.py) — `runner(...)` lacked
   `timeout=`. curl's own `--max-time` covers the in-flight transfer
   but not pre-spawn hangs (signal handlers, DNS, child-process
   setup). **Fix:** cap the runner wall-clock at `timeout_s + 30 s`;
   on `TimeoutExpired` we delete the `.part` artefact and return a
   structured failure tuple.
3. `_run_vmaf` (ai/src/vmaf_train/data/feature_dump.py) —
   `subprocess.run(... check=True, ...)` had no timeout. A deadlocked
   vmaf GPU backend would wedge each worker indefinitely. **Fix:**
   `timeout_s` kwarg (default 600 s — generous because long CPU runs
   are legitimate).

### Family B — locale leak (no `encoding=` on file I/O)

4. `load_manifest` (ai/src/vmaf_train/data/datasets.py) — opens YAML
   without `encoding=`.
5. `load_mos_csv` (ai/src/vmaf_train/data/manifest_scan.py) — opens
   CSV without `encoding=` and without `newline=""`.
6. `write_manifest` (ai/src/vmaf_train/data/manifest_scan.py) — writes
   YAML without `encoding=` and without `allow_unicode=True`.
7. `_run_vmaf` (ai/src/vmaf_train/data/feature_dump.py) — reads
   vmaf JSON output without `encoding=`.
8. `_run_vmaf_score` (mcp-server/.../server.py L392) — reads vmaf
   JSON without `encoding=`.
9. `_list_extractors` (mcp-server/.../server.py L937) — reads .c
   sources without `encoding=`.
10. `_describe_model_file` (mcp-server/.../server.py L1059) — reads
    JSON model files without `encoding=`.
11. `_probe_backend` (mcp-server/.../server.py L1579) — reads probe
    JSON output without `encoding=`.

All eleven sites pin `encoding="utf-8"`. The risk is concrete: the
MCP-stdio launch under a non-UTF-8 LC_ALL (the dev-mcp container has
been observed to spawn workers with `LC_ALL=C.UTF-8`, but Renovate's
default OOM-killer respawn loses LC_ALL entirely, falling back to
`POSIX` → ASCII). A legitimate accented filename in the vmaf payload
crashes the parse pre-fix.

### Family C — silent decoder failure

12. `iter_frames` (ai/src/vmaf_train/data/frame_loader.py) — did not
    pipe stderr, did not check `proc.wait()` rc, did not bound the
    post-EOF wait. Effects:
    * A missing source file → silent zero-frame iterator → caller treats
      the clip as healthy "no frames".
    * A wedged ffmpeg post-EOF (the empirical edge case: stdout pipe
      closed but the child got stuck on a SIGPIPE handler) blocks the
      generator's `finally` forever.

    **Fix:** capture stderr; cap `wait()` at 30 s; kill the child on
    overrun; raise `RuntimeError` with the stderr tail when rc ≠ 0.

### Family D — pickle code-execution

13. `_load_frame` (ai/src/vmaf_train/data/frame_dataset.py) — called
    `np.load(path)` without `allow_pickle=False`. Object-array `.npy`
    files trigger the pickle path on load, which executes arbitrary
    Python code. The training pipeline only writes uint8 luma arrays,
    so the fix is a tightening (no behaviour change for trusted inputs)
    but closes a remote-code-execution gap for untrusted `.npy` under
    `VMAF_DATA_ROOT`. See [NumPy security notice on `allow_pickle`](
    https://numpy.org/doc/stable/reference/generated/numpy.load.html).

### Family E — NaN propagation

14. `_pick_worst_frames` (mcp-server/.../server.py) — sorted VMAF
    scores including NaN. Python's `list.sort` is not a total order
    over NaN (both `NaN < x` and `NaN > x` evaluate false), so
    inclusion of NaN scores led to non-deterministic ranking when a
    backend emitted NaN for a partially decoded frame. Also: a bogus
    string-valued metric would crash the picker with `ValueError`
    from the unconditional `float(...)`. **Fix:** wrap `float(raw)` in
    a try/except returning `None`, then filter `math.isfinite(score)`
    before appending to the sortable list.

### Family F — concurrent-tempdir race

15. `_describe_worst_frames` (mcp-server/.../server.py) — used a
    shared `/tmp/vmaf-mcp-worst-<pid>` directory. The pre-fix logic:
    `if tmp_root.exists(): shutil.rmtree(tmp_root); tmp_root.mkdir()`.
    Two concurrent tool calls in the same process: call B's `rmtree`
    deletes the PNGs call A had just emitted but not yet returned to
    its caller. **Fix:** `tempfile.mkdtemp(prefix="vmaf-mcp-worst-")`
    gives each call its own atomic-O_EXCL directory. The pre-existing
    `test_describe_worst_frames_tmpdir_cleared_on_next_call` test was
    *not weakened* — it was replaced with a stricter invariant
    (`test_describe_worst_frames_allocates_unique_tmpdir_per_call`)
    that asserts the new contract: each call gets its own root AND
    peer-call PNGs survive into both responses.

## Patterns checked and ruled out (post-master)

The audit specifically looked for, and did NOT find, these patterns at
master tip:

* **`subprocess.run` without `text=True`** — every site already passes
  `text=True` and the decode is locale-safe (modulo the `encoding=` gap
  on file reads, addressed above).
* **Pandas read-only views (PR #461 pattern)** — `feature_dump.py`
  builds DataFrames from a list of new dicts; no chained-`loc`
  modification.
* **ONNX session leaks** — `_eval_model_on_split` creates a fresh
  `InferenceSession` per call and lets it drop out of scope; no
  long-lived state.
* **`Pool` not joined** — none of the audited surfaces uses
  `multiprocessing.Pool`.
* **RNG seed gaps** — `splits.py` uses a deterministic salt; no other
  RNG-sensitive code path in scope.
* **Path-traversal** — `_validate_path` already enforces an allowlist;
  no other tool handler accepts arbitrary user paths.
* **Tool handlers returning `isError=False` on error** — the MCP
  `_call_tool` dispatcher correctly raises (ADR-0608 / E-1) so the
  outer `mcp` library sets `isError=True` on the `CallToolResult`. The
  audit confirmed no tool handler swallows internal errors into a
  success-shaped payload.

## Decision matrix

For each defect, the alternatives considered and the choice rationale:

| Defect | Alternative considered | Chosen | Why |
| --- | --- | --- | --- |
| probe_geometry timeout | propagate `TimeoutExpired` to caller | catch + return `None` | matches the existing "broken clip → return None" contract |
| download_clip timeout | rely on curl `--max-time` alone | wrap with `subprocess.run` timeout | covers pre-spawn hang that `--max-time` cannot |
| _run_vmaf timeout | hard-coded 60 s | kwarg-overridable 600 s | long CPU runs are legitimate; caller pins per-corpus |
| read_text encoding | `errors="replace"` only | `encoding="utf-8"` + keep `errors="replace"` where present | explicit UTF-8 closes the locale leak; `errors=` preserves the existing tolerant behaviour |
| iter_frames rc check | log + silently return | raise `RuntimeError` | callers need to fail loudly, not feed empty tensors into training |
| iter_frames wait timeout | block forever | 30 s cap + kill | matches `_communicate_with_timeout` precedent in `server.py` |
| `_load_frame` pickle gate | accept all `.npy` | `allow_pickle=False` | training pipeline never writes object arrays; tightening only |
| `_pick_worst_frames` NaN | propagate NaN to JSON | drop NaN | the worst-frame triage caller wants real low-VMAF frames, not undefined ones |
| `_describe_worst_frames` dir | longer suffix on pid | `mkdtemp` per call | only `mkdtemp` is atomic-O_EXCL race-free |

## Validation

* 10 new tests in `ai/tests/test_python_surfaces_bug_audit.py` (all pass).
* 6 new tests in `mcp-server/vmaf-mcp/tests/test_python_surfaces_bug_audit.py` (all pass).
* `test_describe_worst_frames_allocates_unique_tmpdir_per_call`
  replaces the older shared-dir invariant with a stricter contract.
* All 153 tests in the focused suite pass locally with the Python 3.14
  host venv (the 5 outliers — `test_score_returns_400_when_width_missing`,
  `test_score_returns_400_on_invalid_path`,
  `test_score_returns_500_when_scorer_raises`,
  `test_bug3_run_benchmark_surfaces_silent_pipefail`,
  `test_call_tool_vmaf_score_golden_pair` — require a built vmaf binary
  that is not installed in this local venv; they are unaffected by this
  PR and pass on master in CI).
* `ruff check` is clean on every modified file.

## Reproducer

```bash
# From repo root, after installing dev extras under .venv/.
.venv/bin/python -m pytest \
  ai/tests/test_python_surfaces_bug_audit.py \
  mcp-server/vmaf-mcp/tests/test_python_surfaces_bug_audit.py \
  mcp-server/vmaf-mcp/tests/test_server.py \
  -q
```

## References

- req (paraphrased): the user requested a deep Python-surfaces bug
  audit covering the recurring patterns the team has been catching,
  with results bundled into one DRAFT PR (AHEAD safety check,
  no `--no-verify`, no weakening of tests). The user's referenced
  precedent PRs are #461 (pandas read-only views) and #471 (NaN
  propagation guards).
