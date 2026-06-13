- `vmaf-tune` libvvenc adapter: replaced the fabricated `nnvc_intra`
  toggle (which emitted a `-vvenc-params IntraNN=1` key that has never
  existed in any released VVenC) with a curated 9-knob subset of real
  VVenC 1.14.0 config keys — `PerceptQPA`, `InternalBitDepth`, `Tier`,
  `Tiles`, `MaxParallelFrames`, `RPR`, `SAO`, `ALF`, `CCALF`. Keys
  sourced verbatim from `source/Lib/apputils/VVEncAppCfg.h` at tag
  `v1.14.0` (SHA `9428ea8636ae7f443ecde89999d16b2dfc421524`). Defaults
  preserve the bit-exact Phase A grid baseline. `adapter_version`
  bumped to `"2"` so stale cache entries invalidate. See
  [ADR-0285](docs/adr/0285-vmaf-tune-vvenc-nnvc.md) §"Status update
  2026-05-09".
