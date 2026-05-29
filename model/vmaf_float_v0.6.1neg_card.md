# `vmaf_float_v0.6.1neg` — model card (Netflix upstream float negative model)

> **Lineage**: Netflix/vmaf upstream.

## Identity

| Field | Value |
| --- | --- |
| Model id | `vmaf_float_v0.6.1neg` |
| Files | `model/vmaf_float_v0.6.1neg.json` + `vmaf_float_v0.6.1neg.pkl` |
| Architecture | Nu-SVR, float-precision; "neg" (negative-oriented) score variant |
| License | BSD-3-Clause-Plus-Patent (upstream Netflix) |
| Status | Production (float-precision negative-orientation variant) |

## Purpose

Negated-output float variant of `vmaf_float_v0.6.1`. Outputs `100 - VMAF`
for use as a differentiable loss signal in codec optimisation loops.

## Operating point

- **Resolution**: 1080p and below SDR
- **Output**: `100 - VMAF` score (negated)
- All other fields are the same as `vmaf_float_v0.6.1`.

## License + lineage

BSD-3-Clause-Plus-Patent (Netflix). See `vmaf_float_v0.6.1_card.md`.
