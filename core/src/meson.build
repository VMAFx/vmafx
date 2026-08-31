vmaf_soversion = vmaf_api_version_major

# Build libvmaf
feature_src_dir = './feature/'
src_dir = './'
model_dir = '../../model/'
cuda_dir = './cuda/'
sycl_dir = './sycl/'

vmaf_base_include = include_directories('./', './feature', './feature/common', '../include')

vmaf_cflags_common = cc.get_supported_arguments([
    '-pedantic',
    # ADR-0692: enforce the JPL Rule-24 fallthrough annotation at compile time.
    # C23 provides the standard [[fallthrough]] spelling.
    '-Wimplicit-fallthrough',
    # -DOC_NEW_STYLE_INCLUDES removed: dead legacy define from upstream libogg era;
    # no C consumer exists anywhere in the tree.
    # ADR-0377: hide all symbols by default; only vmaf_*-prefixed entry
    # points annotated with VMAF_EXPORT in the public headers are visible
    # in the final libvmaf.so. This eliminates the 207-symbol leak
    # found by Research-0092 (libsvm C API, pdjson, SIMD internals, etc.)
    '-fvisibility=hidden',
]) + [
    # Suppresses the VMAF_POOL_METHOD_NB deprecation attribute for internal
    # translation units that legitimately iterate over all pool methods (e.g.
    # output.c). External consumers without this flag will see the warning.
    '-DVMAF_BUILDING_LIBVMAF',
]


cdata = configuration_data()
cdata_asm = configuration_data()

if cc.symbols_have_underscore_prefix()
    cdata.set10('PREFIX', true)
    cdata_asm.set10('PREFIX', true)
endif

# Check for locale handling functions
uselocale_test = '''
#define _GNU_SOURCE
#include <locale.h>
int main() {
    locale_t loc = uselocale((locale_t)0);
    return 0;
}
'''
if cc.compiles(uselocale_test, name: 'uselocale availability', args : test_args)
    cdata.set('HAVE_USELOCALE', 1)
endif

# Check for xlocale.h header (needed on some older macOS versions)
if cc.has_header('xlocale.h', args : test_args)
    cdata.set('HAVE_XLOCALE_H', 1)
endif

is_asm_enabled = get_option('enable_asm') == true
is_cuda_enabled = get_option('enable_cuda') == true
is_sycl_enabled = get_option('enable_sycl') == true
# HIP (AMD ROCm) compute backend — scaffold ADR-0209 / T7-10. Default OFF;
# every entry point returns -ENOSYS until the runtime PR lands.
is_hip_enabled = get_option('enable_hip') == true
# Metal (Apple Silicon) compute backend — scaffold ADR-0338 / T8-1. Default
# `auto`: probes for Metal.framework on macOS, disabled elsewhere. Apple
# Silicon (M1+) only at runtime — Intel Macs surface as -ENODEV. Every
# entry point returns -ENOSYS until the runtime PR (T8-1b) lands.
metal_opt = get_option('enable_metal')
if metal_opt.auto()
    # Auto-resolve on host_machine.system() == 'darwin'; disabled on every
    # other host so Linux / Windows builds compile cleanly without the
    # frameworks. The runtime PR (T8-1b) tightens this to require the
    # frameworks once the option resolves to enabled.
    is_metal_enabled = (host_machine.system() == 'darwin')
else
    is_metal_enabled = metal_opt.enabled()
endif
is_avx512_enabled = get_option('enable_avx512') == true
is_nvtx_enabled = get_option('enable_nvtx') == true
# AVX-512 is a pure assembly path and requires enable_asm=true to have any
# effect.  When enable_asm=false (e.g. i686 cross-compile or --no-asm builds)
# silently disabling would produce a confusing build; warn and force-disable
# instead of hard-erroring, so no-asm consumer builds configure cleanly.
if is_avx512_enabled and not is_asm_enabled
    warning('enable_avx512=true has no effect when enable_asm=false; ' +
            'AVX-512 is an assembly path — disabling AVX-512 for this build.')
    is_avx512_enabled = false
endif
if is_nvtx_enabled and host_machine.system() == 'windows'
    warning('NVTX is not supported on Windows, disabling.')
    is_nvtx_enabled = false
endif
# NVTX requires the CUDA toolkit headers even when enable_cuda=false because
# nvToolsExt.h lives under /usr/local/cuda/include. Without this guard, meson
# hard-errors on any host without a CUDA installation when enable_nvtx=true and
# enable_cuda=false (the include_directories call at line ~484 fails with
# "Include dir /usr/local/cuda/include does not exist").
# Audit finding: build-matrix-symbols-audit-2026-05-16.md §1a.
if is_nvtx_enabled and not is_cuda_enabled
    error('enable_nvtx=true requires enable_cuda=true (NVTX headers reside in the CUDA toolkit)')
endif

# DNN runtime (ONNX Runtime) — defines dnn_sources / dnn_defines / dnn_deps.
subdir('dnn')

# Embedded MCP server (ADR-0128 design; ADR-0209 / T5-2 scaffold;
# T5-2b runtime). When `enable_mcp=true`, `subdir('mcp')` exposes
# `mcp_sources` + `mcp_defines`. When the umbrella flag is off the
# variables stay empty and the libvmaf TU set is unchanged.
# Validate MCP sub-flag combinations: the enable_mcp_sse/uds/stdio options
# require the umbrella enable_mcp flag to be true. These options are only
# evaluated inside subdir('mcp'), which is unreachable when enable_mcp=false.
# Without this guard, users can set enable_mcp_sse=enabled + enable_mcp=false
# and silently get a no-op (the promised error from mcp/meson.build:35 is
# never reached). Same for enable_mcp_uds and enable_mcp_stdio.
if not get_option('enable_mcp')
    mcp_sse_opt = get_option('enable_mcp_sse')
    if mcp_sse_opt.enabled()
        error('enable_mcp_sse=enabled/true requires enable_mcp=true')
    endif
    if get_option('enable_mcp_uds')
        error('enable_mcp_uds=true requires enable_mcp=true')
    endif
    if get_option('enable_mcp_stdio')
        error('enable_mcp_stdio=true requires enable_mcp=true')
    endif
endif
is_mcp_enabled = get_option('enable_mcp')
mcp_sources = []
mcp_defines = []
if is_mcp_enabled
    subdir('mcp')
    vmaf_cflags_common += mcp_defines
endif

# HIP compute backend (ADR-0209 / T7-10 scaffold). Same pattern: empty arrays
# when the option is off, no-cost
# on default builds. The dependency() probe inside `subdir('hip')` is
# `required: false`; the scaffold compiles with no ROCm SDK installed.
hip_sources = []
hip_deps = []
hip_hsaco_sources = []
is_hipcc_enabled = get_option('enable_hipcc') == true
if is_hipcc_enabled and not is_hip_enabled
    warning('enable_hipcc=true has no effect when enable_hip=false')
endif
# ADR-0642: gate VMAF_FEATURE_EXTRACTOR_HIP on float_vif_hip behind a Meson
# option (default OFF) pending T7-10c picture-pool plumbing. Without this flag,
# pictures arrive as CPU VmafPictures and float_vif_hip performs explicit HtoD
# copies rather than zero-copy from the HIP picture pool.
if get_option('enable_float_vif_hip_autodispatch')
    vmaf_cflags_common += '-DFLOAT_VIF_HIP_AUTODISPATCH'
endif
if is_hip_enabled
    cdata.set10('HAVE_HIP', true)
    subdir('hip')
else
    # ADR-0212 / libvmaf_hip.h contract: every public HIP entry point
    # returns -ENOSYS when libvmaf is built without HIP support. Without
    # this stub TU the symbols are not emitted into libvmaf.so at all
    # (the real bodies in hip/common.c + libvmaf.c sit behind
    # `#ifdef HAVE_HIP`) and downstream consumers fail at link time.
    # Mirrors the `core/src/dnn/dnn_api.c` `VMAF_HAVE_DNN` stub pattern.
    hip_sources += files('hip/stubs.c')
endif

# HIP HSACO device-kernel pipeline (ADR-0372 / T7-10b batch-1).
# Mirrors the CUDA `enable_nvcc` / `ptx_arrays` pipeline above.
# Gated on `enable_hipcc` (requires a ROCm 6+ toolchain with `hipcc`
# in PATH). Without `enable_hipcc`, `hip_hsaco_sources` stays empty
# and the host TUs fall back to the -ENOSYS scaffold path via
# `#ifndef HAVE_HIPCC`. `HAVE_HIPCC` is propagated to feature TUs
# via `vmaf_cflags_common` below alongside the CUDA / SYCL analogues.
#
# Architecture discovery: `hipconfig --amdgpu-target` returns a
# comma-separated list of installed GFX targets (e.g. `gfx90a,gfx1100`).
# We compile one HSACO fat binary covering all installed targets by
# passing each as a separate `--offload-arch` flag. Falls back to a
# multi-target list covering the commonly-deployed CDNA2 / RDNA2 /
# RDNA3 GPUs (`gfx90a`, `gfx1030`, `gfx1036`, `gfx1100`) when both
# `rocm_agent_enumerator` and `hipconfig` come back empty (the typical
# no-GPU build-sandbox case — BuildKit, CI). See
# ADR-0546 (audit-cleanup-bundle): a previous `gfx90a`-only fallback
# shipped HSACO blobs that failed at runtime on dev hosts with
# consumer iGPUs such as the AMD Raphael APU `gfx1036`
# (override-mapped to `gfx1030` via `HSA_OVERRIDE_GFX_VERSION=10.3.0`).
if is_hipcc_enabled and is_hip_enabled
    cdata.set10('HAVE_HIPCC', true)
    vmaf_cflags_common += '-DHAVE_HIPCC'
    hipcc_exe = find_program('hipcc', required: true)
    xxd_hip = find_program('xxd', required: true)

    # Discover installed GFX targets. Priority:
    #   1. User-supplied hip_gfx_targets meson option (explicit override).
    #   2. rocm_agent_enumerator — filters to GPU lines starting with "gfx",
    #      comma-joined. More reliable than hipconfig on ROCm 6+/7.
    #   3. hipconfig --amdgpu-target — only when (1) and (2) fail, because
    #      some ROCm builds prefix the output with version/system headers.
    #   4. Hardcoded gfx90a fallback (as before).
    hip_gfx_raw = get_option('hip_gfx_targets').strip()
    if hip_gfx_raw == ''
        agent_enum_result = run_command('sh', '-c',
            'rocm_agent_enumerator 2>/dev/null | grep -E "^gfx" | tr "\n" "," | sed "s/,$//g" || true',
            check: false)
        hip_gfx_raw = agent_enum_result.stdout().strip()
    endif
    if hip_gfx_raw == ''
        hipconfig_result = run_command('sh', '-c',
            'hipconfig --amdgpu-target 2>/dev/null | grep -oE "gfx[0-9a-f]+" | tr "\n" "," | sed "s/,$//g" || true',
            check: false)
        hip_gfx_raw = hipconfig_result.stdout().strip()
    endif
    if hip_gfx_raw == ''
        # No GPU visible (no-GPU build sandbox) AND no operator override:
        # widen the fat-binary to cover the four most common targets so
        # the resulting .so works on CDNA2 servers, RDNA2/3 desktops, and
        # the AMD Raphael APU iGPU (`gfx1036`, override-mapped to
        # `gfx1030` via `HSA_OVERRIDE_GFX_VERSION=10.3.0`) used as one of
        # this fork's dev hosts. Operators that want a single-target
        # build for size or speed should pass `-Dhip_gfx_targets=<list>`
        # explicitly. See ADR-0546.
        hip_gfx_raw = 'gfx90a,gfx1030,gfx1036,gfx1100'
    endif
    # Build --offload-arch flag list from comma-separated target string.
    hip_offload_arch_flags = []
    foreach target : hip_gfx_raw.split(',')
        t = target.strip()
        if t != ''
            hip_offload_arch_flags += ['--offload-arch=' + t]
        endif
    endforeach
    if hip_offload_arch_flags.length() == 0
        hip_offload_arch_flags = ['--offload-arch=gfx90a', '--offload-arch=gfx1030',
                                  '--offload-arch=gfx1036', '--offload-arch=gfx1100']
    endif
    message('HIP HSACO targets: ' + ' '.join(hip_offload_arch_flags))

    # Resolve the ROCm include root so hipcc can find <hip/hip_runtime.h>.
    # hipcc normally injects its own ROCM_PATH-based include, but when it
    # is invoked via a symlink or without a package-manager install the
    # auto-detection sometimes fails. Pass -I explicitly as a belt-and-
    # suspenders fix (harmless when hipcc already finds the headers).
    rocm_inc_result = run_command('sh', '-c',
        'hipconfig -R 2>/dev/null || echo /opt/rocm', check: false)
    rocm_root = rocm_inc_result.stdout().strip()
    if rocm_root == ''
        rocm_root = '/opt/rocm'
    endif
    hip_include_flags = ['-I', rocm_root + '/include']
    message('HIP ROCm root: ' + rocm_root)

    # Kernel source → HSACO → xxd-embedded C array.
    # Batch-1 (ADR-0372): psnr_score. float_ansnr_score removed (ADR-0720: legacy metric drop).
    # Batch-1b (ADR-0254): float_psnr_score (pre-existing, omitted from earlier
    #   meson.build edit; added here alongside the float_motion fix).
    # Batch-2 (ADR-0373): float_motion_score (temporal Gaussian SAD).
    # Batch-3 (ADR-0375): moment_score, ssim_score.
    # Batch-4 (ADR-0377): ciede_score (CIEDE2000), motion_v2_score (integer SAD ping-pong).
    # Batch-5: ssimulacra2_blur, ssimulacra2_mul (IIR blur + elementwise mul).
    hip_kernel_sources = {
        'psnr_score' : feature_src_dir + 'hip/integer_psnr/psnr_score.hip',
        'float_psnr_score' : feature_src_dir + 'hip/float_psnr/float_psnr_score.hip',
        'float_motion_score' : feature_src_dir + 'hip/float_motion/float_motion_score.hip',
        'moment_score' : feature_src_dir + 'hip/float_moment/moment_score.hip',
        'ssim_score' : feature_src_dir + 'hip/float_ssim/ssim_score.hip',
        'ciede_score' : feature_src_dir + 'hip/integer_ciede/ciede_score.hip',
        'motion_v2_score' : feature_src_dir + 'hip/integer_motion_v2/motion_v2_score.hip',
        # ADR-0523 follow-up: integer_motion_hip.c loads `motion_score_hsaco`
        # but the .hip kernel was never wired into this dict, so the
        # `extern const unsigned char motion_score_hsaco[]` in
        # `integer_motion_hip.h` resolved to an undefined link reference
        # whenever `enable_hipcc=true` was paired with `enable_hip=true`
        # (i.e. the dev-MCP container's default build mode). The kernel
        # source has lived at `libvmaf/src/feature/hip/integer_motion/
        # motion_score.hip` since ADR-0523 added the registration; this
        # entry closes the loop. Surfaced during ADR-0529 container
        # rebuild verification.
        'motion_score' : feature_src_dir + 'hip/integer_motion/motion_score.hip',
        # T7-10b batch-2 twelfth consumer: float_adm_score (ADR-0468).
        'float_adm_score' : feature_src_dir + 'hip/float_adm/float_adm_score.hip',
        # integer_cambi_hip HSACO blob.
        'cambi_score' : feature_src_dir + 'hip/integer_cambi/cambi_score.hip',
        # integer_vif_hip HSACO (8 kernels: 4 V + 4 H per scale).
        'vif_statistics' : feature_src_dir + 'hip/integer_vif/vif_statistics.hip',
        # ADR-0537: register the HSACO blobs for the remaining HIP extractors
        # that ADR-0533 wired into the dispatch table.  Without these the
        # `enable_hipcc=true` build fails to link `_hsaco` symbols referenced
        # under `#ifdef HAVE_HIPCC` in their host TUs.
        'ms_ssim_score' : feature_src_dir + 'hip/integer_ms_ssim/ms_ssim_score.hip',
        'psnr_hvs_score' : feature_src_dir + 'hip/integer_psnr_hvs/psnr_hvs_score.hip',
        # integer_moment HIP kernel — keyed `integer_moment_score` to emit
        # `integer_moment_score_hsaco` (consumed by integer_moment_hip.c),
        # distinct from the `moment_score` key above which serves
        # float_moment via `hip/float_moment/moment_score.hip`.
        'integer_moment_score' : feature_src_dir + 'hip/integer_moment/moment_score.hip',
        'integer_ssim_score' : feature_src_dir + 'hip/integer_ssim/integer_ssim_score.hip',
        'float_vif_score' : feature_src_dir + 'hip/float_vif/float_vif_score.hip',
        'ssimulacra2_blur' : feature_src_dir + 'hip/ssimulacra2/ssimulacra2_blur.hip',
        'ssimulacra2_mul' : feature_src_dir + 'hip/ssimulacra2/ssimulacra2_mul.hip',
        # integer_adm_hip ADM kernels (ADR-0539) — the 4 .hip files port the
        # CUDA twins under cuda/integer_adm/ to HIP. The CUDA twin's
        # `warp_reduce` (warpSize=32 `__shfl_down_sync`) is replaced by a
        # per-thread `atomicAdd` into the int64/uint64 accumulator on AMD
        # (associative, bit-exact). The xxd-embedded `_hsaco` blobs from
        # these targets define the strong symbols that override the weak
        # fallbacks in `hip_hsaco_stubs.c`.
        'adm_dwt2'        : feature_src_dir + 'hip/integer_adm/adm_dwt2.hip',
        'adm_csf'         : feature_src_dir + 'hip/integer_adm/adm_csf.hip',
        'adm_csf_den'     : feature_src_dir + 'hip/integer_adm/adm_csf_den.hip',
        'adm_cm'          : feature_src_dir + 'hip/integer_adm/adm_cm.hip',
        # ADR-0852: wire speed_chroma_hip + speed_temporal_hip (ADR-0567 HIP)
        # into the HSACO build.  The kernel source was already written and
        # committed; this entry causes hipcc --genco to compile it into an
        # HSACO blob that hipModuleLoadData loads at runtime.
        'speed_score'     : feature_src_dir + 'hip/speed/speed_score.hip',
    }
    # Per-kernel extra hipcc flags. Mirrors `cuda_cu_extra_flags` below for
    # the nvcc path. `ssimulacra2_blur` needs `-ffp-contract=off` so the
    # recursive Gaussian IIR pole-tracking pre/prev sequence keeps IEEE-754
    # add/mul ordering (hipcc / amdclang++ default to `-ffp-contract=fast`
    # on the device side, which auto-fuses the n*sum - d*prev pair into an
    # FMA and shifts the recursion past places=2 vs the CPU / Vulkan
    # `precise` twin). The CUDA twin passes `--fmad=false` for the same
    # reason; HIP's amdclang++-driven hipcc honours `-ffp-contract=off`.
    hip_cu_extra_flags = {
        'ssimulacra2_blur' : ['-ffp-contract=off'],
    }
    foreach name, src : hip_kernel_sources
        per_kernel_flags = hip_cu_extra_flags.get(name, [])
        hsaco_target = custom_target('hip_hsaco_' + name,
            build_by_default: true,
            output: name + '.hsaco',
            input: src,
            command: [hipcc_exe, '--genco'] + hip_offload_arch_flags +
                     hip_include_flags + per_kernel_flags +
                     ['-I', meson.current_source_dir(),
                      '-I', meson.current_source_dir() + '/feature',
                      '-I', meson.current_source_dir() + '/feature/hip',
                      '-I', meson.current_source_dir() + '/hip',
                      '-I', meson.current_build_dir(),
                      '@INPUT@', '-o', '@OUTPUT@'],
        )
        hsaco_c_target = custom_target('hip_hsaco_c_' + name,
            build_by_default: true,
            output: name + '_hsaco.c',
            input: hsaco_target,
            capture: true,
            command: [xxd_hip, '-i', '-n', name + '_hsaco', '@INPUT@'],
        )
        hip_hsaco_sources += hsaco_c_target
    endforeach
