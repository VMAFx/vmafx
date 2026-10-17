---
paths:
  - scripts/ci/prefetch_go_modules.py
  - scripts/ci/tests/test_prefetch_go_modules.py
  - scripts/ci/tests/test_meson_suite_go_prefetch.py
  - scripts/ci/tests/test_sanitizer_suite_timeouts.py
  - scripts/ci/tests/test_meson_suite_checkout_history.py
  - scripts/ci/tests/test_fetch_test_yuvs.py
  - scripts/test/fetch-test-yuvs.sh
  - scripts/codegen/proto_generate.py
invariant: Suite jobs prefetch Go modules as suite user; sanitizer runs triple timeouts; fixtures share one retry policy.
area: tests
---
<!-- markdownlint-disable MD013 MD060 -->
# Meson suite jobs: Go prefetch, sanitizer timeouts, fixture downloads

Three master flakes of 2026-10-10, one page (`docs/development/ci.md`,
"Downloads and timeouts in test jobs"; `docs/development/test-fixtures.md`,
"Download retries").

## Go modules before suite

- Workflow step running `run_meson_test.py` over fast or whole suite needs
  earlier step in same job running `prefetch_go_modules.py`: same `if`, same
  `shell`, same sudo form (`sudo`, `sudo -E`, none). Module cache + tool cache
  are per user; prefetch as runner user fills nothing for suite under `sudo`.
  `test_meson_suite_go_prefetch.py` refuses mismatch.
- `prefetch_go_modules.py`: `go mod download`, then `go install` of pinned buf
  into tool cache. Five attempts each, waits 10/30/60/120 s. No Go on PATH ->
  exit 0 with reason (tests skip themselves, exit 77). Never turn download
  failure into pass or skip.
- `proto_generate.py::buf_command()` runs installed buf only when it reports
  pinned version, else `go run`. Keep prefetch on `go install`: `go run
  <package>@<version>` asks module proxy on every run, full cache or not
  (`module lookup disabled by GOPROXY=off`).
- New Meson test script launching Go: extend prefetch to download its needs,
  then list script in `GO_SCRIPTS` (contract test) and test in `GO_TOOL_TESTS`.
  Completeness proof: empty `GOMODCACHE` + `XDG_CACHE_HOME`, run prefetch,
  then run each test's command with `GOPROXY=off`.
- `suite_commands()` / `runs_suite()` in `test_meson_suite_checkout_history.py`
  = one definition of "step runs suite"; both new contracts load it. It
  reads runner's own arguments only: test name elsewhere in script (deselect
  pattern, comment) must not unclassify step.

## Sanitizer test runs

- Job configuring `-Db_sanitize=<x>` or `-Db_coverage=true`: every
  `run_meson_test.py` command passes `--timeout-multiplier 3`;
  `make test-sanitizers` and `make coverage` use
  `INSTRUMENTED_TEST_TIMEOUT_MULTIPLIER`. One value everywhere
  (`test_sanitizer_suite_timeouts.py::MULTIPLIER`, stated in `ci.md`).
- Changing value: re-measure durations from job logs of all sanitizer legs
  (slowest finished run of every test below third of its timeout), update
  `ci.md` table in same PR. Do not answer sanitizer timeout with per-test
  `timeout:` edits.

## Fixture downloads

- `fetch-test-yuvs.sh` and `download_reactively()` download only through
  `compat/python-vmaf/tools/retrying_download.py` (stdlib only; script form
  run with `python3 -I`). No `curl`/`wget` in script, no `urlretrieve`/`urlopen`
  in `config.py`, no workflow fetching `vmaf_resource/raw/` inline
  (`test_fetch_test_yuvs.py`).
- Waits (5/10/20/40/80/120 s) stay under 600 s per-test timeout of Coverage
  Gate pytest run; raise one only with other.
- Script keeps `main()` + source guard: test sources file to run `main()`
  against stand-in server (`scripts/lib/http_stand_in.py`).
