<!-- GENERATED (`make docs-fragments-write`); do not hand-edit. -->
# Models, prediction, pooling, and Rust: `core/src/` agent invariants

libvmaf.c, meson.build, predict*, model*, read_json_model*, svm*,
percentile.h, bootstrap_names.h, interop/pelorus_interop.c, rust/**,
feature/feature_extractor.*, feature/tad_rust.c, and feature/ciede* upstream-
product parity.

Parent index: [AGENTS.md](AGENTS.md).

Generated index. Match touched paths against `Touching`; read each matching
page first (none -> skip). Text above table binds every file. `Touching`
relative; leading `/` = repo root. New invariant -> `AGENTS.d/` page
([how](../../docs/development/agents-index.md)).

## Topic pages

| Touching | Read first | Invariant |
| --- | --- | --- |
| `feature/ciede.c`, `feature/cuda/integer_ciede/ciede_device.h`, `feature/ciede_ff_math.h`, `/core/test/test_ciede_upstream_products.c` | [ciede-squares-are-products](AGENTS.d/ciede-squares-are-products.md) | ciede.c squares = products (ADR-1467); its chroma and rotation products = float, no double cast (ADR-1476). |
| `libvmaf.c`, `percentile.h`, `bootstrap_names.h` | [pooling-and-bootstrap](AGENTS.d/pooling-and-bootstrap.md) | Pooling accumulators remain O(1) float-exact; bootstrap score names stay centralized. |
| `read_json_model.c`, `read_json_model.h` | [read-json-model](AGENTS.d/read-json-model.md) | Model feature arrays sync capacity before access and free previous names before strdup. |
| `rust/**`, `feature/feature_extractor.cpp`, `feature/feature_extractor.h`, `feature/tad_rust.c`, `libvmaf.c`, `meson.build` | [rust-extractor-framework](AGENTS.d/rust-extractor-framework.md) | One Rust archive, engine library only; registry reaches Rust via shim accessor; twins inherit C descriptor. |
| `predict.c`, `predict.h`, `predict_internal.h`, `model.h`, `model_lifetime.c`, `rust/predict/**`, `rust/shim/rust_predict.c`, `rust/shim/rust_predict.h` | [rust-model-prediction](AGENTS.d/rust-model-prediction.md) | Rust predictor returns C predictor's score bit for bit; change to one changes other in same PR. |
| `predict.c`, `interop/pelorus_interop.c` | [scoring-and-pelorus-interop](AGENTS.d/scoring-and-pelorus-interop.md) | Piecewise linear mapping rejects non-finite scores; Pelorus interop validates framing and QP bounds. |
| `svm.cpp`, `svm.h` | [vendored-libsvm](AGENTS.d/vendored-libsvm.md) | Vendored libsvm preserves thread-locale isolation, JSON in-memory parser, malloc checks, and RAII Solver. |
