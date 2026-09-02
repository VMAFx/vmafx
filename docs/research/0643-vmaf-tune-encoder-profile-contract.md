# Research-0643: vmaf-tune Encoder Profile Reports

- **Date**: 2026-05-20
- **Scope**: `vmaf-tune` report UX, encoder-profile reuse, FFmpeg patch-stack hand-off
- **Related ADR**: [ADR-0643](../adr/0643-vmaf-tune-encoder-profile-contract.md)

## Question

Can the existing `vmaf-tune` report artifact become both the human
explanation layer and the machine-readable input for follow-up encodes?

## Findings

1. The report renderer already owns the complete structured view:
   source metadata, codec comparison rows, multi-target sweep rows,
   ladder samples/rungs, and per-shot rows. Embedding a profile there
   avoids a second sidecar file that can drift from the report a human
   reviewed.

2. HTML reports must stay self-contained. Remote codec logos would make
   archived reports brittle and may introduce licensing/brand-use
   questions. Text identity chips with stable colours and upstream
   project/vendor links provide the useful recognition cue without
   fetching remote assets.

3. FFmpeg should not parse the profile schema. The existing patch stack
   already carries vmaf-tune advisory glue (`-pass-autotune`); extending
   that pattern with `-vmaf-profile <path>` keeps FFmpeg discoverable
   while leaving schema migration and codec-adapter defaults in Python.

4. Selection must be explicit. The profile may contain many codecs,
   targets, failed rows, ladder rungs, and shot rows; the execution tool
   should run one chosen recommendation by default and require operator
   scripting for batch materialisation.

## Implementation Notes

- `ReportData.to_dict()` now embeds `encoder_profile` with schema
  `vmaftune.encoder_profile.v1`.
- `vmaf-tune encode-profile` accepts raw JSON, HTML, and Markdown
  reports, selects one row, and builds the FFmpeg argv through
  `EncodeRequest`.
- Failed rows are preserved in `encoder_profile.failures`; they are not
  eligible recommendations.
- `ffmpeg-patches/0015-vmaf-tune-profile-cli-glue.patch` adds
  `-vmaf-profile <path>` as advisory hand-off only.
