---
paths:
  - core/src/vmafx/provenance*
  - core/src/vmafx/report*
  - core/src/vmafx/exactness*
  - core/src/vmafx/build_commit.h.in
  - core/src/feature/feature_collector.cpp
  - core/src/feature/feature_collector.h
  - core/src/feature/feature_extractor.cpp
  - core/src/output.cpp
  - core/src/model.c
  - core/src/model.h
  - core/src/predict.c
  - scripts/codegen/vmafx_exactness.py
  - core/tools/test/test_vmaf_provenance_report.py
  - core/tools/test/test_vmaf_verify_provenance.py
invariant: Record JSON = proto JSON mapping in RFC 8785 form; digest skips digest + elapsed_ns only; field tables match vmafx.toml.
---
<!-- markdownlint-disable MD013 -->
# Provenance record and reports (RC4 WP5, ADR-2073, #2142)

## Record

- Data read at query time, never cached across queries: build (`provenance_build.c`, `vmafx_build_info.h` + `vmafx_build_commit.h` from `core/src/meson.build`), engine run (`vmaf_engine_run_info()`), device (`vmafx_provenance_device()`), collector (models list, feature vectors, producers), state (annotations, encode record).
- Queries hold `context->provenance.lock`, then collector `fc->lock` (snapshot). Never reverse order. Record strings point into context (collector, models, state): context lifetime.
- `build_id` = digest of compiler, build_flags, fp_policy, backends, rust_twins. Commit + arch excluded on purpose (own fields). `test_build_id_follows_the_build` plants each part.
- CPU device: name `cpu`, runtime = `VMAFX_BUILD_ARCH`. Device backends: runtime `unknown` until WP3 lanes fill `vmafx_provenance_device()` (requests/WP3-1).
- `implementation` = `rust` iff extractor name ends `_rust`; replace with framework flag when #2086 lands.

## Canonical form and digests (do not change without an ADR superseding ADR-2073)

- JSON = proto JSON mapping of `Provenance` (`proto/vmafx_api.proto`): field names as keys, u64/i64 as decimal strings, enums by lower-case value name, `models` / `features` / `annotations` arrays (`proto_repeated`, numbers from 100).
- RFC 8785 subset: keys byte order, no whitespace, plain integers, escape only `"` `\` control chars (`\b\t\n\f\r`, else `\u00xx` lower case), invalid UTF-8 -> U+FFFD. Equals Python `json.dumps(sort_keys=True, separators=(",", ":"), ensure_ascii=False)`; `test_vmaf_provenance_report.py` recomputes digest that way.
- `digest` covers everything but `digest` and `elapsed_ns`. `scores_digest` (inside digest) = lines `<report name> <index> <%016x bits>\n`, names byte order, index order. #2159 signs `digest`.
- Field tables in `provenance_render.c` duplicate definition's field lists (no struct-JSON emitter yet, requests/WP1-7). New struct field -> table entry; `test_vmaf_provenance_report.py` fails on missing or extra key, server's strict protojson fails on unknown one.

## Producers

- `FeatureVector.producer/producer_options/source` copied from thread-local `VmafFeatureProducer` at vector creation. Installed by `ProducerScope` (extract, collect, flush in `feature_extractor.cpp`), direct flush loop in `libvmaf.c`, `vmaf_feature_collector_append_from()` (models in `predict.c`, imports, tiny model). New direct call of extractor callbacks needs same, else `source` = unknown and `test_features_in_name_order` fails.
- `vmaf_engine_feature_producer()` reads collector first, `provided_features` only for vectors not written yet.

## Reports and verification

- `output.cpp` asks `vmafx_report_json_members()` / `vmafx_report_xml_element()` (`report_fragments.h`). CSV / SUB bytes unchanged; sidecar only with `VMAFX_REPORT_PROVENANCE_SIDECAR`.
- Backend receipt (`backend_used`, `feature_backends`) = `vmafx_backend_receipt()`; CLI splice is gone, do not reintroduce.
- `report_verify.c`: flatten to `path -> text`; rebuild canonical text of `provenance` from tokens (keys must be sorted, numbers plain integers) -> recomputed digest. Compare skips `environment()` list only; adding field there weakens verification, needs ADR. Order: compare, then self-check each report.
- Exactness table `exactness_gen.c` generated from `scripts/ci/exact_twins.d` + gate tables; part of `make docs-fragments-write` / `-check`. Alias feature with backend set different from its base: generator refuses (base-row lookup would lie).