endif

# Metal (Apple Silicon) compute backend (ADR-0338 / T8-1 scaffold). Same
# pattern as the HIP / Vulkan scaffolds above — empty arrays when the
# option resolves to disabled (every non-macOS host), no-cost on default
# builds. The dependency() probes inside `subdir('metal')` are
# `required: false`; the scaffold compiles with no Metal SDK linkage on
# any host, including Linux / Windows.
metal_sources = []
metal_deps = []
if is_metal_enabled
    cdata.set10('HAVE_METAL', true)
    subdir('metal')
else
    # ADR-0361 / libvmaf_metal.h contract: every public Metal entry point
    # returns -ENOSYS when libvmaf is built without Metal support (or the
    # auto-probe failed on a non-macOS host). Without this stub TU the
    # symbols are not emitted into libvmaf.so at all (the real bodies in
    # metal/common.mm + metal/picture_import.mm + libvmaf.c sit behind
    # `#ifdef HAVE_METAL`) and downstream consumers fail at link time.
    # Mirrors the `core/src/dnn/dnn_api.c` `VMAF_HAVE_DNN` stub pattern.
    metal_sources += files('metal/stubs.c')
endif
if dnn_have_ort
    cdata.set10('HAVE_DNN', true)
    # Propagate VMAF_HAVE_DNN to every libvmaf TU: feature_extractor.c and
    # feature_lpips.c both gate registration/initialisation on it.
    vmaf_cflags_common += dnn_defines
endif

if is_nvtx_enabled
    cdata.set10('HAVE_NVTX', is_nvtx_enabled)
endif
if is_cuda_enabled
    cdata.set10('HAVE_CUDA', is_cuda_enabled)
endif
if is_sycl_enabled
    cdata.set10('HAVE_SYCL', is_sycl_enabled)
endif

if is_asm_enabled
    if host_machine.cpu_family().startswith('arm64') or host_machine.cpu_family().startswith('aarch64')
        cdata.set10('HAVE_ASM', is_asm_enabled)
        cdata.set10('ARCH_AARCH64', host_machine.cpu_family() == 'aarch64' or host_machine.cpu() == 'arm64')
        cdata.set10('ARCH_ARM',     host_machine.cpu_family().startswith('arm') and host_machine.cpu() != 'arm64')

         if host_machine.system() == 'darwin'
            asm_format = 'macho'
        else
            asm_format = 'elf'
        endif

        # T7-38: probe SVE2 intrinsics support. The SVE2 SSIMULACRA 2 TUs
        # require a compiler that accepts `-march=armv9-a+sve2` (or
        # equivalent) and ships `<arm_sve.h>`. Failure leaves HAVE_SVE2
        # unset; the runtime path falls back to NEON via the dispatch
        # table in ssimulacra2.c.
        #
        # Darwin gate: Apple Silicon (M1–M4) is ARMv8.x without SVE2.
        # Recent Apple Clang accepts `-march=armv9-a+sve2` and ships
        # `<arm_sve.h>` so `cc.compiles()` returns true, but the
        # generated codepath would never run (runtime probe in
        # `arm/cpu.c` is `__linux__`-gated, hardware lacks the
        # extension) and the SVE2 SSIMULACRA 2 TU itself fails to
        # build on Apple Clang. Force HAVE_SVE2 off on Darwin.
        if host_machine.system() == 'darwin'
            is_sve2_supported = false
        else
            sve2_test_src = '''
#include <arm_sve.h>
int main(void) {
    svfloat32_t a = svdup_f32(1.0f);
    svbool_t pg = svptrue_b32();
    svfloat32_t b = svadd_f32_x(pg, a, a);
    return (int)svaddv_f32(pg, b);
}
'''
            is_sve2_supported = cc.compiles(sve2_test_src,
                args: test_args + ['-march=armv9-a+sve2'],
                name: 'aarch64 SVE2 intrinsics')
        endif
        cdata.set10('HAVE_SVE2', is_sve2_supported)
    endif

    if host_machine.cpu_family().startswith('x86')
        cdata.set10('HAVE_ASM', is_asm_enabled)
        cdata.set10('ARCH_X86', host_machine.cpu_family().startswith('x86'))
        cdata.set10('ARCH_X86_64', host_machine.cpu_family() == 'x86_64')
        cdata.set10('ARCH_X86_32', host_machine.cpu_family() == 'x86')

        # check NASM version
        nasm = find_program('nasm')
        if nasm.found()
            nasm_r = run_command(nasm, '-v', check: true)

            if nasm_r.returncode() != 0
                error('failed running nasm to obtain its version')
            endif

            out = nasm_r.stdout().strip().split()
            if out[1].to_lower() == 'version'
                if out[2].version_compare('<2.13.02')
                    error('nasm 2.13.02 or later is required, found nasm @0@'.format(out[2]))
                elif out[2].version_compare('<2.14') and is_avx512_enabled
                    warning('nasm 2.14 or later is required for AVX-512 asm. Disabling AVX-512.')
                    is_avx512_supported = false
                else
                    is_avx512_supported = true
                endif
                cdata.set10('HAVE_AVX512', is_avx512_enabled and is_avx512_supported)
                cdata_asm.set10('HAVE_AVX512', is_avx512_enabled and is_avx512_supported)
            else
                error('unexpected nasm version string: @0@'.format(nasm_r.stdout()))
            endif
        endif

        # Generate config.asm
        cdata_asm.set10('ARCH_X86_64', host_machine.cpu_family() == 'x86_64')
        cdata_asm.set10('ARCH_X86_32', host_machine.cpu_family() == 'x86')
        cdata_asm.set10('PIC', true)
        config_asm_target = configure_file(output: 'config.asm', output_format: 'nasm', configuration: cdata_asm)

        if host_machine.system() == 'darwin'
            nasm_format = 'macho'
        elif host_machine.system() == 'windows'
            nasm_format = 'win'
        else
            nasm_format = 'elf'
        endif
        if host_machine.cpu_family() == 'x86_64'
            nasm_format += '64'
        else
            nasm_format += '32'
        endif

        nasm_gen = generator(nasm,
            output: '@BASENAME@.obj',
            depfile: '@BASENAME@.obj.ndep',
            arguments: [
                '-f', nasm_format,
                '-I', '@0@/src/'.format(libvmaf_src_root),
                '-I', '@0@/'.format(meson.current_build_dir()),
                '-MQ', '@OUTPUT@', '-MF', '@DEPFILE@',
                '@EXTRA_ARGS@',
                '@INPUT@',
                '-o', '@OUTPUT@'
            ])
    endif
endif

json_model_c_sources = []
# BRISQUE (ADR-1115) ships its LIVE-trained libsvm EPSILON_SVR "allmodel" the
# same way the VMAF JSON models are baked in: an `xxd -i` custom_target embeds
# model/other_models/brisque_live.model into a build-dir-only C TU. The 343 KB
# text model expands to ~2.1 MB of C, which is why it is NEVER committed (the
# old committed header tripped the 1 MB check-added-large-files gate). The
# generated symbols src_brisque_live_model[] / _len are declared in
# feature/brisque_model.h and consumed by feature/brisque.c.
brisque_model_c_sources = []
built_in_models_enabled = get_option('built_in_models') == true
float_enabled = get_option('enable_float') == true
cdata.set10('VMAF_FLOAT_FEATURES', float_enabled)
# Default: BRISQUE embed off. Flipped to 1 below only when the model embed
# custom_target actually runs (built_in_models AND xxd supports -n). brisque.c
# uses this to choose between the built-in buffer and the runtime model_path.
cdata.set10('VMAF_BRISQUE_BUILT_IN_MODEL', false)

