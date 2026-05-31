- test(feature): push line / branch coverage on three still-low
  feature files (`integer_motion.h` 58→100 % line via
  `test_integer_motion_edge16_coverage`; `adm_csf_tools.h` 0→75 %
  line via `test_adm_csf_tools_coverage`; `feature_collector.cpp`
  branch 62.5→71.9 % via `test_feature_collector_coverage`). Adds
  three new fast-suite binaries that drive `edge_16` mirror branches,
  the `adm_native_csf` H/V + diagonal CSF curve, and the
  deterministic NULL-guard / duplicate-write / unmount-not-found
  paths in the feature collector. No production code changed. Round
  3 in the coverage-push series (round 1 — PR #344, round 2 — PR
  #433). See ADR-0948.
