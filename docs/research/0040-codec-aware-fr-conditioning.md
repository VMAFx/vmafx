# Research-0040: Codec-aware FR conditioning for the tiny FR regressor

- **Status**: Active
- **Workstream**: [ADR-0235](../adr/0235-codec-aware-fr-regressor.md)
- **Last updated**: 2026-05-01

## Question

Does conditioning the FR regressor (`FRRegressor`,
`model/tiny/fr_regressor_v1.onnx`) on the encoder family that
produced the distorted side measurably lift cross-codec PLCC/SROCC
on the fork's available training corpora (Netflix Public, BVI-DVC,
KoNViD-1k), and if so, by enough to justify a `_v2` release?

## Sources

- 2026 Bristol VI-Lab review (Bull, Zhang) — local copy at
  `.workingdir2/preprints202604.0035.v1.pdf`, §5.3 "Codec-conditioned
  quality models". Surveys the Bampis 2018 → Zhang 2021 line plus
  follow-on multi-codec ablations on BVI-HFR and the AOM CTC.
- Bampis et al. 2018, "Spatiotemporal Feature Integration and Model
  Fusion for Full-Reference Video Quality Assessment" (ST-VMAF),
  IEEE TCSVT 28(8). Original VMAF + temporal feature fusion paper;
  the per-codec breakdown in §V.B reports a 0.011–0.024 PLCC delta
  between the codec-blind global model and per-codec sub-models on
  the NFLX dataset.
- Zhang, Bull et al. 2021, "Enhancing VMAF through new feature
  integration and model combination" — explicit per-codec
  conditioning ablation reports +0.018 PLCC / +0.022 SROCC on the
  multi-codec evaluation set vs the codec-blind baseline.
- Prior fork research digests: [Research-0019](0019-tiny-ai-netflix-training.md)
  (corpus prep), [Research-0027](0027-phase2-feature-importance.md)
  (FULL_FEATURES selection), [Research-0030](0030-phase3b-multiseed-validation.md)
  (multi-seed PLCC variance baseline).
- Prior fork PRs / commits: PR #178 (KoNViD acquisition), PR #214
  (BVI-DVC pipeline), e421d700 (`fr_regressor_v1` C1 baseline).

## Findings

The literature consensus is unambiguous — every cited multi-codec
ablation reports a positive PLCC lift from explicit codec
conditioning, in the range +0.011 to +0.030 PLCC on held-out
material that mixes codecs not seen together at training time. The
mechanism is consistent across papers: codec-specific distortion
signatures (x264 block-edges, x265 CTU-boundary blur, AV1 DCT
ringing + restoration filters, VVC large-CTU deblocking) push the
feature distribution into different sub-manifolds; a global
regressor averages across these and under-fits the codec with the
smallest training share.

The fork's training-data picture today:

- Netflix Public corpus (~9 ref + 70 dis YUVs, 37 GB at
  `.workingdir2/netflix/`): pre-encoded distortions with no
  in-band codec metadata. Tagged `"unknown"` in the new `codec`
  column.
- BVI-DVC Part 1 (4-tier 10-bit YCbCr): reference-only material;
  the fork's `bvi_dvc_to_full_features.py` encodes internally with
  libx264 at CRF 35 today. Tagged `"x264"`.
- KoNViD-1k (1200 in-the-wild MP4s): natural distortions of mixed
  codecs; per-clip `ffprobe stream=codec_name` aliases (h264 →
  x264, hevc → x265, av1 → libsvtav1, vp9 → libvpx-vp9) cover the
  bulk. Mixed codec coverage is the headline win.

A 0.005 PLCC lift bar is consistent with the multi-seed variance
floor measured in [Research-0030](0030-phase3b-multiseed-validation.md)
(σ ≈ 0.003 across 5 seeds on the Phase-3b sweep) — a real lift has
to clear ~2σ to register as a non-noise signal. Setting the bar
lower would risk shipping a regression masquerading as noise; the
literature's reported deltas (+0.011 to +0.030) all clear 0.005
comfortably so the bar is not aggressive.

## Alternatives explored

The ADR's `## Alternatives considered` table covers the design
space — one-hot vs per-codec sub-models vs continuous embedding vs
"more data instead". The decision rests on:

- One-hot wins on simplicity + ONNX op-allowlist compatibility
  (no `Gather` op at inference). The 6-dim input penalty is
  rounding error against the 22-dim FULL_FEATURES vector.
- Per-codec sub-models hit a hard wall on the `"unknown"` bucket:
  no sub-model exists, so we'd need the one-hot fallback regardless.
- A learned embedding gives 4–8 dims at the cost of an extra ONNX
  op for negligible accuracy delta at this corpus scale.

## Open questions

- Empirical PLCC/SROCC lift on the fork's specific multi-codec
  split — the present PR ships the plumbing only; the training run
  is blocked until the agent can reach `~/.cache/vmaf-tiny-ai/`.
  Follow-up PR re-runs the trainer + measures + decides whether to
  ship `_v2`.
- Whether the KoNViD `ffprobe` codec-tagging is reliable enough to
  use the corpus as a primary training signal, or whether we keep
  KoNViD as eval-only and train on Netflix + BVI-DVC. The Bristol
  review §5.3 cautions that natural-distortion corpora often
  conflate codec with content — a per-codec content-balance check
  is required before promoting KoNViD to training.
- Whether the next AV1 / VVC encode sweep should use SVT-AV1 or
  libaom-av1 (the vocabulary uses `libsvtav1` as the canonical
  bucket and aliases `av1` to it). VVENC is already the canonical
  VVC encoder.

## Related

- ADRs: [ADR-0235](../adr/0235-codec-aware-fr-regressor.md),
  [ADR-0042](../adr/0042-tinyai-docs-required-per-pr.md),
  [ADR-0168](../adr/0168-tinyai-konvid-baselines.md).
- Research digests: [Research-0019](0019-tiny-ai-netflix-training.md),
  [Research-0027](0027-phase2-feature-importance.md),
  [Research-0030](0030-phase3b-multiseed-validation.md).
- PRs: this PR (codec-id capture + model surface),
  follow-up: T7-CODEC-AWARE training run.
