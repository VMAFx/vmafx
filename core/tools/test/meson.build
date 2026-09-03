# Netflix/vmaf#1573 hunk (c): every shell-driven test here invokes a binary
# from the build tree, but none declared `depends`. Meson only rebuilds the
# targets a SELECTED test declares (mesonbuild/mtest.py rebuild_deps(): "if not
# targets: return True"), so running one of these as a subset —
# `meson test test_vmaf_cuda_gpumask`, `meson test --suite=gpu` — built nothing
# and the script died with exit 127 ("./tools/vmaf: No such file or directory").
# `workdir` pins the cwd the scripts' relative `./tools/...` paths assume
# instead of relying on mtest's chdir to the build root.
if get_option('enable_cuda')
    test_vmaf_cuda_gpumask = find_program('test_vmaf_cuda_gpumask.sh')
    test('test_vmaf_cuda_gpumask', test_vmaf_cuda_gpumask,
         suite : ['slow', 'gpu'],
         depends : [vmaf],
         workdir : meson.project_build_root(),
         timeout : 10)
endif

# T6-3b / ADR-0222 — per-shot CRF predictor sidecar smoke test.
test_vmaf_per_shot = find_program('test_vmaf_per_shot.sh')
test('test_vmaf_per_shot', test_vmaf_per_shot,
     suite : ['slow'],
     depends : [vmaf_per_shot],
     workdir : meson.project_build_root(),
     env: [
         'MESON_BUILD_ROOT=' + meson.project_build_root(),
         'MESON_SOURCE_ROOT=' + meson.project_source_root(),
     ])

# T6-2b / ADR-0247 — vmaf-roi accepts high-bit-depth planar YUV by
# downscaling luma to the existing 8-bit saliency-model contract.
test_vmaf_roi_high_bitdepth = find_program('test_vmaf_roi_high_bitdepth.sh')
test('test_vmaf_roi_high_bitdepth', test_vmaf_roi_high_bitdepth,
     suite : ['slow'],
     depends : [vmaf_roi],
     workdir : meson.project_build_root(),
     env: [
         'MESON_BUILD_ROOT=' + meson.project_build_root(),
         'MESON_SOURCE_ROOT=' + meson.project_source_root(),
     ])
