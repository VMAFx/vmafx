- Release pipeline repaired and retargeted onto a fresh VMAFx number line
  ([ADR-1151](docs/adr/1151-vmafx-first-release-1-0-0.md), supersedes
  ADR-1127). The fork's first release is **`v1.0.0`**: VMAFx has never
  released, and every `vX.Y.Z` tag reachable today belongs to Netflix
  upstream history, so the one-shot `release-as` moved from `3.2.1` to
  `1.0.0` on a `0.0.0` manifest. The product version (tag, `libvmaf.pc`,
  the three Python distributions, Helm `appVersion`) is explicitly
  independent of the libvmaf ABI SONAME, which keeps its own 3.x line
  (`libvmaf.so.3`) and is not reset by the cut.
- `scripts/release/verify-release-version.sh` now proves the hand-run
  changelog cut actually happened before a tag can publish — exactly one
  dated `## [X.Y.Z]` heading, a matching `changelog.d/releases/X.Y.Z.json`
  receipt, zero active fragments, no `_pre_fragment_legacy.md`, and no
  surviving one-shot `release-as` / `bootstrap-sha`. Previously nothing
  checked this and a release could have shipped a `CHANGELOG.md` whose
  newest section was still `## [Unreleased]`.
- `supply-chain.yml` fails closed unless the `release-publish` and
  `pypi-publish` deployment environments each carry a required-reviewer
  protection rule. GitHub auto-creates a referenced environment with an
  empty rule set, so the twelve write-bearing release jobs were running
  through an approval gate that did not exist.
- `release-please.yml` authenticates as a release-bot GitHub App
  installation instead of `secrets.GITHUB_TOKEN`. GitHub suppresses
  follow-on workflow events for anything `GITHUB_TOKEN` creates, so release
  PRs used to receive zero check runs and the one required context could
  never report. The job's own token drops to `contents: read`, and the
  workflow fails loudly on its first step until the maintainer-created
  `RELEASE_BOT_APP_ID` / `RELEASE_BOT_PRIVATE_KEY` secrets exist rather
  than silently falling back.
- The `Release Script Contract (ADR-1128)` gate plus five sibling
  rule-enforcement gates (deep-dive deliverables, doc-substance,
  `docs/state.md` touch, FFmpeg-patch surface sync, ADR collision) are now
  part of the Required Checks Aggregator's required set, and a release PR
  can no longer pass on check *absence* alone. The four gates that grade
  authoring discipline consult the new `scripts/ci/release-pr-exempt.sh`
  and stand down on a machine-generated release PR — which release-please
  structurally cannot satisfy — so making them required does not hand the
  release PR a permanent red check. The predicate needs a bot author as
  well as a `release-please--` head ref, so branch naming alone cannot
  disarm a required gate.
- `release-please.yml` leaves a release PR alone while it carries the
  `autorelease: cut` label, so the hand-added changelog-rollover commits
  are no longer destroyed when release-please force-recreates its branch on
  the next push to `master`.
- The moving `latest` container tag is resolved from the repository's
  newest published release instead of the trigger event, so a recovery
  dispatch no longer leaves `latest` pointing at a broken digest.
- `pkg/version/version.go` no longer documents a repo-root `VERSION` file
  that does not exist, and `docker/Dockerfile.node` no longer bakes a
  coordinated version marker that nothing in the release path reads.
