# Included from libvmaf/src/meson.build via `subdir('hip')`. Paths
# below are relative to libvmaf/src/hip/. The feature stubs live
# under libvmaf/src/feature/hip/, so we use a `..` walk to reach
# them — same shape ADR-0175 uses for the Vulkan scaffold.
#
# Scaffold-only (ADR-0209 / T7-10): every entry point returns -ENOSYS
# until the kernels land, so the build has no HIP / ROCm SDK
# requirement yet. The dependency() probe below stays optional and
# is only consulted if the toolchain happens to expose pkg-config
# metadata for `hip-lang`; the scaffold compiles cleanly on a stock
# Ubuntu runner without any AMD packages installed.
# Kernel-template helpers (T7-10 first-consumer / ADR-0241):
# `kernel_template.c` carries the per-frame async lifecycle helpers
# (stream + event pair + readback alloc/free) that mirror
# `cuda/kernel_template.h`. Bodies return -ENOSYS while the runtime
# is absent; the runtime PR (T7-10b) replaces them with real HIP
# calls without touching consumer TUs.
#
# `feature/hip/integer_psnr_hip.c` is the first kernel-template
# consumer — registers `vmaf_fex_psnr_hip` and exercises every
# template helper in init/submit/collect/close. -ENOSYS at the
# template layer surfaces verbatim through the consumer.
#
# `feature/hip/ciede_hip.c` is the third kernel-template consumer
# (ADR-0257) — registers `vmaf_fex_ciede_hip` and mirrors the CUDA
# twin (`feature/cuda/integer_ciede_cuda.c`). Same scaffold posture:
# the consumer compiles host-only and `init()` surfaces -ENOSYS
# through the kernel-template helpers until T7-10b lands real
# `hipStreamCreate` / `hipMemcpyAsync` bodies.
#
# `feature/hip/float_moment_hip.c` is the fourth kernel-template
# consumer (ADR-0258) — registers `vmaf_fex_float_moment_hip` and
# mirrors the CUDA twin (`feature/cuda/integer_moment_cuda.c`).
# Same scaffold posture as the third consumer; emits four features
# (`float_moment_ref{1st,2nd}`, `float_moment_dis{1st,2nd}`) once
# the runtime kernel arrives.
# `feature/hip/integer_motion_v2_hip.c` is the sixth kernel-template
# consumer (ADR-0267) — registers `vmaf_fex_integer_motion_v2_hip`
# and mirrors the CUDA twin
# (`feature/cuda/integer_motion_v2_cuda.c`). Pins the **temporal**
# extractor shape (`VMAF_FEATURE_EXTRACTOR_TEMPORAL` flag,
# `flush()` callback) and the ping-pong buffer carry-over needed
# for motion-class metrics. Same scaffold posture: `init()`
# returns -ENOSYS until T7-10b.
#
# `feature/hip/float_motion_hip.c` is the seventh kernel-template
# consumer (ADR-0270) — registers `vmaf_fex_float_motion_hip` and
# mirrors the CUDA twin (`feature/cuda/float_motion_cuda.c`). Pins
# the temporal-extractor shape with a raw-pixel cache + blurred-frame
# ping-pong (`uintptr_t ref_in` + `uintptr_t blur[2]` slots tracked
# outside the kernel-template's readback bundle); emits two features
# (`VMAF_feature_motion_score`, `VMAF_feature_motion2_score`) once
# the runtime kernel arrives.
#
# `feature/hip/float_ssim_hip.c` is the eighth kernel-template
# consumer (ADR-0271) — registers `vmaf_fex_float_ssim_hip` and
# mirrors the CUDA twin (`feature/cuda/integer_ssim_cuda.c`, despite
# its filename emits `float_ssim`). Pins the two-dispatch + five
# intermediate float buffers shape; emits one feature
# (`float_ssim`) once the runtime kernel arrives. v1: scale=1 only,
# auto-decimation rejected at init with -EINVAL (mirrors the CUDA
# twin's validation).
hip_sources += files(
    'common.c',
    'picture_hip.c',
    'dispatch_strategy.c',
    'kernel_template.c',
    # adm_hip.c, vif_hip.c, motion_hip.c were deleted by ADR-0546:
    # they defined only vmaf_hip_{adm,vif,motion}_{init,run,destroy} stubs
    # (init=0, run=-ENOSYS) with no VmafFeatureExtractor registration and
    # zero callers anywhere in the repo. feature_hip.h (their only header)
    # was also removed. See ADR-0546 for the full audit trail.
    '../feature/hip/integer_psnr_hip.c',
    '../feature/hip/ciede_hip.c',
    '../feature/hip/float_moment_hip.c',
    '../feature/hip/integer_motion_v2_hip.c',
    # integer_motion_hip: HIP port of the integer motion extractor (ADR-0528).
    # Mirrors `feature/cuda/integer_motion_cuda.c` — TEMPORAL flag, per-frame
    # SAD reduction, motion3 5-frame window. Emits motion_score, motion2_score,
    # motion3_score. Registered under `#if HAVE_HIP` in feature_extractor.c.
    '../feature/hip/integer_motion_hip.c',
    '../feature/hip/float_motion_hip.c',
    '../feature/hip/float_ssim_hip.c',
    # T7-10b first real kernel: float_psnr_hip (ADR-0254). Registered under
    # `#if HAVE_HIP` in feature_extractor.c. With `enable_hipcc=true` the
    # HSACO blob is embedded and the kernel runs on device; without it
    # init() returns -ENOSYS (same scaffold-posture as the other consumers).
    '../feature/hip/float_psnr_hip.c',
    # integer_vif_hip: real HIP port of the integer VIF extractor.
    # Eight kernels in integer_vif/vif_statistics.hip (four vertical +
    # four horizontal, one per scale); compiled to HSACO when
    # enable_hipcc=true. Without it init() returns -ENOSYS.
    '../feature/hip/integer_vif_hip.c',
    # T7-10b batch-2 twelfth consumer: float_adm_hip (ADR-0468). Mirrors the
    # CUDA twin `feature/cuda/float_adm_cuda.c` — 4-stage DWT+CSF+CM
    # pipeline, 16 launches per frame, host double-precision reduction.
    # With `enable_hipcc=true` the HSACO blob runs on device; without it
    # init() returns -ENOSYS.
    '../feature/hip/float_adm_hip.c',
    # integer_cambi_hip: HIP port of CAMBI extractor.
    # Registered as #if HAVE_HIP entry in feature_extractor.c.
    '../feature/hip/integer_cambi_hip.c',
    # ADR-0533: full HIP-extractor registration sweep. Each TU below
    # already declares `VmafFeatureExtractor vmaf_fex_*_hip` and mirrors
    # a CUDA twin call-graph-for-call-graph, but was never compiled into
    # the HIP runtime archive. The companion edit in
    # `libvmaf/src/feature/feature_extractor.c` adds the extern + registry
    # entry for each so `vmaf_get_feature_extractor_by_name` resolves.
    #
    # Scaffold posture: without `enable_hipcc=true` each init() returns
    # -ENOSYS (same contract as the prior consumers); with hipcc the
    # HSACO blob runs where the kernel is real, otherwise the extractor
    # surfaces "found but runtime not ready" through dispatch.
    '../feature/hip/float_vif_hip.c',
    '../feature/hip/integer_adm_hip.c',
    '../feature/hip/integer_ms_ssim_hip.c',
    '../feature/hip/integer_psnr_hvs_hip.c',
    '../feature/hip/integer_ssim_hip.c',
    '../feature/hip/ssimulacra2_hip.c',
    # ADR-0964: speed_chroma_hip + speed_temporal_hip — HIP GPU twins.
    # Mirror the CUDA twins' hybrid GPU/CPU split: device-side means /
    # covariance / indterm / score, CPU-side eigendecomp + QR + Q^T
    # multiply via feature/speed_internal.c.  Without enable_hipcc=true
    # init() returns -ENOSYS (scaffold posture).
    '../feature/hip/speed_chroma_hip.c',
    '../feature/hip/speed_temporal_hip.c',
    # ADR-0537: weak fallback definitions for HSACO blob symbols whose
    # .hip kernel sources don't yet build standalone via `hipcc --genco`
    # (currently the four integer_adm kernels). Keeps the link succeed
    # under `enable_hipcc=true`; xxd-embedded real blobs override these
    # weak symbols at link time once the corresponding kernel is added
    # to `hip_kernel_sources` in src/meson.build. The file is now
    # committed alongside this reference (PR #1297 removed a dangling
    # version of this entry; this PR adds the actual file).
    '../feature/hip/hip_hsaco_stubs.c',
    # Note: speed_chroma_hip.c and speed_temporal_hip.c are already included
    # above (ADR-0964 block, lines ~120-126). The ADR-0852 duplicate wiring
    # has been removed to prevent duplicate-symbol link errors.
)

