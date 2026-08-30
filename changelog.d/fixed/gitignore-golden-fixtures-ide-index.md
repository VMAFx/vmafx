- **The Netflix golden YUV fixtures are no longer reachable by `git clean -xfd`.**
  `python/test/resource/yuv/` (160 MB, 26 files) and
  `python/test/resource/test_image_yuv/` were neither tracked nor ignored, so a
  routine `git clean -xfd` would have silently deleted the fork's numerical
  ground truth (CLAUDE.md §8). They are now ignored, which both protects them
  from `clean` and removes them from `git status` noise. Re-fetch remains
  `scripts/test/fetch-test-yuvs.sh`.
- **The IDE C/C++ index pointed at a build directory that no longer exists.**
  `.vscode/settings.json` hardcoded an absolute path to
  `core/builddir/compile_commands.json`, contradicting
  `.vscode/c_cpp_properties.json` which uses `${workspaceFolder}/build/`. Worse,
  every build directory configured before the ADR-0700 `libvmaf/` → `core/`
  rename has a source dir pointing into the now-empty `libvmaf/`: the index in
  `core/build/` has 660 entries and all 660 reference
  `/home/kilian/dev/vmaf/libvmaf/src/*.c`. clangd and IntelliSense have therefore
  been resolving nothing since the rename. `settings.json` now uses the same
  `${workspaceFolder}/build/` path as `c_cpp_properties.json`; regenerating that
  directory with `meson setup build core` produces an index that resolves.
- The in-place Cython extension under `compat/python-vmaf/core/*.so`
  (`setup.py build_ext`) is ignored too — it was showing as an untracked file
  after any Python-harness build.
