- The K150K/CHUG feature extractor (`ai/scripts/extract_k150k_features.py`)
  now passes HDR-aware and HFR-aware per-feature options to libvmaf
  ([ADR-0446](../docs/adr/0446-extractor-hdr-and-hfr-feature-options.md)),
  closing [Issue #837](https://github.com/VMAFx/vmafx/issues/837) +
  the parallel CAMBI HDR-EOTF gap surfaced by lawrence's review of the
  in-flight CHUG extraction. Per-clip ffprobe now surfaces
  `color_primaries`/`color_transfer`/`color_space`; HDR sources
  auto-emit `--feature cambi=eotf=pq:full_ref=true` and
  `--feature float_ms_ssim=enable_db=false`; HFR sources auto-emit
  `--feature motion[_v2]=motion_fps_weight=<30/fps>`. The grammar is
  per `core/tools/cli_parse.c:407`: `<extractor>=key=val:key=val`,
  not the `name=<extractor>:k=v` shape an earlier draft used (which
  trips `problem loading feature extractor: name`).
  Output parquet gains `fps`, `is_hdr`, `motion_fps_weight` columns
  for per-clip stratification. The in-flight CHUG run (started
  2026-05-15 11:00 local) needs the HDR + HFR subset re-extracted
  once this lands (~5 h on the same GPU); SDR + 24-30 fps clips are
  unaffected.
