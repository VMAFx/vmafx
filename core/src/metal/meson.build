# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

# Included from core/src/meson.build via `subdir('metal')`. Paths
# below are relative to core/src/metal/.
#
# T8-1b runtime (ADR-0420): three Objective-C++ TUs drive
# Metal.framework directly. The per-feature consumer scaffolds under
# `feature/metal/` stay pure-C unless they need Metal API dispatch
# (T8-1c flips integer_motion_v2_metal to .mm; the other seven stay
# .c with -ENOSYS init() until each kernel arrives).

# `add_languages` MUST come before any target — meson errors with
# "Tried to use 'add_project_arguments' after a build target has been
# declared" otherwise (CI run 25686136776 / job 75410354618).
add_languages('objcpp', native: false, required: true)

# ADR-2170: every link that carries an Objective-C++ object names -lc++ twice. Meson 1.12
# links such a target with the objcpp compiler and adds the C++ standard library flag for the
# cpp objects it also holds (build.py get_used_stdlib_args(): (cpp, objcpp) is not masked),
# while the clang++ driver adds it on its own. A target's `link_language` would only move the
# flag, and the project has several hundred executables. Duplicate libraries on an ld64
# command line are harmless, and the warning would make every Metal link fail under
# -Wl,-fatal_warnings, so the linker is told the duplicates are expected.
metal_ld_dup_args = cc.get_supported_link_arguments(['-Wl,-no_warn_duplicate_libraries'])
add_project_link_arguments(metal_ld_dup_args, language : ['c', 'cpp', 'objcpp'])

# Apple framework linkage.
foundation_dep = dependency('Foundation', required: true)
metal_dep      = dependency('Metal', required: true)
metalkit_dep   = dependency('MetalKit', required: false)
iosurface_dep  = dependency('IOSurface', required: true)
metal_deps += foundation_dep
metal_deps += metal_dep
metal_deps += iosurface_dep
if metalkit_dep.found()
    metal_deps += metalkit_dep
endif

# Obj-C++ compile arguments for the .mm TUs. ARC, no Obj-C exception
# unwinding, fobjc-weak so we don't pull all of Foundation into every TU.
metal_objcpp_args = [
    '-fobjc-arc',
    '-fno-objc-arc-exceptions',
    '-fobjc-weak',
] + vmaf_strict_fp_args + vmaf_engine_name_args
# vmaf_engine_name_args (RC4 WP6, ADR-1852 D3): the engine's libvmaf names
# (core/src/vmafx/engine_names_gen.h) for the Obj-C++ TUs too; the project
# arguments cover only C and C++.
# vmaf_strict_fp_args (ADR-1461): the project-wide no-contraction argument
# covers C and C++; the Obj-C++ host wrappers score on the host too, and on
# Apple Silicon clang would fuse `a * b + c` there.

# Separate static_library for the Obj-C++ TUs — `objcpp_args` is set
# per-target, avoiding the `add_project_arguments` ordering trap.
# Mirrors the x86 AVX2 / NEON sibling pattern: build a dedicated
# static lib for sources that need non-default compile args, then
# extract_all_objects() into the main feature library via
# `metal_sources` (consumed by libvmaf_feature_static_lib in the
# parent meson.build).
#
# The base include directory is the project root (libvmaf/src) so
# `#include "../../metal/common.h"` from the feature/metal TUs and
# `#include "dict.h"` (in core/src/) both resolve.
# Note: `vmaf_include` (which covers feature/ + feature/common) is
# declared after subdir('metal') in the parent meson.build, so we
# build the include set locally from the same paths.
metal_objcpp_includes = include_directories(
    '.', '..',                 # core/src/metal/, core/src/
    '../feature', '../feature/common',
    '../../include',           # libvmaf/include
)

metal_objcpp_lib = static_library(
    'metal_objcpp',
    [
        files('common.mm',
              'picture_metal.mm',
              'picture_import.mm',
              'kernel_template.mm',
              '../feature/metal/integer_motion_v2_metal.mm',
              '../feature/metal/float_psnr_metal.mm',
              '../feature/metal/float_moment_metal.mm',
              '../feature/metal/integer_psnr_metal.mm',
              '../feature/metal/float_motion_metal.mm',
              '../feature/metal/integer_motion_metal.mm',
              '../feature/metal/float_ssim_metal.mm',
              '../feature/metal/float_ms_ssim_metal.mm',
              '../feature/metal/integer_ssim_metal.mm',
              '../feature/metal/float_vif_metal.mm',
              '../feature/metal/float_adm_metal.mm',
              '../feature/metal/integer_vif_metal.mm',
              '../feature/metal/integer_adm_metal.mm',
              '../feature/metal/integer_ciede_metal.mm',
              '../feature/metal/integer_psnr_hvs_metal.mm',
              '../feature/metal/integer_cambi_metal.mm',
              '../feature/metal/ssimulacra2_metal.mm'),
    ],
    include_directories: metal_objcpp_includes,
    objcpp_args: metal_objcpp_args,
    dependencies: [foundation_dep, metal_dep, iosurface_dep],
)

