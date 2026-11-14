<!-- markdownlint-disable MD013 MD025 MD060 -->
# ADR-3544: Refuse bot-authored commits outside the bot's own branch

- **Status**: Accepted
- **Date**: 2026-11-13
- **Deciders**: Project maintainer
- **Tags**: ci, git, dco, fork-local

## Context

Pull request 3426 included commit `66ed53cec`, authored by `coderabbitai[bot]` and signed off by its human committer. Both PR and pre-push DCO checks accepted that commit. The squash landing on master `c4df0c892` then failed the landed-message check because GitHub added a `Co-authored-by:` trailer naming the bot. The identity checks introduced by ADR-3531 deliberately exclude the `[bot]` token, allowing legitimate bot branches, but consequently did not prevent this landing failure.

Bot ownership and sign-off exemption are separate decisions. Render PRs opened by `vmafx-release-bot[bot]` have commits authored by that bot, which is outside the sign-off exemption list `BOT_LOGINS`. Those commits must remain acceptable when signed off.

## Decision

In PR and pre-push modes of `scripts/ci/check-dco.py`, refuse a commit when its author name ends in `[bot]` or its author email ends in `[bot]@users.noreply.github.com`, case-insensitively, unless the commit belongs to the branch opener. Share this check across the normal and sign-off-exempt PR paths. Keep landed-mode behavior unchanged.

For a PR, require `--author-type Bot` and an author email ending with `bot_noreply_suffix(--author)`. Do not consult `BOT_LOGINS` for ownership; that list continues to control only sign-off exemptions. For pre-push mode, use the exact author name and email of the oldest non-merge commit in the outgoing range as the opener identity. Later commits with that same identity remain acceptable when a human updates the branch. An empty range has no commits to judge.

The refusal names the bot and explains that a squash landing would carry its identity as a `Co-authored-by:` trailer refused by the landed gate. Repair the affected commit by re-authoring it with `git commit --amend --reset-author --no-edit`, or drop it. For an earlier commit, stop on that commit during an interactive rebase before amending it; retain the required sign-off.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Check bot authors against the branch opener | Reuses existing commit parsing; catches the failure before landing; preserves bot-owned branches | Pre-push must infer ownership from the oldest outgoing commit | Chosen; PR metadata provides ownership in CI and fixtures cover both modes |
| Reuse `is_exempt_bot_commit()` | Existing helper checks matching bot email | Requires `BOT_LOGINS` membership and would reject signed render-bot commits in their own PRs | Conflates ownership with sign-off exemption |
| Reject every bot author | Simple identity rule | Rejects legitimate dependency and render PRs | Bot-owned branches remain supported |
| Rely only on the landed gate | Existing alarm requires no new check | Detects the defect after it reaches master | Does not prevent recurrence before landing |

## Consequences

- **Positive**: a human branch containing a review bot's commit fails PR and pre-push checks even when its committer supplied a valid sign-off. Release and grandfathered sign-off exemptions cannot bypass this rule. Bot-owned PRs, including bots outside `BOT_LOGINS`, retain their existing sign-off obligations.
- **Negative**: pre-push ownership is an inference from the outgoing range rather than GitHub metadata. Changing that range can change the inferred opener; PR mode independently checks the actual opener metadata.
- **Neutral / follow-ups**: plain names such as `robot` and `bot` remain valid. The shared model/tool attribution checks and landed-mode sign-off and trailer rules remain in force. No published history changes are required. Regression fixtures cover author-field suffixes, case boundaries, bot ownership, mixed-author branches, and sign-off exemptions; removing the new refusal must make the negative fixtures fail.

## References

- [ADR-3531: Refuse tool attribution in pull request commits and bodies](3531-dco-attribution-identity-refusal.md).
- [ADR-3389: Landed commit message gate and bot squash sign-off](3389-landed-commit-message-gate.md).
- [Contributor DCO guide](../development/dco.md#attribution-rules).
- `scripts/ci/tests/test_check_dco_attribution.py`: PR bot-author refusal and opener fixtures.
- `scripts/ci/tests/test_check_dco_pre_push.py`: oldest-author ownership and mixed-author branch fixtures.
