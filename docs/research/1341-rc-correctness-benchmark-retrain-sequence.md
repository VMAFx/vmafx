# Research-1341: RC correctness, benchmark, and retraining sequence
<!-- markdownlint-disable MD013 MD060 -->

## Question

What release-candidate boundary lets VMAFx stop the pre-RC1 correctness sweep,
obtain useful reports from outside hardware, tune performance on measured data,
and run the real model training only once?

## Evidence snapshot — 2026-09-26

### Repository policy before the decision

- [`docs/roadmap.md`](../roadmap.md) placed benchmark/tuning and model
  retraining as consecutive final gates but did not assign responsibilities to
  individual candidates.
- [ADR-1201](../adr/1201-release-candidates-before-1-0-0.md) implemented the
  `v1.0.0-rc.N` publication mechanics and suggested four to eight candidates;
  it did not say which candidate proves correctness, performance, or models.
- [ADR-1105](../adr/1105-ensemble-v2-prod-flip-deferred-oneshot-retrain.md)
  already required one retrain after the toolchain reached RC. The operator
  runbook now estimates the full programme at roughly 125–130 hours, so running
  it while the code and tuning still move is materially expensive.
- [ADR-1228](../adr/1228-upstream-ab-perf-milestone.md) provides the upstream
  A/B methodology and recurring hardware/toolchain triggers, but “before every
  release” did not identify the first-release candidate that owns the full run.
- [ADR-1152](../adr/1152-dependency-pr-gate-exemption.md) exempts strict
  dependency-only bot PRs from documentation deliverables only. It leaves all
  ordinary build, test, review, pinning, and security gates intact.

### Live tracker state

Read-only `gh issue view` and `gh pr list` checks against `VMAFx/vmafx` found:

| Surface | Live state | Consequence |
| --- | --- | --- |
| [Epic #1245](https://github.com/VMAFx/vmafx/issues/1245) | Closed, although all five benchmark/tuning checklist items remain unchecked | The tracker currently reports the RC2 work as complete when its own body says it is not |
| [Epic #1246](https://github.com/VMAFx/vmafx/issues/1246) | Open; calls retraining the last item and requires every other epic first, but describes it only as post-RC | Keep the one-shot contract and assign it explicitly to RC3 |
| [Epic #1255](https://github.com/VMAFx/vmafx/issues/1255) | Open rolling benchmark/model programme | Preserve recurring work there while keeping the first-release RC2/RC3 gates in milestone 1 |
| Open Renovate PRs | Zero at the snapshot | “Unblock version merges” is a merge policy for current and future bot PRs, not a claim that a known queue is waiting |

The milestone 1 description still lists closed #1245 before #1246 and says
epic #1246 gates final rather than RC1. It needs a remote metadata update after the
in-tree decision lands.

### Tester-facing tool audit and the missing seam

The tree contains useful building blocks, but the audit did **not** find a
tester-ready end-to-end path:

| Surface | What is usable | RC1 blocker found |
| --- | --- | --- |
| `vmaf` | Ordinary scoring with exclusive `--backend` selection | Existing probe automation instead passes nonexistent `--cuda`, `--sycl`, and `--hip` switches; its CPU row is not pinned and can auto-dispatch |
| `dev/scripts/smoke-probe-loop.sh` | Bounded score and MCP probe shape | Calls obsolete `vmaf-mcp-server` rather than installed `vmafx-mcp`, can emit invalid JSON from unquoted failure text, and records no commit/artifact/device/driver/tool/command/log provenance |
| `vmaf_bench` and `testdata/bench_all.sh` | CPU/CUDA/SYCL fixture measurements | No HIP or Metal implementation; `bench_all.sh` retains removed Vulkan prose and uses non-exclusive flags that can allow another compiled backend to contaminate a row |
| `scripts/ci/cross_backend_parity_gate.py` | CPU/CUDA/SYCL parity structure | No HIP or Metal path; its default CUDA `--gpumask 1` disables CUDA under the current CLI contract, so the documented default CUDA run is broken |
| `testdata/bench_backends.py` | Scripted CPU/CUDA/SYCL/HIP timing | Assumes Linux `/proc/loadavg` and has no default Metal path, so it is not a cross-platform collector |
| `testdata/bench_upstream_ab.py` | ADR-1228's pinned upstream CPU comparison | Correctly belongs to RC2, not the short RC1 correctness smoke |
| `vmaf-dev-mcp` documentation | Container setup and backend diagnostics | Still documents the removed Vulkan backend and cannot be handed to testers unchanged |

RC1 therefore needs a single bounded collection seam plus repairs or explicit
guards around the building blocks it invokes. A tester should not need to infer
which commands and environment details matter or paste terminal history. The
RC1 collector and guide need to produce one portable directory or archive
containing at least:

1. exact VMAFx commit, release/artifact identity, and executable digest;
2. operating system, architecture, CPU, GPU, driver, runtime, compiler, and
   relevant tool versions;
3. requested backend and detected backend/device availability;
4. commands, exit codes, stdout/stderr logs, and machine-readable results;
5. bounded correctness and parity smoke results on redistributable fixtures;
6. explicit skip/unsupported reasons instead of silently missing rows.

Each advertised backend must either execute through its canonical exclusive
selector or return an explicit, machine-readable unsupported/skip reason. RC1
uses that seam for correctness and hardware engagement, not for performance
claims. RC2 reuses its provenance envelope for the longer benchmark, profiling,
and tuning runs. This keeps the first candidate small enough to send to testers
while making their reports actionable.

## Exit-boundary options

| Boundary | Observable test | Failure mode |
| --- | --- | --- |
| “Every bug is fixed” | None; new findings make it false retroactively | Endless and unverifiable |
| “The current issue count is zero” | Queryable | Mixes performance, training, external-data work, and correctness |
| “No RC1 blocker or untriaged row remains” | Ledger classification plus exact-head required checks and a report-collector smoke | Chosen; bounded without pretending future defects cannot exist |

## Conclusion

Use RC1 for correctness completion and reproducible outside-hardware reports,
RC2 for benchmark/profiling/tuning, and RC3 for the one-shot real retrain.
Permit ordinary version PRs under the existing gates, and invalidate candidate
evidence when a later merge changes the exact head. The decision is codified in
[ADR-1341](../adr/1341-rc-correctness-benchmark-retrain-sequence.md).
