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

`release-please` consumes `CHANGELOG.md` at release time. The script's
`--write` mode must produce output that `release-please` can parse —
specifically: the `## [Unreleased]` header must be preserved verbatim and
the rendered body must not inject extra blank lines before the first
`### Section` heading. The double-awk pipeline in `--write` was designed
to thread this needle; do not simplify it to `sed -i` without verifying
release-please compatibility.
