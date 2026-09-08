<!-- markdownlint-disable MD013 -->
# OpenSSF Scorecard policy and operation

VMAFx requires an **unrounded risk-weighted Scorecard score of at least 8.5 on
master**. [ADR-1247](../adr/1247-scorecard-exact-head-gates.md) supersedes the old
6.2 floor, 7.0 target and permanently accepted blockers in ADR-0263. A passing
aggregate does not mean every check passed or that the project earned an OpenSSF
Best Practices badge. Each check and its reason remain visible in the gate summary.

## What runs and what it proves

| Event | Workflow and check | Evidence and scope |
| --- | --- | --- |
| Ready PR targeting master | `scorecard-policy.yml` / **Scorecard PR Gate** | All 11 local file-based checks on the exact PR head, plus offline policy contracts. The local-only weighted score must be at least 8.5; this is not the full repository score. |
| Master push, weekly Monday 04:19 UTC, classic branch-protection event | `scorecard.yml` / **Scorecard analysis**, then **Scorecard Master Gate** | Full 18-check report from the pinned publisher, with matching repository, exact event commit, reviewed tool identity and aggregate at least 8.5. |

Both applicable gates must actually succeed in Required Checks Aggregator;
missing, skipped or neutral results cannot satisfy them. Draft PRs fail their
PR gate. The PR gate never waits for a future master report. Repository settings,
review history, release history and badge registration are checked by the full
master scan, not inferred from the local PR subset.

The pinned action scans the **remote default HEAD** on master events. It does
not honour the checkout SHA for that scan. If master advances before its report
is produced, the gate rejects a report for the newer commit: use the matching
newer workflow run, never copy its score onto the older head. The gate also
reads the final live master ref because upstream fetches commit metadata and
its default-HEAD archive separately. It must still equal the event SHA. This
binds the scan window under the enforced no-force/no-deletion branch policy;
it cannot defend against a privileged actor changing and restoring that policy
and branch during the run. On PRs the action
reports `file://.` and `commit: unknown`; our before/after receipts independently
bind all tracked file bytes and modes to the requested PR commit, reject extra
untracked/ignored inputs, and identify the workflow run and attempt. The output
JSON is the only permitted new file after the scan. Symlinks retain their
literal committed targets, but every followed component must be another tracked
file or directory. Metadata, untracked, missing, absolute, escaping and cyclic
targets are rejected; directory links already used by the repository remain
supported. Link resolution is bounded to 40 hops on the Linux runner.

## Reports, errors and unavailable evidence

The publisher retains SARIF and its generated JSON in a same-run artifact named
`scorecard-<run-id>-<attempt>-<event-sha>` for 14 days. Its separate gate downloads
only that artifact from its own run, verifies complete unique check coverage,
valid score values, the Scorecard version/source commit and the report identity,
and recomputes the weighted aggregate. Rounded 8.5 from an actual score below
8.5 fails. PR artifacts retain the local JSON, source snapshot and gate receipt.
Both gates add a per-check table to the Actions summary, including zero scores.
Malformed or missing reports fail; the raw retained report explains scanner
failures even when no valid gate receipt can be produced.

**Inconclusive is not passing.** An internal error, including Dockerfile parsing
or inaccessible branch-protection data, fails the gate even if excluding it
would improve the displayed upstream aggregate. The sole explicit unavailable
case is `Signed-Releases = -1` with the exact upstream reason `no releases found`.
It is displayed as **unassessed: no releases (not signed)** and excluded from the
weighted denominator just as upstream does. Once a release exists, its actual
result applies; keyless signing configuration alone proves no shipped artifact.
Code-Review and CII-Best-Practices zero scores remain zero and lower the aggregate.

The public [Scorecard dashboard](https://scorecard.dev/viewer/?uri=github.com/VMAFx/vmafx)
and badge show the latest published result, which may describe an older commit.
They are useful navigation and historical evidence, never the gate's source of
truth. Code-scanning SARIF is available in GitHub's Security tab. An earned Best
Practices badge is a separate external assessment, not the numeric Scorecard score.

## Maintainer workflow

1. Open the failed gate's exact workflow run and check its repository, SHA,
   source scope, run attempt, tool identity and raw artifact.
2. For schema, missing report, parser or API errors, repair the cause. Do not
   suppress a check, change its score, use a stale API result or widen the
   unavailable-release exception.
3. For a low score, follow the pinned upstream check documentation and preserve
   evidence of the actual remedy. An independent review is a human approval;
   a badge requires truthful criteria answers and service verification.
4. Review the policy when upgrading Scorecard or its action. Update the complete
   sets, risk weights and exact tool identity together, validate real output,
   and run the local contracts before publishing the change.

Run the focused offline controls with:

```sh
python3 -B -m unittest discover -s scripts/ci/tests -p 'test_scorecard_*.py' -v
```

These tests are registered in pre-commit/pre-push and both gate jobs. They cover
stale/wrong source, incomplete or forged scores, rounding, scan errors,
no-release handling, source tampering including Git's assume-unchanged flag,
caller Git environment isolation, and real aggregator failure behavior.
The separate `repository-security-contract` hook and both CI gates also run:

```sh
python3 -B scripts/dev/tests/test_repository_security.py -v
```

The master gate additionally runs the read-only
[repository security checker](repository-security.md) with the built-in ephemeral
GitHub token, retaining its live ruleset and private-reporting evidence even if
the Scorecard assessment fails. This follows
[ADR-1248](../adr/1248-repository-security-enforcement.md). Offline controls do not
substitute for the hosted publisher, a successful live settings check or an
external badge decision. The owner-authorized live readback and hosted token
execution are distinct evidence; the latter requires the workflow to run.

## Authentication and publisher restrictions

The default ephemeral GitHub token supplies read access. The publishing job alone
has OIDC and Security-tab write permissions; its only steps are the actions
allowed by the pinned upstream publisher. Keep arbitrary validation scripts in
the separate gate jobs. The PR workflow has read-only permissions, never uses
`pull_request_target`, and never publishes its local result.

The action source is SHA-pinned; its upstream metadata currently launches a
tagged Docker image. Verifying the reported Scorecard 5.5.0/c395761d identity does
not make that image digest-pinned. Repository rulesets can be read by the default
token; classic branch-protection visibility errors must not be mistaken for proof
that the actual settings are strong.

## References

- [ADR-1247](../adr/1247-scorecard-exact-head-gates.md) — current policy and tradeoffs.
- [Research-0053 correction](../research/0053-ossf-scorecard-investigation.md#2026-09-08-correction-and-measured-gate-design) — dated evidence, limitations and superseded claims.
- [Exact action publishing restrictions](https://github.com/ossf/scorecard-action/blob/2d1146689b8cda280b9bc96326124645441f03bc/README.md#workflow-restrictions).
- [Scorecard 5.5.0 checks](https://github.com/ossf/scorecard/blob/c395761df6afe1a69e476bc60a013a94bcbc153f/docs/checks.md).
- [Release guide](release.md) — release artifacts and verification.
