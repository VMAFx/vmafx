# ADR-1238: Require the Go security and test job through impact routing

- **Status**: Accepted
- **Date**: 2026-09-08
- **Deciders**: Lusoris
- **Tags**: ci, go, security

## Context

Commit `ffe453887` (PR #1415) added an unconstrained predictor model-card
read. The Go job failed its `gosec` scan and skipped `go test`, yet the
Required Checks Aggregator passed because `go vet + go test` was absent
from its required list. Three subsequent PRs and master inherited the
same failure. A green merge gate therefore did not establish Go validation.

The Go workflow also used workflow-level path filters. Adding its name
alone would preserve missing checks on draft promotion and would miss
native libvmaf and ONNX-model changes consumed by its tests. ADR-1140
already establishes explicit impact routing for required workflows.

## Decision

Require the existing `go vet + go test` check. Start its job on non-draft
PRs, ready-for-review events, master pushes, and manual dispatches. Route
heavy steps through `go_checks`, which inherits the existing `go` and
`c_core` selectors; unrelated documentation changes report an explicit
no-work success. Treat the Go workflow as a CI-authority input. Keep CPU
and optional-backend build settings, release exemptions, and the
aggregator's existing outcome semantics unchanged. Fix the triggering
model-card read with `os.Root` confinement to the model directory.

## Alternatives considered

| Option | Benefit | Cost | Decision |
| --- | --- | --- | --- |
| Add the Go name but keep workflow path filters | Smallest diff | Missing ready events and native/model coverage remain | Rejected |
| Run the complete Go/native job for every change | Simple routing | Unnecessary builds on documentation-only changes | Rejected |
| Keep Go advisory | No additional merge blocking | Scanner failures can suppress all Go tests behind a green gate | Rejected |
| Use existing impact routing and require Go | Explicit coverage and cheap unrelated changes | Real Go failures must be repaired before merging | Chosen |

## Consequences

- Go scanner, native build, runner smoke, and test failures now block merging.
- Changes to Go inputs, native/model inputs, or release-version authority run
  the job; documentation-only changes skip its heavy steps.
- Model cards may be ordinary files or relative symlinks remaining beneath
  the selected model directory. Escaping symlinks fall back to the existing
  filename/codec classification without reading the target.
- A workflow contract executes the actual aggregator script with mocked
  check outcomes; planner tests cover Go, native, model, release, and docs
  input classes. No new dependency or optional-backend requirement is added.

## References

- `req`: "if you find bugs/whatever, just fix them, its not out of scope".
- [ADR-1140](1140-ci-impact-planner.md): existing required-job routing policy.
- [Research-1238](../research/1238-go-security-required-gate.md):
  exact failing jobs and validation plan.
- [Go failure](https://github.com/VMAFx/vmafx/actions/runs/34219892071/job/102040270094)
  and [successful aggregator](https://github.com/VMAFx/vmafx/actions/runs/34219892163/job/102040269789)
  on the same master commit `ffe453887`.
