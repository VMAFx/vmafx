<!-- markdownlint-disable MD013 MD060 -->
# ADR-1321: Pre-RC1 CI flake remediation and contract alignment

- **Status**: Accepted
- **Date**: 2026-09-25
- **Deciders**: Lusoris
- **Tags**: `ci`, `testing`, `flaky`, `clang-tidy`, `fuzzing`, `fork-local`

## Context

Ahead of the vmafx RC1 release milestone (Issue #1236), a targeted audit examined
the repeatedly failing scheduled workflows and the historical test-quarantine
records. The audit distinguished a deterministic configuration defect from an
intermittent timeout and stale historical prose:

1. **`nightly.yml` whole-tree clang-tidy scan failure**:
   The `clang-tidy-full` job failed deterministically on every master run (15/15 consecutive
   runs). While the PR `clang-tidy` gate in `lint-and-format.yml` was previously hardened to
   use `CC=gcc-15 CXX=g++-15`, `-Db_lto=false`, and LLVM 22 `clang-tidy-22`, `nightly.yml`
   remained on `scripts/setup/ubuntu.sh` defaults without `-Db_lto=false`. Because project
   defaults enable `b_lto=true` with `b_lto_threads=4` (rendering `-flto=4`), Clang's parser
   rejected the option while parsing the compilation database (`unsupported argument '4' to
   option '-flto='`). The latest uploaded report classified 295 of 309 translation units as
   compile failures, making the measurement unusable.

2. **`fuzz.yml` and `sanitizers.yml` runner timeouts**:
   Nightly libFuzzer smoke runs intermittently timed out at GitHub's 15-minute job ceiling.
   Unlike lightweight input harnesses (`fuzz_y4m_input`, `fuzz_yuv_input`), `fuzz_cli_parse`
   links against the full `libvmaf` C/C++ engine. Installing LLVM 22, building `libvmaf`
   with AddressSanitizer, and running the seed corpus takes 11–16 minutes in the inspected
   hosted jobs. Under runner IO and network jitter, run 35981061040 recorded
   `fuzz_cli_parse` as cancelled after 16m36s, while run 35842763631 passed in 11m35s.

3. **Stale ADR-1093 status**:
   `docs/adr/1093-disable-recurring-flaky-tests.md` documented temporary `should_fail: true`
   annotations for `test_pic_preallocation` and `test_sycl_motion_add_uv_parity`. Both root
   causes were subsequently resolved (ADR-1099 for SYCL motion add-UV and PR #840 / commit
   `cf3ff8a8c` for picture pool C++23 CUDA struct alignment), and `should_fail: true` was
   permanently removed from `core/test/meson.build`. However, ADR-1093's header was still
   listed as `Accepted` rather than `Superseded`.

## Decision

1. **Align `nightly.yml` toolchain and flags**:
   Update `clang-tidy-full` in `.github/workflows/nightly.yml` to mirror the required
   CPU ratchet lane in `lint-and-format.yml`: install `gcc-15`, `g++-15`, and
   `clang-tidy-22` from the same configured repositories, configure Meson with
   `CC=gcc-15 CXX=g++-15 -Db_lto=false`, and execute `tidy-ratchet.py` with
   `--clang-tidy /usr/bin/clang-tidy-22`.

2. **Increase fuzz job timeout budget**:
   Raise `timeout-minutes` from 15 to 30 in `.github/workflows/fuzz.yml` (job `fuzz`) and
   `.github/workflows/sanitizers.yml` (job `fuzz-nightly`). This accommodates full `libvmaf`
   ASan compilation and toolchain installation on standard runners without flaking.

3. **Formally supersede ADR-1093**:
   Update `docs/adr/1093-disable-recurring-flaky-tests.md` to `Status: Superseded by ADR-1099
   and PR #840 (commit cf3ff8a8c)` with an explicit post-resolution note.

4. **Fail-closed CI contract testing**:
   Extend `scripts/ci/test_fail_closed_ci.py` with regression assertions verifying that
   `nightly.yml` disables LTO, sets `CC=gcc-15 CXX=g++-15`, and invokes `clang-tidy-22`, and
   verifying that both fuzz workflow jobs retain the reviewed 30-minute budget.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Align `nightly.yml` with PR lane** *(chosen)* | Exact toolchain and flag parity; deterministic zero-drift whole-tree ratchet | Requires explicit toolchain installation block in nightly | Prevents compiler-option mismatch from breaking whole-tree scans. |
| **Keep `nightly.yml` on distro defaults** | Shorter workflow step syntax | Distro Clang fails on `-flto=4` and concept requirements; scan remains broken | Fails closed on every run; unviable for release tracking. |
| **Reduce libFuzzer `MAX_TOTAL_TIME`** | Slightly faster fuzz phase | The fuzz phase is only 60s; installation and compilation dominate the inspected jobs | Does not solve hosted-runner timeout jitter. |
| **Leave ADR-1093 as Accepted** | Zero doc edits | Misleading metadata; suggests active `should_fail` annotations remain | Inaccurate project state documentation. |

## Consequences

- **Positive**:
  - `nightly.yml` whole-tree clang-tidy scan parses all translation units without `-flto=4` failure.
  - Nightly fuzz smoke jobs no longer flake on runner compilation latency.
  - Architectural status of resolved tests in ADR-1093 matches repository truth.
  - Pre-commit and CI test suites enforce these configurations fail-closed.
- **Negative**: A genuinely wedged fuzz target can now consume up to 15 additional
  hosted-runner minutes before GitHub terminates it.
- **Neutral / follow-ups**:
  - The targeted evidence is recorded in
    `docs/research/2107-pre-rc1-scheduled-ci-flake-triage-2026-09-25.md`.

## References

- Issue #1236 (Milestone: RC1 Readiness)
- ADR-1093 (Disable recurring flaky tests via should_fail): [1093-disable-recurring-flaky-tests.md](1093-disable-recurring-flaky-tests.md)
- ADR-1099 (SYCL motion add-UV parity fix): [1099-sycl-fsycl-link-propagation.md](1099-sycl-fsycl-link-propagation.md)
- ADR-1142 (Whole-tree clang-tidy debt ratchet): [1142-whole-codebase-standards.md](1142-whole-codebase-standards.md)
- Research-2107 (Pre-RC1 scheduled-CI flake triage):
  [../research/2107-pre-rc1-scheduled-ci-flake-triage-2026-09-25.md](../research/2107-pre-rc1-scheduled-ci-flake-triage-2026-09-25.md)
- Source: `req` (fix everything before performance tuning and retraining)
