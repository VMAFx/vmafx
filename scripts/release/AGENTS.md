<!-- markdownlint-disable MD060 -->
# `scripts/release/` — agent invariants

Fork-local release utilities. Nothing here merges from upstream Netflix/vmaf.

## concat-changelog-fragments.sh

Renders the `## [Unreleased]` block of `CHANGELOG.md` from per-PR fragment
files under `changelog.d/<section>/*.md`. Modes:

| Flag | Effect |
|---|---|
| *(none)* | Print rendered body to stdout |
| `--check` | Diff rendered output against in-tree `CHANGELOG.md`; exit 1 on drift |
| `--write` | Splice rendered body into `CHANGELOG.md` in place |

### Tempfile lifecycle (ADR-0968)

The `--write` path allocates two tempfiles (`tmp_body`, `tmp_out`) via
`mktemp`. An `EXIT` trap (`trap 'rm -f "$tmp_body" "$tmp_out"' EXIT`) is
registered **immediately after both `mktemp` calls** so that any
subsequent failure — awk pipeline error, `mv` failure, `set -e` abort —
cleans up both files automatically.

**Invariant**: the `EXIT` trap must remain immediately after the two
`mktemp` lines. Do not move it past any code that could trigger early
exit, and do not add manual `rm -f` for the same variables elsewhere in
the `--write` branch (redundant, and easy to miss if the variable names
ever change). The trap is the single cleanup point.

Test coverage: `scripts/release/tests/test-concat-changelog-fragments.sh`
(T1–T4, including simulated awk failure via a `PATH` shim).

### Fragment naming convention

Fragment files are sorted lexically (`LC_ALL=C sort`). Contributors
prefix filenames with a task/ADR ID for implicit ordering, e.g.
`0968-ci-scripts-rebrand-tempfile.md`. Dotfiles are excluded (`! -name
'.*'`).

### Coupling with release-please

The fragment renderer is the sole owner of `CHANGELOG.md`; release-please has
`skip-changelog: true`. Before merging a generated release PR, run
`rollover-changelog-fragments.sh` with the release PR's exact version and UTC
date. It versions the rendered body, empties Unreleased, consumes the active
fragment sources, and writes a receipt under `changelog.d/releases/`.

The `## [Unreleased]` header must be preserved verbatim and the rendered body
must not inject extra blank lines before the first `### Section` heading. The
double-awk pipeline in `--write` was designed to preserve this shape; do not
simplify it to `sed -i` without running both release-script test harnesses.

## rollover-changelog-fragments.sh

This is a release-PR-only operation. Preconditions are deliberately strict:
the working tree is clean, the manifest and every generic version marker equal
the requested version, fragment rendering is current, the target heading is
absent, and at least one active source exists. After the cut, active fragments
and `_pre_fragment_legacy.md` are removed; their exact rendered content remains
in the versioned CHANGELOG section and their source history remains in Git.

The generated receipt records the source count and rendered SHA-256. A second
identical invocation is a no-op only when the target heading and receipt exist
and no active sources remain. Any late merge after rollover adds an active
fragment again, invalidating the release PR until the operator rebuilds the
cut.

Test coverage:
`scripts/release/tests/test-rollover-changelog-fragments.sh`.
