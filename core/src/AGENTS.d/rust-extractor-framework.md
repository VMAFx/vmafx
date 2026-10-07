---
paths:
  - core/src/rust/**
  - core/src/feature/feature_extractor.cpp
  - core/src/feature/feature_extractor.h
  - core/src/feature/tad_rust.c
  - core/src/libvmaf.c
  - core/src/meson.build
invariant: One Rust archive in libvmaf only; registry reaches Rust via the shim accessor; twins inherit C descriptor.
---
<!-- markdownlint-disable MD013 -->
# Rust extractor framework (ADR-1713)

- **Separate, dependency-free workspace.** `core/src/rust/Cargo.toml` is its
  own workspace (the root one `exclude`s it and the TAD crate, which names it
  with `package.workspace`). Its `Cargo.lock` holds no external crate; adding
  one breaks Meson's `cargo build --offline --locked` and the empty-CARGO_HOME
  step of `rust-ci.yml`, on purpose. Never move these crates back into the
  root workspace: cargo resolves the whole workspace and the bindings need
  `bindgen` from the registry.
- **One archive, libvmaf only.** `core/src/meson.build` builds
  `vmafx-core-rs` (`core/src/rust/staticlib`) with
  `core/src/rust/build_staticlib.py` (offline, `--locked`, depfile) and
  links it, the shim `core/src/rust/shim/rust_twins.cpp` and
  `core/src/feature/tad_rust.c` into the `libvmaf` library target only. Never
  add a Rust symbol to `libvmaf_feature_static_lib`, `predict_c_lib` or
  `model.c`: test binaries extract those objects without the archive. A
  second Rust staticlib in the same link duplicates the Rust std symbols.
- **Registry through an accessor.** `vmaf_init()` calls
  `vmaf_rust_twins_install()`, which installs `rust_extractor_at` with
  `vmaf_feature_extractor_install_rust_registry()`. Every registry walk in
  `feature_extractor.cpp` goes through `registry_at()`; a new walk over
  `feature_extractor_list[]` directly misses the Rust extractors and their
  duplicate-name audit.
- **Twins inherit the C descriptor.** `add_twin()` copies the C extractor and
  replaces only the name, callbacks, `priv_size` (C size rounded up plus one
  instance pointer at the tail) and adds `VMAF_FEATURE_EXTRACTOR_RUST`. The
  option table, provided features and `reads_prev_prev_ref` are the C
  extractor's, which the option parser and the feature-name dictionary rely
  on. Do not give a twin options of its own.
- **C stays default.** `first_pass_eligible()` skips Rust twins when no flag
  is asked for; only `vmaf_feature_extractor_impl_select()` (env
  `VMAF_FEATURE_IMPL=rust`, read once through `vmaf_gpu_dispatch_env_get()`)
  swaps one in. Keep that call on every registration path
  (`vmaf_use_feature`, `vmaf_use_features_from_model`,
  `create_context_fallback`).
- **ABI header is generated.** `core/src/rust/include/vmafx_rs.h` comes from
  `scripts/dev/rust-abi-header.sh` (cbindgen 0.29.4); on a conflict take
  either side and regenerate. `test_rust_abi_layout` compares every size and
  offset with Rust's table, `test_rust_twin_registry` the registration,
  `scripts/ci/rust_twin_diff.py` the scores (bit identity, no tolerance).
- Symbols of the archive stay out of `libvmaf.so` via
  `-Wl,--exclude-libs,libvmafx_core_rs.a`; `nm -D build/src/libvmaf.so | grep _RN`
  must print nothing.
