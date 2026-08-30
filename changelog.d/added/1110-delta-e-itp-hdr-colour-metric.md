- **`delta_e_itp` feature extractor** (`core/src/feature/delta_e_itp.c`,
  `delta_e_itp_math.h`): a CPU full-reference HDR/WCG colour-difference metric
  implementing ITU-R BT.2124-0 (ΔE-ITP). Converts reference and distorted YUV
  frames to the scaled ICtCp ("ITP") colour space and reports the ×720
  Euclidean distance (≈1 per just-noticeable colour difference), pooled as the
  mean per-pixel ΔE-ITP. Fills the fork's HDR colour-fidelity gap (`ciede` is
  SDR/BT.709). RC scope: **PQ (SMPTE ST-2084) transfer only** — HLG and
  BT.1886/SDR are deferred follow-ups (their constants are single-sourced in
  BT.2124-0); `transfer=hlg`/`bt1886` are rejected with a clear error. Options
  `transfer` (default `pq`), `matrix` (`bt2020`/`bt709`, default `bt2020`),
  `range` (`limited`/`full`, default `limited`). YUV400 input is rejected;
  all per-pixel math is double precision with no out-of-gamut clamping
  (BT.2124 Annex 4). Provided feature key: `delta_e_itp`. ADR-1110.
- **`core/test/test_delta_e_itp.c`**: seven tests including the BT.2124-0
  Annex 4 normative ITP-triple oracle (places=4), identity (exactly 0), a
  synthetic ΔE-ITP pair, the documented 2.363 cross-check, PQ-transfer
  round-trip, an end-to-end registry/extract path, and the PQ-only scope
  guard.
- **`docs/metrics/delta_e_itp.md`**: user-facing documentation (what it
  measures, PQ-only scope warning, options, CLI usage, references to ITU-R
  BT.2124-0 / BT.2100).