if built_in_models_enabled
    xxd = find_program('xxd', required: false)

    if xxd.found()
        xdd_supports_n = run_command(xxd, '-n', 'test', check: false).returncode() == 0
        if meson.is_subproject () and not xdd_supports_n
            error('xxd with support for -n option is required building as a subproject. Please install a newer version of xxd or disable built-in models.')
        endif

        model_files = [
            'vmaf_v0.6.1.json',
            'vmaf_b_v0.6.3.json',
            'vmaf_v0.6.1neg.json',
            'vmaf_4k_v0.6.1.json',
            'vmaf_4k_v0.6.1neg.json',
        ]

        if float_enabled
            model_files += [
              'vmaf_float_v0.6.1neg.json',
              'vmaf_float_v0.6.1.json',
              'vmaf_float_b_v0.6.3.json',
              'vmaf_float_4k_v0.6.1.json',
            ]
        endif

        foreach model_file : model_files
            if xdd_supports_n
                command = [xxd, '-i', '-n', 'src_@PLAINNAME@', '@INPUT@', '@OUTPUT@']
            else
                command = [xxd, '-i', '@INPUT@', '@OUTPUT@']
            endif
            json_model_c_sources += custom_target(
                  model_file,
                  output : '@PLAINNAME@.c',
                  input : configure_file(
                    input : model_dir + model_file,
                    output : model_file,
                    copy: true
                  ),
                  command : command,
            )
        endforeach

        # VMAF v1.0.16 SDR models (ported from Netflix upstream 4718b4f5f).
        # These live in their own subdirectories under model/, so they use a
        # dedicated dir prefix; the xxd embed pipeline is identical to the v0
        # models above. The generated symbols src_vmaf_v1_0_16_*_json[] / _len
        # are declared in model.c.
        model_v1_0_16_dir = '../../model/vmaf_v1.0.16/'
        model_v1_0_16_files = [
            'vmaf_v1.0.16_3d0h.json',
            'vmaf_v1.0.16_3d0h_2160.json',
            'vmaf_v1.0.16_5d0h.json',
            'vmaf_v1.0.16_1d5h_2160.json',
        ]
        foreach model_file : model_v1_0_16_files
            if xdd_supports_n
                command = [xxd, '-i', '-n', 'src_@PLAINNAME@', '@INPUT@', '@OUTPUT@']
            else
                command = [xxd, '-i', '@INPUT@', '@OUTPUT@']
            endif
            json_model_c_sources += custom_target(
                  model_file,
                  output : '@PLAINNAME@.c',
                  input : configure_file(
                    input : model_v1_0_16_dir + model_file,
                    output : model_file,
                    copy: true
                  ),
                  command : command,
            )
        endforeach

        model_v1_0_16_hfr_dir = '../../model/vmaf_v1.0.16_hfr/'
        model_v1_0_16_hfr_files = [
            'vmaf_v1.0.16_hfr_3d0h.json',
            'vmaf_v1.0.16_hfr_3d0h_2160.json',
            'vmaf_v1.0.16_hfr_5d0h.json',
            'vmaf_v1.0.16_hfr_1d5h_2160.json',
        ]
        foreach model_file : model_v1_0_16_hfr_files
            if xdd_supports_n
                command = [xxd, '-i', '-n', 'src_@PLAINNAME@', '@INPUT@', '@OUTPUT@']
            else
                command = [xxd, '-i', '@INPUT@', '@OUTPUT@']
            endif
            json_model_c_sources += custom_target(
                  model_file,
                  output : '@PLAINNAME@.c',
                  input : configure_file(
                    input : model_v1_0_16_hfr_dir + model_file,
                    output : model_file,
                    copy: true
                  ),
                  command : command,
            )
        endforeach

        # BRISQUE allmodel embed (ADR-1115). Same xxd pipeline as the JSON
        # models above; the model carries no .json extension so it is handled
        # separately. xxd's -n sanitiser maps "src_brisque_live.model" to the
        # C identifier src_brisque_live_model (dots -> underscores), matching
        # the externs declared in feature/brisque_model.h. Requires -n support
        # (no plain-xxd fallback): the symbol name is load-bearing, so when
        # xxd lacks -n we skip the embed and brisque.c relies on the runtime
        # `model_path` option instead.
        if xdd_supports_n
            brisque_model_file = 'other_models/brisque_live.model'
            brisque_model_c_sources += custom_target(
                  'brisque_live_model',
                  output : 'brisque_live.model.c',
                  input : configure_file(
                    input : model_dir + brisque_model_file,
                    output : 'brisque_live.model',
                    copy: true
                  ),
                  command : [xxd, '-i', '-n', 'src_@PLAINNAME@', '@INPUT@', '@OUTPUT@'],
            )
            cdata.set10('VMAF_BRISQUE_BUILT_IN_MODEL', true)
        endif

        cdata.set10('VMAF_BUILT_IN_MODELS', built_in_models_enabled)
    endif
endif

config_h_target = configure_file(output: 'config.h', configuration: cdata)

libvmaf_cpu_sources = [
    src_dir + 'cpu.cpp',
]

if is_asm_enabled
    if host_machine.cpu_family().startswith('x86')
        # NASM source files
        libvmaf_sources_asm = files(
            'x86/cpuid.asm',
        )
        libvmaf_nasm_objects = nasm_gen.process(libvmaf_sources_asm)
        libvmaf_cpu_sources += libvmaf_nasm_objects

        libvmaf_cpu_sources += files(
            src_dir + 'x86/cpu.c',
        )
    endif

    if host_machine.cpu_family().startswith('arm64') or host_machine.cpu_family().startswith('aarch64')
        libvmaf_cpu_sources += files(
            src_dir + 'arm/cpu.c',
        )
    endif
endif

libvmaf_include = include_directories(
    '.',
    '../include',
    src_dir,
    feature_src_dir,
    feature_src_dir + 'common',
    is_nvtx_enabled ? '/usr/local/cuda/include' : '',
)

# GCC/Clang accept `c++23`; MSVC needs `vc++latest`; Intel oneAPI on Windows
# (icx-cl, `intel-llvm-cl`) and clang-cl reject `c++23` and need plain
# `c++latest`. Pick the right token per compiler so override_options
# doesn't fail meson configure.
_cpp_id = meson.get_compiler('cpp').get_id()
if _cpp_id == 'msvc'
    libvmaf_cpu_cpp_std = 'vc++latest'
elif _cpp_id == 'intel-llvm-cl' or _cpp_id == 'intel-cl' or _cpp_id == 'clang-cl'
    libvmaf_cpu_cpp_std = 'c++latest'
else
    libvmaf_cpu_cpp_std = 'c++23'
endif

libvmaf_cpu_static_lib = static_library(
    'libvmaf_cpu',
    libvmaf_cpu_sources,
    include_directories : [libvmaf_include],
    # ADR-0755 / ADR-0860: cpu.cpp uses constinit (C++20) and std::atomic, so
    # this TU needs the compiler-aware cpp_std token; the plain 'c++23' spelling
    # is rejected by MSVC / clang-cl / icx-cl.
    override_options : ['cpp_std=' + libvmaf_cpu_cpp_std],
)

platform_specific_cpu_objects = []

