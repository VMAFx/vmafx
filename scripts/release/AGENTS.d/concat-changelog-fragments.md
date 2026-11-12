---
paths:
  - scripts/release/concat-changelog-fragments.sh
  - scripts/release/tests/test-concat-changelog-fragments.sh
  - changelog.d/**
invariant: EXIT trap immediately after both mktemp calls; sole CHANGELOG.md owner; preserve header.
---
<!-- markdownlint-disable MD013 MD060 -->
# Changelog fragment renderer (`concat-changelog-fragments.sh`)

## concat-changelog-fragments.sh

Renders `## [Unreleased]` block of `CHANGELOG.md` from per-PR fragment
files under `changelog.d/<section>/*.md`. Modes:

| Flag | Effect |
|---|---|
| *(none)* | Print rendered body to stdout |
| `--check` | Diff rendered output against in-tree `CHANGELOG.md`; exit 1 on drift |
| `--write` | Splice rendered body into `CHANGELOG.md` in place |

### Tempfile lifecycle (ADR-0968)

`--write` path allocates two tempfiles (`tmp_body`, `tmp_out`) via
`mktemp`. `EXIT` trap (`trap 'rm -f "$tmp_body" "$tmp_out"' EXIT`)
registered **immediately after both `mktemp` calls**, so any
subsequent failure — awk pipeline error, `mv` failure, `set -e` abort —
cleans up both files automatically.

**Invariant**: `EXIT` trap must remain immediately after both
`mktemp` lines. Never move it past code that could trigger early exit.
Never add manual `rm -f` for same variables elsewhere in `--write`
branch (redundant; easy to miss if variable names change). Trap =
single cleanup point.

Test coverage: `scripts/release/tests/test-concat-changelog-fragments.sh`
(T1–T4, incl. simulated awk failure via `PATH` shim).

### Fragment naming convention

Fragment files sorted lexically (`LC_ALL=C sort`). Contributors prefix
filenames with task/ADR ID for implicit ordering, e.g.
`0968-ci-scripts-rebrand-tempfile.md`. Dotfiles excluded (`! -name
'.*'`).

### Coupling with release-please

Fragment renderer = sole owner of `CHANGELOG.md`; `release-please` has
`skip-changelog: true`. Before merging generated release PR: run
`rollover-changelog-fragments.sh` with release PR's exact version and
UTC date. It versions rendered body, empties Unreleased, consumes
active fragment sources, removes one-time `bootstrap-sha` /
`release-as` cutover fields, writes receipt under
`changelog.d/releases/`. Stage `release-please-config.json` with
CHANGELOG, receipt, fragment removals.

`## [Unreleased]` header must be preserved verbatim; rendered body
must not inject extra blank lines before first `### Section` heading.
Double-awk pipeline in `--write` designed to preserve this shape —
never simplify to `sed -i` without running both release-script test
harnesses.
