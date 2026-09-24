<!-- markdownlint-disable MD013 MD060 -->

# ADR-1297: Every check that reports on a pull request is a required context

- **Status**: Proposed
- **Date**: 2026-09-22
- **Deciders**: lusoris
- **Tags**: `ci`, `policy`, `build`, `security`, `docs`

## Context

Branch protection on `VMAFx/vmafx` (ruleset 22587111) requires exactly one status
context, `Required Checks Aggregator`. Everything else is decided by the
`const required = [...]` array inside
[`.github/workflows/required-aggregator.yml`](../../.github/workflows/required-aggregator.yml):
that array *is* the merge gate, in full. A check name missing from it is a leg
that can be red while the merge button stays green.

Measured on PR #1518 (head `160ddfc3d`), 89 checks reported and the array carried
45 names. Forty-four checks were ungated and nine of those were failing at the
time: `Coverage Gate`, `Ubuntu gcc`, `Ubuntu clang`, `Ubuntu ARM clang`,
`Ubuntu gcc static`, `macOS clang`, `macOS clang+DNN`, `macOS Metal` and
`macOS Clang+Metal`. No macOS leg was gated at all. `Ubuntu gcc` and
`Ubuntu clang` were not gated although their `+DNN` siblings were. `Coverage
Gate` was not gated although `Coverage GPU`, its self-hosted sibling that
legitimately skips, was. The full inventory with each conclusion is at
`.workingdir/evidence/aggregator-gap-2026-09-22.txt`.

`scripts/ci/check-aggregator-names.sh` reported OK throughout, and correctly so:
it verifies that the array and the `# required-aggregator` markers in the other
workflow files describe the same set, and they did. The gap was therefore not
drift. It was the declared state, and the declared state was wrong.

This is the same defect class the rest of this branch has been removing — a
`|| true` that hid five months of truncated `Coverage Gate` runs, a per-test
timeout that aborted a session, a compat shim that hung six legs silently, a
docs build an unrelated branch could cancel. A platform leg that can be red while
the merge button stays green is that defect wearing a different hat. Per the
maintainer, there is no "not a required context so it does not count" category.

## Decision

We will require every check that can report a real verdict on an ordinary pull
request. The `required` array grows from 45 names to 78, adding the platform and
backend build legs, the always-on test and lint gates, the FFmpeg / Docker /
dev-container / Rust consumer contracts, the two documentation builds, and the
two `github-advanced-security` code-scanning checks (`Semgrep OSS`, `gitleaks`),
alongside the marker comments that `check-aggregator-names.sh` requires.

A check is left out only for a structural reason — it cannot report on an
ordinary pull request (`schedule` / `push`-only / label-gated jobs), its reported
name is an unexpanded matrix expression, it is a routing step with no failing
path, or it is incapable of ever being red. Those exclusions are enumerated in
the Consequences section below, each with the line that makes it structural.

Three names change so the gate is coherent rather than merely longer:
`Tidy SYCL (advisory)` loses its suffix and its `continue-on-error`, the docs
site build stops reporting under the bare job id `build`, and the doxygen job
stops reporting under the bare job id `doxygen`. The two `experimental: true`
matrix rows (`macOS clang`, `macOS clang+DNN`) lose that flag, because
`continue-on-error` never neutralised the check-run conclusion anyway — both
reported `failure` on #1518 with the flag set — so it only made the workflow
claim the legs were advisory while nothing acted on the claim.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Gate every check that reports (chosen)** | Removes the whole "reports but blocks nothing" category in one pass; the array becomes a complete statement of the gate; the structurally-excluded checks are enumerated with their reason, so the next reader does not have to re-derive them | 78 contexts means the aggregator waits for the slowest leg; roughly half are path-filtered, so the gate is only as strong as those `paths:` lists; nine legs were red when the decision was taken | Chosen. The red legs are the argument for gating, not against: they were red on this branch and green on master (run 35519558357, 2026-09-20, every leg `success`), so the gate blocks the branch that broke them rather than the repository |
| Gate only the currently-red legs | Smallest diff; unblocks the specific incident | Leaves `Docker Image Build`, `Dev Container Build`, `vmafx-sys CI`, `cargo-deny`, `MCP Smoke`, `No Conflict Markers`, `Markdown Lint` and the rest exactly as they were — green today, ungated tomorrow. It treats "currently failing" as the criterion when the criterion is "can fail without anyone noticing" | Rejected: it re-creates the same hole for every check that happens to be green on the day |
| Gate nothing; rely on review | Zero CI cost; reviewers see the red X on the PR page | Review is the mechanism that already failed here for months across four separate defects on this branch alone; a reviewer cannot see a check that never ran, and GitHub's merge button does not read the X | Rejected |
| List the contexts directly in branch protection instead of the aggregator | No indirection; GitHub itself enforces each name | Loses ADR-0313: a path-filtered check that legitimately does not run reports nothing, and GitHub blocks on a missing required context forever. That is the structural deadlock the aggregator exists to solve, and it is why the doc-only PR case works today. It also moves the gate out of the repository into host configuration, where a diff review cannot see it change | Rejected; ADR-0313 stands |
| Keep `experimental: true` on the macOS rows and gate them anyway | Smaller diff | The workflow would claim the legs are advisory while branch protection treats them as blocking. Two contradictory statements about the same leg is how this whole class of bug starts | Rejected; the flag is removed |