if is_asm_enabled
    if host_machine.cpu_family().startswith('arm64') or host_machine.cpu_family().startswith('aarch64')
        # Integer-only NEON TUs — no floating-point arithmetic; no
        # `-ffp-contract=off` needed.  ADR-0873.
        arm64_int_sources = [
          feature_src_dir + 'arm64/vif_neon.c',
          feature_src_dir + 'arm64/adm_neon.c',
          feature_src_dir + 'arm64/psnr_neon.c',
          # ssim_neon.c lives in its own carve-out below — keep out
          # of this list. See the arm64_ssim_neon_lib block.
          feature_src_dir + 'arm64/motion_neon.c',
          feature_src_dir + 'arm64/cambi_neon.c',
        ]

        arm64_static_lib = static_library(
          'arm64_v8',
          arm64_int_sources,
          include_directories : vmaf_base_include,
          c_args : vmaf_cflags_common + ['-DARCH_AARCH64']
        )

        # Float-arithmetic NEON TUs compiled with `-ffp-contract=off`.
        # Rationale: GCC 10+ / clang default to `-ffp-contract=fast` on
        # aarch64, which auto-fuses plain C `a*b+c` expressions (and the
        # scalar tail of NEON loops) into `fmla` instructions.  NEON
        # intrinsics are opaque to the fusion pass, so without this flag
        # the scalar tail and the NEON main loop can produce different
        # results from the scalar reference (~1 ulp per fused op).
        # ADR-0873.  Same rationale as x86 `x86_avx2_ffp` carve-out.
        arm64_fp_sources = [
          feature_src_dir + 'arm64/float_psnr_neon.c',
          feature_src_dir + 'arm64/float_motion_neon.c',
          feature_src_dir + 'arm64/float_adm_neon.c',
          feature_src_dir + 'arm64/ciede_neon.c',
          feature_src_dir + 'arm64/ms_ssim_decimate_neon.c',
          feature_src_dir + 'arm64/convolve_neon.c',
          feature_src_dir + 'arm64/motion_v2_neon.c',
          feature_src_dir + 'arm64/psnr_hvs_neon.c',
          feature_src_dir + 'arm64/moment_neon.c',
        ]

        arm64_fp_lib = static_library(
          'arm64_v8_fp',
          arm64_fp_sources,
          include_directories : vmaf_base_include,
          c_args : vmaf_cflags_common + ['-DARCH_AARCH64', '-ffp-contract=off'],
        )

        # float-ADM DWT2 NEON carve-out (ADR-1057).
        # `adm_dwt2_s` has a function-scoped non-contracting guard so unrelated
        # adm_tools.c reductions retain their established compiler semantics.
        # This NEON TU mirrors DWT2 with explicit `vmul` + `vadd` operations;
        # `-ffp-contract=off` also keeps its scalar tail and horizontal pass
        # split on every supported AArch64 compiler.
        arm64_adm_dwt2_neon_lib = static_library(
          'arm64_adm_dwt2_neon',
          feature_src_dir + 'arm64/float_adm_dwt2_neon.c',
          include_directories : vmaf_base_include,
          c_args : vmaf_cflags_common + ['-DARCH_AARCH64', '-ffp-contract=off'],
        )

        # ssim_neon carve-out (mirror of ssimulacra2_neon and x86 ssim_avx2).
        # Rationale: ssim_accumulate_neon at line ~105 contains the
        # `vaddq_f32(vaddq_f32(vmulq_f32(rm,rm), vmulq_f32(cm,cm)), vC1)`
        # three-operand pattern. aarch64 GCC/Clang default
        # `-ffp-contract=fast` fuses the outer add+mul into VFMADD,
        # while the scalar reference in ssim_tools.c uses plain
        # mul+add. Identified by simd-reviewer agent audit 2026-05-30
        # (HIGH severity, no parity test exists yet).
        arm64_ssim_neon_lib = static_library(
          'arm64_ssim_neon',
          feature_src_dir + 'arm64/ssim_neon.c',
          include_directories : vmaf_base_include,
          c_args : vmaf_cflags_common + ['-DARCH_AARCH64', '-ffp-contract=off'],
        )

        # ssimulacra2_neon.c compiles in its own static lib with
        # `-ffp-contract=off`. Rationale mirrors arm64_v8_fp above
        # (ADR-0161, ADR-0873).
        arm64_ssimulacra2_lib = static_library(
          'arm64_ssimulacra2',
          [
              feature_src_dir + 'arm64/ssimulacra2_neon.c',
              # ADR-0242: host-kernel variants (plane_stride form for Vulkan pyramid)
              feature_src_dir + 'arm64/ssimulacra2_host_neon.c',
          ],
          include_directories : vmaf_base_include,
          c_args : vmaf_cflags_common + ['-DARCH_AARCH64', '-ffp-contract=off'],
        )

        platform_specific_cpu_objects += arm64_static_lib.extract_all_objects(recursive: false)
        platform_specific_cpu_objects += arm64_fp_lib.extract_all_objects(recursive: false)
        platform_specific_cpu_objects += arm64_adm_dwt2_neon_lib.extract_all_objects(recursive: false)
        platform_specific_cpu_objects += arm64_ssimulacra2_lib.extract_all_objects(recursive: false)
        platform_specific_cpu_objects += arm64_ssim_neon_lib.extract_all_objects(recursive: false)

        # T7-38: SSIMULACRA 2 SVE2 sister TU. Only built when the
        # compiler accepts `-march=armv9-a+sve2`; gated by HAVE_SVE2.
        # `-ffp-contract=off` rationale mirrors the NEON sibling: SVE2
        # intrinsics are opaque to the contraction pass, but the per-
        # lane scalar tail must not auto-fuse `a*b+c` into `fmla`. Same
        # ADR-0161 bit-exact contract.
        if is_sve2_supported
            arm64_ssimulacra2_sve2_lib = static_library(
                'arm64_ssimulacra2_sve2',
                feature_src_dir + 'arm64/ssimulacra2_sve2.c',
                include_directories : vmaf_base_include,
                c_args : vmaf_cflags_common + ['-DARCH_AARCH64', '-march=armv9-a+sve2', '-ffp-contract=off'],
            )
            platform_specific_cpu_objects += arm64_ssimulacra2_sve2_lib.extract_all_objects(recursive: false)

            # ADR-0584: float_moment SVE2 sister TU. VLA f32→f64 widening
            # avoids the fixed 4-lane bottleneck of the NEON path on
            # Neoverse V2 / Cortex-X4 and later hardware.
            # `-ffp-contract=off` mirrors the NEON sibling (arm64_v8_fp)
            # so scalar tails cannot fuse `a*b+c` into fmla.
            arm64_moment_sve2_lib = static_library(
                'arm64_moment_sve2',
                feature_src_dir + 'arm64/moment_sve2.c',
                include_directories : vmaf_base_include,
                c_args : vmaf_cflags_common + ['-DARCH_AARCH64', '-march=armv9-a+sve2', '-ffp-contract=off'],
            )
            platform_specific_cpu_objects += arm64_moment_sve2_lib.extract_all_objects(recursive: false)
        endif
    endif

    if host_machine.cpu_family().startswith('x86')
      # icx (Intel oneAPI DPC++/C++, meson id 'intel-llvm' or 'intel-llvm-cl')
      # ignores #pragma STDC FP_CONTRACT OFF in source files at -O3 unless
      # -fp-model=precise is also passed on the command line. For strict-FP
      # carve-out static libs we add it alongside -ffp-contract=off so that
      # both scalar tails and any C-level glue inside the SIMD TUs stay
      # bit-exact with the scalar reference even under the icx optimizer.
      # GCC and vanilla Clang do not recognise -fp-model=precise; we detect
      # icx via cc.get_id() and append the flag only for that compiler.
      _cc_id_x86 = cc.get_id()
      _x86_simd_strict_fp_extra = []
      if _cc_id_x86 == 'intel-llvm' or _cc_id_x86 == 'intel-llvm-cl'
          _x86_simd_strict_fp_extra = ['-fp-model=precise']
      endif
      x86_avx2_sources = [
          feature_src_dir + 'common/convolution_avx.c',
          feature_src_dir + 'x86/motion_avx2.c',
          feature_src_dir + 'x86/vif_avx2.c',
          feature_src_dir + 'x86/vif_statistic_avx2.c',
          feature_src_dir + 'x86/adm_avx2.c',
          feature_src_dir + 'x86/cambi_avx2.c',
          feature_src_dir + 'x86/speed_avx2.c',
          feature_src_dir + 'x86/psnr_avx2.c',
          # ssim_avx2.c lives in its own carve-out below — keep out of
          # this list. See the x86_ssim_avx2_lib block.
          feature_src_dir + 'x86/float_psnr_avx2.c',
          feature_src_dir + 'x86/float_motion_avx2.c',
          feature_src_dir + 'x86/ciede_avx2.c',
          feature_src_dir + 'x86/convolve_avx2.c',
          feature_src_dir + 'x86/moment_avx2.c',
          feature_src_dir + 'x86/integer_ssim_avx2.c',
      ]

      x86_avx2_static_lib = static_library(
          'x86_avx2',
          x86_avx2_sources,
          include_directories : vmaf_base_include,
          c_args : ['-mavx', '-mavx2', '-mfma'] + vmaf_cflags_common,
      )

      # ssim_avx2 carve-out (mirror of psnr_hvs_avx2 / ms_ssim_decimate_avx2
      # carve-outs above). Rationale: ssim_accumulate_avx2 contains the
      # three-operand `add(add(mul(rm,rm), mul(cm,cm)), vC1)` pattern at
      # line ~106 that GCC/Clang silently contract into VFMADD under
      # -mfma. The scalar reference in ssim_tools.c is built without
      # -mfma so the two paths diverge. Identified by simd-reviewer agent
      # audit 2026-05-30 (HIGH severity, no parity test exists yet).
      x86_ssim_avx2_lib = static_library(
          'x86_ssim_avx2',
          feature_src_dir + 'x86/ssim_avx2.c',
          include_directories : vmaf_base_include,
          c_args : ['-mavx', '-mavx2', '-mfma', '-ffp-contract=off'] + _x86_simd_strict_fp_extra + vmaf_cflags_common,
      )


      # psnr_hvs_avx2 + ms_ssim_decimate_avx2 carve-outs (mirror of
      # ssimulacra2 below). Reason: under -mfma + GCC silently
      # ignoring `#pragma STDC FP_CONTRACT OFF`, plain C `a*b+c`
      # in scalar tails / index math auto-contracts to FMA and
      # diverges from the scalar reference (which is built without
      # -mfma). Causes test_psnr_hvs_simd / test_ms_ssim_decimate
      # to fail with sub-ULP rel-tol drift.
      x86_psnr_hvs_avx2_lib = static_library(
          'x86_psnr_hvs_avx2',
          feature_src_dir + 'x86/psnr_hvs_avx2.c',
          include_directories : vmaf_base_include,
          c_args : ['-mavx', '-mavx2', '-mfma', '-ffp-contract=off'] + _x86_simd_strict_fp_extra + vmaf_cflags_common,
      )
      x86_ms_ssim_decimate_avx2_lib = static_library(
          'x86_ms_ssim_decimate_avx2',
          feature_src_dir + 'x86/ms_ssim_decimate_avx2.c',
          include_directories : vmaf_base_include,
          c_args : ['-mavx', '-mavx2', '-mfma', '-ffp-contract=off'] + _x86_simd_strict_fp_extra + vmaf_cflags_common,
      )

      # ssimulacra2 AVX2 compiles in its own static lib with
      # `-ffp-contract=off`. Rationale (mirror of arm64_ssimulacra2_lib
      # carve-out): GCC 10+ / clang on x86 with `-mfma` auto-fuse plain
      # C `a*b+c` in scalar tails + per-lane `cbrtf`/`powf` libm paths,
      # yielding host-dependent drift. Scoped to ssimulacra2 only so
      # prior AVX2 TUs (psnr_hvs, motion_v2, etc.) keep their
      # established bit-exact behaviour.
      x86_ssimulacra2_avx2_lib = static_library(
          'x86_ssimulacra2_avx2',
          [
              feature_src_dir + 'x86/ssimulacra2_avx2.c',
              # ADR-0242: host-kernel variants (plane_stride form for Vulkan pyramid)
              feature_src_dir + 'x86/ssimulacra2_host_avx2.c',
          ],
          include_directories : vmaf_base_include,
          c_args : ['-mavx', '-mavx2', '-mfma', '-ffp-contract=off'] + _x86_simd_strict_fp_extra + vmaf_cflags_common,
      )

      platform_specific_cpu_objects += x86_avx2_static_lib.extract_all_objects(recursive: true)
      platform_specific_cpu_objects += x86_ssimulacra2_avx2_lib.extract_all_objects(recursive: true)
      platform_specific_cpu_objects += x86_psnr_hvs_avx2_lib.extract_all_objects(recursive: true)
      platform_specific_cpu_objects += x86_ms_ssim_decimate_avx2_lib.extract_all_objects(recursive: true)
      platform_specific_cpu_objects += x86_ssim_avx2_lib.extract_all_objects(recursive: true)

      # float_adm AVX2 compiles in its own static lib with
      # `-ffp-contract=off`.  Rationale (F3 fix, PR #116): the DWT2 vertical
      # pass uses mul+add chains; with `-mfma` the compiler may auto-fuse
      # these into FMAs, diverging from the scalar reference which uses plain
      # C mul+add.  Disabling FP contraction keeps both paths bit-identical.
      # The scalar `adm_sum_cube_s` / `adm_csf_den_scale_s` references
      # compile without -mfma so they never see auto-fusion.
      x86_float_adm_avx2_lib = static_library(
          'x86_float_adm_avx2',
          feature_src_dir + 'x86/float_adm_avx2.c',
          include_directories : vmaf_base_include,
          c_args : ['-mavx', '-mavx2', '-mfma', '-ffp-contract=off'] + _x86_simd_strict_fp_extra + vmaf_cflags_common,
      )
      platform_specific_cpu_objects += x86_float_adm_avx2_lib.extract_all_objects(recursive: true)

      if is_avx512_enabled and is_avx512_supported
        x86_avx512_sources = [
            feature_src_dir + 'x86/motion_avx512.c',
            feature_src_dir + 'x86/vif_avx512.c',
            feature_src_dir + 'x86/psnr_avx512.c',
            feature_src_dir + 'x86/ssim_avx512.c',
            feature_src_dir + 'x86/adm_avx512.c',
            feature_src_dir + 'x86/cambi_avx512.c',
            feature_src_dir + 'x86/speed_avx512.c',
            feature_src_dir + 'x86/float_psnr_avx512.c',
            feature_src_dir + 'x86/float_motion_avx512.c',
            feature_src_dir + 'x86/ciede_avx512.c',
            feature_src_dir + 'x86/ms_ssim_decimate_avx512.c',
            feature_src_dir + 'x86/convolve_avx512.c',
            feature_src_dir + 'x86/moment_avx512.c',
            feature_src_dir + 'common/convolution_avx512.c',
        ]

        x86_avx512_static_lib = static_library(
            'x86_avx512',
            x86_avx512_sources,
            include_directories : vmaf_base_include,
            c_args : ['-mavx512f', '-mavx512dq', '-mavx512bw', '-mavx512cd',
                      '-mavx512vbmi', '-mavx512vl'] +
                     vmaf_cflags_common,
            # b_lto=false: with b_lto=true at meson.build top, LTO over
            # this lib drops the convolution_f32_avx512_{s,sq_s,xy_s}
            # symbols because vif_filter1d_s in vif_tools.c reaches them
            # only via a runtime CPU-flag branch the linker treats as
            # potentially-dead. Disable LTO so the public AVX-512 symbols
            # stay externally visible. (Fixes pre-existing LTO link
            # failure under the b_lto=true global default.)
            override_options : ['b_lto=false'],
        )

        # ssimulacra2 AVX-512 — same `-ffp-contract=off` carve-out as
        # the AVX2 sibling. See x86_ssimulacra2_avx2_lib above.
        x86_ssimulacra2_avx512_lib = static_library(
            'x86_ssimulacra2_avx512',
            feature_src_dir + 'x86/ssimulacra2_avx512.c',
            include_directories : vmaf_base_include,
            c_args : ['-mavx512f', '-mavx512dq', '-mavx512bw', '-mavx512cd',
                      '-mavx512vbmi', '-mavx512vl', '-ffp-contract=off'] +
                     _x86_simd_strict_fp_extra + vmaf_cflags_common,
        )

        platform_specific_cpu_objects += x86_avx512_static_lib.extract_all_objects(recursive: true)
        platform_specific_cpu_objects += x86_ssimulacra2_avx512_lib.extract_all_objects(recursive: true)

        # float_adm AVX-512 — same `-ffp-contract=off` carve-out as
        # the AVX2 sibling above (F3 fix, PR #116).
        x86_float_adm_avx512_lib = static_library(
            'x86_float_adm_avx512',
            feature_src_dir + 'x86/float_adm_avx512.c',
            include_directories : vmaf_base_include,
            c_args : ['-mavx512f', '-mavx512dq', '-mavx512bw', '-mavx512cd',
                      '-mavx512vbmi', '-mavx512vl', '-ffp-contract=off'] +
                     _x86_simd_strict_fp_extra + vmaf_cflags_common,
        )
        platform_specific_cpu_objects += x86_float_adm_avx512_lib.extract_all_objects(recursive: true)
      endif

    endif
endif

cuda_dependency = []
common_cuda_objects = []
cuda_inc = []

if is_nvtx_enabled
    cuda_dependency += declare_dependency(
        dependencies: cc.find_library('dl', required: false),
        include_directories: include_directories('/usr/local/cuda/include'))
endif

# ODR fix (follow-up to PR #840): propagate HAVE_CUDA / HAVE_NVTX / HAVE_SYCL
# into vmaf_cflags_common HERE — before any static_library target captures it —
# so that cuda_static_lib (line ~1178), libvmaf_feature_static_lib (line ~1700),
# and every other lib that uses `c_args: vmaf_cflags_common` sees the defines.
# Previously these three lines appeared after the libvmaf sources list (line ~1856),
# which meant every static-lib target built before that point compiled without the
# defines, producing an ODR violation: VmafPicturePrivate had different sizes in
# the allocator TU vs the consumer TU.  Moving the block here (before
# `if is_cuda_enabled`) fixes cuda_static_lib and libvmaf_feature_static_lib.
# gpu_picture_pool_cpp23_lib carries an explicit cpp_args below (it never uses
# vmaf_cflags_common directly).
if is_cuda_enabled
    vmaf_cflags_common += '-DHAVE_CUDA'
endif
if is_nvtx_enabled
    vmaf_cflags_common += '-DHAVE_NVTX'
endif
if is_sycl_enabled
    vmaf_cflags_common += '-DHAVE_SYCL'
endif

