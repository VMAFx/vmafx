## Fixed

- **CI: libvmafx_tad link error** — CI builds on Ubuntu runners failed with
  `/usr/bin/ld: cannot find .../core/build/src/libvmafx_tad.a: No such file or directory`
  because `enable_rust_features` defaulted to `true`. GitHub Actions Ubuntu runners
  have `cargo` in PATH but lack `cbindgen` (the TAD crate build dependency), so the
  `cargo build` step silently failed to produce the `.a`. The archive path was still
  injected as a `link_arg` into every test that transitively linked libvmaf.a, blocking
  the entire build. Fix: default `enable_rust_features=false`; opt in via
  `-Denable_rust_features=true` when a full Rust + cbindgen toolchain is available.

- **CI: orphan test_ansnr_simd reference** — PR #38 (drop-ansnr) deleted
  `core/test/test_ansnr_simd.c` but left its `executable()` and `test()` declarations
  in `core/test/meson.build`. Meson halted configuration with
  `ERROR: File test_ansnr_simd.c does not exist` on every CPU architecture.
  Both declarations have been removed.
