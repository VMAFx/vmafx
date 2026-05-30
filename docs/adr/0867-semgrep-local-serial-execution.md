# ADR-0867: Run `semgrep-local` pre-commit hook serially to avoid io_uring exhaustion

- **Status**: Accepted
- **Date**: 2026-05-30
- **Deciders**: lusoris
- **Tags**: `ci`, `pre-commit`, `lint`, `developer-experience`

## Context

The `semgrep-local` pre-commit hook in `.pre-commit-config.yaml` is the
fork's project-rule gate (`.semgrep.yml`) for C / C++ / CUDA / Python
sources. By default pre-commit partitions the candidate file list into
`cpu_count()` batches and spawns one `semgrep` process per batch in
parallel, scaling up to whatever the worker has (32 cores on this dev
machine).

`semgrep` 1.78+ ships an OCaml core (`semgrep-core`) that initialises
an `io_uring` instance per process for async file I/O. Each ring
requires pinned memory under `RLIMIT_MEMLOCK` (Linux default: 8 MB).
When pre-commit fans out 32 concurrent `semgrep-core` processes — as
happens whenever the staged-file set is large enough to produce many
partitions — several of them race against the per-user memlock budget
and die during ring setup with:

```text
Fatal error: Unix_error: Cannot allocate memory io_uring_queue_init
Called from Core_scan.iter_targets_and_get_matches_and_exn_to_errors
```

This surfaces in the pre-commit log as an opaque `exit code 2` with no
stderr, because `--quiet` (on by design — the rule output is what
matters, not the banner) swallows the error trace. The user has to
re-run the hook with verbose flags or instrument pre-commit's
internals to discover the root cause.

The bug bit during the VMAFx rebrand (PR #331, 744 changed files) —
the agent had to commit with `SKIP=semgrep-local` after the hook
fell over. Reproducer on pristine master with this fork's hook
config and any worker with `>=16` cores: stage `>=130` source files
matching `types_or: [c, c++, cuda, python]`, run
`pre-commit run semgrep-local`. The "130" threshold is incidental —
it is the smallest file count that yields enough partitions on a
32-core box to cross the memlock cliff. The real triggers are
`cpu_count()` and `RLIMIT_MEMLOCK`, not the file count.

Direct invocation of `semgrep scan --config=.semgrep.yml --error` on
the full 1110-file matching set succeeds in ~3.3 s — the failure is
purely a concurrency artefact in how pre-commit wraps it.

## Decision

We will set `require_serial: true` on the `semgrep-local` hook in
`.pre-commit-config.yaml`. This forces pre-commit to invoke `semgrep`
exactly once with the full file list, leaving internal parallelism to
`semgrep --jobs auto` (which `semgrep-core` already uses, sharing a
single io_uring across worker threads). Wall-time on the full tree
goes from 3.3 s (parallel-but-broken) to 4.3 s (serial-and-correct);
on the typical single-file commit diff it is unchanged.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| `require_serial: true` (chosen) | One-line YAML change. No environment fingerprint. Works on any worker, any `RLIMIT_MEMLOCK`. semgrep already does internal threading. | Loses pre-commit's fan-out parallelism on multi-batch invocations (we never benefited from it — `semgrep-core` was crashing). | — |
| Raise `RLIMIT_MEMLOCK` in `make hooks-install` | No semantic change to the hook. | Requires root or `prlimit` setup not every contributor has. Fragile across distros. Doesn't help users who clone and `pre-commit install` directly. | Pushes platform setup onto every contributor. |
| Pin semgrep to an older version without io_uring | Avoids the bug entirely. | Loses upstream rule and engine improvements. Pre-1.78 versions miss CVE-relevant rule updates. The fork's `.semgrep.yml` uses 1.78+ syntax. | Regression on rule coverage. |
| Switch to `language: system` + custom xargs wrapper | Fine-grained control over chunk size and concurrency. | Reintroduces the "semgrep not on CI PATH" failure mode the current `language: python` setup avoids. Adds a script to maintain. | Strictly worse than the single-flag fix. |
| Lower `target_concurrency` via env var | Cheap. | Pre-commit doesn't expose a per-hook concurrency knob — `PRE_COMMIT_NO_CONCURRENCY=1` is global and equivalent to `require_serial` for the whole config. | `require_serial` on the one offending hook is the surgical version. |

## Consequences

- **Positive**: `pre-commit run semgrep-local --all-files` and
  large-batch pre-commit runs no longer fail with opaque exit=2 on
  workers with `>=16` cores. PR #331 and the next VMAFx-scale rename
  commit cleanly without `SKIP=semgrep-local`. The error class
  disappears from contributor onboarding.
- **Negative**: pre-commit's parallel partitioning is bypassed for this
  hook. On a hypothetical worker where parallel partitions would *not*
  exhaust memlock and the file set were large enough to saturate
  semgrep's internal threading at single-process level, we forgo a
  theoretical speedup. Measured wall-time on 1110 files rose
  3.3 s -> 4.3 s; on the typical 1-5 file commit it is
  indistinguishable.
- **Neutral / follow-ups**: documented in
  `changelog.d/fixed/semgrep-local-batch-size.md`,
  `docs/research/0867-semgrep-local-iouring-audit-20260530.md`, and
  `docs/rebase-notes.md`. If semgrep upstream switches away from
  `io_uring` (or makes the ring shareable across forks), revisit and
  consider re-enabling partitioned parallelism.

## References

- Issue surfaced in PR #331 (VMAFx rebrand, 744-file commit).
- Pre-commit partitioning logic: `/usr/lib/python3.14/site-packages/pre_commit/xargs.py` `partition()` and `xargs()`.
- semgrep-core error: `Core_scan.iter_targets_and_get_matches_and_exn_to_errors`, `OSS/src/core_scan/Core_scan.ml`.
- Pre-commit `require_serial` docs: <https://pre-commit.com/#hooks-require_serial>
- Source: `req` — user direction to fix the `semgrep-local` silent exit-2 on batches over 130 files (paraphrased: PR #331 had to use `SKIP=semgrep-local` to commit; reproducible on pristine master; direct `semgrep scan` returns 0 on the full changed set, so the bug is in the hook wiring, not in semgrep itself).
