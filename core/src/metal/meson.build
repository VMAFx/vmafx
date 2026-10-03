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
] + vmaf_strict_fp_args
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
