- **`vmaf` CLI assertion crash on bad `--threads` / `--subsample` /
  `--cpumask` arguments.** Three handlers in
  `core/tools/cli_parse.c` passed a synthesised short-option char
  (`'t'` / `'s'` / `'c'`) into `parse_unsigned()` for the long-only
  `ARG_THREADS` / `ARG_SUBSAMPLE` / `ARG_CPUMASK` codes. When `optarg`
  failed to parse, `error()` walked `long_opts[]` looking for that
  char, found nothing (the options are long-only), and tripped
  `assert(long_opts[n].name)` — taking the binary down with `SIGABRT`
  instead of the intended clean usage error. Triggered by any
  non-numeric value, including the abbreviated `--th=foo`,
  `--sub=foo`, `--cp=foo` shapes (getopt unique-prefix matching).
  Surfaced by the libFuzzer harness landed in PR #408 (ADR-0311);
  reproducer was parked at
  `core/test/fuzz/cli_parse_known_crashes/cli_threads_abbrev_assert.argv`
  with an early-reject filter in the harness suppressing the bug
  class. Fix passes the long-only enum value
  (`ARG_THREADS` / `ARG_SUBSAMPLE` / `ARG_CPUMASK`) into
  `parse_unsigned()` so `error()` finds the matching `long_opts[]`
  row via its existing `long_opts[n].val < 256` branch, bringing the
  three call-sites into parity with the seven sibling handlers
  (`ARG_GPUMASK`, `ARG_FRAME_*`, `ARG_*_DEVICE`, `ARG_TINY_THREADS`).
  New POSIX-only regression test
  `core/test/test_cli_parse_long_only_args.c` drives `cli_parse`
  via `fork()`/`waitpid()` for `--threads abc`, `--subsample xyz`,
  `--cpumask qqq`, and `--th=foosoxe`; pre-fix the child died from
  `SIGABRT`, post-fix each case exits with status 1 + `Invalid
  argument` on stderr. Reproducer promoted from
  `cli_parse_known_crashes/` to `cli_parse_corpus/`;
  `known_assert_in_input` early-reject filter removed from
  `fuzz_cli_parse.c`. See
  [ADR-0316](../../docs/adr/0316-cli-parse-long-only-error-fix.md).
