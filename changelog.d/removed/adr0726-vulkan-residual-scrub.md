## Removed

- **Vulkan residual config switches and stale doc claims scrubbed (ADR-0726
  follow-up):** Removed the two remaining live `-Denable_vulkan=enabled` matrix
  rows from `.github/workflows/libvmaf-build-matrix.yml` (Ubuntu Vulkan T5-1b
  and macOS MoltenVK lanes). Updated `docs/backends/index.md`,
  `docs/usage/cli.md`, `docs/usage/ffmpeg.md`, `docs/benchmarks.md`,
  `docs/development/ide-setup.md`, and `docs/backends/kernel-scaffolding.md` to
  reflect the removal. Marked `docs/backends/vulkan/overview.md` and all Vulkan
  source `AGENTS.md` files as historical-reference-only. Updated `CLAUDE.md`
  section 15 to remove Vulkan from the active backend list and parallel-lane
  guidance. Removed stale `-Denable_vulkan=disabled` flags from closed-bug
  reproducer commands in `docs/state.md` (the option no longer exists in
  `meson_options.txt`).
