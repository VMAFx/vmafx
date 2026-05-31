<!-- markdownlint-disable MD013 MD060 -->
# ADR-0581: Add `enable_chroma` option to `integer_vif`

- **Status**: Accepted
- **Date**: 2026-05-16
- **Deciders**: lusoris, Claude (Anthropic)
- **Tags**: `feature`, `vif`, `chroma`

## Context

`integer_vif` previously computed VIF on the luma (Y) plane only. PSNR and SSIM
already expose an `enable_chroma` option (default false) that runs the same metric
on Cb and Cr planes and emits per-plane scores. Adding the same option to
`integer_vif` lets callers obtain chroma VIF scores without a separate extractor
and aligns the three metrics' option surface.

The option is forced off for YUV400 (monochrome) input, matching the psnr/ssim
precedent established in ADR-0453.

## Decision

Add `bool enable_chroma` (default `false`) to `VifState`. When `true`, the extractor
runs the existing four-scale VIF pipeline on planes 1 (Cb) and 2 (Cr) in addition to
luma, and emits `integer_vif_scale{0..3}_cb` / `..._cr` keys via
`vmaf_feature_collector_append_with_dict`. For YUV400P input, `enable_chroma` is
clamped to `false` in `init`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Separate `integer_vif_chroma` extractor | Clean separation | Doubles registration boilerplate; no precedent | Inconsistent with psnr/ssim pattern |
| Emit chroma scores unconditionally | Simpler control flow | Breaks existing consumers expecting only luma keys | Default-off preserves backward compatibility |

## Consequences

- **Positive**: callers can request full-plane VIF without a third-party extractor;
  option surface matches psnr and ssim.
- **Negative**: eight new keys in `provided_features`; downstream consumers that
  iterate the feature dict must handle optional keys.
- **Neutral**: luma scores are numerically identical to pre-patch; no golden-data
  change required.

## References

- ADR-0453 (`psnr enable_chroma` precedent)
- req: "Add `enable_chroma` option to `integer_vif` (mirror psnr/ssim pattern)"
