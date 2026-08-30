<!-- markdownlint-disable MD036 -->
# ADR-0171: Bounded `Loop.M` trip-count guard (T6-5b)

- **Status**: Accepted
- **Date**: 2026-04-25
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: tiny-ai, onnx, security, op-allowlist

## Context

[ADR-0169](0169-onnx-allowlist-loop-if.md) admitted `Loop` and `If`
to the ONNX op-allowlist with recursive subgraph scanning so a
forbidden op can't hide inside `Loop.body` / `If.then_branch` /
`If.else_branch`. ADR-0169 § "Bounded-iteration guard — explicitly
deferred" tracked the unfinished half:

> Without the bounded-iteration guard, a malicious or poorly-written
> `Loop` model could enter an unbounded compute loop at runtime.
> ORT's process-level inference timeout is the only defence today.
> Operators consuming untrusted models should set an inference
> timeout via ORT's `RunOptions`. Tracked as follow-up T6-5b in
> BACKLOG.

This ADR closes that follow-up. It picks the **load-time data-flow**
path described in ADR-0169 § Alternatives: prove that every `Loop`
node's first input traces to a `Constant` int64 scalar whose value
is in `[0, MAX_LOOP_TRIP_COUNT]` (default 1024). Models that don't
meet that bound are rejected before ORT ever sees them.

The 1024 ceiling is deliberate: production tiny-AI baselines —
diffusion samplers, RAFT optical-flow refinement, MUSIQ
auxiliary-attention — fit comfortably below 64 iterations; the cap
is chosen high enough to never reject a legitimate model and low
enough that a hostile model pointing `Loop.M` at `int64.MAX` trips
the rejection.

## Decision

### Two-layer enforcement (matching ADR-0167's pattern)

**Layer 1 — Python export-time (rigorous)**

[`vmaf_train.op_allowlist`](../../ai/src/vmaf_train/op_allowlist.py)
gains a `_collect_loop_violations` helper that walks the graph,
finds every `Loop` node, looks up the producer of its first input
(`M`) in the local `output_name → node` map, and:

- accepts when the producer is a `Constant` node carrying a scalar
  `int64` tensor with value in `[0, max_loop_trip_count]`;
- rejects with a precise diagnostic otherwise (`graph input`,
  `not a scalar int64 Constant`, `M=<value>` out of range).

