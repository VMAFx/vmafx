# Research-0109: tiny-AI real-weight limitation cleanup

- **Status**: documentation consistency digest
- **Date**: 2026-05-15
- **Relevant ADRs**: ADR-0215, ADR-0223, ADR-0255, ADR-0261

## Question

Which user-facing tiny-AI docs still describe FastDVDnet or TransNet V2
as placeholder / smoke-only surfaces after their real upstream weights
landed?

## Findings

- `model/tiny/registry.json` and the model cards already describe
  `fastdvdnet_pre` and `transnet_v2` as real-weight entries with
  `smoke: false`.
- `docs/metrics/features.md` and `docs/ai/roadmap.md` still carried the
  older placeholder / real-weights-follow-up wording, which makes the
  user-facing feature matrix contradict the shipped registry state.
- The package `AGENTS.md` notes for DNN / feature code also kept the
  old placeholder framing or stale ADR numbers, which raises rebase
  risk when the tiny-AI extractor contracts are touched.

## Decision Support

| Option | Trade-off | Decision |
| --- | --- | --- |
| Leave the docs stale until the FFmpeg consumers land | Avoids churn, but tells users the shipped real ONNX files are still placeholders. | Rejected |
| Change code / registry to match the stale docs | Would regress already-shipped real-weight model cards and registry entries. | Rejected |
| Refresh user docs and invariant notes only | Correctly separates shipped real-weight extractors from remaining consumer follow-ups. | Accepted |

## Non-Goals

- No model bytes, registry schema, or extractor runtime behaviour change.
- No claim that the pending FFmpeg `vmaf_pre_temporal` or per-shot CRF
  consumers have shipped.
