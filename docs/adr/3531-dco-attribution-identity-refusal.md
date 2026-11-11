<!-- markdownlint-disable MD013 MD025 MD060 -->
# ADR-3531: Refuse tool attribution in pull request commits and bodies

- **Status**: Accepted
- **Date**: 2026-11-11
- **Deciders**: Project maintainer
- **Tags**: ci, git, dco, agents, fork-local

## Context

Fork pull request 1 carried a generated-attribution footer, and commit `0ef1593ef` carried a tool co-author trailer into a pull request. A sign-off alone did not prevent tool identities or attribution from reaching review: the pull request path checked sign-offs while the landed-message path checked attribution after merge. The repository needs a check before landing as well as the existing alarm afterward. The input is contributor-controlled, so delimiter injection, invisible prefixes, and excessive input must fail safely under the bounded-input principle in [principles.md](../principles.md).

## Decision

Extend `scripts/ci/check-dco.py` to refuse model/tool identities in commit author and committer fields, tool `Co-authored-by:` trailers, and generated footers in pull request commits and the pull request body. Share the attribution helpers with landed mode, keep the token list equal to the standards engine's `tools/planning/commit_msg_check.py` list, normalize message/body lines with NFKC and remove leading Unicode format characters, read CI bodies from the event file, and reject input above named byte/line bounds. Keep existing sign-off and release/cutoff exemptions.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Extend the existing DCO check | Reuses commit parsing and line rules; rejects before landing | More contributor-controlled input to validate | Chosen; bounded reads, NUL delimiters, and planted regression cases cover the added input |
| Engine-only scan at squash time | Central policy and one final message | Branch commits and PR body can still carry attribution throughout review | Too late to enforce the review-surface policy |
| Commit-message hook only | Immediate local feedback | Contributors can omit local hooks; author and body changes escape it | No independent required CI barrier |
| CODEOWNERS review | Human context for ambiguous names | Manual repetition; no deterministic refusal or input bound | Does not provide repeatable enforcement |

## Consequences

- **Positive**: the same attribution policy runs before landing and in the landed-commit alarm; bot sign-off exemptions do not bypass attribution checks. The shared linear prefix scanner strips list markers (including `+` and terminated one-to-nine-digit ordered markers) and interleaved Unicode `Cf` characters after NFKC normalization; identity matching applies the same NFKC normalization, removes every `Cf` character, and then uses case-insensitive substring tokens with the existing `[bot]` exclusion.
- **Negative**: substring tokens can collide with human identities, and quoted or fenced attribution examples can be refused. The 2026-11-11 scan measured zero matches across 6,290 master identity records and 300 PR bodies; that is an observed false-positive surface, not a guarantee for future contributors. The [digest](../research/3531-dco-attribution-scan.md) records both corpora, their fingerprints and the rerun command.
- **Neutral / follow-ups**: token changes must remain synchronized with the engine. Byte/line limits require a recorded corpus and stated headroom; regression tests cover exact-bound and bound-plus-one input. CI uses event-file input; local body files and the environment fallback remain supported. No history rewrite is required.

## References

- [ADR-2462: DCO sign-off required](2462-dco-sign-off-required.md).
- [ADR-3389: Landed squash commit message check](3389-landed-commit-message-gate.md).
- [Research digest and reproducible corpus scan](../research/3531-dco-attribution-scan.md).
- `scripts/ci/tests/test_check_dco.py`: positive, negative, delimiter, Unicode, event-file, and input-bound fixtures.
- Decision `vmafx-attribution-scan-placement`, option c, 2026-11-11, attribution scan placement (paraphrased): enforce the repository half in `check-dco.py` on PR commits and bodies.
