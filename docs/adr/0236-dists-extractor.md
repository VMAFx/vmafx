<!-- markdownlint-disable MD060 -->
# ADR-0236 — DISTS extractor as LPIPS companion

- **Status**: Accepted
- **Date**: 2026-05-01
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: ai, fr, dnn, tiny-ai, fork-local, perceptual

## Context

Bristol VI-Lab's 2026 NVC review (audited as
[`Research-0033`](../research/0033-bristol-nvc-review-2026.md)) §5.3
discusses LPIPS [Zhang 2018, ref [239]] *and* DISTS [Ding 2020, ref
[240]] as the deep-feature FR pair widely cited for video quality.
Where LPIPS measures normalised feature distances, DISTS combines a
texture-similarity term (channel-wise mean) with a structural-similarity
term (channel-wise variance) on VGG features, and is documented to
correlate better than LPIPS on synthetic-distortion benchmarks. The
two are complementary, not redundant.

The fork already ships `lpips_sq` ([ADR-0041](0041-lpips-onnx-extractor.md))
as a tiny-AI FR extractor. We do not ship DISTS. The Bristol audit
flagged this as actionable item #5 with effort estimate 1 week.

Filing the design proposal now so the implementation PR has the ABI

- op-allowlist contract pre-committed; the implementation work is
tracked separately as backlog item `T7-DISTS`.

## Decision

Add a new tiny-AI feature extractor `dists_sq` that mirrors the
existing `lpips_sq` shape:

- **Backbone**: VGG-16 features (`relu1_2`, `relu2_2`, `relu3_3`,
  `relu4_3`, `relu5_3`) — same five layers DISTS specifies. (Ding's
  reference implementation also offers a SqueezeNet variant for
  smaller footprint; the `_sq` suffix in `dists_sq` mirrors LPIPS's
  SqueezeNet variant naming convention used in `lpips_sq.onnx`.)
- **Inputs**: two NCHW float32 tensors `ref` + `dist`, ImageNet-
  normalised — byte-identical to `lpips_sq.onnx` so the host-side
  preprocessing in [`core/src/dnn/`](../../core/src/dnn/) can
  be reused.
- **Output**: scalar `score` per frame.
- **Public surface**: registers via `VmafFeatureExtractor`-style
  table; emits `dists_sq` per frame via
  `vmaf_feature_collector_append`. Picks up `model_path` from the
  feature option or `VMAF_DISTS_SQ_MODEL_PATH` env var (mirrors
  LPIPS / FastDVDnet).
- **Decline contract**: `-EINVAL` if neither model path is
  configured at init.
