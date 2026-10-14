<!-- markdownlint-disable MD013 -->
# OpenSSF Scorecard policy and operation

If a Scorecard gate fails, start at [Maintainer workflow](#maintainer-workflow).
The rest of this page is the policy and the evidence each gate relies on.

VMAFx requires an **unrounded risk-weighted Scorecard score of at least 8.5 on
master**. [ADR-1247](../adr/1247-scorecard-exact-head-gates.md) supersedes the
old
6.2 floor, 7.0 target and permanently accepted blockers in ADR-0263. A passing
aggregate does not mean every check passed or that the project earned an OpenSSF
Best Practices badge. Each check and its reason remain visible in the gate
summary.

## What runs and what it proves

| Event | Workflow and check | Evidence and scope |
| --- | --- | --- |
| Ready PR targeting master | `security-scans.yml` / **Scorecard PR Gate** | All 11 local file-based checks on the exact PR head, plus offline policy contracts. The local-only weighted score must be at least 8.5; this is not the full repository score. |
| Master push, weekly Monday 04:19 UTC, classic branch-protection event | `scorecard.yml` / **Scorecard analysis**, then **Scorecard Master Gate** | Full 18-check report from the pinned publisher, with matching repository, exact event commit, reviewed tool identity and aggregate at least 8.5. |

Both applicable gates must actually succeed in Required Checks Aggregator;
missing, skipped or neutral results cannot satisfy them. Draft PRs fail their
PR gate. The PR gate never waits for a future master report. Repository
settings,
review history, release history and badge registration are checked by the full
master scan, not inferred from the local PR subset.

### Scan scope on master events

The pinned action scans the **remote default HEAD** on master events and does
not honour the checkout SHA for that scan. Three rules follow:

- If master advances before its report is produced, the gate rejects a report
  for the newer commit. Use the matching newer workflow run; never copy its
  score onto the older head.
- The gate also reads the final live master ref, because upstream fetches
  commit metadata and its default-HEAD archive separately. Only a ref that still
  equals the event SHA gives a verdict; the next section covers a moved master.
- This binds the scan window under the enforced no-force/no-deletion branch
  policy. It cannot defend against a privileged actor changing and restoring
  that policy and branch during the run.

### Superseded master runs

When master moved on during the scan to a commit that descends from the event
SHA, the run is **superseded** and ends **cancelled**, neither green nor red
([ADR-1686](../adr/1686-scorecard-superseded-master-runs.md)). The gate checks
the ancestry with GitHub's comparison of the two commits (status `ahead`, the
event SHA as base and merge base), writes a receipt with `outcome: superseded`,
and names the newer commit in a notice and in the step summary. That commit's
own push run gives the verdict; open it instead. The gate job cancels its own
run through the REST API, which is why it holds `actions: write`. A final ref
that is invalid, or that names a commit which does not descend from the event
SHA (a rewrite), still fails the gate, as does a comparison GitHub does not
answer. The artifact keeps `final-master-ref.json` and `master-compare.json`.

### Scan scope on pull requests

On PRs the action reports `file://.` and `commit: unknown`. The before/after
receipts of the gate therefore bind the scan independently:

- All tracked file bytes and modes are bound to the requested PR commit.
- Extra untracked or ignored inputs are rejected.
- The workflow run and attempt are identified.
- The output JSON is the only permitted new file after the scan.

!!! note "Symlink rules"
    Symlinks retain their literal committed targets, but every followed
    component must be another tracked file or directory. Metadata, untracked,
    missing, absolute, escaping and cyclic targets are rejected. Directory
    links already used by the repository remain supported. Link resolution is
    bounded to 40 hops on the Linux runner.

## Reports, errors and unavailable evidence

The publisher retains SARIF and its generated JSON in a same-run artifact named
`scorecard-<run-id>-<attempt>-<event-sha>` for 14 days. Its separate gate
downloads
only that artifact from its own run, verifies complete unique check coverage,
valid score values, the Scorecard version/source commit and the report identity,
and recomputes the weighted aggregate. Rounded 8.5 from an actual score below
8.5 fails. PR artifacts retain the local JSON, source snapshot and gate receipt.

Both gates add a per-check table to the Actions summary, including zero scores.
Malformed or missing reports fail; the raw retained report explains scanner
failures even when no valid gate receipt can be produced.

### Inconclusive is not passing

An internal error, including Dockerfile parsing or inaccessible
branch-protection data, fails the gate even if excluding it would improve the
displayed upstream aggregate. How individual results are counted:

