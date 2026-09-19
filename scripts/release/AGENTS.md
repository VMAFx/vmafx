<!-- markdownlint-disable MD060 -->
# `scripts/release/` — agent invariants

Fork-local release utilities. Nothing here merges from upstream Netflix/vmaf.

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

## rollover-changelog-fragments.sh

Release-PR-only operation. Preconditions deliberately strict: working
tree clean; manifest and every generic version marker equal requested
version; fragment rendering current; target heading absent; at least
one active source exists; any remaining `release-as` matches requested
version. After cut: active fragments and `_pre_fragment_legacy.md`
removed, one-time `release-please` fields retired; exact rendered
content remains in versioned CHANGELOG section, source history remains
in Git.

Generated receipt records source count and rendered SHA-256. Second
identical invocation = no-op only when target heading and receipt
exist, and no active sources or cutover fields remain. Late merge
after rollover adds active fragment again, invalidating release PR
until operator rebuilds cut.

Test coverage:
`scripts/release/tests/test-rollover-changelog-fragments.sh`.

## Release-line invariants (ADR-1151)

**Fork's first release = `v1.0.0`, on number line starting at fork.**
VMAFx never released; every `vX.Y.Z` tag reachable from this checkout
belongs to Netflix upstream history, none is ancestor of `master`.
Never "restore" 3.x product version.

**`release-as` and `bootstrap-sha` = ONE-SHOT.** Both live in
`release-please-config.json` only for first cut. `release-as` =
deprecated, *persistent* override applied after commit analysis: left
in place, pins every subsequent release to same version, swallows
feat/BREAKING bumps. `rollover-changelog-fragments.sh` deletes both in
same PR that merges release; `Release Script Contract (ADR-1128)` job
in `.github/workflows/rule-enforcement.yml` fails if either survives
once root manifest reaches 1.0.0. To force later version: use
`Release-As: X.Y.Z` commit footer — inherently one-shot, cannot rot.

**Product version and ABI SONAME = independent.** `release-please`
owns product version (tag, `core/meson.build` `project(version:)`,
therefore `libvmaf.pc`, three Python distributions, Helm `appVersion`).
SONAME = separate `vmaf_soname_version = '3.0.0'` at
`core/meson.build:19`, producing `libvmaf.so.3`, hand-bumped only on
ABI break. 1.0.0 cut does not touch it. Never align them.

**`extra-files` = single marker list.** `verify-release-version.sh`
and `rollover-changelog-fragments.sh` both derive check list from that
array; both require *exactly one* `x-release-please-version` marker
per listed file. Adding second marker to listed file — e.g. annotating
`deploy/helm/vmafx/Chart.yaml`'s `version:` alongside its
`appVersion:` — hard-fails every release preflight. Removing file from
`extra-files` also removes its marker comment.

**`release-please` force-recreates its own branch.** Every push to
`master` rewrites `release-please--branches--master--…` to single
fresh bot commit. Hand-added rollover commits survive only while
release PR carries `autorelease: cut` label, which makes
`release-please.yml` skip its PR-update invocation. Label first, then
push cut.

## verify-release-version.sh

Every publication workflow checks out selected ordinary `vX.Y.Z` tag,
runs this preflight before any job receives write or OIDC permissions.
Script validates root manifest, discovers every coordinated marker
from `release-please`'s `extra-files`, requires exactly one matching
marker per file, confirms checked-out commit = selected tag. Add new
release surfaces to `extra-files`; never duplicate independent version
list in workflow.

Since ADR-1151, also proves fragment cut ran — nothing else did:
exactly one `## [X.Y.Z] - YYYY-MM-DD` heading in `CHANGELOG.md`;
`changelog.d/releases/X.Y.Z.json` receipt whose `.version` matches;
zero active fragments across six section directories; no
`_pre_fragment_legacy.md`; no surviving `release-as` / `bootstrap-sha`.
These mirror post-conditions of `rollover-changelog-fragments.sh`
exactly — change one, change other.
`concat-changelog-fragments.sh --check` is *not* substitute: passes
identically before and after cut.

Test coverage:
`scripts/release/tests/test-verify-release-version.sh`.

## Publication environment binding

Environment names must also *exist server-side with required
reviewer*. GitHub auto-creates referenced environment that does not
exist, with empty rule set — job naming missing environment runs
with no approval gate at all. `test-publication-environment-binding.sh`
only greps YAML, cannot see that; `supply-chain.yml`'s
`validate-release` carries live
`gh api repos/$GITHUB_REPOSITORY/environments/<name>` preflight, fails
closed (ADR-1151). Keep both — grep catches workflow that forgets
binding; preflight catches repository that never configured
environment.

Two Docker publishers bind every GHCR write/OIDC job to
`release-publish`; `supply-chain.yml` binds Sigstore and GitHub
Release writes there, keeping PyPI on its trusted-publisher
environment `pypi-publish`. Third-party SLSA reusable jobs cannot
declare environment, so may mint provenance but must keep
`contents: read` and `upload-assets: false`; protected attachment job
downloads and publishes their provenance artifacts. Container
verification accepts only exact release-tag workflow identity, never
`@.*` ref wildcard.

Test coverage:
`scripts/release/tests/test-publication-environment-binding.sh`.

## verify-native-release-artifacts.sh

Native GitHub Release payload currently Linux ELF. Meson's
`libvmaf.so` -> SONAME -> real-name symlink chain must be staged under
every name as identical regular-file bytes — GitHub artifact downloads
do not preserve symlinks. Verifier parses both library SONAME and
CLI's `DT_NEEDED`, rejects missing or divergent chain member.
Requires `ldd` to resolve dependency from staged directory before
running exact CLI version under `env -i`.

Run verifier both before hashing/signing and after artifact
upload/download round trip. Round-trip job restores `vmaf`'s
executable bit first — raw artifact and release downloads do not
carry POSIX mode metadata. Never replace runtime check with
filename-only assertion.

Test coverage:
`scripts/release/tests/test-verify-native-release-artifacts.sh`.

## check-release-bot-secrets.sh (ADR-1171)

Preflight for release-bot identity: `gh secret list` must show both
`RELEASE_BOT_APP_ID` and `RELEASE_BOT_PRIVATE_KEY`; exit 1 when name
missing, 2 when `gh` cannot list. Invariant: `release-please.yml`
stays idle-green on `push` without secrets (warning, every write step
skipped). This script = only loud local signal that release cannot
be cut — `/prep-release` step 1 runs it, treats non-zero as NO-GO.
Never add `GITHUB_TOKEN` fallback to workflow to "fix" idle run. Test
coverage:
`scripts/release/tests/test-check-release-bot-secrets.sh` (stubbed
`gh`).
