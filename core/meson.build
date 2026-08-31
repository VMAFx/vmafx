project('libvmaf', ['c', 'cpp'],
    version : '4.0.0-lusoris.0', # x-release-please-version
    # c_std is deliberately NOT set here — see the compiler-aware block
    # below. MSVC advertises only ['none','c89','c99','c11'], so any
    # value this project actually wants aborts configure before that
    # block can run.
    default_options : [
                       'warning_level=2',
                       'buildtype=release',
                       'default_library=both',
                       'b_lto=true',
                      ],
    # >= 1.4.0: `c_std=c23` above is only recognised from meson 1.4.0 on
    # (1.3.2, which Ubuntu 24.04 ships, fails with "Unknown C std ['c23']").
    # Declaring it here turns that cryptic configure error into a clear
    # "meson version too old" message. See ADR-0692.
    meson_version: '>= 1.4.0')

vmaf_soname_version       = '3.0.0'
vmaf_api_version_array    = vmaf_soname_version.split('.')
vmaf_api_version_major    = vmaf_api_version_array[0]
vmaf_api_version_minor    = vmaf_api_version_array[1]
vmaf_api_version_revision = vmaf_api_version_array[2]

libvmaf_src_root = meson.current_source_dir()
cc = meson.get_compiler('c')
cxx = meson.get_compiler('cpp')
libvmaf_inc = include_directories(['include'])

# Select the C standard flag by compiler identity, unless the caller supplied
# an explicit -Dc_std override.  This mirrors the cpp_std handling below
# (ADR-1056) and exists for the same reason: the accepted-values list meson
# advertises for c_std is compiler-specific, and putting a value in
# default_options aborts configure on any toolchain missing that exact
# spelling — before any conditional in this file can run.  Observed lists:
#   - MSVC (cl.exe):        ['none', 'c89', 'c99', 'c11']  -> use /std:clatest
#   - GCC 13 (ubuntu-24.04) ['none', ..., 'c17', 'c18', 'c2x', 'gnu2x']
#   - GCC 14+ / Clang 18+   ... plus 'c23'
# ADR-0692 raised this project to C23; 'c2x' is the pre-ratification
# spelling of the same standard, so the fallback is a spelling change, not a
# language downgrade.
c_std_args = []
if get_option('c_std') == 'none'
    if cc.get_id() == 'msvc'
        c_std_args = ['/std:clatest']
    elif cc.has_argument('-std=c23')
        c_std_args = ['-std=c23']
    else
        c_std_args = ['-std=c2x']
    endif
    # NOTE: add_project_arguments() reaches the build but NOT meson's compiler
    # feature checks (cc.check_header / cc.compiles / cc.has_function), which
    # only pick up built-in options and env CFLAGS.  The standard therefore has
    # to be threaded into those checks explicitly via test_args below — with
    # c_std removed from default_options, MSVC otherwise ran the stdatomic.h
    # check in legacy C mode and tripped
    # vcruntime_c11_stdatomic.h: #error "C atomics require C11 or later",
    # which surfaced as the misleading `ERROR: Problem encountered: Atomics
    # not supported`.
    add_project_arguments(c_std_args, language: 'c')
endif

# Select the C++ standard flag based on compiler identity, unless the caller
# already supplied an explicit -Dcpp_std override (e.g. the SYCL CI leg
# passes -Dcpp_std=c++latest for icx-cl under MSVC ABI mode).  When cpp_std is
# 'none' (the meson default when
# not overridden) we inject the flag directly via add_project_arguments:
#   - MSVC (cl.exe / clang-cl): /std:c++latest — 'c++23' is absent from the
#     accepted-values list meson's MSVC backend advertises; /std:c++latest
#     enables C++23 features on VS 2022 17.x+.
#   - Everything else: the newest of -std=c++26 / c++23 / c++2b the compiler
#     actually accepts.  The flag was previously hard-coded to c++26, which
#     GCC 13 (the ubuntu-latest default) rejects outright with
#     "unrecognized command-line option '-std=c++26'"; that break was
#     invisible for as long as the c23 defect below aborted configure before
#     any C++ TU was reached.  c++26 landed in GCC 14 / Clang 17, and c++23
#     in GCC 13 only under the pre-ratification c++2b spelling.
# If cpp_std is already set (any non-'none' value), meson generates the
# appropriate flag itself and we stay out of the way to avoid conflicts.
# See ADR-1056 (docs/adr/1056-msvc-cpp-std.md).
if get_option('cpp_std') == 'none'
    if cxx.get_id() == 'msvc'
        add_project_arguments('/std:c++latest', language: 'cpp')
    else
        # Probing `has_argument` alone is NOT enough: clang 18.1.3 *accepts*
        # -std=c++26 but its accompanying libstdc++ then fails to provide
        # std::expected, so core/src/dict.cpp dies with "no member named
        # 'expected' in namespace 'std'".  A flag the driver tolerates is not
        # the same as a standard library that works.  So compile-test the one
        # C++23 library feature this tree actually depends on (<expected>;
        # the only other non-C++17 header in the tree is <span>, which is
        # C++20) and take the newest candidate that genuinely builds.
        # c++2c is GCC's pre-ratification spelling of c++26, kept in the
        # cascade for the same reason c2x is on the C side.
        cxx_std_probe = '''#include <expected>
        std::expected<int, int> f() { return 1; }
        int main() { return f().value_or(1) - 1; }
        '''
        cxx_std_flag = ''
        foreach candidate : ['-std=c++26', '-std=c++2c', '-std=c++23', '-std=c++2b']
            if cxx_std_flag == ''
                if cxx.has_argument(candidate) and cxx.compiles(
                        cxx_std_probe,
                        args: candidate,
                        name: 'std::expected usable with ' + candidate)
                    cxx_std_flag = candidate
                endif
            endif
        endforeach
        if cxx_std_flag == ''
            error('No -std flag gives this compiler a working <expected>: ' + cxx.get_id())
        endif
        add_project_arguments(cxx_std_flag, language: 'cpp')
    endif
endif

# Arguments in test_args will be used even on feature tests.  It is seeded
# with the C standard flag chosen above; see the note there for why that is
# load-bearing rather than cosmetic.
test_args = c_std_args
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
        include_directories : include_directories('tools/compat/win32', 'tools'),
        sources : files('tools/compat/win32/getopt.c'),
    )
endif

subdir('include')
subdir('src')
subdir('tools')
subdir('doc')
subdir('test')
