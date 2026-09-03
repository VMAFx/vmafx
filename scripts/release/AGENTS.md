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
fragment sources, removes the one-time `bootstrap-sha` / `release-as` cutover
fields, and writes a receipt under `changelog.d/releases/`. Stage
`release-please-config.json` with the CHANGELOG, receipt, and fragment removals.

The `## [Unreleased]` header must be preserved verbatim and the rendered body
must not inject extra blank lines before the first `### Section` heading. The
double-awk pipeline in `--write` was designed to preserve this shape; do not
simplify it to `sed -i` without running both release-script test harnesses.

## rollover-changelog-fragments.sh

This is a release-PR-only operation. Preconditions are deliberately strict:
the working tree is clean, the manifest and every generic version marker equal
the requested version, fragment rendering is current, the target heading is
absent, at least one active source exists, and any remaining `release-as`
matches the requested version. After the cut, active fragments and
`_pre_fragment_legacy.md` are removed and the one-time release-please fields are
retired; their exact rendered content remains in the versioned CHANGELOG section
and their source history remains in Git.

The generated receipt records the source count and rendered SHA-256. A second
identical invocation is a no-op only when the target heading and receipt exist
and no active sources or cutover fields remain. Any late merge after rollover
adds an active fragment again, invalidating the release PR until the operator
rebuilds the cut.

Test coverage:
`scripts/release/tests/test-rollover-changelog-fragments.sh`.

## Release-line invariants (ADR-1151)

**The fork's first release is `v1.0.0` on a number line that starts at the
fork.** VMAFx has never released; every `vX.Y.Z` tag reachable from this
checkout belongs to Netflix upstream history and none is an ancestor of
`master`. Do not "restore" a 3.x product version.

**`release-as` and `bootstrap-sha` are ONE-SHOT.** Both live in
`release-please-config.json` only for the first cut. `release-as` is a
deprecated, *persistent* override applied after commit analysis: left in place
it pins every subsequent release to the same version and swallows
feat/BREAKING bumps. `rollover-changelog-fragments.sh` deletes both in the same
PR that merges the release, and the `Release Script Contract (ADR-1128)` job in
`.github/workflows/rule-enforcement.yml` fails if either survives once the root
manifest reaches 1.0.0. To force a later version use a `Release-As: X.Y.Z`
commit footer — inherently one-shot, so it cannot rot.

**The product version and the ABI SONAME are independent.** release-please owns
the product version (tag, `core/meson.build` `project(version:)` and therefore
`libvmaf.pc`, the three Python distributions, Helm `appVersion`). The SONAME is
the separate `vmaf_soname_version = '3.0.0'` at `core/meson.build:19`, producing
`libvmaf.so.3`, hand-bumped only on an ABI break. The 1.0.0 cut does not touch
it. Do not align the two.

**`extra-files` is the single marker list.** `verify-release-version.sh` and
`rollover-changelog-fragments.sh` both derive their check list from that array
and both require *exactly one* `x-release-please-version` marker per listed
file. Adding a second marker to a listed file — e.g. annotating
`deploy/helm/vmafx/Chart.yaml`'s `version:` alongside its `appVersion:` — hard-
fails every release preflight. Removing a file from `extra-files` means also
removing its marker comment.

**release-please force-recreates its own branch.** Every push to `master`
rewrites `release-please--branches--master--…` to a single fresh bot commit.
The hand-added rollover commits therefore survive only while the release PR
carries the `autorelease: cut` label, which makes `release-please.yml` skip its
PR-update invocation. Label first, then push the cut.

## verify-release-version.sh

Every publication workflow checks out the selected ordinary `vX.Y.Z` tag and
runs this preflight before any job receives write or OIDC permissions. The
script validates the root manifest, discovers every coordinated marker from
release-please's `extra-files`, requires exactly one matching marker per file,
and confirms that the checked-out commit is the selected tag. Add new release
surfaces to `extra-files`; do not duplicate an independent version list in the
workflow.

Since ADR-1151 it also proves the fragment cut actually ran, because nothing
else did: exactly one `## [X.Y.Z] - YYYY-MM-DD` heading in `CHANGELOG.md`, a
`changelog.d/releases/X.Y.Z.json` receipt whose `.version` matches, zero active
fragments across the six section directories, no `_pre_fragment_legacy.md`, and
no surviving `release-as` / `bootstrap-sha`. These mirror the post-conditions of
`rollover-changelog-fragments.sh` exactly — change one and change the other.
`concat-changelog-fragments.sh --check` is *not* a substitute: it passes
identically before and after a cut.

Test coverage:
`scripts/release/tests/test-verify-release-version.sh`.

## Publication environment binding

The environment names must also *exist server-side with a required reviewer*.
GitHub auto-creates a referenced environment that does not exist, with an empty
rule set, so a job naming a missing environment runs with no approval gate at
all. `test-publication-environment-binding.sh` only greps the YAML and cannot
see that; `supply-chain.yml`'s `validate-release` carries the live
`gh api repos/$GITHUB_REPOSITORY/environments/<name>` preflight and fails closed
(ADR-1151). Keep both — the grep catches a workflow that forgets the binding,
the preflight catches a repository that never configured the environment.

The two Docker publishers bind every GHCR write/OIDC job to
`release-publish`; `supply-chain.yml` binds Sigstore and GitHub Release writes
there while keeping PyPI on its trusted-publisher environment
`pypi-publish`. The third-party SLSA reusable jobs cannot declare an
environment, so they may mint provenance but must keep `contents: read` and
`upload-assets: false`; the protected attachment job downloads and publishes
their provenance artifacts. Container verification accepts only the exact
release-tag workflow identity, never an `@.*` ref wildcard.

Test coverage:
`scripts/release/tests/test-publication-environment-binding.sh`.

## verify-native-release-artifacts.sh

The native GitHub Release payload is currently Linux ELF. Meson's
`libvmaf.so` -> SONAME -> real-name symlink chain must be staged under every
name as identical regular-file bytes because GitHub artifact downloads do not
preserve symlinks. The verifier parses both the library SONAME and the CLI's
`DT_NEEDED`, rejects a missing or divergent chain member, and requires `ldd` to
resolve the dependency from the staged directory before running the exact CLI
version under `env -i`.

Run the verifier both before hashing/signing and after the artifact
upload/download round trip. The round-trip job restores `vmaf`'s executable
bit first because raw artifact and release downloads do not carry POSIX mode
metadata. Do not replace the runtime check with a filename-only assertion.

Test coverage:
`scripts/release/tests/test-verify-native-release-artifacts.sh`.
