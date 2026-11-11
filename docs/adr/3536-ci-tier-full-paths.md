# ADR-3536: Escalate own pull requests touching platform launch paths to full CI

- **Status**: Accepted (amends [ADR-2169](2169-ci-fewer-runs.md))
- **Date**: 2026-11-11
- **Deciders**: Repository maintainers
- **Tags**: `ci`, `testing`

## Context

Under [ADR-2169](2169-ci-fewer-runs.md), unlabelled pull requests whose head
repository is this repository run the `light` tier, which skips `full_only`
contexts including the macOS and Windows build and test jobs. Pull request #3401
modified `scripts/lib/safe_subprocess.py` and
`tools/rc1-tester/src/vmaf_rc1_tester/_safe_subprocess.py`, which contain macOS
and Windows process-launch logic. Because #3401 came from this repository and
carried no `ci: full` label, `scripts/ci/ci_tier.py` selected `light`, skipping
the macOS contexts; after merge, macOS `tox` failed on `master`.

`.github/ci-tier.json` is the single tier authority, and `changed_paths()` in
`scripts/ci/ci_tier.py` already fetches pull-request files from the paginated
REST endpoint for the release-branch check. Because
`.github/workflows/ci-tier.yml` uses a sparse checkout, tier evaluation cannot
inspect the full repository tree; path-drift verification against tracked files
must run outside `ci-tier.yml`. Pull-request `Pre-Commit` runs diff-scoped via
`scripts/ci/precommit_scope.py` and skips local hooks listed by
`scripts/ci/suite_registry.py precommit-skip`.

## Decision

1. **Configuration (`full_paths`).** `.github/ci-tier.json` defines
   `full_paths`, validated by `load_config()` as a list of unique, non-empty,
   single-line strings. Patterns use Python `fnmatch.fnmatchcase`, reusing the
   same glob semantics as `scripts/ci/ci_router.py` and
   `scripts/ci/suite_registry.py`: `*` crosses `/`, matching is case-sensitive,
   and each pattern matches the complete repository-relative path string.
   Explicit file paths are preferred over broad directory globs.

2. **Tier escalation and single-fetch reuse.** In `scripts/ci/ci_tier.py`,
   `decide()` evaluates `full_paths` on non-labelled own pull requests after
   the `ci: full` label check and the `release-please--` exemption check, and
   before returning `light`. Changed paths reach `decide()` lazily through a
   callable; `run()` fetches pull-request files at most once via
   `changed_paths()` and shares that result across both the release-branch
   exemption check and `full_paths` matching. When any changed path matches a
   `full_paths` pattern, `decide()` returns `full` with a reason naming the
   first matched path and pattern.

3. **Fail-closed behaviour.** When `full_paths` is non-empty on an own pull
   request and changed paths cannot be obtained (missing API token, missing
   pull-request number, or API failure), `decide()` returns `full` with reason
   `changed paths unavailable` rather than falling back to `light`.

4. **Tracked-file drift gate.** `scripts/ci/ci_tier.py --check-full-paths`
   runs `git ls-files` with an explicit timeout and bounded loops and verifies
   that every `full_paths` pattern matches at least one tracked file, exiting
   non-zero and naming each unmatched pattern on drift. Local pre-commit hook
   `check-ci-tier-full-paths` runs with `always_run: true`,
   `pass_filenames: false`, and stages `pre-commit` and `pre-push`.
   `suite_registry.py precommit-skip` lists only hooks whose entry runs tests
   of the tooling suite; this entry runs a check script, so the hook is not on
   that list and runs on every pull request in CI. The
   `test-readme-badges` hook `files` pattern also includes
   `.github/ci-tier.json`.

5. **Seeded paths from repository audit.** Auditing `sys.platform` and
   `platform.system` across `scripts/lib/*.py` and
   `tools/rc1-tester/src/vmaf_rc1_tester/*.py`, alongside the process-runner
   modules and their tests, yields six kept files and two dropped files:

   - **Kept** `scripts/lib/safe_subprocess.py`: seed process runner; inspects
     macOS extended ACLs under `sys.platform == "darwin"` before starting
     processes with `subprocess.Popen`.
   - **Kept** `scripts/lib/test_safe_subprocess.py`: seed test suite; exercises
     macOS `chmod` ACLs and `safe_subprocess.run` under
     `sys.platform == "darwin"`.
   - **Kept** `tools/rc1-tester/src/vmaf_rc1_tester/_safe_subprocess.py`: seed
     tester copy of `safe_subprocess.py` carrying identical
     `sys.platform == "darwin"` process-launch logic.
   - **Kept** `tools/rc1-tester/tests/test_safe_subprocess_copy.py`: test
     module verifying `_safe_subprocess.py` stays byte-identical to
     `scripts/lib/safe_subprocess.py`.
   - **Kept** `tools/rc1-tester/src/vmaf_rc1_tester/hw_facts.py`:
     `platform.system() == "Darwin"` branch runs `/usr/sbin/sysctl` and
     `/usr/sbin/system_profiler` through `run_bounded`.
   - **Kept** `tools/rc1-tester/src/vmaf_rc1_tester/probe.py`:
     `platform.system() == "Darwin"` branches run `sw_vers`, `sysctl`, and
     `system_profiler` through `run_bounded`.
   - **Dropped** `tools/rc1-tester/src/vmaf_rc1_tester/hw_cudaprobe.py`:
     `sys.platform == "win32"` selects `nvcuda.dll` or `libcuda.so.1` for
     in-process `ctypes.CDLL` loading and starts no process.
   - **Dropped** `tools/rc1-tester/src/vmaf_rc1_tester/hw_l0probe.py`:
     `sys.platform == "win32"` selects `ze_loader.dll` or `libze_loader.so.1`
     for in-process `ctypes.CDLL` loading and starts no process.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| `full_paths` globs in `.github/ci-tier.json` | Single tier authority; automatic `full` routing on matching diffs; fail-closed on missing paths; drift-checked against `git ls-files` | Runs the full suite whenever a listed path changes | Chosen |
| Pull-request body proof check | Avoids full-suite runs when authors paste cross-platform test output | Manual evidence drifts as commits land after the note and does not exercise hosted macOS or Windows environments | Relies on manual attestation instead of automated execution |
| Pre-push notice only | Zero CI cost | Advisory output does not gate merges, can be missed, and runs in a single local OS environment | Leaves `master` exposed to cross-platform regressions |

## Consequences

- **Positive**: Own pull requests that touch seeded platform-guarded
  process-launch files or their tests automatically run the `full` tier,
  exercising macOS and Windows contexts before merge. Missing path metadata
  fails closed to `full`, and file renames or removals fail
  `check-ci-tier-full-paths` on every pull request.
- **Negative**: Own pull requests touching any entry in `full_paths` pay the
  wall time and runner minutes of the `full` suite instead of finishing at
  `light`.
- **Neutral / follow-ups**: Renaming or deleting a file listed in `full_paths`
  requires updating `.github/ci-tier.json` in the same change so
  `--check-full-paths` passes.

## References

- Pull request #3401 and [ADR-3532](3532-trusted-executable-launch-links.md).
- [ADR-2169](2169-ci-fewer-runs.md) (CI tier routing).
- Implementation and tests: `.github/ci-tier.json`, `scripts/ci/ci_tier.py`,
  `scripts/ci/ci_router.py`, and `scripts/ci/tests/test_ci_tier.py`.
