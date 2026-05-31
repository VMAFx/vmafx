<!-- markdownlint-disable MD013 MD060 -->
# ADR-0546: Audit bundle — Vulkan motion dispatch wiring, saliency hard-fail, model-card placeholder

- **Status**: Accepted
- **Date**: 2026-05-18
- **Deciders**: lusoris, Claude (Anthropic)
- **Tags**: `vulkan`, `vmaf-tune`, `ai`, `build`, `docs`

## Context

Three independent audit findings were grouped into a single PR because they
touch entirely different files and have no coupling, yet each is small enough
that a dedicated PR per finding would add more review overhead than value.

**Vulkan-01 — `integer_motion_vulkan` extractor missing from dispatch list.**
`core/src/feature/vulkan/integer_motion_vulkan.c` exports the symbol
`vmaf_fex_integer_motion_vulkan_impl` and its source comment states that
`feature_extractor.c` "registers both" this extractor and the legacy
`motion_vulkan` one.  The `extern` declaration (line 99) existed in
`feature_extractor.c`, but no corresponding entry was ever added to
`feature_extractor_list[]`.  Consequence: `vmaf_get_feature_extractor_by_name("integer_motion_vulkan")`
returned NULL on every Vulkan-enabled build, making the extractor unreachable
from user code.

**saliency-tune-01 — saliency ROI silently falls back for unsupported codecs.**
`saliency_aware_encode()` in `vmaftune/saliency.py` dispatched only the five
codecs wired in `_SALIENCY_DISPATCH` (libx264, libaom-av1, libx265, libsvtav1,
libvvenc).  For every other encoder (NVENC, QSV, AMF, VideoToolbox, libvpx-vp9,
and nine others) it emitted only a WARNING log and produced a plain encode.  A
caller requesting `--saliency` combined with `h264_nvenc` would receive an
unbiased encode with no indication that the ROI pass was skipped — a
half-measure inconsistent with the ADR-0498 hard-fail posture adopted for other
requested-but-unavailable features in this fork.

**ai-01 — synthetic-stub model card emits literal `PLACEHOLDER`.**
`predictor_train.py:_write_model_card()` emitted the string
`"PLACEHOLDER — the synthetic stub ships unsigned"` for the Sigstore signing
note when a synthetic corpus was used.  The word `PLACEHOLDER` is a development
artifact; it signals to readers that the field was never filled in rather than
communicating the actual design intent (synthetic stubs are intentionally
unsigned; production models are signed at the release-please tag step).

## Decision

**Vulkan-01**: Add `&vmaf_fex_integer_motion_vulkan_impl` to the Vulkan block
of `feature_extractor_list[]` in `feature_extractor.c`, guarded by the
existing `#if HAVE_VULKAN`.

**saliency-tune-01**: Change the default behaviour when `--saliency` is
requested for a codec not in `_SALIENCY_DISPATCH`:

- Default: raise `SaliencyUnsupportedEncoderError` (inherits from
  `SystemExit(2)`), which exits the CLI with code 2 and a structured error
  message listing the supported codecs and the requested encoder.
- Opt-in fallback: if `SaliencyConfig.allow_unsupported_encoder_fallback=True`
  (set via `--saliency-fallback-plain`) or the environment variable
  `VMAFTUNE_SALIENCY_FALLBACK_OK=1` is set, demote to a plain encode and emit
  an ERROR log (instead of the former WARNING).

**ai-01**: Replace the literal `PLACEHOLDER` with the factual statement
`"not applicable (synthetic-stub model card; production models are signed via
Sigstore — see docs/development/release.md)"`.  Add `--emit-stub-card-only`
to `predictor_train.py` to expose a smoke-test hook that emits a stub card
without a full training run.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Vulkan-01 alt**: remove the dead `extern` declaration and the comment that references it | Reduces confusion about the number of registered extractors | Silently drops an extractor that the source already implements and any downstream user may name explicitly | Symbol already exists and is valid — registering it is the correct fix |
| **saliency-tune-01 alt A**: keep the WARNING + fallback as the default, add a hard-fail flag (`--saliency-strict`) | Preserves backward compatibility for callers that pass unsupported encoders without noticing | Inverts the ADR-0498 posture; the default remains a half-measure | The ADR-0498 rule is "hard-fail by default, opt-in to lenient"; this option violates it |
| **saliency-tune-01 alt B**: hard-fail unconditionally with no escape hatch | Cleaner API surface | Breaks CI pipelines or wrapper scripts that legitimately want to run all-codecs sweeps where some codecs lack ROI support | The env-var escape hatch costs ~5 LOC and preserves the principle without trapping scripts |
| **ai-01 alt**: delete the signing note for synthetic stubs entirely | Shorter card | Readers have no indication of the fork's signing policy for production models | The note is useful orientation; replacing it with a factual statement is preferable to removing it |

## Consequences

- **Positive**: `vmaf_get_feature_extractor_by_name("integer_motion_vulkan")` now returns
  the correct extractor on Vulkan builds; the dispatch table has no silent holes.
  Callers requesting `--saliency` on unsupported codecs receive an actionable error
  (exit 2, named codec, named supported list) instead of a silent degraded encode.
  Synthetic model cards no longer contain stale development artifact text.
- **Negative**: Any caller that previously relied on the silent fallback behaviour for
  unsupported saliency codecs will now receive exit code 2. They must add
  `--saliency-fallback-plain` to restore the old behaviour.
- **Neutral / follow-ups**: The pre-existing `test_saliency_aware_encode_unknown_encoder_falls_back_to_plain`
  test was updated to opt in to `allow_unsupported_encoder_fallback=True` to preserve
  its original intent (verify the fallback path still works when explicitly requested).
  Three new tests cover the hard-fail path and the env-var escape hatch.

## References

- `req`: "When `--saliency` is requested on a codec NOT in `_SALIENCY_DISPATCH`, exit code 2 with a structured error message listing the supported codecs and the requested codec, instead of silently logging a WARNING"
- `req`: "Replace with: 'not applicable (synthetic-stub model card; production models are signed via Sigstore…)'. The literal PLACEHOLDER is misleading"
- `req`: "Add the entry to `feature_extractor_list[]` guarded by the same `#ifdef HAVE_VULKAN` / `enable_vulkan` mechanism as the other Vulkan entries"
- ADR-0498: enforcement posture for requested-but-unavailable features.
- ADR-0293: saliency-aware encode design.
- ADR-0370: x265 / SVT-AV1 / VVenC ROI dispatch.
- ADR-0042: tiny-AI docs 5-point bar.
- ADR-0108: deep-dive deliverables rule.
- ADR-0165: `docs/state.md` bug-tracking rule.
