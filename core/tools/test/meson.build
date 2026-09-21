# Netflix/vmaf#1573 hunk (c): every shell-driven test here invokes a binary
# from the build tree, but none declared `depends`. Meson only rebuilds the
# targets a SELECTED test declares (mesonbuild/mtest.py rebuild_deps(): "if not
# targets: return True"), so running one of these as a subset —
# `meson test test_vmaf_cuda_gpumask`, `meson test --suite=gpu` — built nothing
# and the script died with exit 127 ("./tools/vmaf: No such file or directory").
# `workdir` pins the cwd the scripts' relative `./tools/...` paths assume
# instead of relying on mtest's chdir to the build root.
# test_vmaf_cuda_gpumask: budget sized from measurement, not from the fixture.
#
# The former `timeout : 10` came from T-CUDA-GPUMASK-TIMEOUT-2026-06-08, where
# it was a fast-fail for a GPU-less host whose SYCL runtime blocks ~30 s
# enumerating OpenCL devices. The `nvidia-smi -L` guard added in that same
# change already exits 77 on such a host, so 10 s no longer buys that; what it
# did buy was a flake. Measured on the RTX 4090 workstation:
#
#   first run after a CUDA build/relink   9.70 s, 9.48 s   (observed twice)
#   steady state, same build              0.95 s … 1.63 s  (observed many)
#
# The cost is bimodal, and the slow mode is a first-run premium on the driver's
# module-load path for a newly built `libvmaf.so.3.0.0` (10.6 MB of embedded
# CUDA fatbins) — paid before a single pixel is read. It does not reproduce on
# every relink, so treat the trigger as unpinned and the 9.7 s as the measured
# worst case, not a ceiling. CI always lands in that mode: it builds, then
# tests. That is the reported 10.09 s overrun, which needed scheduler
# contention to add only 5% on top of what was measured here.
#
# The fixture size cannot fix this and must not be shrunk for it: at 2 frames
# the four invocations total ~1.0 s at 576x324 and ~1.0 s at 1920x1080 (the
# per-invocation pixel work is 14 ms vs 55 ms; ~175 ms of each invocation is
# fixed CUDA bring-up). See the script's header comment for that table.
#
# So: `timeout : 120` is ~12x the measured worst case against a ~1 s steady
# state, which still fails decisively on a wedged context or a blocked
# enumeration — the sibling test_vmaf_<backend>_threads below already uses 300.
# `is_parallel : false` removes the other half: mtest drains in-flight tests
# and runs this one exclusively (mesonbuild/mtest.py: `if not
# runner.is_parallel: await complete_all(futures) ... await complete(future)`),
# so it no longer shares the device with test_vmaf_cuda_threads.
if get_option('enable_cuda')
    test_vmaf_cuda_gpumask = find_program('test_vmaf_cuda_gpumask.sh')
    test('test_vmaf_cuda_gpumask', test_vmaf_cuda_gpumask,
         suite : ['slow', 'gpu'],
         depends : [vmaf],
         workdir : meson.project_build_root(),
         is_parallel : false,
         timeout : 120)
endif

# ADR-1197 — `vmaf --threads N` on a GPU backend must succeed and agree with the
# serial run. Registered once per GPU backend that is compiled in; the script
# skips (exit 77) when the backend is not usable on the machine.
foreach gpu_backend : ['cuda', 'sycl']
    if get_option('enable_' + gpu_backend)
        test_vmaf_gpu_threads = find_program('test_vmaf_gpu_threads.sh')
        test('test_vmaf_' + gpu_backend + '_threads', test_vmaf_gpu_threads,
             args : [gpu_backend],
             suite : ['slow', 'gpu'],
             depends : [vmaf],
             workdir : meson.project_build_root(),
             env : [
                 'MESON_BUILD_ROOT=' + meson.project_build_root(),
                 'MESON_SOURCE_ROOT=' + meson.project_source_root(),
             ],
             timeout : 300)
    endif
endforeach

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

# ADR-1262 — a failed input read must exit non-zero. Pure CPU and tiny (24x24,
# two frames), so it belongs in the fast pre-push suite rather than 'slow'.
test_vmaf_read_error_exit = find_program('test_vmaf_read_error_exit.sh')
test('test_vmaf_read_error_exit', test_vmaf_read_error_exit,
     suite : ['fast'],
     depends : [vmaf],
     workdir : meson.project_build_root(),
     env: [
         'MESON_BUILD_ROOT=' + meson.project_build_root(),
         'MESON_SOURCE_ROOT=' + meson.project_source_root(),
     ])
