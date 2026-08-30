- Fixed `VmafFeatureExtractor.VERSION` stuck at `0.2.7` instead of `0.2.21`,
  causing `VmafexecQualityRunner.VERSION` to report `F0.2.7-0.6.1` instead of
  the correct `F0.2.21-0.6.1`. The fork's reformatting pass preserved the old
  literal while the upstream advanced through fourteen intermediate versions
  (0.2.8 → 0.2.21). Restores the missing version-history comments and bumps
  the active constant to `"0.2.21"`, matching Netflix upstream commit
  `3dee9666`. The `QualityRunnerVersionTest::test_vmafexec_quality_runner_version`
  assertion was always correct; only the code was misaligned.
