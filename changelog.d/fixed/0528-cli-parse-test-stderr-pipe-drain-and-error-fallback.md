# fix(cli): unbreak `test_cli_parse_long_only_args::test_threads_invalid_optarg_does_not_assert` + harden `error()` (ADR-0528)

`core/test/test_cli_parse_long_only_args.c`'s `test_threads_invalid_optarg_does_not_assert`
(regression test for ADR-0316 / ADR-0438) was failing on master even though
the product code itself was correct — invoking `vmaf --threads abc` from a
shell exits `rc=1` with an "Invalid argument" diagnostic.

The failure was a test-only bug. The fork-harness parent allocated a 4 KiB
stderr buffer and stopped reading once full; `usage()`'s help text has grown
past 4 KiB so the child blocked writing into the now-full pipe, and once the
parent closed its end the child took SIGPIPE (signal 13) or SIGABRT (signal 6
via aborting stdio in `vfprintf`). Either way the test's `WIFEXITED` check
rejected the run.

Fix:

- Test (`core/test/test_cli_parse_long_only_args.c`): extract a
  `read_head_drain_tail()` helper that captures the first 511 bytes (enough
  for the "Invalid argument …" needle that always precedes the usage block)
  and then drains the remainder into a scratch buffer so the writer never
  blocks or `SIGPIPE`s. Extract the child-side `dup2 + cli_parse + _exit`
  into `child_parse_via_pipe()` for `readability-function-size` headroom.
- Product (`core/tools/cli_parse.c::error()`): replace
  `assert(long_opts[n].name)` with an explicit `usage()` fallback (banned
  macro per `principles.md §1.2 rule 30`; `-DNDEBUG` would silently no-op
  the check anyway). Replace two `sprintf(optname, …)` calls on a 256-byte
  scratch buffer with `snprintf`. Drop `#include <assert.h>`.

CLI behaviour for end users is unchanged; the defence-in-depth in `error()`
just removes a residual SIGABRT path that any future regression of the
long-only enum fix would re-trigger.
