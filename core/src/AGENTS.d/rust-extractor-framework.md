---
paths:
  - core/src/rust/**
  - core/src/feature/feature_extractor.cpp
  - core/src/feature/feature_extractor.h
  - core/src/feature/tad_rust.c
  - core/src/libvmaf.c
  - core/src/meson.build
invariant: One Rust archive, engine library only; registry reaches Rust via shim accessor; twins inherit C descriptor.
---
<!-- markdownlint-disable MD013 -->
# Rust extractor framework (ADR-1713)

- **Separate, dependency-free workspace.** `core/src/rust/Cargo.toml` = own
  workspace (root workspace `exclude`s it and TAD crate, which names it via
  `package.workspace`). Its `Cargo.lock` holds no external crate; adding one
  breaks Meson's `cargo build --offline --locked` and empty-CARGO_HOME step of
  `rust-ci.yml`, on purpose. Never move these crates back into root workspace:
  cargo resolves whole workspace, bindings need `bindgen` from registry.
- **One archive, engine library only.** `core/src/meson.build` builds
  `vmafx-core-rs` (`core/src/rust/staticlib`) via
  `core/src/rust/build_staticlib.py` (offline, `--locked`, depfile) and links
  it, shim `core/src/rust/shim/rust_twins.cpp` and
  `core/src/feature/tad_rust.c` into engine library target only (`libvmafx`
  since WP6 split, ADR-2094). Never add Rust symbol to
  `libvmaf_feature_static_lib`, `predict_c_lib` or `model.c`: test binaries
  extract those objects without archive. Second Rust staticlib in same link
  duplicates Rust std symbols.
- **Registry through accessor.** `vmaf_init()` calls
  `vmaf_rust_twins_install()`, which installs `rust_extractor_at` via
  `vmaf_feature_extractor_install_rust_registry()`. Every registry walk in
  `feature_extractor.cpp` goes through `registry_at()`; new walk over
  `feature_extractor_list[]` directly misses Rust extractors and their
  duplicate-name audit.
- **Twins inherit C descriptor.** `add_twin()` copies C extractor, replaces
  only name, callbacks, `priv_size` (C size rounded up plus one instance
  pointer at tail), adds `VMAF_FEATURE_EXTRACTOR_RUST`. Option table, provided
  features, `reads_prev_prev_ref` = C extractor's; option parser and
  feature-name dictionary rely on them. No twin-own options. Copy includes
  `merge` and `extend_name_dict` (ADR-2795): `twin_init()` calls
  `extend_name_dict` after building dictionary; registry keeps C extractor and
  twin apart by name, never by callback.
- **Twin advance = shim trampoline (ADR-2090, MI-1).** `add_twin()` sets
  `advance` to `twin_advance` iff C extractor defines one, else NULL; never
  inherited C callback (C advance derives from Rust SADs into C priv, Rust
  flush appends again, `-EINVAL`). `advance_one_extractor()`
  (`core/src/libvmaf.c`) runs `init_shared_rust_twin()` before pooled twin's
  first advance; keep both. Guard: `test_rust_motion_window_incremental`.
- **C stays default.** `first_pass_eligible()` skips Rust twins when no flag
  asks; only `vmaf_feature_extractor_impl_select()` (env
  `VMAF_FEATURE_IMPL=rust`, read once via `vmaf_gpu_dispatch_env_get()`)
  swaps one in. Keep that call on every registration path
  (`vmaf_use_feature`, `vmaf_use_features_from_model`,
  `create_context_fallback`).
- **ABI header generated.** `core/src/rust/include/vmafx_rs.h` comes from
  `scripts/dev/rust-abi-header.sh` (cbindgen 0.29.4); on conflict take either
  side, regenerate. `VMAFX_RS_ABI_VERSION` stays 1 until release ships ABI.
  `test_rust_abi_layout` compares every size and offset with Rust's table,
  `test_rust_twin_registry` registration, `scripts/ci/rust_twin_diff.py`
  scores (bit identity, no tolerance).
- Archive symbols stay out of engine shared library via
  `-Wl,--exclude-libs,libvmafx_core_rs.a`;
  `nm -D build/src/libvmafx.so | grep _RN` must print nothing.
