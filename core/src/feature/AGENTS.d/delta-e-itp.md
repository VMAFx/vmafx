---
paths:
  - core/src/feature/delta_e_itp.c
invariant: Delta E ITP PQ-only transfer characteristics and color space invariants.
area: ssim-and-metrics
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# Delta E ITP PQ-Only Transfer Characteristics

- **`delta_e_itp` PQ-only invariant** (ADR-1110): ΔE-ITP
  extractor (`delta_e_itp.c`) ships **PQ (ST-2084) transfer
  only**; `init()` rejects any `transfer` other than `pq` with
  `-EINVAL`. PQ matrices/constants are triple-sourced against
  ITU-R BT.2124-0; HLG (Annex 3) and BT.1886/SDR (Conversion 5)
  paths are single-sourced and intentionally deferred. Do **not**
  loosen `transfer` guard to accept `hlg`/`bt1886` without first
  cross-validating those constants against independent source.
  PQ EOTF/EOTF⁻¹ live in `delta_e_itp_math.h`; out-of-gamut LMS/ICtCp
  values are deliberately **not clamped** (BT.2124 Annex 4) — unit
  test's places=4 ITP-triple oracle depends on this.
  `scale_chroma_planes` / `scale_chroma_planes_hbd` helpers are
  copied verbatim from `ciede.c` but are independent copies.
