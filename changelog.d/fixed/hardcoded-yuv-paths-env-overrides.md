Replace hardcoded `/home/kilian/dev/libvmaf_vulkan/…` absolute paths in
`testdata/test_all_backends.sh`, `testdata/bench_quick.py`,
`testdata/compare_combined.py`, and `ai/tests/test_e2e_frame_to_score.py`
with `VMAF_BIN` / `VMAF_YUVDIR` / `VMAF_TESTDATA` env-var overrides that
fall back to repo-root-relative defaults.  Also fixes the stale
`libvmaf/build-cpu/` path (renamed to `core/` in ADR-0700) in the AI test.
See ADR-0792.