- **Smoke / placeholder**: ship a randomly-initialised
  ~330-byte–scale ONNX with the right I/O shape, marked
  `smoke: true` in `model/tiny/registry.json`. Real upstream-derived
  weights ([Ding's PyTorch reference](https://github.com/dingkeyan93/DISTS),
  MIT-licensed) tracked as **T7-DISTS-followup**.

Op-allowlist: VGG-16 export hits `Conv`, `MaxPool`, `Relu`,
`Flatten`/`GlobalAveragePool`, `Mul`, `Add`, `Div`, `Sub`,
`ReduceMean`, plus the DISTS-specific channel-wise statistics
(`ReduceMean` along H,W → channel-wise mean and `ReduceMean` of
square minus square of mean → channel-wise variance). All ops are in
the existing op-allowlist used by `lpips_sq` plus `ReduceMean` (which
is already in [`docs/research/0006-tinyai-ptq-accuracy-targets.md`](../research/0006-tinyai-ptq-accuracy-targets.md)).
No new op gating needed.

## Alternatives considered

| Alternative                                                                          | Pros                                                | Cons                                                                                                                                                                                            | Why not chosen |
| ------------------------------------------------------------------------------------ | --------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | -------------- |
| **Add DISTS as a new tiny-AI extractor (chosen)**                                    | Symmetric with LPIPS surface; cheap to integrate    | Two FR extractors that overlap conceptually but not numerically; users have to know which to pick                                                                                               | Right balance — Bristol audit's strongest follow-up signal, low marginal cost |
| Replace LPIPS with DISTS                                                             | Single FR extractor                                 | Backwards-incompatible (drops `lpips_sq` from the registry); some downstream users have already calibrated against LPIPS; LPIPS is more widely cited                                            | Rejected — breaks an existing public surface |
| Ship DISTS only, retire LPIPS                                                        | Forces consumers onto the better-correlating metric | Same incompatibility issue; we'd lose the LPIPS literature lineage that's already cited in tiny-AI docs                                                                                         | Rejected — same reason |
| Re-export LPIPS to also emit DISTS-style channel-variance stats from the same VGG forward pass | One ONNX file, two scalars                          | Conflates two distinct papers' metrics in one model card; complicates the licensing trail (LPIPS BSD-2 vs DISTS MIT); the per-frame compute saving is small (the VGG forward pass dominates either way) | Rejected — clarity wins |
| Defer entirely until a real-world pull comes in                                      | Zero cost                                           | Bristol audit explicitly flags DISTS as the LPIPS companion the fork is missing; deferring drops a documented gap on the floor                                                                  | Rejected — the proposal cost is small enough to land now |

## Consequences

### Positive

- Closes Research-0033 actionable #5 (DISTS as LPIPS companion).
- Symmetric extractor surface with LPIPS — reuses the same
  preprocessing path in `core/src/dnn/`, the same op-allowlist
  framework, and the same model-card / license / sigstore-bundle
  pattern.
- Provides the second FR deep-feature metric that the Bristol audit's
  decision-matrix flagged as a known gap.

### Neutral / follow-ups

1. **T7-DISTS** — implementation PR. Scaffold the C extractor
   (~300 lines mirroring `feature_lpips.c`), the meson wiring, the
   placeholder ONNX export script under `ai/scripts/`, the registry
   entry, the smoke test under `core/test/test_dists.c`, the
   model card under `docs/ai/models/dists_sq.md`. Effort: M (3-5
   days).
2. **T7-DISTS-followup** — replace the placeholder ONNX with
   real upstream weights derived from Ding's reference
   implementation. Pull weights, port the network arithmetic verbatim,
   verify against the published DISTS scores on a small held-out
   sample (the paper reports SROCC numbers on LIVE / TID2013 / KADID
   that we can use as a sanity gate).
3. **PTQ pipeline** — once real weights land, run them through the
   measure_quant_drop.py audit harness ([ADR-0207](0207-tinyai-qat-design.md))
   with `quant_mode="static"` first, then `"dynamic"` if static
   misses budget. PLCC budget: 0.005 (looser than `learned_filter_v1`'s
   0.002 because DISTS is a perceptual signal and small drift is
   tolerated).

### Negative

- Doubles the deep-feature FR surface area (LPIPS + DISTS), which
  means doubling the model-card maintenance + license-tracking
  supply-chain (sigstore bundle, ADR-0211) burden.
- Some marginal user confusion ("which one should I use?") that
  doc copy in the model card has to address head-on.

## References

- Ding, Ma, Wang, Simoncelli, *Image Quality Assessment: Unifying
  Structure and Texture Similarity*, IEEE PAMI 2020.
- Zhang, Isola, Efros, Shechtman, Wang, *The Unreasonable
  Effectiveness of Deep Features as a Perceptual Metric*, CVPR 2018
  (LPIPS).
- Gao et al., *Advances in Neural Video Compression: A Review and
  Benchmarking*, Bristol VI-Lab preprint 2026 (Research-0033 §5.3,
  refs [239]–[240]).
- [`docs/research/0033-bristol-nvc-review-2026.md`](../research/0033-bristol-nvc-review-2026.md)
  — actionable items table, item #5.
- [`docs/research/0043-dists-extractor-design.md`](../research/0043-dists-extractor-design.md)
  — design digest landed alongside this ADR.
- [ADR-0041](0041-lpips-onnx-extractor.md) — the LPIPS sibling whose
  shape this proposal mirrors.
- [ADR-0211](0211-model-registry-sigstore.md) — license + Sigstore
  bundle metadata that the new entry will populate once real weights
  land.

## req / Q

- `req`: Research-0033 audit explicitly flagged DISTS as the missing
  LPIPS companion (actionable item #5).

### Status update 2026-05-08: implementation not started (T7-DISTS)

Audited as part of the 2026-05-08 ADR `Proposed` sweep
([Research-0086](../research/0086-adr-proposed-status-sweep-2026-05-08.md)).

This ADR scoped the API + op-allowlist contract before the
implementation PR. The acceptance criteria are not yet in tree:

- No `core/src/feature/dists*` files
  (`ls core/src/feature/dists*` returns no match).
- No `dists_sq` row in `model/tiny/registry.json`.
- No placeholder ONNX, no env-var consumer, no smoke test.

The work is tracked as backlog item **T7-DISTS** per the ADR's own
scoping note. The "actionable item #5" framing from the Bristol
NVC review (`Research-0033`) holds; nothing in tree contradicts the
scope as written. Stays **Proposed** until the implementation PR
lands; that PR will close out via a follow-up status-update
appendix.

Verification command:

```sh
ls core/src/feature/dists* 2>&1 | grep -E "no match|cannot access"
grep -c '"dists_sq"' model/tiny/registry.json   # expects 0
```

### Status update 2026-05-14: Accepted — smoke extractor landed

The implementation PR closes the original `T7-DISTS` surface:

- `core/src/feature/feature_dists.c` registers `dists_sq` and consumes
  `model_path` or `VMAF_DISTS_SQ_MODEL_PATH`.
- `model/tiny/dists_sq.onnx` and `model/tiny/dists_sq.json` ship the
  smoke checkpoint contract with registry id `dists_sq_placeholder_v0`.
- `core/test/test_dists.c` locks registration and missing-model
  behaviour.
- [`docs/metrics/dists.md`](../metrics/dists.md) and
  [`docs/ai/models/dists_sq.md`](../ai/models/dists_sq.md) document the
  user-facing metric and model-card surface.

The real learned DISTS weights remain `T7-DISTS-followup`. The accepted
decision is the extractor ABI and in-tree smoke surface, not a claim that
the placeholder ONNX reproduces Ding et al.'s published DISTS scores.
