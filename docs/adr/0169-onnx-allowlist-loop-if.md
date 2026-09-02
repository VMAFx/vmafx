<!-- markdownlint-disable MD013 -->
# ADR-0169: ONNX op-allowlist — admit `Loop` + `If` with recursive subgraph scan (T6-5)

- **Status**: Accepted
- **Date**: 2026-04-25
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: tiny-ai, onnx, security, op-allowlist

## Context

[BACKLOG T6-5](../../.workingdir2/BACKLOG.md) calls for "Op-allowlist
expansion (`Loop`, `If` with bounded-iteration guard). Unlocks MUSIQ /
RAFT / small VLMs. `Scan` stays rejected." The existing allowlist in
[`core/src/dnn/op_allowlist.c`](../../core/src/dnn/op_allowlist.c)
contains pure feed-forward ops (arithmetic, conv, normalisation,
activations, dense, dropout, QDQ, structural, constants). Every
control-flow op was rejected outright.

The cost of rejecting all control flow is concrete: well-known
small-model architectures rely on `Loop` for iterative refinement
(diffusion-style or RAFT optical-flow), and `If` for conditional
branches (resolution-dependent backbones). Without these ops, the
allowlist hard-stops anyone wanting to ship those models through
the fork's tiny-AI surface.

The risk of admitting `Loop` and `If` is also concrete: their
**subgraphs** (`Loop.body`, `If.then_branch`, `If.else_branch`) can
contain arbitrary ops. A naive allowlist edit that just adds `Loop`
to the top-level scanner would let an attacker hide forbidden ops
inside a Loop body — defeating the whole point of the allowlist.

The fork's existing scanner
([`onnx_scan.c`](../../core/src/dnn/onnx_scan.c)) walks the
ModelProto wire format three levels deep (Model → Graph → Node →
op_type) and explicitly does not recurse into `NodeProto.attribute`,
which is where the embedded subgraphs live (per ADR D39's bounded-
auditable-scope justification for not pulling in `libprotobuf-c`).

## Decision

### 1. Add `Loop` and `If` to the C allowlist; keep `Scan` rejected

[`op_allowlist.c`](../../core/src/dnn/op_allowlist.c) gains two
new entries under a `/* control flow */` section. `Scan` stays off
the list — its variant-typed input/output binding (sequence-typed
inputs/outputs, axis specifications, scan_input_directions) makes
static bound-checking impractical for a wire-format scanner; revisit
if a concrete consumer model surfaces.

### 2. Recursive subgraph scan in the C wire-format scanner

[`onnx_scan.c`](../../core/src/dnn/onnx_scan.c) gains a
`scan_attribute` helper that walks `AttributeProto` and, for every
`AttributeProto.g` (single embedded `GraphProto`, field 6) or
`AttributeProto.graphs` (repeated `GraphProto`, field 11),
recursively invokes `scan_graph` on the embedded subgraph.
`scan_node` is extended to descend into `NodeProto.attribute`
(field 5) and call `scan_attribute` on each.

The recursion is depth-bounded by `VMAF_DNN_MAX_SUBGRAPH_DEPTH = 8`.
Real models keep nesting shallow (Loop within If at most). The cap
is a defence-in-depth bound against pathological recursion in
malformed input — not a feature ceiling.

The scan is **structural** — it visits every NodeProto and verifies
its op_type. It does **not** care about op_type when deciding to
recurse: it always recurses into AttributeProto graph fields. This
means the allowlist contract is uniformly applied at every depth: a
forbidden op nested anywhere fails the scan regardless of the
enclosing op_type.

### 3. Mirror the recursion in the Python export-time check

[`vmaf_train.op_allowlist`](../../ai/src/vmaf_train/op_allowlist.py)
gains a `_collect_op_types` helper that walks `GraphProto.node` and
recurses into `AttributeProto.g` / `.graphs` whenever the attribute
type is `GRAPH` / `GRAPHS`. Both `check_model` and `check_graph` use
it. This keeps the export-time check (catches the issue at
`vmaf-train export` time) and the runtime load-time check (catches
the issue at `vmaf_dnn_load`) in lockstep.

### 4. Bounded-iteration guard — explicitly deferred

The BACKLOG row called for "bounded-iteration guard" alongside the
allowlist expansion. **This is deferred to a follow-up PR.** Proving
that a Loop's trip-count input is statically bounded requires
backwards data-flow analysis (the trip-count input must trace to a
`Constant` node with a known `int64_data` value ≤ some cap), which
is non-trivial in a wire-format scanner. The follow-up PR can either:

- (a) Extend `onnx_scan.c` to enforce that any `Loop` node's first
  input traces to a `Constant ≤ MAX_LOOP_ITERATIONS`, with
  `MAX_LOOP_ITERATIONS` a build-time constant (suggested: 65536).
- (b) Add a runtime guard via an ORT custom kernel that wraps `Loop`
  with an iteration counter.
- (c) Punt entirely — accept that `Loop` consumers must self-bound
  their iteration counts, and document this as a contract.

Path (a) keeps enforcement at the load-time scanner; path (b) moves
it to runtime; path (c) trusts the consumer. Tracking the choice as
a separate ADR keeps this PR's scope honest.