## Consequences

- **Positive**: the array is now a complete description of the merge gate. Nine
  legs that could be red at merge no longer can. Every macOS leg, the plain
  Ubuntu gcc/clang/ARM/static legs, all four backend compile lanes, the Windows
  ARM64 NEON lane, the FFmpeg consumer contract, both container builds, the Rust
  binding and its supply-chain policy, the documentation builds and the two
  code-scanning verdicts all block a merge when they fail.
- **Positive**: `Coverage Gate`, `MCP Smoke`, `Markdown Lint`,
  `No Conflict Markers`, `Tiny-Model Registry Validate`, `Tidy SYCL` and
  `Windows ARM64 MSVC` also join `strictMustReport`, so for those seven "the check
  never appeared" is a failure rather than an assumed path skip. They have no
  trigger path filter and no conditional skip, and they report on both
  `pull_request` and `push` to master, which is the precondition for that list.
  `Sanitizers ASan+UBSan` is deliberately not in it: its `if:` excludes `push`,
  and the aggregator also runs on pushes to master.
- **Negative**: the nine legs listed in Context block this branch's merge until
  they are fixed. That is intended, and two of them (`Ubuntu gcc+DNN`,
  `Ubuntu clang+DNN`) were already required and already red, so the branch was
  blocked regardless. The fixes travel in the same merge train.
- **Negative**: 78 contexts lengthens the aggregator's wait to the slowest leg.
  The macOS and Windows legs are budgeted at 60–75 minutes and
  `Dev Container Build` at 60, inside the 240-minute deadline and 250-minute
  timeout. The failure mode when the hosted queue is deep is a timeout that
  blocks merges and deepens the queue — the starvation loops recorded in the
  workflow comments for 2026-05-09 and 2026-08-30. Watch the first wave.
- **Negative**: gating `Semgrep OSS` promotes registry-rule-pack findings into
  merge blockers. The check is created by the `github-advanced-security` app from
  the alerts, not from the job, and both the `semgrep-local` and
  `semgrep-registry` SARIF uploads carry the tool name `Semgrep OSS`, so the
  `continue-on-error: true` on the registry step no longer makes its findings
  advisory — it now only keeps a rate-limited registry *fetch* from failing the
  run. An upstream rule-pack update can therefore introduce alerts with no diff
  in this repository. Tracked as an open row in [`docs/state.md`](../state.md).
- **Neutral / follow-ups**: `WARN_AS_ERROR` in `core/doc/Doxyfile.public-api`
  stays OFF. ADR-0953 planned to flip it the moment the workflow joined this
  array, on the basis that the public headers were warning-clean (95 → 0). They
  are not: run 35739609579 measured 228 warning-log lines, so flipping it here
  would have made the gate red on arrival. The workflow instead enforces a
  ceiling at the measured count, which can only move down; the flip happens in
  the PR that drives it to zero.
- **Neutral / follow-ups**: roughly half the additions are path-filtered, so
  ADR-0313's absent-means-skip applies and the gate is only as strong as those
  `paths:` lists. A known gap: `rust-ci.yml` does not fire on
  `core/include/libvmaf/**` although the binding binds that header. Auditing the
  filters is a separate change, tracked in [`docs/state.md`](../state.md).
