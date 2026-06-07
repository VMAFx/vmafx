if get_option('enable_cuda')
    test_vmaf_cuda_gpumask = find_program('test_vmaf_cuda_gpumask.sh')
    test('test_vmaf_cuda_gpumask', test_vmaf_cuda_gpumask,
         suite : ['slow', 'gpu'],
         timeout : 10)
endif

# T6-3b / ADR-0222 — per-shot CRF predictor sidecar smoke test.
test_vmaf_per_shot = find_program('test_vmaf_per_shot.sh')
test('test_vmaf_per_shot', test_vmaf_per_shot,
     suite : ['slow'],
     env: [
         'MESON_BUILD_ROOT=' + meson.project_build_root(),
         'MESON_SOURCE_ROOT=' + meson.project_source_root(),
     ])

# T6-2b / ADR-0247 — vmaf-roi accepts high-bit-depth planar YUV by
# downscaling luma to the existing 8-bit saliency-model contract.
test_vmaf_roi_high_bitdepth = find_program('test_vmaf_roi_high_bitdepth.sh')
test('test_vmaf_roi_high_bitdepth', test_vmaf_roi_high_bitdepth,
     suite : ['slow'],
     env: [
         'MESON_BUILD_ROOT=' + meson.project_build_root(),
         'MESON_SOURCE_ROOT=' + meson.project_source_root(),
     ])
