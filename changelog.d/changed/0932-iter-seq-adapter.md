- Add `iter.Seq[T]` companion methods to four Go packages so callers can
  walk long, single-pass collections without allocating an intermediate
  slice (ADR-0932). New surfaces:
  - `pkg/bisect`: `Result.IterSamples() iter.Seq[Sample]`.
  - `pkg/ladder`: `LadderResult.IterCloud() iter.Seq[Point]` and
    `LadderResult.IterHull() iter.Seq[Point]`.
  - `cmd/vmafx-controller/nodes`: `Registry.AllSeq() iter.Seq[*Node]`.
  - `pkg/ai`: `Registry.ListModelsSeq() iter.Seq[string]`.

  The slice-returning forms (`Result.Samples` field, `LadderResult.Cloud` /
  `.Hull` fields, `Registry.All()`, `Registry.ListModels()`) remain
  available. `Registry.All()` and `Registry.ListModels()` are marked
  `// Deprecated:` and will be removed one release after this one; the
  ladder and bisect fields stay because they back the JSON schema.

  Motivated by gRPC streaming responses for the controller / node split
  and by long bisect walks (12+ iterations × ladder cells) where the
  caller currently snapshots the whole `[]Sample` just to iterate it
  once. Net result: zero allocation on the hot path when the caller
  iterates linearly with no random access.
