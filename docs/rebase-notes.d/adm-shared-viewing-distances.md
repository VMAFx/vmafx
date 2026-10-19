## Netflix/vmaf 33e5f0aca + cffd5b77d: ADM shares two viewing distances (2026-10-08)

- `core/src/feature/feature_extractor.h`: `merge` as upstream, plus the
  fork-only `extend_name_dict` hook. **On sync**: keep both; upstream's
  `merge` sits between `close` and `options`, the fork's after
  `reads_shared_luma_only` (designated initializers make the position
  irrelevant).
- `core/src/fex_ctx_vector.cpp`: `offer_merge()` runs after the dedup loop and
  checks the extractor name and `is_initialized`; upstream offers inside its
  dedup loop on the callback pointer alone. **On sync**: keep the fork's pass.
- `core/src/feature/integer_adm.c`: the fork's per-frame driver is split into
  `*_transform()` and `*_weigh()` per scale; upstream rewrites its single
  `integer_compute_adm()` with an inner per-distance loop and an `AdmScore`
  pair. Upstream keeps a second dictionary (`feature_name_dict_extra`); the
  fork extends the one dictionary with `<base>:nvde` keys through
  `adm_extend_name_dict()`, which the Rust twin shim also calls.
  `adm_merge_view_dist()` differs from upstream's `adm_try_merge_view_dist()`:
  it absorbs a repeat of the second distance and declines a `debug` incoming
  context (docs/development/known-upstream-bugs.md). **On sync**: take
  upstream's arithmetic changes into the `*_weigh()` helpers, keep the
  fork's merge rules.
- `core/src/rust/feature/adm/`: `adm_rust` mirrors the split and files the
  second distance under `score.rs::EXTRA_VIEW_NAMES`.
- `core/test/test_{cuda,sycl,hip}_adm_parity.c`,
  `core/test/test_metal_twin_option_tables_contract.py`: the twins' missing
  `adm_norm_view_dist_extra` is a recorded gap until each backend's pull
  request adds the option and deletes the gap.
