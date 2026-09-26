<!-- markdownlint-disable MD013 MD060 -->
# ADR-1341: Stage correctness, benchmarking, and retraining across RC1 through RC3

- **Status**: Accepted
- **Date**: 2026-09-26
- **Deciders**: Kilian
- **Tags**: release, rc, process, benchmarking, performance, ai, dependencies, docs

## Context

The first-release plan previously made benchmarking, tuning, and one-shot model
retraining consecutive gates before the final release, but it did not define what
each release candidate was meant to prove. That ambiguity kept correctness work,
performance work, and a roughly 130-hour retraining programme in one queue. It
also left external testers without a bounded hardware-validation and report
contract, so a failure could arrive without enough environment and artifact
identity to reproduce it.

The live tracker amplified the ambiguity on 2026-09-26. Epic
[#1245](https://github.com/VMAFx/vmafx/issues/1245) was closed while all five of
its benchmark and tuning tasks remained unchecked; epic
[#1246](https://github.com/VMAFx/vmafx/issues/1246) still described retraining
only as a generic post-RC last step. A conversational freeze on ordinary version
updates also had no durable repository policy. The existing tester-facing
scripts are not yet a safe substitute for the missing report path: the audit
found invalid backend selectors, an obsolete MCP executable name, unsafe JSON
construction, stale Vulkan documentation, incomplete backend coverage, and
missing provenance. The measured starting state and tool audit are recorded in
[Research-1341](../research/1341-rc-correctness-benchmark-retrain-sequence.md).

## Decision

The first VMAFx release will use three ordered candidate responsibilities:

| Candidate | Responsibility | Exit evidence |
| --- | --- | --- |
| **RC1 — correctness and tester readiness** | Finish confirmed release-blocking correctness, reliability, security, build, packaging, and hardware-usability defects. Ship a bounded validation path that outside testers can run on their own CPU, CUDA, SYCL, HIP, or Metal hardware and return as a portable report. | Required checks are green on the exact candidate head; every remaining `docs/state.md` row is classified as RC1-blocking, RC2 performance, RC3 training, or explicitly deferred; no confirmed RC1 blocker or untriaged row remains; the report path records commit/artifact identity, host and device details, tool/driver versions, backend availability, correctness/parity results, commands, logs, and failures. |
| **RC2 — benchmarking and tuning** | Run comparable benchmarks, profiles, and tuning sweeps on the hardware generations available to testers and the maintainer. Land measured wins without changing numerical-correctness contracts. | Reports pin the exact RC2 artifact and fixtures, include repeatable baselines, medians/spread and environment identity, and show the required correctness/parity gates still green after each accepted tuning change. |
| **RC3 — real model retraining** | Run the locked one-shot retraining programme only after RC2's tuned tree is clean and frozen, then validate and package the real model artifacts. | PLCC/SROCC/RMSE gates, model cards, registry hashes and signing metadata, and the unchanged Netflix golden-data gate all pass on the exact RC3 artifact. |

RC1 is not a claim that the project will never contain another bug. “Done
fixing” has a bounded meaning: there are no confirmed, actionable RC1 blockers
and no untriaged rows; required integrated checks pass on the candidate head;
and a tester can produce a reproducible report. Performance-only findings wait
for RC2, real training waits for RC3, and genuinely external work remains
explicitly deferred with its evidence and trigger.

RC1, RC2, and RC3 name the first candidate intended to prove each stage. Tags
remain immutable. If a stage exposes a correctness regression, fix it and rerun
the affected stage evidence before proceeding; a later repair candidate may be
cut, but benchmarking never moves into RC1 and retraining never moves before
RC3. Final `v1.0.0` follows accepted RC3 evidence and any required repair
candidate.

Ordinary Renovate and other version-update PRs are not frozen during these
stages. They may merge when the repository's normal required checks, review,
pinning, and component-specific validation pass. Security updates are
prioritised, not the only version changes allowed. Major or coordinated SDK,
toolchain, and base-image updates retain their existing specialised validation.
Any merge after candidate evidence was collected invalidates that exact-head
evidence and requires the affected checks or measurements to be rerun; this is
how dependency updates remain unblocked without making a stale candidate look
verified.

This ADR supersedes ADR-1201's suggested candidate count and supplies the
missing phase responsibilities. It retains ADR-1201's prerelease tag and
publication mechanics, refines ADR-1228's first-release benchmark trigger to
RC2, and fixes ADR-1105's one-shot retrain at RC3. ADR-1152's conservative
dependency-only bot exemption remains unchanged; version PRs still pass their
normal code, build, test, and security gates.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Finish correctness, benchmarking, tuning, and retraining before RC1 | RC1 would contain every final result | Delays the first external hardware feedback and repeatedly invalidates expensive performance and training evidence while correctness fixes still land | It defeats the purpose of an early candidate and keeps unrelated work in one critical path |
| Ship RC1, then do benchmarking and training only after final `v1.0.0` | Fastest first final tag | Releases unmeasured performance and unvalidated production model artifacts | RC2 and RC3 exist to make those results release evidence rather than post-release promises |
| Freeze all non-security version updates until RC2 or final | Minimises candidate churn | Accumulates routine maintenance, creates a subjective security exception, and makes later integration larger | Exact-head evidence already handles churn safely; normal gates should decide whether a version update merges |
| Assign RC1 to correctness/reporting, RC2 to performance, and RC3 to retraining (**chosen**) | Produces useful external feedback early, separates measurement from model fitting, and gives “done fixing” a checkable boundary | A merge after a candidate run requires targeted evidence to be rerun, and three stages require explicit tracker upkeep | The cost is visible and bounded, while each candidate answers one coherent question |

## Consequences

- **Positive**: RC1 can reach hardware testers without waiting for performance
  tuning or a multi-day training run; returned reports carry enough identity to
  reproduce failures; RC2 measurements operate on a correctness-qualified tree;
  RC3 trains against the tuned tree exactly once; ordinary dependency upkeep
  continues under existing safeguards.
- **Negative**: maintainers must classify remaining ledger rows and keep each
  report tied to an exact commit and artifact. Merging any change after evidence
  collection creates deliberate revalidation work.
- **Neutral / follow-ups**: update milestone 1 and epics #1238, #1245, #1246,
  and #1255 to the three-stage language; reopen #1245 for RC2 because its work
  is unchecked; keep #1246 open and mark it RC3-only. Before the RC1 exit
  decision, install and verify the report collector and operator guide, repair
  or bypass the audited stale probe/benchmark paths, remove removed-backend
  instructions, and prove each advertised backend either runs or reports an
  explicit unsupported reason. This policy adds no dependency, build-time
  fetch, runtime surface, or SBOM component.

## References

- `req` (verbatim, 2026-09-26):

  > okay, change of plans:
  >
  > 1. move benching to rc2
  > 2. move the real training to rc3
  > 3. unblock version merges (renovate already has new ones i think)
  > 4. check that people that thest rc1 actually can use all tools they need to bench and test on their hardware and create reports that they can send me so that I can fix and tune further?... something like that?
  > 5. somehow we should be done fixing soon?
- [Research-1341](../research/1341-rc-correctness-benchmark-retrain-sequence.md)
- [ADR-1201](1201-release-candidates-before-1-0-0.md) — release-candidate
  tag and publication mechanics; candidate count and phase scheduling are
  superseded here.
- [ADR-1105](1105-ensemble-v2-prod-flip-deferred-oneshot-retrain.md) — the
  one-shot retrain contract, now assigned to RC3.
- [ADR-1228](1228-upstream-ab-perf-milestone.md) — benchmark methodology and
  recurring triggers; its first-release full run is assigned to RC2.
- [ADR-1152](1152-dependency-pr-gate-exemption.md) — dependency-only bot PRs
  remain exempt only from documentation-process gates, not normal validation.
- GitHub epics [#1238](https://github.com/VMAFx/vmafx/issues/1238),
  [#1245](https://github.com/VMAFx/vmafx/issues/1245),
  [#1246](https://github.com/VMAFx/vmafx/issues/1246), and
  [#1255](https://github.com/VMAFx/vmafx/issues/1255).
