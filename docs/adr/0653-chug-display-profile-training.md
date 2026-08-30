<!-- markdownlint-disable MD013 MD060 -->
# ADR-0653: CHUG Display Profile Training

- **Status**: Proposed
- **Date**: 2026-05-20
- **Deciders**: Lusoris, Codex
- **Tags**: ai, hdr, chug, mos, training

## Context

CHUG gives the fork a real HDR subjective-MOS corpus before Netflix ships an
HDR VMAF model, but the MOS labels are not enough by themselves to make the
result panel-aware. The signal-mix audit found that HDR display and panel
context is still a blind spot: peak luminance, black level, color volume,
ambient light, local dimming, panel class, and tone-mapping all change the
meaning of HDR quality decisions.

The completed CHUG feature rows are clip and ladder rows. They do not carry a
target consumer display profile, and we should not mutate corpus JSONL rows to
encode one operator's local panel. At the same time, local HDR training needs a
stable way to bind a checkpoint and manifest to the viewing context used for
the experiment.

## Decision

Add a CHUG-local `chug-hdr-display-v1` feature schema and a
`--display-profile-json` flag for `ai/scripts/train_chug_hdr_mos_head.py`.

The schema appends target-display features to `chug-hdr-wide-v1`: normalized
peak luminance, black level, log contrast ratio, ambient lux, BT.2020/P3
coverage, OLED/QLED/LCD panel flags, local dimming, and dynamic tone-mapping.
When `--display-profile-json` is provided and `--feature-schema` is omitted,
the CHUG wrapper selects `chug-hdr-display-v1`; without a profile it keeps the
existing `chug-hdr-wide-v1` default. The profile is normalized by the shared
trainer and written into the emitted manifest with a sha256 of the source JSON.
If a future row already carries display columns, row-local values win and the
profile fills only missing display features.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep CHUG MOS display-invariant | No new schema or operator input | Leaves a known HDR blind spot open; panel differences cannot be studied | Rejected because HDR guidance without panel context is misleading |
| Add profile fields to CHUG corpus JSONL | Simple trainer projection; rows are self-contained | Treats one local target panel as source data and forces corpus regeneration for display-only experiments | Rejected because profiles are operator context, not CHUG source facts |
| Require row-local display data only | Correct for multi-panel datasets | Does not help CHUG, which has no panel rows | Rejected as too slow for the current HDR workstream |
| Add a trainer-side target display profile | Preserves corpus rows, works with CHUG now, and still lets future row-local display data override the profile | Adds one more named schema and manifest surface | Chosen as the smallest reversible implementation |

## Consequences

- **Positive**: CHUG HDR MOS runs can now record and consume target panel
  context instead of pretending subjective HDR MOS is display-invariant.
- **Positive**: Existing CHUG runs remain compatible because no-profile runs
  still default to `chug-hdr-wide-v1`.
- **Positive**: Future multi-display HDR corpora can carry row-local display
  columns without losing their per-row context to a global profile.
- **Negative**: Display-aware CHUG manifests now have a 45-column feature
  order, so experiment tooling must read `feature_order` instead of assuming
  the 34-column wide schema.
- **Neutral / follow-ups**: This is profile plumbing, not proof that the new
  features improve correlation. The next step is to train and compare
  `chug-hdr-display-v1` against the existing wide baseline on held-out CHUG
  rows and future multi-display HDR corpora.

## References

- [ADR-0649](0649-chug-hdr-wide-mos-feature-schema.md)
- ADR-0650 signal-mix audit finding (active PR when this ADR was authored)
- [Research-0653](../research/0653-chug-display-profile-training.md)
- Source: `req` - "the biggest win will be hdr"
- Source: `req` - "CHUG is hdr mos... so thats different because netflix (current) model is 8bit only etc... so its our model or the chug model we use for hdr until netflix finally releases their model"
- Source: `req` - "so that means we need panel tuning on tune ai etc.! (backlog)"
