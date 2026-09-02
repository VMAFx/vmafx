<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1152: Exempt Dependency-Only Bot PRs from Documentation Gates

- **Status**: Accepted
- **Date**: 2026-09-03
- **Deciders**: Kilian, Antigravity
- **Tags**: ci, process, docs, dependencies

## Context

The workflow `.github/workflows/rule-enforcement.yml` runs "Doc-Substance Gate (ADR-0100 / 0167)" and "Deep-Dive Deliverables Checklist (ADR-0108)". These checks verify that every non-trivial contribution ships with required process deliverables (research digests, decision records, changelog fragments, rebase notes) and user-discoverable documentation under `docs/`.

Automated dependency-update pull requests opened by `renovate[bot]` or `dependabot[bot]` cannot satisfy either gate: automated dependency updates do not write architectural decision records, research digests, or human documentation. Consequently, every dependency PR is permanently red on both checks — as observed in PR #1206 (both), PR #1207 (deliverables), PR #1212 (both), and PR #1214 (deliverables).

Because these documentation checks are not in the required-checks aggregator list, they do not block merging. However, they create persistent false-positive red badges across all dependency PRs. This failure fatigue trains reviewers and developers to ignore failing checks, masking real failures and undermining confidence in the CI gate pipeline.

## Decision

We will exempt strictly dependency-only bot pull requests from the Doc-Substance Gate (ADR-0100 / ADR-0167) and the Deep-Dive Deliverables Checklist (ADR-0108) via a shared helper script (`scripts/ci/classify-dependency-pr.sh`) invoked early in both jobs.

Exemption requires **BOTH** conditions to hold:

1. **Bot Identity**: The PR author (`github.event.pull_request.user.login`) is `renovate[bot]` or `dependabot[bot]` (or `app/renovate` / `app/dependabot`), OR the head branch matches `renovate/*` or `dependabot/*`.
2. **Strict Manifest-Only Paths**: Every changed path in the PR diff matches an explicit, conservative allowlist of dependency manifests and lockfiles:
   - `renovate.json`
   - `.github/renovate.json*`
   - `package.json`, `package-lock.json`, `pnpm-lock.yaml`, `yarn.lock`
   - `go.mod`, `go.sum`
   - `Cargo.toml`, `Cargo.lock`, `deny.toml`
   - `pyproject.toml`, `poetry.lock`, `uv.lock`, `tox.ini`
   - `requirements*.txt`, `constraints*.txt`
   - `.pre-commit-config.yaml`
   - `Dockerfile*`, `docker/**`, `dev/Containerfile`
   - `.github/workflows/**` (action version pins)
   - `changelog.d/**`

If ANY other path is touched — including any source or test code under `core/`, `ai/`, `python/`, `compat/`, `cmd/`, `pkg/`, `internal/`, `bindings/`, `tools/`, `scripts/`, `docs/`, or `model/` — the PR is **NOT** dependency-only and both documentation gates run normally. A bot PR that modifies source code must still be gated: that asymmetry is essential to ensure documentation and design rigor is never bypassed.

When a PR is classified as dependency-only, the classifier logs an informational GitHub Actions `::notice::` specifying the author, branch, and changed paths, and sets `exempt=true` in step outputs, allowing the jobs to succeed cleanly without executing the deliverables or doc-coverage body checks.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Make documentation gates required and force Renovate PRs to carry sentinels | Keeps gates uniform across all PR types without adding exemption logic | Requires maintainers to manually edit every bot PR body or maintain fragile Renovate body templates to insert canned checklists | Defeats the automated nature of dependency updates; fake checklists add noise without providing real review |
| Turn off documentation gates entirely | Eliminates all false positives immediately; zero maintenance | Stops automated enforcement of ADR-0100, ADR-0108, and ADR-0167 across the repository | Unacceptable regression in repository architectural and documentation hygiene |
| Configure Renovate to write a canned PR body with valid sentinels | Keeps workflow YAML untouched | Renovate PR descriptions are regenerated/overwritten on rebase; does not resolve Doc-Substance path-mapping check (ADR-0167) which inspects the git diff under `docs/` | Ineffective for the Doc-Substance gate and high maintenance overhead across package managers |

## Consequences

- **Positive**: Routine bot dependency updates no longer display failing red badges on documentation gates; CI badges accurately reflect build and test health.
- **Negative**: Adds a classification script (`scripts/ci/classify-dependency-pr.sh`) and step wiring in `rule-enforcement.yml` that must be maintained.
- **Neutral / follow-ups**: If new dependency manifest or lockfile formats are introduced into the project, they must be registered in the allowlist in `scripts/ci/classify-dependency-pr.sh` and covered in its test suite.

## References

- PR #1206: chore(deps): Update dependency torch to v2.14.0 (failed both gates)
- PR #1207: chore(deps): Update docker/dockerfile Docker tag to v1.27 (failed deliverables gate)
- PR #1212: chore(deps): Update dependency anyio to >=4.15.0 (failed both gates)
- PR #1214: chore(deps): migrate Renovate config (failed deliverables gate)
- [ADR-0100](0100-project-wide-doc-substance-rule.md): Project-Wide Documentation Substance Rule
- [ADR-0108](0108-deep-dive-deliverables-rule.md): Deep-Dive Deliverables Rule
- [ADR-0167](0167-doc-substance-gate-blocking-and-path-mapped.md): Path-Mapped Doc-Substance Gate
