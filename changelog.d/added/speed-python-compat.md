- **SpEED Python compat wrappers** (`compat/python-vmaf/core/feature_extractor.py`):
  ports `SpeedChromaFeatureExtractor` and `SpeedTemporalFeatureExtractor` from
  Netflix/vmaf upstream into the compat harness. Both wrap the `speed_chroma` and
  `speed_temporal` C extractors via the vmafexec CLI subprocess. Research-0732
  audit (PR #22) identified these as missing.
- **SpEED QualityRunner wrappers** (`compat/python-vmaf/core/quality_runner.py`):
  ports `SpeedChromaQualityRunner`, `SpeedChromaUQualityRunner`,
  `SpeedChromaVQualityRunner`, and `SpeedTemporalQualityRunner` from Netflix/vmaf
  upstream, providing per-channel and combined quality score runners backed by the
  new extractor wrappers.
- **Smoke tests** (`python/test/feature_extractor_test.py`): `SpeedChromaFeatureExtractorTest`
  and `SpeedTemporalFeatureExtractorTest` verify executor_id, ATOM_FEATURES, the
  vmafexec key mapping, TYPE, and VERSION for both new extractor classes — no C
  binary required.
- **Docs update** (`docs/metrics/speed_qa.md`): added "Python compat wrappers"
  section listing all six new Python classes with usage example.
