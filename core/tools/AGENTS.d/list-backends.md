---
paths:
  - core/tools/cli_backends.cpp
  - core/tools/cli_backends.h
  - core/tools/cli_parse.cpp
  - core/tools/vmaf.cpp
  - tools/vmaf-tune/src/vmaftune/score_backend.py
  - pkg/scorebackend/scorebackend.go
invariant: vmaf --list-backends is one source of usable backends; both score-backend selectors read it.
---
# `vmaf --list-backends` (ADR-1874)

- `cli_list_backends()` (`cli_backends.cpp`) prints cpu, cuda, sycl, hip,
  metal in that order: `compiled` from `HAVE_*`, `usable` from backend's
  own `vmaf_*_state_init()` on device 0 then free, `init_status` on failure.
  Same initialiser scoring run calls. Never infer from `--help` (it names
  every backend on every build) or from vendor tools.
- Option sets `CLISettings.list_backends`; `cli_parse()` returns right after
  getopt loop, before input validation; `vmaf_cli_main()` prints and
  exits through `CliRunGuard`. Keep both, or `vmaf --list-backends` dies on
  missing reference.
- Readers: `vmaftune/score_backend.py::backend_report()` and
  `pkg/scorebackend.Report()`. new backend in CLI joins `kBackends`,
  `ALL_BACKENDS` / `AllBackends()`, `auto` chains and
  `testdata/score_backend_selection.json` in one PR. Selection cases in that
  table are replayed by both test suites; edit table, not one side.
- Tests: `test_vmaf_list_backends` (order, `compiled` vs build options),
  `test_cli_parse` (`test_list_backends_needs_no_inputs`).
