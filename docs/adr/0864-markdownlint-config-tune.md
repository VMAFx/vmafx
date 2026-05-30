# ADR-0864: Markdownlint config tuned for prose-heavy docs/ + autofix safety

- **Status**: Accepted
- **Date**: 2026-05-30
- **Deciders**: lusoris
- **Tags**: docs, lint, hygiene, config

## Context

The fork's `.markdownlint.json` shipped with `default: true` plus two
customisations (MD013 line-length on prose, MD024 siblings-only). Running
`markdownlint-cli2` against `docs/` + `changelog.d/` surfaced 19,748
warnings across 2,742 files, dominated by:

- **10,378** MD060 (table-column-style) — every compact `|---|---|`
  table is flagged because the rule wants single-space padding. The
  project's tables are uniformly compact by convention.
- **4,557** MD013 (line-length) — most are research-digest dumps with
  long URLs, multi-line GitHub link references, or quoted source text
  that has no natural break point.
- **1,434** MD041 (first-line-heading) — every `changelog.d/<section>/<topic>.md`
  fragment is body-only by design (the heading is rendered in
  `CHANGELOG.md` from the directory hierarchy).
- **666** MD032 / **533** MD022 / **281** MD031 — blanks-around-X rules;
  safely auto-fixable.

The auto-fixer (`markdownlint-cli2 --fix`) is **unsafe** for several of
the default-enabled rules in this corpus:

- MD004 (ul-style) rewrites `+` to `-` even when `+` is prose-conjunction
  in a wrapped list-item continuation line — silently changing meaning
  (e.g. "BVI-DVC test fold + Netflix Public Drop corpus" → "… - Netflix
  …", flipping a list-of-features into a list-with-negation).
- MD007 (ul-indent) unindents reference-list sub-bullets that depend on
  the indentation for visual hierarchy.
- MD018 (no-missing-space-atx) turns `#140` (PR number) at start of line
  into `# 140` (H1 heading).
- MD029 (ol-prefix) renumbers ordered lists that intentionally use
  non-sequential numbering for PR/commit references.
- MD037 (no-space-in-emphasis) eats spaces around `*` and `_` even when
  they are arithmetic operators (`H = 0.5 * log2(...)`) or part of
  identifiers (`FEATURE_NAMES, _METRIC_ALIASES`).
- MD038 (no-space-in-code) eats spaces before backtick-prose like
  `at \`peak\``.
- MD027 (no-multiple-space-blockquote) collapses license-block quote
  indentation in reference pages.

None of the markdownlint rules are wired into `make lint`, the
pre-commit pipeline, or any CI gate today. They produce only adversarial
review noise — but per CLAUDE.md §12 rule 12, every PR must leave every
file it touches lint-clean to the fork's strictest profile. With 19,748
pre-existing warnings, the touched-file rule becomes impossible to
satisfy on innocent PRs that brush a docs neighbour.

## Decision

Tune `.markdownlint.json` to:

1. Keep `default: true` as the baseline.
2. Disable the **10 noise rules** that conflict with the project's
   documentation conventions: MD025, MD033, MD034, MD036, MD041, MD046,
   MD049, MD050, MD060 (cosmetic / convention-incompatible).
3. Disable the **7 unsafe-to-autofix rules**: MD004, MD007, MD018,
   MD027, MD029, MD037, MD038.
4. Widen MD013 to also exempt headings (in addition to tables + code
   blocks already exempted) — keeps the 80-col limit on prose body.

Then run `markdownlint-cli2 --fix` across `docs/` and `changelog.d/`
with this config to discharge the safe whitespace cleanups
(MD012/MD022/MD030/MD031/MD032 — blanks around headings/lists/fences
and consistent list-marker spacing) that the autofixer can apply
without semantic risk.

Files owned by in-flight DRAFT PRs (as of 2026-05-30 snapshot) and all
`docs/adr/[0-9]{4}-*.md` files are excluded from the sweep — ADRs are
immutable once Accepted (per the global CLAUDE.md user-rule), and
in-flight PR files would cascade into merge conflicts. The ADR-0108
compliance-audit research file is also excluded (audit-trail content
per the ADR-0108 deliverables rule).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **(Chosen) Tune config + run safe autofix on non-conflicting files** | Discharges the pre-existing debt that gates the touched-file rule; future PRs brushing docs/ are no longer gated by neighbour drift; reversible (any disabled rule can be re-enabled) | Some cosmetic rules permanently off; sweep PR covers many files | Pragmatic balance: zero-risk autofix on safe rules, surgical opt-out for unsafe ones |
| Wire markdownlint into `make lint` + CI gate AS-IS, leave 19k warnings | Forces every PR to confront the debt | Touched-file rule becomes impossible to satisfy on innocent docs neighbours; new contributors get a wall of red on first PR | Adversarial without value — most of those 19k are convention-mismatch, not real issues |
| Disable markdownlint entirely (`default: false`) | Zero lint noise | Loses the legitimate gates (heading consistency, blanks-around, dup-heading detection) that catch real readability bugs | Throws out useful coverage with the noise |
| Manually edit every offending table / line / list to comply with `default: true` | Maximally compliant | ~19k touches across 2,742 files; would conflict with every in-flight PR; meaning-changing risk on every edit | Scope-explosion, no proportional value |

## Consequences

- **Positive**:
  - Touched-file lint-clean rule (CLAUDE.md §12 r12) is achievable for
    docs-adjacent PRs without 19k pre-existing warnings as a blocker.
  - 218 of the 2,742 markdown files now lint-clean to the tuned profile.
  - Future PRs that auto-fix on commit (e.g. via `markdownlint-cli2 --fix`
    in a future pre-commit hook) will not silently destroy meaning via
    MD004/MD018/MD029/MD037/MD038/MD007.
- **Negative**:
  - Lint coverage narrower than the `default: true` baseline. If a
    future maintainer wants stricter coverage on a specific surface,
    they must re-enable the rule and clean the corpus first.
  - The sweep PR touches 107 files; reviewers must trust that the
    diff is whitespace + safe-autofix (verifiable via
    `git diff -U0 | grep -E '^[+-]' | grep -v '^\s*$'`).
- **Neutral / follow-ups**:
  - If markdownlint is wired into `make lint` or pre-commit in a future
    PR, that PR ships its own scope-bounded coverage decisions.
  - Remaining ~600 warnings on touched files are mostly MD013
    (long URLs / long tables) and MD040 (missing code-block language)
    — addressable per-PR by the surface owner when they next edit.

## References

- `req` (paraphrased): the user requested a focused sweep of pre-existing
  markdown lint debt in `docs/` + `changelog.d/` so future PRs touching
  neighbours are not gated by the touched-file rule.
- [CLAUDE.md §12 r12](../../CLAUDE.md) — touched-file lint-clean rule.
- [ADR-0141](0141-touched-file-cleanup-rule.md) — original touched-file
  cleanup-rule definition.
- [ADR-0108](0108-deep-dive-deliverables-rule.md) — deliverables-rule
  context.
- [ADR-0100](0100-project-wide-doc-substance-rule.md) — project-wide
  doc-substance rule (this ADR ensures the substance rule is enforceable
  without lint-debt blocking it).
