<!-- markdownlint-disable MD060 -->
# ADR-0316: cli_parse — handle long-only options in `error()`

- **Status**: Accepted
- **Date**: 2026-05-06
- **Deciders**: lusoris, Claude
- **Tags**: cli, security, fork-local, fuzzing

## Context

The libFuzzer harness expansion in PR #408 (ADR-0311 / Research-0083)
surfaced a real assertion crash in `core/tools/cli_parse.c`. The
handlers for the long-only options `--threads` (`ARG_THREADS`),
`--subsample` (`ARG_SUBSAMPLE`) and `--cpumask` (`ARG_CPUMASK`) called
`parse_unsigned(optarg, 't' / 's' / 'c', argv[0])` with a hardcoded
short-option char. When `optarg` failed to parse, `parse_unsigned()`
called `error()`, which walks `long_opts[]` looking for a matching
`val` and trips `assert(long_opts[n].name)` because none of the chars
`'t'` / `'s'` / `'c'` appears as a `val` in that table — those options
are long-only. Result: any non-numeric `--threads`, `--subsample`, or
`--cpumask` value (including `--th=foo` via getopt's unique-prefix
abbreviation) crashed the binary with `SIGABRT` instead of emitting a
clean usage error and exiting with status 1.

The fuzz harness (`core/test/fuzz/fuzz_cli_parse.c`) carried an
early-reject filter (`known_assert_in_input`) suppressing this bug
class so the nightly stayed green; the parked reproducer is
`cli_parse_known_crashes/cli_threads_abbrev_assert.argv`. ADR-0311
made the follow-up fix explicit; this ADR records the fix.

## Decision

We will pass the long-only enum value (e.g. `ARG_THREADS`) — not a
synthesised short-option char — into `parse_unsigned()` at the three
buggy call-sites. `error()` already handles long-only options
correctly via its `long_opts[n].val < 256` branch; feeding it the real
`ARG_*` value lets the table-walk find the entry and emit `--threads`
(rather than `-t/--threads`) in the usage line. The same shape is
already used for `ARG_GPUMASK`, `ARG_FRAME_CNT`, `ARG_FRAME_SKIP_REF`,
`ARG_FRAME_SKIP_DIST`, `ARG_SYCL_DEVICE`, `ARG_VULKAN_DEVICE`, and
`ARG_TINY_THREADS`, so this brings the three offending lines into
parity with their siblings.

The fuzzer's `known_assert_in_input` filter is removed in the same
commit, and the parked reproducer is promoted from
`cli_parse_known_crashes/` to `cli_parse_corpus/` so the nightly
fuzzer carries it as a permanent regression seed.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Pass `ARG_*` enum value to `parse_unsigned()` (chosen)** | 3-line diff; brings the three call-sites into parity with the seven sibling handlers that already do this; no API change to `error()`. | None. | Chosen — minimum-diff fix that exploits an already-correct branch in `error()`. |
| Extend `error()` with an explicit long-option-name parameter | Self-documenting; future-proofs against new long-only call-sites. | Touches every existing call-site, expands the function signature, and the `long_opts[n].val < 256` branch in `error()` already does the right thing. | Larger diff with no extra correctness over the chosen variant. |
| Wrap `parse_unsigned()` with a `parse_unsigned_long()` trampoline | Surfaces "long-only" intent at the type level. | Two parallel parsers to maintain; the underlying `error()` already supports both shapes via one parameter. | Unjustified API surface for a 3-character bug. |

## Consequences

- **Positive**: invalid `--threads` / `--subsample` / `--cpumask`
  arguments now emit a clean `Invalid argument "..." for option
  --threads` line and `exit(1)`; no more `SIGABRT` from a parser
  assertion. The fuzz harness regains coverage of the
  `--th*` / `--s*` / `--c*` abbreviation prefixes that were
  previously rejected at the harness boundary.
- **Negative**: none. The visible error message changes for these
  three options — it now reads `--threads` instead of `-t/--threads`
  — but no real binary ever printed the latter, since the
  short-option char did not exist in `long_opts[]`.
- **Neutral / follow-ups**:
  - Promoted `cli_threads_abbrev_assert.argv` from
    `cli_parse_known_crashes/` to `cli_parse_corpus/` so the
    abbreviation-driven path is a permanent regression seed.
  - New unit test `core/test/test_cli_parse_long_only_args.c`
    guards all four shapes (`--threads abc`, `--subsample xyz`,
    `--cpumask qqq`, `--th=foosoxe`) via fork()/waitpid() — POSIX
    only, gated off Windows alongside `test_y4m_411_oob`.

## References

- Parent ADR: [ADR-0311](0311-libfuzzer-harness-expansion.md) —
  introduced the harness that surfaced the crash and parked the
  reproducer.
- Research digest:
  [Research-0083](../research/0083-libfuzzer-harness-expansion-target-survey.md).
- Related: [ADR-0270](0270-fuzzing-scaffold.md) — original libFuzzer
  scaffold.
- Source: `req` — task brief 2026-05-06 ("the libfuzzer harness
  expansion in PR #408 surfaced a real assertion crash in
  cli_parse.c…"; paraphrased to neutral English in §Context).