# T7-10b runtime linkage. ROCm ships `hip_runtime_api.h` under
# `/opt/rocm/include` and the host runtime under
# `/opt/rocm/lib/libamdhip64.so`. The ROCm 7.x package does NOT
# publish a `hip-lang.pc`; the cmake config is the supported
# discovery path. Try meson's pkg-config / cmake probe first; on
# miss, fall back to a manual `find_library` rooted at the usual
# ROCm install prefixes plus an `HIP_PATH` env override.
hip_search_paths = []
hip_path_env = run_command(
    'sh', '-c', 'printf %s "${HIP_PATH:-}"', check: true).stdout()
if hip_path_env != ''
    hip_search_paths += [hip_path_env / 'lib', hip_path_env / 'lib64']
endif
hip_search_paths += [
    '/opt/rocm/lib',
    '/opt/rocm/lib64',
    '/usr/lib/x86_64-linux-gnu',
    '/usr/lib64',
]

hip_runtime_dep = dependency('hip-lang', required: false)
if not hip_runtime_dep.found()
    hip_lib = cc.find_library('amdhip64',
        dirs: hip_search_paths,
        required: true)
    hip_include_candidates = []
    if hip_path_env != ''
        hip_include_candidates += hip_path_env / 'include'
    endif
    hip_include_candidates += ['/opt/rocm/include', '/usr/include']
    hip_include_dir = ''
    foreach candidate : hip_include_candidates
        if hip_include_dir == '' and run_command(
            'sh', '-c', 'test -f "@0@/hip/hip_runtime_api.h"'.format(candidate),
            check: false).returncode() == 0
            hip_include_dir = candidate
        endif
    endforeach
    if hip_include_dir == ''
        error('HIP runtime header `<hip/hip_runtime_api.h>` not found. ' +
              'Set HIP_PATH or install rocm-hip-runtime-dev.')
    endif
    hip_runtime_dep = declare_dependency(
        dependencies: [hip_lib],
        compile_args: ['-D__HIP_PLATFORM_AMD__=1'],
        include_directories: include_directories(hip_include_dir, is_system: true),
    )
endif
hip_deps += hip_runtime_dep
