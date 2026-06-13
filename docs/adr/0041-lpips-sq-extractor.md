<!-- markdownlint-disable MD013 MD060 -->
# ADR-0041: Ship LPIPS-SqueezeNet FR extractor with inverse-ImageNet in graph

- **Status**: Accepted
- **Date**: 2026-04-17
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: ai, dnn, cli

## Context

Wave 1 track 2.2 (ADR-0036b) needs an externally-validated perceptual FR baseline. `richzhang/PerceptualSimilarity` SqueezeNet v0.1 is the reference implementation (~724k params, 3.2 MB ONNX). LPIPS's native input convention is `[-1,1]`, while every ImageNet-family sibling model (MobileSal, MUSIQ) expects the standard ImageNet normalisation. The choice of where the range conversion lives determines whether the C tensor-I/O surface stays unified or fragments per model.

## Decision

Land `ai/lpips_export.py` that re-exports `richzhang/PerceptualSimilarity` SqueezeNet v0.1 (sha256 `1402626680d5b69a793e647edda2c32f04e192f5cf1e7837bec8bde14187a261`) as a two-input graph with named inputs `ref`/`dist` and scalar output `score` (consumes ADR-0040 API verbatim). The exporter wraps the LPIPS core in `_LpipsImagenetWrapper` that absorbs the inverse-ImageNet transform inside the graph (`x01 = x·std + mean; x_lpips = x01·2 − 1`): C ships ImageNet-normalised tensors via `vmaf_tensor_from_rgb_imagenet()`, graph converts to LPIPS `[-1,1]` on entry. Opset: requested 17, torch dynamo emitted 18 (downconvert 18→17 failed with `RuntimeError` in `onnx.version_converter`); registry + sidecar record 18. Extractor `core/src/feature/feature_lpips.c`: 8-bit-only (10-bit rejected at init); BT.709 limited-range YUV→RGB with nearest-neighbour chroma upsample (deterministic, no bilinear drift vs. upstream reference); model-path resolution is option → `VMAF_LPIPS_MODEL_PATH` env → `-EINVAL`; registered unconditionally in `feature_extractor_list[]` — when DNN is disabled, stubs return `-ENOSYS` from init().

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Native `[-1,1]` transform in a new C helper | Minimal exporter change | Every ImageNet-family sibling model needs its own helper or a second branch in common path | Rejected — fragments surface |
| Compile-time `model_path` macro | Simple | Violates runtime-discovery pattern every other extractor uses | Rejected |
| Auto-discover `lpips_sq.onnx` relative to argv[0] | Plug-and-play | Breaks cross-invocation reproducibility; surprises packaged installs | Rejected |
| Gate registration on `dnn_have_ort` | Smaller feature list when DNN off | `--feature=lpips` then errors with "unknown feature" instead of "DNN disabled" — worse UX | Rejected |
| Absorb range conversion in graph + unconditional registration (chosen) | Unified C tensor surface; self-describing graph; consistent error UX | Opset bumps to 18 | Correct |

Rationale note: absorbing the inverse transform in-graph means (a) C path is identical for LPIPS, MobileSal, MUSIQ, future ImageNet-family; (b) graph is self-describing — consumers don't need a sidecar to know normalisation; (c) no fragmentation of tensor-helper surface. Cost is negligible `Mul+Add+Mul+Sub` at graph entrypoint. Opset 18 forced by torch 2.11 dynamo; registry schema permits 7–21, so we record the emitted opset rather than fail the export. Unconditional registration gives a clean `-ENOSYS` on DNN-off builds. 8-bit-only / BT.709 limited-range are not ADR-level decisions — they match the LPIPS reference contract (ImageNet sRGB); 10-bit / BT.2020 would need a new checkpoint, out of Wave 1 scope.

## Consequences

- **Positive**: first externally-validated FR baseline; every ImageNet-family sibling model reuses `vmaf_tensor_from_rgb_imagenet()` unchanged; `--feature=lpips` has a clean error on DNN-off builds.
- **Negative**: opset drift (18, not 17) locks newer ONNX Runtime; 8-bit/BT.709 only for now.
- **Neutral / follow-ups**: ADR-0042 requires `docs/ai/models/lpips_sq.md` in the same PR.

## References

- Source: `req` (user popup: "Install deps + ship real ONNX")
- Related ADRs: ADR-0036, ADR-0040, ADR-0042
