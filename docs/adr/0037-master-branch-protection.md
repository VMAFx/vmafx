<!-- markdownlint-disable MD013 MD060 -->
# ADR-0037: Protect master branch on GitHub with required checks

- **Status**: Accepted
- **Date**: 2026-04-17
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: github, ci, security, release

## Context

The fork's CLAUDE.md §12 rules 2 and 3 prohibited force-push to `master` and mandated linear history, but those were client-side conventions — nothing stopped a slip. GitHub branch protection enforces the same rules at the host layer, and adds required status checks so merges cannot land while CI is red. User: "perhaps just protect it now, i asked about this like 3 times already".

## Decision

Protect `master` on GitHub (enabled 2026-04-17) with 19 required status checks (pre-commit, ruff+mypy+black, semgrep, Netflix CPU golden (ADR-0024), ASan/UBSan/MSan ×3, Assertion density, CodeQL ×4, clang-tidy, cppcheck, Tiny AI, MINGW build, dependency-review, gitleaks, shellcheck+shfmt). Settings: `strict=false` (branch does not need rebase onto master to merge), `required_linear_history: true`, `allow_force_pushes: false`, `allow_deletions: false`, `enforce_admins: false` (owner keeps emergency bypass), `required_pull_request_reviews: null` (solo dev). Deliberately not required: Coverage gate (~40 min, finicky), GPU-advisory jobs, Semgrep OSS — non-blocking signals.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| No host protection, rely on hooks | Zero host config | Any slip bypasses CLAUDE.md rules | User explicitly rejected |
| Strict=true (require rebase before merge) | Always linear | Busy-work rebases for solo dev | Rejected |
| Require reviews (1+) | Extra eyes | Solo dev — no second reviewer available | Not applicable |
| `strict=false` + 19 checks (chosen) | Host-enforced linear history + critical gates; no busy-work | Must curate required-check list | Correct for solo workflow |

Rationale: strict=false avoids solo-dev busy-work; 19 checks covers every failure mode that matters at merge time; Coverage + GPU-advisory stay informational because they are slow/flaky and not true correctness gates.

## Consequences

- **Positive**: force-push and deletion impossible at host layer; merge gated on the 19 checks; linear history guaranteed.
- **Negative**: any check marked "required" that flakes blocks merge.
- **Neutral / follow-ups**: CLAUDE.md §12 rules 2 and 3 are now dual-enforced; `docs/development/release.md#master-branch-protection` documents.

## References

- Source: `req` (user: "perhaps just protect it now, i asked about this like 3 times already")
- Related ADRs: ADR-0002, ADR-0024, ADR-0015