| Check | Result | Counted how |
| --- | --- | --- |
| Any check | Internal error or unavailable data | Gate fails. |
| `Signed-Releases` | `-1` with the exact upstream reason `no releases found` | Displayed as **unassessed: no releases (not signed)** and excluded from the weighted denominator, as upstream does. This is the only unavailable case. |
| `Signed-Releases` | A release exists | Its actual result applies; keyless signing configuration alone proves no shipped artifact. |
| `Code-Review` | Zero | Stays zero and lowers the aggregate. |
| `CII-Best-Practices` | Passing badge, earned 2026-09-26 ([record](best-practices-assessment.md)) | Scores 5 out of 10; silver scores 7 and gold 10. |

The public [Scorecard
dashboard](https://scorecard.dev/viewer/?uri=github.com/VMAFx/vmafx)
and badge show the latest published result, which may describe an older commit.
They are useful navigation and historical evidence, never the gate's source of
truth. Code-scanning SARIF is available in GitHub's Security tab. An earned Best
Practices badge is a separate external assessment, not the numeric Scorecard
score.

## Maintainer workflow

1. Open the failed gate's exact workflow run and check its repository, SHA,
   source scope, run attempt, tool identity and raw artifact. A cancelled run
   whose summary says **superseded** needs no action: open the run of the newer
   commit it names.
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

The default ephemeral GitHub token supplies read access. The master gate job
also holds `actions: write`, used only to cancel its own superseded run
([ADR-1686](../adr/1686-scorecard-superseded-master-runs.md)). The publishing job
alone
has OIDC and Security-tab write permissions; its only steps are the actions
allowed by the pinned upstream publisher. Keep arbitrary validation scripts in
the separate gate jobs. The PR workflow has read-only permissions, never uses
`pull_request_target`, and never publishes its local result.

The action source is SHA-pinned; its upstream metadata currently launches a
tagged Docker image. Verifying the reported Scorecard 5.5.0/c395761d identity
does
not make that image digest-pinned. Repository rulesets can be read by the
default
token; classic branch-protection visibility errors must not be mistaken for
proof
that the actual settings are strong.

## Dependency pinning and hash locking

Under [ADR-1305](../adr/1305-hash-locked-python-installs.md), all Python
dependencies
across workflows, Dockerfiles, setup scripts, and Makefile are locked with
cryptographic
SHA-256 hashes (`--require-hashes`).

Every `uses:` reference in `.github/workflows/` is pinned to a full commit SHA,
with
no exception. The VMAFx organisation enforces this as well
(`sha_pinning_required`),
and the enforcement reaches actions nested inside reusable workflows. The former
exception, `slsa-framework/slsa-github-generator`, required a tag reference and
calls
its own sub-actions by tag, so it failed under that policy during the
v1.0.0-rc.2
publication. Release provenance now comes from the SHA-pinned
`actions/attest-build-provenance`
([ADR-1356](../adr/1356-release-provenance-attest.md)), and
`scripts/release/tests/test-publication-environment-binding.sh` rejects any
unpinned
action in the release workflows.

## History

- Prior live evidence was Scorecard 8.7, Pinned Dependencies 7, and CII
  InProgress project 14549 at 42 percent. Local work is not post-merge proof
  until merged and scanned by the master publisher.
- Project 14549 reached the passing badge on 2026-09-26.
- The former `slsa-framework/slsa-github-generator` exception to SHA pinning
  failed under the organisation policy during the v1.0.0-rc.2 publication and
  was replaced by `actions/attest-build-provenance` (see
  [ADR-1356](../adr/1356-release-provenance-attest.md)).

## References

- [ADR-1247](../adr/1247-scorecard-exact-head-gates.md) — current policy and
  tradeoffs.
- [ADR-1305](../adr/1305-hash-locked-python-installs.md) — hash-locked Python
  dependency installs.
- [Research-0053
  correction](../research/0053-ossf-scorecard-investigation.md#2026-09-08-correction-and-measured-gate-design)
  — dated evidence, limitations and superseded claims.
- [Exact action publishing
  restrictions](https://github.com/ossf/scorecard-action/blob/2d1146689b8cda280b9bc96326124645441f03bc/README.md#workflow-restrictions).
- [Scorecard 5.5.0
  checks](https://github.com/ossf/scorecard/blob/c395761df6afe1a69e476bc60a013a94bcbc153f/docs/checks.md).
- [Release guide](release.md) — release artifacts and verification.
