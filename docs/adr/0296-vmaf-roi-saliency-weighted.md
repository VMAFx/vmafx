<!-- markdownlint-disable MD013 MD060 -->
# ADR-0296: Region-of-interest VMAF scoring (`vmaf-roi-score`) — saliency-weighted scaffold

- **Status**: Accepted (Option C scaffold only; Option A remains Proposed)
- **Date**: 2026-05-03
- **Deciders**: Lusoris
- **Tags**: tooling, ai, saliency, vmaf, fork-local

## Context

Standard VMAF pools per-pixel feature responses with a uniform spatial
weight. For content where viewer attention is non-uniform — talking-head
video, sports broadcasts, faces under low-quality backgrounds — a flat
pool penalises bad-background pixels equally with bad-face pixels, which
mis-reflects subjective quality. The fork has two pieces of
infrastructure that together unblock a "region-of-interest VMAF" surface:

- `saliency_student_v1` (ADR-0286 / PR #359) — a per-pixel RGB→[0,1]
  saliency model the fork already ships and validates.
- The libvmaf CLI's `--reference / --distorted` JSON output already
  exposes `pooled_metrics.vmaf.mean` as a stable scalar.

The user-facing framing is: *"content with bad background but good
faces shouldn't penalise as much as content with bad faces"*. We need
a tool that lets a downstream caller dial in how much the salient
region dominates the score.

The decision is which surface to modify:

- **Option A** — modify the libvmaf C feature pooling step to weight
  per-pixel features by the saliency mask before reduction. Per-pixel
  correct, but invasive (touches `feature_collector.c` and the model
  JSON's `feature_norm` section), risks bit-exactness contracts the
  Netflix golden gate enforces, and would burn ADR effort on numerical
  validation before any user can try the surface.
- **Option B** — apply saliency weighting at post-pool / post-predict
  time (temporal weighting across frames). Easy, but doesn't actually
  do what the user asked: spatial pooling has already collapsed the
  per-pixel signal by the time we see the scalar.
- **Option C** — wrap the existing `vmaf` CLI in a tool that runs it
  twice and blends. Easiest, no C changes, no bit-exactness risk, no
  Netflix golden-gate exposure. Cannot deliver true per-pixel weighting
  but can expose a useful "salient-region dominance" knob to callers
  today.

## Decision

We will ship `tools/vmaf-roi-score/` as a fork-local Python tool
implementing **Option C** (the name disambiguates from the existing
`core/tools/vmaf_roi.c` encoder-steering sidecar shipped under
ADR-0247): drive the `vmaf` CLI twice (full-frame + saliency-masked
distorted YUV), blend the two pooled scores via a user weight in
`[0, 1]`, and emit a JSON record with both scalars and the blend. The
CLI surface, the combine math, and the test seam ship in this PR. The
saliency-mask materialisation (the YUV-rewrite step that replaces
low-saliency pixels in the distorted YUV with the reference's pixels)
is scaffolded but not yet wired — it becomes a follow-up PR (T6-2c)
gated on `saliency_student_v1` (PR #359) merging. **Option A is
explicitly deferred** to a separate ADR; this PR documents it as future
work but does not commit the libvmaf C side to anything.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Option C (chosen) — tool-level: run `vmaf` twice + blend** | Lowest risk; zero C changes; zero bit-exactness exposure; ships behind the same model JSON the rest of the fork consumes; obvious upgrade path to A later | Cannot deliver true per-pixel weighting (the masked run gives "salient region dominance" via pixel substitution, not per-feature weighting); two `vmaf` invocations cost ~2× wall-clock | Right scaffold for the first PR — proves the surface, the JSON schema, the test seam, and the saliency-model contract without risking the Netflix golden gate or bit-exactness invariants |
| Option A — modify libvmaf C feature pooling | Mathematically correct per-pixel weighting; one invocation; integrates with GPU backends naturally | Invasive change to `feature_collector.c` and the model JSON schema; high risk of touching the Netflix golden gate (CPU bit-exact contract); much larger ADR + research surface; cross-backend numerical-diff burden (CPU/CUDA/SYCL/Vulkan all need to match) | Deferred — needs its own ADR with research-grade numerical validation. Option C unblocks user experimentation today |
| Option B — temporal saliency weighting (post-pool) | Trivial to implement (just a weighted mean across frames) | Doesn't address the user's actual request (spatial weighting). The pooling step has already discarded the per-pixel signal by the time we see scalars | Wrong surface — solves a different problem |
| Wrap inside `mcp-server/vmaf-mcp/` only | Agent-driven from day one; reuses MCP JSON-RPC | Forces non-agent users through MCP; orthogonal to a CLI need; still needs the underlying combine logic | Rejected — the combine logic belongs in a CLI tool; MCP wrapping is a follow-up |
| Defer entirely; treat ROI-VMAF as research-only | Smaller fork surface; lets downstream researchers prototype on their own | Wastes the fork-trained `saliency_student_v1` we just shipped; the user explicitly asked for a usable surface | Rejected — Option C is cheap enough to ship while we figure out Option A |
| Re-use the `vmaf_pre` filter for masking | Reuses existing filter plumbing; lives in libvmaf | `vmaf_pre` is a denoising filter, not a region selector; semantics don't compose | Rejected — wrong primitive |

## Consequences

- **Positive**:
  - User-facing ROI-VMAF surface available behind a small Python tool;
    no C changes, no bit-exactness exposure, no Netflix golden-gate
    risk.
  - Establishes the `vmafroi.SCHEMA_VERSION` JSON contract that future
    Option A or B variants can re-use.
  - Tests are pure Python with `subprocess.run` mocked — runs in any
    CI environment that has the rest of `make test` infrastructure.
  - Documents Option A as a tracked deferred item rather than letting
    the idea bit-rot in the planning dossier.
- **Negative**:
  - "ROI-VMAF" computed by Option C is **not** equivalent to a
    per-pixel saliency-weighted VMAF. The user-facing docs make this
    explicit; the limitation is also pinned in the AGENTS.md note.
  - Two `vmaf` invocations roughly double scoring wall-clock for the
    ROI variant — acceptable for the prototype use case but means we
    cannot use this surface for high-throughput per-frame analysis.
- **Neutral / follow-ups**:
  - **Implementation phasing**:
    1. *(this PR, Phase 0)* — combine math, CLI surface, JSON schema,
       subprocess seam, smoke tests with `--synthetic-mask`.
    2. *(T6-2c, follow-up PR)* — wire the saliency-mask materialiser
       to ONNX Runtime + numpy YUV reader/writer; integration test
       against a real `saliency_student_v1` ONNX.
    3. *(separate ADR, future)* — Option A: per-pixel feature pooling
       with saliency weights inside `feature_collector.c`; full
       cross-backend numeric validation gate.
  - The ROI-VMAF ↔ MOS correlation question (does this score actually
    track subjective quality better than uniform VMAF?) is **explicitly
    not answered** by this PR. Validation is research follow-up — see
    Research-0063 §"What we deliberately don't measure".
  - When PR #359 (`saliency_student_v1`) merges, update the docs
    install snippet to reference the canonical model path
    `model/tiny/saliency_student_v1.onnx` instead of the current
    `mobilesal.onnx` placeholder.

## References

- Source: agent task brief (paraphrased: the user requested a
  region-of-interest VMAF mode that weights the score by the
  `saliency_student_v1` mask, with the explicit instruction to pick
  Option C as the lowest-risk first scaffold and document Option A as
  future work).
- Hard rule from the brief: "DO NOT modify the libvmaf C side in this
  PR — that's the riskier Option A."
- Hard rule from the brief: "DO NOT claim ROI-VMAF correlates better
  with MOS without measurement (research validation is a follow-up)."
- ADR-0286 — `saliency_student_v1` (the saliency model this surface
  consumes).
- ADR-0042 — Tiny-AI docs-required-per-PR rule.
- ADR-0100 — Project-wide doc-substance rule (per-surface bars).
- ADR-0108 — Six deep-dive deliverables rule.
- ADR-0237 — `vmaf-tune` (sibling tool; `vmaf-roi` mirrors its
  layout + its `score.py` subprocess seam).
- Research-0063 — Region-of-interest VMAF: option-space digest.
- PR #359 — `saliency_student_v1` (currently open; this PR will work
  with the model once that PR merges).
