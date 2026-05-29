# `vmaf_4k_v0.6.1neg` — model card (Netflix upstream 4K negative model)

> **Lineage**: Netflix/vmaf upstream.

## Identity

| Field | Value |
| --- | --- |
| Model id | `vmaf_4k_v0.6.1neg` |
| File | `model/vmaf_4k_v0.6.1neg.json` |
| Architecture | Nu-SVR, 4K-calibrated; "neg" (negative-oriented) score variant |
| License | BSD-3-Clause-Plus-Patent (upstream Netflix) |
| Status | Production (negative-orientation variant for codec optimisation use) |

## Purpose

The "neg" (negative) variant outputs `100 - VMAF`, making it suitable for
direct use as a loss function in codec rate-distortion optimisation where
lower values are better. The underlying model is identical to
`vmaf_4k_v0.6.1`.

## Operating point

- **Resolution**: 4K (3840×2160) at 4K viewing distance
- **Output**: `100 - VMAF` score (negated)
- All other fields are the same as `vmaf_4k_v0.6.1`.

## License + lineage

BSD-3-Clause-Plus-Patent (Netflix). See `vmaf_4k_v0.6.1_card.md`.
