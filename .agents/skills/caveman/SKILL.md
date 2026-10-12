---
# SPDX-FileCopyrightText: 2026 lusoris <lusoris@pm.me>
#
# SPDX-License-Identifier: EUPL-1.2
name: caveman
description: Caveman form for internal agent traffic (briefs, agent returns, research fan-outs, workflow returns, tool-call notes). Fragments, one fact per line, code, paths, errors verbatim, nothing a reader pays for twice. The `internal` text register; forge text uses social-text, docs and human replies use full prose.
metadata:
  derived_from: "https://github.com/JuliusBrussee/caveman (MIT)"
---

# Caveman: internal register (`caveman`)

Adapted from [Caveman](https://github.com/JuliusBrussee/caveman) by Julius
Brussee and
[contributors](https://github.com/JuliusBrussee/caveman/graphs/contributors);
MIT terms at adaptation (2026-09-18).

Caveman = `internal` register of text-register policy (`register:` in
`.standards.yaml`; "Text Register" section of AGENTS.md). Config value stays
`internal`; this skill = its form. Agent traffic pays per token, per hop ->
caveman line carries same facts, fewer tokens.

## Scope

Caveman covers text another agent reads:

- briefs, task prompts to subagent or cheap lane;
- agent returns, workflow returns;
- research fan-out results;
- tool-call notes, progress lines between agents.

Human-facing text keeps its own register: forge text (issues, PR bodies, review
comments, commit bodies, changelog titles) -> `social-text`; documentation tree
(`docs`), READMEs, ADR bodies -> docs register; reply to human operator -> full
prose.

## Rules

1. **Cut grammar with no fact.**

   | Drop | Examples |
   | :--- | :--- |
   | articles | `a`, `an`, `the` |
   | pronouns | `I`, `we`, `it` |
   | copulas | `is`, `are`, `was` |
   | auxiliaries | `have`, `will`, `did` |
   | hedges | `seems`, `probably`, `might` |
   | politeness | `please`, `thanks` |
   | framing | `I think`, `note that`, `it looks like`, `as requested` |

2. **Fragments.** One fact per line. Lists over sentences.
3. **Symbols** replace connective phrases:

   | Symbol | Reads as | Example |
   | :--- | :--- | :--- |
   | `->` | causes, leads to, then | `stale receipt -> push rejected` |
   | `=` | is, equals, means | `block = 15 lines` |
   | `x2` | count, times | `retry x3` |
   | `!` | warning, risk | `! gate not rerun after rebase` |
   | `?` | open question | `? Windows path untested` |

4. **Verbatim tokens.** Code, paths, `file:line`, commands, flags, error text,
   ids, shas, version strings, numbers = copied exact, never abbreviated.
   Compression hits English around them, never them. Invented abbreviations
   (`cfg ldr`, `ctx cmp`) cost reader more decoding than they save; use full
   word or real identifier.
5. **Start with answer.** No restated brief, no step recap before result, no
   closing line. Verdict first, then evidence.
6. **Tables only** when more than 3 rows compare more than 2 fields. Otherwise:
   list.
7. **Return shape** follows register block: verdict, changed paths, commands
   run, evidence pointers, open questions.
8. **Evidence** follows register block, same wording: evidence above 58 lines or
   1500 tokens -> file in the evidence directory the block names; return
   `evidence: <path> sha256:<12 hex> lines:<n>`, fetch only when decision needs
   it. Manifest may tighten both numbers; block in AGENTS.md prints values in
   force.

## Static checker

`praetorctl caveman check` defaults to runtime message form. Exception:
AGENTS.md + compiled vendor files at own path below `--root` (exact,
case-sensitive) default to `--kind=context`, same verdict as gate. Explicit
`--kind` wins. Structured traffic selects kind explicitly:

- brief: `--kind=brief`; fields = `goal`, `inputs`, `return`, `evidence`,
  `task`; goal first;
- return: `--kind=return`; fields = `verdict`, `changed`, `ran`, `evidence`,
  `open`; verdict first;
- policy text: `--kind=context`; C1-C8 heuristics only, uncovered skill rules
  reported advisory.

PASS summary lists mechanically checked and advisory skill-rule numbers. Runtime
producer wiring separate; checker validates supplied text only.

## Clarity floor

Caveman = shorter text, same information, never shorter text that makes reader
guess. Before sending: check every line -> can reader act on it without asking
back? When compression drops fact reader needs (which file, which count, new
failure or pre-existing, check bypassed or not), put fact back. Keep negations
explicit (`no bypass`, `not rerun`); dropped `not` inverts line.

## Before and after

Real return from a praetor lane:

<!-- caveman:off -->
- Before: "First two pushes were rejected by the pre-push gate with 'state
  synchronization stale' because gate run wrote .standards-receipt.json after
  the last state sync; fixed by re-running state sync and pushing again. Not
  bypassed."
<!-- caveman:on -->
- After: `push rejected x2: state stale (gate run wrote receipt after sync).
  fix: resync, push. no bypass.`

Test return:

<!-- caveman:off -->
- Before: "I ran the tests for the repository with the race detector and all of
  them passed. go vet did not report any issues. The only file I changed was
  .agents/skills/caveman/SKILL.md, where I added an error for a missing end
  marker."
<!-- caveman:on -->
- After:

  ```text
  verdict: pass
  changed: .agents/skills/caveman/SKILL.md (missing end marker -> error)
  ran: go test -race -count=1 ./... = pass; go vet = clean
  evidence: none
  open: none
  ```

Clarity floor in action:

<!-- caveman:off -->
- Before: "The dedupe scan reported two clones. Only one of them comes from this
  change; the other one is in .agents/skills/social-text/SKILL.md and was
  already present on main, so I left it alone."
<!-- caveman:on -->
- Too far: `dedupe: 2 clones.` Reader cannot tell whether change is blocked.
- After: `dedupe scan: 2 clones. 1 new (this change), 1 pre-existing on main
  (.agents/skills/social-text/SKILL.md), left as is.`
