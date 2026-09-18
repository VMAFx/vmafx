- **`make tidy-ratchet LANE=cuda`, `LANE=hip` and `LANE=sycl` run again, and
  measure the CUDA and HIP kernel files.** `tidy-ratchet.py` handed the lanes'
  compiler flags to clang-tidy as clang-tidy options, and resolved the build
  directory and the SYCL wrapper relative to each translation unit's
  directory, so every GPU lane failed before measuring anything. The `.cu` and
  `.hip` files were also missing from `compile_commands.json`, because meson
  builds them through custom targets; the new
  `scripts/ci/gen-gpu-compile-commands.py` adds them, and the make target runs
  it first. The Tidy Ratchet job now tests this tooling on every pull request.