if is_cuda_enabled
    # Check for required CUDA headers
    if not cc.has_header('ffnvcodec/dynlink_cuda.h', args : test_args)
        error('ffnvcodec/dynlink_cuda.h not found. Please install nv-codec-headers from https://code.ffmpeg.org/FFmpeg/nv-codec-headers (commit 876af32a202d0de83bd1d36fe74ee0f7fcf86b0d or greater required).')
    endif
    if not cc.has_header('ffnvcodec/dynlink_loader.h', args : test_args)
        error('ffnvcodec/dynlink_loader.h not found. Please install nv-codec-headers from https://code.ffmpeg.org/FFmpeg/nv-codec-headers (commit 876af32a202d0de83bd1d36fe74ee0f7fcf86b0d or greater required).')
    endif

    cuda_cu_sources = {
        'adm_dwt2' : [feature_src_dir + 'cuda/integer_adm/adm_dwt2.cu'],
        'adm_cm' : [feature_src_dir + 'cuda/integer_adm/adm_cm.cu'],
        'adm_csf' : [feature_src_dir + 'cuda/integer_adm/adm_csf.cu'],
        'adm_csf_den' : [feature_src_dir + 'cuda/integer_adm/adm_csf_den.cu'],
        'filter1d' : [feature_src_dir + 'cuda/integer_vif/filter1d.cu'],
        'motion_score' : [feature_src_dir + 'cuda/integer_motion/motion_score.cu'],
        'motion_v2_score' : [feature_src_dir + 'cuda/integer_motion_v2/motion_v2_score.cu'],
        'psnr_score' : [feature_src_dir + 'cuda/integer_psnr/psnr_score.cu'],
        'moment_score' : [feature_src_dir + 'cuda/integer_moment/moment_score.cu'],
        'ciede_score' : [feature_src_dir + 'cuda/integer_ciede/ciede_score.cu'],
        # float_ssim CUDA kernel (11-tap float Gaussian, emits float_ssim).
        'ssim_score' : [feature_src_dir + 'cuda/integer_ssim/ssim_score.cu'],
        # Real integer_ssim CUDA kernel (9-tap int64 moments, ADR-0564).
        'integer_ssim_score' : [feature_src_dir + 'cuda/integer_ssim/integer_ssim_score.cu'],
        'ms_ssim_score' : [feature_src_dir + 'cuda/integer_ms_ssim/ms_ssim_score.cu'],
        'psnr_hvs_score' : [feature_src_dir + 'cuda/integer_psnr_hvs/psnr_hvs_score.cu'],
        'float_psnr_score' : [feature_src_dir + 'cuda/float_psnr/float_psnr_score.cu'],
        'float_motion_score' : [feature_src_dir + 'cuda/float_motion/float_motion_score.cu'],
        'float_vif_score' : [feature_src_dir + 'cuda/float_vif/float_vif_score.cu'],
        'ssimulacra2_blur' : [feature_src_dir + 'cuda/ssimulacra2/ssimulacra2_blur.cu'],
        'ssimulacra2_mul' : [feature_src_dir + 'cuda/ssimulacra2/ssimulacra2_mul.cu'],
        'float_adm_score' : [feature_src_dir + 'cuda/float_adm/float_adm_score.cu'],
        # T3-15 / ADR-0360: cambi CUDA twin (Strategy II hybrid — three integer kernels).
        'cambi_score' : [feature_src_dir + 'cuda/integer_cambi/cambi_score.cu'],
        # ADR-0965: speed_chroma_cuda + speed_temporal_cuda share a single kernel
        # (speed_score.cu) that computes means, covariance, and per-channel
        # scores.  Both C wrappers extern the symbol `speed_score_ptx`; the PTX
        # array is generated here via the standard bin2c pipeline.
        'speed_score' : [feature_src_dir + 'cuda/speed/speed_score.cu'],
    }
    # Per-kernel extra nvcc flags.
    #
    # `ssimulacra2_blur` needs `--fmad=false` so the IIR pole-tracking
    # expression (n2*sum - d1*prev1 - prev2) keeps its CPU-port
    # FMUL/FSUB ordering — without it, NVCC fuses the multiply + subtract
    # pair into FMAs and the recursive Gaussian's per-step rounding
    # compounds across radius * 6-scale pyramid into a places=2 drift
    # versus CPU. The per-pixel multiply kernel does not need it
    # (single FMUL, no fused-add candidate). Mirrors GLSL `precise` /
    # `NoContraction` decorations on ssimulacra2_blur.comp.
    #
    # `float_adm_score` needs `--fmad=false` so the angle-flag and cube
    # reductions in stage 2/3 keep IEEE-754 add/mul ordering — matches
    # the GLSL `precise` qualifier in float_adm.comp. Without this,
    # scale-3 and adm2 drift past places=4 vs CPU adm_decouple_s /
    # adm_cm_s. The integer ADM kernels use int64 accumulators so FMA
    # is irrelevant; default FMA-on flags stay for them.
    cuda_cu_extra_flags = {
        'ssimulacra2_blur' : ['-Xcompiler=-ffp-contract=off', '--fmad=false'],
        'float_adm_score' : ['-Xcompiler=-ffp-contract=off', '--fmad=false'],
    }
    message(cuda_cu_sources)
    # gpu_picture_pool.cpp moved to libvmaf_sources (always-compiled) per
    # ADR-0239 — SYCL + Vulkan also link against it.
    cuda_sources = [
        cuda_dir + 'common.c',
        cuda_dir + 'dispatch_strategy.c',
        cuda_dir + 'drain_batch.c',
        cuda_dir + 'picture_cuda.c',
    ]
    gencode = []
    if get_option('enable_nvcc')
        nvcc_exe = find_program('nvcc')

        # On Windows, nvcc requires MSVC's cl.exe as host compiler for preprocessing.
        # We avoid adding cl.exe to PATH as that causes meson to pick MSVC as the
        # default C compiler. Use vswhere + powershell to find cl.exe automatically.
        if host_machine.system() == 'windows'
            cl_find_result = run_command('powershell', '-NoProfile', '-Command',
                '(Get-ChildItem -Path (& "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe" -latest -products * -property installationPath) -Recurse -Filter cl.exe -ErrorAction SilentlyContinue | Where-Object { $_.DirectoryName -like "*HostX64*x64*" } | Select-Object -First 1).FullName',
                check: false)
            if cl_find_result.returncode() == 0 and cl_find_result.stdout().strip() != ''
                cl_path = cl_find_result.stdout().strip()
                message('Found MSVC cl.exe at: ' + cl_path)
                nvcc_ccbin_flags = ['--allow-unsupported-compiler', '-ccbin', cl_path]
            else
                cl_exe = find_program('cl', required: false)
                if cl_exe.found()
                    nvcc_ccbin_flags = ['--allow-unsupported-compiler', '-ccbin', cl_exe.full_path()]
                else
                    error('MSVC cl.exe not found. nvcc on Windows requires Visual Studio Build Tools.')
                endif
            endif

            # Find MSVC and Windows SDK include dirs so cl.exe can find system
            # headers when invoked by nvcc outside a vcvars environment.
            # Pass them as -I flags directly to nvcc.
            # Derive MSVC include path from cl.exe's location to ensure version match.
            msvc_include_result = run_command('powershell', '-NoProfile', '-Command',
                '$clPath = "' + cl_path + '"; ' +
                '$msvcRoot = ($clPath -replace "\\\\bin\\\\.*$", ""); ' +
                'Write-Output $msvcRoot',
                check: false)
            winsdk_include_result = run_command('powershell', '-NoProfile', '-Command',
                '$sdkInc = Get-ChildItem "${env:ProgramFiles(x86)}/Windows Kits/10/Include" -Directory | Sort-Object Name | Select-Object -Last 1; ' +
                'Write-Output $sdkInc.FullName',
                check: false)
            nvcc_host_includes = []
            if msvc_include_result.returncode() == 0 and winsdk_include_result.returncode() == 0
                msvc_root_dir = msvc_include_result.stdout().strip()
                winsdk_inc_dir = winsdk_include_result.stdout().strip()
                nvcc_host_includes = [
                    '-I', msvc_root_dir / 'include',
                    '-I', winsdk_inc_dir / 'ucrt',
                    '-I', winsdk_inc_dir / 'shared',
                    '-I', winsdk_inc_dir / 'um',
                ]
                message('MSVC include: ' + msvc_root_dir / 'include')
                message('UCRT include: ' + winsdk_inc_dir / 'ucrt')
            else
                warning('Could not find MSVC/Windows SDK include paths. nvcc compilation may fail.')
            endif
        else
            nvcc_ccbin_flags = []
            nvcc_host_includes = []
        endif

        # Detect CUDA version from nvcc directly instead of add_languages('cuda'),
        # which requires MSVC on Windows and fails with MinGW.
        # nvcc --version outputs: "...Cuda compilation tools, release 12.6, V12.6.85..."
        nvcc_ver_out = run_command(nvcc_exe, '--version', check: true)
        cuda_version = nvcc_ver_out.stdout().strip().split('release ')[1].split(',')[0]
        message('Found CUDA version = @0@'.format(cuda_version))

        # Gencode coverage: explicit cubins for every currently-shipping
        # consumer Nvidia generation from Turing (sm_75, RTX 20xx) through
        # Blackwell (sm_120, RTX 50xx) whenever the host nvcc supports them.
        # A compute_80 PTX is always emitted as the backward-JIT fallback
        # so any sm_80+ GPU missing a native cubin (future minor revisions,
        # headless Tegra variants) can still JIT a compatible kernel at
        # load time.
        #
        # Upstream Netflix meson.build only shipped cubins at the Txx major
        # boundaries (sm_75, sm_80, sm_90, sm_100, sm_120) plus a PTX at the
        # highest compute cap the toolchain supports. On CUDA 12.x that
        # meant Ampere sm_86 (RTX 30xx) and Ada sm_89 (RTX 40xx) had no
        # matching cubin and no PTX that could JIT backward to their arch
        # (the only PTX shipped was compute_90 or compute_120). The fork
        # closes that coverage hole unconditionally — independent of
        # toolchain version.
        #
        # Scope note: this fix addresses the cubin/PTX coverage surface
        # only. It does NOT fix every CUDA crash reported against the
        # upstream tree; upstream commits dff4082b (batch threading) and
        # 46d3a154 (picture pool) are now ported (ADR-1051); this comment
        # was updated from the old experimental-flag framing.
        gencode = [
            '--fatbin',
            '-gencode=arch=compute_75,code=sm_75',
            '-gencode=arch=compute_80,code=sm_80',
            # PTX at compute_80 is the JIT floor for every sm_80+ GPU that
            # might be missing a native cubin. Keep it unconditional.
            '-gencode=arch=compute_80,code=compute_80',
            '-gencode=arch=compute_86,code=sm_86',
            '-gencode=arch=compute_89,code=sm_89',
        ]
        if cuda_version.version_compare('<13')
            gencode += '-gencode=arch=compute_50,code=compute_50'
        endif
        # always compile device code to enable quick startup on newer GPUs, for the last supported GPU also generate PTX for future compatibility
        if cuda_version.version_compare('>11.8')
            gencode += '-gencode=arch=compute_90,code=sm_90'
            if cuda_version.version_compare('>12.8')
                gencode += [
                    '-gencode=arch=compute_100,code=sm_100',
                    '-gencode=arch=compute_120,code=sm_120',
                    # PTX at the highest compute cap the toolchain can
                    # target, for forward JIT to future GPUs.
                    '-gencode=arch=compute_120,code=compute_120',
                ]
            else
                gencode += '-gencode=arch=compute_90,code=compute_90'
            endif
        endif

    else
        nvcc_exe = find_program('clang')
        gencode = [
            '--cuda-gpu-arch=sm_75',
            '--cuda-device-only',
            '-S'
            # many complaints about device intrinsics when compiling without CTK and the ffmpeg cuda runtime header
            #            '-nocudainc',
            #            '-nocudalib',
        ]
    endif
    message('CUDA gencode = @0@'.format(gencode))


    # --std c++20: required when the host libstdc++ is gcc >= 16, whose
    # bundled headers use C++20 features (char8_t, constexpr semantics)
    # that nvcc's default C++17 host parser cannot consume. CUDA 13+
    # supports c++20 natively; 12.x clamps at c++17 and loses on gcc-16.
    #
    # -D__MATH_NO_INLINES: glibc 2.43 (Ubuntu 26.04) declares rsqrt(double)
    # in bits/mathcalls.h with noexcept(true). CUDA 13.0/13.1's
    # crt/math_functions.h redeclares the same symbol without the noexcept
    # spec, producing C2059/C2085 cascade errors from NVCC's host-compile
    # pass. CUDA 13.2 aligns its noexcept annotations with glibc 2.43, so
    # the flag is belt-and-suspenders for 13.2+ but harmless to include.
    # It instructs glibc to suppress the inline math-function definitions
    # (including the noexcept-annotated rsqrt overload), preventing the
    # redeclaration conflict even if a future CUDA patch re-introduces the
    # mismatch. ADR-0603.
    cuda_flags = ['-D_USE_MATH_DEFINES', '-D__MATH_NO_INLINES', '--std', 'c++20']
    if get_option('buildtype') == 'debug'
        cuda_flags += ['-DCUDA_DEBUG', '-G']
    else
        if is_nvtx_enabled
            cuda_flags += ['-lineinfo']
        endif
    endif

    # On Windows MSVC, the .cu sources transitively include
    # feature_collector.h and cuda/common.h, both of which `#include
    # <pthread.h>`. nvcc shells the host pass out to cl.exe, which
    # has no pthread.h on its system include path. The shim at
    # libvmaf/src/compat/win32/pthread.h is wired into the C/C++ builds
    # via pthread_dependency, but custom_target nvcc invocations build
    # their own -I list — so add the shim path explicitly here too.
    cuda_extra_includes = []
    if host_machine.system() == 'windows'
        cuda_extra_includes += ['-I', '../src/compat/win32']
    endif
    ptx_files = {}
    foreach name, _cu : cuda_cu_sources
        per_kernel_flags = cuda_cu_extra_flags.get(name, [])
        t = custom_target('cu_ptx_target_' + name,
            build_by_default: true,
            output : ['@0@.fatbin'.format(name)],
            input : _cu,
            command : [nvcc_exe,
                gencode,
                '@INPUT@',
                '-o', '@OUTPUT@' ,
                '-I', './src',
                '-I', '../src',
                '-I', '../include',
                '-I', '../src/feature',
                '-I', '../src/' + cuda_dir,
                cuda_extra_includes,
                '-DDEVICE_CODE',
            ] + cuda_flags + nvcc_ccbin_flags + nvcc_host_includes + per_kernel_flags
        )
        ptx_files += {name : [t]}
    endforeach

    message('ptx_files = @0@'.format(ptx_files.keys()))

    # bin2c is distributed along with cuda tools. Use '--padd 0x00' to add a NULL-terminator byte
    # to the end of the generated array.
    bin2c_exe = find_program('bin2c')
    ptx_arrays = []
    foreach name, _ptx : ptx_files
        t = custom_target('ptx_bin2c_@0@'.format(name),
            build_by_default: true,
            output : ['@PLAINNAME@.c'],
            input : _ptx,
            capture : true,
            command : [bin2c_exe, '--const', '--padd', '0x00',
                '--name', '@BASENAME@_ptx', '@INPUT@',
            ]
        )
        ptx_arrays += t
    endforeach

    cuda_static_lib = static_library(
        'cuda_common_vmaf_lib',
        [cuda_sources, ptx_arrays,],
        include_directories : [
            libvmaf_include,
            vmaf_base_include,
            cuda_dir,
            feature_src_dir,
            include_directories('../src/cuda/'),
            cuda_inc,
        ],
        c_args : vmaf_cflags_common,
        # pthread_dependency is a no-op on POSIX (empty list) and on Windows
        # MSVC routes <pthread.h> to the header-only Win32 shim at
        # src/compat/win32/pthread.h. gpu_picture_pool.cpp in this lib still uses
        # pthread directly (even though Netflix#1472 removed it from
        # common.h), so the dependency must be threaded in here.
        dependencies : [pthread_dependency],
        cuda_args: cuda_flags + nvcc_ccbin_flags + nvcc_host_includes
    )

    common_cuda_objects += cuda_static_lib.extract_all_objects(recursive: false)
endif

sycl_dependency = []
common_sycl_objects = []

