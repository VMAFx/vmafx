# Research 0649: CHUG HDR Wide MOS Feature Schema

## Question

Should the CHUG HDR MOS trainer keep the existing 11-column KonViD MOS-head
feature layout, or should it consume the richer CHUG feature shards?

## Findings

- Completed CHUG shards contain 5136 reference-aligned rows with canonical-6
  means, p10, p90, and standard deviation columns for every canonical feature.
- The same rows also carry HDR/ladder metadata that is relevant to subjective
  MOS: bitrate ladder, orientation, distorted and reference geometry, duration,
  frame count, and bit depth.
- The first 11-column CHUG probes plateaued below the production gate:
  PLCC/SROCC improved from `0.7961` / `0.7822` at 30 epochs to only
  `0.8276` / `0.8140` at 300 epochs.
- Replacing the global `FEATURE_COLUMNS` constant would break
  `konvid_mos_head_v1` compatibility, because the committed ONNX expects the
  existing 11-column order.

## Result

The lowest-risk next experiment is a named CHUG-local schema:
`chug-hdr-wide-v1`. It widens only CHUG local training while leaving
`konvid-v1` untouched for the committed KonViD MOS model.

## Verification

Regression coverage pins the schema order, CHUG metadata projection, the CHUG
wrapper default, and the exported manifest's `feature_schema` / `feature_order`
fields.