The check recurses into embedded subgraphs: a `Loop` nested inside
a `Loop.body` must itself be statically bounded (each subgraph
scope tracks its own producer map; a `Constant` from the outer
graph can't satisfy a `Loop.M` inside a body).

`AllowlistReport` gains a `loop_violations: tuple[str, ...]` field
and a strengthened `pretty()` that surfaces both forbidden ops and
unbounded Loops in the same string. `MAX_LOOP_TRIP_COUNT = 1024` is
a module-level constant; both `check_model` and `check_graph` accept
a `max_loop_trip_count=` override for callers with longer iterative
pipelines.

**Layer 2 — C wire-format scanner (counter cap)**

[`onnx_scan.c`](../../core/src/dnn/onnx_scan.c) gains a counter
that increments every time we see `op_type == "Loop"` at any depth
(top-level graph or any embedded subgraph). The counter is shared
across the recursion via a new `unsigned *loop_count` parameter
threaded through `scan_graph` / `scan_node` / `scan_attribute`.
Exceeding `VMAF_DNN_MAX_LOOP_NODES = 16` returns `-EPERM` with
`first_bad = "Loop"`, mirroring the existing forbidden-op rejection
path.

The counter cap is intentionally **simpler** than the Python
data-flow check. Reproducing the producer-map lookup in the
wire-format scanner would require:

- buffering every `Constant` node's int64 value alongside its
  output name,
- buffering every `Loop` node's first input name,
- a second pass to join the two,

which violates the ADR D39 "bounded-auditable-scope" rationale that
keeps the scanner from pulling in `libprotobuf-c`. The counter cap
is a coarser bound (16 nested Loops can still iterate a lot), but
it's the smallest invariant that catches the worst-case "thousands
of Loops within Loops" attack purely from wire-format observation.

**Layered defence model** mirrors ADR-0167 (doc-drift):

- The Python check is the **practical** gate — it runs at
  `vmaf-train export` time, before any model touches the wire, and
  surfaces precise per-Loop diagnostics.
- The C check is the **last-line** gate — it runs at
  `vmaf_dnn_load` time on every model, including ones that bypass
  the trainer (third-party models, MCP-uploaded models).
- Together they bound runtime exposure: even a model that lies its
  way past the Python check still hits a 16-Loop cap when the
  C scanner sees it.

## Alternatives considered

1. **Skip the C scanner cap; rely on Python alone.** Rejected: the
   C scanner runs at every `vmaf_dnn_session_open` call, not just
   on models that came through `vmaf-train`. A model loaded from
   an HTTP URL by the MCP server, or one a user `pip install`s
   from a third-party tiny-AI registry, never sees the trainer.
2. **Walk the producer chain in the C scanner.** Rejected for ADR
   D39 scope reasons (would double the scanner LoC and require a
   second pass through the wire format buffer). The counter cap is
   pragmatically equivalent in the worst case.
3. **Set `MAX_LOOP_TRIP_COUNT = ∞` and add an ORT runtime timeout
   instead.** Rejected: ORT's `RunOptions::SetTerminate()` is a
   thread-cooperative check that fires on subgraph boundaries; a
   model that doesn't yield often will run for the full timeout
   window before yielding to the cancel signal. Load-time rejection
   is strictly safer.
4. **Bake `MAX_LOOP_TRIP_COUNT` into a build flag.** Rejected for
   premature configurability. Per-call override on the Python side
   (`max_loop_trip_count=`) handles the few legitimate
   exceptions; the C side picks one constant deliberately and
   keeps it close to the call site for auditability.
5. **Reject any Loop whose `M` input is a graph input** (i.e. fed
   by the caller at runtime). The current Python implementation
   *does* reject these — there's no static evidence that the
   trip count is bounded. Listed here for clarity; this is the
   chosen behaviour, not an alternative rejected.

## Consequences

**Positive:**

- Closes the bounded-iteration follow-up flagged in ADR-0169 with
  the picked path (a) from that ADR's § Alternatives.
- The Python check produces actionable diagnostics
  (`<top>::Loop(M=4096, max_trip_count=1024)`); model authors know
  exactly which Loop and exactly which constant to fix.
- The C cap is a 5-line addition (counter + check); the rest of
  the diff is plumbing the counter through three function
  signatures.
- Per-call `max_loop_trip_count=` override lets operators with
  legitimate longer pipelines (e.g. iterative diffusion samplers
  with > 1024 steps) opt in without forking the codebase.

**Negative:**

- The C cap is coarse — 16 well-bounded Loops trip the same gate
  as 16 unbounded ones. Operators using a model with many Loops
  (rare, but possible in some VLM architectures) need to either
  bump the constant or rely on the Python check having already
  cleared the model. Acceptable for now; revisit if a real
  consumer hits the cap.
- Test fixtures for "many Loops" require runtime-built protobuf
  buffers (the existing tests are static byte arrays). Cost is
  one ~30-line helper function in
  `test/dnn/test_onnx_scan.c`.
- A model author who hand-writes `Loop.M` as a graph input
  (rather than wiring it to a `Constant`) gets rejected with a
  clear "M is a graph input, not a Constant" message. The fix
  is to fold the trip count into a `Constant` node — standard
  ONNX export convention.

## Tests

- `ai/tests/test_op_allowlist.py` (5 new):
  - `test_loop_with_bounded_const_trip_passes` — Constant M=512 ok.
  - `test_loop_with_too_large_const_trip_rejected` — M=4096
    rejects.
  - `test_loop_with_negative_const_trip_rejected` — M=-1 rejects.
  - `test_loop_with_dynamic_M_input_rejected` — M as graph input
    (non-Constant) rejects with "graph input" diagnostic.
  - `test_caller_can_override_max_trip_count` — explicit
    `max_loop_trip_count=10000` lets a 5000-trip Loop pass.
  - Existing `test_loop_body_with_allowed_op_passes` updated to
    use a `Constant` trip count (the previous fixture used a
    graph input M, which now fails the bounded check by design).
- `core/test/dnn/test_onnx_scan.c` (1 new):
  - `test_too_many_loop_nodes_rejected` — 17 top-level Loops trip
    the `VMAF_DNN_MAX_LOOP_NODES = 16` cap with `-EPERM` and
    `first_bad = "Loop"`.

## References

- [BACKLOG T6-5b](../../.workingdir2/BACKLOG.md) — backlog row.
- [ADR-0169](0169-onnx-allowlist-loop-if.md) — sibling ADR that
  deferred this guard.
- [ADR-0167](0167-doc-drift-enforcement.md) — same two-layer
  enforcement pattern (in-session hook + CI gate; here Python
  export check + C scanner check).
- ONNX `Loop` operator spec:
  <https://github.com/onnx/onnx/blob/main/docs/Operators.md#Loop>
- `req` — user popup choice 2026-04-25: "T6-5b bounded-iteration
  guard (S, Recommended)".