if is_sycl_enabled
    # Find SYCL compiler — defaults to 'icpx' from Intel oneAPI.
    # Override with -Dsycl_compiler=/path/to/icpx if not in PATH.
    # This lets the project C/C++ compiler be gcc/clang while SYCL
    # feature extractors are compiled with icpx -fsycl.
    #
    # ADR-0335: AdaptiveCpp (acpp / syclcc, formerly OpenSYCL / hipSYCL)
    # is a supported alternative toolchain. We detect it from the
    # configured `sycl_compiler` name and switch flags + runtime
    # libraries accordingly. icpx remains the primary toolchain.
    sycl_compiler_opt = get_option('sycl_compiler')
    icpx = find_program(sycl_compiler_opt, required : true)
    fs = import('fs')

    sycl_compiler_basename = fs.name(sycl_compiler_opt)
    is_sycl_acpp = (
        sycl_compiler_basename == 'acpp'
        or sycl_compiler_basename == 'syclcc'
        or sycl_compiler_basename == 'syclcc-clang'
    )
    if is_sycl_acpp
        message('SYCL toolchain: AdaptiveCpp (' + sycl_compiler_basename + ') — ADR-0335')
    else
        message('SYCL toolchain: Intel oneAPI (' + sycl_compiler_basename + ')')
    endif

    # Include paths as explicit -I flags (custom_target cannot use
    # include_directories objects)
    sycl_inc_flags = [
        '-I' + meson.current_build_dir(),
        '-I' + meson.current_source_dir(),
        '-I' + meson.current_source_dir() / '..' / 'include',
        '-I' + meson.current_source_dir() / 'feature',
        '-I' + meson.current_source_dir() / 'feature' / 'common',
        '-I' + meson.current_source_dir() / 'sycl',
    ]

    # icpx-cl on Windows targets x86_64-pc-windows-msvc and rejects
    # -fPIC ("unsupported option for target"). PIC is the default on
    # Windows DLL builds anyway, so drop the flag there. Mirrors the
    # CUDA / nvcc fatbin handling at cuda_extra_includes.
    sycl_pic_arg = []
    if host_machine.system() != 'windows'
        sycl_pic_arg = ['-fPIC']
    endif

    # SYCL .cpp sources include feature_collector.h transitively, which
    # pulls in pthread.h. icpx custom_targets bypass meson's dependency
    # plumbing, so thread the Win32 pthread shim include path into the
    # explicit -I list on Windows. Same pattern as cuda_extra_includes.
    if host_machine.system() == 'windows'
        sycl_inc_flags += [
            '-I' + meson.current_source_dir() / 'compat' / 'win32',
        ]
    endif

    # Toolchain-specific compile flags.
    #   icpx:  `-fsycl` enables single-source SYCL.
    #          When `sycl_icpx_aot_targets` is non-empty (the default),
    #          we additionally pass `-fsycl-targets=spir64_gen,spir64` to
    #          request AOT compilation into native GPU ISA, and
    #          `-Xs '-device <list>'` to specify target micro-architectures.
    #          This eliminates the JIT cold-start cost paid on every first
    #          kernel launch. The binary embeds native ISA blobs for each
    #          listed target plus a SPIR-V fallback for unlisted devices.
    #          Set `sycl_icpx_aot_targets=''` to revert to SPIR-V JIT only
    #          (smaller binary, mandatory JIT on first launch). ADR-0567.
    #   acpp:  `--acpp-targets=<targets>` selects backend(s); the
    #          equivalent of `-fsycl` is implicit.
    if is_sycl_acpp
        acpp_targets = get_option('sycl_acpp_targets')
        sycl_toolchain_args = ['--acpp-targets=' + acpp_targets]
    else
        icpx_aot_targets = get_option('sycl_icpx_aot_targets')
        if icpx_aot_targets != ''
            # AOT path: emit spir64_gen (IGC) ISA blobs for the listed targets.
            # `-Xs '-device <list>'` is forwarded to IGC via clang-offload-wrapper.
            # `spir64` fallback embeds portable SPIR-V for any unlisted device.
            sycl_toolchain_args = [
                '-fsycl',
                '-fsycl-targets=spir64_gen,spir64',
                '-Xs', '-device ' + icpx_aot_targets,
            ]
            message('SYCL AOT targets (icpx): ' + icpx_aot_targets)
        else
            # JIT path: portable SPIR-V only; compiled to native ISA at first launch.
            sycl_toolchain_args = ['-fsycl']
            message('SYCL AOT: disabled — SPIR-V JIT only (sycl_icpx_aot_targets is empty)')
        endif
    endif

    sycl_common_args = vmaf_cflags_common + ['-c'] + sycl_toolchain_args + ['-std=c++20'] + sycl_pic_arg

    sycl_sources = [
        sycl_dir + 'common.cpp',
        sycl_dir + 'dispatch_strategy.cpp',
        sycl_dir + 'picture_sycl.cpp',
        sycl_dir + 'dmabuf_import.cpp',
        sycl_dir + 'd3d11_import.cpp',
    ]

    sycl_surface_args = []
    sycl_surface_deps = []

    # Level Zero loader — always required when SYCL is enabled because
    # common.cpp uses zeMemFree / get_native<ext_oneapi_level_zero>.
    libze_dep = cc.find_library('ze_loader', required : true)

    # Linux: DMA-BUF + VA-API zero-copy path
    libva_dep = dependency('libva', required : false)
    libva_drm_dep = dependency('libva-drm', required : false)
    have_sycl_dmabuf = libva_dep.found() and libva_drm_dep.found()

    if have_sycl_dmabuf
        sycl_surface_args += '-DHAVE_SYCL_DMABUF=1'
        sycl_surface_deps += [libva_dep, libva_drm_dep]
        message('SYCL DMA-BUF import: enabled (VA-API found)')
    else
        message('SYCL DMA-BUF import: disabled (missing VA-API)')
    endif

    # Windows: D3D11 staging-texture surface import. d3d11_import.cpp is
    # wrapped in #ifdef _WIN32 so the TU is a no-op on non-Windows; we
    # link d3d11 + dxgi only when building for Windows.
    if host_machine.system() == 'windows'
        d3d11_lib = cc.find_library('d3d11', required : true)
        dxgi_lib  = cc.find_library('dxgi',  required : true)
        sycl_surface_deps += [d3d11_lib, dxgi_lib]
        message('SYCL D3D11 import: enabled (Windows target)')
    endif

    # Compile each SYCL common source with icpx
    foreach src : sycl_sources
        name = fs.stem(src)
        common_sycl_objects += custom_target('sycl_common_' + name,
            output : name + '.o',
            input : src,
            command : [icpx] + sycl_common_args + sycl_inc_flags
                      + sycl_surface_args + ['@INPUT@', '-o', '@OUTPUT@'],
        )
    endforeach

    # Derive the SYCL runtime library directory from the compiler
    # location so that gcc/g++ can link against the SYCL runtime
    # without needing the toolchain-specific driver flag.
    icpx_bindir = fs.parent(icpx.full_path())
    sycl_lib_dir = fs.parent(icpx_bindir) / 'lib'

    sycl_extra_runtime_deps = []
    if is_sycl_acpp
        # AdaptiveCpp installs its runtime as `libacpp-rt.so` (and the
        # legacy alias `libhipSYCL-rt.so`). Try the modern name first,
        # then the legacy one for older packages. ADR-0335.
        sycl_lib = cc.find_library('acpp-rt',
            dirs : [sycl_lib_dir], required : false)
        if not sycl_lib.found()
            sycl_lib = cc.find_library('hipSYCL-rt',
                dirs : [sycl_lib_dir], required : true)
        endif
        # AdaptiveCpp does not ship Intel SVML / libirc; skip the
        # icpx-only Intel-runtime lookup. AdaptiveCpp objects also
        # don't depend on the Level Zero loader unless the host has
        # explicitly opted in via `--acpp-targets` containing
        # `spirv` against an L0 backend; we keep `libze_dep` in
        # `dependencies` below so the existing `common.cpp`
        # `get_native<ext_oneapi_level_zero>` path still resolves
        # at link time when targeting Intel HW under acpp.
    else
        sycl_lib = cc.find_library('sycl',
            dirs : [sycl_lib_dir], required : true)
        # icpx-compiled objects reference Intel runtime symbols
        # (_intel_fast_memcpy, __svml_*) that gcc/g++ won't find.
        # Link the Intel runtime libraries explicitly so a non-Intel
        # host linker can resolve them. On Windows the host C/C++
        # compiler is icx-cl (the same Intel toolchain), which
        # auto-injects svml/irc at link time — and the Windows lib
        # names differ (svml_dispmd.lib, libirc.lib) — so skip the
        # explicit lookup there.
        if host_machine.system() != 'windows'
            svml_lib = cc.find_library('svml',
                dirs : [sycl_lib_dir], required : true)
            irc_lib = cc.find_library('irc',
                dirs : [sycl_lib_dir], required : true)
            sycl_extra_runtime_deps += [svml_lib, irc_lib]
        endif
    endif

    # `-fsycl` must be present at every link step that pulls in SYCL `.o`
    # files from a static archive (e.g. libvmaf.a linked into test
    # executables).  Without it icpx skips clang-offload-wrapper and the
    # SYCL runtime's ProgramManager never registers the device kernels,
    # causing a null-dereference SIGSEGV in
    # ProgramManager::getDeviceKernelInfo at the first q.submit() call.
    # Embedding the flag in sycl_dependency propagates it automatically to
    # every target that declares this dependency — libvmaf.so and all SYCL
    # test executables — so no per-target duplication is needed.
    # Root-cause investigation: docs/research/sycl-fsycl-link-flag.md
    # (companion to ADR-NNNN-sycl-fsycl-link-propagation).
    sycl_dependency += declare_dependency(
        dependencies : [sycl_lib, libze_dep] + sycl_extra_runtime_deps + sycl_surface_deps,
        link_args : ['-fsycl'],
    )
endif

thread_lib = dependency('threads')
math_lib = cc.find_library('m', required : false)

vmaf_include = include_directories(
    src_dir,
    feature_src_dir,
    feature_src_dir + 'common',
)

# ---------------------------------------------------------------------------
# ADR-0707: Rust/cbindgen pilot — build the TAD feature extractor crate.
#
# cargo build --release produces libvmafx_tad.a in the crate's
# target/release/ directory.  We wrap it in a declare_dependency so the
# archive is linked into libvmaf.so alongside the C sources.
#
# The option enable_rust_features (default: false, see meson_options.txt)
# lets operators enable the Rust build on hosts with a Rust toolchain
# (cargo + cbindgen).  When disabled, the tad extractor is silently
# omitted and --feature tad returns -ENOSYS.
# ---------------------------------------------------------------------------
is_rust_enabled = get_option('enable_rust_features')

rust_tad_objects = []
rust_tad_dep = []

if is_rust_enabled
    cargo_exe = find_program('cargo', required: false)
    if not cargo_exe.found()
        warning('enable_rust_features=true but cargo not found; TAD extractor will be skipped.')
        is_rust_enabled = false
    endif
endif

if is_rust_enabled
    rust_tad_crate_dir = meson.current_source_dir() / 'feature' / 'rust' / 'tad'
    rust_tad_target_dir = meson.current_build_dir() / 'rust_tad_target'
    rust_tad_lib_name = 'libvmafx_tad.a'

    rust_tad_staticlib = custom_target('rust_tad_staticlib',
        output: rust_tad_lib_name,
        input: [
            rust_tad_crate_dir / 'src' / 'lib.rs',
            rust_tad_crate_dir / 'Cargo.toml',
        ],
        command: [
            'sh', '-c',
            cargo_exe.full_path() + ' build --release' +
                ' --manifest-path ' + rust_tad_crate_dir / 'Cargo.toml' +
                ' --target-dir ' + rust_tad_target_dir +
                ' && cp ' + rust_tad_target_dir / 'release' / rust_tad_lib_name +
                ' @OUTPUT@',
        ],
        build_by_default: true,
        build_always_stale: false,
    )

    # -DHAVE_RUST_TAD is emitted as a compile_args so any TU that
    # depends on this dependency gets the define. In practice only
    # feature_extractor.c and tad_rust.c use it — the define gates the
    # extern vmafx_tad_* declarations so builds without the archive can
    # still compile (they get -ENOSYS stubs instead).
    rust_tad_dep = declare_dependency(
        sources: [rust_tad_staticlib],
        compile_args: ['-DHAVE_RUST_TAD'],
        link_args: [
            meson.current_build_dir() / rust_tad_lib_name,
        ],
    )
endif

# tad_rust.c is compiled as a DIRECT source of the libvmaf library() target,
# not into libvmaf_feature.a. This keeps it out of the static-lib variant that
# tests link against (avoiding unresolved vmafx_tad_* refs in tests that don't
# include rust_tad_dep). The shared libvmaf.so picks it up via libvmaf_sources.
rust_tad_direct_sources = []
if is_rust_enabled
    rust_tad_direct_sources = [feature_src_dir + 'tad_rust.c']
endif

libvmaf_feature_sources = [
    feature_src_dir + 'picture_copy.cpp',
    feature_src_dir + 'integer_psnr.c',
    feature_src_dir + 'third_party/xiph/psnr_hvs.c',
    feature_src_dir + 'feature_extractor.cpp',
    feature_src_dir + 'feature_name.cpp',
    feature_src_dir + 'alias.c',
    feature_src_dir + 'integer_adm.c',
    feature_src_dir + 'feature_collector.c',
    feature_src_dir + 'integer_motion.c',
    feature_src_dir + 'integer_motion_v2.c',
    feature_src_dir + 'integer_vif.c',
    feature_src_dir + 'ciede.c',
    feature_src_dir + 'delta_e_itp.c',
    # NIQE (no-reference, ADR-1112) — scalar CPU extractor; pristine MVG model
    # embedded as niqe_model.h, numeric kernels in niqe_math.h.
    feature_src_dir + 'niqe.c',
    # PU21 HDR perceptual adapter (PU-PSNR + PU-SSIM). CPU-only, additive
    # fork feature. pu21_ssim.c is a self-contained L=256 SSIM that does NOT
    # touch the golden float_ssim.c. See docs/adr/<NNNN>-pu21-hdr-metric.md.
    feature_src_dir + 'pu21.c',
    feature_src_dir + 'pu21_ssim.c',
    # BRISQUE (no-reference, opinion-aware, ADR-1115) — scalar CPU extractor;
    # bundled LIVE-trained EPSILON_SVR model embedded as brisque_model.h,
    # numeric kernels in brisque_math.h. Predicts via the vendored libsvm
    # (svm.cpp / svm.h), proving the "extractor owns its own SVR model" pattern.
    feature_src_dir + 'brisque.c',
    feature_src_dir + 'common/alignment.c',
    feature_src_dir + 'mkdirp.cpp',

    feature_src_dir + 'float_ssim.c',
    feature_src_dir + 'float_ms_ssim.c',
    feature_src_dir + 'integer_ssim.c',
    feature_src_dir + 'ssim.c',
    feature_src_dir + 'ms_ssim.c',
    feature_src_dir + 'ms_ssim_decimate.c',
    feature_src_dir + 'iqa/decimate.c',
    feature_src_dir + 'iqa/ssim_tools.c',
    feature_src_dir + 'iqa/math_utils.c',
    feature_src_dir + 'iqa/convolve.c',
    feature_src_dir + 'cambi.c',
    feature_src_dir + 'luminance_tools.cpp',
    feature_src_dir + 'null.c',
    feature_src_dir + 'speed_qa.c',
    # LPIPS extractor uses the public libvmaf/dnn.h surface, which stays
    # available as -ENOSYS stubs when ORT is missing — so compile
    # unconditionally; init() declines at runtime on stub builds.
    feature_src_dir + 'feature_lpips.c',
    # DISTS-Sq extractor — same DNN-stub-friendly registration and
    # runtime-decline posture as feature_lpips.c.
    feature_src_dir + 'feature_dists.c',
    # FastDVDnet temporal pre-filter (T6-7) — same -ENOSYS-stub-friendly
    # libvmaf/dnn.h surface as feature_lpips.c.
    feature_src_dir + 'fastdvdnet_pre.c',
    # MobileSal saliency extractor (T6-2a). Same -ENOSYS-on-stub pattern
    # as feature_lpips.c above. See docs/adr/0218-mobilesal-saliency-extractor.md.
    feature_src_dir + 'feature_mobilesal.c',
    # TransNet V2 shot-boundary detector — same -ENOSYS-stub story
    # as feature_lpips.c; compile unconditionally and let init()
    # decline at runtime on stub builds.
    feature_src_dir + 'transnet_v2.c',
    # Note: tad_rust.c is NOT added here — it is compiled directly into
    # the libvmaf library() target (see rust_tad_direct_sources) to avoid
    # pulling vmafx_tad_* undefined references into libvmaf_feature.a, which
    # would break static-lib-linked test executables that don't include
    # rust_tad_dep. See ADR-0707.
]

