compat_cflags = []
if cc.has_function('strsep', args : test_args)
  compat_cflags += '-DHAVE_STRSEP'
endif

if not get_option('enable_tools')
    subdir_done()
endif

# getopt_dependency is declared in libvmaf/meson.build: an empty list on
# POSIX / MinGW hosts, or a declare_dependency that pulls in the Win32
# header-only + companion-source shim on MSVC / clang-cl hosts.
vmaf_tool_deps = [stdatomic_dependency, cuda_dependency, getopt_dependency]
vmaf_tool_cflags = [vmaf_cflags_common, compat_cflags]
vmaf_tool_link_args = []
if get_option('enable_cuda')
    vmaf_tool_cflags += ['-DHAVE_CUDA=1']
endif
if get_option('enable_sycl')
    vmaf_tool_cflags += ['-DHAVE_SYCL=1']
    vmaf_tool_deps += sycl_dependency
endif
if get_option('enable_hip')
    # ADR-0514: the vmaf CLI gates HIP support on `#ifdef HAVE_HIP` (see
    # `core/tools/vmaf.cpp` — includes of `libvmaf_hip.h`, init/cleanup
    # of `VmafHipState`, the `--backend hip` strict-mode arm). Without
    # this define the CLI compiles the "built without hip support"
    # strict-mode error path even when the library itself was built with
    # HIP enabled — exactly the failure mode that the dev-MCP container
    # surfaced when `-Denable_hip=true` was set but `vmaf --backend hip`
    # still refused.
    vmaf_tool_cflags += ['-DHAVE_HIP=1']
endif

vmaf_tool_link_target = libvmaf
if get_option('default_library') == 'both'
    vmaf_tool_link_target = libvmaf.get_shared_lib()
endif

# ADR-0809: vmaf.c and cli_parse.c converted to C++23 (vmaf.cpp, cli_parse.cpp).
# The two C++ TUs use override_options to pin cpp_std independently of the
# project-level default, matching the per-target pattern established in ADR-0708.
# libvmaf_cpu_cpp_std is defined in src/meson.build (ADR-0860 follow-up):
#   'vc++latest' on MSVC, 'c++latest' on clang-cl/icx-cl, 'c++23' elsewhere.
# y4m_input.c / vidinput.c / yuv_input.c remain C23; they are compiled under the
# project default (c_args only) and linked together with the C++ TUs.
vmaf = executable(
    'vmaf',
    ['vmaf.cpp', 'cli_parse.cpp', 'y4m_input.c', 'vidinput.c', 'yuv_input.c'],
    include_directories : [libvmaf_inc, vmaf_include],
    dependencies: vmaf_tool_deps,
    c_args : vmaf_tool_cflags,
    cpp_args : vmaf_tool_cflags,
    link_args : vmaf_tool_link_args,
    link_with : vmaf_tool_link_target,
    override_options : ['cpp_std=' + libvmaf_cpu_cpp_std],
    install : true,
)

# vmaf-perShot — T6-3b per-shot CRF predictor sidecar (ADR-0222).
# Standalone tool: only links against libvmaf for future transnet_v2
# integration; v1 itself uses libc-only frame-difference heuristic.
vmaf_per_shot_deps = [stdatomic_dependency, getopt_dependency]
vmaf_per_shot_cflags = [vmaf_cflags_common, compat_cflags]
vmaf_per_shot_link_target = libvmaf
if get_option('default_library') == 'both'
    vmaf_per_shot_link_target = libvmaf.get_shared_lib()
endif

vmaf_per_shot = executable(
    'vmaf-perShot',
    ['vmaf_per_shot.c'],
    include_directories : [libvmaf_inc, vmaf_include],
    dependencies: vmaf_per_shot_deps,
    c_args : vmaf_per_shot_cflags,
    link_with : vmaf_per_shot_link_target,
    install : true,
)

# vmaf-roi — T6-2b per-CTU QP offset sidecar (x265 / SVT-AV1 ROI).
vmaf_roi = executable(
    'vmaf_roi',
    ['vmaf_roi.c'],
    include_directories : [libvmaf_inc, vmaf_include],
    dependencies : [stdatomic_dependency, getopt_dependency],
    c_args : vmaf_tool_cflags,
    link_args : vmaf_tool_link_args,
    link_with : vmaf_tool_link_target,
    install : true,
)

vmaf_bench_deps = [stdatomic_dependency]
vmaf_bench_cflags = vmaf_cflags_common
vmaf_bench_link_args = []
if get_option('enable_cuda')
    vmaf_bench_deps += cuda_dependency
    vmaf_bench_cflags += ['-DHAVE_CUDA=1']
endif
if get_option('enable_sycl')
    vmaf_bench_deps += sycl_dependency
    vmaf_bench_cflags += ['-DHAVE_SYCL=1']
endif

vmaf_bench_link_target = get_option('default_library') == 'both' ? libvmaf.get_shared_lib() : libvmaf

vmaf_bench = executable(
    'vmaf_bench',
    ['vmaf_bench.c'],
    include_directories : [libvmaf_inc, vmaf_include],
    dependencies: vmaf_bench_deps,
    c_args : vmaf_bench_cflags,
    link_args : vmaf_bench_link_args,
    link_with : vmaf_bench_link_target,
)

# VPL decode → SYCL pipeline tool
# Requires SYCL + VA-API + Level Zero + VPL
if get_option('enable_sycl')
    vpl_dep = dependency('vpl', required : false)

    vmaf_vpl_deps = [stdatomic_dependency, vpl_dep]
    vmaf_vpl_ok = vpl_dep.found()

    # Linux: VA-API + libva-drm
    libva_dep_tools = dependency('libva', required : false)
    libva_drm_dep_tools = dependency('libva-drm', required : false)
    vmaf_vpl_ok = vmaf_vpl_ok and libva_dep_tools.found() and libva_drm_dep_tools.found()
    if vmaf_vpl_ok
        vmaf_vpl_deps += [libva_dep_tools, libva_drm_dep_tools]
    endif

    if vmaf_vpl_ok
        vmaf_vpl = executable(
            'vmaf_vpl',
            ['vmaf_vpl.c'],
            include_directories : [libvmaf_inc, vmaf_include],
            dependencies: vmaf_vpl_deps,
            c_args : vmaf_cflags_common + ['-DHAVE_SYCL=1'],
            link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
        )
        message('vmaf_vpl tool: enabled (VPL + VA-API + SYCL)')
    else
        message('vmaf_vpl tool: disabled (missing VPL or platform surface deps)')
    endif
endif

subdir('test')
