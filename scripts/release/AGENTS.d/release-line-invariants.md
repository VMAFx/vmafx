---
paths:
  - release-please-config.json
  - .release-please-manifest.json
  - scripts/release/rollover-changelog-fragments.sh
  - scripts/release/check-one-shot-fields.sh
  - scripts/release/tests/test-check-one-shot-fields.sh
  - .github/workflows/release-please.yml
  - .github/workflows/rule-enforcement.yml
  - core/meson.build
invariant: Fork starts at v0.1.0; release-as + bootstrap-sha one-shot; SONAME independent; 1 marker/file.
---
<!-- markdownlint-disable MD013 MD060 -->
# Release-line invariants (ADR-1151, ADR-3292)

## Release-line invariants (ADR-1151)

**Fork's first release = `v0.1.0` (ADR-3292), on number line starting at fork.**
VMAFx never released a final; `v1.0.0-rc.1`..`rc.3` = withdrawn
candidates. Every other `vX.Y.Z` tag reachable from this checkout
belongs to Netflix upstream history, none is ancestor of `master`.
Never "restore" 3.x product version. `1.0.0` = first stable (Rust P3 +
frozen interface); candidates only for it, from `rc.4`.

**`release-as` and `bootstrap-sha` = ONE-SHOT.** Both live in
`release-please-config.json` only for one cut (rc.1 then; `0.1.0`
restart now). `release-as` = deprecated, *persistent* override: version
taken from it, not from commits; left in place, pins every subsequent
release to same version, swallows feat/BREAKING bumps (scratch proof:
manifest `0.1.0` + fix -> `0.1.0` again). `rollover-changelog-fragments.sh`
checks `release-as` = `--version`, deletes both in same PR that merges
release. `scripts/release/check-one-shot-fields.sh`, run by
`Release Script Contract (ADR-1128)` job in
`.github/workflows/rule-enforcement.yml`, fails if either survives once
root manifest is final (`0.x` included); test
`scripts/release/tests/test-check-one-shot-fields.sh`. To force later
version: `Release-As: X.Y.Z` commit footer — inherently one-shot, cannot
rot.

**0.x numbering:** `versioning: default`, `bump-minor-pre-major: true`,
`bump-patch-for-minor-pre-major: false`: fix -> patch, feat / feat! ->
minor. Candidate manifest (`1.0.0-rc.3`) + `versioning: default` allowed
only while `release-as` set (restart); otherwise gate fails.
1.0.0 candidates (ADR-1348): PR sets `versioning: prerelease`,
`prerelease: true`, `release-as: 1.0.0-rc.4`; then `1.0.0-rc.N` +
fix/feat/breaking -> `1.0.0-rc.N+1`; `prerelease: false` -> `1.0.0`.
`versioning: default` on rc gave `1.0.1-rc.1` (release PR #1575).

**Product version and ABI SONAME = independent.** `release-please`
owns product version (tag, `core/meson.build` `project(version:)`,
therefore `libvmaf.pc`, three Python distributions, Helm `appVersion`).
SONAME = separate `vmaf_soname_version = '3.0.0'` at
`core/meson.build:38`, producing `libvmaf.so.3`, hand-bumped only on
ABI break. No product cut (0.1.0, 1.0.0) touches it. Never align them.

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
