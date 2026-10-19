<!-- markdownlint-disable MD013 MD060 -->
# ADR-2461: A lighter deliverables track for small pull requests

- **Status**: Accepted
- **Date**: 2026-10-08
- **Deciders**: lusoris
- **Tags**: `process`, `docs`, `community`

## Context

[ADR-0108](0108-deep-dive-deliverables-rule.md) asks every fork-local pull
request for six deliverables: a research digest, a decision matrix, an
`AGENTS.md` invariant note, a reproducer, a changelog fragment and a rebase
note. The rule fits a feature or a GPU twin. For a one-line fix or a
documentation correction, five of the six are "no ... needed" lines, and a
first-time contributor meets the checklist before any review. The maintainer
asked for a stated lighter route for small changes (Q-182), to lower the bar
for a first contribution without changing the rule for everything else.

`scripts/ci/deliverables-check.sh` already accepts a one-line opt-out for any
deliverable. What is missing is the policy that says when an opt-out is the
expected path, so that a contributor does not have to guess and a reviewer does
not have to argue each line.

## Decision

A pull request is **small** when all of these hold:

1. It changes at most 100 lines in total, not counting changelog fragments,
   generated files and lock files.
2. It touches one subtree (one CODEOWNERS row).
3. It does not add, remove or change a user-discoverable surface of
   [agent-hard-rules](../development/agent-hard-rules.md) rule 7 beyond
   correcting existing text: no new CLI flag, public header entry, build option,
   extractor, backend, output field, MCP tool or tiny-AI surface.
4. It needs no ADR (another engineer could not reasonably have chosen
   differently).
5. It does not change a SIMD or GPU twin, a numeric result, or a Netflix golden
   assertion.

A small pull request waives, without a written reason beyond the marker
`small PR (ADR-2461)`, the research digest, the decision matrix, the
`AGENTS.md` invariant note and the rebase note. It keeps:

- the reproducer or smoke-test command (a test that fails without the change
  where a behaviour changed);
- the changelog fragment, when the change is visible to a user;
- documentation in the same pull request when it corrects or touches a
  documented surface;
- every lint, test and golden-data gate, unchanged.

Reviewers may refuse the track when a pull request does not meet the five
conditions and ask for the full six.

`scripts/ci/deliverables-check.sh` recognises the marker in a follow-up change
and checks what it can measure, refusing the marker otherwise: at most 100
changed lines (not counting changelog and rebase-note fragments, ADR index
fragments and lock files); one top-level directory (`docs/` and `changelog.d/`
do not count when another directory is touched); and no path that signals a
surface, a numeric change or an ADR: `core/src/`, `core/include/`,
`core/meson_options.txt`, `core/tools/`, `python/test/`, `ffmpeg-patches/`, a
new file under `docs/adr/`. The conditions that need judgement (a new
user-discoverable surface elsewhere) stay with the reviewer.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep six deliverables for every pull request | One rule, no edge cases | Five "no ... needed" lines on a typo fix; discourages first contributions | The cost lands on the contributors the project wants most |
| Waive all six for small pull requests | Lowest bar | Loses the reproducer and the changelog line, which are cheap and useful even for small fixes | Those two carry the value of the checklist |
| Size limit only (lines changed) | Simple to automate | A 20-line change to a GPU twin is not small in risk | Conditions 3 and 5 cover risk, not just size |
| Reuse the existing free-form opt-out | Nothing to change | Leaves the expectation implicit; each reviewer decides again | The gap this ADR closes |

## Consequences

- **Positive**: a small pull request needs a reproducer and a changelog
  fragment, and a marker for the rest; the first contribution path
  ([CONTRIBUTING.md](../../CONTRIBUTING.md)) can say so.
- **Negative**: a borderline pull request needs a judgement call, and the
  script does not enforce the conditions yet.
- **Neutral / follow-ups**: update the pull request template
  and `scripts/ci/deliverables-check.sh` together (marker recognised, size
  and subtree measured, negative test with a pull request that carries the
  marker and exceeds the limit). Until that lands CONTRIBUTING.md states that
  the existing opt-out form applies.

## References

- `Q-182` (maintainer decision, 2026-10-08): add a lighter deliverables track
  for small pull requests as part of the newcomer path.
- `Q-197` (maintainer decision, 2026-10-08): accept the track as written.
- `Q-186` (same decision): the track needs its own ADR, because the six deliverables are
  ADR-0108 policy.
- [ADR-0108](0108-deep-dive-deliverables-rule.md) (the rule this narrows),
  [ADR-0100](0100-project-wide-doc-substance-rule.md).
