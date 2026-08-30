Add pytest coverage suite for `compat/python-vmaf/core/` wrapper layer

New test directory `compat/python-vmaf/tests/` (175 tests) covers:
`Result` / `BasicResult` (serialisation, aggregation, dataframe, equality),
`FeatureAssembler` (construction, option-dict routing, result assembly),
`QualityRunner` (clip/transform/rectification logic, key helpers, opts-dict merging),
and `FeatureExtractor` (class metadata, ATOM_FEATURES, wildcard discovery).
No binary invocation; all I/O paths mocked.
