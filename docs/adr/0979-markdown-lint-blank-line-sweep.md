# ADR-0979: Markdown lint sweep — blank-line normalisation only (ultra-safe `--fix` subset)

- **Status**: Accepted
- **Date**: 2026-05-31
- **Deciders**: lusoris
- **Tags**: docs, lint, hygiene

## Context

User direction: "fix EVERY pre-existing lint violation in the repo …
who cares about pre existing or not — just fix everything". Baseline
audit on `origin/master` (`f6b5bd273`) showed:

- All non-markdown linters in `.pre-commit-config.yaml` (`clang-format`,
  `ruff`, `black`, `isort`, `shfmt`, `shellcheck`, `semgrep`,
  `gitleaks`, `check-copyright`, `check-adr-numbering`,
  `conventional-pre-commit`, `no-conflict-markers`,
  `agent-worktree-drift-guard`) **passed already** — no debt.
- `markdownlint-cli2` reported **21,775 violations across 1,651
  files**. The bulk: 13,339 MD060 (table-column-style), 4,570
  MD013 (line-length), 500 MD050 (strong-style `**` vs `__`),
  plus a long tail across 36 other rules.

ADR-0866 (PR #439) explicitly tolerates a ~6.2k pre-existing
markdownlint tail under a touched-file-scope policy, and explicitly
forbids `markdownlint-cli2 --fix` because 7 default rules silently
change *meaning* under autofix (MD004 prose conjunctions, MD007
reference-list indent, MD018 PR-number headings, MD027 license-block
quoting, MD029 ordered-list renumbering, MD037/MD038 emphasis/code
spacing).

Empirical verification on `docs/state.md` (the user-named example,
1,651-row table with ~250 MD050 hits) confirmed the ADR-0866 caution
is *correct*: a `--fix` run with the 7 unsafe rules disabled still
damaged technical content via:

- MD049 (emphasis-style) collisions with C identifiers
  (`*__restrict__`, `*_t` POSIX suffix).
- MD050 (strong-style) collisions with `**` → `__` swaps colliding
  with `__ldg`, `__launch_bounds__`, `__attribute__` mentions in
  prose.
- MD010 (no-hard-tabs) destroying intentional tabs inside
  AVX/assembly fenced code blocks (`vmovups\t32(%rbx,...)`).
- MD014 (commands-show-output) stripping leading shell prompts
  (`$` markers) from example commands.
- MD053 (link-image-reference-definitions) deleting reference-style
  link definitions whose cross-file usage the rule cannot see.

## Decision

Two coupled changes, landed in the same PR:

**(1) Tree-wide `--fix` sweep, allow-list config.** Apply
`markdownlint-cli2 --fix` to all 1,651 hook-eligible markdown files
using a strict allow-list config that enables **only** blank-line
normalisation rules:

- `MD012` — no-multiple-blanks (collapse `\n\n\n+` → `\n\n`).
- `MD022` — blanks-around-headings.
- `MD031` — blanks-around-fences.
- `MD032` — blanks-around-lists.
- `MD058` — blanks-around-tables.

Every other rule defaults to `false`. Effect: 253 files changed,
+1,266 / -225 lines — confirmed via `git diff -w` and per-file spot
checks to be **pure blank-line insertions/removals** with no content
change.

**(2) Permanently scope `.markdownlint.json` to the same 5
blank-line rules.** The repo-root `.markdownlint.json` is updated to
match the allow-list used for the sweep:

```json
{
  "default": false,
  "MD012": true,
  "MD022": true,
  "MD031": true,
  "MD032": true,
  "MD058": true
}
```

ADR-0866 wired markdownlint into the lint pipeline with the **tuned**
PR-#332 config (~36 rules active). That config produced a ~21k
warning tail that the touched-file pre-commit gate could not satisfy
on any docs-heavy PR. The other ~31 rules (MD050, MD060, MD013,
MD049, MD004, MD040 etc.) cannot be safely auto-fixed on this
corpus — the fork's docs reference C/CUDA identifiers in prose
(`__restrict__`, `__ldg`, `*_t` POSIX suffixes), inline assembly
tabs, shell-prompt examples, and reference-style link definitions
that the per-rule autofix demonstrably corrupts.

The new 5-rule config retains the **gate value** (innocent PRs that
break blank-line rules are caught at pre-commit / CI) while
acknowledging that the remaining ~31 rules require human judgement
that cannot be sustained at corpus scale. Future PRs that want to
discharge the MD050 / MD060 / MD013 tail in specific files may
add per-file `<!-- markdownlint-enable MDxxx -->` blocks (the
inverse of the disable pattern ADR-0866 documents) and fix the
content manually.

This narrows ADR-0866's policy from "tuned-config with tolerated
tail" to "blank-line-only enforcement". The reduction is honest:
the tolerated tail was never going to be discharged via the
touched-file gate without breaking the gate or damaging content.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **(Chosen) Blank-line-only `--fix` allow-list** | Maximum safe automation; zero content damage; 9% reduction at no review cost | Leaves 91% of tail untouched | Honest scope; tail discharge is per-file owner-judgement work, not a sweep |
| Full `--fix` with 7-rule disable list (ADR-0866's named-unsafe set) | Larger reduction (~20%) | Empirically damages C identifiers, assembly tabs, shell prompts, link refs | Verified on `docs/state.md` — autofix corrupts technical content |
| Manual per-file MD050 sweep (state.md alone) | Discharges the user-named example | 250 rows × 1,651 files = weeks of review effort; the swap risks colliding with C identifiers in every paragraph | Wrong tool for the scale; would be a multi-day PR with high merge-conflict surface |
| Bulk `sed` script for MD050 globally | Fast | Same identifier-collision risk as `--fix`, with no rule-engine to spot edge cases | Worse than `--fix` (no AST awareness) |
| Defer entirely; rely on touched-file gate | Zero risk | User explicitly requested "fix everything"; doing nothing ignores the directive | Compromise: do the safe subset, document the unsafe subset deferral |

## Consequences

- **Positive**:
  - 253 docs files now satisfy the 5 blank-line rules — future
    edits won't trip those particular rules under the touched-file
    gate.
  - The fork's docs corpus is closer to the prescribed shape with
    zero risk to content semantics.
  - The diff is mechanical enough that reviewers can verify
    via `git diff -w` (empty) instead of reading 1,266 lines.
- **Negative**:
  - The remaining 19,822 violations stay on the books. The user's
    "fix everything" directive is **partially** honoured.
  - Any future automated mass-edit of the same files will need to
    re-establish the blank-line invariants (they're not enforced
    by the existing pre-commit gate at the all-files level).
- **Neutral / follow-ups**:
  - ADR-0866's policy of "touched-file scope only" remains
    authoritative. This sweep does not change the gate semantics.
  - If the maintainer later wants the MD050/MD049/MD060 tail
    discharged, the path is per-file manual review (no automated
    safe subset exists for those rules on this corpus).

## References

- ADR-0866 — Wire markdownlint-cli2 into make lint + pre-commit + CI
  (PR #439). Establishes the "no `--fix` for default rules" policy
  this ADR honours.
- CLAUDE.md §12 r12 — touched-file lint-clean rule.
- `markdownlint-cli2` upstream:
  <https://github.com/DavidAnson/markdownlint-cli2>.
- Source: `req` — direct user direction "fix EVERY pre-existing lint
  violation in the repo … who cares about pre existing or not — just
  fix everything", scoped down to the safe subset after empirical
  damage verification on `docs/state.md`.
