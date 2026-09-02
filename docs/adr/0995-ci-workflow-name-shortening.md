# ADR-0995: Shorten CI workflow and job display names

- **Status**: Accepted
- **Date**: 2026-06-03
- **Deciders**: Lusoris
- **Tags**: `ci`, `github-actions`, `docs`

## Context

The fork's CI workflow display names (`name:` in `.github/workflows/*.yml`) and
several prominent job names grew long during iterative development — some exceeding
80 characters with em-dash separators, enumerated sub-systems, and ADR citations.
These names appear verbatim in README badges, the GitHub Actions UI workflow and
job rollup column, the PR check status section, and terminal output of
`gh run watch`. Long names truncate at around 40 characters in the PR status
column and wrap awkwardly in the badge row. The sub-system details that justified
the long names (e.g., "Netflix Golden / Sanitizers / Tiny AI / Coverage") are
redundant: the workflow filename already identifies it, and the individual job
names within the workflow provide per-component detail. ADR-0116 originally
codified the verbose convention; this ADR supersedes that naming policy.

## Decision

Rename all workflow-level `name:` fields and the `Docs Build — mkdocs strict
(PR gate)` job name to 1-2 word short forms. Update `required-aggregator.yml`
to reference the renamed job name. Update README badge labels and operational
`gh workflow run` usages in `docs/development/ci-tmate-debug.md` and
`docs/state.md` to match.

Workflow name renames:

| File | Old | New |
| --- | --- | --- |
| `tests-and-quality-gates.yml` | `Tests & Quality Gates — Netflix Golden / …` | `Tests` |
| `lint-and-format.yml` | `Lint & Format — Pre-Commit / Clang-Tidy / …` | `Lint` |
| `libvmaf-build-matrix.yml` | `libvmaf Build Matrix — Linux/macOS/Windows/ARM × …` | `Builds` |
| `ffmpeg-integration.yml` | `FFmpeg Integration — Linux/macOS × gcc/clang + …` | `FFmpeg` |
| `security-scans.yml` | `Security Scans — Semgrep / CodeQL / …` | `Security` |
| `sanitizers.yml` | `Sanitizers + Fuzz` | `Sanitizers` |
| `rule-enforcement.yml` | `Rule Enforcement — ADR-0100 / 0106 / 0108` | `Rules` |
| `required-aggregator.yml` | `Required Checks Aggregator` | `CI` |
| `rust-ci.yml` | `Rust CI` | `Rust` |
| `go-ci.yml` | `Go CI` | `Go` |
| `build.yml` | `Build — 1 per OS` | `Build` |
| `docker-image.yml` | `Docker Image Build` | `Docker` |

Job name renames (check names visible in PR status and `required-aggregator.yml`):

| Workflow | Old job name | New | Aggregator updated? |
| --- | --- | --- | --- |
| `lint-and-format.yml` | `Docs Build — mkdocs strict (PR gate)` | `Docs` | Yes |
| `tests-and-quality-gates.yml` | `Perf regression gate (CPU wall-clock, ADR-0907)` | `Perf` | No (advisory) |
| `tests-and-quality-gates.yml` | `Coverage Gate (70% Overall / 90% Critical — ADR-0922)` | `Coverage` | No (advisory) |

The `Required Checks Aggregator` job name (within `required-aggregator.yml`) is
intentionally **not** renamed because it is the branch-protection context name
(`"contexts": ["Required Checks Aggregator"]` in the GitHub API settings);
changing it requires a UI/API change outside this PR.

Also fixes three pre-existing conflict markers (`<<<<<<< HEAD`) in
`libvmaf-build-matrix.yml` and `security-scans.yml` that survived the ADR-0700
`core/` rename merge train; resolves to HEAD (`core\` path) in all cases.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Keep verbose names | Self-documenting, no migration needed | Truncate in PR status, clutter badge row | File name + job names already provide the detail |
| Two-word names (e.g., "Build Matrix") | More descriptive than one word | Still add visual noise; badge row cramped | One word suffices for all covered workflows |
| Only rename badges, keep workflow names | Zero CI risk | Mismatch between badge label and Actions UI | Inconsistent; not chosen |

## Consequences

- **Positive**: Badges in README are visually compact; PR status column no longer
  truncates; `gh run watch` output fits in 80-column terminals.
- **Negative**: Existing links to `Actions → libvmaf Build Matrix` in ADR bodies,
  research docs, and rebase-notes become descriptively stale (they still resolve
  because GitHub redirects by filename, but the display name differs). Historical
  research and ADR files referencing old names are left as-is (they record state
  at time of writing).
- **Neutral / follow-ups**: The `Required Checks Aggregator` job name (branch
  protection context) requires a separate API/UI change; tracked as a follow-up.
  ADR-0116 is superseded by this ADR for the workflow naming policy.

## References

- ADR-0116 (ci-workflow-naming-convention) — superseded by this ADR.
- ADR-0313 (ci-required-checks-aggregator).
- Source: per user direction — shorten workflow and job display names for badge
  and rollup readability.
