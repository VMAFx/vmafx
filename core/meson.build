project('libvmaf', ['c', 'cpp'],
    version : '3.2.1', # x-release-please-version
    default_options : ['c_std=c23',
                       'warning_level=2',
                       'buildtype=release',
                       'default_library=both',
                       'b_lto=true',
                      ],
    meson_version: '>= 0.58.0')

vmaf_soname_version       = '3.0.0'
vmaf_api_version_array    = vmaf_soname_version.split('.')
vmaf_api_version_major    = vmaf_api_version_array[0]
vmaf_api_version_minor    = vmaf_api_version_array[1]
vmaf_api_version_revision = vmaf_api_version_array[2]

libvmaf_src_root = meson.current_source_dir()
cc = meson.get_compiler('c')
cxx = meson.get_compiler('cpp')
libvmaf_inc = include_directories(['include'])

# Select the C++ standard flag based on compiler identity, unless the caller
# already supplied an explicit -Dcpp_std override (e.g. the SYCL CI leg
# passes -Dcpp_std=c++14 for icx-cl under MSVC ABI mode, which drops c++23
# from its supported list).  When cpp_std is 'none' (the meson default when
# not overridden) we inject the flag directly via add_project_arguments:
#   - MSVC (cl.exe / clang-cl): /std:c++latest — 'c++23' is absent from the
#     accepted-values list meson's MSVC backend advertises; /std:c++latest
#     enables C++23 features on VS 2022 17.x+.
#   - Everything else (GCC 11+, Clang 14+, icpx/dpcpp on Linux): -std=c++26.
# If cpp_std is already set (any non-'none' value), meson generates the
# appropriate flag itself and we stay out of the way to avoid conflicts.
# See ADR-1056 (docs/adr/1056-msvc-cpp-std.md).
if get_option('cpp_std') == 'none'
    if cxx.get_id() == 'msvc'
        add_project_arguments('/std:c++latest', language: 'cpp')
    else
        add_project_arguments('-std=c++26', language: 'cpp')
    endif
endif

# Arguments in test_args will be used even on feature tests
test_args = []
if host_machine.system() == 'linux'
    test_args += '-D_GNU_SOURCE'
    add_project_arguments('-D_GNU_SOURCE', language: ['c', 'cpp'])
elif host_machine.system() == 'darwin'
    test_args += '-D_DARWIN_C_SOURCE'
    add_project_arguments('-D_DARWIN_C_SOURCE', language: ['c', 'cpp'])
endif

# Header checks
stdatomic_dependency = []
if not cc.check_header('stdatomic.h')
    if cc.compiles('''int main() { int v = 0; return __atomic_fetch_add(&v, 1, __ATOMIC_SEQ_CST); }''',
                     name : 'GCC-style atomics', args : test_args)
        stdatomic_dependency = declare_dependency(
            include_directories : include_directories('src/compat/gcc'),
        )
    else
        error('Atomics not supported')
    endif
endif

# Windows MSVC / clang-cl ships no <pthread.h>. libvmaf uses pthread mutex /
# cond / thread-create across the core (~14 files), so resolve the include
# locally via a header-only Win32 SRWLOCK / CONDITION_VARIABLE shim. MinGW
# ships its own winpthreads, so check_header() succeeds there and the shim
# stays out of the include path. See src/compat/win32/pthread.h and
# docs/adr/0121-windows-gpu-build-only-legs.md for the rationale.
pthread_dependency = []
if not cc.check_header('pthread.h')
    pthread_dependency = declare_dependency(
        include_directories : include_directories('src/compat/win32'),
    )
endif

# Windows MSVC / clang-cl ships no <getopt.h>. The CLI tools (vmaf, cli_parse)
# and the test_cli_parse unit-test target use getopt_long, so resolve both
# declarations and the companion implementation via a POSIX/GNU-compatible
# shim under tools/compat/win32/. The dependency auto-propagates the shim .c
# into any target that consumes it. MinGW ships getopt.h via mingw-w64-crt,
# so check_header() succeeds there and the shim stays out of the build.
getopt_dependency = []
if not cc.check_header('getopt.h')
    getopt_dependency = declare_dependency(
        include_directories : include_directories('tools/compat/win32', 'tools'),
        sources : files('tools/compat/win32/getopt.c'),
    )
endif

subdir('include')
subdir('src')
subdir('tools')
subdir('doc')
subdir('test')
