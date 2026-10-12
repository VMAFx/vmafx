---
# SPDX-FileCopyrightText: 2026 lusoris <lusoris@pm.me>
#
# SPDX-License-Identifier: EUPL-1.2
name: social-text
description: "Write forge-facing text (issues, pull-request bodies, review comments, commit bodies, changelog titles): scannable human prose derived from adhd-format; keeps PR template, receipt fence, conventional commits, Keep-a-Changelog intact."
metadata:
  derived_from: "https://github.com/ayghri/i-have-adhd (MIT)"
---

# Social register for the forge (`social-text`)

Adapted from [i-have-adhd](https://github.com/ayghri/i-have-adhd) by Ayoub
Ghriss and
[contributors](https://github.com/ayghri/i-have-adhd/graphs/contributors) via
adhd-format; MIT licence.

Use this skill for forge text a person reads: issue, pull-request body, review
comment, commit body, changelog fragment title. = `social` register of
text-register policy (`register:` in `.standards.yaml`; "Text Register" section
of AGENTS.md). Agent-to-agent text -> `internal` register, documentation ->
`docs` register; neither uses this skill.

## Inherited from adhd-format

Skill inherits 3 `adhd-format` principles; item 2 adds the line-break and list
rules:

1. **Bottom line first**: first sentence = decision, defect or ask.
2. **Scannable structure**:
   - paragraphs of one to three sentences, one idea each;
   - bold operative words of action bullet;
   - a list whenever three or more parallel items appear, never a comma chain;
   - a blank line before and after every list, table and heading;
   - a table when three or more items are compared across two dimensions.
3. **Progressive disclosure**: summary first, commands second, detail linked or
   collapsed below.

Overrides rest of adhd-format for humans: at most 1 GitHub alert per text; no
Mermaid unless flow is point of change; tables only for 3+ rows; no emoji
headings; no anchor bolding in running prose.

## Voice

- Full sentences, plain words. Maintainer must act on text without opening diff.
- One idea per paragraph or bullet. Name file and line
  (`.agents/skills/caveman/SKILL.md:42`) when reader will look there.
- Say 4 things, then stop: what changed, why, how verified, what reviewer must
  decide. Pull-request summary stays around 250 words before evidence links.
- No meta-commentary (`as requested`, `based on my analysis`), no hedging
  filler, no code restatement diff already shows.
- Quote operator paraphrased in neutral English; never verbatim colloquial line.
- No tool attribution anywhere: no `Co-Authored-By` naming model or tool, no
  `generated with` footer (AGENTS.md rule 12). Strip such footer when editing
  existing body.

## Evidence

Link or attach evidence; never paste beyond inline bound (`register.evidence`,
default 58 lines / 1500 tokens). Test log, SARIF file or transcript ->
`evidence: <path> sha256:<12 hex> lines:<n>`, or attached file. Signed receipt =
one exception, pasted where template asks for it.

## Standards this register does not change

**Pull-request body.** Fill the pull request template exactly as shipped. Social
register governs only prose under `## Summary of Changes`. Sections 1-4 (HISS-16
checklist, 3D-testing verification, context-integrity checklist, receipt) =
filled, never rewritten, reordered or removed. Receipt goes inside fence
labelled `receipt`; unlabelled fence not read by validator.

**Commit message.** Subject stays conventional commit, `type(scope): subject`,
`type` one of `feat | fix | docs | style | refactor | perf | test | build | ci |
chore | revert`. Commit-msg hook rejects other shape, requires `Signed-off-by`
trailer. Keep subject near 72 characters; hook does not measure it, reader's
terminal does. Breaking change carries `!` in subject, `Migration:` footer
(HISS-14). Only body written in this register: why first, then what, wrapped at
72 columns.

**Changelog.** Never edit `CHANGELOG.md` directly. Write one fragment
`<date>-<slug>.yaml` in the changelog fragment directory with `type` from
`added | changed | deprecated | removed | fixed | security`, one-sentence
imperative `title`, optional `issue`, `breaking: true` when applicable. Register
shapes title only.

**Issue.** Title = defect in one sentence. Body: observed, expected,
reproduction commands, evidence pointers. Search open issues, pull requests
before filing (AGENTS.md rule 5).

**Review comment.** One finding per comment; severity word first, then
file:line, then fix. Address code, not author.
