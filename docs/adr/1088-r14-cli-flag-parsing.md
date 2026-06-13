<!-- markdownlint-disable MD013 MD041 MD060 -->

# ADR-1088: CLI flag-parsing hardening — parse_unsigned overflow/negative guards and --help in cli_parse.cpp

- **Status**: Accepted
- **Date**: 2026-06-06
- **Deciders**: Lusoris
- **Tags**: `cli`, `security`, `correctness`

## Context

`parse_unsigned` in both `core/tools/cli_parse.c` and `core/tools/cli_parse.cpp`
used `strtoul()` to parse every unsigned integer flag (`--frame_cnt`,
`--frame_skip_ref`, `--frame_skip_dist`, `--threads`, `--subsample`,
`--cpumask`, `--gpumask`, `--width`, `--height`, `--sycl_device`,
`--hip_device`, `--metal_device`, `--tiny_threads`, `--tiny_crf`).

Two silent mis-parses were present:

1. **Negative strings.** POSIX `strtoul("-1", ...)` returns `ULONG_MAX` via
   unsigned wrapping. The `*end == '\0'` guard passes, so `--frame_cnt -1`
   silently stored `UINT_MAX` in `settings->frame_cnt`. The downstream loop
   `for (i = 0; i < frame_cnt; i++)` then ran for 4 294 967 295 iterations
   (or until the input stream was exhausted), which is not what the user intended
   and is a robustness issue under fuzzing (surfaced in the r14 audit).

2. **32-bit overflow on 64-bit hosts.** `strtoul("5000000000", ...)` on a 64-bit
   platform returns `5 000 000 000` without setting `errno`, which is larger than
   `UINT_MAX`. The subsequent `static_cast<unsigned>` / `(unsigned)` truncation
   wrapped silently to `705 032 704`, a completely different value. The fix checks
   `ul > UINT_MAX` in addition to `errno == ERANGE`.

Additionally, `cli_parse.cpp` (the production binary, compiled per ADR-0809)
was missing the `--help` flag that `cli_parse.c` (used only by unit tests) had.
Running `vmaf --help` with the compiled binary fell into `getopt`'s unknown-option
path, which silently broke out of the parse loop, then immediately failed the
mandatory `--reference` check and printed a confusing error rather than the help
text.

## Decision

Add two guards to `parse_unsigned` in both files:

- Reject any `optarg` whose first character is `'-'` (catches all negative
  strings before `strtoul` touches them).
- After `strtoul`, check `errno == ERANGE || ul > UINT_MAX` to catch values
  that exceed `unsigned` range on 64-bit hosts.

Both error paths call `error()` → `usage()` → `exit(1)`, consistent with every
other bad-input path in the parser.

Port the `--help` / `ARG_HELP` entry from `cli_parse.c` to `cli_parse.cpp`
(enum value, `long_opts[]` row, `case ARG_HELP:` arm in the switch, usage text
line). The usage text path is the existing `[[noreturn]]` `usage(argv[0], nullptr)`
call already present for `--version`.

Add five regression tests in `test_cli_parse_long_only_args.c` using the
existing `fork()`/`waitpid()` fixture (ADR-0316 pattern) to verify that
negative and overflow inputs produce a clean `exit(1)` rather than silent
`UINT_MAX` acceptance.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Leave strtoul, add only errno check | Simpler | Still accepts "-1" on platforms where strtoul of negative wraps without ERANGE | Incomplete fix — POSIX is explicit that strtoul("-1") is ULONG_MAX without ERANGE |
| Switch to `strtol` + signed check | Symmetrical with signed types | Returns `long`; needs extra signedness dance; doesn't match the unsigned semantic callers expect | More complex with no benefit — leading-'-' check achieves the same at less code |
| Clamp instead of reject | User-friendlier | Silent semantic change; contradicts "correctness first" rule | Rejected per ADR-0108 correctness principle |

## Consequences

- **Positive**: `--frame_cnt -1`, `--frame_cnt 5000000000`, `--threads -1` etc.
  all produce a clear `"Invalid argument … should be an integer in [0, 2^32-1]"`
  message and `exit(1)`, instead of silently running with a nonsensical frame count.
  The `vmaf --help` shortcut now works in the production binary.
- **Negative**: Callers that previously passed e.g. `--frame_cnt 4294967296` and
  accidentally relied on wrap-to-0 behaviour will now get a usage error. This is
  intentional.
- **Neutral**: No public C API change. No model or score format change. Both
  `cli_parse.c` (test target) and `cli_parse.cpp` (production binary) are updated
  in lock-step so the test suite covers the same logic as the shipped binary.

## References

- r14 audit mandate: "every CLI flag has consistent type validation;
  --frame_range parsing handles edge cases (negative, swap, overflow)"
- ADR-0316 (fork/waitpid test pattern for usage-error assertions)
- ADR-0523 (banned-function audit — `strtoul` without `errno` check is the analogous
  pattern to `assert` without guard)
