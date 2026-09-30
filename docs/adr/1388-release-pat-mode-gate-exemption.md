<!-- markdownlint-disable MD013 MD060 -->
# ADR-1388: Exempt PAT-mode release PRs from authoring-discipline gates via verified release-only diff

- **Status**: Accepted
- **Date**: 2026-09-30
- **Deciders**: lusoris
- **Tags**: ci, release, authoring-gates, security, fork-local

## Context

[ADR-1151](1151-vmafx-first-release-1-0-0.md) exempts automated `release-please` pull requests from the repository's authoring-discipline CI gates:

- Deliverables Checklist ([ADR-0108](0108-deep-dive-deliverables-rule.md))
- Path-mapped Doc-Substance Gate ([ADR-0100](0100-project-wide-doc-substance-rule.md), [ADR-0167](0167-doc-drift-enforcement.md))
- Bug-status / `docs/state.md` Gate ([ADR-0165](0165-state-md-bug-tracking.md), [ADR-0334](0334-state-md-touch-check-ci-gate.md))
- Silent-Revert Guard ([ADR-1284](1284-silent-revert-detection-gate.md))
- FFmpeg-Patches Surface Sync ([ADR-0409](0409-ffmpeg-patches-surface-gate.md))

A release PR carries neither an ADR checklist nor bug-state updates, and its coordinated version updates (e.g. `mcp-server/vmaf-mcp/pyproject.toml`) map to documentation directories (such as `docs/mcp/`) that an automated release tool has no mandate to alter. Without exemption, genuine release PRs fail these required status checks and block automated publishing.

ADR-1151 predicated exemption on two criteria:

1. The PR head branch matches `release-please--*`.
2. The PR author is a GitHub bot (`type: Bot` or username ending in `[bot]`).

When running release automation via GitHub App tokens (`RELEASE_BOT_APP_ID` / `RELEASE_BOT_PRIVATE_KEY`), GitHub attributes commits to the App bot identity (`github-actions[bot]` or the configured bot), satisfying the author check. However, in personal access token (PAT) fallback mode via `RELEASE_BOT_TOKEN`, GitHub attributes the PR and its commits to the PAT account owner (`lusoris`, with `type: User`).

As identified in issue #1608, ADR-1151 fails on PAT-mode release PRs: the author check fails closed, subjecting machine-generated release PRs to authoring gates designed for human contributors. The Doc-Substance gate rejects the PR due to unexempted version bumps, and the Deliverables Checklist fails due to missing checkboxes.

Simply exempting any human author on a `release-please--*` branch would create a major security hole: any contributor could push arbitrary code changes to a branch named `release-please--*` and bypass documentation, state tracking, and revert guards.

## Decision

Extend `scripts/ci/release-pr-exempt.sh` and associated pre-push validation with a secure, fail-closed PAT exemption protocol based on verified release diffs.

1. **Dual-Path Exemption**:
   - **Bot Mode (ADR-1151)**: If `head_ref` matches `release-please--*` and author is `Bot` or `*[bot]`, immediately exempt the PR.
   - **PAT Mode (ADR-1388)**: If `head_ref` matches `release-please--*` and author matches the designated PAT username (`RELEASE_BOT_PAT_USER`, default `lusoris`), evaluate the PR diff.
   - **Unauthorized Authors**: If any other user authors or pushes to a `release-please--*` branch, deny exemption unconditionally.

2. **Verified Release-Only Diff Invariant**:
   In PAT mode, exemption requires that 100% of files modified in the PR diff belong strictly to the approved release file set:
   - Root manifest and config: `.release-please-manifest.json`, `release-please-config.json`
   - Release notes and changelogs: `CHANGELOG.md`, `changelog.d/*`, `docs/changelog-archive/*`
   - Coordinated version markers dynamically parsed from `release-please-config.json` (`.packages["."]."extra-files"`), falling back to the 10 canonical files if unreadable:
     - `core/meson.build`
     - `compat/python-vmaf/__init__.py`
     - `ai/pyproject.toml`
     - `ai/src/vmaf_train/__init__.py`
     - `dev-llm/pyproject.toml`
     - `dev-llm/src/vmaf_dev_llm/__init__.py`
     - `mcp-server/vmaf-mcp/pyproject.toml`
     - `mcp-server/vmaf-mcp/src/vmaf_mcp/__init__.py`
     - `deploy/helm/vmafx/Chart.yaml`
     - `build-config.env`

   If the diff touches even a single file outside this whitelist (e.g. C source, CI workflows, test scripts), or if the diff is empty / cannot be determined, exemption is denied (fails closed) and all authoring gates remain armed.

