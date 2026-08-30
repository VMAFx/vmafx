<!-- markdownlint-disable MD013 -->
<!--
  Copyright 2026 Lusoris
  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
-->

# Research-0932: `iter.Seq[T]` adapter audit for Go packages

**Date:** 2026-05-31
**Branch:** `chore/iter-seq-adapter`
**Scope:** Identify Go functions / fields whose callers iterate once linearly
with no random access, and refactor them to expose `iter.Seq[T]` companions
without breaking the existing slice surfaces (JSON-load-bearing fields, error
formatting via `strings.Join`).

---

## 1. Candidate sweep

`grep -rIn 'func.*) \[\]' --include='*.go' pkg/ cmd/` surfaced 12 functions
returning slices. Each was triaged by reading every caller (`grep` for the
function name across the tree):

| Function / field | Callers | Pattern | Decision |
| --- | --- | --- | --- |
| `pkg/encoder.KnownEncoders()` | 1 (`strings.Join` for error msg) | Random-access via `strings.Join` | **Skip** — slice form load-bearing |
| `pkg/encoder.AvailableHardwareEncoders()` | 0 production, exported for ops | Returned over a probe API | Skip — small N, low payoff |
| `pkg/encoder.AllKnownEncoders()` | 1 (`strings.Join` for error msg) | Random-access via `strings.Join` | Skip — slice form load-bearing |
| `pkg/encoder.hardwareEncoderNames()` | 1 (concat then return) | Concat | Skip — internal |
| `pkg/storage.buildServeArgs` / `buildMountArgs` | 1 (`exec.Cmd.Args = ...`) | Random-access by `exec` | Skip — `[]string` is the contract |
| `pkg/gpu.nonEmptyLines` | 3 (uses `len(lines)`, `lines[0]`) | Random access | Skip — slice-shaped consumer |
| `pkg/libvmaf.AllowedRoots` | small N | `for _, root := range ...` | Possible, but tiny payoff (N <= 4); skip |
| `cmd/vmafx-mcp.pickWorstFrames` | 1 (JSON marshalled) | Returns into JSON | Skip — schema-load-bearing |
| `cmd/vmafx-controller/nodes.Registry.All()` | 1 (test only, expects `len`) | Iterated, future gRPC stream | **Convert** + deprecate |
| `pkg/ai.Registry.ListModels()` | 4 (tests only) | Iterated for length / presence | **Convert** + deprecate |
| `pkg/bisect.Result.Samples` (field) | 2 (JSON + chart) | JSON serialised, walked once for chart | **Add iterator companion**, keep slice |
| `pkg/ladder.LadderResult.Cloud` / `.Hull` (fields) | tests + JSON | JSON serialised, walked once by plotter | **Add iterator companion**, keep slice |
| `pkg/ladder.LadderResult.Renditions` (field) | tests + JSON, random access in tests | Random-access in tests (`renditions[i].BitratekBps`) | Skip — random-access caller |

Final set: 5 new iterator surfaces (one per package row marked Convert /
Companion).

---

## 2. Implementation choices

- **Functor signature**: `iter.Seq[T]` (Go 1.23+). The project's `go.mod`
  pins `go 1.25.0`, well past the requirement.
- **Yield contract**: every adapter honours `return false` from the
  yield callback — verified by per-surface `earlyBreak` tests.
- **Lock scope on `Registry.AllSeq`**: the iterator holds the read lock
  for its entire walk (RLock at entry, deferred RUnlock at exit). This
  mirrors the existing `All()` semantics. Callers must not call back
  into write-lock methods from inside the yield function, or they
  deadlock. The common pattern (filter → accumulate → dispatch outside
  the loop) is safe. Documented in the GoDoc.
- **Deprecation shim**: `Registry.All()` and `Registry.ListModels()`
  reduce to `slices.Collect(...AllSeq()/...ListModelsSeq())`. Behaviour
  is identical; a shim test (`TestAll_shimMatchesAllSeq`,
  `TestListModels_shimMatchesListModelsSeq`) guards the equivalence
  during the one-release deprecation window.
- **JSON-load-bearing fields stay slices**: `Samples`, `Cloud`, `Hull`
  remain `[]T` because their JSON tags drive the external schema
  (ADR-0705). The iterator is an additive ergonomic adapter.

---

## 3. Performance expectations

Two scenarios motivate the change. Numbers are back-of-envelope, not
benchmarked yet — they reflect the *shape* of the win, which is the
primary motivation; absolute numbers are not on the critical path.

| Scenario | Slice form | Iterator form |
| --- | --- | --- |
| Streaming gRPC `ListNodes` response, 50 nodes | `make([]*Node, 0, 50)` + 50 copies; handler ranges and `stream.Send`s; slice header survives the call | Direct `for n := range r.AllSeq()` → `stream.Send(n)`; no intermediate slice |
| "First node with `gpu_vendor=nvidia`" lookup | `All()` allocates 50 entries; caller breaks at index 0..k | `AllSeq()` walks until match, yields k+1 entries, then `break` releases the read lock |
| Per-title bisect, 50 ladder cells × 12 samples = 600 Sample | Single slice survives until JSON encode | If a future caller wants the streaming-chart-renderer path, no extra alloc; the field also stays for the JSON path |

---

## 4. Lint / test posture

- `go test -race ./pkg/bisect/... ./pkg/ladder/... ./pkg/ai/... ./cmd/vmafx-controller/...` — clean.
- `go vet ./pkg/...` — clean.
- `gofmt -l` — clean after one fix-up.
- Per-surface tests: full-walk parity, early-break, empty case, and (for
  the deprecated shims) shim-vs-iterator equivalence.

---

## 5. Out of scope

- Replacing the JSON-serialised slice fields entirely (would need a
  schema-major-version bump per ADR-0705).
- Adding iterators to error-formatting paths that use `strings.Join`
  (the slice round-trip via `slices.Collect` is acceptable for the
  rare error path).
- Adding iterators to vmafx-operator and vmafx-mcp surfaces (the
  former has a kubebuilder-test infrastructure gap that's unrelated;
  the latter's only slice-returner is a JSON-bound `pickWorstFrames`).

---

## 6. Reproducer

```sh
git fetch origin chore/iter-seq-adapter
git checkout chore/iter-seq-adapter
go test -race ./pkg/bisect/... ./pkg/ladder/... ./pkg/ai/... \
  ./cmd/vmafx-controller/...
```

Expected: all packages `ok`; the new tests (`TestResult_IterSamples_*`,
`TestLadderResult_Iter*`, `TestAllSeq*`, `TestListModelsSeq*`) all
pass under `-race`.
