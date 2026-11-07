# Included from core/src/meson.build via `subdir('vulkan')` when
# `-Denable_vulkan=enabled` (ADR-3402). Paths below are relative to
# core/src/vulkan/. The integer ADM kernel lives under
# core/src/feature/vulkan/shaders/, the device probe under
# core/src/feature/vulkan/probe/shaders/.
#
# Trimmed from the pre-ADR-0726 wiring (snapshot 816a5f33f): the
# floating-point shader matrix, the two-level reduction shaders and the
# import.c entry points are NOT restored (ADR-0726 removal stands for
# everything outside the opt-in integer path).
#
#  - volk: thin Vulkan function loader (dlopen at runtime; wrap-pinned
#    vulkan-sdk-1.4.341.0, ADR-0198 private-remap).
#  - vk-mem-alloc (VMA): Vulkan Memory Allocator, single-header, v3.3.0.
#  - GLSL -> SPIR-V: glslang (what the PoC pinned for byte-for-byte
#    SPIR-V reproducibility) or glslc/shaderc, whichever is on PATH;
#    configured at setup time, no runtime JIT.

# Locally imported: the parent meson.build does not export `py3` / `fs`
# as named variables (it imports them inline at each use site).
py3 = import('python').find_installation()
fs = import('fs')

vulkan_sdk_dep = dependency('vulkan', required: true)
volk_dep = dependency('volk', fallback: ['volk', 'volk_dep'])
vma_dep = dependency('vk-mem-alloc', fallback: ['vk-mem-alloc', 'vk_mem_alloc_dep'])

# ADR-0200: pull the volk private-remap header path from the wrap and
# apply `-include <path>` privately on libvmaf's targets via
# `vmaf_cflags_common`. (Adding it inside `subdir('vulkan')` ended up
# being silently dropped — meson's variable-propagation behaves oddly
# when the addition is via an `subproject().get_variable()` lookup
# inside a subdir.) The parent `core/src/meson.build` does the actual
# `+=` directly after `subdir('vulkan')` for scope-propagation safety.
volk_subproj = subproject('volk')
volk_priv_remap_h_path = volk_subproj.get_variable('volk_priv_remap_h_path')

# GLSL -> SPIR-V compiler. ADR-3402 open item: toolchain pin. The PoC
# gate used standalone glslang 16.6.0 (`-V --target-env vulkan1.3`) with
# a byte-for-byte SPIR-V reproducibility guarantee; the Vulkan SDK's
# glslc is the equivalent shape (`--target-env=vulkan1.3`). Either is
# accepted here; setup fails with a clear message when neither exists.
glslang_exe = find_program('glslang', required: false)
glslc_exe = find_program('glslc', required: false)
if glslang_exe.found()
    vulkan_spirv_compiler = glslang_exe
    vulkan_spirv_args = ['-V', '--target-env', 'vulkan1.3']
elif glslc_exe.found()
    vulkan_spirv_compiler = glslc_exe
    vulkan_spirv_args = ['--target-env=vulkan1.3']
else
    error('enable_vulkan=enabled requires a GLSL -> SPIR-V compiler (glslang, or glslc from the Vulkan SDK / shaderc) in PATH. See ADR-3402 (toolchain pin is an open item).')
endif
message('Vulkan GLSL compiler: ' + vulkan_spirv_compiler.full_path())

# Compute shaders -> SPIR-V -> embedded C header. Each `.comp` becomes
# a `<name>_spv.h` header declaring `static const uint32_t <name>_spv[]`
# for direct embedding into the host-side dispatch (spv_embed.py).
#
# Only the integer ADM kernel and the device probe ship in this PR. The
# removed tree's `integer_adm_reduce.comp` / `vif_reduce.comp` are
# deliberately NOT restored: their `subgroupAdd(int64_t)` is banned by
# ADR-3402, and the in-tree integer ADM folds the stage-3 reduction into
# the main shader as a shared-memory serial sum.
vulkan_shader_sources = [
    '../feature/vulkan/shaders/integer_adm.comp',
    '../feature/vulkan/probe/shaders/probe.comp',
]

vulkan_shader_headers = []
foreach shader : vulkan_shader_sources
    shader_name = fs.name(shader).split('.')[0]
    spv = custom_target(shader_name + '.spv',
        input: shader,
        output: shader_name + '.spv',
        command: [vulkan_spirv_compiler] + vulkan_spirv_args + ['@INPUT@', '-o', '@OUTPUT@'],
    )
    embed = custom_target(shader_name + '_spv.h',
        input: spv,
        output: shader_name + '_spv.h',
        command: [py3, meson.current_source_dir() / 'spv_embed.py', '@INPUT@', '@OUTPUT@', shader_name],
    )
    vulkan_shader_headers += embed
endforeach

# VMA requires C++17 (aligned_alloc); libvmaf's project default is newer.
# Compile the VMA implementation TU as its own static lib so the cpp_std
# override doesn't leak into libvmaf's other C++ TUs. vma_impl.cpp also
# needs the volk priv-remap so its `vk*` calls bind to volk's
# `vmaf_priv_vk*` PFN dispatchers.
vma_impl_lib = static_library(
    'vma_impl',
    'vma_impl.cpp',
    override_options: ['cpp_std=c++17', 'warning_level=0'],
    cpp_args: ['-include', volk_priv_remap_h_path],
    dependencies: [vulkan_sdk_dep, volk_dep, vma_dep],
    pic: true,
    install: false,
)

# Runtime subset (13 of the 15 snapshot files; import.c/import_picture.h
# are NOT restored — superseded by the ADR-2152 import surface, and the
# opt-in path is host upload/download only). Includes the SPIR-V embed
# headers; the feature TU core/src/feature/vulkan/integer_adm_vulkan.c
# is added to libvmaf_feature_sources in core/src/meson.build under the
# same guard (it consumes these headers via the `vulkan` include dir).
vulkan_sources += files(
    'common.c',
    'dispatch_strategy.c',
    'picture_vulkan.c',
    'picture_vulkan_pool.c',
) + vulkan_shader_headers

vulkan_deps += vulkan_sdk_dep
vulkan_deps += volk_dep
vulkan_deps += vma_dep
vulkan_deps += declare_dependency(link_with: vma_impl_lib)
