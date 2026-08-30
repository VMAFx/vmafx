# ADR-0149: Port Netflix #1376 — FIFO-hang fix via `multiprocessing.Semaphore`

- **Status**: Accepted
- **Date**: 2026-04-24
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: upstream-port, python, concurrency, fifo

## Context

The Python harness under `python/vmaf/core/executor.py` opens
reference and distorted workfiles (and procfiles) in FIFO mode by
spawning child `multiprocessing.Process` workers that call
`os.mkfifo(...)` and then stream data into the pipe. The parent
then needs to wait for the pipes to exist before proceeding.

Pre-port, the parent waited via a busy-poll loop:

```python
for i in range(10):
    if os.path.exists(asset.ref_workfile_path) and os.path.exists(asset.dis_workfile_path):
        break
    sleep(0.1)
else:
    raise RuntimeError("ref or dis video workfile path ... is missing.")
```

Total window: 1 second. On slow systems (loaded CI, heavily
contended I/O, virtualised hosts), the child processes can miss
this window — the result is a `RuntimeError` or, worse, a hang
when the eventual open(2) blocks on a pipe that was never created.

Netflix upstream PR
[#1376](https://github.com/Netflix/vmaf/pull/1376) — "Fix fifo
hangs" — replaces the polling loop with a
`multiprocessing.Semaphore`. Each child releases the semaphore
after creating its pipe; the parent acquires with a 5-second
soft-timeout warning, then blocks indefinitely. PR is OPEN
upstream as of 2026-04-24; the fix is uncontroversial and fork-
applicable unchanged.

## Decision

Port Netflix #1376 verbatim into the fork's Python harness, with
two carve-outs:

1. **Do not bump `python/vmaf/__init__.py:__version__`** from
   `"3.0.0"` to `"4.0.0"`. The fork tracks its own versioning
   (`v3.x.y-lusoris.N` — see
   [ADR-0025](0025-copyright-handling-dual-notice.md)); an API
   version bump is not in this PR's scope.
2. **Drive-by cleanup**: remove `from time import sleep` from both
   touched files, since the polling loop is the sole user and
   ADR-0141 (touched-file lint-clean) requires touched files to
   pass ruff `F401`.

The port covers:

- **`python/vmaf/core/executor.py`**
  - Base `Executor` class (fork lines ~309+): delete
    `_wait_for_workfiles` + `_wait_for_procfiles`; rewrite
    `_open_workfiles_in_fifo_mode` + `_open_procfiles_in_fifo_mode`
    to pass a `multiprocessing.Semaphore(0)` into both spawned
    processes and `acquire` it twice (once for each child), with a
    5-second soft-timeout warn on the first acquire.
  - Add `open_sem=None` kwarg to `_open_ref_workfile`,
    `_open_dis_workfile`, the `_open_workfile` staticmethod,
    `_open_ref_procfile`, `_open_dis_procfile`. Each calls
    `open_sem.release()` after its `mkfifo(...)` (or the new
    touch-file fallback in the non-fifo branch of
    `_open_workfile`).
  - `ExternalVmafExecutor`-style subclass (fork lines ~1107+,
    which operates on `dis` only): delete its
    `_wait_for_workfiles` + `_wait_for_procfiles` overrides;
    rewrite its two fifo-mode openers with the single-process
    Semaphore pattern (one `acquire`, inside the timeout-else
    branch).
- **`python/vmaf/core/raw_extractor.py`** — two no-op overrides:
  - `AssetExtractor`: `_open_ref_workfile` + `_open_dis_workfile`
    take `open_sem=None` and release it if non-None. Delete the
    `_wait_for_workfiles` no-op override (the parent no longer
    calls it).
  - `DisYUVRawVideoExtractor`: `_open_ref_workfile` takes
    `open_sem=None` and releases. Delete the `_wait_for_workfiles`
    override that previously polled for `dis_workfile_path`.

Preserve upstream's warning-message wording verbatim, including
the upstream typo "to be created to be created" in the subclass
variant (an inline comment notes the typo so a future reader
doesn't silently "fix" it).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| **Cherry-pick upstream `1c06ca4f` verbatim** | One-shot `git cherry-pick`; traceable to upstream | Includes the `__version__ = "4.0.0"` bump the fork cannot accept; fork-local executor.py has diverged enough that a clean 3-way merge was uncertain | Rejected — hand-port preserves the two carve-outs (version + ruff F401) cleanly |
| **Reject upstream's unreleased PR; keep the polling loop** | Zero code churn | Users running the Python harness on slow/loaded systems hit the hang; the bug is real | Rejected — the fix is small and self-contained |
| **Replace polling with `multiprocessing.Event` instead of Semaphore** | Slightly lighter-weight primitive | Upstream chose Semaphore because the parent needs to count two releases (ref + dis); `Event` requires either two Events or a counter anyway; upstream's shape is simpler | Rejected — mirror upstream's primitive choice to minimize rebase delta |
| **Keep the 1-second polling but make the window configurable** | Minimal diff | Dodges the real race; still prone to flake on busy CI | Rejected — band-aid, not a fix |

## Consequences

- **Positive**:
  - Eliminates the FIFO race under load. The parent now waits
    exactly as long as the child needs, no more and no less.
  - Aligns fork with upstream's direction; future upstream sync
    will merge cleanly on the touched hunks.
  - Ruff F401 cleanup on touched files: `from time import sleep`
    dropped from both `executor.py` and `raw_extractor.py`.
- **Negative**:
  - `multiprocessing.Semaphore` requires a forkable interpreter,
    which Python 3 provides universally on POSIX. On Windows the
    "spawn" start-method means slightly heavier child startup,
    but this was already the case — upstream tested the change on
    Linux.
  - The 5-second soft-timeout warning will log once on every
    slow-disk system that wasn't failing before. Benign but
    visible. Not a regression.
- **Neutral / follow-ups**:
  - Upstream PR #1376 is still OPEN. Revisit on the next
    `/sync-upstream` pass and re-diff against upstream's final
    merged form; the path will likely be conflict-free at the
    hunk level because the fork now carries the same shape.
  - Netflix #1376 also adds an `else: with open(workfile_path,
    'wb'): pass` touch-file branch in `_open_workfile`. This
    makes the non-FIFO path create an empty file before the
    semaphore-release, which is more defensive but technically
    a behavioural change (previously the non-FIFO path only
    wrote via the actual frame dumper). Preserved verbatim.

## References

- Upstream PR:
  [Netflix/vmaf#1376](https://github.com/Netflix/vmaf/pull/1376),
  head `1c06ca4f1bb5da38b54db075a27c35ba8ea9d7b7`.
- Backlog: `.workingdir2/BACKLOG.md` T4-7.
- [ADR-0025](0025-copyright-handling-dual-notice.md) — fork
  versioning policy (why we skip the `__version__` bump).
- [ADR-0141](0141-touched-file-cleanup-rule.md) — touched-file
  lint-clean rule (why we drop `from time import sleep`).
- User direction 2026-04-24 popup: "Port to both class
  hierarchies (Recommended)".
