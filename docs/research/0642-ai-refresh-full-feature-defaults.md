<!-- markdownlint-disable MD013 -->
# Research: AI full-feature refresh defaults

Companion to [ADR-0642](../adr/0642-ai-refresh-full-feature-defaults.md).

## Findings

The refresh audit found three stale-data risks:

- Several scripts defaulted to `build/tools/vmaf`, `/usr/local/bin/vmaf`, or PATH lookup.
  Those locations can point at an older binary without fork-only extractors such as
  `motion_v2`, SSIMULACRA2, and the SpEED features.
- `extract_ugc_features.py` preserved an old v5 shortcut that emitted canonical-6 rows and
  filled the rest of `FULL_FEATURES` with NaN. That shape is incompatible with current
  full-feature FR refreshes.
- KoNViD-1k had a synthetic-FR pair extractor but not a full-feature table generator with
  deterministic fold metadata.

Local smoke checks used the current fork CPU binary at `core/build-cpu/tools/vmaf`.
BVI-DVC raw-YUV input produced all-zero VMAF in a one-clip probe, while the local lossless
MKV bundle produced finite metrics; the refresh driver therefore accepts `.mkv`, `.mp4`,
and `.yuv` but documents the MKV bundle as the known-good source.

## Verification

Focused tests:

```bash
.venv/bin/python -m pytest \
  ai/tests/test_feature_extractor_defaults.py \
  ai/tests/test_bvi_dvc_dir_mode.py \
  ai/tests/test_extract_k150k_features.py \
  ai/tests/test_e2e_frame_to_score.py \
  ai/tests/test_extract_ugc_features.py \
  ai/tests/test_konvid_full_features.py \
  ai/tests/test_combine_full_feature_parquets.py \
  ai/tests/test_feature_sets.py \
  -q
```

Real local smokes completed for:

- one KoNViD-1k full-feature clip (`200` rows, current `FULL_FEATURES` + `vmaf`);
- one UGC distorted pair (`3` frames, current `FULL_FEATURES` + `vmaf`);
- BVI-DVC tier-D lossless MKV refresh (`193` clips, `6176` frames).

Long-running local background jobs continue for Netflix, KoNViD, UGC, and CHUG; their
outputs are gitignored corpus artifacts and not part of this PR.

## References

- [ADR-0026](../adr/0026-cross-metric-feature-fusion.md)
- [ADR-0340](../adr/0340-multi-corpus-aggregation.md)
- [ADR-0362](../adr/0362-k150k-corpus-integration.md)