# Pure-C TUs that still go through the main feature library's normal
# C compile path — appended to metal_sources directly.
# NOTE: The seven batch-1/batch-2 scaffolds (ADR-0361) that previously
# lived here as .c -ENOSYS stubs have been replaced by real .mm
# dispatch files (T8-1d–T8-1j / ADR-0421) in metal_objcpp_lib above.
metal_sources += files(
    'dispatch_strategy.c',
    # integer_adm_metal's host logic without the Metal API (ADR-1806): plain C,
    # shared with core/test/test_metal_integer_adm_host_replay.
    '../feature/metal/integer_adm_metal_host.c',
)

# The Obj-C++ static lib's objects fold into the same feature
# aggregation the parent meson.build consumes (same shape as
# `x86_avx2_static_lib.extract_all_objects()`).
metal_deps += declare_dependency(
    link_with: metal_objcpp_lib,
)

# ---------------------------------------------------------------------
# Metallib compilation (T8-1c / ADR-0421).
#
# Pipeline: `.metal` → `.air` → `.metallib` via xcrun. The compiled
# metallib is embedded into the libvmaf binary's __TEXT,__metallib
# section via the linker flag `-Wl,-sectcreate,__TEXT,__metallib,…`.
# Consumer .mm reads the byte range via linker-defined symbols.
xcrun = find_program('xcrun', required: true)

# ADR-1496: the language revision and the deployment target of the kernels.
# Without -mmacosx-version-min the offline compiler stamps each .air with the
# build machine's SDK version, and a metallib stamped newer than the host's
# macOS refuses to load ("This library is using a deployment target ... that
# is not supported on this OS"): every Metal twin then fails init on a tester's
# Mac whose macOS is older than the hosted runner's SDK. macOS 14 is the floor
# the macOS tester bundle builds its C code for (MACOSX_DEPLOYMENT_TARGET in
# scripts/ci/build-macos-tester-bundle.sh); Metal 3.1 is the language revision
# of macOS 14 (Metal Shading Language Specification 4.1, section 1.6.10).
metal_shader_target_args = ['-std=metal3.1', '-mmacosx-version-min=14.0']

# BEGIN VMAF Metal shader strict FP policy
# ADR-1498: every .metal kernel compiles without fast math and without FP
# contraction. The Metal compiler's default is fast math (no NaNs or INFs,
# reciprocals for division, reassociation, contraction across statements);
# -fno-fast-math (= -fmetal-math-mode=safe -fmetal-math-fp32-functions=precise)
# makes fp32 + - * /, sqrt and fma correctly rounded, and -ffp-contract=off
# stops the contraction safe mode still allows within a statement (Metal
# Shading Language Specification 4.1, sections 1.6.3 and 8.4). The twins
# reproduce the CPU's fp32 operations one rounding at a time, as the CUDA
# (ADR-1403), HIP and SYCL (ADR-1367) policies make theirs do. One list for
# every kernel; no kernel adds a floating-point flag of its own.
metal_shader_strict_fp_args = ['-fno-fast-math', '-ffp-contract=off']
# END VMAF Metal shader strict FP policy

# Kernels include the backend-neutral arithmetic of feature/ and the Metal
# arithmetic headers of feature/metal/ (metal_portable.h), ADR-1498.
metal_shader_include_args = [
    '-I', meson.current_source_dir() / '..' / 'feature',
    '-I', meson.current_source_dir() / '..' / 'feature' / 'metal',
]

# One .air per kernel file (core/src/feature/metal/<name>.metal).
metal_kernel_names = [
    'integer_motion_v2',
    'float_psnr',
    'float_moment',
    'integer_psnr',
    'float_motion',
    'integer_motion',
    'float_ssim',
    # ADR-0490 / ADR-0545: 5-scale float MS-SSIM pyramid.
    'float_ms_ssim',
    # Fixed-point SSIM (feature "ssim"), two-pass separable Gaussian.
    'integer_ssim',
    # 4-scale VIF Gaussian pyramid (feature "float_vif").
    'float_vif',
    # 4-scale DWT + CSF + decouple + CM (feature "float_adm").
    'float_adm',
    # 4-scale fixed-point VIF Gaussian pyramid (feature "vif").
    'integer_vif',
    # Fixed-point 4-scale DWT+CSF+decouple+CM (feature "adm").
    'integer_adm',
    # Standalone metrics (parity sweep): ciede2000, psnr_hvs, cambi, ssimulacra2.
    'integer_ciede',
    'integer_psnr_hvs',
    'integer_cambi',
    'ssimulacra2',
]

metal_air_files = []
foreach _k : metal_kernel_names
    metal_air_files += custom_target(_k + '_air',
        input: '../feature/metal/' + _k + '.metal',
        output: _k + '.air',
        command: [xcrun, '-sdk', 'macosx', 'metal'] + metal_shader_target_args
            + metal_shader_strict_fp_args + metal_shader_include_args
            + ['-c', '@INPUT@', '-o', '@OUTPUT@'],
    )
endforeach

metallib_target = custom_target('libvmaf-metallib',
    input: metal_air_files,
    output: 'default.metallib',
    command: [xcrun, '-sdk', 'macosx', 'metallib',
              '@INPUT@', '-o', '@OUTPUT@'],
)

metal_deps += declare_dependency(
    sources: metallib_target,
    link_args: ['-Wl,-sectcreate,__TEXT,__metallib,' +
                metallib_target.full_path()],
)