if float_enabled
    libvmaf_feature_sources += [
        feature_src_dir + 'float_adm.c',
        feature_src_dir + 'float_psnr.c',
        feature_src_dir + 'float_motion.c',
        feature_src_dir + 'float_vif.c',
        feature_src_dir + 'float_moment.c',
        feature_src_dir + 'speed.c',
        # ADR-0964: speed_internal.c — CPU-side helpers shared by speed.c
        # and the speed_{chroma,temporal}_{cuda,hip,sycl} GPU twins.
        # See feature/speed_internal.h for the contract.
        feature_src_dir + 'speed_internal.c',

        feature_src_dir + 'common/convolution.c',
        feature_src_dir + 'offset.c',
        feature_src_dir + 'adm.c',
        feature_src_dir + 'adm_tools.c',
        feature_src_dir + 'vif.c',
        feature_src_dir + 'vif_tools.c',
        feature_src_dir + 'motion.c',
        feature_src_dir + 'psnr.c',
        feature_src_dir + 'psnr_tools.cpp',
        feature_src_dir + 'moment.c',
        feature_src_dir + 'common/blur_array.c',
    ]
endif

if is_cuda_enabled
    libvmaf_feature_sources += [
        feature_src_dir + 'cuda/integer_adm_cuda.c',
        feature_src_dir + 'cuda/integer_vif_cuda.c',
        feature_src_dir + 'cuda/integer_motion_cuda.c',
        feature_src_dir + 'cuda/integer_motion_v2_cuda.c',
        feature_src_dir + 'cuda/integer_psnr_cuda.c',
        feature_src_dir + 'cuda/integer_moment_cuda.c',
        feature_src_dir + 'cuda/integer_ciede_cuda.c',
        feature_src_dir + 'cuda/integer_ssim_cuda.c',
        # Real integer_ssim CUDA host glue (ADR-0564). ssim_cuda.c provides
        # vmaf_fex_integer_ssim_cuda; integer_ssim_cuda.c keeps providing
        # vmaf_fex_float_ssim_cuda (misnomed legacy, kept for back-compat).
        feature_src_dir + 'cuda/ssim_cuda.c',
        feature_src_dir + 'cuda/integer_ms_ssim_cuda.c',
        feature_src_dir + 'cuda/integer_psnr_hvs_cuda.c',
        feature_src_dir + 'cuda/float_psnr_cuda.c',
        feature_src_dir + 'cuda/float_motion_cuda.c',
        feature_src_dir + 'cuda/float_vif_cuda.c',
        feature_src_dir + 'cuda/ssimulacra2_cuda.c',
        feature_src_dir + 'cuda/float_adm_cuda.c',
        # T3-15 / ADR-0360: cambi CUDA twin host glue.
        feature_src_dir + 'cuda/integer_cambi_cuda.c',
        # ADR-0965: speed_chroma_cuda + speed_temporal_cuda wired after
        # CHECK_CUDA → CHECK_CUDA_GOTO repair pass (closes
        # T-CUDA-SPEED-TU-REPAIR-2026-05-31).
        feature_src_dir + 'cuda/speed_chroma_cuda.c',
        feature_src_dir + 'cuda/speed_temporal_cuda.c',
    ]
endif

# SYCL feature extractors are compiled with icpx -fsycl.
# They are built separately via custom_target and linked via objects.
sycl_feature_objects = []
if is_sycl_enabled
    sycl_feature_sources = [
        feature_src_dir + 'sycl/integer_vif_sycl.cpp',
        feature_src_dir + 'sycl/integer_adm_sycl.cpp',
        feature_src_dir + 'sycl/integer_motion_sycl.cpp',
        feature_src_dir + 'sycl/integer_motion_v2_sycl.cpp',
        feature_src_dir + 'sycl/float_psnr_sycl.cpp',
        feature_src_dir + 'sycl/float_motion_sycl.cpp',
        feature_src_dir + 'sycl/float_vif_sycl.cpp',
        feature_src_dir + 'sycl/integer_psnr_sycl.cpp',
        feature_src_dir + 'sycl/integer_moment_sycl.cpp',
        feature_src_dir + 'sycl/integer_ciede_sycl.cpp',
        feature_src_dir + 'sycl/integer_ssim_sycl.cpp',
        feature_src_dir + 'sycl/integer_ms_ssim_sycl.cpp',
        feature_src_dir + 'sycl/integer_psnr_hvs_sycl.cpp',
        feature_src_dir + 'sycl/ssimulacra2_sycl.cpp',
        feature_src_dir + 'sycl/float_adm_sycl.cpp',
        feature_src_dir + 'sycl/integer_cambi_sycl.cpp',
        # ADR-0964: speed_chroma_sycl + speed_temporal_sycl — GPU twins of
        # vmaf_fex_speed_chroma / vmaf_fex_speed_temporal.  Hybrid GPU/CPU
        # split mirrors the CUDA and HIP twins; CPU-side eigendecomp + QR
        # live in feature/speed_internal.c.
        feature_src_dir + 'sycl/speed_chroma_sycl.cpp',
        feature_src_dir + 'sycl/speed_temporal_sycl.cpp',
    ]

    # `-fp-model=precise` is Intel's strict-FP flag: no FMA contraction,
    # no associativity reordering, IEEE-754 compliant. Matches CPU
    # integer_adm.c's gcc -O2 default behaviour (gcc defaults to
    # -ffp-contract=off for non-FMA-friendly architectures). Without
    # it, icpx fuses the cos(1deg) angle-flag comparison's RHS
    # multiplications differently than gcc and produces a 2.4e-4
    # adm_scale2 drift on Arc vs CPU at the rounding boundary (T7-16).
    # `-ffp-contract=off` and `#pragma clang fp contract(off)` blocks
    # in the source files were not sufficient — those flags don't
    # propagate to icpx's device-side codegen pass for SYCL kernel
    # lambdas; `-fp-model=precise` does.
    # `-fp-model=precise` is icpx's strict-FP flag (see comment above).
    # AdaptiveCpp doesn't accept `-fp-model=precise`; it uses the
    # underlying clang's `-ffp-contract=off` plus the source-level
    # `precise` decoration on per-kernel arithmetic. Per
    # `libvmaf/src/sycl/AGENTS.md`, the precise-FP contract is
    # load-bearing for places=4 conformance, so emit `-ffp-contract=off`
    # under acpp; the residual numerical drift vs icpx is documented
    # in ADR-0335 §"Numerical conformance".
    if is_sycl_acpp
        sycl_strict_fp_args = ['-ffp-contract=off']
    else
        sycl_strict_fp_args = ['-fp-model=precise']
    endif
    sycl_feature_args = vmaf_cflags_common + ['-c'] + sycl_toolchain_args + ['-std=c++20'] + sycl_strict_fp_args + sycl_pic_arg + ['-fpermissive']
    sycl_feature_inc = sycl_inc_flags + [
        '-I' + meson.current_source_dir() / 'cuda',
        '-I' + meson.current_source_dir() / 'feature' / 'sycl',
    ]

    foreach src : sycl_feature_sources
        name = fs.stem(src)
        sycl_feature_objects += custom_target('sycl_feature_' + name,
            output : name + '.o',
            input : src,
            command : [icpx] + sycl_feature_args + sycl_feature_inc
                      + ['@INPUT@', '-o', '@OUTPUT@'],
        )
    endforeach
endif

# ADR-0891 (round-2 SIMD bit-exact fix, follow-up to #339):
# Under icx, `-ffp-contract=off` alone does not constrain the
# optimizer's relaxed FP-model defaults. Scalar reference TUs
# inside `libvmaf_feature_static_lib` (e.g. `ms_ssim_decimate_scalar`)
# end up with non-strict floating-point semantics, while the SIMD
# carve-out libs already gained `-fp-model=precise` in #339. The
# resulting asymmetry caused `test_ms_ssim_decimate` to diverge
# from the SIMD path. Mirror the same flag onto every static-lib
# that owns scalar references the SIMD tests compare against.
_libvmaf_feature_icx_args = []
if cc.get_id() == 'intel-llvm' or cc.get_id() == 'intel-llvm-cl'
    _libvmaf_feature_icx_args = ['-fp-model=precise']
endif

# ssimulacra2.c scalar extractor compiles in its own static lib with
# `-ffp-contract=off`. Rationale: `compute_vars` / `compute_masks` /
# `accumulate_error` and the scalar fallback functions mix with the
# SIMD function-pointer dispatch path; even when `xyb_fn` / `blur_fn`
# etc. are SIMD, the scalar glue around them (accumulate_error, the
# main extract loop, float pooling) must use the same FMA-contract
# policy as the SIMD TUs to avoid cross-host drift. Scoped here so
# other extractors' bit-exact behaviour is untouched.
libvmaf_ssimulacra2_static_lib = static_library(
    'libvmaf_ssimulacra2',
    feature_src_dir + 'ssimulacra2.c',
    include_directories : [libvmaf_include, vmaf_include, cuda_dir],
    dependencies: [stdatomic_dependency, pthread_dependency, cuda_dependency, sycl_dependency],
    c_args: vmaf_cflags_common + ['-ffp-contract=off'] + _libvmaf_feature_icx_args,
)

# Y-FUNQUE+ atoms-only extractor (ADR-1114) compiles with `-ffp-contract=off`
# for the same cross-host-determinism reason as ssimulacra2: the DLM cube-root
# pooling, MS-SSIM std/mean covariance pooling, and the Nadenau CSF exp()/pow()
# must not be FMA-contracted differently per host, which would drift the
# atoms beyond the places=4 snapshot gate. Scoped to its own static lib so no
# other extractor's bit-exact behaviour is touched.
libvmaf_y_funque_plus_static_lib = static_library(
    'libvmaf_y_funque_plus',
    feature_src_dir + 'y_funque_plus.c',
    include_directories : [libvmaf_include, vmaf_include, cuda_dir],
    dependencies: [stdatomic_dependency, pthread_dependency, cuda_dependency, sycl_dependency],
    c_args: vmaf_cflags_common + ['-ffp-contract=off'] + _libvmaf_feature_icx_args,
)

libvmaf_feature_static_lib = static_library(
    'libvmaf_feature',
    # The BRISQUE allmodel embed (ADR-1115) lives here so that every consumer
    # of feature_brisque.c.o (libvmaf and the several test executables that
    # extract the feature lib's objects) resolves src_brisque_live_model[]
    # without each having to list the generated TU separately.
    libvmaf_feature_sources + brisque_model_c_sources + hip_sources + hip_hsaco_sources + metal_sources,
    include_directories : [libvmaf_include, vmaf_include, cuda_dir],
    c_args : vmaf_cflags_common + _libvmaf_feature_icx_args,
    dependencies: [stdatomic_dependency, pthread_dependency, cuda_dependency, sycl_dependency, hip_deps, metal_deps],
    objects: [common_cuda_objects, common_sycl_objects, sycl_feature_objects,
              libvmaf_ssimulacra2_static_lib.extract_all_objects(recursive: false),
              libvmaf_y_funque_plus_static_lib.extract_all_objects(recursive: false)]
)

libsvm_cpp_args = vmaf_cflags_common
if meson.get_compiler('cpp').get_id() == 'clang' or meson.get_compiler('cpp').get_id() == 'apple-clang'
    libsvm_cpp_args += ['-U_LIBCPP_ENABLE_ASSERTIONS']
endif

libsvm_static_lib = static_library(
    'libsvm',
    src_dir + 'svm.cpp',
    include_directories : [libvmaf_include, vmaf_include],
    dependencies : [thread_lib],
    cpp_args : libsvm_cpp_args,
)

# ADR-0708: metadata_handler.cpp is the first C++20 pilot TU. Compiled as a
# separate static lib so its cpp_std override (c++20) does not propagate to
# any other C++ TU in libvmaf. Linked into the final libvmaf via link_with below.
metadata_handler_cpp20_lib = static_library(
    'metadata_handler_cpp20',
    src_dir + 'metadata_handler.cpp',
    include_directories : [vmaf_base_include, libvmaf_include],
    override_options : ['cpp_std=c++20'],
    pic : true,
    install : false,
)

# ADR-0708 (Wave 1, second pilot after dict — supersedes PR #42 / ADR-0722):
# log.cpp converts the libvmaf logging subsystem from C to C++23. The public
# C ABI in src/log.h is preserved verbatim via `extern "C"` guards; this
# isolated static-library pattern keeps the cpp_std=c++23 override scoped to
# the single TU so it cannot leak into other C++ TUs that may still need a
# lower floor (notably the SYCL device-image TUs compiled by icpx).
log_cpp23_lib = static_library(
    'log_cpp23',
    src_dir + 'log.cpp',
    include_directories : [vmaf_base_include, libvmaf_include],
    # libvmaf_cpu_cpp_std is the compiler-aware token defined above
    # (gcc/clang: 'c++23', MSVC: 'vc++latest', icx-cl: 'c++latest').
    override_options : ['cpp_std=' + libvmaf_cpu_cpp_std],
    pic : true,
    install : false,
)

