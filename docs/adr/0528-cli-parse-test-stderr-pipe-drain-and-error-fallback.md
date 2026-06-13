<!-- markdownlint-disable MD013 MD060 -->
# ADR-0528: `test_cli_parse_long_only_args` stderr-pipe drain + `error()` non-fatal fallback

- **Status**: Accepted
- **Date**: 2026-05-18
- **Deciders**: lusoris, Claude (Opus 4.7, 1M-context, picked up from a Sonnet pre-context handoff)
- **Tags**: cli, test, regression, fork-local, bugfix

## Context

`core/test/test_cli_parse_long_only_args.c::test_threads_invalid_optarg_does_not_assert`
has been failing on `master`. It is a regression test for the long-only
short-option synthesis bug closed by ADR-0316 / ADR-0438 — invoking
`vmaf --threads abc` used to trip `assert(long_opts[n].name)` inside
`error()` because the case arm passed the synthesised short-option char
(`'t'`) instead of the long-only enum (`ARG_THREADS`).

The product-code fix (passing the `ARG_*` enum) has been in tree for
several releases. The CLI itself works correctly — invoking
`/usr/local/bin/vmaf --threads abc` from a shell exits cleanly with
`rc=1` and an "Invalid argument" stderr line.

The test still fails because of a **separate, test-only bug** in the
fork/pipe harness:

1. The parent allocated a 4096-byte stderr-capture buffer and stopped
   reading from the pipe once full.
2. `usage()`'s help text has grown past 4 KiB (the fork has added many
   `--tiny-*`, `--vulkan-*`, `--hip-*`, `--metal-*`, `--backend`,
   `--precision`, `--dnn-ep`, `--no-reference`, `--tiny-codec`,
   `--tiny-preset`, `--tiny-crf` flags since the test was written).
3. The child blocks writing to the now-full pipe; once the parent
   closes the read end, the child either gets `SIGPIPE` (signal 13) or,
   on glibc with `--enable-werror`-style aborting stdio, gets `SIGABRT`
   (signal 6) from a write-error inside `vfprintf`. Either way the
   test's `WIFEXITED` check rejects the run.

Independently, the audit of `error()` itself surfaced two
defensive-coding gaps unrelated to the immediate regression but worth
closing in the same change:

- `assert(long_opts[n].name)` uses `<assert.h>`, which has historically
  bitten the fork (`-DNDEBUG` release builds silently skip the check,
  while debug builds raise `SIGABRT` with no recovery path). The
  long-only enum fix makes the assert unreachable in practice, but
  any future short-option-to-`error()` regression (e.g. a new case
  arm that passes a synthesised char) would re-trigger SIGABRT
  instead of a clean usage line.
- `sprintf(optname, ...)` writes into a 256-byte stack buffer with no
  bound. The long-option name plus the `-X/--` prefix currently fits,
  but `clang-tidy`'s `clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling`
  flags it, and `principles.md §1.2 rule 30` bans `sprintf`.

## Decision

Two coordinated changes:

1. **Test harness (`core/test/test_cli_parse_long_only_args.c`)**:
   refactor the parent's pipe reader into a `read_head_drain_tail()`
   helper that captures the first 511 bytes (enough for the
   "Invalid argument …" line) and then drains the remainder of the
   pipe into a 256-byte scratch buffer so the writer never blocks
   or `SIGPIPE`s. Extract the child-side `dup2 + cli_parse + _exit`
   into a `child_parse_via_pipe()` helper to keep `run_parse_expect_usage_error`
   under the `readability-function-size` threshold.
2. **Product code (`core/tools/cli_parse.c::error()`)**: replace
   `assert(long_opts[n].name)` with an explicit `if (!found) { usage(…); return; }`
   fallback so the SIGABRT path is unreachable even if a future
   case arm regresses. Replace the two `sprintf(optname, …)` calls
   with `snprintf(optname, sizeof(optname), …)`. Drop the
   `#include <assert.h>`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Grow the test buffer to 8 KiB and call it done | One-line fix | Bandaid — the help text keeps growing; the next round of flags re-breaks the test | Doesn't address the root cause (parent must drain past its capture window) |
| Use `popen()` / `posix_spawn()` + `execlp(vmaf)` instead of in-process fork | Closer to a real-user repro | Adds a build-time dependency on the locally-built `vmaf` binary in the test path; complicates Windows porting (test is already POSIX-only) | The fork helper is already POSIX-only and adequate; no need to escalate |
| Leave `error()` untouched (test-only fix) | Smaller diff | Misses the defence-in-depth benefit; leaves a banned `sprintf` and a banned `assert` in tree on a touched file | Touched-file cleanup rule (CLAUDE §12 r12) requires lint-clean on every file the PR touches |
| Replace `assert` with `__builtin_unreachable()` | Saves an `if` | Crashes on regression with no diagnostic; defeats the test's purpose | The whole point is to emit a clean usage line on unexpected input |

## Consequences

- **Positive**: `meson test -C build --suite=fast` is green; the
  long-only `--threads` / `--subsample` / `--cpumask` regression
  surface is permanently locked in. `error()` no longer relies on
  `assert()` semantics. `cli_parse.c` is one step closer to
  banned-function-clean.
- **Negative**: The test harness is slightly more complex (two extra
  helpers, ~50 LoC). The drained tail is discarded; if a future test
  needs to inspect the full usage output, it must grow the head buffer
  or re-architect the drainer.
- **Neutral / follow-ups**: None — the `error()` audit is local; no
  upstream port is needed (Netflix `error()` still uses `assert` but
  has no equivalent test harness). The long-only enum fix already
  ported the meaningful behaviour upstream-side via PR #408.

## References

- ADR-0316 — original long-only short-option synthesis fix.
- ADR-0438 — short-option handler coverage invariant.
- `core/test/fuzz/cli_parse_corpus/cli_threads_abbrev_assert.argv` — parked fuzzer reproducer.
- Source: agent dispatch dossier (verbatim user request) — "Fix the
  pre-existing `test_cli_parse_long_only_args::test_threads_invalid_optarg_does_not_assert`
  failure".