## Alternatives considered

1. **Reject `Scan` explicitly via a deny-list rather than relying on
   default-deny.** Rejected: the allowlist is already a closed list;
   listing `Scan` separately as denied would suggest there's a
   carve-out, which there isn't. Better to document in the comment
   block above the new `"Loop", "If"` entries that `Scan` is
   intentionally absent.

2. **Walk `AttributeProto.subgraph` only when the parent op_type is
   `Loop` or `If`.** Rejected: Future ONNX ops may also embed
   subgraphs (e.g. `SequenceMap`). Always recursing into any
   AttributeProto subgraph field is forward-compatible and never
   wrong — a non-control-flow op simply has no embedded graph
   attribute, so the recursion never fires.

3. **Defer until ORT's `OptimizedModel` introspection API matures.**
   Rejected: ORT 1.22 still does not expose a stable C API for
   per-node iteration that includes subgraph descent. Pinning the
   scanner to wire-format parsing (per ADR D39) is the long-game
   choice.

4. **Add a `--allow-control-flow` CLI flag that gates the relaxation
   per call site.** Rejected: complicates the trust-root model. The
   allowlist is the contract; either an op is on it or it isn't.
   Per-call gates create surface area for bypasses.

5. **Ship the bounded-iteration guard in the same PR.** Rejected:
   the data-flow analysis for `Loop.M → Constant` doubles the scope
   and the existing T6-5 sizing was "S". Better to ship the
   allowlist + recursive-scan now and queue the iteration guard
   separately.

## Consequences

**Positive:**

- Tiny-AI surface admits `Loop` + `If` baselines that were
  previously blocked. MUSIQ / RAFT / small-VLM-class models become
  exportable through the fork's pipeline.
- The `kind: "filter"` enum from
  [ADR-0168](0168-tinyai-konvid-baselines.md) is now compatible
  with stateful filter models (e.g. recurrent denoisers using
  `Loop`).
- Recursive-scan invariant is uniform: a forbidden op cannot hide
  inside any embedded subgraph at any depth.
- Python and C scanners stay symmetric — the export-time check
  catches issues before the model ever leaves the trainer.

**Negative:**

- Without the bounded-iteration guard, a malicious or poorly-
  written `Loop` model could enter an unbounded compute loop at
  runtime. ORT's process-level inference timeout is the only
  defence today. Operators consuming untrusted models should set
  an inference timeout via ORT's `RunOptions`. Tracked as
  follow-up T6-5b in BACKLOG.
- `MAX_SUBGRAPH_DEPTH = 8` is an arbitrary heuristic. If a future
  legitimate model nests deeper than 8 levels, the cap would need
  bumping. No model in the fork's current consumer list comes
  close.
- ABI-additive change to `op_allowlist.c` only — no header-level
  surface change for downstream consumers. A few existing tests
  that asserted "Loop / If are rejected" needed flipping (already
  updated in this PR).

## Tests

Test changes in this PR:

- `core/test/dnn/test_op_allowlist.c`
  - Renamed `test_custom_ops_rejected` to keep just the
    NULL/empty/unknown checks.
  - New `test_control_flow_ops_allowed` asserts Loop + If accepted,
    Scan still rejected.
- `core/test/dnn/test_onnx_scan.c`
  - `test_disallowed_op_loop` → `test_loop_top_level_allowed`
    (flipped expectation).
  - `test_disallowed_op_if_after_allowed` →
    `test_if_after_allowed_now_accepted` (flipped expectation).
  - New `test_scan_still_rejected` covers `Scan` rejection.
  - New `test_loop_with_allowed_subgraph` (Loop body = Conv → ok).
  - New `test_loop_with_forbidden_subgraph` (Loop body = "Fake"
    → rejected with `first_bad="Fake"`).
- `ai/tests/test_op_allowlist.py`
  - `test_control_flow_ops_are_not_allowed` →
    `test_loop_and_if_now_allowed` + `test_scan_still_rejected`.
  - `test_forbidden_op_rejected` switched fixture from `Loop` to
    `Scan`.
  - New `test_loop_body_with_allowed_op_passes` (Loop body = Relu
    → ok).
  - New `test_loop_body_with_forbidden_op_rejected` (Loop body =
    `FakeOp` → rejected; Loop wrapper itself still allowed).

## References

- [BACKLOG T6-5](../../.workingdir2/BACKLOG.md) — backlog row.
- [Wave 1 roadmap § Op-allowlist expansion](../ai/roadmap.md).
- ADR D39 — onnx_scan.c bounded-scope rationale (no libprotobuf-c).
- [ADR-0020](0020-tinyai-four-capabilities.md) — Tiny-AI four-
  capabilities surface.
- [ADR-0022](0022-inference-runtime-onnx.md) — ONNX Runtime as the
  inference backend.
- [ADR-0168](0168-tinyai-konvid-baselines.md) — sister ADR landing
  the C2/C3 baselines.
- ONNX wire format reference:
  <https://github.com/onnx/onnx/blob/main/onnx/onnx.proto>
- `req` — user popup choice 2026-04-25: "T6-5 op-allowlist expansion
  (S, Recommended)".
