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
