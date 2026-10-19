<!-- markdownlint-disable MD013 MD060 -->
# ADR-2199: actionlint runs under a deadline and fails loudly when it hangs

- **Status**: Accepted
- **Date**: 2026-10-07
- **Deciders**: lusoris
- **Tags**: `ci`, `hooks`, `tooling`

## Context

The `actionlint` pre-commit and pre-push hook hung, without output or exit, on
about every other push of 2026-10-07 on the development workstation (six push
retries of one pull request). The process sat in `futex_wait` with no child.

A goroutine dump (`SIGQUIT`) of a hung run shows `main` waiting in
`LintFiles` for a worker that is blocked in `os.(*File).Write` at
`process.go:32`, called from `cmdExecution.run`. actionlint v1.7.12 writes the
script of a `run:` block to the child's stdin pipe before it starts the child
(`cmd.StdinPipe()`, `io.WriteString`, then `cmd.Output()`), so the write can
only complete if the whole script fits in the pipe; when it does not, nothing
reads and nothing starts. A pipe holds 64 KiB, but the kernel shrinks new pipes
of a user who holds more than `fs.pipe-user-pages-soft` pages in total (16384, 64
MiB, by default) to two pages. With many builds and agents running at once the
workstation is over that limit, and a `run:` script of 8 KiB or more hangs the
hook for as long as the pressure lasts.

Reproduced: a process holding 1100 to 2500 pipes (a new pipe then holds 8192
bytes) makes `actionlint .github/workflows/lint-and-format.yml
.github/workflows/rule-enforcement.yml` hang; without them it takes 0.1 s. A
loop of the hook's parallel invocation (four files per process) hung two runs of
six.

The defect is upstream, already reported as rhysd/actionlint#702 (which names the
pipe limit), #704 and #712 (#650 is the closed macOS report); the version this
repository pins has no fix.

The maintainer decided (Q-078, 2026-10-07): fix it here so that it cannot hang,
bounded and loud, never a silent pass, proven with a planted hang.

## Decision

We will run actionlint through `scripts/ci/run_actionlint.py`, in the pre-commit
hook (`entry:` overridden in `.pre-commit-config.yaml`) and in `make
lint-actions`:

- it runs actionlint under a deadline, `ACTIONLINT_TIMEOUT_S`, default 90 s (a
  healthy run takes about a second), through `scripts/lib/safe_subprocess.py`,
  which terminates the whole process group on the deadline;
- on the deadline it sends `SIGQUIT`, which makes the Go program print the stack
  of every goroutine, saves that dump to a file it names
  (`ACTIONLINT_DUMP_DIR`, default the temporary directory; a hook's captured
  output is gone after the run, and the stack is what an upstream report needs),
  terminates the process group, prints why (the upstream cause, and the capacity
  of a pipe created now: below 64 KiB it names the pipe limit as the cause) and
  exits 124, a failure; it never reports a pass for a run that did not finish;
- every other outcome is actionlint's own output and exit status; a missing
  actionlint is exit 127.

The pin stays at v1.7.12 until upstream fixes the write; the wrapper is then a
bounded run and can stay.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Disable actionlint's shellcheck integration (`-shellcheck=`) | No pipe write, no hang | The `run:` blocks of every workflow lose shellcheck | A lint hole for a flake |
| Raise `fs.pipe-user-pages-soft` on the workstation | Removes the cause here | A host setting, not in the repository; CI and other machines unchanged | Not reproducible from the tree |
| Retry on a hang | Passes when the pressure passes | Waits the full deadline each time and still fails under sustained pressure | A retry hides the cause; the failure names it |
| Patch and vendor actionlint | Fixes the bug | A fork of a pinned third-party tool (HISS-11) | Upstream owns the fix |
| Do nothing | No change | Every push can hang for as long as another process leaks pipes | The reason for this ADR |

## Consequences

- **Positive**: a hang ends in 90 s with the cause named instead of hanging a
  push for good; the hook cannot pass without actionlint having finished.
- **Negative**: under sustained pipe pressure the hook still fails (loudly)
  when a workflow has a `run:` script larger than the shrunken pipe; the fix is
  to find the process holding the pipes.
- **Neutral / follow-ups**: remove the wrapper's rationale when upstream ships
  the fix and the pin moves; the planted-hang test stays.

## References

- `Q-078` (maintainer decision, 2026-10-07): investigate and fix the
  actionlint pre-push hook deadlock as its own pull request; bounded, loud,
  proven with a planted hang; report upstream where the defect lives.
- rhysd/actionlint#702, #704, #712, #650.
