# Research-0100: `vmaf-tune` / Tiny-AI Scaffold-State Audit

- **Date**: 2026-05-14
- **Scope**: `tools/vmaf-tune/`, `model/tiny/`, `docs/state.md`
- **Trigger**: user request to continue unblocked AI and `vmaf-tune`
  scaffold cleanup before the expected upstream HDR-model release.

## Findings

1. `tools/vmaf-tune/src/vmaftune/auto.py` still emitted a single
   hard-coded PQ `hdr_args` tuple for every HDR cell. That bypassed
   `vmaftune.hdr.hdr_codec_args()`, so x265/SVT-AV1/NVENC/VVenC
   cells could not surface their codec-specific HDR signalling.
2. `auto.py` computed recipe-adjusted `effective_thresholds` and then
   re-created `thresholds = confidence_thresholds or
   ConfidenceThresholds()` before the cell loop. The JSON metadata
   and confidence decisions therefore ignored the recipe-tightened
   F.3 gate.
3. `model/tiny/registry.json` still marked
   `fr_regressor_v2_ensemble_v1_seed{0..4}` as `smoke: true`.
   ADR-0321, `ai/AGENTS.md`, the model card, PROMOTE.json, the
   per-seed sidecars, and the on-disk SHA-256s all point to the
   production full-corpus flip.
4. `docs/state.md` still listed T-HDR-ITER-ROWS and Tiny-AI C1 in
   Deferred even though both have Recently closed rows and landed
   implementation evidence.
5. Fetched `upstream/master` still has no `model/vmaf_hdr_*.json`
   entry as of this audit. `origin/master` only carries the local
   `model/vmaf_hdr_model_card.md` fallback.

## Action

- Wire `auto` HDR cells through `hdr_codec_args(codec, info)`.
- Preserve recipe-adjusted F.3 thresholds through the cell loop and
  emitted metadata.
- Flip the five ensemble seed registry rows to `smoke: false`, backed
  by their existing sidecars and PROMOTE gate evidence.
- Remove stale Deferred rows from `docs/state.md`.

## No New ADR

No new architectural decision is introduced. These changes implement
already-accepted contracts in ADR-0300, ADR-0321, and ADR-0325 and
repair stale scaffold leftovers.
