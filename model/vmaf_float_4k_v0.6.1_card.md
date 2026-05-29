# `vmaf_float_4k_v0.6.1` — model card (Netflix upstream float 4K model)

> **Lineage**: Netflix/vmaf upstream.

## Identity

| Field | Value |
| --- | --- |
| Model id | `vmaf_float_4k_v0.6.1` |
| File | `model/vmaf_float_4k_v0.6.1.json` |
| Architecture | Nu-SVR, float-precision, 4K-calibrated |
| License | BSD-3-Clause-Plus-Patent (upstream Netflix) |
| Status | Production; float-precision 4K variant |

## Purpose

Float-precision feature extraction variant of `vmaf_4k_v0.6.1`, for
use-cases requiring floating-point numerical fidelity at 4K resolution.

## Operating point

- **Resolution**: 4K (3840×2160) — 4K viewing distance calibrated
- **Bit depth**: 8/10 bpc (float features preserve precision)
- **Output**: VMAF score `[0, 100]`
- All other fields match `vmaf_4k_v0.6.1`.

## License + lineage

BSD-3-Clause-Plus-Patent (Netflix). See `vmaf_4k_v0.6.1_card.md`.
