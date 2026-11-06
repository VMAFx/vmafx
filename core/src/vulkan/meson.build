# Copyright 2026 luoye-cpu
# SPDX-License-Identifier: EUPL-1.2

# Included from core/src/meson.build via `subdir('vulkan')`. Paths below are
# relative to core/src/vulkan/. ADR-3401: the integer path's bring-up —
# header-only probe, dynamic-loader compile flags and the shader embed chain.

fs = import('fs')

# The runtime loads `vulkan-1` / `libvulkan.so.1` dynamically (VK_NO_PROTOTYPES,
# no volk, no VMA), so the build needs the Vulkan headers only. Probe pkg-config
# first, then the explicit vulkan_include_dir option; fail the configure with
# instructions when neither resolves.
vulkan_dep = dependency('vulkan', required: false)
if vulkan_dep.found()
    # Headers only: no link flags — nothing may link against vulkan-1.
    vulkan_header_dep = vulkan_dep.partial_dependency(compile_args : true,
                                                      includes : true)
else
    vulkan_sdk_inc = get_option('vulkan_include_dir')
    if vulkan_sdk_inc == ''
        error('enable_vulkan=true but the Vulkan headers are not found via ' +
              'pkg-config. Install the Vulkan SDK (or the distro ' +
              'vulkan-headers package) or pass -Dvulkan_include_dir=<path to ' +
              'the directory that contains vulkan/vulkan.h>.')
    endif
    vulkan_header_dep = declare_dependency(compile_args : ['-I' + vulkan_sdk_inc])
endif
if not cc.has_header('vulkan/vulkan.h', dependencies : vulkan_header_dep)
    error('vulkan/vulkan.h not found: pass -Dvulkan_include_dir=<path to the ' +
          'directory that contains vulkan/vulkan.h>.')
endif
vulkan_target_deps += vulkan_header_dep

# Every TU that includes <vulkan/vulkan.h> compiles with VK_NO_PROTOTYPES:
# the function table (vulkan_loader.h) is resolved through the dynamic loader
# at run time, never linked.
vmaf_cflags_common += '-DVK_NO_PROTOTYPES'

vulkan_sources += files('common.c')

# ADR-3401: compile the GLSL compute shaders to SPIR-V at build time and embed
# them as C sources (spv_embed.py) — the same shape the HIP backend uses for
# its xxd-generated HSACO embeds, with the compiled TU carrying the embed
# dependency itself. No runtime file lookup.
glslangvalidator_exe = find_program('glslangValidator', required: false)
glslc_exe = find_program('glslc', required: false)
glslang_exe = find_program('glslang', required: false)
if not glslangvalidator_exe.found() and not glslc_exe.found() and not glslang_exe.found()
    error('enable_vulkan=true but no GLSL-to-SPIR-V compiler found: install ' +
          'glslangValidator (the glslang package) or glslc (the Vulkan SDK).')
endif

# SPIR-V emitted for the Vulkan 1.1 environment (SPIR-V 1.3): the shader needs
# Int64 and nothing newer, so Vulkan 1.1 devices consume the module as-is and
# the fp64-free ADR-3401 path keeps its broad device coverage. glslangValidator
# and glslang infer the compute stage from the .comp extension.
vulkan_spv_embedder = import('python').find_installation()
foreach shader : ['../feature/vulkan/shaders/integer_adm.comp']
    name = fs.name(shader).split('.')[0]
    if glslangvalidator_exe.found()
        spv = custom_target(name + '_spv',
            input : shader,
            output : name + '.spv',
            command : [glslangvalidator_exe, '-V', '--target-env', 'vulkan1.1',
                       '@INPUT@', '-o', '@OUTPUT@'],
            build_by_default : true,
        )
    elif glslc_exe.found()
        spv = custom_target(name + '_spv',
            input : shader,
            output : name + '.spv',
            command : [glslc_exe, '--target-env=vulkan1.1', '@INPUT@', '-o', '@OUTPUT@'],
            build_by_default : true,
        )
    else
        spv = custom_target(name + '_spv',
            input : shader,
            output : name + '.spv',
            command : [glslang_exe, '-S', 'comp', '--target-env', 'vulkan1.1',
                       '@INPUT@', '-o', '@OUTPUT@'],
            build_by_default : true,
        )
    endif
    embed = custom_target(name + '_spv_embed',
        input : spv,
        output : name + '_spv.c',
        command : [vulkan_spv_embedder, files('spv_embed.py'), '@INPUT@', '@OUTPUT@', name + '_spv'],
        build_by_default : true,
    )
    vulkan_sources += embed
endforeach
