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

Version shape = verifier's shape, no wider: `X.Y.Z` or `X.Y.Z-rc.N`
(ADR-1201). RC cut = own dated section + `releases/X.Y.Z-rc.N.json`
receipt; rc.1 cut retires one-shot fields, as `verify-release-version.sh`
demands at every tag. Marker extractor keeps optional `-rc.N` group, same
as verifier; without it `1.0.0-rc.1` marker reads `1.0.0`, mismatches.

Archive path `docs/changelog-archive/X.Y.Z.md` exempt from 1 MB
`check-added-large-files` gate (ADR-1345): exclusion
`^docs/changelog-archive/[^/]+\.md$` in `.pre-commit-config.yaml`. Move
archive path -> move exclusion in same PR; never widen it. T18 pins both.

Test coverage:
`scripts/release/tests/test-rollover-changelog-fragments.sh` (T14 runs
verifier against RC cut).

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
Later RCs need no footer (ADR-1348): `versioning: prerelease` bumps
`1.0.0-rc.N` + fix/feat/breaking -> `1.0.0-rc.N+1`; `prerelease: false`
-> `1.0.0`. `versioning: default` gave `1.0.1-rc.1` (release PR #1575).
Release Script Contract fails on non-`prerelease` versioning while
manifest = rc.

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

## pep440-version.sh (ADR-1201)

Tag, manifest, markers carry SemVer spelling (`1.0.0-rc.1`). Hatchling
names vmaf-mcp wheel + sdist after PEP 440 normalized version:
`vmaf_mcp-1.0.0rc1-py3-none-any.whl`, `vmaf_mcp-1.0.0rc1.tar.gz`.
`supply-chain.yml` `validate-release` derives `pep440_version` output
once via this script; every version-bound `vmaf_mcp-*` filename glob and
PyPI JSON release URL use it (`VMAFX_PEP440_VERSION`). SemVer glob
matches zero wheels on RC -> `mcp-build` fails, SBOM / signing / PyPI /
attachment skipped.

Converter exact for verifier's two shapes only: `X.Y.Z` -> `X.Y.Z`,
`X.Y.Z-rc.N` -> `X.Y.ZrcN`; anything else exit 64, no output. No
`packaging` import: `validate-release` installs nothing, and ADR-1305
forbids unhashed install. Widen verifier shape -> widen converter + test
in same PR.

SBOM identity checks stay on SemVer `version`, not `pep440_version`:
hatchling 1.32 writes pyproject spelling (`1.0.0-rc.1`) into METADATA;
Syft 1.51 copies it verbatim into SPDX `versionInfo` and purl
`pkg:pypi/vmaf-mcp@1.0.0-rc.1`. Filenames normalized, metadata not --
never "fix" one to match other. Hatchling or Syft bump -> re-check both.

Version-bound sdist pattern has no wildcard: nullglob keeps literal
path. Every version-bound count check also requires
`-f "${sdists[0]}"`.

Test coverage: `scripts/release/tests/test-pep440-version.sh`
(converter cases, workflow wiring, glob resolution against
hatchling-shaped filenames with decoys, SemVer-regression fixtures).

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

Version check (ADR-1201). `VERSION` argument = narrow tag shape minus
`v`: `X.Y.Z` or `X.Y.Z-rc.N`, no leading zero; anything else exit 64.
`vmaf --version` prints `VMAF_VERSION` from `core/include/meson.build`
`git describe --tags --long --match 'v*.*.*'`, so build checked out
at tag normally reports `vX.Y.Z[-rc.N]-0-g<hex>`, not bare `X.Y.Z`
(reproduced on real CPU builds: `v1.0.0-rc.1-0-gd0f0e7e`,
`v1.0.0-0-g8820048`). Accept exactly two strings: that describe form
(distance `0`, lowercase hex 7–64) or bare `X.Y.Z[-rc.N]` vcs_tag
fallback (tagless checkout; `verify-release-version.sh` pins
`core/meson.build` marker to tag). Never prefix-match: `-N-g` with N>0
= commit after tag, other suffix = not tagged tree (`-dirty` rejected as
defence in depth; `vcs_tag` passes no `--dirty`, so it never appears), `rc.10`
never satisfies `rc.1`. Changing `vcs_tag` command or `--version`
output format -> update verifier + test fixtures in same PR.

Test coverage:
`scripts/release/tests/test-verify-native-release-artifacts.sh`.

## build-native-release-artifacts.sh (ADR-1346)

Runs inside `build-deps` stage image, never on runner host:
`supply-chain.yml` `build-artifacts` builds stage from release tag via
`scripts/ci/build-dev-container-stage.sh build-deps`, then
`docker run --pull never --network none` as runner UID/GID (usually no
passwd entry in image -> HOME=/) with checkout mounted. Dev Container PR gate rehearses
same invocation with manifest version and local tag `v<version>` on HEAD.
Order fixed: `--assert` container marker first (host run compiles nothing),
`GITHUB_SHA` set -> `git rev-parse HEAD` must equal it (stamp records
`GITHUB_SHA` as `git_commit`; mismatch or unresolvable HEAD = exit 1 before
Meson), Meson build, stage, `--stamp`, `verify-native-release-artifacts.sh`.
Meson flags keep hosted-era bundle: `--buildtype=release
-Denable_avx512=true -Denable_cuda=false -Denable_sycl=false` plus
`-Denable_dnn=disabled` (no-op in `build-deps`, which has no ONNX Runtime;
pinned so a stage that ships ORT cannot make `libvmaf.so` NEED
`libonnxruntime.so.1`) and `CCACHE_DISABLE=1` (load-bearing: `build-deps`
ships ccache, Meson auto-wraps compiler, ccache as HOME=/ UID fails every
compile with `Permission denied`). Full default target set (tests included)
links only because `build-deps` registers `gcc-ar`/`gcc-nm`/`gcc-ranlib` as
`update-alternatives` slaves of `gcc`; without them Meson falls back to plain
`ar`, which cannot index GCC LTO objects there. `SOURCE_DATE_EPOCH` scoped
to build subshell; stamp keeps wall-clock `stamped_at`. Bundle needs glibc
>= 2.43 (Ubuntu 26.04 image), so `verify-native-artifacts` runs
`ubuntu-26.04`; never move it back to `ubuntu-latest` while `ubuntu-latest`
is 24.04. Release-track (Debian 13) build = deferred
`T-RELEASE-NATIVE-BUNDLE-RELEASE-TRACK-2026-09-27`, due before final 1.0.0.

Test coverage:
`scripts/release/tests/test-build-native-release-artifacts.sh` (stub
`meson`, real ELF fixture chain, `GITHUB_SHA` match / mismatch /
abbreviated / unset / no-Git cases, no Docker).

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
