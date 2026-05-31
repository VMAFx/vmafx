<!-- markdownlint-disable MD013 -->
# ADR-0438: CLI parser short-option handler coverage invariant

- **Status**: Accepted
- **Date**: 2026-05-15
- **Deciders**: lusoris, Claude (Anthropic)
- **Tags**: `cli`, `lint`, `testing`, `correctness`

## Context

An audit of the `vmaf` CLI (slice A, 2026-05-15) found that
`core/tools/cli_parse.c` declared `short_opts[] = "r:d:w:h:p:b:m:c:o:nvq"`,
advertising `-c` as the short form of `--cpumask`, but the `switch` statement in
`cli_parse()` had only `case ARG_CPUMASK:` (the long-option enum value 264) with no
corresponding `case 'c':`. As a result `getopt_long` correctly consumed `-c <value>`
from the command line but the switch fell into `default:` and discarded the parsed
value silently. `docs/usage/cli.md:158` documented `-c` as functional; the code did
not implement it.

The same file already contained a complementary pattern for `--dnn-ep` / `--tiny-device`
where `case ARG_TINY_DEVICE: /* fall through */ case ARG_DNN_EP:` routes both codes to
the same handler block. The fix applies the same fall-through pattern for `'c'` →
`ARG_CPUMASK`.

SEI CERT C MSC11-C requires that every declared code path produces an observable effect
when exercised. A silently-dropped argument violates that requirement and constitutes an
observable correctness defect (user passes `-c 0xff`; the ISA restriction never applies).

## Decision

We will:

1. Add a `case 'c':` arm before `case ARG_CPUMASK:` in `cli_parse()`, using a
   fall-through so both the short and long forms execute the same `parse_unsigned`
   call with the `ARG_CPUMASK` enum key (ensuring `error()` reports the long-option
   name on invalid input, matching the ADR-0316 pattern).
2. Add a code comment near the switch documenting the invariant: every short option
   declared in `short_opts[]` must have a `case` arm in the switch.
3. Add regression tests `test_cpumask_short_opt` in
   `core/test/test_cli_parse.c` that assert `-c 0xff` sets `cpumask == 255` and
   `-c 3` sets `cpumask == 3`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Remove `c:` from `short_opts[]` and drop the short-form from docs | Zero new code; docs become accurate | Removes a documented and useful short flag with no user benefit; breaks scripts that already use `-c` | Unhelpful regression |
| Add the case arm with a duplicate `parse_unsigned` call (no fall-through) | Marginally clearer to a reader who does not know the fall-through idiom | Duplicates code; `error()` would need a different enum or separate `error()` branch for the short-form path | Code duplication; already established fall-through idiom in the same switch |

## Consequences

- **Positive**: `-c <bitmask>` works as documented. The ISA restriction is applied
  when users pass it. The invariant comment prevents a future recurrence.
- **Negative**: None — the change is additive.
- **Neutral / follow-ups**: The `test_cpumask_short_opt` regression test joins the
  `test_cli_parse` binary (no new binary; no change to meson.build required because
  the test file is already included in the existing executable).

## References

- Audit slice A finding F1 (`.workingdir/audit-2026-05-15/A-cli-and-ffmpeg.md`).
- ADR-0316 (`0316-cli-parse-long-only-error-fix.md`) — prior cpumask-adjacent fix
  (SIGABRT on bad `--cpumask` value); established the `ARG_CPUMASK` enum key pattern.
- SEI CERT C MSC11-C — ensure proper alignment/handling of all declared paths.
- `docs/usage/cli.md:158` — the user-facing documentation that claimed `-c` worked.
