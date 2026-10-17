<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-3333: Move the praetor pin to 2d23b29 and give every agent persona a model tier

- **Status**: Accepted
- **Date**: 2026-10-15
- **Deciders**: lusoris
- **Tags**: ci, governance, standards

## Context

Praetor `2d23b2924` (cordanaLLM/praetor#1341) makes the persona front matter
key `model` a client-neutral tier, `flash` or `pro`, and projects it into each
client's own model name when `compile-context` writes the persona copies under
`.claude/agents/`, `.codex/agents/`, `.gemini/agents/`, `.github/agents/` and
the other client folders. `compile-context --verify` at that engine refuses a
persona whose canonical `model` is missing or not a tier. The twelve personas
under `.agents/agents/` named `model: sonnet`, a Claude Code model name.

An engine older than `2d23b29` does not know the tiers: the engine at
`3bbde07` refuses the projections `2d23b29` writes (`agent persona projection
differs from its canonical source: .claude/agents/c-reviewer.md`). The
projections and the pin therefore move in one change, after the pin to
`3bbde07` (pull requests #2794, #2797, #2798 and the declared-linter
jobs) has landed.

## Decision

- Every canonical persona carries a tier, set by hand: `repo-gatekeeper`
  (dependency checks and scans) is `flash`; the eleven reviewers, the profiler
  and `repo-auditor` are `pro`.
- The client copies are regenerated with `praetorctl compile-context` at
  `2d23b29`; none is edited by hand.
- `praetorctl adopt` at `2d23b29` refreshes the files praetor ships unchanged
  here: `tools/apicompat/gate/main.go` (audit reported it outdated) and
  `.config/agent/hooks/block_evasion.py` (refuses admin merges through `gh` and
  the REST and GraphQL merge endpoints, and joins line continuations before it
  scans).
- Move `PRAETOR_REF` to `2d23b292417386fe8259da29690266ce331062c6` under
  [ADR-1351](1351-praetor-engine-pin-move.md)'s conditions.
- Not copied back: the pre-tool guard `adopt` registers in `.codex/hooks.json`
  (a change of its own, ledger `vmafx-adopt-pretool-hook`), and the three
  `declared-linters` exceptions `adopt` writes into `.standards.yaml`: the
  linters run in CI instead.

## Alternatives considered

| Option | Why not |
|---|---|
| Keep `model: sonnet` and stay at `3bbde07` | The fleet's personas carry tiers from this engine on; the next pin move would have to do this anyway, together with the engine's later changes. |
| Run `adopt` over the personas too | Two personas are edited here (`repo-auditor`, `repo-gatekeeper`); `adopt` would overwrite the edits with praetor's scaffold. |
| Take `adopt`'s declared-linter exceptions | They would hide the gate the linter jobs satisfy; an exception for a linter that runs is a stale exception. |
| Make `repo-auditor` `flash` | Its sweeps judge findings across the tree; that is review work, which the fleet routes to the stronger tier. |

## Consequences

- **Positive**: each client runs a persona on the model tier its work needs,
  named once in `.agents/agents/`. Admin-merge commands are refused by the
  local hook as well as by the merge rule.
- **Neutral**: `compile-context --verify` is clean at `2d23b29`; it fails on a
  persona with a planted invalid tier (`model: bogus`) and on a hand-edited
  projection, both measured for this change.
- **Follow-ups**: the pre-tool guard (`vmafx-adopt-pretool-hook`).

## References

- Source: operator order of 2026-10-15, ledger id
  `fleet-persona-model-key-20261015` (paraphrased): edited persona copies get
  the tier by hand, unedited copies are refreshed with `adopt` at the current
  engine, then context verification and the full verify run.
- cordanaLLM/praetor#1341, #1291.
- [ADR-1351](1351-praetor-engine-pin-move.md), [ADR-3285](3285-praetor-pin-66223619.md) (the previous pin move).
