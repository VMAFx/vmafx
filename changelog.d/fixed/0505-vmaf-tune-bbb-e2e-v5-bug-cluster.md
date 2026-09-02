- `vmaf-tune ladder` against a container source (`.mp4` / `.mkv`
  / `.y4m`) now produces plausible VMAF on the rendition cloud.
  Previously the encode driver passed `-f rawvideo -pix_fmt
  yuv420p -s WxH -i src.mp4` against every input regardless of
  container shape, so ffmpeg re-interpreted the container's
  compressed bytes as planar YUV pixels and emitted a uniformly
  bogus ~50 Mbps encode with VMAF in the 4-9 band irrespective
  of CRF. `corpus.iter_rows` now sets
  `EncodeRequest.source_is_container=True` whenever the source
  suffix is outside the raw-YUV set, so ffmpeg auto-detects the
  container format and the rung-target scale filter handles the
  resolution change (ADR-0505, Bug #V5-2).
- `vmaf-tune ladder --format json` now emits the *full* per-CRF
  sweep cloud in `samples[]` instead of one row per
  `(resolution, target_vmaf)` cell. The historic emit path
  dropped every non-winning CRF and double-listed any rendition
  whose target-VMAFs converged on the same CRF; a new
  `cloud_sink` kwarg on `make_default_sampler` captures every
  scored row before the per-target collapse and a new
  `_dedup_samples` helper drops `(width, height, crf)` repeats
  before emit (ADR-0505, Bug #V5-2 + #V5-3).
- Hardened the V4-A regression test for `vmaf --backend vulkan`
  strict-mode refusal. The V4 gate skipped silently on every
  developer host without the binary on `$PATH`; the V5
  replacement also probes `$VMAF_BIN_FOR_TESTS` and the
  canonical `build/tools/vmaf` meson output so the assertion
  fires whenever a built binary is reachable (ADR-0505,
  Bug #V5-1 regression guard).
