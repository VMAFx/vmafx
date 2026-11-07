project('vk-mem-alloc', 'cpp',
    version : '3.3.0',
    license : 'MIT',
    meson_version : '>= 0.56.1',
    default_options : ['cpp_std=c++17', 'warning_level=0'])

# VMA is a single-header C++ library. We only export the include path
# here; one TU in libvmaf provides the implementation by `#define
# VMA_IMPLEMENTATION` before `#include "vk_mem_alloc.h"`. Tying the
# include path to a meson dependency keeps the wrap reproducible
# (single sha256 pin) without forking VMA's CMakeLists.txt.
vma_inc = include_directories('include')

vk_mem_alloc_dep = declare_dependency(
    include_directories : vma_inc,
    # VMA does not need any preprocessor switches by default; we
    # opt into VMA_VULKAN_VERSION / VMA_DEDICATED_ALLOCATION via the
    # implementation TU's `#define` block in libvmaf/src/vulkan/.
)
