## Added

- **Per-surface documentation closeout for ADR-0100 gaps** (audit
  2026-05-30): document every user-discoverable surface that landed
  in tree without its own docs page or row, per CLAUDE.md §12 r10 and
  ADR-0100 per-surface minimum bars.
  - `docs/development/build-flags.md`: added six previously undocumented
    Meson options (`sycl_acpp_targets`, `sycl_icpx_aot_targets`,
    `enable_metal`, `enable_float_vif_hip_autodispatch`, `hip_gfx_targets`,
    `enable_rust_features`). The "Options referenced in docs but not
    present" note is now exhaustive — every `core/meson_options.txt`
    entry has a row.
  - `docs/api/dnn.md`: added entries for `vmaf_dnn_set_codec_context`
    (ADR-0519 codec block) and `vmaf_dnn_set_resize_mode` (ADR-0550
    auto-resize). Both are exported by `core/include/libvmaf/dnn.h` and
    were exercised by the CLI without a public-API doc page.
  - `docs/usage/cli.md`: added a "Codec-context flags" section
    documenting `--tiny-codec`, `--tiny-preset`, `--tiny-crf`, and
    `--tiny-resize`. These flags drive the new DNN entry points and
    had no CLI reference.
