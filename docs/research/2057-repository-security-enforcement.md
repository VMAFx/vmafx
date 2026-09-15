# Research-2057: Repository security enforcement

## Finding and source

Live reads on 2026-09-08 found no repository rulesets, no required reviews,
administrator bypass enabled and strict status freshness disabled in the classic
master protection. The documented private vulnerability-reporting route was
disabled. The Scorecard v5.5.0 scan at master `78c9d2bfc580880919d5168c18ad19441f9db96a`
could not inspect classic protection with its workflow token.

[Scorecard's matching source](https://github.com/ossf/scorecard/blob/c395761df6afe1a69e476bc60a013a94bcbc153f/clients/githubrepo/branches.go)
uses ruleset protection when available. GitHub documents
[public ruleset reads](https://docs.github.com/en/rest/repos/rules#get-a-repository-ruleset)
and notes that bypass actors are omitted without write access. The checker
therefore verifies the GraphQL `RepositoryRuleset.bypassActors.totalCount` when
REST omits actors. It rejects truncated listings and inaccessible counts.

## Implementation and verification

[ADR-1248](../adr/1248-repository-security-enforcement.md) records the alternatives.
The checked-in payload adds one active master-only ruleset, no bypass actors,
one independent latest-push approval, stale-review dismissal, thread resolution,
strict required checks bound to GitHub Actions, linear history and force/delete
protection. Existing classic protection is retained unchanged. Private reporting
was enabled, then read back as true. Ruleset `22587111` was created and its complete
owner readback matched the reviewed payload; the public checker passed afterward.

Offline controls cover missing/duplicate rulesets, scope/enforcement drift,
administrator bypass, hidden bypass counts, truncated/error responses, effective
rule omissions, required review/freshness/check-origin drift, disabled reporting,
and wrong repository identity. The live pre-ruleset run failed with the expected
missing-policy error and the post-application run passed. Reproduce the controls
with the commands in the [operator guide](../development/repository-security.md).

## Limits and retained evidence

These checks verify current settings, not completed human reviews, security
response history, release signatures, or full RC1 acceptance. The previous full
native gate remains evidence for its recorded source; this change touches no
native code or Netflix assertions. Required local lint and hosted checks remain
independent merge conditions.

The local evidence root is
`.workingdir2/evidence/scorecard-repository-security-2026-09-08/`. It retains the
before/after API responses, canonical payload, offline controls, commit/hook
receipts and checksums. API metadata is read-only and contains no credentials.

## Addendum, 2026-09-15 — the control could not be satisfied

The payload this digest measured declared no bypass actors and one required
independent approval. Seven days of evidence showed that combination is
unsatisfiable here: `VMAFx` has one collaborator, who authors every pull request,
and GitHub forbids self-approval.

Measured outcome — the last merge was #1413 at 2026-09-08 16:15 UTC, and the
ruleset was created at 23:26 local the same day. Nothing merged in the week that
followed, including a fully green #1396 and two security dependency bumps. Every
merge in the repository's history predates the ruleset and had zero reviews, so
the control was never satisfied, only avoided.

[ADR-1252](../adr/1252-solo-maintainer-declared-bypass.md) supersedes ADR-1248:
the ruleset keeps every other control and declares exactly one `User` bypass
actor, and the drift checker now verifies the declared list rather than asserting
an empty one. The limitation that matters for this digest's own method: a reader
without ruleset write access sees only `bypassActors.totalCount`, so it can
confirm how many actors bypass but not which.
