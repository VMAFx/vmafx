- **`nightly.yml` whole-tree clang-tidy scan**: Fixed `clang-tidy-full` job to use
  `CC=gcc-15 CXX=g++-15 -Db_lto=false` and `--clang-tidy /usr/bin/clang-tidy-22`,
  matching the PR lane that was already correct. Previously the missing `-Db_lto=false`
  caused Meson to emit `-flto=4` into compile commands, making the ratchet measurement
  unusable (15/15 consecutive master job failures; ADR-1321, Research-2107).
- **`fuzz.yml` and `sanitizers.yml` runner timeout**: Raised `timeout-minutes` from 15
  to 30 for the `fuzz_cli_parse` harness that compiles the full libvmaf with ASan after
  downloading Clang 22 — an 11–16 min hosted workload with no margin
  for IO jitter (ADR-1321, Research-2107).
- **ADR-1093 status**: Marked Superseded (both `should_fail` annotations resolved:
  ADR-1099 for `test_sycl_motion_add_uv_parity`, PR #840 for `test_pic_preallocation`).
