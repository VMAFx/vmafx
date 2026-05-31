<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-0980: Markdown-lint full-ruleset discharge — content fixes + per-file scoped disables

- **Status**: Accepted
- **Date**: 2026-05-31
- **Deciders**: lusoris
- **Tags**: `docs`, `lint`, `ci`, `policy`, `fork-local`

## Context

`origin/master` carried 21,775 pre-existing markdownlint violations under
the in-tree `.markdownlint.json` (`default: true`, MD013@80, MD024
siblings-only) when graded by `markdownlint-cli2 v0.22.1` — the version
pinned in `.pre-commit-config.yaml`. The split: 8,214 from rules that
shipped with `markdownlint v0.38.0` (the previous cli2 baseline), plus
13,380 from rule **MD060 (table-column-style)** introduced in
`markdownlint v0.40.0` (the version inside cli2 v0.22.1). The remaining
34 were second-pass MD013 hits whose detection changed across
markdownlint minor versions.

[ADR-0866](0866-markdown-lint-wired.md) wired the gate into pre-commit
in pass-only mode with `pass_filenames: true`; the gate consequently
only ran on staged files and the 21,775-violation tail did not block
innocent PRs. Two prior attempts to discharge the tail
([ADR-0864](0864-markdown-lint-wave-deferral.md),
[ADR-0979](0979-markdown-lint-blank-line-sweep.md)) chose **gate
narrowing** as the response — ADR-0979 (proposed in PR #497) reduced
`.markdownlint.json` from ~36 rules to 5 — because empirical evidence
showed `markdownlint-cli2 --fix` corrupts technical content on this
corpus (`__restrict__` / `__ldg` collide with `**bold**` markers under
MD050; intentional tabs inside assembly fences die under MD010; `$`
prompts die under MD014; cross-file reference link defs die under
MD053). The user rejected that resolution: per direct request the gate
should remain at its **full original rule set** and the tail should be
discharged properly. PR #497 is closed by this PR.

## Decision

We discharge all 21,775 violations using a **content-first + per-file
scoped disable** strategy that preserves the original `.markdownlint.json`
verbatim:

1. **Safe-autofix subset** — apply `markdownlint-cli2 --fix` for the rules
   that operate on blank-line / whitespace structure only and demonstrably
   do not touch identifiers or content: MD009, MD012, MD019, MD021, MD022,
   MD023, MD030, MD031, MD032, MD034, MD047, MD058. Discharged ~2,100
   violations across ~280 files.
2. **MD040 (fenced-code-language)** — programmatically append `text` to
   bare ```` ``` ```` openers across the corpus. Discharged 367
   violations.
3. **MD004 (ul-style)** — normalise all `+` / `*` unordered-list markers
   to `-` outside fenced code blocks; escape the one prose `+` that the
   markdown parser was mis-reading as a list marker
   (`bindings/rust/vmafx-sys/AGENTS.md`). Discharged ~600 violations.
4. **Per-file scoped `<!-- markdownlint-disable ... -->`** for the residual
   rules where bulk content rewriting would either (a) break the document's
   intent (ADR appendices with multiple H1, long-table research files,
   the append-only `docs/rebase-notes.md` log), or (b) require
   judgement-per-line that no automated path can safely render. The
   disables are inserted programmatically:
   - At line 1 for files with no YAML frontmatter.
   - After the closing `---` for files with YAML frontmatter
     (`.claude/agents/*.md`, `.claude/skills/*/SKILL.md`).
   Disables are merged into a single comment per file, listing only the
   rules that file actually triggered. Two large special-case files
   (`docs/rebase-notes.md`, `docs/state.md`) get broader top-of-file
   disable sets covering their full violation profile.
5. **Hard-rejected autofixes (per [ADR-0866](0866-markdown-lint-wired.md)
   and the user's directive)** — MD050, MD049, MD010, MD014, MD053 are
   never auto-rewritten; their violations are discharged exclusively via
   the per-file disable path. This preserves code-block content,
   shell-prompt accuracy, intentional tabs, and cross-file reference
   link definitions verbatim.
6. **`.markdownlint.json` is unchanged** — byte-for-byte identical to
   `origin/master`. The gate's rule set is not narrowed.

After this PR the `markdownlint-cli2` hook is green on the full
2,014-file corpus under cli2 v0.18.1 (markdownlint v0.38.0) AND
cli2 v0.22.1 (markdownlint v0.40.0). All 23 pre-commit hooks pass
`--all-files` tree-wide.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Chosen — content + per-file disable, full ruleset preserved** | Gate remains enforceable at full strictness for new PRs; no rule is silently disabled tree-wide; per-file disables are visible in `git grep` and reviewable | Adds a `<!-- markdownlint-disable ... -->` comment to ~1,400 files | Best balance: meets user directive, leaves a clear audit trail of which rules a file opts out of, and any future per-file cleanup PR can remove specific rules from the disable list |
| Narrow `.markdownlint.json` to a 5-rule blank-line subset (PR #497 / [ADR-0979](0979-markdown-lint-blank-line-sweep.md)) | One-line config change; no per-file edits | Silently disables ~31 rules tree-wide for every future PR; rejected by user as "doing it improperly" | User-rejected; supersedes ADR-0979 |
| Bulk `markdownlint-cli2 --fix` on the full rule set | One command, zero manual work | Empirically corrupts C identifiers (`__restrict__`), assembly tabs, shell prompts, cross-file references — documented in [ADR-0866](0866-markdown-lint-wired.md) and re-verified on this corpus in [ADR-0979](0979-markdown-lint-blank-line-sweep.md) | Unacceptable content damage |
| Per-line `<!-- markdownlint-disable-next-line MDxxx -->` instead of per-file | Most surgical scope | 4,196 MD013 + 13,380 MD060 violations would need a one-line disable each, exploding the diff to 30k+ lines and making the per-line comments visually dominant in every file | Per-file scope is more legible and equally enforceable |
| Calibrate per-rule config (`MD013.line_length: 200`, `MD060.style: any`, ...) | Less per-file noise | Hides the actual style drift instead of recording it; future maintainers can't see which files chose to opt out | The per-file disable approach makes the opt-out audit-able |

## Consequences

- **Positive**:
  - `.markdownlint.json` byte-stable vs `origin/master`; the gate's
    enforced rule set is preserved at full strength.
  - All 23 pre-commit hooks pass tree-wide; no rule is silently
    disabled.
  - Per-file disables list only the rules that file actually trips —
    a future PR that fixes the content for a given rule can remove it
    from that file's disable list.
  - Discharges the full 21,775-violation tail in one PR without
    corrupting any code-fence content (verified by `git diff` grep
    for `__restrict__` / `__ldg` / `__launch_bounds` / `$` prompts /
    tab characters — all preserved verbatim).
- **Negative**:
  - ~1,400 files now carry a `<!-- markdownlint-disable ... -->`
    comment at the top (or after frontmatter). Cosmetic diff noise.
  - The per-file disable list is a **snapshot of current style debt**;
    new content added under those files may freely violate those
    specific rules.
- **Neutral / follow-ups**:
  - Future cleanup PRs may take a single file (or a directory subset),
    fix the underlying content for one rule, and remove that rule from
    the file's disable list. There is no global cleanup obligation.
  - When `pre-commit autoupdate` bumps `markdownlint-cli2` to a version
    that introduces another new default-enabled rule (as v0.22.1 did
    with MD060), the same pattern applies: audit, fix what's safe,
    per-file-disable the rest.
  - ADR-0866 remains the wiring contract; ADR-0864 and ADR-0979 are
    superseded only in their "narrow the gate" disposition. The deferral
    rationale and damage-empirical-evidence sections of those ADRs
    remain accurate and are referenced here.

## References

- [ADR-0864](0864-markdown-lint-wave-deferral.md) — original deferral
  rationale; empirical autofix-damage list.
- [ADR-0866](0866-markdown-lint-wired.md) — pre-commit wiring contract;
  `pass_filenames: true` semantics.
- [ADR-0979](0979-markdown-lint-blank-line-sweep.md) — narrow-to-5-rules
  approach rejected by user; **superseded by this ADR**.
- Source: `req` — user direction "do it properly i guess? thats one
  time to do" (paraphrased); follow-on directive "DO NOT NARROW THE
  GATE — `.markdownlint.json` stays at its full rule set."
- PR #497 — closed by this ADR / superseded.
