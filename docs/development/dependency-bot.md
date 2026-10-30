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
App reads it on every webhook. It extends the fleet's shared preset
`github>plurano/horistes` (Greek *horistes*, the boundary commissioner who
fixes limits, pronounced ho-RIS-tees; repository
<https://github.com/plurano/horistes>), which sets `config:recommended`,
`:gitSignOff`, `minimumReleaseAge: 3 days`, `automerge: false` and turns off
updates of GitHub-hosted runner labels. `renovate.json` keeps only what is
specific to this repository; the per-rule `automerge` values below override
the preset's default. Top-level knobs:

| Setting | Value |
|---------|-------|
| `extends` | `github>plurano/horistes`, `config:best-practices`, `:semanticCommits`, `helpers:disableTypesNodeMajor` |
| `schedule` | `before 6am on Monday` (`Europe/Vienna`) |
| `prHourlyLimit` | `0` (unlimited) |
| `prConcurrentLimit` | `3` |
| `prCreation` | `immediate` |
| `minimumReleaseAge` | `3 days` (from the preset) |

Check a change with the validator in strict mode, for example:

```bash
npx --yes --package renovate@44.161.2 -- \
  renovate-config-validator --no-global --strict renovate.json
```

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

### CI Python interpreter

The CI interpreter is one version written in two kinds of places:
`PYTHON_CI_VERSION` in `build-config.env` and the `python-version:` input of
every `actions/setup-python` step in `.github/workflows/`.
`scripts/ci/check-workflow-versions.py` fails a pull request in which the two
disagree. Renovate's built-in `github-actions` manager reads only the
workflows, so its pull requests raised the workflow pins without
`build-config.env` and failed that check.

One custom manager (`depNameTemplate: python`, `packageNameTemplate:
actions/python-versions`) now reads both kinds of places, and a package rule
turns the `github-actions` manager off for the `python` input. A second rule
groups the interpreter as "CI Python interpreter", so one pull request raises
every pin together. That rule also holds the interpreter below 3.15.0
(`allowedVersions: "<3.15.0"`) until 1.0.0 is released. To move to 3.15 after
that, raise the bound in the same pull request as `PYTHON_VERSION` and
`requires-python`. The decision is
[ADR-3312](../adr/3312-renovate-python-ci-version-group.md).

`PythonCiVersionGroup` in `scripts/ci/tests/test_renovate_file_patterns.py`
reads `build-config.env` and the workflows with the same pattern as the
version check. It fails in any of these cases:

- a workflow pin the check reads is not read by the manager;
- the manager reads a value the check does not;
- the `github-actions` rule is turned back on;
- the version bound or the group is removed.

The test runs from the same pre-commit hook as the other file-selection
fixtures:

```bash
python3 -m pytest scripts/ci/tests/test_renovate_file_patterns.py -k PythonCiVersionGroup
```

### CI Node.js

Node.js is named in four places: `NODE_VERSION` in `build-config.env`,
`default_language_version.node` in `.pre-commit-config.yaml`, `.node-version`,
and the `node-version:` input of every `actions/setup-node` step under
`.github/workflows/`. `.node-version` is the file the engine-managed
`praetor-docs.yml` reads once the engine is re-adopted; an engine seeds it only
when it is absent, so it stays on our release. The check also refuses a missing
`.node-version`.
`scripts/ci/check-workflow-versions.py` refuses a pull
request in which any copy differs from `build-config.env` (a major-only workflow
input such as `"24"` must name the same major). Renovate's built-in
`github-actions` manager reads only the workflow pins, so its pull requests
raised the workflow pins without `build-config.env` and
`.pre-commit-config.yaml` and failed the single-source gate.

Following the ADR-3312 pattern, one custom manager (`depNameTemplate: node`,
`datasourceTemplate: github-releases`,
`packageNameTemplate: actions/node-versions`, the source `actions/setup-node`
installs from) reads all four places, and package rules turn off the
`github-actions` manager for the `node` input (`depType: uses-with`) and the
`nodenv` manager, which reads another datasource for `.node-version`. A
second rule groups the updates under `CI
Node.js` (`draftPR: false`, `automerge: false`), so one reviewed pull request
raises every pin together. Exact three-part pins (`x.y.z`) are matched, leaving
major-only pins such as `praetor-docs.yml` untouched.

`NodeCiVersionGroup` in `scripts/ci/tests/test_renovate_file_patterns.py` holds
the manager to the pins the single-source gate reads. It fails in any of these
cases:

- a workflow pin the check reads is not read by the manager;
- `build-config.env`, `.pre-commit-config.yaml` or `.node-version` is not selected;
- the manager reads a value the check does not;
- the `github-actions` or `nodenv` rule is turned back on;
- the group rule is removed.

### Images pinned in Go source

The controller store tests start PostgreSQL from two Go constants in
`cmd/vmafx-controller/internal/storetest/storetest.go`: `Image`, the release the
chart deploys, and `OldestImage`, the oldest release external servers may run.
Each is a `postgres:<tag>@sha256:<digest>` string, because golusoris
`testutil/pg` refuses a test image without a digest. No built-in Renovate
manager reads a Go string, so a custom manager (`depNameTemplate: postgres`)
matches both constants and proposes new digests and tags for them. A package
rule keeps `OldestImage` on its major line (`allowedVersions` `/^16\./`);
`Image` receives major updates as ordinary manual pull requests.

The package is internal on purpose. A digest update changes the value of both
constants, which the Go API compatibility gate would report as an incompatible
change of an exported constant; the gate compares no package with an `internal`
path element, so these updates pass without a breaking marker.

`test_storetest_postgres_images_are_tracked` and
`test_oldest_postgres_image_stays_on_its_major` in
`scripts/ci/tests/test_renovate_file_patterns.py` evaluate the configured
patterns against the file. They fail when a constant is added that the
manager does not match, when the manager loses the digest, or when the rule
stops matching the oldest image.

`cmd/vmafx-controller/internal/storetest/storetest.go` is allowlisted in
`scripts/ci/classify-dependency-pr.sh` (ADR-1152), so automated Renovate pull
requests bumping these image digests qualify for the dependency-only
deliverables checklist exemption without admitting other Go files under `cmd/`.

## Migration from self-hosted (2026-05-10)

Removed `.github/workflows/renovate.yml`. The App's webhook-driven model
replaces the cron-driven workflow. The `RENOVATE_TOKEN` secret is no
longer needed and can be deleted from repo secrets after install.

See [ADR-0387](../adr/0387-renovate-github-app-migration.md) for the
decision record (supersedes the self-hosted half of ADR-0363).