3. **Diff Input Sources**:
   `release-pr-exempt.sh` inspects the diff through three supported mechanisms:
   - `DIFF_FILE`: Pre-generated path list file, or `-` for standard input.
   - `BASE_SHA` and `HEAD_SHA`: Git commit range `BASE_SHA...HEAD_SHA` (with automatic fetch fallback for shallow CI clones).
   - Local Git repository: Compares merge-base against `origin/master` or `HEAD`.

4. **Workflow Integration**:
   `.github/workflows/rule-enforcement.yml` exports `BASE_SHA` and `HEAD_SHA` to the `steps.release_pr` step in all five authoring-discipline jobs:
   - `deliverables-check`
   - `doc-substance-check`
   - `state-md-check`
   - `silent-revert-check`
   - `ffmpeg-patches-surface-check`

5. **Local Pre-Push Hook Alignment**:
   `scripts/git-hooks/pre-push-pr-body-lint.sh` generates a staged diff against `origin/master` when pushing a `release-please--*` branch, ensuring local pre-push validation accurately mirrors CI gate evaluation.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Verified release-only diff for designated PAT user (chosen) | Resolves issue #1608; allows PAT-mode automation to pass gates; prevents arbitrary code bypass on release branches | Requires diff collection and parsing in exemption script | Secure and preserves integrity of authoring gates |
| Blanket exemption for branch name `release-please--*` | Trivial implementation | Severe security vulnerability: any developer or attacker pushing to such a branch bypasses all authoring and doc gates | Fails basic security and auditability standards |
| Hardcode PAT user `lusoris` without diff check | Simple check | Allows `lusoris` to accidentally or intentionally bypass gates on non-release commits on release branches | Fails defense-in-depth principle |
| Force GitHub App token only; disallow PAT mode | No script changes needed | Unusable when GitHub App permissions or private keys are unavailable or revoked | PAT fallback is a required operational fallback |

## Consequences

- **Positive**: PAT-mode release PRs from `release-please` cleanly pass authoring-discipline CI gates without administrative override; automated release train can operate seamlessly under both App and PAT credentials.
- **Positive**: Rigorous defense-in-depth: non-release edits accidentally pushed to `release-please--*` branches cannot bypass gates.
- **Negative**: Adds a diff inspection step in PAT mode, requiring `BASE_SHA` and `HEAD_SHA` to be available.
- **Neutral / follow-ups**: Automated regression test suite `scripts/ci/tests/test-release-pr-exempt.sh` covers 16 distinct matrix cases for bot, PAT, clean release diffs, dirty diffs, and non-whitelisted authors.

## References

- Issue #1608: `ci(release): PAT-mode release PRs are not exempt from the authoring gates (ADR-1151)`
- [ADR-1151](1151-vmafx-first-release-1-0-0.md): Automated release PR exemption from authoring-discipline gates
- [ADR-0108](0108-deep-dive-deliverables-rule.md): Deliverables checklist required for human PRs
- [ADR-0100](0100-project-wide-doc-substance-rule.md), [ADR-0167](0167-doc-drift-enforcement.md): Path-mapped documentation substance gate
- [ADR-0165](0165-state-md-bug-tracking.md), [ADR-0334](0334-state-md-touch-check-ci-gate.md): State and bug ledger update protocol
- [ADR-1284](1284-silent-revert-detection-gate.md): Silent revert guard against merge regression
- [ADR-0409](0409-ffmpeg-patches-surface-gate.md): FFmpeg patches surface synchronization
- `scripts/ci/release-pr-exempt.sh` and `scripts/ci/tests/test-release-pr-exempt.sh`
