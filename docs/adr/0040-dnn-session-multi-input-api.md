<!-- markdownlint-disable MD013 MD060 -->
# ADR-0040: Extend DNN session API to multi-input/multi-output with named bindings

- **Status**: Accepted
- **Date**: 2026-04-17
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: ai, dnn, cli

## Context

The current `vmaf_dnn_session_run_luma8` + backend `vmaf_ort_infer` path is hard-wired to single-input NCHW `[1,1,H,W]` luma float32. LPIPS-SqueezeNet (Wave 1 track 2.2, FR baseline, ADR-0036b) needs two inputs (`ref`, `dist`) each NCHW `[1,3,H,W]` with per-channel ImageNet normalization. Without an API extension, every future multi-channel/multi-input model (MobileSal §2.3, TransNet V2 §2.4, FastDVDnet) would require a new back-channel.

## Decision

Extend `core/include/libvmaf/dnn.h` with `VmafDnnInput { const char *name; const float *data; const int64_t *shape; size_t rank; }`, `VmafDnnOutput { const char *name; float *data; size_t capacity; size_t written; }`, and `vmaf_dnn_session_run(sess, inputs, n_inputs, outputs, n_outputs)`. Tensors are bound by name when `name != NULL` (matches ONNX graph input names), else positional. Add helper `vmaf_tensor_from_rgb_imagenet()` in `tensor_io.c` taking three planar R/G/B uint8 sources + strides and emitting NCHW `[1,3,H,W]` float32 normalised with ImageNet mean/std. Relax `vmaf_dnn_session_open()` open-time shape check; move luma shape enforcement into `vmaf_dnn_session_run_luma8()` (remains backward-compatible). Split into two PRs: PR (A) lands the API + helper + tests with no new extractor; PR (B) lands the LPIPS-SqueezeNet ONNX export, registry entry, and `feature_lpips.c`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Single-input concat `[1,6,H,W]` | Reuses existing API | Bakes opinion into graph; prevents branch sharing; diverges from upstream `richzhang/PerceptualSimilarity` export | Rejected |
| Keep luma-only API, private back-channel for LPIPS | Smallest API diff | Permanently blocks MobileSal, TransNet V2, FastDVDnet — all need multi-channel or multi-input | Rejected |
| Defer API change, ship LPIPS via one-shot hack | Faster LPIPS | Forces second breaking API change weeks later when track 2.3/2.4 lands | Rejected |
| Multi-input named-binding API + 2-PR split (chosen) | Bisectable; handles every Wave 1 sibling model; API extension bounded | One extra PR | Correct |

Rationale note: LPIPS is two backbone forward-passes over `ref`/`dist`. Single-input `[1,6,H,W]` would duplicate the SqueezeNet backbone in the graph and re-implement weight sharing at export time, producing a ~2× larger graph with no runtime benefit. Two-input form lets ORT's optimiser share identical sub-graphs. Cost of API extension is bounded; pays off immediately for MobileSal and TransNet V2. 2-PR split preserves bisectability.

## Consequences

- **Positive**: all Wave 1 multi-input/multi-channel models consume the same API; ImageNet helper shared.
- **Negative**: `vmaf_dnn_session_open()` is now more permissive — luma check moved to run time.
- **Neutral / follow-ups**: PR (B) in ADR-0041 uses this surface verbatim.

## References

- Source: `req` (user popup: "Full RGB, 2-PR split (Recommended)" + "Two-input ONNX (Recommended)")
- Related ADRs: ADR-0036, ADR-0039, ADR-0041
