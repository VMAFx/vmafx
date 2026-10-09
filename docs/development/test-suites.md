<!-- markdownlint-disable MD013 MD060 -->
# Test suites and the checks that run them

Every test file in the repository belongs to exactly one **suite**, and every
suite is run by one or more required CI checks.
[`.github/test-suites.json`](../../.github/test-suites.json) records both
mappings, and the required `Tooling Tests` job fails when a test file is not
in any suite ([ADR-1528](../adr/1528-test-suite-registry.md)).

## Adding a test

- **To an existing suite** (a new `test_*.py` under `tools/vmaf-tune/tests/`,
  a new `test-*.sh` under `scripts/ci/tests/`, ...): there is nothing to
  wire. The job that runs the suite picks the file up.
- **In a new directory**: add the directory to a suite's `paths` in
  `.github/test-suites.json`, or add a new suite there with the job that runs
  it. A new job must be in the `required` list of
  [`required-aggregator.yml`](../../.github/workflows/required-aggregator.yml),
  or the check refuses the suite. For a new Python package, follow
  [the Python test orchestrator page](python-test-orchestrator.md#adding-a-new-python-package)
  as well.
- **A file whose name looks like a test but is not one** (a driver script, a
  dataset module) goes under `not_tests`, with a reason.
- **Do not add a CI step that runs a tooling test.** A test runs once in CI
  ([ADR-1568](../adr/1568-tests-run-once-in-ci.md)): Tooling Tests runs every
  file of the tooling suite, and `suite_registry.py check` fails when another
  workflow runs one again. A test that needs a tool only one job installs
  (the pinned helm, for example) gets its own suite naming that job; the most
  specific path in the registry wins, so a single file can leave the tooling
  suite.

Check the registry locally. The pre-commit hook `suite-registry` runs the same
command:

```bash
python3 scripts/ci/suite_registry.py check
python3 scripts/ci/suite_registry.py list tooling   # the files of one suite
```

A test file is any tracked file named `test_*.py`, `test-*.py`, `*_test.py`, `test_*.sh`,
`test-*.sh`, `*_test.sh`, `*_test.go`, `*_test.rs`, `test_*.c`, `test_*.cpp` or
`test_*.cu`, or any `.rs` file in a `tests/` directory. The check also fails
when a suite path or `not_tests` entry no longer matches any file, so the
registry cannot go stale.

## Suites

| Suite | Paths | Required check(s) | How it runs | Skipped in CI, and why |
|---|---|---|---|---|
| `core` | `core/test/`, `core/tools/test/` | `Ubuntu gcc`, `Linux Intel LLVM` | Meson tests (`scripts/ci/run_meson_test.py`); the backend-gated contract tests run in the all-backend `Linux Intel LLVM` leg | Device tests skip without a GPU |
| `python-harness` | `python/test/` | `Coverage Gate`, `Ubuntu gcc` | `pytest python/test/` against the gcov build, after an editable install of `python/` that compiles the Cython extension `cy_test.py` needs (as tox does); `tox -c python` on C-core changes | — |
| `compat` | `compat/python-vmaf/tests/` | `Python Package Tests (compat)` | `pytest compat/vmaf/tests` with `python/requirements-test-lock.txt`; the decorator file also runs on every OS in `build.yml` | — |
| `ai` | `ai/tests/`, `ai/sidecar/tests/` | `Tiny AI` | `pytest` with `ai/requirements-dev-lock.txt`, the job's DNN build as `VMAF_BIN`, the golden YUVs and ffmpeg | One socket test needs root or user namespaces to start a peer with another UID |
| `mcp` | `mcp-server/vmaf-mcp/tests/` | `MCP Smoke` | `pytest` with the dev lock (which carries the `eval` extra), the MCP build as `VMAF_BIN` and the golden YUVs | — |
| `rc1-tester` | `tools/rc1-tester/tests/` | `RC1 Tester Report` | `pytest` with the package's dev lock | — |
| `vmaf-tune` | `tools/vmaf-tune/tests/` | `Python Package Tests (vmaf-tune)` (its own job: it needs `MCP Smoke`) | `pytest` over the files `suite_registry.py list vmaf-tune` prints, with the package's dev lock, MCP Smoke's `vmaf` (artifact `vmaf-cli-mcp`) as `VMAF_BIN_FOR_TESTS` and the golden YUVs; a skip for a missing binary or missing YUVs fails the job | 4: `VMAF_TUNE_INTEGRATION=1` with ffmpeg/x265 (2; opt-in; the two-pass case fails until `T-VMAFTUNE-X265-TWO-PASS-CRF-2026-10-04` closes, PR #2020, ADR-1565), QSV hardware (1) and the BBB corpus (1) |
| `dev-llm` | `dev-llm/tests/` | `Python Package Tests (dev-llm)` | `pytest` with the dev lock, which carries the `modelcard` extra | — |
| `vmaf-roi-score` | `tools/vmaf-roi-score/tests/` | `Python Package Tests (vmaf-roi-score)` | `pytest` with the package's dev lock | — |
| `go` | `api/`, `cmd/`, `internal/`, `pkg/` | `go vet + go test` | `go test ./...` against the CPU + ONNX Runtime libvmaf build | Individual tests skip when a tool they drive is absent |
| `rust` | `bindings/rust/` | `vmafx-sys CI` | `cargo test --workspace --all-features`, which also runs the inline tests of `core/src/feature/rust/tad` | — |
| `helm-chart` | `scripts/ci/tests/test_check_helm_selector_isolation.py`, `test_helm_controller_auth.py`, `test_helm_node_contract.py` | `helm lint + template` | `unittest` in the helm job, which installs the pinned, checksum-verified helm these tests render the chart with; the helm impact selector covers the three files | — |
| `ffmpeg-patches` | `ffmpeg-patches/test/` | `FFmpeg Patch Stack` | `make ffmpeg-input-contract`, which `check_input_contract.py` requires of that job | — |
| `tooling` | `scripts/`, `dev/scripts/`, `testdata/`, `tools/ensemble-training-kit/tests/`, `tools/external-bench/tests/` | `Tooling Tests` | `suite_registry.py run tooling`: one pytest run over the Python files, then each shell file with `bash`, using `requirements/locks/tooling-tests.txt` (pytest, PyYAML, reuse, semgrep, pre-commit, mypy and the docs stack) and the distribution's cppcheck | Three live-build cases of `test_device_target_header_dependencies.py` (the `core` suite runs them under Meson with a build); two 4K cases of `testdata/test_sycl_4k_repeat_determinism.py` (the 4K fixtures are local-only) |

Each pytest call in these jobs passes `-rs`, so the job log names every
skipped test and its reason.

## Not tests

| Path | Why |
|---|---|
| `python/test/resource/` | Dataset definition modules named after their dataset |
| `scripts/ci/run_meson_test.py` | The Meson test runner wrapper ([ADR-1333](../adr/1333-meson-test-secret-env-sanitization.md)) |
| `scripts/test-matrix.sh` | Local driver that runs `make ci` in every `docker/dev` image |
| `testdata/test_all_backends.sh` | Benchmark driver that needs oneAPI, an installed `vmaf` and GPU devices; it asserts nothing |

## Running a suite locally

| Suite | Command |
|---|---|
| `tooling` | `nox -s tooling`, or install `requirements/locks/package-build.txt` and then `requirements/locks/tooling-tests.txt` (`--no-build-isolation`) and run `python scripts/ci/suite_registry.py run tooling` |
| `vmaf_tune`, `dev_llm`, `roi_score`, `mcp`, `ai`, `rc1_tester` | `nox -s <session>` ([orchestrator](python-test-orchestrator.md)) |
| `compat` | In a Python 3.14 venv: install `requirements/locks/package-build.txt`, then `python/requirements-test-lock.txt` (`--no-build-isolation`), and run `pytest compat/vmaf/tests`. The harness lock is Linux-only, so there is no nox session; `nox -s compat_decorator` runs the decorator file alone |
| `rust` | `LIBVMAF_PREFIX=<prefix> LD_LIBRARY_PATH=<prefix>/lib cargo test --workspace --all-features` after installing a CPU libvmaf ([Rust guide](rust.md)) |
| `core` | `python3 scripts/ci/run_meson_test.py -- -C build` |
| `go` | `go test ./...` ([CI overview](ci.md#go-checks)) |

The tooling suite's shell tests expect `git`, `bash`, `cc`, `readelf`,
`patchelf`, `node` and `timeout` on `PATH`, as the hosted Ubuntu runner has.
Docker is optional: the container-source test runs its Docker-backed cases
only when `docker info` succeeds.

## Meson suites of the core: `fast` and `timing`

The `core` suite's Meson tests carry Meson suite labels of their own.
`fast` is the pre-push gate (`make test-fast`) and the merge train's C gate.
`timing` holds the checks of a wall-clock budget, which hold only when
nothing else runs on the host. Today that is the live window harness's
latency budget: a window whose frames are final per frame completes within
two frame periods (33 ms at 60 fps) of the submit of its last frame
(`test_vmafx_window_live_timing`).

- **`fast` stays exact on any host load.** It runs the same harness with
  `VMAFX_TEST_TIMING=0`: every window still equals an offline session bit
  for bit, the textures the library holds stay within the in-flight bound
  and no frame is copied through the host, but the wall-clock budget is
  not asserted. `test_vmafx_live_pacing` checks the pacing and budget
  arithmetic the harness uses (`core/test/vmafx_live_pacing.h`) on a
  virtual clock: frames are due on an absolute schedule that an overrun
  never shifts, a latency runs from the submit of the window's last frame,
  and two frame periods are within the budget while one nanosecond more is
  not. It refuses two planted pacing bugs (a pacer that waits one period
  from now, and a latency taken from a window's first frame).
- **`timing` runs alone.** Run it after the other suites, one test at a
  time, on a host that does nothing else:

  ```bash
  make test-timing
  python3 scripts/ci/run_meson_test.py -- -C build --suite=timing --num-processes 1
  ```

  The tests are `is_parallel: false`, so a full `meson test` run (the
  `Ubuntu gcc` and `Linux Intel LLVM` legs) runs them with no other test
  beside them on the job's runner. A merge train or a local gate runs the
  `timing` suite as its own step after the build and the `fast` suite,
  never next to a build or another gate.
- **`timing` exists only in a plain optimised build.** Coverage counters
  (`-Db_coverage=true`), a sanitizer (`-Db_sanitize=...`) or a build
  without optimisation (`--buildtype=debug`, `-Doptimization=0` or `g`)
  slow the code down, so a budget there measures the instrumentation, not
  the code. Such a build compiles the harness with the budget off and
  registers no `timing` test; its `fast` run keeps every other check. The
  coverage and sanitizer jobs therefore run the harness without the
  budget, and the release-built CPU legs, the merge train and
  `make test-timing` on a default (release) build hold it.
  `test_timing_suite_plain_builds` (`fast`) reads each build's Meson
  introspection and fails if an instrumented build registers a `timing`
  test or a plain optimised Linux build lacks
  `test_vmafx_window_live_timing`. The condition is `vmafx_timing_build` in
  `core/test/meson.build`.

Nothing is loosened: the budget is the same 33 ms, held in the `timing`
suite; the `fast` suite drops only the one assertion that depends on the
host being idle (maintainer decision Q-325,
`T-WINDOW-LIVE-LATENCY-UNDER-LOAD-2026-10-09`). A new check of a wall-clock
bound goes to `timing` the same way, with its arithmetic tested on a virtual
clock in `fast`.

## How a test finds the `vmaf` binary

A Python test that runs the `vmaf` CLI runs the build under test. It takes the
binary from [`scripts/lib/vmaftest.py`](../../scripts/lib/vmaftest.py), which
looks in this order and nowhere else:

1. `VMAF_BIN`;
2. `VMAF_BIN_FOR_TESTS`;
3. `build/tools/vmaf`, `core/build/tools/vmaf`, then `core/build-cpu/tools/vmaf`
   under the repository root (`vmaf.exe` on Windows).

A variable that is set must name an executable file. If it does not, the test
errors and does not go on to the build directories. A relative value is read
from the directory the test run started in. With neither variable set and no
build, a test that needs the binary skips with a message that names both
variables and `meson setup build core && ninja -C build`. In `ai`, `mcp` and
`vmaf-tune` that skip fails the run, both in CI and in
`run_affected_suites.py` (`fail_on_skip`).

The resolver never searches `PATH` and never uses `/usr/local/bin/vmaf`. A test
that picked up a stale host install used to pass while the tree under test was
never run. Point the suites at a build like this:

```bash
VMAF_BIN=$PWD/build/tools/vmaf python3 -m pytest ai/tests/test_e2e_frame_to_score.py
python3 scripts/ci/run_affected_suites.py --base origin/master --head HEAD --vmaf-bin build/tools/vmaf
```

A test that needs a particular build, such as a CUDA test, checks that the
resolved binary can do the job and skips or fails when it cannot. It does not
look for another binary. Name such a build with `VMAF_BIN`. The `mcp` suite's
`conftest.py` sets the server's `VMAF_BIN` to the resolved binary before every
test, so the server's own lookup, which does check `/usr/local/bin` for
installed use, never reaches a host install from a test.
[`scripts/ci/tests/test_tests_use_vmaf_under_test.py`](../../scripts/ci/tests/test_tests_use_vmaf_under_test.py)
(pre-commit hook `tests-use-vmaf-under-test`) fails on a `which("vmaf")` call
in any suite's test files. It also fails when one of them names the host path,
except for the listed files that only pass it as data. The Go tests follow the
same rule through `internal/vmaftest` ([Go development](languages.md)).

## Run the affected suites locally

`scripts/ci/run_affected_suites.py` runs the Python suites a change touches, the
way CI does, before a change lands. Use it after a rebase or before a push, and
as the merge train's Python gate.

```bash
make test-affected BASE=origin/master HEAD=HEAD VMAF_BIN=build/tools/vmaf
python3 scripts/ci/run_affected_suites.py --base <sha> --head <sha> --vmaf-bin build/tools/vmaf
python3 scripts/ci/run_affected_suites.py --files ai/src/vmaf_train/x.py --list   # what would run
```

A suite is affected when a changed file is one of its test files, sits under its
`paths` or `source_paths`, or is a lock file or editable package of its install
spec. A documentation-only change affects nothing and the command exits 0. The
mapping is the registry's own, so a new suite or test directory needs no change
here.

For each affected suite the runner builds a virtual environment once in
`~/.cache/vmafx-suite-venvs/<suite>-<hash of its lock files>`
(`VMAFX_SUITE_VENVS` or `--venv-root` moves it), installs the registry's locks
with `--require-hashes` and, with `uv`, byte-compiles the packages as pip does.
A changed lock gives a new directory, so the next run rebuilds; delete the old
ones by hand when disk matters (`ai` holds torch and is several GB). The
editable packages are re-pointed at the checkout on every run, so one cache
serves every worktree, and a per-venv file lock serialises two runs that share
it. A suite with `venv_of` runs in another suite's venv, as in CI (no suite uses it
since the predictor trainer moved into `ai`, ADR-1886). The tests
run with `pytest -p no:cacheprovider -rs` and the per-test timeout CI uses,
under a 900 s cap per suite (`--time-cap`).

Measured on a loaded 32-core workstation with a warm `uv` wheel cache: a venv
builds in 1 to 7 s (the first build with an empty wheel cache downloads the
locked wheels, which is several GB for `ai`); `ai` runs in about 50 to 80 s,
`vmaf-tune` in 20 s, `mcp` in 9 s, and `tooling`, which runs every script test,
in 5 to 8 minutes, so a change under `scripts/` is the slow case.

A suite fails on a failed test, on a time-cap overrun and on a skip whose reason
is a missing dependency (`could not import`, `No module named`, `not installed`)
or a missing input the suite declares in `fail_on_skip`: the `vmaf` binary and
the Netflix golden YUVs for `ai`, `mcp` and `vmaf-tune`. Pass the binary the train built with `--vmaf-bin` (or
`VMAF_BIN`); the YUVs are `python/test/resource/yuv`. Suites that need a build or
a toolchain (`core`, `python-harness`, `go`, `rust`, `helm-chart`,
`ffmpeg-patches`) carry a `not_local` reason; the runner prints `NOT RUN` for
them and `--strict` turns that into a failure. One line per suite reports
passed, failed and skipped counts and the duration.

The registry fields it reads are `source_paths`, `install` (`python`, `locks`,
`editable` or `venv_of`), `pytest` (`timeout`, `method`, `rewrite`) and
`fail_on_skip`; every suite has an `install` or a `not_local` reason, which
`suite_registry.py check` enforces. The CI jobs still spell their installs in
the workflow; a follow-up makes them read `install` too.

## Pre-commit hooks that run tests

Fifty-one local pre-commit hooks run a test of the tooling suite when a file
they watch changes. They stay active at commit time, where they give the
fastest feedback. The CI `Pre-Commit` job skips them, because Tooling Tests runs the
same files on every pull request and push:
`python scripts/ci/suite_registry.py precommit-skip` names the hooks whose
entry runs only tooling tests, and the job passes that list in `SKIP`. A hook
that runs anything else as well (a check of the live tree, a test of another
suite) keeps running in CI.
