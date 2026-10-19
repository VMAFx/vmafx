## The provenance record moves into the library (2026-10-06)

`rc4/api-wp5-provenance` (RC4 work package 5, ADR-2073), on top of
`rc4/api-wp8-options`. Upstream-mirror files touched:

- `core/src/feature/feature_collector.{h,cpp}`: `FeatureVector` gains
  `producer`, `producer_options` and `source`, recorded from the thread's
  producer (`vmaf_feature_producer_swap()`) when a vector is created. An
  upstream change to vector creation keeps the call to
  `feature_vector_record_producer()`.
- `core/src/feature/feature_extractor.cpp`: `ProducerScope` around `extract`,
  `collect` and `flush`; `core/src/libvmaf.c` installs the producer around the
  direct `fex->flush()` loop and appends imported and tiny-model scores with
  `vmaf_feature_collector_append_from()`; `core/src/predict.c` appends model
  scores the same way. A new direct call of an extractor's callbacks needs the
  same scope, or its features have no producer.
- `core/src/model.{h,c}` / `model_lifetime.c`: `VmafModel` gains `source`,
  `sha256`, `load_flags`, `overrides`; every loader ends in
  `vmaf_model_stamp_loaded()` and `vmaf_model_feature_overload()` records the
  override. Keep both on an upstream sync of the loaders.
- `core/src/output.cpp`: the JSON writer ends with `json_write_provenance()`
  (`score_format`, the record, the backend receipt) and the XML writer with
  `xml_write_provenance()`. `aggregate_metrics` no longer ends with a newline
  of its own. Netflix's harness reads neither element.
- `core/tools/vmaf.cpp`: the JSON splice (`amend_json_with_backend_receipt`,
  `amend_cli_backend_receipt`) and `cli_format_backend_members()` are gone;
  do not bring them back on a rebase that touches the output path.
- `core/src/libvmaf.c`: `VmafContext.run` (atomics: frames, size, format,
  first / flush times) is what a provenance query reads, from any thread;
  `run_note_frame()` follows each `vmaf->pic_cnt++` of the submit paths and
  the flush paths store `run.flush_ns`. A rebase that adds a submit path (RC4
  WP4's asynchronous windows) calls `run_note_frame()` where it counts a frame;
  never let `vmaf_engine_run_info()` read `pic_cnt` or `pic_params`.
- `core/src/meson.build`: `vmafx_build_info.h` (configure_file) and
  `vmafx_build_commit.h` (vcs_tag) feed `provenance_build.c`; keep them next
  to the library target.
- Generated: `core/src/vmafx/exactness_gen.c`
  (`python3 scripts/codegen/vmafx_exactness.py --write`, part of
  `make docs-fragments-write`) follows `scripts/ci/exact_twins.d` and the
  parity gate's tables; a PR that adds a fragment regenerates it with the
  exact-twins page (`make docs-fragments-check`,
  `test_vmafx_exactness_table_current`). On a conflict take either side and
  regenerate.
