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
   first matched path and pattern. Before matching globs, a change to
   `.github/ci-tier.json` selects `full` with that filename in the reason,
   including when the same change removes a listed file and its last glob.
   That tier-configuration check also runs for an exempt release pull request
   before the `autorelease: cut` label check, so a release bot cannot earn
   `release-light` while changing the tier definition. `TIER_CONFIG_PATH` owns
   the relative filename for routing and config reads.

3. **Fail-closed behaviour.** When an own pull request's changed paths cannot
   be obtained (missing API token, missing pull-request number, or API
   failure), `decide()` returns `full` with reason
   `changed paths unavailable`, including with an empty `full_paths` list.
   `run.exempt()` refuses an unavailable diff before calling the release
   exemption script, so a release bot cannot earn `release-light` on author
   and branch alone after a failed fetch. `changed_paths()` and
   `live_labels()` raise `TierError` naming the REST endpoint whenever any
   paginated entry is not an object or lacks its string `filename` or `name`,
   so malformed rows fail closed to `full` instead of being silently omitted.

4. **Tracked-file drift gate.** `scripts/ci/ci_tier.py --check-full-paths`
   runs `git ls-files` with an explicit timeout and bounded loops and verifies
   that every `full_paths` pattern matches at least one tracked file, exiting
   non-zero and naming each unmatched pattern on drift. `_tracked_files()`
   always drops repository-locating `GIT_*` environment variables, keeping an
   absolute `GIT_INDEX_FILE` only when listing this checkout's staged index and
   stripping every `GIT_*` variable for any other root. Local pre-commit hook
   `check-ci-tier-full-paths` runs with `always_run: true`,
   `pass_filenames: false`, and stages `pre-commit` and `pre-push`.
   `suite_registry.py precommit-skip` lists only hooks whose entry runs tests
   of the tooling suite; this entry runs a check script, so the hook is not on
   that list and runs on every pull request in CI. The
   `test-readme-badges` hook `files` pattern also includes
   `.github/ci-tier.json`.

5. **Seeded paths from repository audit.** The
   [research digest](../research/3536-ci-tier-full-paths.md) records the
   reproducible `sys.platform`, `os.name`, and `platform.system()` search,
   each listed path, and each excluded candidate. Its seven listed files
   include the Windows-only packaged-loader launch in `hw_sycl.py`, found
   when extending the original search to `os.name`.

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

- Source: `req` (the maintainer's request, 2026-11-11, paraphrased): list the
  platform-guarded launch files as full-tier paths in `.github/ci-tier.json`,
  amend ADR-2169, prove it with a planted case (the pull request 3401 path
  list decides full, an unlisted path light, one boundary glob), and check
  that every listed glob still matches a tracked file so the list cannot drift
  silently.
- [Research-3536](../research/3536-ci-tier-full-paths.md) (platform-path audit).
- Pull request #3401 and [ADR-3532](3532-trusted-executable-launch-links.md).
- [ADR-2169](2169-ci-fewer-runs.md) (CI tier routing).
- Implementation and tests: `.github/ci-tier.json`, `scripts/ci/ci_tier.py`,
  `scripts/ci/ci_router.py`, and `scripts/ci/tests/test_ci_tier.py`.