- **Neutral / follow-ups**: `Windows ARM64 MSVC` pins the
  `windows-11-vs2026-arm` label across the 2026-09-21..30 image migration. Its
  `strictMustReport` entry is what stops a vanished label from reading as a pass.

### Structurally excluded, with the line that makes it structural

| Check | Why it cannot be a required context |
|---|---|
| `Fuzz ${{ matrix.target }}` | The reported name is an unexpanded matrix expression, and `sanitizers.yml` gates the job on `schedule` / `workflow_dispatch`, so it never runs on a pull request |
| `Sanitizers TSan` | `sanitizers.yml` requires `push` to `refs/heads/master`. PR-time thread-sanitizer coverage is the already-required `Sanitizers (thread)` |
| `deploy` | `docs.yml` gates on `github.event_name == 'push'` and deploys to the `github-pages` environment |
| `FFmpeg Release Refresh` | `ffmpeg-patch-stack.yml` gates on `schedule`. PR-time coverage is the already-required `FFmpeg Patch Stack` |
| `Gate — check label / trigger` | Routing step; computes `should_run` and has no failing path |
| `Prepare YUV fixtures`, `Build e2e images`, `E2E — kind + kuttl` | All three carry `needs: gate` plus `should_run == 'true'`, which is false without the `run-e2e-k8s` label or the nightly schedule (ADR-0783). Requiring them would mean requiring a label on every PR |
| `Probe SYCL Runner` | Routing probe whose failure is already load-bearing: it skips `SYCL Parity (Arc A380)`, and the aggregator rejects that skip when the lane is enabled. Gating the probe adds nothing and would block merges whenever its PAT expires |
| `Label PR by commit type` | Write-side automation on `pull_request_target` with `pull-requests: write` (ADR-1233). It asserts nothing about the product |
| `ADR-Backfill Advisory` | The job's script has no `exit 1` on any branch (`rule-enforcement.yml`), so the context could only ever report success. Making it a gate means writing a decidable predicate first; the enforceable half of ADR-0106 already lives in the required `ADR Collision Guard` and `Deliverables Checklist` |

## References

- [ADR-0313](0313-ci-required-checks-aggregator.md) — the aggregator and its
  absent-means-path-skip semantics, which is why a path-filtered check costs
  nothing when it does not run.
- [ADR-1151](1151-vmafx-first-release-1-0-0.md) — the release-PR handling that
  shaped this array: it added the rule-enforcement process gates, which were
  reporting but not required, and the `releaseMustReport` carve-out for the one
  PR whose diff lets every path-routed gate legitimately select nothing. The
  same carve-out reasoning applies unchanged to the 33 names added here.
- [ADR-0217](0217-sycl-toolchain-cleanup.md) — made `Tidy SYCL` advisory "until
  one green master run confirms the wrapper holds across all current SYCL TUs;
  gate tightens after that". The last six Lint runs on master
  (2026-09-19T21:20Z … 2026-09-20T15:26Z) all report it `success`.
- [ADR-1260](1260-windows-arm64-cpu-lane.md) — left `Windows ARM64 MSVC`
  advisory until the maintainer promoted it; this ADR is that promotion.
- [ADR-1294](1294-docs-build-concurrency-per-job.md) — the ref-scoped docs
  concurrency group that this gate gives teeth to.
- [ADR-0953](0953-doxygen-public-api-clean.md) — the `WARN_AS_ERROR` promotion
  path, deferred here with a measured reason.
- [ADR-1102](1102-phase4b9-container-only-publishing.md) — the container as the
  canonical published artifact, which is why both image builds are gated.
- [ADR-1142](1142-whole-codebase-standards.md) — no tier is exempt from the fork's
  standards, which is why `Tidy SYCL` cannot stay advisory.
- [ADR-1177](1177-sycl-arc-self-hosted-runner.md) — the self-hosted-lane treatment.
  No check added here runs on a self-hosted runner, verified by `grep -n
  'runs-on:' .github/workflows/*.yml`, so none needs it.
- Evidence: `.workingdir/evidence/aggregator-gap-2026-09-22.txt`; PR #1518 at
  `160ddfc3d`; master run 35519558357 (2026-09-20, every build leg `success`).
- Source: `req` — the maintainer's standing direction, given more than once,
  that a check which reports but blocks nothing does not count as a gate.
