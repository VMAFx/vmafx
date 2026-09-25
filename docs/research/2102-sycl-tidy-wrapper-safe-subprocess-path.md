<!-- markdownlint-disable MD013 MD024 MD060 -->
# Research-2102: SYCL Clang-Tidy Wrapper Executable Path Resolution for Safe Subprocess

- **Status**: Active
- **Workstream**: [ADR-1142](../adr/1142-whole-codebase-standards.md), [ADR-1270](../adr/1270-bounded-process-execution.md)
- **Last updated**: 2026-09-25

## Question

Why did `make tidy-ratchet LANE=sycl` fail with `CommandValidationError: allowlisted executable must be bare or absolute: scripts/ci/clang-tidy-sycl.sh`, and how do we durably guarantee that the SYCL clang-tidy wrapper can be invoked across arbitrary worktrees and working directories under `safe_subprocess`?

## Sources

- `scripts/lib/safe_subprocess.py`: ADR-1270 bounded subprocess execution boundary.
- `docs/adr/1270-bounded-process-execution.md`: contract requiring allowlisted executables to be bare or absolute.
- `scripts/ci/tidy-ratchet.py`: whole-tree clang-tidy debt ratchet runner.
- `scripts/ci/clang-tidy-sycl.sh`: icpx-aware wrapper injecting SYCL headers and flags.
- `Makefile`: definition of `TIDY_RATCHET_EXTRA_sycl` and `tidy-ratchet` targets.

## Findings

1. **Root cause in `safe_subprocess.py` contract enforcement**:
   ADR-1270 hardened repository subprocess boundaries by validating argument vectors, deadlines, and allowlists. In `_parse_allowed_executables()` and `_resolve_allowed_executable()`, any executable token that contains directory components (`len(candidate.parts) > 1`) and is not absolute is rejected with `allowlisted executable must be bare or absolute: <token>` to prevent working-directory traversal and path ambiguity.

2. **Root cause in `Makefile` and `tidy-ratchet.py`**:
   `Makefile` configured `TIDY_RATCHET_EXTRA_sycl := --clang-tidy scripts/ci/clang-tidy-sycl.sh` as a relative path.
   When `tidy-ratchet.py` ran:
   - `shutil.which(args.clang_tidy)` returned `"scripts/ci/clang-tidy-sycl.sh"` (preserving the relative string).
   - `measure()` called `clang_tidy_version(binary)` at startup.
   - `clang_tidy_version` invoked `run_command([binary, "--version"], allowed_executables=(binary,))` without resolving `binary`.
   - `safe_subprocess` immediately rejected the relative path with `CommandValidationError`, causing `tidy-ratchet.py` to abort with exit code 5 (`measurement/output failed: allowlisted executable must be bare or absolute: scripts/ci/clang-tidy-sycl.sh`).
   - Although `run_one()` previously contained an ad-hoc resolution check (`if "/" in binary and Path(binary).exists(): binary = str(Path(binary).resolve())`), it never ran because `clang_tidy_version()` crashed first, and resolving per-TU inside the thread pool was fragile when `cwd` was not the repository root.

3. **Remediation**:
   - In `Makefile`, anchor `TIDY_RATCHET_EXTRA_sycl` to `$(CURDIR)/scripts/ci/clang-tidy-sycl.sh`, ensuring that when Make runs from any worktree (including via `make -C <path>`), an absolute path anchored to that worktree's root is passed.
   - In `scripts/ci/tidy-ratchet.py`, add `resolve_clang_tidy(binary, repo_root)`:
     - Absolute paths are preserved.
     - Bare executable names (no slashes) are preserved for standard PATH resolution by `safe_subprocess`.
     - Multi-component relative paths are resolved to absolute paths against `Path.cwd()` or `repo_root`.
     - Wire `resolve_clang_tidy()` across `main()`, `measure()`, `clang_tidy_version()`, and `run_one()`.
     - Update `clang_tidy_version()` to catch `(OSError, ValueError)` so unexpected validation failures report clean version unavailability rather than uncaught crashes.

## Verification

1. **Reproduction on parent**:
   - `make tidy-ratchet LANE=sycl TIDY_RATCHET_BUILD_DIR=/tmp/test-sycl-build` reproduced `error: measurement/output failed: allowlisted executable must be bare or absolute: scripts/ci/clang-tidy-sycl.sh`.
   - Python unit test calling `ratchet.measure("sycl", build, root, "scripts/ci/fake-clang-tidy.sh", [], 1)` failed with `CommandValidationError`.

2. **Post-fix verification**:
   - The focused regression test `test_relative_wrapper_path_in_subdirectory_survives_safe_subprocess` and 3D tests in `ResolveClangTidy` pass.
   - `make tidy-ratchet LANE=sycl TIDY_RATCHET_BUILD_DIR=/tmp/test-sycl-build TIDY_RATCHET_ARGS="--only core/src/cpu.cpp"` succeeds (exit code 0), verifying `safe_subprocess` accepts the wrapper.
   - Direct CLI invocation `python3 scripts/ci/tidy-ratchet.py --lane sycl --build-dir /tmp/test-sycl-build --clang-tidy scripts/ci/clang-tidy-sycl.sh --only core/src/cpu.cpp` succeeds (exit code 0).
