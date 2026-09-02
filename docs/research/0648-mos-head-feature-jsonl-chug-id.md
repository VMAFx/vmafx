# Research 0648: CHUG HDR MOS Trainer Surface

## Question

Can CHUG HDR subjective-MOS training reuse the existing small MOS-head training
loop without exposing CHUG through a KonViD-named command?

## Findings

- CHUG feature rows already carry trainer-ready fields: `mos` mapped to `[1, 5]`,
  `mos_raw_0_100`, canonical feature columns such as `adm2` and `motion2`, and
  content-level `split` labels.
- The existing MOS trainer's loader can consume JSONL rows and preserves explicit
  split labels, but the public CLI had only KonViD-named JSONL inputs.
- The same model architecture is usable for a local CHUG HDR MOS experiment, but
  the model identity must not be `konvid_mos_head_v1`; CHUG is HDR MOS, while
  the current Netflix teacher is SDR/8-bit calibrated.

## Result

The lowest-risk implementation is a CHUG-named wrapper,
`ai/scripts/train_chug_hdr_mos_head.py`, over the shared MOS-head loop. It gives
operators a truthful command and defaults while preserving the committed KonViD
model's existing script and provenance.

## Verification

Regression coverage adds a CHUG-wrapper smoke path that trains from synthetic
CHUG-style feature JSONL, writes a local ONNX/manifest pair, and asserts the
manifest id is `chug_hdr_mos_head_v1`.
