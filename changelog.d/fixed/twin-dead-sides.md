- **core**: Resolve the three dead `.c`/`.cpp` twin sides surfaced by the
  ADR-1135 twin-drift gate (task `T-TWIN-DEAD-SIDES-2026-09-02`, [ADR-1153](docs/adr/1153-twin-dead-sides-resolution.md)):
  - Deleted `core/src/model.cpp`. An evidence-based function-by-function diff
    revealed `model.cpp` was stale and incomplete (missing the 8 built-in VMAF
    v1.0.16 SDR models from PR #1024, missing `predict_cache_lock` mutex destroy
    from PR #864, containing a heap-buffer-overflow in `vmaf_model_destroy`
    fixed in `model.c` by PR #743, and lacking promised RAII guards).
    `core/src/model.c` remains the sole, verified, authoritative model source.
  - Deleted `core/test/test_dict.c` and `core/test/test_feature.c`. Both were
    uncompiled pre-port C test files that text-included superseded `.c` sources
    (`dict.c` and `feature_name.c`); their surviving C++ twins `core/test/test_dict.cpp`
    and `core/test/test_feature.cpp` are already compiled and active in
    `core/test/meson.build`.
  - Removed all three resolved rows from `scripts/ci/twin-drift-allowlist.txt`,
    shrinking the allowlist to zero dead sides.
