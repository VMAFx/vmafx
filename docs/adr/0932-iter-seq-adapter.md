# ADR-0932: `iter.Seq[T]` companion APIs for single-pass Go collections

- **Status**: Accepted
- **Date**: 2026-05-31
- **Deciders**: lusoris
- **Tags**: `go`, `api`, `performance`, `ergonomics`

## Context

The fork's Go packages (`pkg/bisect`, `pkg/ladder`, `pkg/ai`, and
`cmd/vmafx-controller/nodes`) historically expose collections as `[]T`
return values or as exported slice fields. Every caller that wants to
walk the collection — whether to render a chart, stream a gRPC response,
or pattern-match for "first node matching X" — pays for a full slice
allocation up front, even when the walk would break out after the first
hit or when the result is going straight into a streaming wire-format.

Two concrete pressure points motivated the change:

1. The vmafx-controller is moving toward a streaming gRPC API
   (`ListNodes` server-streaming RPC, planned for Phase 4b.2). The
   handler currently calls `Registry.All()` and then ranges over the
   slice to send messages — twice the work the iterator would do.
2. `bisect.Result.Samples` is consumed once linearly by the chart
   renderer and by JSON marshalling, but a hypothetical long
   per-title-ladder run (12 bisect iterations × N ladder cells) keeps
   thousands of `Sample` structs live for the entire dispatch even
   though no caller re-iterates them.

Go 1.23 added `iter.Seq[T]` (the project pins Go 1.25 in `go.mod`), so
the language now offers a first-class single-pass adapter that costs the
caller a single function-pointer indirection per yield. The standard
library's `slices.Collect` is the bridge back to `[]T` for callers that
do need the slice form.

## Decision

Add `iter.Seq[T]` companion APIs to four touched packages, alongside
the existing slice APIs. The new surfaces are:

| Package | New API | Old API status |
| --- | --- | --- |
| `pkg/bisect` | `Result.IterSamples() iter.Seq[Sample]` | `Result.Samples` field unchanged (JSON-load-bearing) |
| `pkg/ladder` | `LadderResult.IterCloud() iter.Seq[Point]` | `LadderResult.Cloud` field unchanged (JSON-load-bearing) |
| `pkg/ladder` | `LadderResult.IterHull() iter.Seq[Point]` | `LadderResult.Hull` field unchanged (JSON-load-bearing) |
| `cmd/vmafx-controller/nodes` | `Registry.AllSeq() iter.Seq[*Node]` | `Registry.All()` reduced to `slices.Collect(r.AllSeq())` shim, marked `// Deprecated:`, removal targeted for v3.x.y-lusoris.N+2 |
| `pkg/ai` | `Registry.ListModelsSeq() iter.Seq[string]` | `Registry.ListModels()` reduced to `slices.Collect(r.ListModelsSeq())` shim, marked `// Deprecated:`, removal targeted for v3.x.y-lusoris.N+2 |

Two of the five existing surfaces (`Registry.All()`, `Registry.ListModels()`)
get the deprecation marker because they have zero non-test callers; the
ladder and bisect fields stay un-deprecated because they are part of the
JSON schema that external tooling consumes (ADR-0705 forbids removing
schema fields without a major-version bump).

The iterator implementations are textbook one-shot adapters: they range
over the underlying slice (or map) and call `yield`, honouring the
`return false` break contract.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| **Replace `[]T` with `iter.Seq[T]` outright** | Smaller surface; one obvious way to consume the data | Breaks the JSON-marshalled fields on `Result` / `LadderResult` (`json:"cloud"` etc. cannot marshal an `iter.Seq`); requires an unrelated schema-forward review | Schema-breaking; rejected |
| **Add iterators only on the deprecated slice methods, leave fields alone** | Smallest blast radius | Misses the long-walk wins on `Samples` / `Cloud` / `Hull` where the streaming consumer pattern is the actual motivator | Half-measure; rejected |
| **Channels (`<-chan T`) instead of `iter.Seq`** | Pre-1.23 idiom familiar to older Go reviewers | Mandatory goroutine, double-allocation, cancellation requires `context.Context`, no break-out without leaking the producer goroutine | Strictly worse; `iter.Seq` is the modern replacement |
| **Add an `Iterate(fn func(T) bool)` callback method** | No new language feature; works on Go 1.21 | Loses `for v := range coll.Iter()` ergonomics; tooling (linters, debuggers) doesn't recognise it as a range expression | Pre-1.23 workaround; rejected |
| **Status quo (slices everywhere)** | Zero churn | Misses the streaming gRPC opportunity; long-walk allocations stay | Rejected — explicit user direction to modernise |

## Consequences

- **Positive**: callers that walk a collection linearly with early-break
  semantics (e.g. "first NVIDIA node", "first failing sample") avoid the
  intermediate `make([]T, 0, len(src))` + per-element copy. The
  streaming-gRPC handler can hand the iterator straight to a `stream.Send`
  loop. New code has a single, idiomatic `for v := range coll.Iter()`
  pattern that matches `slices.All` / `maps.Values` shape.
- **Negative**: two API surfaces per touched package instead of one;
  reviewers have to decide which form to use. Callers that want the
  slice still pay one `slices.Collect` round-trip (one extra allocation
  vs. the old direct return) — the deprecation shim makes this explicit.
- **Neutral / follow-ups**: drop the `// Deprecated:` shims
  (`Registry.All`, `Registry.ListModels`) in v3.x.y-lusoris.N+2. If/when
  a major-version bump permits, revisit the JSON-backed fields on
  `Result` / `LadderResult` and consider replacing the slices with
  iterator-only access.

## References

- Go 1.23 release notes — `iter` package and `range func` support.
- Project user direction (verbatim): "Modernization #14: replace
  single-pass slice returns with `iter.Seq[T]` iterators in Go pkg."
  Paraphrased per CLAUDE.md user-quote rule; source `req`.
- ADR-0700 (Go binaries layout) — establishes `pkg/` as the canonical
  Go module location.
- ADR-0705 (JSON schema-forward invariant) — explains why the
  `cloud` / `hull` / `samples` JSON fields stay as slices.
- ADR-0713 (vmafx-node Go worker binary) — context for the
  `pkg/ai.Registry` surface being a hot loop on the worker.
