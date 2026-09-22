- Stale pre-[ADR-0700](docs/adr/0700-vmafx-repo-layout.md) source paths in
  comments and documentation under `core/`. The `libvmaf/` → `core/` rename left
  267 cross-references naming a directory that no longer exists, so every "see
  `libvmaf/src/feature/...`" note sent the reader nowhere. 216 of them, across
  124 files, now name their real path.

  Renaming the prefix was only half the fix: 24 of the rewritten targets still
  did not resolve, because a comment stale enough to name the old directory was
  usually stale about the filename too. Those were corrected against the tree as
  it actually is — the C-to-C++ moves (`feature_extractor`, `cli_parse`,
  `output`, `metadata_handler`, `mem`, `opt` → `.cpp`), the Objective-C++ moves
  (`metal/kernel_template`, `integer_motion_v2_metal` → `.mm`), `picture_copy.c`
  → `core/src/feature/picture_copy.cpp`, the Metal metallib `custom_target`
  (which lives in `core/src/metal/meson.build`, not `core/src/feature/metal/`),
  `motion_v2_avx2.c` → `core/src/feature/x86/motion_avx2.c`, the `float_motion`
  Metal CPU reference → `core/src/feature/float_motion.c`, and the
  `dispatch_strategy` module list, which now names the four backends that
  actually ship one (`sycl`, `cuda`, `hip`, `metal`) instead of the Vulkan
  backend removed by [ADR-0726](docs/adr/0726-drop-vulkan-backend.md). Every
  rewritten path was then checked to exist on disk.

  Comments and documentation only — no code, no behaviour, and no Netflix
  golden-data assertion is touched.

  **51 references are deliberately left as they are.** Three are correct: two
  `github.com/Netflix/vmaf` links in `core/README.md` and the ansnr
  re-introduction watch in `core/src/feature/AGENTS.md` all name *upstream's*
  tree, not ours. The other 48 sit in 34 files that carry pre-existing HISS debt
  (legacy `goto` spines, oversized GPU and SIMD kernels, Rust `unsafe` blocks).
  The touched-file-must-be-clean rule ignores the baseline by construction, so
  correcting a comment in one of those files makes the whole file the PR's
  responsibility — turning a documentation fix into a refactor of numerical hot
  paths. They are tracked as `BUG-052` follow-up work and want their own scoped
  change, not a comment sweep.
