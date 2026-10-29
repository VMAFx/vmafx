<!-- markdownlint-disable MD013 MD060 -->
# Research-3388: where VMAFx listed its encoders, and how far the lists drifted

- **Status**: Active
- **Workstream**: [ADR-3388](../adr/3388-central-encoder-registry.md)
- **Last updated**: 2026-10-29

Before [ADR-3388](../adr/3388-central-encoder-registry.md) the same encoder
facts were written by hand in four places. This digest records what each list
held at `51e6facdd` (master on 2026-10-29), the drift the registry exposed, and
how the replacement was checked. The user page is
[encoder-registry.md](../usage/encoder-registry.md).

## 1. The four lists

| List | File | Held |
|---|---|---|
| Python adapters | `tools/vmaf-tune/src/vmaftune/codec_adapters/` (`_REGISTRY`) | 19 adapters: knob, window, default, presets, version, probe settings, capability flags |
| Go tuner table | `pkg/encoder/adapter.go` (`adapterRegistry`) | the same 19 ids with windows, defaults, preset vocabularies |
| Go adapter port | `pkg/codecadapter/codecadapter.go` | the same facts again, per constructor |
| Bisect window | `tools/vmaf-tune/src/vmaftune/bisect.py` (`_ABSOLUTE_CRF_RANGE_BY_NAME`) | absolute CRF range for 5 software encoders |

## 2. Drift found

- `AdapterVersion` in `pkg/codecadapter` was empty for `h264_nvenc`,
  `hevc_nvenc`, `av1_nvenc`, `h264_amf`, `hevc_amf`, `av1_amf`, `libaom-av1`
  and `libsvtav1`, while the Python adapters carry `"1"` (NVENC, libaom,
  SVT-AV1) and `"2"` (AMF). `pkg/codecadapter/adapter_version_test.go` pinned
  only the eleven ids that agreed. Once the Go adapters read the registry, the
  test failed on exactly these eight; the pin now lists all nineteen.
- The bisect table agreed with the adapters for its five rows. For the
  fourteen other encoders the bisect fell back to the adapter's quality window;
  the registry's `absolute_range` for those rows equals their window, so the
  bisect window is unchanged for every encoder
  (`tests/test_encoder_registry.py::test_bisect_window_is_registry_absolute_range`).

## 3. Checks run

| Check | Command | Result |
|---|---|---|
| Generated outputs current | `python3 scripts/codegen/vmafx-api.py --check` | 90 generated files match |
| Loader rules, each refusing its planted defect | `python3 -m unittest test_vmafx_api_encoders` (in `scripts/codegen/tests`) | 16 tests OK |
| Python adapters equal rows | `pytest tools/vmaf-tune/tests` (fingerprint extra installed) | 2309 passed, 33 skipped (optional extras), 0 failed |
| Go consumers | `go test ./pkg/encoder ./pkg/encoderregistry ./pkg/codecadapter` | ok |
| Mutation, Python copy | `absolute_range` of `libx264` set to `[0, 50]`, version `"99"` | `test_adapter_matches_registry_row[libx264]`, `test_python_copy_matches_go_copy`, `test_absolute_crf_range_table_lookup` fail |
| Mutation, Go copy | `quality_range` of `libx264` set to `[1, 51]`, version `"99"` | `TestAdapter_QualityWindows`, `TestAdapterVersionMatchesPython`, `TestValidate`, `TestAdapterMetadata_matchesPythonGolden`, `TestRegistryMatchesPythonMetadata` fail |

## 4. Not in this slice

The coverage matrix (gap rows for every family with licence class, upstream
state, paradigm, use-case tags and distortion-source artifact domains), the
tuning-profile and fork variant rows, and the per-use-case ranking land as
later slices on the same format. Their research is recorded with those
slices, with the upstream state of each row checked live on its `checked`
date.
