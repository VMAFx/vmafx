<!-- markdownlint-disable MD060 -->
# Dependency-update bot — operator playbook

The fork uses **Mend Renovate** as a [GitHub App][app], not self-hosted.
The App reads the in-tree [`renovate.json`](../../renovate.json) and opens
grouped dependency-update PRs on the configured weekday schedule.

[app]: https://github.com/apps/renovate

## Quick start

1. Visit <https://github.com/apps/renovate> and install the App on
   `VMAFx/vmafx`.
2. The App posts a Dependency Dashboard issue (currently
   [#749](https://github.com/VMAFx/vmafx/issues/749)) listing pending /
   awaiting / errored updates.
3. Tick a checkbox in the dashboard issue to force creation of any
   awaiting update; the App reacts within a minute or two.

## Merge policy during release candidates

Ordinary Renovate and other version-update pull requests are not frozen during
any release candidate, RC1 through RC8. Merge them when the repository's normal
required checks, review, digest and pin policy, and component-specific
validation pass.

- **Not frozen.** Security updates are prioritised, but they are not the only
  version changes allowed. The strict dependency-only classification in
  [ADR-1152](../adr/1152-dependency-pr-gate-exemption.md) exempts qualifying
  bot PRs from documentation-process gates only; it does not waive build, test,
  security or review requirements.
- **Specialised checks stay.** Major or coordinated CUDA, ROCm, oneAPI,
  compiler, action and base-image updates retain their existing smoke and
  lockstep checks.
- **Revalidate after a merge.** Candidate acceptance is bound to an exact
  commit and artifact under
  [ADR-1341](../adr/1341-rc-correctness-benchmark-retrain-sequence.md), so a
  version update merged after evidence collection requires the affected
  checks, benchmarks or model validation to be rerun. Revalidation replaces a
  blanket version freeze; it does not permit stale evidence.

## Configuration

All configuration lives in [`renovate.json`](../../renovate.json). The
App reads it on every webhook. Top-level knobs:

| Setting | Value |
|---------|-------|
| `schedule` | `before 6am every weekday` (`Europe/Vienna`) |
| `prHourlyLimit` | `0` (unlimited) |
| `prConcurrentLimit` | `3` |
| `prCreation` | `immediate` |
| `minimumReleaseAge` | `3 days` |

## Disable / rollback to Dependabot

To switch back to Dependabot (for example when the Renovate App is unavailable):

1. Uninstall the App at <https://github.com/settings/installations>.
2. Rename `.github/dependabot.yml.disabled` → `.github/dependabot.yml`.

## Maintainer notes

Design notes for people who edit `renovate.json`.

### Root Python requirement

The repository-root `pyproject.toml` contains shared tooling configuration, not
a distributable Python package. Its `requires-python` value therefore records
the supported Python 3.14 series (`>=3.14`) instead of following every CPython
patch release. Dependabot's updater image can trail the newest patch and cannot
build the pip dependency graph when the declared floor is newer than its bundled
interpreter.

An exact-root `pep621` package rule excludes only that metadata entry from
Renovate. Patch-pinned workflow runtimes and each installable subpackage's own
`requires-python` range continue to receive normal dependency maintenance.

### Custom manager file selection

Custom managers use anchored regexes with exactly one delimiter at each end,
for example `"/^build-config\\.env$/"` in JSON. `"//^build-config\\.env$//"`
is a valid regex that matches no repository path: schema validation alone
cannot catch that mistake.

Run the file-selection fixtures after editing `managerFilePatterns`:

```bash
python3 -m unittest discover -s scripts/ci/tests -p test_renovate_file_patterns.py -v
```

The pre-commit hook runs the same tests locally and in CI. Every custom pattern
must select a tracked input and exclude archived copies and backup suffixes.
The base-image manager must cover `build-config.env` and all nine Dockerfile
mirrors whose built-in Docker manager is disabled. Keep those two lists aligned
when adding a consumer. These tests check selection, not whether a scheduled
Renovate run has opened an update PR. The
[research note](../research/renovate-file-pattern-delimiters.md) records the
installed Renovate matcher used to reproduce the defect.

### Images pinned in Go source

The controller store tests start PostgreSQL from two Go constants in
`cmd/vmafx-controller/store/storetest/storetest.go`: `Image`, the release the
chart deploys, and `OldestImage`, the oldest release external servers may run.
Each is a `postgres:<tag>@sha256:<digest>` string, because golusoris
`testutil/pg` refuses a test image without a digest. No built-in Renovate
manager reads a Go string, so a custom manager (`depNameTemplate: postgres`)
matches both constants and proposes new digests and tags for them. A package
rule keeps `OldestImage` on its major line (`allowedVersions` `/^16\./`);
`Image` receives major updates as ordinary manual pull requests.

`test_storetest_postgres_images_are_tracked` and
`test_oldest_postgres_image_stays_on_its_major` in
`scripts/ci/tests/test_renovate_file_patterns.py` evaluate the configured
patterns against the file. They fail when a constant is added that the
manager does not match, when the manager loses the digest, or when the rule
stops matching the oldest image.

## Migration from self-hosted (2026-05-10)

Removed `.github/workflows/renovate.yml`. The App's webhook-driven model
replaces the cron-driven workflow. The `RENOVATE_TOKEN` secret is no
longer needed and can be deleted from repo secrets after install.

See [ADR-0387](../adr/0387-renovate-github-app-migration.md) for the
decision record (supersedes the self-hosted half of ADR-0363).
