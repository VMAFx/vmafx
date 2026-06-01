- **#143 model registry audit**: Added missing `license`, `license_url`, and
  `sigstore_bundle` fields to `fr_regressor_v1` entry; registered
  `smoke_multi_output_v0` and `smoke_v0_symbolic_batch` as `smoke: true`
  entries; all 26 registry entries now pass `validate_model_registry.py`.
  Updated `docs/ai/model-registry.md` smoke-fixtures table.
- **#132 Rust crate audit (Research-0760)**: Full audit of TAD extractor and
  `vmafx-sys` crates covering unsafe justification, cbindgen header drift,
  transitive vulnerability scan (RUSTSEC-2022-0027 / lazycell noted),
  Cargo.lock health, ADR-0707 dispatch contract, and `enable_rust_features`
  default state. All PASS except RUSTSEC-2022-0027 (bindgen transitive,
  build-time-only). SKIPPED — branch carried pre-existing conflict markers
  from sweep commit 24bb5daf89.
- **#189 model cards sweep**: 34 new co-located `_card.md` files covering
  all shipped model artefacts under `model/tiny/`, `model/` root, and
  `model/vmaf_rb_*/`. Each card satisfies the ADR-0042 five-point bar.
  SKIPPED — branch carried pre-existing conflict markers from sweep commit
  24bb5daf89.
