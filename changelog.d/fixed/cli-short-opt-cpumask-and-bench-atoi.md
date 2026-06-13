- **`vmaf` CLI `-c` / `--cpumask` short option silently dropped.** `cli_parse.c`
  declared `'c'` in `short_opts[]` so `getopt_long` consumed `-c <value>` from the
  command line, but the `switch` statement had only `case ARG_CPUMASK:` (the
  long-option enum value 264) with no `case 'c':` arm. The switch fell into
  `default:` and discarded the parsed value without any diagnostic. Users passing
  `-c 0xff` to restrict CPU ISAs received no effect and no error — equivalent to
  omitting the flag entirely. Fixed by adding `case 'c':` as a fall-through before
  `case ARG_CPUMASK:`, matching the `ARG_TINY_DEVICE` / `ARG_DNN_EP` alias pattern
  already in the same switch. A code comment documents the invariant: every entry in
  `short_opts[]` must have a matching `case` arm. New regression test
  `test_cpumask_short_opt` in `core/test/test_cli_parse.c` asserts that `-c 0xff`
  sets `cpumask == 255` and `-c 3` sets `cpumask == 3`.
  See [ADR-0438](../../docs/adr/0438-cli-parse-short-opt-handler-coverage.md).

- **Banned `atoi()` in `vmaf_bench.c` `--device` parser replaced.** The GPU device
  index parser in `core/tools/vmaf_bench.c` used `atoi(argv[++i])` for the
  `--device N` argument. `atoi` is on the banned-function list in `CLAUDE.md §6` /
  `docs/principles.md §1.2 r30` because it returns 0 silently on invalid input,
  making non-numeric or out-of-range values indistinguishable from a legitimate
  device index of 0. Replaced with `strtol` + explicit `endptr` / bounds checks
  following the `parse_unsigned()` pattern from `cli_parse.c`; invalid values now
  print a clear error to stderr and exit non-zero.
  See [ADR-0438](../../docs/adr/0438-cli-parse-short-opt-handler-coverage.md).

- **`--tiny-model-verify` documented correctly as a boolean flag.** `docs/usage/cli.md`
  incorrectly showed `--tiny-model-verify <path>` with a path argument. The flag is
  defined as `no_argument` in `cli_parse.c`; the model and bundle paths are inferred
  from `--tiny-model`. Documentation updated to remove the spurious `<path>` and
  clarify the flag enables Sigstore verification mode.
