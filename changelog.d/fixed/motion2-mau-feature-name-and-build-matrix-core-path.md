Fix two CI regressions: (1) `test_sycl_motion_add_uv_parity` queried the wrong feature
name `float_motion2_mau` instead of the correct aliased name `motion2_mau` (alias.c maps
`VMAF_feature_motion2_score` → `motion2`, not `float_motion2`); (2) `libvmaf-build-matrix.yml`
used stale `libvmaf` source-directory in all `meson setup` invocations (ADR-0700 renamed the
directory to `core`), causing every job in the matrix to fail with "Neither source directory
'libvmaf' nor build directory exist".
