- **docs**: Research-0085 (vendor-neutral VVC encode landscape) flipped
  from `Status: SKELETON` to `Status: Active`. Re-ran every open
  question against primary sources: NVIDIA Video Codec SDK 13.0 docs,
  AMD AMF SDK GitHub (latest v1.5.0, 2025-10-29), Intel oneVPL GitHub
  (`mfxstructures.h` + `CHANGELOG.md` 2.16.0), Khronos registry,
  Phoronix coverage of Mesa 25.2 RADV AV1 encode, Fraunhofer HHI VVenC
  issue tracker, ZLUDA repository. `[UNVERIFIED]` tag count in the
  digest dropped from 25 to 10 — remaining items are legitimate gaps
  requiring benchmarks (NN-VC quality lift, vvenc per-kernel CPU-time
  distribution) or proprietary roadmap access (HHI's GPU-port plans).
  ADR-0315 `## Context` and `## Alternatives considered` refreshed
  with the verified data points; ADR status stays `Proposed`.
