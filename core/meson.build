project('libvmaf', ['c', 'cpp'],
    version : '3.2.1', # x-release-please-version
    # c_std is deliberately NOT set here — see the compiler-aware block
    # below. MSVC advertises only ['none','c89','c99','c11'], so any
    # value this project actually wants aborts configure before that
    # block can run.
    default_options : [
                       'warning_level=3',
                       'werror=true',
                       # Meson's ordered standard lists select the first spelling
                       # supported by the active compiler without emitting the
                       # warnings caused by manual -std=/std: project arguments.
                       'c_std=c23,c2x,c11',
                       'cpp_std=c++23,c++2b,c++latest,c++20',
                       'buildtype=release',
                       'default_library=both',
                       'b_lto=true',
                       # ADR-1172: bound the per-link LTO partition parallelism. GCC's
                       # plain -flto means -flto=auto = every core per link; ninja -j N
                       # then runs N links x nproc ltrans jobs and load hits 5x the core
                       # count on a workstation. -flto=4 keeps LTO and caps it.
                       'b_lto_threads=4',
                      ],
    # >= 1.4.0: the explicit `-Dc_std=c23` override is only recognised from
    # meson 1.4.0 on (1.3.2 fails with "Unknown C std ['c23']"). Declaring the
    # floor here turns that cryptic configure error into a clear version error.
    # See ADR-0692.
    meson_version: '>= 1.4.0')

# ABI SONAME - deliberately INDEPENDENT of the release-please-owned product
# version on line 2. libvmaf.so keeps its own 3.x line (libvmaf.so.3) and is
# hand-bumped only on a C API change; the fork's first product release resets
# the tag / Python / Helm / image version to 1.0.0 and does NOT touch this.
# This value is ALSO what libvmaf.pc advertises as its `Version:` field, because
# downstream gates on the interface, not on the product: unpatched upstream
# FFmpeg requires `libvmaf >= 2.0.0` and the fork's ffmpeg-patches require
# `libvmaf >= 3.0.0`. Do not 'align' the two - see ADR-1151 and ADR-1235.
vmaf_soname_version       = '3.0.0'
vmaf_api_version_array    = vmaf_soname_version.split('.')
vmaf_api_version_major    = vmaf_api_version_array[0]
vmaf_api_version_minor    = vmaf_api_version_array[1]
vmaf_api_version_revision = vmaf_api_version_array[2]

libvmaf_src_root = meson.current_source_dir()
cc = meson.get_compiler('c')
cxx = meson.get_compiler('cpp')
libvmaf_inc = include_directories(['include'])

# Meson's built-in c_std/cpp_std options apply to compiler feature checks as
# well as project targets. This keeps configure output warning-free and avoids
# the old split where checks ran under a different language mode than builds.
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
if not cc.check_header('stdatomic.h', args : test_args)
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
if not cc.check_header('pthread.h', args : test_args)
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
if not cc.check_header('getopt.h', args : test_args)
    getopt_dependency = declare_dependency(
        include_directories : include_directories('tools/compat/win32', 'tools', 'src'),
        sources : files('tools/compat/win32/getopt.c'),
    )
endif

subdir('include')
subdir('src')
subdir('tools')
subdir('doc')
subdir('test')
