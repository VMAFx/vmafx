- test(feature): push line coverage on four low-coverage feature-extractor
  files (`mkdirp.c` 0%->80%, `luminance_tools.c` 78%->93%, `feature_name.c`
  85%->92%, `feature_extractor.c` 73%->77%). New `test_mkdirp` binary
  exercises mkdirp's NULL guard, single-level / nested / idempotent
  (EEXIST) paths — the file had 0/35 line coverage at the 2026-05-30
  baseline because the only in-tree caller is cambi's heatmap sidecar
  which no test drives. Adds focused unit tests to `test_luminance_tools`
  (`init_eotf` dispatch + invalid `VmafPixelRange`),
  `test_feature` (NULL-input guards, STRING-typed options,
  `dict_from_provided_features` empty/non-empty paths), and
  `test_feature_extractor` (NULL/unknown lookups, ADR-0530 CUDA-flag
  fallback, NULL guards on extract/submit/collect/flush/close/destroy
  and pool create/aquire/release/flush/destroy). No production code
  changed. Helps preserve the ADR-0114 Coverage Gate floor at 37%.
