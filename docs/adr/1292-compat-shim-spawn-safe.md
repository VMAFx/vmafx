<!-- markdownlint-disable MD013 MD060 -->
# ADR-1292: Resolve the `vmaf` compatibility shim by file location instead of re-import

- **Status**: Proposed
- **Date**: 2026-09-22
- **Deciders**: VMAFx maintainers
- **Tags**: `python`, `testing`, `concurrency`

## Context

`python/vmaf/__init__.py` is the compatibility shim that keeps legacy
`import vmaf` working after the package moved to `compat/python-vmaf/`
(ADR-0700). It redirected by mutating the import system: insert `compat/`
into `sys.path` if absent, delete itself from `sys.modules`, then re-import
its own name and let path resolution find `compat/vmaf` instead.

That redirect is correct only while `compat/` precedes the shim's own parent
directory on `sys.path`, and the "if absent" guard silently assumes the two are
the same condition. They are not. A directory can be present on `sys.path` and
still lose.

ADR-1278 moved the FIFO workfile and procfile helpers in
`compat/python-vmaf/core/executor.py` onto an explicit `spawn` context. A spawn
child is a fresh interpreter: `multiprocessing.spawn.prepare()` restores the
parent's `sys.path` and then unpickles the bound method, which imports `vmaf`
from scratch. Under pytest the restored path is

```text
['…/python', '…/compat', '…/python', …]
```

so `python/` wins, the shim loads, its guard sees `compat/` already present and
skips the insert, and the re-import resolves back to the shim. The child
recursed until `RecursionError` and died before reaching
`open_sem.release()`. The parent sat in `Executor._open_workfiles_in_fifo_mode`
on an unconditional `sem.acquire()` with no timeout, which never returns.

This is what hung `Ubuntu gcc` and `Ubuntu clang`: the last test output was
`quality_runner_test.py`, then 62 minutes of silence until the job was
cancelled. `python/test/raw_extractor_test.py` is the next file, and its first
test is the first `fifo_mode` consumer after that point. Reproduced locally:
the file never completes; with this change it is `4 passed in 1.44s`.

The parent process does not recurse because the first `import vmaf` succeeds
while `sys.modules['vmaf']` is still empty and the shim's insert therefore
happens. The defect is invisible to every in-process test and appears only in a
freshly started interpreter — that is, only under `spawn`, which is why master
is unaffected: Python 3.14 defaults to `forkserver` on Linux, whose children
inherit `sys.modules` and never re-import.

## Decision

Load the real package directly with
`importlib.util.spec_from_file_location(__name__, compat/vmaf/__init__.py,
submodule_search_locations=[compat/vmaf])`, publish the resulting module as
`sys.modules['vmaf']` before executing it, and let the import machinery pick
that object up. The shim never re-enters the import system for its own name, so
no ordering of `sys.path` can route the redirect back to itself.

`compat/` is still inserted into `sys.path` when absent, for legacy callers that
reach for sibling packages there. Submodule resolution no longer depends on it:
`vmaf.__path__` is pinned to the real package directory by the spec. A missing
`compat/vmaf/__init__.py` now raises `ImportError` naming the path, instead of
recursing.

Observable identity is unchanged: `vmaf.__file__` and `vmaf.__path__` still
point at `compat/vmaf`, submodules keep the `vmaf.core.executor` module name
that pickle records, and `VmafConfig.root_path()` returns the same directory.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Move `compat/` to the front of `sys.path` unconditionally instead of only when absent | One-line change; keeps the re-import strategy | Still order-dependent, and still recurses forever rather than failing if `compat/vmaf` is ever missing; reorders `sys.path` for the whole process as a side effect of an import | Fixes this instance, not the class |
| Add a re-entrancy guard (a sentinel on `sys` or in `sys.modules`) around the re-import | Turns the hang into an error | Keeps the fragile mechanism and adds a second piece of global state to carry it | Detects the failure instead of removing it |
| Revert ADR-1278's `spawn` context back to `fork` / default | Restores the previously passing behaviour | Reinstates the unsafe-fork warning ADR-1278 removed, and leaves the shim broken for every other `spawn` caller | Trades a fixed defect for an unfixed one |
| Delete the shim and require `compat/` on `PYTHONPATH` | Removes the problem outright | Breaks `import vmaf` for existing callers, and the pytest `pythonpath` ini entry, mid-release | Out of scope for a hang fix; can follow once callers migrate |
| Resolve by file location and self-replace in `sys.modules` | Order-independent; no global state; fails loudly when the target is missing | Slightly more code than the original four lines | Chosen |

## Consequences

- **Positive**: the Ubuntu legs complete instead of being cancelled at the
  six-hour job ceiling; any `spawn` child can import `vmaf`; a broken or
  unsupported `compat/vmaf` symlink reports a named `ImportError` rather than
  recursing.
- **Negative**: the shim is longer than the four lines it replaces, and it now
  depends on `compat/vmaf/__init__.py` existing as a file rather than on
  whatever `import vmaf` happens to find.
- **Neutral / follow-ups**: `Executor._open_workfiles_in_fifo_mode` and
  `_open_procfiles_in_fifo_mode` still call `sem.acquire()` with no timeout
  after their warning branch, so any future child that dies before releasing
  the semaphore hangs the parent again rather than failing. That is a separate
  robustness fix, tracked in `docs/state.md`.
- An upstream sync touching `python/vmaf/__init__.py` must preserve the
  file-location load; see `docs/rebase-notes.md`.

## References

- req: "And get it green, what do you mean by not relevant? Are we fixing or
  destroying" — per user direction, no failing leg is written off as
  not-required.
- [ADR-0700](0700-vmafx-repo-layout.md) — the move that created the shim.
- [ADR-1278](1278-python-safe-parallel-execution.md) — the `spawn` context that
  exposed it.
- `docs/research/compat-shim-spawn-recursion-2026-09-22.md` — reproduction and
  measured `sys.path` states.
