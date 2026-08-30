<!-- markdownlint-disable MD036 MD060 -->
# ADR-0348: Globally suppress CodeQL `cpp/poorly-documented-function`

- **Status**: Accepted
- **Date**: 2026-05-09
- **Deciders**: Lusoris, Claude
- **Tags**: `ci`, `security`, `codeql`, `policy`, `fork-local`

## Context

The fork's CodeQL configuration ([`.github/codeql-config.yml`](../../.github/codeql-config.yml))
opts in to the `security-extended` and `security-and-quality` query packs.
The latter pack ships `cpp/poorly-documented-function`, which raises a
warning on every C/C++ function whose signature is not preceded by a
`/** */` Doxygen-style comment block describing each parameter.

The fork's documented coding standard goes the other way. Both
[`CLAUDE.md` §6](../../CLAUDE.md) and [`docs/principles.md`](../principles.md)
direct contributors to "default to writing no comments. Only add one
when the *WHY* is non-obvious." The standard treats short,
self-documenting C helpers as the desired house style — a `/** */`
header block on every internal helper would add boilerplate without
adding signal.

The two policies collide on ~15 alerts in the latest CodeQL scan,
spread across `core/src/`. Each alert points at a function that is
genuinely fine under the project standard. Agent #538 reviewed the
class and explicitly recommended a global config-level suppression
rather than per-instance review or mass-comment-add: the rule is
over-zealous against this project's house style, not flagging real
correctness issues.

## Decision

Add a single `query-filters: - exclude: id: cpp/poorly-documented-function`
clause to [`.github/codeql-config.yml`](../../.github/codeql-config.yml).
The rule is suppressed globally because it conflicts with the fork's
documented style guide; the suppression carries an inline comment in
the config citing this ADR and the principle reference. The remaining
`security-extended` and `security-and-quality` rules stay enabled —
this is a targeted, project-rule-aligned exclusion, not a wholesale
weakening of the security scan.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Global config suppression** (chosen) | Single line of config, single decision, single ADR. Aligns with the documented project style. Re-applies automatically to future code. | Loses any future, genuinely useful hit from the rule. Project standard already says "no comments unless WHY non-obvious" — there is no future hit this rule would catch that the standard would also flag. | — |
| Per-file `lgtm[cpp/poorly-documented-function]` inline annotations | Scoped — leaves the rule on for any future use. | ~15 inline comment annotations across multiple files; each annotation is itself the kind of noise comment the project standard discourages. New code triggers more annotations indefinitely. | High noise, high recurring cost, contradicts the standard at the comment level. |
| Mass-add `/** */` doc blocks to the 15 flagged functions | Clears the alerts at the source. | Directly contradicts the project's "no comments unless WHY non-obvious" rule. Every future function added to the fork would have to carry the same boilerplate or re-trigger the alert. | Contradicts the documented coding standard. |
| Per-instance review (each of 15 alerts evaluated for "WHY non-obvious") | Highest fidelity — only suppresses sites where the standard genuinely says "no comment needed". | 15 case-by-case decisions, requires re-running on every new alert, no durable resolution. The 15 current sites all clear the project standard already. | Cost-of-review dominates; outcome would be 15× "suppress" anyway. |

## Consequences

**Positive**

- 15 currently-open CodeQL alerts auto-close on the next scan after
  this change merges and a fresh scan runs against `master`.
- Future contributors are not steered toward writing
  standard-violating boilerplate doc blocks to silence the rule.
- One decision, one place, one ADR — easy for the next maintainer to
  audit (`grep -n cpp/poorly-documented-function .github/codeql-config.yml`
  surfaces the suppression and the inline rationale).

**Negative**

- A future case where adding a `/** */` block on a non-obvious
  function would *genuinely* be the right call has no automated
  reminder. Mitigated by the project standard already calling that
  scenario out by name: "Only add one when the *WHY* is non-obvious."
- Verification of the alert closure is post-merge — GitHub's CodeQL
  scan re-runs on the next push to `master` (or via the next
  scheduled scan in `security-scans.yml`). The 15 flagged alerts
  should auto-close in the Security tab. Per the fork's
  no-guessing rule, this PR does **not** assert pre-merge closure;
  the verification step is explicitly post-merge.

**Neutral / follow-ups**

- If a future PR brings in a third-party C/C++ subproject with a
  documented style requiring Doxygen blocks, the suppression would
  need to be re-evaluated (re-enable on a path scope, or accept the
  external project's noise). Out of scope today — the fork has no
  such subproject.
- No code change paired with this config change. The decision is
  policy-only.

## References

- [`CLAUDE.md` §6 (Coding standards)](../../CLAUDE.md) — "default to
  writing no comments. Only add one when the *WHY* is non-obvious."
- [`docs/principles.md`](../principles.md) — fork-wide coding-standard
  charter.
- [`.github/codeql-config.yml`](../../.github/codeql-config.yml) — the
  config file modified by this ADR; carries an inline comment citing
  this ADR.
- [CodeQL `cpp/poorly-documented-function` rule docs](https://codeql.github.com/codeql-query-help/cpp/cpp-poorly-documented-function/)
  — upstream description of the rule.
- Source: paraphrased — agent #538 review note recommending "Project
  default is 'no comments unless WHY non-obvious'; recommend
  suppressing this rule via CodeQL config in a follow-up PR."