# ADR-0761: opt.cpp and predict.cpp — C++23 Wave 8. Isolated static libs so
# cpp_std=c++23 does not propagate to the remaining C TUs in libvmaf.
# Linked into libvmaf.so via extract_all_objects() in the library() call below.
opt_cpp23_lib = static_library(
    'opt_cpp23',
    src_dir + 'opt.cpp',
    include_directories : [vmaf_base_include, libvmaf_include],
    # libvmaf_cpu_cpp_std: 'vc++latest' on MSVC, 'c++latest' on clang-cl/icx-cl,
    # 'c++23' elsewhere — same compiler-aware token as log_cpp23_lib (ADR-0860).
    override_options : ['cpp_std=' + libvmaf_cpu_cpp_std],
    pic : true,
    install : false,
)

# ADR-0768: picture_pool.cpp compiled as an isolated C++23 static lib
# (same isolation pattern as metadata_handler_cpp20_lib / ADR-0708).
# pthread_dependency: on Windows MSVC (no pthread.h in system headers) this
# adds src/compat/win32/ to the include path so the win32 pthreads shim is
# found.  On POSIX it is an empty list and has no effect.
picture_pool_cpp23_lib = static_library(
    'picture_pool_cpp23',
    src_dir + 'picture_pool.cpp',
    include_directories : [vmaf_base_include, libvmaf_include],
    # libvmaf_cpu_cpp_std: compiler-aware token (ADR-0860 follow-up, PR #692 fix).
    override_options : ['cpp_std=' + libvmaf_cpu_cpp_std],
    # ODR fix: picture_pool.cpp must see the same HAVE_CUDA / HAVE_SYCL defines
    # as libvmaf.c so that VmafPicturePrivate has an identical layout in both
    # the pool allocator TU and every consumer TU.  Without this, the 40-byte
    # CUDA sub-struct inside VmafPicturePrivate shifts buf_type from offset 16
    # (no-CUDA layout) to offset 56 (CUDA layout); validate_pic_params then
    # reads garbage at the wrong offset and returns -EINVAL on every
    # vmaf_read_pictures call.  See test_pic_preallocation failure analysis.
    cpp_args : (is_cuda_enabled ? ['-DHAVE_CUDA'] : []) +
               (is_sycl_enabled ? ['-DHAVE_SYCL'] : []),
    dependencies : [pthread_dependency],
    pic : true,
    install : false,
)

# ADR-0768: gpu_picture_pool.cpp compiled as an isolated C++23 static lib.
# pthread_dependency: same win32 shim rationale as picture_pool_cpp23_lib above.
gpu_picture_pool_cpp23_lib = static_library(
    'gpu_picture_pool_cpp23',
    src_dir + 'gpu_picture_pool.cpp',
    include_directories : [vmaf_base_include, libvmaf_include],
    # libvmaf_cpu_cpp_std: compiler-aware token (ADR-0860 follow-up, PR #692 fix).
    override_options : ['cpp_std=' + libvmaf_cpu_cpp_std],
    # ODR fix (follow-up to PR #840): gpu_picture_pool.cpp includes picture.h
    # (line 30) and gpu_picture_pool.h (line 31, which re-includes picture.h).
    # VmafPicturePrivate's cuda sub-struct changes size when HAVE_CUDA is absent,
    # shifting buf_type and other fields.  This lib does not inherit vmaf_cflags_common
    # (it carries no c_args at all), so the defines must be passed explicitly —
    # mirroring the same fix applied to picture_pool_cpp23_lib in PR #840.
    cpp_args : (is_cuda_enabled ? ['-DHAVE_CUDA'] : []) +
               (is_nvtx_enabled ? ['-DHAVE_NVTX'] : []) +
               (is_sycl_enabled ? ['-DHAVE_SYCL'] : []),
    dependencies : [pthread_dependency],
    pic : true,
    install : false,
)

# ADR-0858: gpu_dispatch_env.cpp is compiled as an isolated C++23 static lib
# so its cpp_std override (c++23) does not propagate to any other TU — same
# isolation pattern as metadata_handler_cpp20_lib (ADR-0708).  The public C
# symbol vmaf_gpu_dispatch_env_get retains its extern "C" linkage; callers
# in C translation units link without modification.
gpu_dispatch_env_cpp23_lib = static_library(
    'gpu_dispatch_env_cpp23',
    src_dir + 'gpu_dispatch_env.cpp',
    include_directories : [vmaf_base_include, libvmaf_include],
    # libvmaf_cpu_cpp_std: compiler-aware token (ADR-0860 follow-up, PR #692 fix).
    override_options : ['cpp_std=' + libvmaf_cpu_cpp_std],
    pic : true,
    install : false,
)

read_json_model_cpp23_lib = static_library(
    'read_json_model_cpp23',
    src_dir + 'read_json_model.cpp',
    include_directories : [vmaf_base_include, libvmaf_include],
    # libvmaf_cpu_cpp_std: compiler-aware token (ADR-0860 follow-up, PR #692 fix).
    override_options : ['cpp_std=' + libvmaf_cpu_cpp_std],
    pic : true,
    install : false,
    # read_json_model.cpp includes model.h which carries pthread_mutex_t
    # (predict_cache_lock, added by the race-fix PR #864).  On Windows MSVC
    # there is no native pthread.h; pthread_dependency provides the win32
    # shim header — same rationale as picture_pool_cpp23_lib (PR #838).
    dependencies : [pthread_dependency],
)

libvmaf_sources = [
    src_dir + 'libvmaf.c',
    src_dir + 'predict.c',
    src_dir + 'model.c',
    src_dir + 'picture.c',
    src_dir + 'picture_v2.c', # ADR-0928 cycle N+1: v2 allocator / converter implementation
    src_dir + 'mem.cpp',
    src_dir + 'output.cpp',
    src_dir + 'fex_ctx_vector.cpp',
    src_dir + 'thread_pool.c',
    src_dir + 'dict.cpp',
    # opt.cpp compiled as opt_cpp23_lib (ADR-0761)
    src_dir + 'ref.cpp',
    # read_json_model.cpp compiled as read_json_model_cpp23_lib (ADR-0761)
    src_dir + 'pdjson.c',
    # log.cpp is compiled as log_cpp23_lib (ADR-0708 Wave 1 pilot)
    src_dir + 'framesync.c',
    # metadata_handler.cpp is compiled as metadata_handler_cpp20_lib (ADR-0708)
    # picture_pool.cpp compiled as picture_pool_cpp23_lib (ADR-0768)
    src_dir + 'thread_locale.cpp',
    # gpu_picture_pool.cpp compiled as gpu_picture_pool_cpp23_lib (ADR-0768)
    # gpu_dispatch_env.cpp is compiled as gpu_dispatch_env_cpp23_lib (ADR-0858)
    # ADR-1113: vendored Pelorus <-> vmafx interop ABI (pack/parse, deband +
    # denoise param contracts, x265 CSV QP-report reader, version/result-string
    # accessors). CPU-only, dependency-free, NO Vulkan. Verbatim mirror of
    # VMAFx/pelorus@818d844 (ABI 1.3) — DO NOT EDIT; re-sync via
    # scripts/sync-pelorus-interop.sh. See docs/api/pelorus-interop.md.
    src_dir + 'interop/pelorus_interop.c',
    src_dir + 'interop/pelorus_deband_params.c',
    src_dir + 'interop/pelorus_denoise_params.c',
    src_dir + 'interop/pelorus_qp_report_csv.c',
    src_dir + 'interop/pelorus_version.c',
    # ADR-1118: Pelorus-driven perceptual spatial-pooling weighting. Parses the
    # vendored side-data blob and derives a per-frame pooling weight consumed by
    # vmaf_feature_score_pooled. Lives in libvmaf_sources (not the feature
    # static lib) because it is a pooling helper called directly from
    # libvmaf.c, not a registered feature extractor. CPU-only, NO Vulkan/GPU.
    # GOLDEN-GATE ISOLATION: inert unless opted in AND side-data present — the
    # Netflix golden pairs carry no side-data and score bit-exact.
    feature_src_dir + 'perceptual_weight.c',
    # ADR-0707: TAD Rust extractor C wrapper — compiled directly into
    # the libvmaf library() target so it is not included in the static
    # intermediate libs (avoids unresolved vmafx_tad_* refs in test
    # binaries that link against libvmaf.a without rust_tad_dep).
] + rust_tad_direct_sources

# `-fsycl` is embedded in sycl_dependency.link_args (see the
# declare_dependency block above) so every consumer — libvmaf.so and all
# SYCL test executables — gets it automatically without per-target
# repetition.  vmaf_link_args is kept as an empty list for any future
# link-time flags that apply only to the library target and not to tests.
vmaf_link_args = []

if is_sycl_enabled
    cpp = meson.get_compiler('cpp')
    cpp_id = cpp.get_id()
    # Linux: icpx → 'intel-llvm'.
    # Windows MSVC ABI: icx-cl → 'intel-llvm-cl' (newer meson) or
    #                            'clang-cl' (older meson, when meson
    #                            falls through to the clang-cl detector).
    sycl_cpp_ids = ['intel-llvm', 'intel-llvm-cl', 'clang-cl']
    if cpp_id not in sycl_cpp_ids
        error('enable_sycl=true requires the project C++ compiler to be ' +
              'icpx / icx-cl (Intel oneAPI DPC++/C++ Compiler). Re-run ' +
              'meson setup with `CXX=icpx CC=icx` after sourcing ' +
              'setvars.sh on Linux, or `set CXX=icx-cl CC=icx-cl` after ' +
              'calling setvars.bat on Windows. Detected `cpp.get_id()` = ' +
              '\'' + cpp_id + '\'. See docs/development/oneapi-install.md ' +
              'for details.')
    endif
endif

libvmaf = library(
    'vmaf',
    [libvmaf_sources, rev_target, json_model_c_sources, dnn_sources, dnn_libvmaf_only_sources, mcp_sources],
    include_directories : [libvmaf_inc, vmaf_include, dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    link_args : vmaf_link_args,
    dependencies : [
      thread_lib,
      math_lib,
      stdatomic_dependency,
      pthread_dependency,
      cuda_dependency,
      sycl_dependency,
      dnn_deps,
      hip_deps,
      metal_deps,
      rust_tad_dep,
    ],
    objects : [
        platform_specific_cpu_objects,
        libvmaf_feature_static_lib.extract_all_objects(recursive: true),
        libvmaf_cpu_static_lib.extract_all_objects(recursive: true),
        libsvm_static_lib.extract_all_objects(recursive: true),
        # ADR-0708: C++20 pilot TU compiled as isolated static lib.
        metadata_handler_cpp20_lib.extract_all_objects(recursive: true),
        # ADR-0708 Wave 1: log.cpp C++23 pilot TU compiled as isolated
        # static lib. Same isolation rationale as metadata_handler_cpp20_lib.
        log_cpp23_lib.extract_all_objects(recursive: true),
        # ADR-0858: C++23 gpu_dispatch_env TU compiled as isolated static lib.
        gpu_dispatch_env_cpp23_lib.extract_all_objects(recursive: true),
        # ADR-0768: C++23 Wave 9 isolated static libs.
        picture_pool_cpp23_lib.extract_all_objects(recursive: true),
        gpu_picture_pool_cpp23_lib.extract_all_objects(recursive: true),
        # ADR-0761: C++23 Wave 8 isolated static libs.
        opt_cpp23_lib.extract_all_objects(recursive: true),
        read_json_model_cpp23_lib.extract_all_objects(recursive: true),
    ],
    version : vmaf_soname_version,
    soversion : vmaf_soversion,
    install: true,
)

pkg_mod = import('pkgconfig')

# Collect private dependencies so that static linking (pkg-config --static)
# pulls in all transitive libraries the consumer needs.
# Use requires_private for deps that have .pc files so pkg-config resolves
# their own transitive deps (e.g. ze_loader -> spdlog -> fmt, etc.).
# Use libraries_private for deps found via cc.find_library() that lack .pc files.
libvmaf_private_libs = [thread_lib, math_lib]
libvmaf_private_reqs = []

if is_sycl_enabled
    # sycl, svml, irc are Intel compiler runtime libs without .pc files.
    # On Windows the explicit svml/irc lookup is skipped (icx-cl
    # auto-injects them at link time), so don't reference them here.
    libvmaf_private_libs += [sycl_lib] + sycl_extra_runtime_deps
    # ze_loader has a .pc file — use requires_private for transitive resolution
    libvmaf_private_reqs += 'libze_loader'
    if have_sycl_dmabuf
        libvmaf_private_reqs += ['libva', 'libva-drm']
    endif
endif

if is_metal_enabled
    # T8-1b runtime (ADR-0420): when a downstream consumer (FFmpeg, the
    # tap's ffmpeg formula, etc.) links libvmaf statically via
    # pkg-config --static, it needs to inherit -framework Foundation +
    # -framework Metal — otherwise the LTO link fails with:
    #   Undefined symbols for architecture arm64:
    #     "_MTLCopyAllDevices", referenced from: _vmaf_metal_context_new
    #     "___CFConstantStringClassReference", referenced from: lto.o
    # (CI run 25686136624 / job 75410239602, FFmpeg macOS lane).
    # pkg-config has no native "framework" syntax, but raw linker flags
    # in libraries_private get embedded verbatim into Libs.private.
    libvmaf_private_libs += ['-Wl,-framework,Foundation', '-Wl,-framework,Metal']
endif

pkg_mod.generate(libraries: libvmaf,
    version: meson.project_version(),
    name: 'libvmaf',
    filebase: 'libvmaf',
    description: 'VMAF, Video Multimethod Assessment Fusion',
    subdirs: [ '.', 'libvmaf'],
    requires_private: libvmaf_private_reqs,
    libraries_private: libvmaf_private_libs,
)

libvmaf_dep = declare_dependency(
    link_with: libvmaf,
    include_directories: [libvmaf_inc],
)
meson.override_dependency('libvmaf', libvmaf_dep)
