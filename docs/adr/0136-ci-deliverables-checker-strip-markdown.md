<!-- markdownlint-disable MD013 MD060 -->
# ADR-0136: Strip markdown emphasis/code characters before ADR-0108 deliverables grep

- **Status**: Accepted
- **Date**: 2026-04-20
- **Deciders**: @lusoris, Claude
- **Tags**: ci, rule-enforcement, adr-0108

## Context

The `Deep-Dive Deliverables Checklist (ADR-0108)` job in
`.github/workflows/rule-enforcement.yml` greps the PR body for each of
six deliverable labels, expecting a ticked `- [x] **<label>**` line
OR an `no <label-keyword> needed` opt-out. The grep pattern is
`- \[x\].*${item}` where `${item}` is a literal string like
`AGENTS.md invariant note`.

The PR template (`.github/PULL_REQUEST_TEMPLATE.md`) itself emits the
matching bullet as:

```markdown
- [ ] **`AGENTS.md` invariant note** — ...
```

The backticks around `AGENTS.md` mean that a PR author who ticks the
template verbatim produces a body line like:

```markdown
- [x] **`AGENTS.md` invariant note** — added to core/src/cuda/AGENTS.md ...
```

The checker's regex `.*AGENTS.md invariant note` expects exactly one
space between `.md` and `invariant`, but the rendered line has
backtick-plus-space there. The match fails, the PR is rejected, and
the author has to manually strip the backticks from the label — even
though the template told them to include the backticks.

PR #72 tripped this exact failure on its first CI run, matching one
of the items in the standard template without modification.

## Decision

Strip markdown emphasis/code characters (`` ` ``, `*`, `_`) from the
PR body before writing it to the grep target file. A `tr -d` pipe is
enough:

```bash
printf '%s' "${PR_BODY:-}" | tr -d '`*_' > /tmp/pr_body.md
```

The six item-name patterns (`Research digest`, `Decision matrix`,
`AGENTS.md invariant note`, etc.) remain unchanged — they now match
against a normalised body where `**label**`, `*label*`, and
`` `label` `` all flatten to `label`. Case-insensitivity
(`grep -i`) was already enabled. The same normalisation is applied
to the second step of the job ("Verify ticked file-referencing
items appear in diff") so the two halves agree on what the author
wrote.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Fix in every PR body | Zero CI change | The template ships with backticks, so every PR that ticks `AGENTS.md` verbatim fails. Infinite tax on authors. | Working around a checker bug in prose doesn't scale. |
| Re-word the template to drop backticks | Template becomes its own answer | Loses the semantic hint that `AGENTS.md` is a filename; readers now have to infer. Also leaves the checker brittle to any future markdown in a label. | The display wrapping is correct; the parser is wrong. |
| Normalise body via `tr -d '\`*_'` (this ADR) | One-line fix, fails-open on any future markdown wrappers in labels, no behavioural change for already-matching bodies | Whitelist-narrow set of stripped chars — a label containing a `[`, `(`, or `/` could still mismatch a literal-item comparison. All current six items are free of those chars. | Chosen |
| Rewrite in Python / `jq` with tokenised match | Cleanest long-term, handles tables/emphasis | Big rewrite for a small bug. Shell job stays shell. | Over-engineered for the blast radius. |

## Consequences

- **Positive**: Authors can use the PR template's deliverables
  checklist verbatim and have the checker recognise their ticks. No
  more "fix the body" loops before Rule Enforcement goes green.
- **Positive**: The companion "Verify ticked file-referencing items
  appear in diff" step shares the same normalisation and stops
  disagreeing with the parse step.
- **Neutral**: Any opt-out line that used `*markdown emphasis*` on
  the reason still matches — the opt-out matching was already loose
  (`no .*(needed|impact|...)`). The tr pass makes it more loose,
  not less.
- **Risk**: If a future deliverable label introduces one of the
  three special chars (`` ` ``, `*`, `_`) *inside* the item name
  (e.g. `DEPENDENCIES.md*.lock` — hypothetical), the tr pass would
  silently drop it. The current six items and the template have
  none, so this is forward-looking.

## References

- PR #72 CI failure surfacing the bug: <https://github.com/VMAFx/vmafx/actions/runs/24689802565/job/72208484948>
- ADR-0108 — deep-dive deliverables rule (the policy this CI job enforces)
- ADR-0133 — earlier rule-enforcement-adjacent CI fix (clang-tidy scope)
- `.github/PULL_REQUEST_TEMPLATE.md` — the template whose backticks
  the parser was rejecting
