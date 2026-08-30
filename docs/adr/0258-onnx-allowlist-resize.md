<!-- markdownlint-disable MD013 MD060 -->
# ADR-0258: ONNX op-allowlist — admit `Resize` for saliency / segmentation models (T7-32)

- **Status**: Accepted
- **Date**: 2026-05-03
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: tiny-ai, onnx, security, op-allowlist

## Context

The fork's tiny-AI surface gates ONNX models behind a curated operator
allowlist
([`core/src/dnn/op_allowlist.c`](../../core/src/dnn/op_allowlist.c)).
PR #341 (U-2-Net mobilesal blocker) surfaced that `Resize` is not on
the list, which hard-stops every saliency / segmentation backbone
that ships bilinear or nearest-neighbour upsampling — U-2-Net,
mobilesal, BASNet, PiDiNet, and FPN-style detectors all rely on
`Resize` for the decoder's spatial-upsampling stages.

The risk profile of `Resize` is small. The op has no filesystem
access, no network access, no shell-out — ORT executes a pure tensor
transform parameterised by `mode` (`nearest` / `linear` / `cubic`),
`coordinate_transformation_mode`, and target spatial dims. The
worst case is a model that triggers an oversized output allocation,
which is bounded by the ADR-0167 model-size cap (50 MB) plus ORT's
own tensor-allocation guards.

The fork's wire-format scanner
([`onnx_scan.c`](../../core/src/dnn/onnx_scan.c)) gates op-type
strings, **not** attributes — per ADR-0169 / ADR D39 the scanner
deliberately stays inside a bounded-auditable scope and refuses to
pull in `libprotobuf-c`. Per-attribute restrictions (e.g. "only
`mode in ("nearest", "linear")`") would either require parsing
`AttributeProto` strings in the wire scanner or defer enforcement
to ORT runtime. We choose the latter — admit `Resize` wide-open at
the allowlist layer and document the mode contract so consumers
know the supported subset.

## Decision

Add `"Resize"` to the C allowlist's `/* convolutional */` block in
[`op_allowlist.c`](../../core/src/dnn/op_allowlist.c). The
addition is one line plus a comment block citing this ADR and
the supported-mode contract (`nearest`, `linear` recommended;
`cubic` not exercised by any in-tree consumer and numerically less
stable on quantised inputs). Python's `vmaf_train.op_allowlist`
parses the C file directly, so the export-time check picks up the
new entry automatically.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Admit `Resize` wide-open (chosen) | Trivial change; zero scanner-scope expansion; ORT enforces tensor sanity | Consumer-shipped models with `mode="cubic"` load and run | `cubic` carries no fork risk beyond what ORT already bounds; per-attribute enforcement violates the ADR D39 wire-scanner scope |
| Restrict `mode in ("nearest", "linear")` in the wire scanner | Tightest gate at load time | Requires parsing `AttributeProto` string fields — expands the scanner beyond op-type checks; precedent risk for every future attribute-restricted op | The threat model treats `Resize` as a pure tensor op; per-attribute gating is over-engineering |
| Defer until U-2-Net actually lands | Smaller PR scope | Blocks PR #341 + every future saliency / segmentation model behind a chained dependency | The blocker is concrete and the addition is one line |
| Add a runtime ORT custom-kernel wrapper | Full mode + shape enforcement at runtime | Disproportionate complexity for one op; couples the fork to an ORT custom-kernel registration path it otherwise doesn't use | The cost-benefit doesn't justify the new surface |

## Consequences

**Positive:**

- Unblocks PR #341 (U-2-Net) and the wider saliency / segmentation
  surface (mobilesal, BASNet, PiDiNet, FPN-style detectors) on the
  fork's tiny-AI pipeline.
- Symmetric with the ADR-0169 pattern: the C allowlist gains one
  entry, the Python export-time check picks it up via the regex
  parser in `vmaf_train.op_allowlist`, no additional Python edit
  needed.
- Recursive-subgraph scan (ADR-0169) automatically covers a
  hypothetical future `Resize` nested inside a `Loop.body` — no
  additional plumbing.

**Negative:**

- Models that ship `mode="cubic"` will load and run at the
  fork's tiny-AI surface. Per ADR D39 / ADR-0169 the scanner
  is op-type-only; mode-level filtering would either expand the
  scanner scope or require an ORT-runtime gate. We accept this:
  `cubic` carries no fork-specific safety risk beyond what ORT
  already bounds (output allocation, tensor-shape sanity).
- The `mode` contract (`nearest`, `linear` recommended) lives in
  the source comment + [docs/ai/security.md](../ai/security.md) +
  this ADR. Future consumers reading the allowlist must know to
  cross-reference. No mechanical enforcement.

**Neutral / follow-ups:**

- If a future fork model needs `Loop`-style attribute gating
  symmetric to ADR-0171 (bounded-iteration guard), the precedent
  exists — but `Resize` does not need it.

## Tests

- `core/test/dnn/test_op_allowlist.c` — `test_resize_op_allowed`
  asserts `Resize` accepted and case-sensitive (lowercase / uppercase
  variants still rejected).
- `core/test/dnn/test_onnx_scan.c` — `test_resize_top_level_allowed`
  hand-crafts a `ModelProto { graph = { node = Resize } }` wire fixture
  and asserts the scanner accepts it.
- `ai/tests/test_op_allowlist.py` — `test_resize_now_allowed` asserts
  the regex parser surfaces `Resize` in `load_allowlist()`.

## References

- PR #341 — U-2-Net / mobilesal blocker that surfaced the gap.
- [ADR-0039](0039-onnx-runtime-op-walk-registry.md) — op-allowlist
  walk + registry origin.
- [ADR-0169](0169-onnx-allowlist-loop-if.md) — Loop / If precedent
  recursive-subgraph scan.
- [ADR-0171](0171-bounded-loop-trip-count.md) — bounded-iteration
  guard pattern referenced for completeness.
- [docs/ai/security.md § Layer 1 — operator allowlist](../ai/security.md).
- ONNX `Resize` op spec:
  <https://github.com/onnx/onnx/blob/main/docs/Operators.md#Resize>.
- Source: `req` — user request 2026-05-03 to widen the allowlist for
  the U-2-Net + saliency unblock.
