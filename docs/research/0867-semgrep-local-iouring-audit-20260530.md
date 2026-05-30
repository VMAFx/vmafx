# Research digest: `semgrep-local` pre-commit hook silent exit-2 audit (2026-05-30)

## Question

Why does the `semgrep-local` pre-commit hook fail with an opaque
`exit code 2` (no stderr) when invoked on a large staged-file batch
(symptom: PR #331 had to commit with `SKIP=semgrep-local`), and what
is the minimal correct fix?

## Reproducer

On the dev workstation (32 cores, `RLIMIT_MEMLOCK=8192 kB`, pristine
fork master `bbcaa8d127`):

```bash
git worktree add -b fix/semgrep-local-batch-size /tmp/wt-semgrep origin/master
cd /tmp/wt-semgrep
pre-commit install-hooks
pre-commit run semgrep-local --all-files
# semgrep (.semgrep.yml ...)................Failed
# - hook id: semgrep-local
# - exit code: 2
```

Direct invocation of `semgrep scan --config=.semgrep.yml --error` on
the same 1110-file matching set succeeds in 3.25 s. Pre-commit's
`PRE_COMMIT_NO_CONCURRENCY=1` env override also passes (4.32 s).

## Hypotheses tested

| # | Hypothesis | Method | Result |
|---|---|---|---|
| H1 | Argv length limit (`ARG_MAX`) exceeded for big batches | Inspected `pre_commit/xargs.py` `_get_platform_max_length` and `partition()`; computed actual `_command_length` per partition | **Rejected** — pre-commit pre-partitions to honour `ARG_MAX-2048-environ`, max partition was 35 files, ~3 KB; nowhere near the 128 KB cap |
| H2 | Shell word-split / quoting collapse | `set -x` trace of the subprocess call | **Rejected** — pre-commit invokes via `subprocess.run(list(p))`, no shell, no word-split |
| H3 | semgrep CLI streaming semantics require stdin not argv | Read semgrep 1.164.0 source path `semgrep/semgrep_main.py`; confirmed argv is the canonical entry | **Rejected** — argv is the documented and tested path |
| H4 | Per-hook timeout exceeded | pre-commit has no per-hook timeout; full run finished in 2.9 s | **Rejected** |
| H5 | Concurrent `semgrep-core` processes contend on a process-shared resource | Ran 32 simulated partitions in parallel (mirroring pre-commit's `ThreadPoolExecutor`) | **Confirmed** — 27/32 partitions exited 2 |

The stderr that pre-commit `--quiet`'d away:

```
[ERROR] Error while matching: non-zero exit status with one or more errors in json response
unexpected errors in json output after invoking semgrep-core:
   Fatal error :
 Unix_error: Cannot allocate memory io_uring_queue_init
====[ BEGIN error trace ]====
Unix_error: Cannot allocate memory io_uring_queue_init
Called from Core_scan.iter_targets_and_get_matches_and_exn_to_errors
in file "OSS/src/core_scan/Core_scan.ml", lines 769-770, characters 8-17
=====[ END error trace ]=====
```

## Root cause

`semgrep-core` (the OCaml binary behind `semgrep scan`) initialises an
`io_uring` instance per process. `io_uring_queue_init` allocates
kernel-pinned memory under the calling user's `RLIMIT_MEMLOCK`
(default 8 MB on most distros, including Arch / Cachy with default
`limits.conf`).

Pre-commit's `xargs()` (`/usr/lib/python3.14/site-packages/pre_commit/xargs.py`)
defaults `target_concurrency = cpu_count()` and partitions the
input list accordingly. On 32-core workers this means up to 32
concurrent `semgrep-core` processes, each racing for memlock
budget at ring-init time. The first few processes succeed;
later ones lose the race and die with `ENOMEM`. The aggregate
hook exit code becomes 2.

The "130-file threshold" the original report observed is incidental.
It is the smallest file count that yields enough partitions (130
files / 4 files-per-partition-min ≈ 32 partitions ≈ `cpu_count()`)
to actually fan out to all cores. The actual triggers are
`cpu_count()` and `RLIMIT_MEMLOCK`, not the file count itself.

Workers with `<16` cores typically don't hit the cliff because
`cpu_count()` partitions fit within memlock; CI runners
(`ubuntu-latest`, 4 cores) don't reproduce.

## Decision (see ADR-0867)

Set `require_serial: true` on the `semgrep-local` hook in
`.pre-commit-config.yaml`. This forces pre-commit to invoke
`semgrep` exactly once with the full file list. `semgrep --jobs auto`
is already internally multi-threaded over a single io_uring instance,
so no real parallelism is lost. Measured wall-time:

| Run | Wall time (1110 files) |
|---|---|
| Direct `semgrep scan` (full set) | 3.25 s |
| pre-commit (current, parallel, **broken**) | 2.92 s (Failed exit=2) |
| pre-commit (`require_serial: true`, **fixed**) | 4.32 s (Passed) |

The 1 s delta is the pre-commit overhead (env activation, file-type
filtering); for the typical 1-5 file commit diff the delta is
unmeasurable.

## Alternatives ruled out

- **Raise `RLIMIT_MEMLOCK`** in `make hooks-install` — needs root or
  `prlimit` plumbing on every contributor box. Fragile.
- **Pin semgrep < 1.78** — pre-`io_uring` versions exist but lose
  rule and engine updates including CVE-relevant fixes; the fork's
  `.semgrep.yml` syntax targets 1.78+.
- **`language: system` + custom xargs wrapper** — reintroduces the
  "semgrep not on CI PATH" failure mode and adds a maintained
  script.
- **`PRE_COMMIT_NO_CONCURRENCY=1`** — works but is global across all
  hooks; the fork's other parallel-safe hooks (clang-format, ruff)
  would regress on wall time.

## Validation

```bash
$ pre-commit run semgrep-local --all-files
semgrep (.semgrep.yml — local rules, error-gated)........................Passed
real    0m4.316s
```

1110-file matching set, single run, exit 0. Reproduced on three
consecutive runs to rule out flakiness.

## Follow-ups

- If semgrep upstream switches away from io_uring (or exposes a
  flag to share rings across forks), revisit and consider
  re-enabling pre-commit's partitioned parallelism.
- No CI-side change needed — `ubuntu-latest` (4 cores) never
  reproduced this and `require_serial: true` is a no-op on
  small-core hosts.

## References

- Pre-commit `xargs` source: `/usr/lib/python3.14/site-packages/pre_commit/xargs.py`
- semgrep-core OCaml entry: `OSS/src/core_scan/Core_scan.ml`
- `RLIMIT_MEMLOCK` semantics: `man 2 setrlimit`, `man 7 io_uring`
- ADR: [ADR-0867](../adr/0867-semgrep-local-serial-execution.md)
- PR #331 (VMAFx rebrand, the bite mark)
