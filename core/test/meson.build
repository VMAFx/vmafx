if not get_option('enable_tests')
    subdir_done()
endif

# ADR-0707: rust_tad_dep is available here (defined in src/meson.build via
# subdir('src') before subdir('test')). Only test_tad_rust uses it directly —
# other tests link against libvmaf.so (shared) which already carries the Rust
# archive, so no further propagation is needed.

test_inc = include_directories('.')

# Shared-device scheduling is part of Meson's public test metadata.  This check
# reads the configured test registry and fails if any `gpu` suite member can run
# concurrently with another test.
test('check_gpu_test_serialization',
    import('python').find_installation(),
    args : [files('check_gpu_test_serialization.py'),
            meson.global_build_root() / 'meson-info' / 'intro-tests.json',
            meson.project_name() + ':gpu'],
    suite : ['fast'],
)

test('test_gpu_serialization_contract',
    import('python').find_installation(),
    args : files('test_gpu_serialization_contract.py'),
    suite : ['fast'],
)

# ADR-1333: Guard Meson test environment sanitization so secret-bearing GitHub
# credential variables are excluded from test environments and testlog.json logs.
test('test_meson_secret_env_sanitization',
    import('python').find_installation(),
    args : files('test_meson_secret_env_sanitization.py'),
    suite : ['fast'],
    timeout : 120,
)

# ADR-1364: the runner must return meson test's status, and only once it has
# finished. Windows has no exec, so os.execvp there ended the runner with 0.
test('test_run_meson_test_exit_status',
    import('python').find_installation(),
    args : files('test_run_meson_test_exit_status.py'),
    suite : ['fast'],
    timeout : 90,
)

# Execute Windows discovery with stubbed tools on POSIX build hosts. This
# exercises Meson control flow without requiring a Windows SDK or CUDA device.
if build_machine.system() != 'windows'
    test('test_windows_cuda_compiler_discovery',
        import('python').find_installation(),
        args : files('test_windows_cuda_compiler_discovery.py'),
        suite : ['fast'],
        timeout : 90,
    )
endif

# The configure command scripts/setup/* print at the end must name Meson's
# core/ source directory; the repository root has no meson.build.
test('test_setup_scripts_configure_hint',
    import('python').find_installation(),
    args : files('test_setup_scripts_configure_hint.py'),
    suite : ['fast'],
    timeout : 30,
)

# Execute the compiler-ID policy that selects strict floating-point flags in
# src/meson.build. The fixture catches GCC-style flags leaking into MSVC-style
# drivers without requiring a Windows SDK or Intel compiler on this host.
test('test_strict_fp_compiler_args',
    import('python').find_installation(),
    args : files('test_strict_fp_compiler_args.py'),
    # ADR-1461: the test also reads this build's compile_commands.json.
    env : ['VMAF_STRICT_FP_BUILD_ROOT=' + meson.project_build_root()],
    suite : ['fast'],
    timeout : 90,
)

# ADR-1495: an icx / icpx build links glibc's libm, not Intel's libimf, so its
# CPU scores are a GCC build's. Reads this build's libvmaf.so and vmaf and runs
# vmaf --version under the dynamic loader's binding trace; the binary checks
# skip, with the reason, when neither compiler is Intel LLVM.
icx_libm_env = ['VMAF_ICX_LIBM_BUILD_ROOT=' + meson.project_build_root()]
icx_libm_depends = []
if get_option('default_library') != 'static'
    icx_libm_shared = get_option('default_library') == 'both' ? libvmaf.get_shared_lib() : libvmaf
    icx_libm_env += ['VMAF_ICX_LIBM_LIBRARY=' + icx_libm_shared.full_path()]
    icx_libm_depends += [icx_libm_shared]
endif
if get_option('enable_tools')
    icx_libm_env += ['VMAF_ICX_LIBM_CLI=' + vmaf.full_path()]
    icx_libm_depends += [vmaf]
endif
test('test_icx_system_libm',
    import('python').find_installation(),
    args : files('test_icx_system_libm.py'),
    env : icx_libm_env,
    depends : icx_libm_depends,
    suite : ['fast'],
    timeout : 90,
)

# ADR-1360: the build-time SYCL AOT image check, driven by synthetic ELF
# fixtures so it runs on every host, oneAPI or not.
test('test_sycl_aot_image_check',
    import('python').find_installation(),
    args : files('test_sycl_aot_image_check.py'),
    suite : ['fast'],
    timeout : 30,
)

# SYCL runtime (core/src/sycl/): the state is constructed from the queues of
# the selected device (a default-constructed sycl::queue touches the default
# device), a type C and C++ both read has one definition (no C++-only enum
# underlying type), and the de-tile submits of the VA import run under a try.
# Device-free.
test('test_sycl_runtime_contract',
    import('python').find_installation(),
    args : files('test_sycl_runtime_contract.py'),
    suite : ['fast'],
    timeout : 30,
)

# ADR-1470: an enum that C and C++ translation units both see has one size.
# No header a C translation unit includes may fix an enum's underlying type
# narrower or wider than int (C on the required toolchains cannot spell it,
# so the enum would be int-sized in C and another size in C++), and a C++
# `unsigned int` spelling needs a UINT_MAX enumerator. Device-free and
# compiler-free.
test('test_c_cxx_enum_definition_contract',
    import('python').find_installation(),
    args : files('test_c_cxx_enum_definition_contract.py'),
    suite : ['fast'],
    timeout : 60,
)

# The Cython extension (adm_dwt2_cy.pyx) re-declares adm.c's static
# init_dwt_band_d(); a stale declaration generated a call no C compiler accepts
# and failed every hosted job that builds the wheel. Device-free and
# compiler-free.
test('test_cython_adm_dwt_band_decl_contract',
    import('python').find_installation(),
    args : files('test_cython_adm_dwt_band_decl_contract.py'),
    suite : ['fast'],
    timeout : 60,
)

# ADR-1468: a SYCL kernel may require only a sub-group size every target of
# the default sycl_icpx_aot_targets list accepts (16 or 32; Xe2 has no 8).
# Device-free and compiler-free: it reads the option's default, the check in
# sycl_compat.h and every size the SYCL sources require. The compile itself is
# test_sycl_aot_default_targets (suite sycl-aot), further down.
test('test_sycl_sub_group_size_contract',
    import('python').find_installation(),
    args : files('test_sycl_sub_group_size_contract.py'),
    suite : ['fast'],
    timeout : 30,
)

# ADR-1364: the cross-backend gates read vmaf's JSON by the alias.c name.
test('test_parity_gate_metric_names',
    import('python').find_installation(),
    args : files('test_parity_gate_metric_names.py'),
    suite : ['fast'],
    timeout : 30,
)

# ADR-1460: every twin feature_extractor_list[] registers for a backend the
# parity gate runs (cuda, sycl, hip) is the extractor of a gate feature, a
# backend the gate cannot run is on record, and both gate scripts carry the
# same tables. Device-free.
test('test_parity_gate_covers_registered_twins',
    import('python').find_installation(),
    args : files('test_parity_gate_covers_registered_twins.py'),
    suite : ['fast'],
    timeout : 30,
)

# ADR-1496: the macOS tester bundle measures every open Metal row with exact
# tests: tools/rc1-tester/image/metal-rows.json against docs/state.md, the
# Metal parity tests, the bundle's unit list and the gate. Device-free.
test('test_metal_report_rows_contract',
    import('python').find_installation(),
    args : files('test_metal_report_rows_contract.py'),
    suite : ['fast'],
    timeout : 30,
)

# ADR-1496: every Metal kernel takes one language revision and the macOS
# tester bundle's deployment target, so the metallib loads on the tester's Mac.
# Device-free.
test('test_metal_shader_build_contract',
    import('python').find_installation(),
    args : files('test_metal_shader_build_contract.py'),
    suite : ['fast'],
    timeout : 30,
)

test('test_cuda_parity_gate_default_run',
    import('python').find_installation(),
    args : files('test_cuda_parity_gate_default_run.py'),
    suite : ['slow', 'gpu'],
    is_parallel : false,
    timeout : 120,
)

# The default run above is registered for every build: on a libvmaf without
# CUDA it must skip, not report the CLI's refusal of `--backend cuda` as a
# parity failure. Device-free.
test('test_cuda_parity_gate_skip',
    import('python').find_installation(),
    args : files('test_cuda_parity_gate_skip.py'),
    suite : ['fast'],
    timeout : 30,
)

# ADR-1364: the Windows SYCL device-link anchor patcher, driven by synthetic
# COFF objects so it runs on every host, Windows or not.
test('test_sycl_coff_anchor',
    import('python').find_installation(),
    args : files('test_sycl_coff_anchor.py'),
    suite : ['fast'],
    timeout : 30,
)

# ADR-0220 and Research-2090: source-level guard for fp64-free SpEED
# device regions, unique cross-TU kernel names, and explicit output captures.
test('test_sycl_kernel_source_contract',
    import('python').find_installation(),
    args : files('test_sycl_kernel_source_contract.py'),
    suite : ['fast'],
    timeout : 30,
)

# ADR-1372 / ADR-1373 / ADR-1374: source-level guard for the CUDA RC3 parity
# design (diff-first motion, no mid-frame host wait, shared option helpers,
# the CPU's SSIM arithmetic, clamped ADM rows, VIF CPU fallback, and the
# engine initialising an extractor before it picks its dispatch path).
# Device-free.
test('test_cuda_kernel_source_contract',
    import('python').find_installation(),
    args : files('test_cuda_kernel_source_contract.py'),
    suite : ['fast'],
    timeout : 30,
)

# ADR-1377 / ADR-1381 / ADR-1382: device-free source guard for the HIP RC3
# parity design (diff-first motion SAD, one host wait per frame, tile and ADM
# row clamps, vif_hip CPU fallback, CPU option helpers), with a planted
# regression per check.
test('test_hip_kernel_source_contract',
    import('python').find_installation(),
    args : files('test_hip_kernel_source_contract.py'),
    suite : ['fast'],
    timeout : 30,
)

# ADR-1435: vif_hip reads the log2 table the CPU extractor builds
# (vif_log2_table_generate() in integer_vif.h, the table's one definition)
# instead of evaluating log2f() on the device. Device-free, with a planted
# regression per check; the device check is test_hip_vif_parity.
test('test_hip_vif_log2_table_contract',
    import('python').find_installation(),
    args : files('test_hip_vif_log2_table_contract.py'),
    suite : ['fast'],
    timeout : 30,
)

# ADR-1440: float_psnr_hip adds its squared differences as integers (the
# CPU's float square as uint32, one sum per block, low and high halves at 16
# bits), so its sums are exact as the CPU's are. Device-free, with a planted
# regression per check; the device check is test_hip_float_psnr_parity.
test('test_hip_float_psnr_exact_contract',
    import('python').find_installation(),
    args : files('test_hip_float_psnr_exact_contract.py'),
    suite : ['fast'],
    timeout : 30,
)

# ADR-1408: device-free source guard for the HIP shared frame (an adopted twin
# asks the context's shared frame for its planes and uploads nothing itself,
# vmaf_read_pictures() announces and ends the frame around the dispatch loop,
# the shared upload waits, a slot is fenced before it is written), with a
# planted regression per check.
test('test_hip_shared_frame_contract',
    import('python').find_installation(),
    args : files('test_hip_shared_frame_contract.py'),
    suite : ['fast'],
    timeout : 30,
)

# ADR-1378 / ADR-1384: device-free source guard for the device-resident HIP
# CAMBI and SpEED twins (no host stage, one staged upload, one readback and
# one wait per frame, CPU helpers for the host combine, fp64-free device code
# without inexact intrinsics), with a planted regression per check.
test('test_hip_device_resident_contract',
    import('python').find_installation(),
    args : files('test_hip_device_resident_contract.py'),
    suite : ['fast'],
    timeout : 30,
)

# ADR-1423: adm_hip takes its CSF weights, border, shifts and score conclusion
# from the CPU's integer_adm_kernels.h, folds the denominator once per row and
# clears its accumulators after the frame's upload. Device-free source contract
# with a planted regression per construct; the device check is
# test_hip_adm_exact.
test('test_hip_adm_exact_contract',
    import('python').find_installation(),
    args : files('test_hip_adm_exact_contract.py'),
    suite : ['fast'],
    timeout : 30,
)

# ADR-1407: device-free guard for the HIP strict FP policy (one hipcc flag
# list for every kernel: contraction off, correctly rounded fp32 division and
# square root; no per-kernel table; the device probe built with the same
# list), with a planted regression per check.
test('test_hip_strict_fp_policy',
    import('python').find_installation(),
    args : files('test_hip_strict_fp_policy.py'),
    suite : ['fast'],
    timeout : 30,
)

# ADR-1183 / T-GPU-TWIN-OPTION-ALIAS-DRIFT: a non-default feature option
# becomes part of the published collector key. Keep GPU spellings equal to the
# CPU reference without requiring any GPU runtime on the test host.
test('test_gpu_option_alias_contract',
    import('python').find_installation(),
    args : files('test_gpu_option_alias_contract.py'),
    suite : ['fast'],
    timeout : 30,
)

# Model dispatch must distinguish a mirrored option schema from the values a
# GPU implementation can execute. Keep every default-only site inventoried
# without requiring a GPU compiler or device on the test host.
test('test_gpu_option_value_capability_contract',
    import('python').find_installation(),
    args : files('test_gpu_option_value_capability_contract.py'),
    suite : ['fast'],
    timeout : 30,
)

# ADR-1379 / ADR-1380: device-free guard for the device-resident CUDA CAMBI and
# SpEED twins -- no host residual or host combine, one readback and one wait
# per frame at collect, fp64-free kernels with the CPU's rounding spelled out,
# one argument block per kernel.
test('test_cuda_device_resident_contract',
    import('python').find_installation(),
    args : files('test_cuda_device_resident_contract.py'),
    suite : ['fast'],
    timeout : 30,
)

# ADR-1397 / ADR-1401: psnr_hvs_cuda, psnr_hvs_sycl and psnr_hvs_hip return the
# CPU's scores bit for bit -- each kernel stores every term calc_psnrhvs()
# sums, in the CPU's arithmetic and built without contraction, and the host
# adds them in the CPU's order through psnr_hvs_score.c. Device-free; the
# device checks are test_{cuda,sycl,hip}_psnr_hvs_parity.
test('test_psnr_hvs_twin_exact_sum_contract',
    import('python').find_installation(),
    args : files('test_psnr_hvs_twin_exact_sum_contract.py'),
    suite : ['fast'],
    timeout : 30,
)

# ADR-1412: float_vif_cuda returns the CPU's scores bit for bit -- the taps
# are vif_get_filter()'s, log2 is the reference's polynomial, vif_sigma_nsq
# stays fp64 and the terms are added row by row in fp32. Device-free; the
# arithmetic check is test_float_vif_device_math, the device check is
# test_cuda_float_vif_parity.
test('test_cuda_float_vif_exact_contract',
    import('python').find_installation(),
    args : files('test_cuda_float_vif_exact_contract.py'),
    suite : ['fast'],
    timeout : 30,
)

# ADR-1444: float_vif_hip returns the CPU's scores bit for bit by running the
# arithmetic the CUDA twin runs (src/feature/float_vif_gpu_common.h) -- the
# taps are vif_get_filter()'s, no kernel holds a tap or calls a math-library
# log2, vif_sigma_nsq reaches the kernel as a double and the terms are added
# row by row. Device-free; the device check is test_hip_float_vif_parity.
test('test_hip_float_vif_exact_contract',
    import('python').find_installation(),
    args : files('test_hip_float_vif_exact_contract.py'),
    suite : ['fast'],
    timeout : 30,
)

# ADR-1416: adm_cuda returns the CPU's scores bit for bit -- the CSF weights,
# the rounding shifts and the float conclusion are the CPU's routines
# (integer_adm_kernels.h), and the denominator is folded once per row.
# Device-free; the fold itself is test_adm_cm_row_rounding, the device check
# is test_cuda_adm_parity.
test('test_cuda_adm_exact_contract',
    import('python').find_installation(),
    args : files('test_cuda_adm_exact_contract.py'),
    suite : ['fast'],
    timeout : 30,
)

# ADR-1436: ciede_sycl runs ciede.c's statements with every fp64 value as an
# fp32 pair, stores one float per pixel and lets the host add them in the
# reference's order; the constants and tables of sycl_ff_math.h are the
# generator's. Device-free; the arithmetic check is test_sycl_ciede_math, the
# device check is test_sycl_ciede_parity.
test('test_sycl_ciede_exact_contract',
    import('python').find_installation(),
    args : files('test_sycl_ciede_exact_contract.py'),
    suite : ['fast'],
    timeout : 30,
)

# ADR-1422: float_vif_sycl returns the CPU's scores bit for bit -- the taps
# are vif_get_filter()'s, log2 is the reference's polynomial, the two fp64
# expressions around vif_sigma_nsq are evaluated without an fp64 type, and the
# terms are added row by row in fp32. Device-free; the arithmetic check is
# test_sycl_float_vif_math, the device check is test_sycl_float_vif_parity.
test('test_sycl_float_vif_exact_contract',
    import('python').find_installation(),
    args : files('test_sycl_float_vif_exact_contract.py'),
    suite : ['fast'],
    timeout : 30,
)

# ADR-1443: integer_ssim_sycl runs the reference's fp64 operations in 64-bit
# integers, stores every pixel's term unreduced and the host adds the plane in
# calc_ssim()'s raster order. Device-free; the arithmetic check is
# test_sycl_integer_ssim_math, the device check is test_sycl_ssim_parity.
test('test_sycl_ssim_exact_contract',
    import('python').find_installation(),
    args : files('test_sycl_ssim_exact_contract.py'),
    suite : ['fast'],
    timeout : 30,
)

# ADR-1463: float_ssim_sycl forms iqa/ssim_tools.c's per-window lv and cv as
# the CPU's doubles in 64-bit integers, stores every window's term unreduced
# and the host adds them in raster order (the CPU's order; a work-group sum
# rounds elsewhere and moved the float mean on frames whose terms cancel).
# Device-free; the score checks are test_sycl_float_ssim_parity (host and
# device).
test('test_sycl_float_ssim_exact_contract',
    import('python').find_installation(),
    args : files('test_sycl_float_ssim_exact_contract.py'),
    suite : ['fast'],
    timeout : 30,
)

# ADR-1446: ssimulacra2_sycl returns the CPU's score bit for bit -- each of
# the six per-sample terms is the reference's double, formed in 64-bit
# integers, and each sum is the reference's loop over them (increments per
# chunk of consecutive pixels composed in pixel order, one walk per sum with
# a term-by-term fallback); the fp32 pair sums are advice for the plan only.
# Device-free; the arithmetic checks are test_sycl_ssimulacra2_math and
# test_sycl_ordered_sum, the device check is test_sycl_ssimulacra2_parity.
test('test_sycl_ssimulacra2_exact_contract',
    import('python').find_installation(),
    args : files('test_sycl_ssimulacra2_exact_contract.py'),
    suite : ['fast'],
    timeout : 30,
)

# vif_sycl's host tail rounds each scale's sums to float and divides in
# single precision, as integer_vif.c does, and its debug option defaults to
# the CPU's false. Device-free; the device check is test_sycl_vif_parity.
test('test_sycl_vif_float_sums_contract',
    import('python').find_installation(),
    args : files('test_sycl_vif_float_sums_contract.py'),
    suite : ['fast'],
    timeout : 30,
)

# ADR-1420 — float_adm_cuda reproduces the CPU extractor bit for bit: the angle
# threshold, the gain and the two CSF constants have the reference's types and
# association, every reduction is added row by row in fp32, and the weights,
# the region, the pooling and the floor are the reference's own. Device-free;
# the arithmetic check is test_float_adm_device_math, the device check is
# test_cuda_float_adm_parity.
test('test_cuda_float_adm_exact_contract',
    import('python').find_installation(),
    args : files('test_cuda_float_adm_exact_contract.py'),
    suite : ['fast'],
    timeout : 30,
)

# ADR-1458 — float_adm_hip returns the CPU extractor's bits with the CUDA
# twin's arithmetic (src/feature/float_adm_gpu_common.h): plain operators
# under the strict FP list, no wave or block reduction, the reference's own
# routines on the host, the frame-size check before any device resource.
# Device-free; the arithmetic checks are test_float_adm_device_math (host) and
# test_hip_float_adm_math (device), the score check is
# test_hip_float_adm_parity.
test('test_hip_float_adm_exact_contract',
    import('python').find_installation(),
    args : files('test_hip_float_adm_exact_contract.py'),
    suite : ['fast'],
    timeout : 30,
)

# ADR-1453 — float_moment_cuda's 16bpc kernel adds moment_float_square(), one
# fp32 product of the sample with itself as an integer, which is the term
# moment.c adds; the host recovers the moment with the CPU's two divisions.
# Device-free; the device check is test_cuda_float_moment_parity.
test('test_cuda_float_moment_exact_contract',
    import('python').find_installation(),
    args : files('test_cuda_float_moment_exact_contract.py'),
    suite : ['fast'],
    timeout : 30,
)

# ADR-1455 — float_psnr_cuda adds float_psnr.c's terms, the float squares of
# the sample differences, as integers: uint64 per warp, per block and on the
# host, which divides the exact total by scaler^2 and the pixel count.
# Device-free; the device check is test_cuda_float_psnr_parity.
test('test_cuda_float_psnr_exact_contract',
    import('python').find_installation(),
    args : files('test_cuda_float_psnr_exact_contract.py'),
    suite : ['fast'],
    timeout : 30,
)

# ADR-1462 — vif_cuda reads the CPU's log2 table from a global of its kernel
# module, uploaded at init, and evaluates no logarithm on the device. This is
# the device-free half: the statistic takes each logarithm from the table,
# no kernel source calls a math function, and init uploads
# vif_log2_table_generate()'s values, size-checked, before it returns.
test('test_cuda_vif_log2_contract',
    import('python').find_installation(),
    args : files('test_cuda_vif_log2_contract.py'),
    suite : ['fast'],
    timeout : 30,
)

# ADR-1434: float_adm_sycl returns the CPU's scores bit for bit -- the
# decouple's quotient is the IEEE fp32 one (ADR-1442), the three
# fp64 expressions of adm_tools.c are evaluated without an fp64 type, the
# masking threshold and every reduction are added in the reference's order,
# and the weights, the region, the pooling and the floor are the reference's
# own. Device-free; the arithmetic check is test_sycl_float_adm_math, the
# device check is test_sycl_float_adm_parity.
test('test_sycl_float_adm_exact_contract',
    import('python').find_installation(),
    args : files('test_sycl_float_adm_exact_contract.py'),
    suite : ['fast'],
    timeout : 30,
)

# ADR-1432: vif_sycl computes the two integers integer_vif.c truncates from
# its fp64 gain exactly -- integer arithmetic, a replay of the fp64 operations
# for the undecided samples, no fp32 gain, no sycl::mul_hi(). Device-free; the
# arithmetic check is test_sycl_integer_vif_math, the device check is
# test_sycl_vif_parity.
test('test_sycl_vif_exact_gain_contract',
    import('python').find_installation(),
    args : files('test_sycl_vif_exact_gain_contract.py'),
    suite : ['fast'],
    timeout : 30,
)

# ADR-1442 — float ADM divides: the decouple's quotient is the IEEE fp32 one in
# the CPU reference and in float_adm_cuda, with no reciprocal estimate, probe
# or table anywhere and no fast-math flag that would bring one back.
# Device-free; the value check is test_float_adm_device_math.
test('test_float_adm_divides_contract',
    import('python').find_installation(),
    args : files('test_float_adm_divides_contract.py'),
    suite : ['fast'],
    timeout : 30,
)

# The CLI read-ahead's invariants are assert()s, not early returns: a lint
# cleanup once replaced all seven after a host-toolchain false positive.
test('test_cli_frame_reader_asserts_contract',
    import('python').find_installation(),
    args : files('test_cli_frame_reader_asserts_contract.py'),
    suite : ['fast'],
    timeout : 30,
)

# A default scale=0 is only executable by the GPU float-SSIM twins while the
# CPU auto-scale resolves to 1. Pin the model-only, first-frame fallback seam
# and all four backend declarations without requiring an accelerator.
test('test_gpu_float_ssim_auto_scale_contract',
    import('python').find_installation(),
    args : files('test_gpu_float_ssim_auto_scale_contract.py'),
    suite : ['fast'],
    timeout : 30,
)

# Integer ADM's fixed-point viewing-geometry floor is backend-independent and
# must run before CSF normalization or any GPU resource work. Keep that ordering
# device-free so Linux CI covers Metal as well as CUDA, HIP, and SYCL.
test('test_adm_viewing_geometry_contract',
    import('python').find_installation(),
    args : files('test_adm_viewing_geometry_contract.py'),
    suite : ['fast'],
    timeout : 30,
)

# BUG-048 / ADR-0453: PSNR options parity across CPU, CUDA, SYCL, HIP, and Metal.
test('test_gpu_psnr_option_parity_contract',
    import('python').find_installation(),
    args : files('test_gpu_psnr_option_parity_contract.py'),
    suite : ['fast'],
    timeout : 30,
)

# ADR-1167 / T-ADM-CM-ROUNDING-PLACEMENT-UNOBSERVABLE-2026-09-19: every
# backend must feed a complete row to the raw accumulator fold before the
# score's float conversion. Source-level mutation controls keep device-free
# coverage for AVX2/AVX-512, CUDA, HIP, SYCL and Metal call-site topology.
test('test_adm_cm_row_rounding_contract',
    import('python').find_installation(),
    args : files('test_adm_cm_row_rounding_contract.py'),
    suite : ['fast'],
    timeout : 30,
)

# BUG-048: these diagnostics were standardized once, then silently restored
# from stale source. Keep their line framing and severity prefix stable.
test('test_vmaf_log_callsite_format',
    import('python').find_installation(),
    args : files('test_vmaf_log_callsite_format.py'),
    suite : ['fast'],
    timeout : 30,
)

# CodeQL must see one predictor implementation identity. Text-including
# predict.c into its test creates a second static call graph that the official
# unused-function query correctly reports as unreachable.
test('test_predict_source_authority',
    import('python').find_installation(),
    args : files('test_predict_source_authority.py'),
    suite : ['fast'],
    timeout : 30,
)

# ADR-1320 / T-CUDA-FATBIN-NO-HEADER-DEP-2026-09-05: verify durable dependency
# tracking for CUDA fatbin and HIP HSACO device targets without GPU execution.
test('test_device_target_header_dependencies',
    import('python').find_installation(),
    args : files('test_device_target_header_dependencies.py'),
    env : {'VMAFX_DEVICE_DEP_BUILD_DIR': meson.project_build_root()},
    suite : ['fast'],
    timeout : 30,
)

# ADR-1336: CUDA modules belong to the context that loaded them. Keep the
# complete load-owner inventory device-free and reject raw unload calls that
# depend on whichever unrelated context happens to be current at close time.
test('test_cuda_module_lifecycle_contract',
    import('python').find_installation(),
    args : files('test_cuda_module_lifecycle_contract.py'),
    suite : ['fast'],
    timeout : 30,
)

# ADR-1226 / T-CUDA-ADM-CM-REGISTER-PRESSURE-2026-09-07: verify zero stack spill
# and bounded register usage across all architectures in adm_cm.fatbin.
test('test_cuda_adm_cm_register_pressure',
    import('python').find_installation(),
    args : files('test_cuda_adm_cm_register_pressure.py'),
    env : {'VMAFX_DEVICE_DEP_BUILD_DIR': meson.project_build_root()},
    suite : ['fast'],
    timeout : 30,
)


# BUG-048 B2 / ADR-0759: keep the four large HIP ADM compute kernels on the
# device-resident AdmBufferHip pointer contract, including host launch wiring,
# one-time upload, and the BUG-092-compatible failure/close lifecycle.
test('test_hip_adm_buffer_pointer_contract',
    import('python').find_installation(),
    args : files('test_hip_adm_buffer_pointer_contract.py'),
    suite : ['fast'],
    timeout : 30,
)

# BUG-048 A6 / Research-2081: Linux has no Metal compiler or device, so pin
# float_moment's exact uint64 workgroup reduction and host lo/hi reconstruction
# as a source contract on every platform. The live parity test below remains
# the Apple-Silicon execution gate.
test('test_metal_float_moment_contract',
    import('python').find_installation(),
    args : files('test_metal_float_moment_contract.py'),
    suite : ['fast'],
)

# BUG-048 / Research-2113: Metal float_motion lifecycle correctness, force-zero,
# debug-controlled emission, and flush idempotency source contract.
test('test_metal_float_motion_contract',
    import('python').find_installation(),
    args : files('test_metal_float_motion_contract.py'),
    suite : ['fast'],
)

# ADR-1334 / T-GAP-METAL-MS-SSIM-DB-CHROMA-OPTIONS-2026-09-07: device-free contract
# asserting Metal float_ms_ssim options (enable_db, clip_db, enable_chroma, enable_lcs),
# max_db derivation, chroma min-dim checks, and shared score emitter wiring parity.
test('test_metal_ms_ssim_options_contract',
    import('python').find_installation(),
    args : files('test_metal_ms_ssim_options_contract.py'),
    suite : ['fast'],
)

test_metal_ms_ssim_option_semantics = executable('test_metal_ms_ssim_option_semantics',
    ['test.c', 'test_metal_ms_ssim_option_semantics.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    dependencies : math_lib,
)
test('test_metal_ms_ssim_option_semantics', test_metal_ms_ssim_option_semantics,
    suite : ['fast'],
)

# ADR-0480: both bootstrap score paths consume the shared suffix owner.
test('test_bootstrap_name_contract',
    import('python').find_installation(),
    args : files('test_bootstrap_name_contract.py'),
    suite : ['fast'],
)

# Public GPU headers carry lifecycle/ownership semantics that the compiler
# cannot check. Keep the Doxygen contract executable so a stale merge cannot
# silently reduce it to parameter names again.
test('test_gpu_public_header_docs',
    import('python').find_installation(),
    args : files('test_gpu_public_header_docs.py'),
    suite : ['fast'],
    timeout : 120,
)

# ADR-0379: libvmaf.so exports its public vmaf_* API and nothing else of ours.
# ELF only (nm -D); skips with 77 when nm or c++filt is missing. Not in coverage
# builds: libgcov links into the instrumented .so and exports its own globals
# (__gcov_*, mangle_path), and a coverage build is never the library that ships.
if host_machine.system() == 'linux' and get_option('default_library') != 'static' \
        and not get_option('b_coverage')
    test('check_exported_symbols',
        import('python').find_installation(),
        args : [files('check_exported_symbols.py'),
                get_option('default_library') == 'both' ? libvmaf.get_shared_lib() : libvmaf,
                meson.project_source_root() / 'include'],
        suite : ['fast'],
    )
endif

# Aggregate of all GPU/compute backend dependency objects so test
# binaries that must link against ANY enabled backend can reference
# one variable instead of repeating the per-backend list.
gpu_all_deps = [
  cuda_dependency,
  sycl_dependency,
  hip_deps,
  metal_deps,
]

# Private-source tests historically compiled predict.c independently. Link the
# single source-authority archive instead so CodeQL and runtime tests observe
# the same static call graph as libvmaf.
predict_test_dependencies = gpu_all_deps + [predict_c_dependency]

libvmaf_public_link = get_option('default_library') == 'both' ? libvmaf.get_shared_lib() : libvmaf
test_output_lto_override = host_machine.system() == 'darwin' ? ['b_lto=false'] : []

# ADR-0708 Wave 1: log is compiled in log_cpp23_lib (isolated C++ static lib).
# Every test executable that previously direct-compiled '../src/log.c' must
# now pick up the log symbols via this object aggregate so that
# vmaf_log / vmaf_set_log_level resolve at link time.
log_cpp23_test_objects = log_cpp23_lib.extract_all_objects(recursive: true)
# ADR-0761: opt.cpp and predict.cpp are compiled as isolated C++23 static libs
# in src/meson.build. Tests that previously listed these files as direct sources
# now pull them in via extract_all_objects() so they are compiled exactly once,
# at the project-wide C++ standard (ADR-1003).
wave8_cpp23_objects = [
    opt_cpp23_lib.extract_all_objects(recursive: true),
    read_json_model_cpp23_lib.extract_all_objects(recursive: true),
]

# Subset of wave8_cpp23_objects for tests that only need opt.cpp (no model
# loading, no pdjson).  wave8_cpp23_objects always pulls in
# read_json_model_cpp23_lib which references vmaf_model_generate_name /
# vmaf_model_destroy from model.c; tests that don't link model.c must use
# this lighter variant to avoid spurious undefined-reference errors.
wave8_opt_only_objects = [
    opt_cpp23_lib.extract_all_objects(recursive: true),
]

# SIMD bit-exactness tests: suppress FP contraction in the test TU so
# the inline scalar reference functions match the SIMD libs (which are
# compiled with -ffp-contract=off via the carve-out static libraries).
#
# GCC defaults to -ffp-contract=off. Clang and icx default to
# -ffp-contract=on which auto-contracts mul-add pairs to FMA.
# Adding -ffp-contract=off keeps all three compilers consistent.
# Intel icx additionally needs its precise model. src/meson.build owns the
# compiler-ID mapping so production carve-outs and these scalar references
# cannot drift onto different contraction settings. See ADR-0161.
#
# ORDER MATTERS, measured. `-fp-model=precise` implies `-ffp-contract=on`, so
# when it lands AFTER `-ffp-contract=off` it silently re-enables contraction --
# which is the opposite of what this list is for. It therefore goes FIRST and
# the contract flag LAST. Same hazard, same fix, as the carve-outs in
# core/src/meson.build; see T-ICX-FP-CONTRACT-FLAG-ORDER-2026-09-07 in
# docs/state.md for the vfmadd counts that measured it.
_simd_strict_fp_args = vmaf_strict_fp_args

# PR #850 regression — HIP wavefront warpSize fix.
# Pure host-side tree-reduce logic: verifies the shared-memory warp-partial
# accumulation produces the correct sum for both wave32 (warpSize=32) and
# wave64 (warpSize=64) scenarios without requiring an AMD GPU.  CPU-only,
# no HIP dependency.
test_hip_wavefront_reduce = executable('test_hip_wavefront_reduce',
    ['test.c', 'test_hip_wavefront_reduce.c'],
    include_directories : [libvmaf_inc, test_inc],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies : [pthread_dependency, math_lib],
)
test('test_hip_wavefront_reduce', test_hip_wavefront_reduce, suite : ['fast'])

# ADR-1188 — percentile temporal pooling (MEDIAN / PERC5 / PERC10 / PERC20).
# Pools imported per-frame scores, so it needs no YUV fixture and no model.
test_pool_percentile = executable('test_pool_percentile',
    ['test.c', 'test_pool_percentile.c'],
    include_directories : [libvmaf_inc, test_inc],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [math_lib, stdatomic_dependency, pthread_dependency, gpu_all_deps],
)

# T-UPSTREAM-930 / ADR-1194 — integer-ADM angle_flag predicate parity.
# Pins the two evaluations in core/src/feature/adm_angle_flag.h against each
# other and against the golden-frozen CPU expression, on the band quadruples
# where the four legacy per-backend spellings disagreed. Host-only: exercises
# the header the CUDA / HIP / SYCL / Metal kernels now include, so no GPU or
# device runtime is needed.
test_adm_angle_flag = executable('test_adm_angle_flag',
    ['test.c', 'test_adm_angle_flag.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/feature/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies : [pthread_dependency, math_lib],
)
test('test_adm_angle_flag', test_adm_angle_flag, suite : ['fast'])

# T-SYCL-ADM-FRACTIONAL-GAIN-LIMIT-2026-09-29 (ADR-1413): the integer form of
# the enhancement gain limit's truncated double product, against the double
# product itself. Host-only: exercises the header the SYCL kernels include.
test_adm_gain_limit = executable('test_adm_gain_limit',
    ['test.c', 'test_adm_gain_limit.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/feature/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies : [pthread_dependency, math_lib],
)
test('test_adm_gain_limit', test_adm_gain_limit, suite : ['fast'])

test_context = executable('test_context',
    ['test.c', 'test_context.c'],
    include_directories : [libvmaf_inc, test_inc],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies:[stdatomic_dependency, pthread_dependency, gpu_all_deps],
)

test_picture_lib = static_library('test_picture_impl',
    ['../src/picture.c', '../src/mem.cpp', '../src/ref.cpp'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    dependencies:[stdatomic_dependency, pthread_dependency, thread_lib, gpu_all_deps],
)

test_picture = executable('test_picture',
    ['test.c', 'test_picture.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : test_picture_lib,
    dependencies:[stdatomic_dependency, pthread_dependency, thread_lib, gpu_all_deps],
)

# ADR-0928 cycle N+1: picture_v2 unit tests
test_picture_v2 = executable('test_picture_v2',
    ['test.c', 'test_picture_v2.c', '../src/picture_v2.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : test_picture_lib,
    dependencies:[stdatomic_dependency, pthread_dependency, thread_lib, gpu_all_deps],
)

test_propagate_metadata = executable('test_propagate_metadata',
    ['test.c', 'test_propagate_metadata.c', '../src/metadata_handler.cpp'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    dependencies: [pthread_dependency, sycl_dependency],
)

test_feature_collector = executable('test_feature_collector',
    ['test.c', 'test_feature_collector.c', '../src/metadata_handler.cpp', rev_target],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/feature/'), include_directories('../src')],
    # ADR-0708 Wave 1: log is now compiled in log_cpp23_lib; link rather than
    # direct-compile the source.
    link_with : [get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
                 log_cpp23_lib],
    c_args : vmaf_cflags_common,
    dependencies: [pthread_dependency, predict_test_dependencies],
    objects : [libsvm_static_lib.extract_all_objects(recursive: true)] + wave8_cpp23_objects,
)

# ADR-1359: device-twin lookup for a CPU extractor name and the registered-
# extractor report. Calls internal test accessors in libvmaf_priv.h to assert
# twin verdicts, gpumask behavior, and context fallbacks.
test_feature_backend_twin = executable('test_feature_backend_twin',
    ['test.c', 'test_feature_backend_twin.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/feature/'), include_directories('../src')],
    link_with : [get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf],
    c_args : vmaf_cflags_common,
    dependencies: [pthread_dependency],
)

# Finding R2-10: flush_context terminal-flush ordering. White-box test that
# calls internal test accessors in libvmaf_priv.h to assert the inner
# threaded flush no longer sets vmaf->flushed prematurely — the bug
# that let a CUDA-flush error skip flush_context_sycl with no retry possible.
test_flush_context_ordering = executable('test_flush_context_ordering',
    ['test.c', 'test_flush_context_ordering.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/feature/'), include_directories('../src')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [math_lib, pthread_dependency, gpu_all_deps],
)

# T-ASYNC-EXTRACTOR-THREAD-POOL-EINVAL-2026-10-01: an extractor with submit()
# and collect() and no backend flag (a GPU twin reachable by name only, such
# as adm_hip) runs on the calling thread when a worker pool exists; the pool's
# workers call extract(), which such an extractor does not have. Mock
# extractors, no device.
test_async_extractor_thread_pool = executable('test_async_extractor_thread_pool',
    ['test.c', 'test_async_extractor_thread_pool.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/feature/'), include_directories('../src')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [math_lib, pthread_dependency, gpu_all_deps],
)

# Coverage follow-up to PR #313 (R3 from
# docs/development/coverage-gap-analysis-2026-05-02.md): exercise
# core/src/output.cpp writers (XML / JSON / CSV / SUB) end-to-end.
#
# macOS/ld64 invariant: test_output must link one libvmaf definition only.
# Do not include libvmaf.c/output.c directly while also linking libvmaf.a;
# Apple LTO has resolved that duplicate-symbol pattern incorrectly under
# allocator poisoning and crashed the writer tests.
test_output = executable('test_output',
    ['test.c', 'test_output.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/feature/'), include_directories('../src')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, gpu_all_deps],
    override_options : test_output_lto_override,
)

test_log = executable('test_log',
    ['test.c', 'test_log.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    # ADR-0708 Wave 1: log.cpp is compiled in log_cpp23_lib (C++23 override);
    # link against the static lib instead of recompiling log.c into the test
    # binary directly. Preserves the white-box test semantics — test_log still
    # exercises the symbol definitions in src/log.cpp, just through link rather
    # than through #include / direct-compile.
    link_with : log_cpp23_lib,
    dependencies: [pthread_dependency, sycl_dependency],
)

# Public API coverage test: vmaf_score_at_index, vmaf_model_collection_load,
# vmaf_write_output.  Uses the full libvmaf link (no .c include-tricks) so that
# the public VMAF_EXPORT entry points are exercised through the shared library
# ABI rather than through static compilation of internals.
# Keeps the public ABI entry points covered by a linked-library test.
test_public_api_score = executable('test_public_api_score',
    ['test.c', 'test_public_api_score.c'],
    include_directories : [libvmaf_inc, test_inc],
    link_with : libvmaf_public_link,
    dependencies : [pthread_dependency, thread_lib, gpu_all_deps],
)

# Linux static-link fault control for output_file_open(). The project's
# large-file flags redirect open(2) to open64(2), so GNU ld wraps that symbol
# and injects EINTR once for the target path.
# LTO can resolve the call before the linker sees it, so build this control
# only in the configuration where --wrap is authoritative.
if host_machine.system() == 'linux' and get_option('default_library') != 'shared' \
        and not get_option('b_lto')
    test_output_open_eintr = executable('test_output_open_eintr',
        ['test.c', 'test_output_open_eintr.c'],
        include_directories : [libvmaf_inc, test_inc],
        link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
        link_args : ['-Wl,--wrap=open64'],
        dependencies : [pthread_dependency, thread_lib, gpu_all_deps],
    )
    test('test_output_open_eintr', test_output_open_eintr, suite : ['fast'])
endif

test_thread_pool = executable('test_thread_pool',
    ['test.c', 'test_thread_pool.c', '../src/thread_pool.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    dependencies : [pthread_dependency, thread_lib, sycl_dependency],
)

# Source inclusion allows deterministic pthread failure and wakeup injection.
#
# Needs a real `pthread_cond_timedwait`: the test bounds every wait so a broken
# gate fails instead of hanging the suite. Linux and MinGW-w64's winpthreads
# both have it. MSVC does not, and neither does this repo's Win32 shim
# (`core/src/compat/win32/pthread.h` implements init/destroy/wait/signal/
# broadcast over CONDITION_VARIABLE, but no timed wait), so on the MSVC lanes
# the call was only ever an implicit declaration. Probe for it rather than
# matching on a platform name, so the target reappears by itself if the shim
# grows one.
has_cond_timedwait = cc.has_header_symbol('pthread.h', 'pthread_cond_timedwait',
                                          dependencies : [pthread_dependency, thread_lib])
if has_cond_timedwait
  test_thread_pool_backpressure = executable('test_thread_pool_backpressure',
      ['test.c', 'test_thread_pool_backpressure.c'],
      include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
      dependencies : [pthread_dependency, thread_lib],
  )
endif

# `pdjson.c` is also part of libvmaf. Give each separately compiled test copy a
# distinct internal symbol set: CodeQL otherwise coalesces same-location static
# helpers across link targets and can orphan one copy from the public roots.
# Keep the public json_* names intact so the test still exercises the production
# API and whole-program analyzers see one truthful external symbol graph.
pdjson_static_symbols = [
    'json_begin_error',
    'push',
    'pop',
    'buffer_peek',
    'buffer_get',
    'stream_get',
    'stream_peek',
    'init',
    'is_match',
    'pushchar',
    'init_string',
    'encode_utf8',
    'hexchar',
    'read_unicode_cp',
    'read_unicode',
    'read_escaped',
    'char_needs_escaping',
    'utf8_seq_length',
    'is_legal_utf8',
    'read_utf8',
    'read_string',
    'is_digit',
    'read_digits',
    'read_fraction',
    'read_exponent',
    'read_number',
    'next_nonspace',
    'read_value',
    'read_array_item',
    'read_member_name',
    'read_object_item',
    'finish_document',
    'user_get',
    'user_peek',
]

pdjson_default_impl_args = []
foreach symbol : pdjson_static_symbols
    pdjson_default_impl_args += ['-D' + symbol + '=vmafx_test_pdjson_default_' + symbol]
endforeach
pdjson_default_impl = static_library('test_pdjson_impl',
    '../src/pdjson.c',
    include_directories : include_directories('../src'),
    c_args : pdjson_default_impl_args,
)

test_pdjson = executable('test_pdjson',
    ['test.c', 'test_pdjson.c'],
    include_directories : [test_inc, include_directories('../src')],
    link_with : pdjson_default_impl,
    c_args : ['-Drun_tests=vmafx_test_pdjson_default_run_tests'],
)

foreach increment_case : [['zero', '0'], ['oversized', 'SIZE_MAX']]
    increment_test_name = 'test_pdjson_stack_increment_' + increment_case[0]
    increment_impl_args = ['-DPDJSON_STACK_INC=' + increment_case[1]]
    foreach symbol : pdjson_static_symbols
        increment_impl_args += [
            '-D' + symbol + '=vmafx_test_pdjson_' + increment_case[0] + '_' + symbol,
        ]
    endforeach
    increment_impl = static_library(increment_test_name + '_impl',
        '../src/pdjson.c',
        include_directories : include_directories('../src'),
        c_args : increment_impl_args,
    )
    increment_test = executable(increment_test_name,
        ['test.c', 'test_pdjson_stack_increment.c'],
        include_directories : [test_inc, include_directories('../src')],
        link_with : increment_impl,
        c_args : ['-Drun_tests=vmafx_test_pdjson_' + increment_case[0] + '_run_tests'],
    )
    test(increment_test_name, increment_test, suite : ['fast'])
endforeach

# Behaviour pin for the vendored cJSON and its fork delta (ADR-0683, ADR-1061).
# Deliberately NOT behind enable_mcp: the test compiles cJSON.c itself, so every
# CPU build runs it and cJSON.c is in the CPU lane's compile database, which is
# what puts it in front of the Tidy Ratchet and Cppcheck jobs. It needs libm for
# the fabs() in cJSON's number comparison.
test_cjson = executable('test_cjson',
    ['test.c', 'test_cjson.c', '../src/mcp/3rdparty/cJSON/cJSON.c'],
    include_directories : [test_inc, include_directories('../src/mcp/3rdparty/cJSON')],
    dependencies : [math_lib],
)
test('test_cjson', test_cjson, suite : ['fast'])

test_model = executable('test_model',
    ['test.c', 'test_model.c', '../src/dict.cpp', json_model_c_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    c_args : [vmaf_cflags_common, '-DJSON_MODEL_PATH="'+join_paths(meson.project_source_root(), '../model/')+'"'],
    dependencies : [pthread_dependency, thread_lib, gpu_all_deps],
    objects : [libsvm_static_lib.extract_all_objects(recursive: true),
               read_json_model_cpp23_lib.extract_all_objects(recursive: true)],
)

# Duplicate-`model`-key libsvm leak regression (fuzz_json_model LeakSanitizer
# lane). Deliberately its own TU rather than a case in test_model.c: that file
# carries pre-existing clang-tidy debt that PR #1192 is clearing, and
# WarningsAsErrors makes the required changed-files tidy job fail for any PR
# that touches it. Mirrors test_model's sources/objects because it drives the
# same vmaf_read_json_model_from_buffer entry point.
test_model_libsvm_dup_key = executable('test_model_libsvm_dup_key',
    ['test.c', 'test_model_libsvm_dup_key.c', '../src/dict.cpp',
     json_model_c_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    c_args : [vmaf_cflags_common, '-DJSON_MODEL_PATH="'+join_paths(meson.project_source_root(), '../model/')+'"'],
    dependencies : [pthread_dependency, thread_lib, gpu_all_deps],
    objects : [libsvm_static_lib.extract_all_objects(recursive: true),
               read_json_model_cpp23_lib.extract_all_objects(recursive: true)],
)

# Netflix/vmaf#1242 regression: VmafFeatureDictionary ownership contract on
# vmaf_model_feature_overload / vmaf_model_collection_feature_overload.
test_model_feature_overload_ownership = executable('test_model_feature_overload_ownership',
    ['test.c', 'test_model_feature_overload_ownership.c', '../src/dict.cpp', json_model_c_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    c_args : [vmaf_cflags_common, '-DJSON_MODEL_PATH="'+join_paths(meson.project_source_root(), '../model/')+'"'],
    dependencies : [pthread_dependency, thread_lib, gpu_all_deps],
    objects : [libsvm_static_lib.extract_all_objects(recursive: true),
               read_json_model_cpp23_lib.extract_all_objects(recursive: true)],
)

# Public-only failed-registration ownership regression (Research-2048).
test_model_registration_ownership = executable('test_model_registration_ownership',
    ['test.c', 'test_model_registration_ownership.cpp'],
    include_directories : [libvmaf_inc, test_inc],
    link_with : libvmaf_public_link,
    dependencies : [math_lib, pthread_dependency, thread_lib],
)

# Fault control: private dictionary-copy failure, public registration callers.
# `-Wl,--wrap=vmaf_dictionary_copy` redirects the library's references to the
# test's wrapper, so the test links the static archive. It used to interpose the
# symbol in libvmaf.so, which only worked while the shared library leaked this
# internal function against ADR-0379. Same LTO constraint as the `--wrap`
# harness below: LTO resolves the call before the linker sees a reference, so
# "copy failure must propagate" would fail. Build it only where wrapping works.
if host_machine.system() == 'linux' and get_option('default_library') != 'shared' \
        and not get_option('b_lto')
    test_registration_partial_copy = executable('test_registration_partial_copy',
        ['test.c', 'test_registration_partial_copy.cpp'],
        include_directories : [libvmaf_inc, test_inc, include_directories('../src')],
        link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
        link_args : ['-Wl,--wrap=vmaf_dictionary_copy'],
        dependencies : [math_lib, pthread_dependency, thread_lib],
    )
    test('test_registration_partial_copy', test_registration_partial_copy, suite : ['fast'])
endif

# T-MODEL-COLLECTION-GROWTH-FAILURE-UNTESTED-2026-10-01: a failed growth of the
# model array must leave the collection intact (the fix of Netflix/vmaf PR
# #1590, which the fork already carries). `-Wl,--wrap=realloc` fails one chosen
# realloc inside the library, so this needs the static archive and a linker
# that sees the reference: same constraints as test_registration_partial_copy.
if host_machine.system() == 'linux' and get_option('default_library') != 'shared' \
        and not get_option('b_lto')
    test_model_collection_growth = executable('test_model_collection_growth',
        ['test.c', 'test_model_collection_growth.c'],
        include_directories : [libvmaf_inc, test_inc, include_directories('../src')],
        link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
        link_args : ['-Wl,--wrap=realloc'],
        c_args : [vmaf_cflags_common, '-DJSON_MODEL_PATH="'+join_paths(meson.project_source_root(), '../model/')+'"'],
        dependencies : [math_lib, pthread_dependency, thread_lib],
    )
    test('test_model_collection_growth', test_model_collection_growth, suite : ['fast'])
endif

# Coverage for model-collection public API: vmaf_model_collection_load,
# vmaf_use_features_from_model_collection, vmaf_score_at_index_model_collection,
# vmaf_score_pooled_model_collection, vmaf_model_collection_load_from_path,
# vmaf_model_collection_feature_overload. Imports feature scores synthetically
# via vmaf_import_feature_score() — no YUV frames required.
test_model_collection_api = executable('test_model_collection_api',
    ['test.c', 'test_model_collection_api.c'],
    include_directories : [libvmaf_inc, test_inc],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    c_args : [vmaf_cflags_common, '-DJSON_MODEL_PATH="'+join_paths(meson.project_source_root(), '../model/')+'"'],
    dependencies : [math_lib, pthread_dependency, thread_lib],
)

test_predict = executable('test_predict',
    ['test.c', 'test_predict.c', '../src/dict.cpp', '../src/metadata_handler.cpp',
     '../src/feature/feature_collector.cpp', '../src/feature/alias.c', '../src/model.c',
     json_model_c_sources, '../src/feature/feature_name.cpp', '../src/feature/feature_extractor.cpp',],    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    c_args : vmaf_cflags_common,
    dependencies : [pthread_dependency, thread_lib, gpu_all_deps],
    objects : [libsvm_static_lib.extract_all_objects(recursive: true),
               read_json_model_cpp23_lib.extract_all_objects(recursive: true)],
)

# Coverage-targeted runtime tests for vendored libsvm 3.24 (svm.cpp).
# Complements test_svm_parser.c (parser rejection paths) by
# exercising svm_train + the inspector and predict family +
# svm_check_parameter + a save/load round-trip — together lifting
# svm.cpp coverage past the parser-only floor.
# Observation-only: vendored source is not modified (ADR-0889, core/src/AGENTS.md §10).
test_svm_api = executable('test_svm_api',
    ['test.c', 'test_svm_api.c', '../src/thread_locale.cpp'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    c_args : vmaf_cflags_common,
    dependencies : [pthread_dependency, thread_lib],
    objects : libsvm_static_lib.extract_all_objects(recursive: true),
)

# Regression test for the sequential-realloc double-free fixed in PR #708.
# Exercises svm_group_classes() and svm_check_parameter() with 17- and
# 32-class fixtures, crossing the max_nr_class=16 realloc threshold.
# Under ASan/UBSan any double-free aborts immediately; the test is most
# valuable in sanitised build configurations.
test_svm_multiclass = executable('test_svm_multiclass',
    ['test.c', 'test_svm_multiclass.c', '../src/thread_locale.cpp'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    c_args : vmaf_cflags_common,
    dependencies : [pthread_dependency, thread_lib],
    objects : libsvm_static_lib.extract_all_objects(recursive: true),
)

# SAN-MODEL-MALLOC-OOB regression suite (ADR-0889). Covers the vendored
# libsvm text-format parser hardening — crafted-model rejection paths
# that pre-empt oob Malloc sizes and read-before-write defects in the
# upstream-3.24 baseline.
test_svm_parser = executable('test_svm_parser',
    ['test.c', 'test_svm_parser.c', '../src/thread_locale.cpp'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    c_args : vmaf_cflags_common,
    dependencies : [pthread_dependency, thread_lib],
    objects : libsvm_static_lib.extract_all_objects(recursive: true),
)

# Coverage-targeted unit tests for the vendored IQA helpers
# (libvmaf/feature/iqa): math_utils, decimate, KBND_* border handlers,
# iqa_filter_pixel / iqa_img_filter, and an iqa_ssim end-to-end drive
# that exercises ssim_tools.c's scalar precompute/variance/accumulate
# fallback paths. Complementary to test_iqa_convolve.c, which is
# SIMD-vs-scalar bit-exact (covers convolve.c only).
test_iqa_helpers = executable('test_iqa_helpers',
    ['test.c', 'test_iqa_helpers.c',
     '../src/mem.cpp', '../src/picture.c', '../src/ref.cpp',
     '../src/dict.cpp',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp', dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'),
                           include_directories('../src/feature/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib,
                    predict_test_dependencies, dnn_deps],
    objects : [
      common_cuda_objects,
      log_cpp23_test_objects,
      platform_specific_cpu_objects,
      libvmaf_feature_static_lib.extract_all_objects(recursive: true),
      libvmaf_cpu_static_lib.extract_all_objects(recursive: true),
      libsvm_static_lib.extract_all_objects(recursive: true),
    ] + wave8_opt_only_objects,
)


test_feature_extractor = executable('test_feature_extractor',
    # dnn_sources pulled in directly so feature_lpips.c can resolve its
    # libvmaf/dnn.h references (session_open/run/close,
    # vmaf_tensor_from_rgb_imagenet) without linking against libvmaf.so.
    # fex_ctx_vector.c is part of libvmaf_sources (not libvmaf_feature_static_lib)
    # so it must be listed explicitly; the T-CUDA-FEATURE-EXTRACTOR-DOUBLE-WRITE
    # regression test exercises feature_extractor_vector_append() directly.
    ['test.c', 'test_feature_extractor.c', '../src/mem.cpp', '../src/picture.c', '../src/ref.cpp',
     '../src/dict.cpp',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp',
     '../src/fex_ctx_vector.cpp',
     dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib, predict_test_dependencies,
                    dnn_deps],
    objects : [
      common_cuda_objects,
      platform_specific_cpu_objects,
      libvmaf_feature_static_lib.extract_all_objects(recursive: true),
      log_cpp23_test_objects,
      libvmaf_cpu_static_lib.extract_all_objects(recursive: true),
      libsvm_static_lib.extract_all_objects(recursive: true),
    ] + wave8_opt_only_objects
)

# Registration identity and checked growth, through private and public callers.
# The allocation-failure harness needs GNU `--wrap`, which redirects *references*
# to a symbol at link time. Under `b_lto=true` (set project-wide in
# core/meson.build) LTO resolves those calls internally before the linker sees a
# reference, so the wrapper never runs. The failure is not a quiet loss of
# coverage: the injected allocation then succeeds, `feature_extractor_vector_append`
# takes ownership of a context the test still holds, and the run ends in
# `free(): double free detected in tcache 2` — SIGSEGV on the CI runner. Debug
# builds hid it because nothing inlines at -O0.
#
# So the harness is compiled in only where it can actually intercept. `meson test
# --suite fast` on a debug or `-Db_lto=false` build still covers these paths, as
# does the sanitizer lane; the release+LTO lane builds the identity tests without
# the injection. Re-enabling it needs a mechanism that survives LTO, not a
# `-Db_lto=false` override on the test target — that fails to link, because the
# target consumes LTO bitcode objects from the libraries.
# ADR-0723 / Research-2047.
fex_vector_alloc_harness = host_machine.system() == 'linux' and not get_option('b_lto')
fex_vector_test_args = []
fex_vector_link_args = []
if fex_vector_alloc_harness
  fex_vector_test_args = ['-DFEX_VECTOR_ALLOC_TEST=1']
  fex_vector_link_args = ['-Wl,--wrap=realloc', '-Wl,--wrap=vmaf_feature_name_from_options']
endif
test_fex_ctx_vector = executable('test_fex_ctx_vector',
    # Compile private vector dependencies without interposing on a public-API test.
    ['test.c', 'test_fex_ctx_vector.cpp', '../src/mem.cpp', '../src/picture.c', '../src/ref.cpp',
     '../src/dict.cpp',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp',
     '../src/fex_ctx_vector.cpp',
     dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    cpp_args : fex_vector_test_args,
    link_args : fex_vector_link_args,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib, predict_test_dependencies,
                    dnn_deps],
    objects : [
      common_cuda_objects,
      platform_specific_cpu_objects,
      libvmaf_feature_static_lib.extract_all_objects(recursive: true),
      log_cpp23_test_objects,
      libvmaf_cpu_static_lib.extract_all_objects(recursive: true),
      libsvm_static_lib.extract_all_objects(recursive: true),
    ] + wave8_opt_only_objects
)

# Keep public calls in a separate executable: private objects in the caller
# can interpose on ELF shared-library internals and invalidate an old/new control.
test_fex_ctx_vector_public = executable('test_fex_ctx_vector_public',
    ['test.c', 'test_fex_ctx_vector_public.cpp'],
    include_directories : [libvmaf_inc, test_inc],
    link_with : libvmaf_public_link,
    dependencies : [pthread_dependency, thread_lib, gpu_all_deps],
)

# Link-time shims synchronize normal pool growth and force legal realloc moves.
# No production hook or GPU execution: GNU-compatible --wrap is Linux-only here.
if host_machine.system() == 'linux'
# Same LTO constraint as the fault-injection harness above, and a harder one:
# this target wraps malloc/calloc/strdup/realloc for the whole link. Under
# `b_lto=true` the icpx and clang linkers reject the mix outright —
# `final link failed: bad value` on the SYCL lanes — and even where it links,
# LTO resolves the calls internally so the wrappers never run. Build it only
# where the interception works. ADR-0723 / Research-2047.
if not get_option('b_lto')
    test_fex_pool_growth = executable('test_fex_pool_growth',
        ['test.c', 'test_fex_pool_growth.c', '../src/mem.cpp', '../src/picture.c', '../src/ref.cpp',
         '../src/dict.cpp',
         '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp',
         dnn_sources],
        include_directories : [libvmaf_inc, test_inc, include_directories('../src/'), dnn_inc],
        link_args : ['-Wl,--wrap=pthread_cond_wait', '-Wl,--wrap=realloc',
                     '-Wl,--wrap=malloc', '-Wl,--wrap=calloc', '-Wl,--wrap=strdup', '-Wl,--wrap=pthread_cond_init',
                     '-Wl,--wrap=pthread_cond_destroy'],
        c_args : vmaf_cflags_common + dnn_defines,
        dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib, predict_test_dependencies,
                        dnn_deps],
        objects : [
          common_cuda_objects,
          platform_specific_cpu_objects,
          libvmaf_feature_static_lib.extract_all_objects(recursive: true),
          log_cpp23_test_objects,
          libvmaf_cpu_static_lib.extract_all_objects(recursive: true),
          libsvm_static_lib.extract_all_objects(recursive: true),
        ] + wave8_opt_only_objects
    )
    test('test_fex_pool_growth', test_fex_pool_growth, suite : ['fast'], timeout : 15)
endif
endif

test_dict = executable('test_dict',
    ['test.c', 'test_dict.cpp', '../src/dict.cpp'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    dependencies: [pthread_dependency, sycl_dependency],
)

test_opt = executable('test_opt',
    ['test.c', 'test_opt.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    dependencies: [math_lib, pthread_dependency, sycl_dependency],
    # wave8_opt_only_objects: test_opt only exercises opt.cpp (vmaf_option_set);
    # full wave8_cpp23_objects would pull in read_json_model_cpp23_lib which
    # references pdjson symbols not present in this test's link recipe.
    objects : wave8_opt_only_objects,
)

test_cpu = executable('test_cpu',
    ['test.c', 'test_cpu.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    objects : libvmaf_cpu_static_lib.extract_all_objects(recursive: true),
    dependencies: [pthread_dependency, sycl_dependency],
)

# Pins down the VMAF_<BACKEND>_DISPATCH env-variable parser's strict-match
# contract.  The parser is a header-only static-inline (no link deps), so the
# test only needs the public src include path.  See the file header in
# test_gpu_dispatch_parse.c for the regression history.
test_gpu_dispatch_parse = executable('test_gpu_dispatch_parse',
    ['test.c', 'test_gpu_dispatch_parse.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    dependencies: [pthread_dependency, sycl_dependency],
)

test_ref = executable('test_ref',
    ['test.c', 'test_ref.c', '../src/ref.cpp'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    dependencies: [pthread_dependency, sycl_dependency],
)

test_feature = executable('test_feature',
    ['test.c', 'test_feature.cpp', '../src/feature/alias.c', '../src/dict.cpp',
     '../src/feature/feature_name.cpp'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    dependencies: [pthread_dependency, sycl_dependency],
)

test_ciede = executable('test_ciede',
    ['test.c', 'test_ciede.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, gpu_all_deps],
)

# ADR-1476 — ciede2000() forms upstream's two float products
# (Netflix/vmaf libvmaf/src/feature/ciede.c:224-225 and :235-236). Replays
# ciede_delta_e() of cuda/integer_ciede/ciede_device.h with the header's own
# helpers, with float products and with the widened ones the fork had after
# PR #552, and requires the float form on every colour pair of a grid that
# tells the two apart. test_ciede_device_math holds the header to ciede.c.
test_ciede_upstream_products = executable('test_ciede_upstream_products',
    ['test.c', 'test_ciede_upstream_products.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, gpu_all_deps, math_lib],
)
test('test_ciede_upstream_products', test_ciede_upstream_products, suite : ['fast'])

# ADR-1426 — the arithmetic ciede_cuda's kernel runs
# (src/feature/cuda/integer_ciede/ciede_device.h), compiled for the host and
# replayed over whole frames against the CPU extractor, bit for bit. No CUDA
# build is needed: the header is plain C on the host and the reference is
# reached through the public API. The header's fp64 expressions round like
# the device's only without FMA contraction (hence the flag). ciede.c is built
# without contraction on every architecture since ADR-1461, so the test runs
# everywhere; it was x86-64 only while ciede.c was contraction-free only where
# the baseline has no FMA. Since ADR-1467 it also holds on every compiler: the
# squares are products on both sides, where one side used to call powf(x, 2)
# and a compiler was free to fold either call.
test_ciede_device_math = executable('test_ciede_device_math',
    ['test.c', 'test_ciede_device_math.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies : [pthread_dependency, gpu_all_deps, math_lib],
    c_args : vmaf_strict_fp_args,
)
test('test_ciede_device_math', test_ciede_device_math, suite : ['fast'])

test_delta_e_itp = executable('test_delta_e_itp',
    ['test.c', 'test_delta_e_itp.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, gpu_all_deps],
)

# PU21 HDR perceptual adapter — encoder + PU-PSNR + PQ EOTF + PU-SSIM oracle.
# Includes feature/pu21.c directly (like test_ciede) to reach the static math
# helpers; the non-static vmaf_fex_pu21 symbol and the linked deps
# (pu21_ssim.c, iqa convolve, feature_collector, mem) resolve from libvmaf.
test_pu21 = executable('test_pu21',
    ['test.c', 'test_pu21.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [math_lib, pthread_dependency, gpu_all_deps],
)

# ADR-1113: SHARED Pelorus <-> vmafx interop ABI conformance fixture. Compiles
# the vendored pack/parse + deband/denoise-param + x265 CSV QP-report + version
# sources DIRECTLY (no libvmaf link, and no test.c — this fixture carries its own
# main()), proving vmafx's vendored parser is byte-identical to pelorus's writer.
# pelorus_qp_report_csv.c is REQUIRED to link: the minor-3 fixture exercises
# pel_x265_csv_parse / pel_qp_report_from_x265_frames. Same vectors both repos
# run; see docs/api/pelorus-interop.md + scripts/sync-pelorus-interop.sh.
test_pelorus_interop = executable('test_pelorus_interop',
    ['test_pelorus_interop.c',
     '../src/interop/pelorus_interop.c',
     '../src/interop/pelorus_deband_params.c',
     '../src/interop/pelorus_denoise_params.c',
     '../src/interop/pelorus_qp_report_csv.c',
     '../src/interop/pelorus_version.c'],
    include_directories : [libvmaf_inc, test_inc],
)

# LPIPS registration tests — verify feature wiring without depending on a
# real ORT session. Links against libvmaf_feature_static_lib for the
# extractor list and pulls in opt.cpp for the VmafOption typedef.
test_lpips = executable('test_lpips',
    ['test.c', 'test_lpips.c', '../src/mem.cpp', '../src/picture.c', '../src/ref.cpp',
     '../src/dict.cpp',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp', dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib, predict_test_dependencies,
                    dnn_deps],
    objects : [
      common_cuda_objects,
      platform_specific_cpu_objects,
      libvmaf_feature_static_lib.extract_all_objects(recursive: true),
      log_cpp23_test_objects,
      libvmaf_cpu_static_lib.extract_all_objects(recursive: true),
      libsvm_static_lib.extract_all_objects(recursive: true),
    ] + wave8_opt_only_objects
)

# DISTS-Sq registration tests — mirrors test_lpips and verifies the
# two-input extractor is discoverable without requiring an ORT session.
test_dists = executable('test_dists',
    ['test.c', 'test_dists.c', '../src/mem.cpp', '../src/picture.c', '../src/ref.cpp',
     '../src/dict.cpp',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp', dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib, predict_test_dependencies,
                    dnn_deps],
    objects : [
      common_cuda_objects,
      platform_specific_cpu_objects,
      libvmaf_feature_static_lib.extract_all_objects(recursive: true),
      log_cpp23_test_objects,
      libvmaf_cpu_static_lib.extract_all_objects(recursive: true),
      libsvm_static_lib.extract_all_objects(recursive: true),
    ] + wave8_opt_only_objects
)

# FastDVDnet temporal pre-filter (T6-7) — same registration-only test
# pattern as test_lpips. Verifies the 5-frame extractor wiring without
# requiring a real ORT session; CLI smoke against
# model/tiny/fastdvdnet_pre.onnx covers the live inference path.
test_fastdvdnet_pre = executable('test_fastdvdnet_pre',
    ['test.c', 'test_fastdvdnet_pre.c', '../src/mem.cpp', '../src/picture.c', '../src/ref.cpp',
     '../src/dict.cpp',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp', dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib, predict_test_dependencies,
                    dnn_deps],
    objects : [
      common_cuda_objects,
      platform_specific_cpu_objects,
      libvmaf_feature_static_lib.extract_all_objects(recursive: true),
      log_cpp23_test_objects,
      libvmaf_cpu_static_lib.extract_all_objects(recursive: true),
      libsvm_static_lib.extract_all_objects(recursive: true),
    ] + wave8_opt_only_objects
)

# MobileSal saliency registration tests (T6-2a) — same pattern as
# test_lpips above. Verifies the extractor wires up without depending
# on a real ORT session; full inference is exercised by the CLI smoke
# gate against model/tiny/mobilesal.onnx.
test_mobilesal = executable('test_mobilesal',
    ['test.c', 'test_mobilesal.c', '../src/mem.cpp', '../src/picture.c', '../src/ref.cpp',
     '../src/dict.cpp',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp', dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib, predict_test_dependencies,
                    dnn_deps],
    objects : [
      common_cuda_objects,
      platform_specific_cpu_objects,
      libvmaf_feature_static_lib.extract_all_objects(recursive: true),
      log_cpp23_test_objects,
      libvmaf_cpu_static_lib.extract_all_objects(recursive: true),
      libsvm_static_lib.extract_all_objects(recursive: true),
    ] + wave8_opt_only_objects
)

# TransNet V2 shot-boundary detector — structural test, no real ORT
# session required (mirrors test_lpips). CLI smoke against
# model/tiny/transnet_v2.onnx covers the live inference path.
test_transnet_v2 = executable('test_transnet_v2',
    ['test.c', 'test_transnet_v2.c', '../src/mem.cpp', '../src/picture.c', '../src/ref.cpp',
     '../src/dict.cpp',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp', dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib, predict_test_dependencies,
                    dnn_deps],
    objects : [
      common_cuda_objects,
      platform_specific_cpu_objects,
      libvmaf_feature_static_lib.extract_all_objects(recursive: true),
      log_cpp23_test_objects,
      libvmaf_cpu_static_lib.extract_all_objects(recursive: true),
      libsvm_static_lib.extract_all_objects(recursive: true),
    ] + wave8_opt_only_objects
)

test_cambi = executable('test_cambi',
    ['test.c', 'test_cambi.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/feature/'), include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, gpu_all_deps],
)

cambi_simd_test_archs = ['x86_64', 'x86', 'aarch64', 'arm64']
if cambi_simd_test_archs.contains(host_machine.cpu_family())
test_cambi_simd = executable('test_cambi_simd',
    ['test.c', 'test_cambi_simd.c',
     '../src/mem.cpp', '../src/picture.c', '../src/ref.cpp',
     '../src/dict.cpp',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp', dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'),
                           include_directories('../src/feature/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib,
                    predict_test_dependencies, dnn_deps],
    objects : [
      common_cuda_objects,
      platform_specific_cpu_objects,
      libvmaf_cpu_static_lib.extract_all_objects(recursive: true),
      libvmaf_feature_static_lib.extract_all_objects(recursive: true),
      log_cpp23_test_objects,
      libsvm_static_lib.extract_all_objects(recursive: true),
    ] + wave8_opt_only_objects,
)

# Every other CAMBI per-stage SIMD kernel (decimate, anti-dithering, mode
# filter, derivative row, histogram range updates, c-values row and frame) on
# AVX2, AVX-512 and NEON against the production scalar stages.
test_cambi_stage_simd = executable('test_cambi_stage_simd',
    ['test.c', 'test_cambi_stage_simd.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/feature/'), include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, gpu_all_deps],
)

# CAMBI spatial-mask row kernels (compute_dp_row / compute_mask_row):
# AVX2, AVX-512 and NEON twins against the production scalar in cambi.c.
test_cambi_spatial_mask_simd = executable('test_cambi_spatial_mask_simd',
    ['test.c', 'test_cambi_spatial_mask_simd.c',
     '../src/mem.cpp', '../src/picture.c', '../src/ref.cpp',
     '../src/dict.cpp',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp', dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'),
                           include_directories('../src/feature/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib,
                    predict_test_dependencies, dnn_deps],
    objects : [
      common_cuda_objects,
      platform_specific_cpu_objects,
      libvmaf_cpu_static_lib.extract_all_objects(recursive: true),
      libvmaf_feature_static_lib.extract_all_objects(recursive: true),
      log_cpp23_test_objects,
      libsvm_static_lib.extract_all_objects(recursive: true),
    ] + wave8_opt_only_objects,
)
endif

# speed_chroma + speed_temporal registration smoke tests — link against
# libvmaf_feature_static_lib for the extractor list (mirrors test_lpips).
# Both extractors live in core/src/feature/speed.c, which is only
# compiled when -Denable_float=true (see core/src/meson.build); the
# corresponding entries in feature_extractor.c's extractor list are
# wrapped in `#if VMAF_FLOAT_FEATURES`. Gate the test on the same option
# so a default `meson setup` (enable_float=false) doesn't fail on a
# config that legitimately omits these extractors.
if get_option('enable_float')
test_speed = executable('test_speed',
    ['test.c', 'test_speed.c', '../src/mem.cpp', '../src/picture.c', '../src/ref.cpp',
     '../src/dict.cpp',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp', dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib, predict_test_dependencies,
                    dnn_deps],
    objects : [
      common_cuda_objects,
      platform_specific_cpu_objects,
      libvmaf_feature_static_lib.extract_all_objects(recursive: true),
      log_cpp23_test_objects,
      libvmaf_cpu_static_lib.extract_all_objects(recursive: true),
      libsvm_static_lib.extract_all_objects(recursive: true),
    ] + wave8_opt_only_objects
)

# Frame buffers of the CPU SpEED extractors: speed_temporal at speed_prescale > 1
# (T-SPEED-TEMPORAL-PRESCALE-UP-OVERFLOW-2026-09-30, Netflix/vmaf#1626) and
# speed_chroma at odd frame sizes (T-SPEED-CHROMA-ODD-SIZE-OVERFLOW-2026-09-30).
# Both overran a heap buffer, which only fails visibly under ASan
# (sanitizers.yml); same link recipe and float gate as test_speed.
test_speed_frame_buffers = executable('test_speed_frame_buffers',
    ['test.c', 'test_speed_frame_buffers.c', '../src/mem.cpp', '../src/picture.c',
     '../src/ref.cpp', '../src/dict.cpp',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp', dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib, predict_test_dependencies,
                    dnn_deps],
    objects : [
      common_cuda_objects,
      platform_specific_cpu_objects,
      libvmaf_feature_static_lib.extract_all_objects(recursive: true),
      log_cpp23_test_objects,
      libvmaf_cpu_static_lib.extract_all_objects(recursive: true),
      libsvm_static_lib.extract_all_objects(recursive: true),
    ] + wave8_opt_only_objects
)

# The lanczos4 prescale weight table the device-resident SpEED twins read
# (T-GPU-SPEED-LANCZOS4-PRESCALE-DRIFT-2026-09-30): resampling with the table
# in the scale kernel's operation order must equal vif_scale_frame_s() bit for
# bit. Device-free, so it runs on every lane; same link recipe and float gate
# as test_speed. The project-wide strict FP policy (ADR-1461) keeps Intel
# LLVM's default fast model from reordering the replayed sums, as it does for
# the library.
test_speed_lanczos4_weights = executable('test_speed_lanczos4_weights',
    ['test.c', 'test_speed_lanczos4_weights.c', '../src/mem.cpp', '../src/picture.c',
     '../src/ref.cpp', '../src/dict.cpp',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp', dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib, predict_test_dependencies,
                    dnn_deps],
    objects : [
      common_cuda_objects,
      platform_specific_cpu_objects,
      libvmaf_feature_static_lib.extract_all_objects(recursive: true),
      log_cpp23_test_objects,
      libvmaf_cpu_static_lib.extract_all_objects(recursive: true),
      libsvm_static_lib.extract_all_objects(recursive: true),
    ] + wave8_opt_only_objects
)
test('test_speed_lanczos4_weights', test_speed_lanczos4_weights, suite : ['fast'])

# Netflix/vmaf 76ea5f03 (+ the checkasm sizes of cea2b4d8; the fork carries no
# checkasm tree): vif_filter1d_dec16_s(), the fused anti-alias filter and
# decimation SpEED runs on every target but x86, must leave the bits of
# vif_filter1d_s() + vif_dec16_s(). Device-free; the aarch64 cross build runs
# it under qemu, where the extractor takes the fused path. Same link recipe,
# float gate and fp model as test_speed_lanczos4_weights.
test_speed_filter = executable('test_speed_filter',
    ['test.c', 'test_speed_filter.c', '../src/mem.cpp', '../src/picture.c',
     '../src/ref.cpp', '../src/dict.cpp',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp', dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib, predict_test_dependencies,
                    dnn_deps],
    objects : [
      common_cuda_objects,
      platform_specific_cpu_objects,
      libvmaf_feature_static_lib.extract_all_objects(recursive: true),
      log_cpp23_test_objects,
      libvmaf_cpu_static_lib.extract_all_objects(recursive: true),
      libsvm_static_lib.extract_all_objects(recursive: true),
    ] + wave8_opt_only_objects
)
# 1620 filter cases: 0.3 s in a release build, above meson's default 30 s under
# UBSan on a hosted runner (Sanitizers (undefined), 2026-10-02).
test('test_speed_filter', test_speed_filter, suite : ['fast'], timeout : 180)

# ADR-1477: SpEED evaluates Netflix's statements. speed.c's create_givens(),
# update_entropy() and get_speed_score() (through the test entries of
# speed_internal.h) against Netflix's statements evaluated with the host's own
# sqrt() and log2(), the fp32-only Givens routine of the GPU twins on every
# input it can receive (2^23 + 1 floats), the twins' host tail against the
# same statements, and, on glibc only, the CPU extractors against the values
# Netflix master returns on the tracked 576x324 pair. The _foreign_libm
# variant defines VMAF_TEST_ASSUME_FOREIGN_LIBM and runs the path every C
# library other than glibc takes (Netflix's values reported as skipped), so
# both paths run on every lane. Device-free; same link recipe and float gate
# as test_speed_filter. 8.4 million Givens inputs with a software fmaf() where
# the build targets a CPU without FMA: a second in a release build, more under
# a sanitizer or qemu.
foreach upstream_form : [['test_speed_upstream_form', []],
                         ['test_speed_upstream_form_foreign_libm', ['-DVMAF_TEST_ASSUME_FOREIGN_LIBM']]]
    upstream_form_exe = executable(upstream_form[0],
        ['test.c', 'test_speed_upstream_form.c', '../src/mem.cpp', '../src/picture.c',
         '../src/ref.cpp', '../src/dict.cpp',
         '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp',
         dnn_sources],
        include_directories : [libvmaf_inc, test_inc, include_directories('../src/'), dnn_inc],
        c_args : vmaf_cflags_common + dnn_defines + upstream_form[1] +
                 ['-DSPEED_TESTDATA_DIR="' + join_paths(meson.project_source_root(), '../testdata') + '"'],
        dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib,
                        predict_test_dependencies, dnn_deps],
        objects : [
          common_cuda_objects,
          platform_specific_cpu_objects,
          libvmaf_feature_static_lib.extract_all_objects(recursive: true),
          log_cpp23_test_objects,
          libvmaf_cpu_static_lib.extract_all_objects(recursive: true),
          libsvm_static_lib.extract_all_objects(recursive: true),
        ] + wave8_opt_only_objects
    )
    test(upstream_form[0], upstream_form_exe, suite : ['fast'], timeout : 300)
endforeach
endif
# SpEED-QA extractor tests -- unconditional (speed_qa.c is not float-gated).
# ADR-0253 real-impl: registration, vtable, flat/noise entropy, temporal positivity.
test_speed_qa = executable('test_speed_qa',
    ['test.c', 'test_speed_qa.c', '../src/mem.cpp', '../src/picture.c', '../src/ref.cpp',
     '../src/dict.cpp',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp', dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib, predict_test_dependencies,
                    dnn_deps],
    objects : [
        common_cuda_objects,
        platform_specific_cpu_objects,
        libvmaf_feature_static_lib.extract_all_objects(recursive: true),
      log_cpp23_test_objects,
        libvmaf_cpu_static_lib.extract_all_objects(recursive: true),
        libsvm_static_lib.extract_all_objects(recursive: true),
    ] + wave8_opt_only_objects
)

test_luminance_tools = executable('test_luminance_tools',
    ['test.c', 'test_luminance_tools.cpp'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'), include_directories('../src/feature/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, sycl_dependency],
)

test_cli_parse = executable('test_cli_parse',
    ['test.c', 'test_cli_parse.c', '../tools/cli_parse.cpp'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'), include_directories('../tools/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    c_args : [compat_cflags],
    cpp_args : [compat_cflags],
    dependencies: [pthread_dependency, getopt_dependency, sycl_dependency],
)

# Netflix/vmaf#743 regression: the CLI progress spinner must fall back to an
# ASCII table (and drop the CSI erase-to-EOL) on a console that reports a
# non-UTF-8 code page or refuses VT processing, and must stay byte-identical
# on POSIX.
test_spinner = executable('test_spinner',
    ['test.c', 'test_spinner.cpp'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../tools/')],
    dependencies : [pthread_dependency],
)

# test_y4m_411_oob uses POSIX fmemopen which has no MSVC/MinGW
# equivalent. The reproducer exercises the Y4M parser via an
# in-memory FILE*; on Windows the test isn't built (the bug is
# parser-side, the test wiring isn't platform-specific). Guarded
# off on Windows targets only.
if host_machine.system() != 'windows'
    test_y4m_411_oob = executable('test_y4m_411_oob',
        ['test_y4m_411_oob.c', '../tools/vidinput.c', '../tools/y4m_input.c', '../tools/yuv_input.c'],
        include_directories : [libvmaf_inc, test_inc, include_directories('../src/'), include_directories('../tools/')],
        link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
        c_args : [compat_cflags],
        dependencies: [pthread_dependency, sycl_dependency],
    )

    # T-VIDINPUT-ODD-DIMENSION-READBACK-UNTESTED-2026-10-01: raw and y4m
    # clips with an odd width or height read back sample for sample through
    # both reader entry points (the defect of Netflix/vmaf PR #1604, which
    # the fork's ceiling chroma geometry does not have). In-memory clips via
    # fmemopen, so gated off Windows alongside test_y4m_411_oob.
    test_video_input_odd_dims = executable('test_video_input_odd_dims',
        ['test.c', 'test_video_input_odd_dims.c', '../tools/vidinput.c', '../tools/y4m_input.c',
         '../tools/yuv_input.c'],
        include_directories : [libvmaf_inc, test_inc, include_directories('../src/'), include_directories('../tools/')],
        link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
        c_args : [compat_cflags],
        dependencies: [pthread_dependency, sycl_dependency],
    )

    # test_cli_parse_long_only_args forks a child to drive
    # cli_parse() with intentionally-bad argv and asserts the
    # child exits with status 1 + a "usage error" stderr line
    # — instead of dying from the SIGABRT the pre-ADR-0316
    # `error()` assertion would raise. POSIX fork()/waitpid()
    # required; gated off Windows alongside test_y4m_411_oob.
    test_cli_parse_long_only_args = executable('test_cli_parse_long_only_args',
        ['test.c', 'test_cli_parse_long_only_args.c', '../tools/cli_parse.cpp'],
        include_directories : [libvmaf_inc, test_inc, include_directories('../src/'), include_directories('../tools/')],
        link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
        c_args : [compat_cflags],
        cpp_args : [compat_cflags],
        dependencies: [pthread_dependency, getopt_dependency, sycl_dependency],
    )

    # test_y4m_alloc_failure drives the y4m parser against a header that
    # requests an absurdly-large dst_buf; combined with RLIMIT_AS the
    # malloc inside y4m_input_open_impl is forced to fail. Pre-fix the
    # parser ignored the failed malloc and returned success, leaving
    # dst_buf == NULL; the next fread() crashed inside libc. POSIX
    # fmemopen + setrlimit, gated off Windows alongside the others.
    test_y4m_alloc_failure = executable('test_y4m_alloc_failure',
        ['test_y4m_alloc_failure.c', '../tools/vidinput.c', '../tools/y4m_input.c', '../tools/yuv_input.c'],
        include_directories : [libvmaf_inc, test_inc, include_directories('../src/'), include_directories('../tools/')],
        link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
        c_args : [compat_cflags],
        dependencies: [pthread_dependency, sycl_dependency],
    )
endif

test_psnr = executable('test_psnr',
    ['test.c', 'test_psnr.c', '../src/picture.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, sycl_dependency],
)

# Coverage round 2 — drives the integer_psnr extract paths (10/12/16-bit
# HBD), the min_sse=>psnr_max branch, and the apsnr flush surface that
# test_psnr.c does not exercise.  Same link-recipe as test_speed_qa.
test_integer_psnr_coverage = executable('test_integer_psnr_coverage',
    ['test.c', 'test_integer_psnr_coverage.c', '../src/mem.cpp', '../src/picture.c', '../src/ref.cpp',
     '../src/dict.cpp',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp',
     dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib, predict_test_dependencies,
                    dnn_deps],
    objects : [
        common_cuda_objects,
      log_cpp23_test_objects,
        platform_specific_cpu_objects,
        libvmaf_feature_static_lib.extract_all_objects(recursive: true),
        libvmaf_cpu_static_lib.extract_all_objects(recursive: true),
        libsvm_static_lib.extract_all_objects(recursive: true),
    ] + wave8_opt_only_objects
)

# T-UPSTREAM-1109 / ADR-1193 — the `uncapped` option that splits the
# `mse == 0` infinity sentinel from the psnr_max truncation, on both the
# integer `psnr` and the float `float_psnr` extractor. Same link-recipe as
# test_integer_psnr_coverage (float_psnr pulls in picture_copy.c).
test_psnr_uncapped = executable('test_psnr_uncapped',
    ['test.c', 'test_psnr_uncapped.c', '../src/mem.cpp', '../src/picture.c', '../src/ref.cpp',
     '../src/dict.cpp',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp',
     dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib, predict_test_dependencies,
                    dnn_deps],
    objects : [
        common_cuda_objects,
      log_cpp23_test_objects,
        platform_specific_cpu_objects,
        libvmaf_feature_static_lib.extract_all_objects(recursive: true),
        libvmaf_cpu_static_lib.extract_all_objects(recursive: true),
        libsvm_static_lib.extract_all_objects(recursive: true),
    ] + wave8_opt_only_objects
)

# Coverage round 2 — drives integer_motion (v1) motion_force_zero +
# three-frame default + moving-average + five-frame-under-min branches.
test_integer_motion_coverage = executable('test_integer_motion_coverage',
    ['test.c', 'test_integer_motion_coverage.c', '../src/mem.cpp', '../src/picture.c', '../src/ref.cpp',
     '../src/dict.cpp',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp',
     dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib, predict_test_dependencies,
                    dnn_deps],
    objects : [
        common_cuda_objects,
      log_cpp23_test_objects,
        platform_specific_cpu_objects,
        libvmaf_feature_static_lib.extract_all_objects(recursive: true),
        libvmaf_cpu_static_lib.extract_all_objects(recursive: true),
        libsvm_static_lib.extract_all_objects(recursive: true),
    ] + wave8_opt_only_objects
)

# Coverage round 2 — drives integer_motion_v2 pipelined extractor:
# init() rejection of five-frame, index=0 short-circuit, force_zero,
# multi-frame extract+flush, moving-average, 10-bit pipeline.
test_integer_motion_v2_coverage = executable('test_integer_motion_v2_coverage',
    ['test.c', 'test_integer_motion_v2_coverage.c', '../src/mem.cpp', '../src/picture.c',
     '../src/ref.cpp', '../src/dict.cpp',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp',
     dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib, predict_test_dependencies,
                    dnn_deps],
    objects : [
        common_cuda_objects,
      log_cpp23_test_objects,
        platform_specific_cpu_objects,
        libvmaf_feature_static_lib.extract_all_objects(recursive: true),
        libvmaf_cpu_static_lib.extract_all_objects(recursive: true),
        libsvm_static_lib.extract_all_objects(recursive: true),
    ] + wave8_opt_only_objects
)

# Coverage round 3 — integer_ssim.c enable_db / clip_db / 10-bit HBD paths.
# Uses the same link-recipe as test_integer_psnr_coverage.
test_ssim_coverage = executable('test_ssim_coverage',
    ['test.c', 'test_ssim_coverage.c', '../src/mem.cpp', '../src/picture.c', '../src/ref.cpp',
     '../src/dict.cpp',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp',
     dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib, predict_test_dependencies,
                    dnn_deps],
    objects : [
        common_cuda_objects,
        log_cpp23_test_objects,
        platform_specific_cpu_objects,
        libvmaf_feature_static_lib.extract_all_objects(recursive: true),
        libvmaf_cpu_static_lib.extract_all_objects(recursive: true),
        libsvm_static_lib.extract_all_objects(recursive: true),
    ] + wave8_opt_only_objects
)

# Coverage round 3 — integer_adm.c end-to-end: default, debug=true,
# 10-bit HBD, and invalid norm_view_dist returning -EINVAL.
test_adm_coverage = executable('test_adm_coverage',
    ['test.c', 'test_adm_coverage.c', '../src/mem.cpp', '../src/picture.c', '../src/ref.cpp',
     '../src/dict.cpp',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp',
     dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib, predict_test_dependencies,
                    dnn_deps],
    objects : [
        common_cuda_objects,
        log_cpp23_test_objects,
        platform_specific_cpu_objects,
        libvmaf_feature_static_lib.extract_all_objects(recursive: true),
        libvmaf_cpu_static_lib.extract_all_objects(recursive: true),
        libsvm_static_lib.extract_all_objects(recursive: true),
    ] + wave8_opt_only_objects
)

# ADR-1325 — integer-ADM CSF fixed-point normalisation: full-scale
# adm_csf_mode=1 must produce finite, non-degenerate scores; invalid blend-table
# geometry must still fail; default and already-representable configurations stay accepted.
test_adm_csf_representable = executable('test_adm_csf_representable',
    ['test.c', 'test_adm_csf_representable.c', '../src/mem.cpp', '../src/picture.c',
     '../src/ref.cpp', '../src/dict.cpp',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp',
     dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib, predict_test_dependencies,
                    dnn_deps],
    objects : [
        common_cuda_objects,
        log_cpp23_test_objects,
        platform_specific_cpu_objects,
        libvmaf_feature_static_lib.extract_all_objects(recursive: true),
        libvmaf_cpu_static_lib.extract_all_objects(recursive: true),
        libsvm_static_lib.extract_all_objects(recursive: true),
    ] + wave8_opt_only_objects
)

# ADR-1472 (T-ADM-AIM-BARTEN-SCALE-TERM-WRAP-2026-10-01): the fixed-point CSF
# weight limits follow from the contrast-masking cube and the largest wavelet
# coefficient of a scale; frames that reach that coefficient must score.
test_integer_adm_cm_budget = executable('test_integer_adm_cm_budget',
    ['test.c', 'test_integer_adm_cm_budget.c', '../src/mem.cpp', '../src/picture.c',
     '../src/ref.cpp', '../src/dict.cpp',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp',
     dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib, predict_test_dependencies,
                    dnn_deps],
    objects : [
        common_cuda_objects,
        log_cpp23_test_objects,
        platform_specific_cpu_objects,
        libvmaf_feature_static_lib.extract_all_objects(recursive: true),
        libvmaf_cpu_static_lib.extract_all_objects(recursive: true),
        libsvm_static_lib.extract_all_objects(recursive: true),
    ] + wave8_opt_only_objects
)

# ADR-1397: host tail of calc_psnrhvs() for the GPU twins. One running float
# over every term of a plane, in term order and across block boundaries;
# device-free, built with the strict FP arguments the library uses for it.
test_psnr_hvs_score = executable('test_psnr_hvs_score',
    ['test.c', 'test_psnr_hvs_score.c', '../src/feature/psnr_hvs_score.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    c_args : vmaf_cflags_common + vmaf_strict_fp_args,
    dependencies : [math_lib],
)

# ADR-1409: host tail of compute_motion_simd() for the GPU float_motion twins.
# One running float over the row sums, top to bottom, and a float division;
# device-free, built with the strict FP arguments of the scalar reference.
test_float_motion_sad = executable('test_float_motion_sad',
    ['test.c', 'test_float_motion_sad.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    c_args : vmaf_cflags_common + vmaf_strict_fp_args,
    dependencies : [math_lib],
)
test('test_float_motion_sad', test_float_motion_sad, suite : ['fast'])

# Coverage round 3 — psnr_hvs.c: bpc>12 rejection, YUV400P chroma-disable,
# enable_chroma=false option, and 10-bit HBD path.
test_psnr_hvs_coverage = executable('test_psnr_hvs_coverage',
    ['test.c', 'test_psnr_hvs_coverage.c', '../src/mem.cpp', '../src/picture.c', '../src/ref.cpp',
     '../src/dict.cpp',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp',
     dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib, predict_test_dependencies,
                    dnn_deps],
    objects : [
        common_cuda_objects,
        log_cpp23_test_objects,
        platform_specific_cpu_objects,
        libvmaf_feature_static_lib.extract_all_objects(recursive: true),
        libvmaf_cpu_static_lib.extract_all_objects(recursive: true),
        libsvm_static_lib.extract_all_objects(recursive: true),
    ] + wave8_opt_only_objects
)

# Coverage round 3 — ssimulacra2.c end-to-end: default 8-bit, identical
# inputs, 10-bit HBD, and YUV444P full-chroma input.
test_ssimulacra2_coverage = executable('test_ssimulacra2_coverage',
    ['test.c', 'test_ssimulacra2_coverage.c', '../src/mem.cpp', '../src/picture.c', '../src/ref.cpp',
     '../src/dict.cpp',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp',
     dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib, predict_test_dependencies,
                    dnn_deps],
    objects : [
        common_cuda_objects,
        log_cpp23_test_objects,
        platform_specific_cpu_objects,
        libvmaf_feature_static_lib.extract_all_objects(recursive: true),
        libvmaf_cpu_static_lib.extract_all_objects(recursive: true),
        libsvm_static_lib.extract_all_objects(recursive: true),
    ] + wave8_opt_only_objects
)

# Issue #1526: the two ordered-comparison sites that used to turn a failed
# SSIMULACRA 2 computation into a perfect score are pure and fast enough to
# exercise without an image fixture.
test_ssimulacra2_nonfinite = executable('test_ssimulacra2_nonfinite',
    ['test.c', 'test_ssimulacra2_nonfinite.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    c_args : vmaf_cflags_common,
    dependencies : [math_lib],
)
test('test_ssimulacra2_nonfinite', test_ssimulacra2_nonfinite, suite : ['fast'])

# ADR-1433 — feature/ordered_sum.h (the bits of `for (...) s += x[i];` over
# non-negative doubles, from chunk-wise integer increments) against that loop:
# random terms, terms that tie on every add, zeros, 600 binades of range, NaN
# and infinity, wrong plans. It runs the pieces in the layout of the
# ssimulacra2 CUDA kernels. No device is needed. The loop the header is
# compared with is the test's own, so the test needs the strict FP flags: at
# its default fast model icx reorders that loop and the comparison fails.
test_ordered_sum = executable('test_ordered_sum',
    ['test.c', 'test_ordered_sum.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    c_args : vmaf_cflags_common + vmaf_strict_fp_args,
    dependencies : [math_lib],
)
test('test_ordered_sum', test_ordered_sum, suite : ['fast'])

# float_bits.h: the bit-identity helper of the exact-twin and replay tests.
test_float_bits = executable('test_float_bits',
    ['test.c', 'test_float_bits.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    c_args : vmaf_cflags_common,
    dependencies : [math_lib],
)
test('test_float_bits', test_float_bits, suite : ['fast'])

# ADR-1497 — feature/float_moment_sum.h, the float_moment twins' form of the
# CPU's second-moment sum past 2^53 units, run on the host the way the kernels
# lay it out and compared bit for bit with picture_copy() +
# compute_2nd_moment() on frames up to 7680x4320, with wrong plans too. The
# device checks are test_{cuda,sycl,hip}_float_moment_parity.
test_float_moment_sum = executable('test_float_moment_sum',
    ['test.c', 'test_float_moment_sum.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    c_args : vmaf_cflags_common + vmaf_strict_fp_args,
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies : [math_lib, pthread_dependency, gpu_all_deps],
)
# 35 s on a workstation under the sanitizer job's debug ASan + UBSan build;
# the hosted runner of that job passed 120 s twice (#1918, #1922).
test('test_float_moment_sum', test_float_moment_sum, suite : ['fast'], timeout : 600)
# The device-free half: every twin's kernels and host wired to the header.
test('test_float_moment_sum_contract',
    import('python').find_installation(),
    args : files('test_float_moment_sum_contract.py'),
    suite : ['fast'],
    timeout : 30,
)
# ADR-1498: the host side of core/src/feature/metal/metal_portable.h, the
# prelude the Metal twins' arithmetic headers compile on in a kernel and in
# the host tests that hold them against the CPU. Every host.
test_metal_portable = executable('test_metal_portable',
    ['test.c', 'test_metal_portable.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    c_args : vmaf_cflags_common + vmaf_strict_fp_args,
    dependencies : [math_lib],
)
test('test_metal_portable', test_metal_portable, suite : ['fast'])

# Host tests and source contracts of the Metal ports (ADR-1498). Each group
# of ports adds its blocks directly below its own marker line.

# [metal-ports: soft double]

# ADR-1498: the fp64 operations in 64-bit integers that the exact Metal twins
# run without an fp64 type (src/feature/metal/metal_soft_double.h and
# metal_soft_signed.h, the Metal spelling of the SYCL twins'
# sycl_soft_double.h / sycl_soft_signed.h), compiled on the host and held
# bit for bit against the host's binary64 arithmetic: at least 10^6 operands
# per operation plus edge tables. The reference is the host's own double
# arithmetic, so the TU needs the strict FP flags. Every host.
test_metal_soft_double = executable('test_metal_soft_double',
    ['test.c', 'test_metal_soft_double.cpp'],
    include_directories : [test_inc, include_directories('../src/')],
    c_args : vmaf_cflags_common + vmaf_strict_fp_args,
    cpp_args : vmaf_strict_fp_args,
    dependencies : [math_lib],
)
test('test_metal_soft_double', test_metal_soft_double, suite : ['fast'], timeout : 120)

# ADR-1498: the Metal soft-double headers stay in the subset Metal Shading
# Language, C and C++ share (no double, no long long, no 64-bit literal
# suffix, no std::, no pointer or reference parameter, no static local, no
# 128-bit type) and define every operation the SYCL headers define, under the
# documented name mapping. Device-free, with planted regressions.
test('test_metal_soft_double_contract',
    import('python').find_installation(),
    args : files('test_metal_soft_double_contract.py'),
    suite : ['fast'],
    timeout : 30,
)

# (end of soft-double group)

# [metal-ports: host steps and option tables]

# T-BUG048-GPU-OPTION-PARITY-REMAINDER-2026-09-26 (psnr part) and item (2) of
# T-GPU-TWIN-PARITY-GAPS-OUTSIDE-CUDA-2026-09-30 (ADR-1498): integer_psnr_metal
# keeps an exact uint64 SSE per threadgroup, the CPU option table, the
# psnr_score.h helpers, a flush for apsnr_* and the TEMPORAL flag. Device-free;
# metal_option_tables.py reads both option tables from the sources.
test('test_metal_integer_psnr_exact_contract',
    import('python').find_installation(),
    args : files('test_metal_integer_psnr_exact_contract.py'),
    suite : ['fast'],
    timeout : 30,
)

# Item (3) of T-GPU-TWIN-PARITY-GAPS-OUTSIDE-CUDA-2026-09-30 (ADR-1498):
# motion_v2_metal stores the CPU's weighted, capped SAD score from a uint64 SAD,
# derives motion2_v2 / motion3_v2 with vmaf_motion_window_flush(), takes the
# five-frame window and motion_force_zero, and has the CPU option table.
test('test_metal_motion_v2_exact_contract',
    import('python').find_installation(),
    args : files('test_metal_motion_v2_exact_contract.py'),
    suite : ['fast'],
    timeout : 30,
)

# T-GPU-INTEGER-VIF-MIN-DIM-TWINS-2026-09-29 (ADR-1498, the HIP guard of
# ADR-1381): the border fold of the integer VIF Metal kernels
# (feature/metal/metal_integer_vif_math.h) against integer_vif.c's single
# reflection, every tile load inside the plane, and the 16-pixel minimum from
# the CPU's filter widths. Every host. The contract pins the twin's ADR-1324
# context check, CPU fallback and init() refusal.
test_metal_integer_vif_math = executable('test_metal_integer_vif_math',
    ['test.c', 'test_metal_integer_vif_math.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    c_args : vmaf_cflags_common + vmaf_strict_fp_args,
    dependencies : [math_lib],
)
test('test_metal_integer_vif_math', test_metal_integer_vif_math, suite : ['fast'])

test('test_metal_integer_vif_min_dim_contract',
    import('python').find_installation(),
    args : files('test_metal_integer_vif_min_dim_contract.py'),
    suite : ['fast'],
    timeout : 30,
)

# T-BUG048-GPU-OPTION-PARITY-REMAINDER-2026-09-26, option tables (ADR-1498):
# integer_psnr_hvs_metal, integer_cambi_metal, ssimulacra2_metal and
# integer_vif_metal declare the CPU's options (one recorded gap: cambi's
# heatmaps_path), flags and provided features, and execute the options they
# declare (cambi's src_width / src_height / full_ref included). Device-free.
test('test_metal_twin_option_tables_contract',
    import('python').find_installation(),
    args : files('test_metal_twin_option_tables_contract.py'),
    suite : ['fast'],
    timeout : 30,
)

# integer vif gain (ADR-1498)
# T-METAL-INTEGER-VIF-FP32-GAIN-2026-10-03: the two integers integer_vif_metal's
# kernels take from the gain (feature/metal/metal_integer_vif_gain.h, the Metal
# spelling of the SYCL twin's sycl_integer_vif_math.h, ADR-1432) against the
# CPU's fp64 expressions of integer_vif.c::vif_accumulate_pixel(), value by
# value, on the host: random and boundary variances, variances from random
# pixel windows at 8, 10, 12 and 16 bits, six gain limits. The reference is the
# host's own double arithmetic, so the TU needs the strict FP flags. Every
# host. The contract pins the construct and fails on planted regressions.
test_metal_integer_vif_gain = executable('test_metal_integer_vif_gain',
    ['test.c', 'test_metal_integer_vif_gain.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    c_args : vmaf_cflags_common + vmaf_strict_fp_args,
    dependencies : [math_lib],
)
test('test_metal_integer_vif_gain', test_metal_integer_vif_gain, suite : ['fast'], timeout : 300)

test('test_metal_integer_vif_gain_contract',
    import('python').find_installation(),
    args : files('test_metal_integer_vif_gain_contract.py'),
    suite : ['fast'],
    timeout : 30,
)

# (end of host-step group)

# [metal-ports: integer kernels]

# T-METAL-FLOAT-PSNR-FP32-BLOCK-SUMS-2026-10-02: float_psnr_metal's per-sample
# term (core/src/feature/metal/metal_float_psnr_math.h) against float_psnr.c's
# float square through the CPU's picture_copy(), for every sample difference
# at 8, 10, 12 and 16 bits, and the twin's integer group sums against the
# CPU's noise on full-range noise frames. The source contract pins the integer
# reduction of float_psnr.metal and the host's exact total.
test_metal_float_psnr_math = executable('test_metal_float_psnr_math',
    ['test.c', 'test_metal_float_psnr_math.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    c_args : vmaf_cflags_common + vmaf_strict_fp_args,
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies : [pthread_dependency, math_lib],
)
test('test_metal_float_psnr_math', test_metal_float_psnr_math, suite : ['fast'])
test('test_metal_float_psnr_exact_contract',
    import('python').find_installation(),
    args : files('test_metal_float_psnr_exact_contract.py'),
    suite : ['fast'],
    timeout : 30,
)

# T-GPU-FLOAT-MOMENT-16BIT-SQUARES-2026-10-02: float_moment_metal's 16-bit
# second-moment term (core/src/feature/metal/metal_float_moment_math.h)
# against moment.c's float square (picture_copy() and compute_2nd_moment())
# for every sample value at 8 to 16 bits, and the twin's moments against
# compute_1st_moment() / compute_2nd_moment() on full-range noise and a bright
# 16-bit 1920x1080 frame. The source contract pins the kernel's float squares
# and the host's uint64 totals.
test_metal_float_moment_math = executable('test_metal_float_moment_math',
    ['test.c', 'test_metal_float_moment_math.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    c_args : vmaf_cflags_common + vmaf_strict_fp_args,
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies : [pthread_dependency, math_lib],
)
test('test_metal_float_moment_math', test_metal_float_moment_math, suite : ['fast'])
# ADR-1497 / ADR-1498: metal_float_moment_sum.h, the Metal copy of the rounded
# second-moment sum past 2^53 units, run on the host the way float_moment.metal
# lays it out and compared bit for bit with picture_copy() +
# compute_2nd_moment() on frames up to 7680x4320, with wrong plans too.
test_metal_float_moment_sum = executable('test_metal_float_moment_sum',
    ['test.c', 'test_metal_float_moment_sum.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    c_args : vmaf_cflags_common + vmaf_strict_fp_args,
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies : [math_lib, pthread_dependency, gpu_all_deps],
)
test('test_metal_float_moment_sum', test_metal_float_moment_sum, suite : ['fast'], timeout : 120)
test('test_metal_float_moment_exact_contract',
    import('python').find_installation(),
    args : files('test_metal_float_moment_exact_contract.py'),
    suite : ['fast'],
    timeout : 30,
)

# T-METAL-ADM-GAIN-LIMIT-FLOAT32-2026-10-01: integer_adm_metal's decouple
# (core/src/feature/metal/metal_integer_adm_math.h: div_lookup's integer
# reciprocal, the Q15 ratio and adm_gain_limit_product() of the shared
# adm_gain_limit.h) against the CPU's adm_decouple_band() and
# adm_decouple_band_s123() at gain limits 1, 1.2, 1.5 and 100: every scale-0
# operand, every distorted value at the operands whose fp32 reciprocal is
# wrong, and two million scales-1-3 pairs. The source contract pins the
# kernel's use of the header and the host's split of the limit.
test_metal_integer_adm_math = executable('test_metal_integer_adm_math',
    ['test.c', 'test_metal_integer_adm_math.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src')],
    c_args : vmaf_cflags_common + vmaf_strict_fp_args,
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies : [pthread_dependency, math_lib],
)
test('test_metal_integer_adm_math', test_metal_integer_adm_math, suite : ['fast'],
    timeout : 120)
test('test_metal_integer_adm_exact_contract',
    import('python').find_installation(),
    args : files('test_metal_integer_adm_exact_contract.py'),
    suite : ['fast'],
    timeout : 30,
)

# T-METAL-MOTION-BLUR-THEN-DIFF-2026-09-29 and
# T-GPU-MOTION-SAD-SCORE-NOT-EMITTED-2026-10-02: integer_motion_metal's
# diff-first arithmetic (core/src/feature/metal/metal_integer_motion_math.h)
# run in the kernel's threadgroup layout against the CPU `motion` extractor's
# VMAF_integer_feature_motion_sad_score on tiny, odd and 16x16-straddling
# frames at 8 to 16 bits, and the mirror's range at every tile index. The
# source contract pins the kernel's order, the host's ring and score, the
# CPU's option table and provided features, and the CPU's window flush.
test_metal_integer_motion_math = executable('test_metal_integer_motion_math',
    ['test.c', 'test_metal_integer_motion_math.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    c_args : vmaf_cflags_common + vmaf_strict_fp_args,
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies : [pthread_dependency, math_lib],
)
test('test_metal_integer_motion_math', test_metal_integer_motion_math, suite : ['fast'],
    timeout : 120)
test('test_metal_integer_motion_exact_contract',
    import('python').find_installation(),
    args : files('test_metal_integer_motion_exact_contract.py'),
    suite : ['fast'],
    timeout : 30,
)

# (end of integer-kernel group)

# [metal-ports: float_motion and float_vif]

# ADR-1498 / ADR-1409 — float_motion_metal's arithmetic
# (feature/metal/metal_float_motion_math.h, the lines float_motion.metal and
# float_motion_metal.mm compile) against the CPU's own functions, bit for bit:
# the reflect-101 fold, the taps, picture_copy() and convolution_f32_c_s() at
# 8 to 16 bits and every motion_filter_size, the row sums against
# compute_motion() with and without motion_add_scale1, and the frame score
# against the float_motion extractor. No Apple device is needed.
test_metal_float_motion_math = executable('test_metal_float_motion_math',
    ['test.c', 'test_metal_float_motion_math.cpp'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    c_args : vmaf_cflags_common + vmaf_strict_fp_args,
    cpp_args : vmaf_strict_fp_args,
    dependencies : [pthread_dependency, math_lib],
)
test('test_metal_float_motion_math', test_metal_float_motion_math,
    suite : ['fast'],
    timeout : 120)

# ADR-1498 — the source contract of the float_motion_metal port: the kernels
# and the host compute through metal_float_motion_math.h in the CPU's order;
# motion3 at the CPU's indices through motion_blend_tools.h (ADR-1404); the
# option table is the CPU's. Device-free; fails on a planted return of each
# construct the port removed.
test('test_metal_float_motion_exact_contract',
    import('python').find_installation(),
    args : files('test_metal_float_motion_exact_contract.py'),
    suite : ['fast'],
    timeout : 30,
)

# float_vif (ADR-1498)
# The arithmetic float_vif_metal's kernels run (src/feature/metal/
# metal_float_vif_math.h, the Metal spelling of the SYCL twin's
# sycl_float_vif_math.h, ADR-1422), compiled on the host and held value by
# value against vif_statistic_s(), picture_copy() and compute_vif(), with the
# kernels' passes composed from the header's helpers. The reference is the
# CPU extractor's own code, linked from libvmaf, so the TU needs the strict FP
# flags (ADR-1461). Device-free, every host.
test_metal_float_vif_math = executable('test_metal_float_vif_math',
    ['test.c', 'test_metal_float_vif_math.cpp',
     '../src/mem.cpp', '../src/picture.c', '../src/ref.cpp', '../src/dict.cpp',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp',
     '../src/gpu_picture_pool.cpp'] + dnn_sources,
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines + vmaf_strict_fp_args,
    cpp_args : vmaf_strict_fp_args,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib,
                    predict_test_dependencies, dnn_deps],
    objects : [
        common_cuda_objects,
        platform_specific_cpu_objects,
        libvmaf_feature_static_lib.extract_all_objects(recursive: true),
        log_cpp23_test_objects,
        libvmaf_cpu_static_lib.extract_all_objects(recursive: true),
        libsvm_static_lib.extract_all_objects(recursive: true),
    ] + wave8_opt_only_objects,
    link_language : 'cpp',
)
test('test_metal_float_vif_math', test_metal_float_vif_math, suite : ['fast'], timeout : 300)

# ADR-1498: float_vif_metal runs the SYCL twin's design -- the taps come from
# vif_get_filter() as kernel arguments, log2 is the reference's polynomial as
# bit patterns, vif_sigma_nsq reaches the statistic as a significand and
# exponent, a row is added in one thread and the rows on the host, and the
# option table is the CPU's. Device-free, with planted regressions; the
# arithmetic check is test_metal_float_vif_math, the device check is
# test_metal_float_vif_parity.
test('test_metal_float_vif_exact_contract',
    import('python').find_installation(),
    args : files('test_metal_float_vif_exact_contract.py'),
    suite : ['fast'],
    timeout : 30,
)

# (end of float_motion / float_vif group)

# [metal-ports: ssim family]

# T-GPU-SSIM-FRAME-SUM-ORDER-2026-10-01: integer_ssim_metal's arithmetic
# (core/src/feature/metal/metal_integer_ssim_math.h) compiled on the host: the
# per-pixel term against integer_ssim.c's fp64 expression bit for bit at 8, 10,
# 12 and 16 bits, and the whole twin (both passes, the raster-order sum of the
# term plane, the line weights) against the CPU `ssim` extractor at `==` on the
# frames of ssim_twin_parity.h. The source contract pins the kernel's stored
# terms, the host's raster-order sum and the reference lines.
test_metal_integer_ssim_math = executable('test_metal_integer_ssim_math',
    ['test.c', 'test_metal_integer_ssim_math.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    c_args : vmaf_cflags_common + vmaf_strict_fp_args,
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies : [pthread_dependency, math_lib],
)
test('test_metal_integer_ssim_math', test_metal_integer_ssim_math, suite : ['fast'],
    timeout : 120)
test('test_metal_integer_ssim_exact_contract',
    import('python').find_installation(),
    args : files('test_metal_integer_ssim_exact_contract.py'),
    suite : ['fast'],
    timeout : 30,
)

# T-GPU-TWIN-PARITY-GAPS-OUTSIDE-CUDA-2026-09-30 items (1) and (4):
# float_ssim_metal's arithmetic (core/src/feature/metal/metal_ssim_terms.h)
# compiled on the host: the window terms against iqa/ssim_tools.c bit for bit,
# and the whole twin (both passes, the raster-order sum of the stored terms,
# the fp32 frame means) against compute_ssim() at `==`, flat identical frames
# included. The source contract pins the kernel and host layout.
test_metal_float_ssim_math = executable('test_metal_float_ssim_math',
    ['test.c', 'test_metal_float_ssim_math.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    c_args : vmaf_cflags_common + vmaf_strict_fp_args,
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies : [pthread_dependency, math_lib],
)
test('test_metal_float_ssim_math', test_metal_float_ssim_math, suite : ['fast'],
    timeout : 120)
test('test_metal_float_ssim_exact_contract',
    import('python').find_installation(),
    args : files('test_metal_float_ssim_exact_contract.py'),
    suite : ['fast'],
    timeout : 30,
)

# T-GPU-FLOAT-MS-SSIM-CPU-ARITHMETIC-2026-10-01: float_ms_ssim_metal's
# arithmetic (core/src/feature/metal/metal_ms_ssim_math.h and
# metal_ssim_terms.h) compiled on the host: the decimation against
# ms_ssim_decimate.c bit for bit, and the whole twin (pyramid, both window
# passes of every scale, the raster-order sums of the stored terms, the fp32
# scale means, the combine) against compute_ms_ssim() at `==`. The source
# contract pins the kernel and host layout.
test_metal_float_ms_ssim_math = executable('test_metal_float_ms_ssim_math',
    ['test.c', 'test_metal_float_ms_ssim_math.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    c_args : vmaf_cflags_common + vmaf_strict_fp_args,
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies : [pthread_dependency, math_lib],
)
test('test_metal_float_ms_ssim_math', test_metal_float_ms_ssim_math, suite : ['fast'],
    timeout : 120)
test('test_metal_float_ms_ssim_exact_contract',
    import('python').find_installation(),
    args : files('test_metal_float_ms_ssim_exact_contract.py'),
    suite : ['fast'],
    timeout : 30,
)

# (end of ssim group)

# [metal-ports: ciede and float_adm]

# ADR-1498: the arithmetic ciede_metal's kernel runs, on the host:
# src/feature/ciede_ff_math.h in its Metal subset (VMAF_FF_MSL_SUBSET) on the
# primitives of src/feature/metal/metal_ciede_math.h, with the C++ library's
# float functions standing in for Metal's. It is test_sycl_ciede_math.c with
# a host probe for Metal, as test_hip_ciede_math is for HIP: the pair
# functions against the host's extended-precision math library, the pixel
# against the reference's fp64 statements, at 8, 10, 12 and 16 bits. The
# _coarse build makes the cube and fifth root estimates 2^-18 worse, more
# than Metal's exp() and log() may be off, under the same bounds. Both TUs are
# compiled without contraction, like the kernels.
foreach _ciede : [['test_metal_ciede_math', []],
                  ['test_metal_ciede_math_coarse', ['-DVMAF_TEST_METAL_CIEDE_COARSE_ROOTS=1']]]
  _exe = executable(_ciede[0],
      ['test.c', 'test_sycl_ciede_math.c', 'test_metal_ciede_math_probe.cpp'],
      include_directories : [test_inc, include_directories('../src/')],
      dependencies : [math_lib],
      c_args : vmaf_strict_fp_args + ['-DVMAF_TEST_CIEDE_MATH_HOST_ONLY=1'],
      cpp_args : vmaf_strict_fp_args + _ciede[1],
  )
  test(_ciede[0], _exe, suite : ['fast'], timeout : 120)
endforeach

# ADR-1498: ciede_metal compiles ciede_ff_math.h on Metal primitives, stores
# one float per pixel and the host adds them with ciede_frame_sum(); the shared
# headers keep a Metal subset the SYCL and HIP twins never preprocess.
# Device-free; the arithmetic check is test_metal_ciede_math, the device check
# is test_metal_integer_ciede_parity.
test('test_metal_ciede_exact_contract',
    import('python').find_installation(),
    args : files('test_metal_ciede_exact_contract.py'),
    suite : ['fast'],
    timeout : 30,
)

# float_adm (ADR-1498)

# ADR-1498: float_adm_metal holds the CPU float_adm's behaviour in source:
# init() refuses frames below 17x17 with adm_frame_size_check() before any
# device resource, and the frame sums are floored at compute_adm()'s
# 1e-10 * area / 1080p. Device-free, with planted regressions; the device
# check is test_metal_float_adm_parity.
test('test_metal_float_adm_exact_contract',
    import('python').find_installation(),
    args : files('test_metal_float_adm_exact_contract.py'),
    suite : ['fast'],
    timeout : 30,
)

# ADR-1498: what float_adm_metal's kernels run per work-item
# (src/feature/metal/metal_float_adm_math.h, the Metal spelling of the SYCL
# twin's sycl_float_adm_math.h) compiled as C on the host and compared bit for
# bit with adm_tools.c's own routines: the three fp64 expressions of the
# reference, then one scale composed as the three kernels compose it. The
# header's host operators round like the device only without FMA contraction
# (hence the flag), and adm_tools.c is contraction-free by construction on
# x86-64 only (its baseline has no FMA), so the test is registered there.
if host_machine.cpu_family() == 'x86_64'
test_metal_float_adm_math = executable('test_metal_float_adm_math',
    ['test.c', 'test_metal_float_adm_math.c',
     '../src/mem.cpp', '../src/picture.c', '../src/ref.cpp',
     '../src/dict.cpp',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp',
     '../src/gpu_picture_pool.cpp'] + dnn_sources,
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines + vmaf_strict_fp_args,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib,
                    predict_test_dependencies, dnn_deps],
    objects : [
        common_cuda_objects,
        platform_specific_cpu_objects,
        libvmaf_feature_static_lib.extract_all_objects(recursive: true),
        log_cpp23_test_objects,
        libvmaf_cpu_static_lib.extract_all_objects(recursive: true),
        libsvm_static_lib.extract_all_objects(recursive: true),
    ] + wave8_opt_only_objects
)
test('test_metal_float_adm_math', test_metal_float_adm_math, suite : ['fast'], timeout : 300)
endif

# (end of ciede / float_adm group)

# ADR-1499 — feature/float_psnr_rows.h, the float_psnr twins' form of the
# CPU's sum of its rows past 2^53 units, fed with host-made segment sums and
# compared bit for bit with the CPU float_psnr extractor on the cases of
# float_psnr_twin_parity.h and a 7680x4320 frame. The device checks are
# test_{cuda,sycl,hip}_float_psnr_parity.
test_float_psnr_rows = executable('test_float_psnr_rows',
    ['test.c', 'test_float_psnr_rows.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    c_args : vmaf_cflags_common + vmaf_strict_fp_args,
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies : [math_lib, pthread_dependency, gpu_all_deps],
)
test('test_float_psnr_rows', test_float_psnr_rows, suite : ['fast'], timeout : 120)

test_adm_nonfinite_score = executable('test_adm_nonfinite_score',
    ['test.c', 'test_adm_nonfinite_score.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    c_args : vmaf_cflags_common,
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies : [math_lib, pthread_dependency, gpu_all_deps],
)
test('test_adm_nonfinite_score', test_adm_nonfinite_score, suite : ['fast'])

test('test_nonfinite_collector_wiring',
    import('python').find_installation(),
    args : files('test_nonfinite_collector_wiring.py'),
    suite : ['fast'],
)

# NIQE (no-reference, ADR-1112): niqe_math.h unit oracles + an end-to-end
# score oracle driven through the public extractor-context API against frame 0
# of the natural fixture testdata/ref_576x324_48f.yuv. NIQE_TESTDATA_DIR points
# the test at the in-tree YUV (mirrors the JSON_MODEL_PATH precedent in
# test_model). Reuses the libvmaf_feature_static_lib object-extraction pattern
# from test_ssimulacra2_coverage so the niqe extractor + collector are linked.
test_niqe = executable('test_niqe',
    ['test.c', 'test_niqe.c', '../src/mem.cpp', '../src/picture.c', '../src/ref.cpp',
     '../src/dict.cpp',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp',
     dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines +
             ['-DNIQE_TESTDATA_DIR="' + join_paths(meson.project_source_root(), '../testdata') + '"'],
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib, predict_test_dependencies,
                    dnn_deps],
    objects : [
        common_cuda_objects,
        log_cpp23_test_objects,
        platform_specific_cpu_objects,
        libvmaf_feature_static_lib.extract_all_objects(recursive: true),
        libvmaf_cpu_static_lib.extract_all_objects(recursive: true),
        libsvm_static_lib.extract_all_objects(recursive: true),
    ] + wave8_opt_only_objects
)

# BRISQUE (no-reference, opinion-aware, ADR-1115): brisque_math.h unit oracles +
# an end-to-end score oracle driven through the public extractor-context API
# against frame 0 of testdata/ref_576x324_48f.yuv (natural uncompressed content,
# cross-platform stable — the original dis frame 12 was sensitive to FP
# summation order on near-flat MSCN fields, diverging ~0.1 units between x86 gcc
# and macOS/ARM clang; see test_brisque.c for the full rationale).
# The bundled LIVE allmodel is embedded as brisque_model.h and predicted via the
# vendored libsvm — so libsvm_static_lib must be linked. Same object-extraction
# pattern as test_niqe.
test_brisque = executable('test_brisque',
    ['test.c', 'test_brisque.c', '../src/mem.cpp', '../src/picture.c', '../src/ref.cpp',
     '../src/dict.cpp',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp',
     dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines +
             ['-DBRISQUE_TESTDATA_DIR="' + join_paths(meson.project_source_root(), '../testdata') + '"'],
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib, predict_test_dependencies,
                    dnn_deps],
    objects : [
        common_cuda_objects,
        log_cpp23_test_objects,
        platform_specific_cpu_objects,
        libvmaf_feature_static_lib.extract_all_objects(recursive: true),
        libvmaf_cpu_static_lib.extract_all_objects(recursive: true),
        libsvm_static_lib.extract_all_objects(recursive: true),
    ] + wave8_opt_only_objects
)

# Y-FUNQUE+ atoms-only (ADR-1114): analytic identical-input oracles
# (ms_ssim==0, dlm==1, mad==0), a deterministic non-trivial Python-reference
# oracle at places=4, a 2-frame MAD-Ref temporal check, an odd-dim (65x33)
# regression, and a too-small-frame rejection. Reuses the
# libvmaf_feature_static_lib object-extraction pattern from
# test_ssimulacra2_coverage so the y_funque_plus extractor + collector link.
test_y_funque_plus = executable('test_y_funque_plus',
    ['test.c', 'test_y_funque_plus.c', '../src/mem.cpp', '../src/picture.c', '../src/ref.cpp',
     '../src/dict.cpp',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp',
     dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib, predict_test_dependencies,
                    dnn_deps],
    objects : [
        common_cuda_objects,
        log_cpp23_test_objects,
        platform_specific_cpu_objects,
        libvmaf_feature_static_lib.extract_all_objects(recursive: true),
        libvmaf_cpu_static_lib.extract_all_objects(recursive: true),
        libsvm_static_lib.extract_all_objects(recursive: true),
    ] + wave8_opt_only_objects
)

# Coverage round 2 — integer_vif.h log2_32 / log2_64 helpers exercised
# directly (otherwise reached only via the AVX-512 hot path).
test_integer_vif_log2 = executable('test_integer_vif_log2',
    ['test.c', 'test_integer_vif_log2.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies : [math_lib, pthread_dependency, sycl_dependency],
)

# Coverage round 2 — iqa/convolve.c boundary helpers (KBND_*) and the
# 2-D iqa_img_filter / iqa_filter_pixel paths.
test_iqa_convolve_coverage = executable('test_iqa_convolve_coverage',
    ['test.c', 'test_iqa_convolve_coverage.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies : [pthread_dependency, sycl_dependency],
)

# Coverage round 2 — barten_csf_tools.h non-1080p resolutions + MAE
# variant + -EINVAL fallthrough.
test_barten_csf_coverage = executable('test_barten_csf_coverage',
    ['test.c', 'test_barten_csf_coverage.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies : [math_lib, pthread_dependency, sycl_dependency],
)

# Coverage round 2 — ms_ssim_decimate scalar NULL-out branch + the
# runtime-dispatch wrapper entry.
test_ms_ssim_decimate_coverage = executable('test_ms_ssim_decimate_coverage',
    ['test.c', 'test_ms_ssim_decimate_coverage.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies : [pthread_dependency, sycl_dependency],
)

# ADR-0912: end-to-end pixel-format edge coverage (4:2:2, 4:4:4, 10/12-bit)
# through the public CPU extractor surface. Links libvmaf the same way as
# test_feature_extractor / test_psnr — no internal-source includes; the file
# uses only the public picture/extractor/collector APIs.
test_pixel_format_edge_coverage = executable('test_pixel_format_edge_coverage',
    ['test.c', 'test_pixel_format_edge_coverage.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, sycl_dependency],
)

test_framesync = executable('test_framesync',
    ['test.c', 'test_framesync.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    c_args : vmaf_cflags_common,
    cpp_args : vmaf_cflags_common,
    dependencies : [pthread_dependency, thread_lib, sycl_dependency],
)

# framesync.c is recompiled into the executable, the way every other target in
# this file recompiles the sources it exercises, with its four pthread entry
# points renamed so the deterministic wrappers in
# test_framesync_init_failure.c are what it calls.
#
# The renames arrive through a force-included header, NOT through four -D
# flags. A command-line -D is in force before <pthread.h> is read, so it also
# rewrites whatever <pthread.h> declares -- harmless where the entry points
# are extern (glibc, macOS libSystem, MinGW winpthreads), fatal where they are
# defined inline. core/src/compat/win32/pthread.h is exactly that: a
# header-only SRWLOCK / CONDITION_VARIABLE shim whose four entry points are
# `static inline`, wired in by core/meson.build on every MSVC / clang-cl
# build. Under -D the rename landed on those definitions, framesync.c called
# the real primitives under the wrapper's name, no injected failure ever
# happened, and test_init_failure_unwinds_initialized_primitives failed --
# silently on every MSVC lane, visibly only on Windows ARM64 MSVC, the one
# MSVC lane that runs `meson test` instead of a named subset of executables.
# test_framesync_interpose.h reads <pthread.h> first and defines the renames
# after it, so they reach call sites only. See that header for the full
# argument. The force-include reaches all three translation units: test.c
# never mentions pthreads, so it is inert there, and
# test_framesync_init_failure.c undefines the four names above its first
# #include so it still sees the real entry points to wrap.
#
# `framesync_interposer_lto_override` turns LTO off for this one target on
# macOS. Two earlier attempts tried to steer *which* symbols survive LTO --
# first by dropping the link_whole/-force_load, then by dropping
# `vmaf_cflags_common` so the definitions keep default visibility. Both landed,
# and Apple's linker rejected the target again, byte-identically:
#
#   Use `-u <symbol_name>` to build missing LTO symbols for architecture arm64:
#     '_vmafx_test_pthread_cond_destroy' ... '_vmaf_framesync_init' ...
#     '_mu_tests_run' ...
#   ld: symbol(s) defined in LTO objects are referenced but missing in
#       compiled objects for architecture arm64
#
# What is measured, by replaying the target's own compile and link commands
# through clang/LLD: with b_lto=true this executable's entire input is LLVM
# bitcode, the LTO resolution hands libLTO exactly one externally-visible
# symbol (the entry point), and the generated object comes back with `main` as
# its only global -- every cross-translation-unit symbol the diagnostic names
# is either internalised or deleted. What is NOT established is why Apple's
# linker tolerates that outcome for the pure-bitcode siblings that link a few
# steps earlier in the same run (test_pdjson, test_thread_pool,
# test_pdjson_stack_increment_zero, all of which collapse to `main` in exactly
# the same way) and refuses it here. Do not add a third theory about that
# without a macOS machine to test it on.
#
# So this does not try to win the argument about the preserve list -- it
# removes the precondition. With b_lto=false the three inputs are ordinary
# Mach-O objects, the link contains no bitcode at all, and no code path that
# can emit "symbol(s) defined in LTO objects ..." is reachable. The override is
# the same mechanism `test_output` above already uses for its own Apple-LTO
# problem; LTO stays on for this target everywhere else. Nothing is lost: LTO
# buys a three-assertion unit test nothing, and the target exists precisely to
# keep a cross-translation-unit call boundary observable, which is the thing
# whole-program optimisation is built to dissolve.
#
# `vmaf_cflags_common` is still not on this target. Its absence is *not* what
# fixes the link (the attempt that removed it is one of the two that failed);
# it is merely unneeded here -- framesync.c includes only libc, framesync.h and
# the public header, and a test executable is neither installed nor exports
# anything. All three translation units were checked to compile warning-clean
# with that flag set, so restoring it for consistency with the other test
# targets is safe whenever someone wants to.
framesync_interposer_lto_override = host_machine.system() == 'darwin' ? ['b_lto=false'] : []

# Force-include spelling. cl / clang-cl take /FI, everything else takes
# -include; both resolve the file as if its #include were the first line of
# the primary source. The path is absolute so neither has to fall back on an
# include-path search.
framesync_interpose_header = meson.current_source_dir() / 'test_framesync_interpose.h'
framesync_interpose_args = cc.get_argument_syntax() == 'msvc' \
    ? ['/FI' + framesync_interpose_header] \
    : ['-include', framesync_interpose_header]

test_framesync_init_failure = executable('test_framesync_init_failure',
    ['test.c', 'test_framesync_init_failure.c', '../src/framesync.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    c_args : framesync_interpose_args,
    dependencies : [pthread_dependency, thread_lib],
    override_options : framesync_interposer_lto_override,
)

# Ported from upstream Netflix/vmaf c70debb1 ("libvmaf/test: port new
# adm/vif/speed tests"). The vif_tools and speed_chroma test files from that
# commit depend on upstream symbols (NUM_KERNELSCALES, vif_validate_kernelscale,
# speed_get_antialias_filter, the SpEED feature) that have not been ported to
# the fork; only the adm_csf and barten_csf cases are wired here.
test_adm_csf = executable('test_adm_csf',
    ['test.c', 'test_adm_csf.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies : [math_lib, pthread_dependency, sycl_dependency],
)

test_barten_csf = executable('test_barten_csf',
    ['test.c', 'test_barten_csf.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies : [math_lib, pthread_dependency, sycl_dependency],
)

# The CSF weights of float ADM (Watson step and Barten CSF) are upstream's
# float arithmetic, and the Barten header returns the same bits in a C and
# in a C++ translation unit (ADR-1489).
test_float_adm_csf_upstream = executable('test_float_adm_csf_upstream',
    ['test.c', 'test_float_adm_csf_upstream.c', 'barten_csf_cxx.cpp'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies : [math_lib, pthread_dependency, sycl_dependency],
)

# Bit-exactness between the scalar-separable reference and the arch-
# specific SIMD paths (x86 AVX2/AVX-512 and arm64 NEON). Links the
# platform SIMD object libraries directly so the kernels can be called
# without CPU-flag gating from inside the test. See
# docs/adr/0125-ms-ssim-decimate-simd.md.
iqa_convolve_test_archs = ['x86_64', 'x86', 'aarch64', 'arm64']
if iqa_convolve_test_archs.contains(host_machine.cpu_family())
test_iqa_convolve = executable('test_iqa_convolve',
    ['test.c', 'test_iqa_convolve.c',
     '../src/mem.cpp', '../src/picture.c', '../src/ref.cpp',
     '../src/dict.cpp',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp', dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'),
                           include_directories('../src/feature/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib,
                    predict_test_dependencies, dnn_deps],
    objects : [
      common_cuda_objects,
      platform_specific_cpu_objects,
      libvmaf_cpu_static_lib.extract_all_objects(recursive: true),
      libvmaf_feature_static_lib.extract_all_objects(recursive: true),
      log_cpp23_test_objects,
      libsvm_static_lib.extract_all_objects(recursive: true),
    ] + wave8_opt_only_objects,
)
endif

psnr_hvs_avx2_test_archs = ['x86_64', 'x86']
if psnr_hvs_avx2_test_archs.contains(host_machine.cpu_family())
test_psnr_hvs_avx2 = executable('test_psnr_hvs_avx2',
    ['test.c', 'test_psnr_hvs_avx2.c',
     '../src/mem.cpp', '../src/picture.c', '../src/ref.cpp',
     '../src/dict.cpp',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp', dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'),
                           include_directories('../src/feature/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib,
                    predict_test_dependencies, dnn_deps],
    objects : [
      common_cuda_objects,
      platform_specific_cpu_objects,
      libvmaf_cpu_static_lib.extract_all_objects(recursive: true),
      libvmaf_feature_static_lib.extract_all_objects(recursive: true),
      log_cpp23_test_objects,
      libsvm_static_lib.extract_all_objects(recursive: true),
    ] + wave8_opt_only_objects,
)
endif

vif_simd_test_archs = ['x86_64', 'x86']
if vif_simd_test_archs.contains(host_machine.cpu_family())
test_vif_simd = executable('test_vif_simd',
    ['test.c', 'test_vif_simd.c',
     '../src/mem.cpp', '../src/picture.c', '../src/ref.cpp',
     '../src/dict.cpp',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp', dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'),
                           include_directories('../src/feature/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib,
                    predict_test_dependencies, dnn_deps],
    objects : [
      common_cuda_objects,
      platform_specific_cpu_objects,
      libvmaf_cpu_static_lib.extract_all_objects(recursive: true),
      libvmaf_feature_static_lib.extract_all_objects(recursive: true),
      log_cpp23_test_objects,
      libsvm_static_lib.extract_all_objects(recursive: true),
    ] + wave8_opt_only_objects,
)
endif

psnr_hvs_simd_test_archs = ['x86_64', 'x86']
if psnr_hvs_simd_test_archs.contains(host_machine.cpu_family())
test_psnr_hvs_simd = executable('test_psnr_hvs_simd',
    ['test.c', 'test_psnr_hvs_simd.c',
     '../src/mem.cpp', '../src/picture.c', '../src/ref.cpp',
     '../src/dict.cpp',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp', dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'),
                           include_directories('../src/feature/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines + _simd_strict_fp_args,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib,
                    predict_test_dependencies, dnn_deps],
    objects : [
      common_cuda_objects,
      platform_specific_cpu_objects,
      libvmaf_cpu_static_lib.extract_all_objects(recursive: true),
      libvmaf_feature_static_lib.extract_all_objects(recursive: true),
      log_cpp23_test_objects,
      libsvm_static_lib.extract_all_objects(recursive: true),
    ] + wave8_opt_only_objects,
)
endif

# test_integer_adm_simd also runs on aarch64: the NEON scale-zero decouple
# (Netflix/vmaf 9e48141b) is held to the scalar kernels there.
integer_adm_simd_test_archs = ['x86_64', 'x86']
integer_adm_decouple_test_archs = integer_adm_simd_test_archs + ['aarch64']
if integer_adm_decouple_test_archs.contains(host_machine.cpu_family())
test_integer_adm_simd = executable('test_integer_adm_simd',
    ['test.c', 'test_integer_adm_simd.c',
     '../src/mem.cpp', '../src/picture.c', '../src/ref.cpp',
     '../src/dict.cpp',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp', dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'),
                           include_directories('../src/feature/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines + _simd_strict_fp_args,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib,
                    predict_test_dependencies, dnn_deps],
    objects : [
      common_cuda_objects,
      platform_specific_cpu_objects,
      libvmaf_cpu_static_lib.extract_all_objects(recursive: true),
      libvmaf_feature_static_lib.extract_all_objects(recursive: true),
      log_cpp23_test_objects,
      libsvm_static_lib.extract_all_objects(recursive: true),
    ] + wave8_opt_only_objects,
)
endif

if integer_adm_simd_test_archs.contains(host_machine.cpu_family())
test_adm_dwt2_x86 = executable('test_adm_dwt2_x86',
    ['test.c', 'test_adm_dwt2_x86.c',
     '../src/mem.cpp', '../src/picture.c', '../src/ref.cpp',
     '../src/dict.cpp',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp', dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'),
                           include_directories('../src/feature/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib,
                    predict_test_dependencies, dnn_deps],
    objects : [
      common_cuda_objects,
      platform_specific_cpu_objects,
      libvmaf_cpu_static_lib.extract_all_objects(recursive: true),
      libvmaf_feature_static_lib.extract_all_objects(recursive: true),
      log_cpp23_test_objects,
      libsvm_static_lib.extract_all_objects(recursive: true),
    ] + wave8_opt_only_objects,
)

# ADR-1473: the x86 float ADM wavelet and CSF kernels return the scalar
# functions' bits, and compute_adm() is the same on every dispatch level.
test_float_adm_x86 = executable('test_float_adm_x86',
    ['test.c', 'test_float_adm_x86.c',
     '../src/mem.cpp', '../src/picture.c', '../src/ref.cpp',
     '../src/dict.cpp',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp', dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'),
                           include_directories('../src/feature/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib,
                    predict_test_dependencies, dnn_deps],
    objects : [
      common_cuda_objects,
      platform_specific_cpu_objects,
      libvmaf_cpu_static_lib.extract_all_objects(recursive: true),
      libvmaf_feature_static_lib.extract_all_objects(recursive: true),
      log_cpp23_test_objects,
      libsvm_static_lib.extract_all_objects(recursive: true),
    ] + wave8_opt_only_objects,
)
endif


# Bit-exactness test for the AVX2 horizontal moment accumulation kernel (ADR-0784).
# Mirrors test_integer_adm_simd in structure; only the 8bpc / 16bpc accumulate_row
# paths are tested here.
integer_ssim_simd_test_archs = ['x86_64', 'x86']
if integer_ssim_simd_test_archs.contains(host_machine.cpu_family())
test_integer_ssim_simd = executable('test_integer_ssim_simd',
    ['test.c', 'test_integer_ssim_simd.c',
     '../src/mem.cpp', '../src/picture.c', '../src/ref.cpp',
     '../src/dict.cpp',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp', dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'),
                           include_directories('../src/feature/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib,
                    predict_test_dependencies, dnn_deps],
    objects : [
      common_cuda_objects,
      platform_specific_cpu_objects,
      libvmaf_cpu_static_lib.extract_all_objects(recursive: true),
      libvmaf_feature_static_lib.extract_all_objects(recursive: true),
      log_cpp23_test_objects,
      libsvm_static_lib.extract_all_objects(recursive: true),
    ] + wave8_opt_only_objects,
)
endif

# T-ICX-SSIM-AVX512-FP-CONTRACT-2026-10-01: the AVX2 and AVX-512 float SSIM
# kernels (precompute, variance, accumulate) against transcriptions of the
# scalar reference in iqa/ssim_tools.c, bit for bit, at element counts with
# and without a scalar tail. The x86 sibling of test_ssim_neon. The
# transcriptions need the same no-contraction policy as the kernels' carve-out
# libraries, hence _simd_strict_fp_args.
ssim_x86_simd_test_archs = ['x86_64', 'x86']
if ssim_x86_simd_test_archs.contains(host_machine.cpu_family())
test_ssim_x86_simd = executable('test_ssim_x86_simd',
    ['test.c', 'test_ssim_x86_simd.c',
     '../src/mem.cpp', '../src/picture.c', '../src/ref.cpp',
     '../src/dict.cpp',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp', dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'),
                           include_directories('../src/feature/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines + _simd_strict_fp_args,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib,
                    predict_test_dependencies, dnn_deps],
    objects : [
      common_cuda_objects,
      platform_specific_cpu_objects,
      libvmaf_cpu_static_lib.extract_all_objects(recursive: true),
      libvmaf_feature_static_lib.extract_all_objects(recursive: true),
      log_cpp23_test_objects,
      libsvm_static_lib.extract_all_objects(recursive: true),
    ] + wave8_opt_only_objects,
)
test('test_ssim_x86_simd', test_ssim_x86_simd, suite : ['fast', 'simd'])
endif

# T-FLOAT-SSIM-SUB-WINDOW-SIMD-COUNT-2026-10-02: iqa_ssim() on planes smaller
# than, equal to and larger than its 11x11 window, every SIMD dispatch of the
# host against the scalar path, bit for bit. The dispatch site handed the SIMD
# kernels the product of the two window extents, which is positive when both
# are negative: an 8x8 frame's float_ssim was read from uninitialised
# workspace, and a 4x4 frame wrote past it. Builds on every architecture; a
# host without a SIMD dispatch reports a skip.
test_iqa_ssim_sub_window = executable('test_iqa_ssim_sub_window',
    ['test.c', 'test_iqa_ssim_sub_window.c',
     '../src/mem.cpp', '../src/picture.c', '../src/ref.cpp',
     '../src/dict.cpp',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp', dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'),
                           include_directories('../src/feature/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib,
                    predict_test_dependencies, dnn_deps],
    objects : [
      common_cuda_objects,
      platform_specific_cpu_objects,
      libvmaf_cpu_static_lib.extract_all_objects(recursive: true),
      libvmaf_feature_static_lib.extract_all_objects(recursive: true),
      log_cpp23_test_objects,
      libsvm_static_lib.extract_all_objects(recursive: true),
    ] + wave8_opt_only_objects,
)
test('test_iqa_ssim_sub_window', test_iqa_ssim_sub_window, suite : ['fast', 'simd'])

ciede_simd_parity_test_archs = ['x86_64', 'x86']
if ciede_simd_parity_test_archs.contains(host_machine.cpu_family())
test_ciede_simd_parity = executable('test_ciede_simd_parity',
    ['test.c', 'test_ciede_simd_parity.c',
     '../src/mem.cpp', '../src/picture.c', '../src/ref.cpp',
     '../src/dict.cpp',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp', dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'),
                           include_directories('../src/feature/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib,
                    predict_test_dependencies, dnn_deps],
    objects : [
      common_cuda_objects,
      platform_specific_cpu_objects,
      libvmaf_cpu_static_lib.extract_all_objects(recursive: true),
      libvmaf_feature_static_lib.extract_all_objects(recursive: true),
      log_cpp23_test_objects,
      libsvm_static_lib.extract_all_objects(recursive: true),
    ] + wave8_opt_only_objects,
)
endif

motion_v2_simd_test_archs = ['x86_64', 'x86']
if motion_v2_simd_test_archs.contains(host_machine.cpu_family())
test_motion_v2_simd = executable('test_motion_v2_simd',
    ['test.c', 'test_motion_v2_simd.c',
     '../src/mem.cpp', '../src/picture.c', '../src/ref.cpp',
     '../src/dict.cpp',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp', dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'),
                           include_directories('../src/feature/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib,
                    predict_test_dependencies, dnn_deps],
    objects : [
      common_cuda_objects,
      platform_specific_cpu_objects,
      libvmaf_cpu_static_lib.extract_all_objects(recursive: true),
      libvmaf_feature_static_lib.extract_all_objects(recursive: true),
      log_cpp23_test_objects,
      libsvm_static_lib.extract_all_objects(recursive: true),
    ] + wave8_opt_only_objects,
)
endif

# ADR-0854: direct AVX-512 parity tests for the motion kernels.
# Covers motion_score_pipeline_{8,16}_avx512, sad_avx512,
# y_convolution_{8,16}_avx512, and x_convolution_16_avx512 against
# their scalar references.  Gated to x86_64/x86 (AVX-512 ISA); the
# test itself also gates on simd_test_have_avx512() at runtime so it
# skips cleanly on hosts without AVX-512.
motion_avx512_parity_test_archs = ['x86_64', 'x86']
# Also guard on is_avx512_enabled: when enable_avx512=false the AVX-512
# kernel objects are absent from platform_specific_cpu_objects, so
# linking test_motion_avx512_parity would fail with unresolved symbols.
# is_avx512_enabled is defined in src/meson.build (subdir called first).
if motion_avx512_parity_test_archs.contains(host_machine.cpu_family()) and is_avx512_enabled
test_motion_avx512_parity = executable('test_motion_avx512_parity',
    ['test.c', 'test_motion_avx512_parity.c',
     '../src/mem.cpp', '../src/picture.c', '../src/ref.cpp',
     '../src/dict.cpp',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp', dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'),
                           include_directories('../src/feature/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib,
                    predict_test_dependencies, dnn_deps],
    objects : [
      common_cuda_objects,
      platform_specific_cpu_objects,
      libvmaf_cpu_static_lib.extract_all_objects(recursive: true),
      libvmaf_feature_static_lib.extract_all_objects(recursive: true),
      log_cpp23_test_objects,
      libsvm_static_lib.extract_all_objects(recursive: true),
    ] + wave8_opt_only_objects,
)
endif

ssimulacra2_simd_test_archs = ['x86_64', 'x86', 'aarch64', 'arm64']
if ssimulacra2_simd_test_archs.contains(host_machine.cpu_family())
# Strict FP is required for bit-exactness:
# - aarch64: GCC/clang default to -ffp-contract=fast, which auto-fuses
#   mul-add pairs to fmla. NEON intrinsics are opaque to that fusion.
#   Without the flag, the scalar tail and SIMD loop drift by ~1 ULP.
# - x86 with icx (Intel oneAPI, meson id 'intel-llvm'): icx ignores
#   #pragma STDC FP_CONTRACT OFF unless -fp-model=precise is also on
#   the command line. This caused the "all-backends" CI failure on
#   master 2026-05-30 where the scalar reference (auto-contracted)
#   diverged from the SIMD lib (compiled with the strict-FP flags).
# _simd_strict_fp_args aliases the source policy: [-ffp-contract=off] on
# GCC/Clang, [-fp-model=precise, -ffp-contract=off] on Unix icx, and the
# compiler-native MSVC-style spellings on Windows drivers.
#
# ADR-0891 (round-2 fix): the ssimulacra2 ptlr tests use fmaf() in the
# scalar reference and fmadd intrinsics in the SIMD lib. On x86 without
# -mfma, GCC does NOT emit a hardware vfmadd for fmaf() — instead it
# calls libm or uses double-precision emulation, which may differ from
# vfmadd by 1 ULP. Adding -mavx2 -mfma ensures fmaf() in the test TU
# generates the same hardware vfmadd instruction as _mm256_fmadd_ps /
# _mm512_fmadd_ps in the SIMD libs, restoring bit-exactness on all
# GCC/Clang builds. Safe: the test is already x86_64/x86-gated, and
# the fmaf() result is identical to vfmadd on FMA-capable hardware.
# aarch64 does not need this — Clang's fmaf() is already vfmla on NEON.
_ssimulacra2_test_x86_fma_args = []
if host_machine.cpu_family() in ['x86_64', 'x86']
    _ssimulacra2_test_x86_fma_args = ['-mavx2', '-mfma']
endif
# ADR-1207 — a score must not depend on the host instruction set.
# Drives the public API twice over one fixture, once with the host ISA and
# once with cpumask disabling every SIMD flag, and asserts bit-identical
# scores. Complements the per-feature *_simd tests, which compare a SIMD
# kernel against a scalar reference defined inside the test TU rather than
# against the shipped one.
test_feature_isa_invariance = executable('test_feature_isa_invariance',
    ['test.c', 'test_feature_isa_invariance.c'],
    include_directories : [libvmaf_inc, test_inc],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies : [pthread_dependency, math_lib],
)
test('test_feature_isa_invariance', test_feature_isa_invariance,
    suite : ['fast'])

# CAMBI end-to-end dispatch invariance: every option path that routes through
# different CAMBI stages (10-bit, full reference, max_log_contrast 5, high-res
# speedup, window 33, 4:4:4 odd size), scored at each dispatch level the host
# offers (host / AVX2 only / scalar on x86, host / scalar on aarch64), must
# give bit-identical per-frame scores.
test_cambi_dispatch_invariance = executable('test_cambi_dispatch_invariance',
    ['test.c', 'test_cambi_dispatch_invariance.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies : [pthread_dependency, math_lib],
)
test('test_cambi_dispatch_invariance', test_cambi_dispatch_invariance,
    suite : ['fast', 'simd'])

# psnr_hvs whole-path dispatch invariance (T-PSNR-HVS-NEON-NOT-SCALAR-BITS-
# 2026-10-02): the extractor, driven through the public API with the host's
# instruction set and with every flag masked, returns the same score bits on
# blocks recorded from the Netflix pair, generated texture, random samples, the
# CPU-vs-GPU twin fixtures and edge inputs at 8 to 12 bits. The kernel tests
# (test_psnr_hvs_neon, test_psnr_hvs_avx2, test_psnr_hvs_simd) do not reach the
# shipped scalar calc_psnrhvs(); this one does, on x86 and on aarch64.
test_psnr_hvs_dispatch_invariance = executable('test_psnr_hvs_dispatch_invariance',
    ['test.c', 'test_psnr_hvs_dispatch_invariance.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies : [pthread_dependency, math_lib],
)
test('test_psnr_hvs_dispatch_invariance', test_psnr_hvs_dispatch_invariance,
    suite : ['fast', 'simd'])
test_integer_adm_tiny_frames = executable('test_integer_adm_tiny_frames',
    ['test.c', 'test_integer_adm_tiny_frames.c'],
    include_directories : [libvmaf_inc, test_inc],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies : [pthread_dependency, math_lib],
)
test('test_integer_adm_tiny_frames', test_integer_adm_tiny_frames,
    suite : ['fast'])

# T-CUDA-HIP-ADM-DWT-VERT-TINY-HEIGHT-OOB-2026-09-29 / ADR-1374: replays the
# rows the cuda ADM scale-0 vertical DWT loads for every plane height, from the
# header the kernel and its launch share. Device-free, so it runs everywhere.
test_cuda_adm_dwt2_rows = executable('test_cuda_adm_dwt2_rows',
    ['test.c', 'test_cuda_adm_dwt2_rows.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies : [pthread_dependency, math_lib],
)
test('test_cuda_adm_dwt2_rows', test_cuda_adm_dwt2_rows,
    suite : ['fast'])

# T-ADM-CM-ROUNDING-PLACEMENT-UNOBSERVABLE-2026-09-19: score-level parity
# loses one-unit rounding-placement errors at the float conversion. Exercise
# the internal row-fold seam on raw int64 accumulators instead.
test_adm_cm_row_rounding = executable('test_adm_cm_row_rounding',
    ['test.c', 'test_adm_cm_row_rounding.c'],
    include_directories : [test_inc, include_directories('../src')],
)
test('test_adm_cm_row_rounding', test_adm_cm_row_rounding,
    suite : ['fast'])

# T-CUDA-HIP-ADM-DWT-VERT-TINY-HEIGHT-OOB-2026-09-29 / ADR-1381: replays the
# rows the hip ADM scale-0 vertical DWT loads for every plane height, and the
# motion tile loads for every extent, from the headers the kernels and their
# launches share. Device-free, so it runs everywhere.
test_hip_adm_dwt2_rows = executable('test_hip_adm_dwt2_rows',
    ['test.c', 'test_hip_adm_dwt2_rows.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies : [pthread_dependency, math_lib],
)
test('test_hip_adm_dwt2_rows', test_hip_adm_dwt2_rows,
    suite : ['fast'])

# ADR-1402 (T-ADM-CM-CENTRE-TAP-WRAP-ABOVE-ONE-2026-10-01,
# T-ADM-CM-X86-TAIL-NEGATIVE-THRESHOLD-SHIFT-2026-10-01): the scale-0 masking
# threshold keeps its centre tap in int32 and the excess over it is clamped in
# int64. The picture tests score isolated patches on a flat reference on every
# dispatch level: nothing above 1, every level bit-identical to scalar. The
# 24x24 picture lands in the x86 scalar tails, where the sanitizer lane stopped
# on the old expression (left shift of a negative value).
test_integer_adm_cm_threshold = executable('test_integer_adm_cm_threshold',
    ['test.c', 'test_integer_adm_cm_threshold.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies : [pthread_dependency, math_lib],
)
test('test_integer_adm_cm_threshold', test_integer_adm_cm_threshold,
    suite : ['fast'])

# ADR-1475: the Watson quantisation step of integer ADM is upstream's
# arithmetic (the exponent k * temp * temp is a float product). The C test
# checks the CPU's values against that form and, on glibc, against the bits of
# a Netflix/vmaf build; the source contract reads the CPU header and the SYCL
# and Metal copies, with a planted regression per copy.
test_integer_adm_quant_step = executable('test_integer_adm_quant_step',
    ['test.c', 'test_integer_adm_quant_step.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies : [pthread_dependency, math_lib],
)
test('test_integer_adm_quant_step', test_integer_adm_quant_step,
    suite : ['fast'])

test('test_integer_adm_quant_step_contract',
    import('python').find_installation(),
    args : files('test_integer_adm_quant_step_contract.py'),
    suite : ['fast'],
    timeout : 30,
)

# T-ADM-CM-SIMD-NOISE-NOT-BIT-EXACT-2026-09-18: integer ADM SIMD dispatch
# against scalar on full-range noise.
test_integer_adm_simd_noise = executable('test_integer_adm_simd_noise',
    ['test.c', 'test_integer_adm_simd_noise.c'],
    include_directories : [libvmaf_inc, test_inc],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies : [pthread_dependency, math_lib],
)
test('test_integer_adm_simd_noise', test_integer_adm_simd_noise,
    suite : ['fast'])

# T-GPU-ADM-DWT2-16BIT-INT32-OVERFLOW-2026-09-18: the 16-bit vertical DWT sum
# must stay in int64 on every dispatch level.
test_integer_adm_dwt16_range = executable('test_integer_adm_dwt16_range',
    ['test.c', 'test_integer_adm_dwt16_range.c'],
    include_directories : [libvmaf_inc, test_inc],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies : [pthread_dependency, math_lib],
)
test('test_integer_adm_dwt16_range', test_integer_adm_dwt16_range,
    suite : ['fast'])

# T-ADM-INTEGER-AIM-ABOVE-ONE-2026-10-01 (ADR-1417): integer AIM is the
# unclipped ratio upstream defines, float AIM is clipped at 1. The float
# extractor is compiled only with -Denable_float=true.
if get_option('enable_float')
test_integer_adm_aim_unclipped = executable('test_integer_adm_aim_unclipped',
    ['test.c', 'test_integer_adm_aim_unclipped.c'],
    include_directories : [libvmaf_inc, test_inc],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies : [pthread_dependency, math_lib],
)
test('test_integer_adm_aim_unclipped', test_integer_adm_aim_unclipped,
    suite : ['fast'])
endif

test_ssimulacra2_simd = executable('test_ssimulacra2_simd',
    ['test.c', 'test_ssimulacra2_simd.c',
     '../src/mem.cpp', '../src/picture.c', '../src/ref.cpp',
     '../src/dict.cpp',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp', dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'),
                           include_directories('../src/feature/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines + _simd_strict_fp_args + _ssimulacra2_test_x86_fma_args,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib,
                    predict_test_dependencies, dnn_deps],
    objects : [
      common_cuda_objects,
      platform_specific_cpu_objects,
      libvmaf_cpu_static_lib.extract_all_objects(recursive: true),
      libvmaf_feature_static_lib.extract_all_objects(recursive: true),
      log_cpp23_test_objects,
      libsvm_static_lib.extract_all_objects(recursive: true),
    ] + wave8_opt_only_objects,
)
endif

psnr_hvs_neon_test_archs = ['aarch64', 'arm64']
if psnr_hvs_neon_test_archs.contains(host_machine.cpu_family())
test_psnr_hvs_neon = executable('test_psnr_hvs_neon',
    ['test.c', 'test_psnr_hvs_neon.c',
     '../src/mem.cpp', '../src/picture.c', '../src/ref.cpp',
     '../src/dict.cpp',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp', dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'),
                           include_directories('../src/feature/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib,
                    predict_test_dependencies, dnn_deps],
    objects : [
      common_cuda_objects,
      platform_specific_cpu_objects,
      libvmaf_cpu_static_lib.extract_all_objects(recursive: true),
      libvmaf_feature_static_lib.extract_all_objects(recursive: true),
      log_cpp23_test_objects,
      libsvm_static_lib.extract_all_objects(recursive: true),
    ] + wave8_opt_only_objects,
)

# ADR-1057: prove the universal adm_dwt2_8_neon kernel is scalar-bit-exact and
# separately lock the Apple production wrapper to its legacy j==0 contract.
test_adm_dwt2_neon = executable('test_adm_dwt2_neon',
    ['test.c', 'test_adm_dwt2_neon.c',
     '../src/mem.cpp', '../src/picture.c', '../src/ref.cpp',
     '../src/dict.cpp',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp', dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'),
                           include_directories('../src/feature/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib,
                    predict_test_dependencies, dnn_deps],
    objects : [
      common_cuda_objects,
      platform_specific_cpu_objects,
      libvmaf_cpu_static_lib.extract_all_objects(recursive: true),
      libvmaf_feature_static_lib.extract_all_objects(recursive: true),
      log_cpp23_test_objects,
      libsvm_static_lib.extract_all_objects(recursive: true),
    ] + wave8_opt_only_objects,
)
endif

# --- NEON parity tests: one per previously-uncovered arm64 kernel file ---
if psnr_hvs_neon_test_archs.contains(host_machine.cpu_family())
test_vif_neon = executable('test_vif_neon',
    ['test.c', 'test_vif_neon.c',
     '../src/mem.cpp', '../src/picture.c', '../src/ref.cpp',
     '../src/dict.cpp',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp', dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'),
                           include_directories('../src/feature/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib,
                    predict_test_dependencies, dnn_deps],
    objects : [
      common_cuda_objects,
      platform_specific_cpu_objects,
      libvmaf_cpu_static_lib.extract_all_objects(recursive: true),
      libvmaf_feature_static_lib.extract_all_objects(recursive: true),
      log_cpp23_test_objects,
      libsvm_static_lib.extract_all_objects(recursive: true),
    ] + wave8_opt_only_objects,
)

test_ssim_neon = executable('test_ssim_neon',
    ['test.c', 'test_ssim_neon.c',
     '../src/mem.cpp', '../src/picture.c', '../src/ref.cpp',
     '../src/dict.cpp',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp', dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'),
                           include_directories('../src/feature/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines + _simd_strict_fp_args,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib,
                    predict_test_dependencies, dnn_deps],
    objects : [
      common_cuda_objects,
      platform_specific_cpu_objects,
      libvmaf_cpu_static_lib.extract_all_objects(recursive: true),
      libvmaf_feature_static_lib.extract_all_objects(recursive: true),
      log_cpp23_test_objects,
      libsvm_static_lib.extract_all_objects(recursive: true),
    ] + wave8_opt_only_objects,
)

test_float_adm_neon = executable('test_float_adm_neon',
    ['test.c', 'test_float_adm_neon.c',
     '../src/mem.cpp', '../src/picture.c', '../src/ref.cpp',
     '../src/dict.cpp',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp', dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'),
                           include_directories('../src/feature/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines + _simd_strict_fp_args,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib,
                    predict_test_dependencies, dnn_deps],
    objects : [
      common_cuda_objects,
      platform_specific_cpu_objects,
      libvmaf_cpu_static_lib.extract_all_objects(recursive: true),
      libvmaf_feature_static_lib.extract_all_objects(recursive: true),
      log_cpp23_test_objects,
      libsvm_static_lib.extract_all_objects(recursive: true),
    ] + wave8_opt_only_objects,
)

test_float_adm_dwt2_neon = executable('test_float_adm_dwt2_neon',
    ['test.c', 'test_float_adm_dwt2_neon.c',
     '../src/mem.cpp', '../src/picture.c', '../src/ref.cpp',
     '../src/dict.cpp',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp', dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'),
                           include_directories('../src/feature/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib,
                    predict_test_dependencies, dnn_deps],
    objects : [
      common_cuda_objects,
      platform_specific_cpu_objects,
      libvmaf_cpu_static_lib.extract_all_objects(recursive: true),
      libvmaf_feature_static_lib.extract_all_objects(recursive: true),
      log_cpp23_test_objects,
      libsvm_static_lib.extract_all_objects(recursive: true),
    ] + wave8_opt_only_objects,
)

test_motion_neon = executable('test_motion_neon',
    ['test.c', 'test_motion_neon.c',
     '../src/mem.cpp', '../src/picture.c', '../src/ref.cpp',
     '../src/dict.cpp',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp', dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'),
                           include_directories('../src/feature/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib,
                    predict_test_dependencies, dnn_deps],
    objects : [
      common_cuda_objects,
      platform_specific_cpu_objects,
      libvmaf_cpu_static_lib.extract_all_objects(recursive: true),
      libvmaf_feature_static_lib.extract_all_objects(recursive: true),
      log_cpp23_test_objects,
      libsvm_static_lib.extract_all_objects(recursive: true),
    ] + wave8_opt_only_objects,
)

test_motion_pipeline_neon = executable('test_motion_pipeline_neon',
    ['test.c', 'test_motion_pipeline_neon.c',
     '../src/mem.cpp', '../src/picture.c', '../src/ref.cpp',
     '../src/dict.cpp',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp', dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'),
                           include_directories('../src/feature/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib,
                    predict_test_dependencies, dnn_deps],
    objects : [
      common_cuda_objects,
      platform_specific_cpu_objects,
      libvmaf_cpu_static_lib.extract_all_objects(recursive: true),
      libvmaf_feature_static_lib.extract_all_objects(recursive: true),
      log_cpp23_test_objects,
      libsvm_static_lib.extract_all_objects(recursive: true),
    ] + wave8_opt_only_objects,
)

test_float_motion_neon = executable('test_float_motion_neon',
    ['test.c', 'test_float_motion_neon.c',
     '../src/mem.cpp', '../src/picture.c', '../src/ref.cpp',
     '../src/dict.cpp',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp', dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'),
                           include_directories('../src/feature/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib,
                    predict_test_dependencies, dnn_deps],
    objects : [
      common_cuda_objects,
      platform_specific_cpu_objects,
      libvmaf_cpu_static_lib.extract_all_objects(recursive: true),
      libvmaf_feature_static_lib.extract_all_objects(recursive: true),
      log_cpp23_test_objects,
      libsvm_static_lib.extract_all_objects(recursive: true),
    ] + wave8_opt_only_objects,
)

test_psnr_neon = executable('test_psnr_neon',
    ['test.c', 'test_psnr_neon.c',
     '../src/mem.cpp', '../src/picture.c', '../src/ref.cpp',
     '../src/dict.cpp',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp', dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'),
                           include_directories('../src/feature/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib,
                    predict_test_dependencies, dnn_deps],
    objects : [
      common_cuda_objects,
      platform_specific_cpu_objects,
      libvmaf_cpu_static_lib.extract_all_objects(recursive: true),
      libvmaf_feature_static_lib.extract_all_objects(recursive: true),
      log_cpp23_test_objects,
      libsvm_static_lib.extract_all_objects(recursive: true),
    ] + wave8_opt_only_objects,
)

test_ciede_neon = executable('test_ciede_neon',
    ['test.c', 'test_ciede_neon.c',
     '../src/mem.cpp', '../src/picture.c', '../src/ref.cpp',
     '../src/dict.cpp',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp', dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'),
                           include_directories('../src/feature/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib,
                    predict_test_dependencies, dnn_deps],
    objects : [
      common_cuda_objects,
      platform_specific_cpu_objects,
      libvmaf_cpu_static_lib.extract_all_objects(recursive: true),
      libvmaf_feature_static_lib.extract_all_objects(recursive: true),
      log_cpp23_test_objects,
      libsvm_static_lib.extract_all_objects(recursive: true),
    ] + wave8_opt_only_objects,
)

endif

moment_simd_test_archs = ['x86_64', 'x86', 'aarch64', 'arm64']
if float_enabled and moment_simd_test_archs.contains(host_machine.cpu_family())
test_moment_simd = executable('test_moment_simd',
    ['test.c', 'test_moment_simd.c',
     '../src/mem.cpp', '../src/picture.c', '../src/ref.cpp',
     '../src/dict.cpp',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp', dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'),
                           include_directories('../src/feature/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib,
                    predict_test_dependencies, dnn_deps],
    objects : [
      common_cuda_objects,
      platform_specific_cpu_objects,
      libvmaf_cpu_static_lib.extract_all_objects(recursive: true),
      libvmaf_feature_static_lib.extract_all_objects(recursive: true),
      log_cpp23_test_objects,
      libsvm_static_lib.extract_all_objects(recursive: true),
    ] + wave8_opt_only_objects,
)
endif

# ADR-0865 / ADR-0720: test_ansnr_simd removed — ansnr feature was dropped.

# SpEED covariance row kernels (AVX2, AVX-512, NEON) and matrix-product
# kernels against their scalar references, bit for bit (ADR-1459).
# Gated on float_enabled (speed.c is float-only); the aarch64 cross build runs
# it under qemu.
speed_simd_test_archs = ['x86_64', 'x86', 'aarch64', 'arm64']
if float_enabled and speed_simd_test_archs.contains(host_machine.cpu_family())
test_speed_simd = executable('test_speed_simd',
    ['test.c', 'test_speed_simd.c',
     '../src/mem.cpp', '../src/picture.c', '../src/ref.cpp',
     '../src/dict.cpp',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp', dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'),
                           include_directories('../src/feature/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib,
                    predict_test_dependencies, dnn_deps],
    objects : [
      common_cuda_objects,
      platform_specific_cpu_objects,
      libvmaf_cpu_static_lib.extract_all_objects(recursive: true),
      libvmaf_feature_static_lib.extract_all_objects(recursive: true),
      log_cpp23_test_objects,
      libsvm_static_lib.extract_all_objects(recursive: true),
    ] + wave8_opt_only_objects,
)
endif

ms_ssim_decimate_test_archs = ['x86_64', 'x86', 'aarch64', 'arm64']
if ms_ssim_decimate_test_archs.contains(host_machine.cpu_family())
test_ms_ssim_decimate = executable('test_ms_ssim_decimate',
    ['test.c', 'test_ms_ssim_decimate.c',
     '../src/mem.cpp', '../src/picture.c', '../src/ref.cpp',
     '../src/dict.cpp',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp', dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'),
                           include_directories('../src/feature/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines + _simd_strict_fp_args,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib,
                    predict_test_dependencies, dnn_deps],
    objects : [
      common_cuda_objects,
      platform_specific_cpu_objects,
      libvmaf_cpu_static_lib.extract_all_objects(recursive: true),
      libvmaf_feature_static_lib.extract_all_objects(recursive: true),
      log_cpp23_test_objects,
      libsvm_static_lib.extract_all_objects(recursive: true),
    ] + wave8_opt_only_objects,
)
endif

test_locale_handling = executable('test_locale_handling',
    ['test.c', 'test_locale_handling.c'],
    include_directories : [
        libvmaf_inc,
        test_inc,
        include_directories('../src/'),
        include_directories('..')
    ],
    c_args : ['-I' + meson.project_build_root() / 'src'] + vmaf_cflags_common,
    dependencies : [pthread_dependency],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
)

# CPU-only regression test for the vmaf_gpu_picture_pool_init() UAF
# (caller's *pool was left dangling on failure paths). Built on every
# matrix entry — not gated by enable_cuda — so a future regression on
# the pure-C init shape is caught even when no GPU backend is in scope.
test_gpu_picture_pool_uaf = executable('test_gpu_picture_pool_uaf',
    ['test.c', 'test_gpu_picture_pool_uaf.c', '../src/gpu_picture_pool.cpp'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency],
)
test('test_gpu_picture_pool_uaf', test_gpu_picture_pool_uaf, suite : ['fast'],
    env : [
        'ASAN_OPTIONS=allocator_may_return_null=1',
        # TSan uses a separate allocator wrapper; without this override libtsan
        # calls abort() (exit 66) when the pool requests ~192 GB — the same
        # intentional huge-alloc that exercises the alloc-failure UAF path.
        # No data race is present; this is purely the allocator-abort behaviour.
        'TSAN_OPTIONS=allocator_may_return_null=1',
    ])

# ADR-0982 round-26 audit — regression for gpu_picture_pool_init partial-
# init unwind. CPU-only: uses pure-C stub alloc/free callbacks so no GPU
# runtime is required. ASan / UBSan catches any leak if the unwind in
# vmaf_gpu_picture_pool_init regresses.
test_gpu_picture_pool_partial_init = executable('test_gpu_picture_pool_partial_init',
    ['test.c', 'test_gpu_picture_pool_partial_init.c', '../src/gpu_picture_pool.cpp'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency],
)
test('test_gpu_picture_pool_partial_init', test_gpu_picture_pool_partial_init,
    suite : ['fast'])

# Issue #1455 regression: vmaf_gpu_picture_pool_init must return -ENOMEM
# and clear *pool to NULL when pool struct allocation fails.
# Recompiles gpu_picture_pool.cpp with force-included allocator interposer.
gpu_pool_interposer_lto_override = host_machine.system() == 'darwin' ? ['b_lto=false'] : []

gpu_pool_alloc_interpose_header = meson.current_source_dir() / 'test_gpu_picture_pool_alloc_interpose.h'
gpu_pool_alloc_interpose_args = cc.get_argument_syntax() == 'msvc' \
    ? ['/FI' + gpu_pool_alloc_interpose_header] \
    : ['-include', gpu_pool_alloc_interpose_header]

test_gpu_picture_pool_alloc_failure = executable('test_gpu_picture_pool_alloc_failure',
    ['test.c', 'test_gpu_picture_pool_alloc_failure.c', '../src/gpu_picture_pool.cpp'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    c_args : gpu_pool_alloc_interpose_args,
    cpp_args : gpu_pool_alloc_interpose_args,
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency],
    override_options : gpu_pool_interposer_lto_override,
)
test('test_gpu_picture_pool_alloc_failure', test_gpu_picture_pool_alloc_failure,
    suite : ['fast'])

if get_option('enable_cuda')
test_gpu_picture_pool = executable('test_gpu_picture_pool',
    ['test.c', 'test_gpu_picture_pool.c', '../src/gpu_picture_pool.cpp', '../src/cuda/picture_cuda.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, cuda_dependency],
    c_args: ['-DHAVE_CUDA=1']
)

test_cuda_pic_preallocation = executable('test_cuda_pic_preallocation',
    ['test.c', 'test_cuda_pic_preallocation.c'],
    include_directories : [libvmaf_inc, test_inc],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, cuda_dependency],
    c_args: ['-DHAVE_CUDA=1']
)

# Owner scoping of the ADR-0242 drain batch
# (docs/state.md T-UPSTREAM-1305-CUDA-DRAIN-BATCH-THREAD-GLOBAL-2026-09-03).
# Exercises only the bookkeeping paths — a foreign owner short-circuits the
# flush and the destroy path returns as soon as it sees a NULL drain stream —
# so it needs nvcc to compile but no live device, and is deliberately NOT in
# the 'gpu' suite.
test_cuda_drain_batch = executable('test_cuda_drain_batch',
    ['test.c', 'test_cuda_drain_batch.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, cuda_dependency],
    c_args: ['-DHAVE_CUDA=1']
)

test('test_gpu_picture_pool', test_gpu_picture_pool,
    suite : ['fast', 'gpu'],
    is_parallel : false)
test('test_cuda_pic_preallocation', test_cuda_pic_preallocation,
    suite : ['fast', 'gpu'],
    is_parallel : false)
test('test_cuda_drain_batch', test_cuda_drain_batch, suite : ['fast'])

# Error-path unwinds of the CUDA runtime (ADR-0982, restored after #504:
# docs/state.md T-GPU-ADR0982-REVERTED-BY-504-2026-09-18). Drives
# picture_cuda.c / common.c / drain_batch.c through a table of fake driver
# entry points, so it needs the nv-codec headers but no driver or device and
# is deliberately NOT in the 'gpu' suite. No --wrap, so it also runs under LTO.
test_cuda_runtime_unwind = executable('test_cuda_runtime_unwind',
    ['test.c', 'test_cuda_runtime_unwind.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, cuda_dependency],
    c_args: ['-DHAVE_CUDA=1']
)
test('test_cuda_runtime_unwind', test_cuda_runtime_unwind, suite : ['fast'])

# The flush drain loop must terminate (T-CUDA-SINGLE-FRAME-HANG-2026-09-05).
# Stub-driven, so it needs no CUDA device and no nvcc.
test_cuda_single_frame_flush = executable('test_cuda_single_frame_flush',
    ['test.c', 'test_cuda_single_frame_flush.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency],
)
test('test_cuda_single_frame_flush', test_cuda_single_frame_flush, suite : ['fast'])


# Finding R2-2 — HOST_PINNED picture-allocator dimension-overflow guard.
# vmaf_cuda_picture_alloc_pinned now rejects w/h out of [1, 32768] BEFORE
# touching cuda_state, mirroring vmaf_picture_alloc's VMAF_PIC_DIM_MAX guard
# (CERT INT30-C). The rejection paths run with cuda_state == NULL, so the
# test needs nvcc to compile picture_cuda.c but no live GPU device. Pre-fix
# the oversized call fell through to the NULL cuda_state deref (or under-
# allocated on a real device → OOB); post-fix it returns -EINVAL. Not in the
# 'gpu' suite because it deliberately exercises only the device-free guard.
test_cuda_picture_pinned_overflow = executable('test_cuda_picture_pinned_overflow',
    ['test.c', 'test_cuda_picture_pinned_overflow.c', '../src/cuda/picture_cuda.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, cuda_dependency],
    c_args: ['-DHAVE_CUDA=1']
)
test('test_cuda_picture_pinned_overflow', test_cuda_picture_pinned_overflow, suite : ['fast'])

# Netflix#1420 — graceful-OOM reducer. Exercises the wholesale
# CHECK_CUDA_GOTO/_RETURN rewrite by requesting an impossible
# 1 TiB cuMemAlloc and asserting the returned errno is -ENOMEM
# rather than an aborted process. GPU-gated: skips cleanly if
# no driver/device is visible to the process.
test_cuda_buffer_alloc_oom = executable('test_cuda_buffer_alloc_oom',
    ['test.c', 'test_cuda_buffer_alloc_oom.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, cuda_dependency],
    c_args: ['-DHAVE_CUDA=1']
)
test('test_cuda_buffer_alloc_oom', test_cuda_buffer_alloc_oom,
    suite : ['fast', 'gpu'],
    is_parallel : false)

# ADR-1223 — the compute-capability floor predicate. Pure function of
# (major, minor), so it runs in the `fast` suite with no GPU: the architectures
# it most needs to pin (Turing and older) are exactly the ones no runner has.
test_cuda_arch_floor = executable('test_cuda_arch_floor',
    ['test.c', 'test_cuda_arch_floor.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, cuda_dependency],
    c_args: ['-DHAVE_CUDA=1']
)
test('test_cuda_arch_floor', test_cuda_arch_floor,
    suite : ['fast', 'gpu'],
    is_parallel : false)

# PR #850 — libvmaf.c CUDA-only NULL deref guard.
# Before the fix, in CUDA-only mode (HW_FLAG_HOST not set) the
# unconditional `ref = &ref_host` reassignment passed a zero-initialised
# picture to the thread pool → NULL dereference.  The fix gates the
# reassignment behind `if (hw_flags & HW_FLAG_HOST)`.
# GPU-gated: skips cleanly when no CUDA device is visible.
test_libvmaf_cuda_only_no_host = executable('test_libvmaf_cuda_only_no_host',
    ['test.c', 'test_libvmaf_cuda_only_no_host.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, cuda_dependency, math_lib],
    c_args: ['-DHAVE_CUDA=1']
)
test('test_libvmaf_cuda_only_no_host', test_libvmaf_cuda_only_no_host,
    suite : ['fast', 'gpu'],
    is_parallel : false)

# Netflix#1300 / ADR-0157 — preallocation-leak reducer. Runs N
# init/preallocate/fetch/close cycles with FULL cleanup on each cycle
# (vmaf_close + vmaf_cuda_state_free + vmaf_model_destroy). ASan is the
# gate: pre-fix, the framework leaked the VmafCudaState struct, the
# CudaFunctions dlopen table, and the ring-buffer mutex state on every
# cycle. GPU-gated: skips cleanly if no driver/device is visible.
test_cuda_preallocation_leak = executable('test_cuda_preallocation_leak',
    ['test.c', 'test_cuda_preallocation_leak.c'],
    include_directories : [libvmaf_inc, test_inc],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, cuda_dependency],
    c_args: ['-DHAVE_CUDA=1']
)
test('test_cuda_preallocation_leak', test_cuda_preallocation_leak,
    suite : ['fast', 'gpu'],
    is_parallel : false)

# CHUG audit gap-fill — motion3 CPU vs. CUDA parity (ADR-0214 gate).
# The motion3 post-process (moving-average over blended motion2) is
# implemented independently in integer_motion.c (CPU) and
# integer_motion_cuda.c (CUDA).  No cross-backend assertion existed before
# this test; boundary-condition drift would silently pollute CHUG-extracted
# motion3_mean/std columns.  GPU-gated: skips cleanly when no CUDA device
# is visible (mirrors test_cuda_buffer_alloc_oom pattern).
test_cuda_motion3_parity = executable('test_cuda_motion3_parity',
    ['test.c', 'test_cuda_motion3_parity.c'],
    include_directories : [libvmaf_inc, test_inc],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, cuda_dependency, math_lib],
    c_args: ['-DHAVE_CUDA=1']
)
test('test_cuda_motion3_parity', test_cuda_motion3_parity,
    suite : ['fast', 'gpu'],
    is_parallel : false)

# motion_cuda emits the CPU `motion` extractor's whole output set: the SAD
# score of every frame (VMAF_integer_feature_motion_sad_score) next to
# motion2 / motion3 and the debug score. Eleven frames, so the twin's
# eight-frame batch boundary and its flush tail both emit; every output of
# every frame compared with == at 8 and 10 bits, also with debug, with
# motion_force_zero and with a weight and a cap. Fails on a twin without the
# SAD score.
test_cuda_motion_sad_score = executable('test_cuda_motion_sad_score',
    ['test.c', 'test_cuda_motion_sad_score.c'],
    include_directories : [libvmaf_inc, test_inc],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, cuda_dependency, math_lib],
    c_args: ['-DHAVE_CUDA=1']
)
test('test_cuda_motion_sad_score', test_cuda_motion_sad_score,
    suite : ['fast', 'gpu'],
    is_parallel : false,
    timeout : 60)

# ADR-1491: motion_cuda and motion_v2_cuda compute motion_five_frame_window
# with the CPU's bits. Every output of every frame, `==`, for 11, 1, 2 and 3
# frames at 8 and 10 bits, the option alone and with the other motion options
# (motion_five_frame_twin_parity.h). Fails on a twin without the window.
test_cuda_motion_five_frame_window = executable('test_cuda_motion_five_frame_window',
    ['test.c', 'test_cuda_motion_five_frame_window.c'],
    include_directories : [libvmaf_inc, test_inc],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, cuda_dependency, math_lib],
    c_args: ['-DHAVE_CUDA=1']
)
test('test_cuda_motion_five_frame_window', test_cuda_motion_five_frame_window,
    suite : ['fast', 'gpu'],
    is_parallel : false,
    timeout : 120)

# ADR-0597 — integer_vif CPU vs. CUDA parity gate.  Pins the luma-only
# design contract as a regression assertion so DEEP_AUDIT 2026-05-18
# finding 23 (false-positive "CUDA VIF skips chroma planes") cannot
# refile.  Also exercises the vestigial enable_chroma=true option to
# confirm its documented no-op behaviour (bit-identical to default).
# GPU-gated: skips cleanly when no CUDA device is visible.
test_integer_vif_cpu_cuda_parity = executable('test_integer_vif_cpu_cuda_parity',
    ['test.c', 'test_integer_vif_cpu_cuda_parity.c'],
    include_directories : [libvmaf_inc, test_inc],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, cuda_dependency, math_lib],
    c_args: ['-DHAVE_CUDA=1']
)
test('test_integer_vif_cpu_cuda_parity', test_integer_vif_cpu_cuda_parity,
    suite : ['fast', 'gpu'],
    is_parallel : false)

# ADR-0956 — CUDA kernel parity coverage round 4. Closes the last
# three float-path CPU twins (`float_adm_cuda`, `float_motion_cuda`,
# `float_ssim_cuda`) with cross-backend parity assertions at
# `places=4` / 1e-4 (ADR-0214). Each follows the
# `test_cuda_motion3_parity.c` template: 256x144 YUV420P 8-bpc
# synthetic fixture, 3 frames, score at frame index 1,
# `[skip: no CUDA device]` guard when vmaf_cuda_state_init fails.
test_cuda_float_adm_parity = executable('test_cuda_float_adm_parity',
    ['test.c', 'test_cuda_float_adm_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, cuda_dependency, math_lib],
    c_args: ['-DHAVE_CUDA=1']
)
test('test_cuda_float_adm_parity', test_cuda_float_adm_parity,
    suite : ['fast', 'gpu'],
    is_parallel : false)

test_cuda_float_motion_parity = executable('test_cuda_float_motion_parity',
    ['test.c', 'test_cuda_float_motion_parity.c'],
    include_directories : [libvmaf_inc, test_inc],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, cuda_dependency, math_lib],
    c_args: ['-DHAVE_CUDA=1']
)
test('test_cuda_float_motion_parity', test_cuda_float_motion_parity,
    suite : ['fast', 'gpu'],
    is_parallel : false)

test_cuda_float_ssim_parity = executable('test_cuda_float_ssim_parity',
    ['test.c', 'test_cuda_float_ssim_parity.c'],
    include_directories : [libvmaf_inc, test_inc],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, cuda_dependency, math_lib],
    c_args: ['-DHAVE_CUDA=1']
)
test('test_cuda_float_ssim_parity', test_cuda_float_ssim_parity,
    suite : ['fast', 'gpu'],
    is_parallel : false)

# T-GPU-FLOAT-SSIM-FRAME-SUM-ORDER-2026-10-02 — float_ssim_cuda adds its
# per-window terms in iqa_ssim()'s raster order. Scores the frame pair of
# float_ssim_order_frame.h (shared with the HIP and SYCL twin tests), on which a
# per-block sum of the same terms rounds the mean to the neighbouring float,
# and compares the float's bits with the CPU extractor's and with the recorded
# CPU bits. GPU-gated: exits 77 when no CUDA device is visible.
test_cuda_float_ssim_order = executable('test_cuda_float_ssim_order',
    ['test.c', 'test_cuda_float_ssim_order.c'],
    include_directories : [libvmaf_inc, test_inc],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, cuda_dependency, math_lib],
    c_args: ['-DHAVE_CUDA=1']
)
test('test_cuda_float_ssim_order', test_cuda_float_ssim_order,
    suite : ['fast', 'gpu'],
    is_parallel : false)

# The same contract without a device: the pass-2 kernels of ssim_score.cu store
# every window's terms at its raster position, the host adds each sum in index
# order, and the shared fixture header has the bytes the three lanes agreed on.
test('test_cuda_float_ssim_exact_contract',
    import('python').find_installation(),
    args : files('test_cuda_float_ssim_exact_contract.py'),
    suite : ['fast'],
    timeout : 30,
)

# ADR-1399 — the float_ssim_cuda decimation kernel against iqa_decimate():
# launches calculate_ssim_decimate_{8,16}bpc directly and compares the
# decimated planes byte for byte (scales 2-10, 16 and 128; 8 / 10 / 12 / 16
# bits; odd sizes). GPU-gated: skips cleanly when no CUDA device is visible.
test_cuda_float_ssim_decimate = executable('test_cuda_float_ssim_decimate',
    ['test.c', 'test_cuda_float_ssim_decimate.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, cuda_dependency, math_lib],
    c_args: ['-DHAVE_CUDA=1']
)
test('test_cuda_float_ssim_decimate', test_cuda_float_ssim_decimate,
    suite : ['fast', 'gpu'],
    is_parallel : false)

# ADR-1462 — the device half: loads the vif module, runs the extractor's
# table upload and compares all 32768 entries of the module's table with
# vif_log2_table_generate(); the table is empty before the upload, and the
# upload refuses a module without the table. Skips without a CUDA device.
test_cuda_vif_log2_table = executable('test_cuda_vif_log2_table',
    ['test.c', 'test_cuda_vif_log2_table.c'],
    include_directories : [libvmaf_inc, test_inc,
        include_directories('../src/', '../src/feature/', '../src/cuda/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, cuda_dependency, math_lib],
    c_args: ['-DHAVE_CUDA=1']
)
test('test_cuda_vif_log2_table', test_cuda_vif_log2_table,
    suite : ['fast', 'gpu'],
    is_parallel : false,
    timeout : 60)

# ADR-1457: the CUDA twins declared exact (motion, motion_v2, psnr, float_ssim,
# float_ms_ssim, cambi) return the CPU extractor's bits: four frames of
# 640x480 at 8 and at 10 bits, `==` on every output, the debug, L / C / S and
# per-scale outputs included. Exits 77 when no CUDA device is visible.
test_cuda_exact_twins = executable('test_cuda_exact_twins',
    ['test.c', 'test_cuda_exact_twins.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, cuda_dependency, math_lib],
    c_args: ['-DHAVE_CUDA=1']
)
test('test_cuda_exact_twins', test_cuda_exact_twins,
    suite : ['fast', 'gpu'],
    is_parallel : false,
    timeout : 120)

# ADR-0956 — CUDA-only speed kernels. `speed_chroma_cuda` and
# `speed_temporal_cuda` have no CPU twin emitting the same
# `Speed_*_feature_*_score` family, so the gate is a smoke test:
# register the extractor, run a 640x360 YUV420P fixture through the
# filter → eigendecomp → kernel pipeline (ADR-0567), and assert
# finite scores at frame index 1. Catches NaN/Inf drift from kernel
# grid changes or covariance-matrix degenerate cases.
test_cuda_speed_chroma_smoke = executable('test_cuda_speed_chroma_smoke',
    ['test.c', 'test_cuda_speed_chroma_smoke.c'],
    include_directories : [libvmaf_inc, test_inc],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, cuda_dependency, math_lib],
    c_args: ['-DHAVE_CUDA=1']
)
test('test_cuda_speed_chroma_smoke', test_cuda_speed_chroma_smoke,
    suite : ['fast', 'gpu'],
    is_parallel : false)

test_cuda_speed_temporal_smoke = executable('test_cuda_speed_temporal_smoke',
    ['test.c', 'test_cuda_speed_temporal_smoke.c'],
    include_directories : [libvmaf_inc, test_inc],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, cuda_dependency, math_lib],
    c_args: ['-DHAVE_CUDA=1']
)
test('test_cuda_speed_temporal_smoke', test_cuda_speed_temporal_smoke,
    suite : ['fast', 'gpu'],
    is_parallel : false)

# ADR-0965 — speed_chroma + speed_temporal CPU vs. CUDA parity gates.
# speed_temporal pins the cross-backend tolerance (places=4); speed_chroma
# compares its three scores at every frame to one part in a million on a
# fixture with a regular covariance (ADR-1430: what is left is the CPU's
# log2f).  Both tests skip cleanly when no CUDA device is visible
# (mirrors test_cuda_motion3_parity.c).
test_cuda_speed_chroma_parity = executable('test_cuda_speed_chroma_parity',
    ['test.c', 'test_cuda_speed_chroma_parity.c'],
    include_directories : [libvmaf_inc, test_inc],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, cuda_dependency, math_lib],
    c_args: ['-DHAVE_CUDA=1']
)
test('test_cuda_speed_chroma_parity', test_cuda_speed_chroma_parity,
    suite : ['fast', 'gpu'],
    is_parallel : false)

test_cuda_speed_temporal_parity = executable('test_cuda_speed_temporal_parity',
    ['test.c', 'test_cuda_speed_temporal_parity.c'],
    include_directories : [libvmaf_inc, test_inc],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, cuda_dependency, math_lib],
    c_args: ['-DHAVE_CUDA=1']
)
test('test_cuda_speed_temporal_parity', test_cuda_speed_temporal_parity,
    suite : ['fast', 'gpu'],
    is_parallel : false)

# 1920x1080 variant (T-CUDA-SPEED-TEMPORAL-SOLVE-LAUNCH-1080P-2026-09-30): the
# 1080p luma plane has 312 SpEED blocks. The host-split twin before ADR-1380
# launched its solve kernel with ((blocks + 7) / 8) * 32 threads per block,
# 1248 > 1024, so every frame at 1080p and above failed with
# CUDA_ERROR_INVALID_VALUE. The 768x432 and 960x540 fixtures stay below 256
# blocks and never reached it.
test_cuda_speed_temporal_parity_1080p = executable('test_cuda_speed_temporal_parity_1080p',
    ['test.c', 'test_cuda_speed_temporal_parity.c'],
    include_directories : [libvmaf_inc, test_inc],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, cuda_dependency, math_lib],
    c_args: ['-DHAVE_CUDA=1', '-DFIXTURE_W=1920u', '-DFIXTURE_H=1080u']
)
test('test_cuda_speed_temporal_parity_1080p', test_cuda_speed_temporal_parity_1080p,
    suite : ['fast', 'gpu'],
    is_parallel : false)

test_cuda_speed_singular_parity = executable('test_cuda_speed_singular_parity',
    ['test.c', 'test_cuda_speed_singular_parity.c'],
    include_directories : [libvmaf_inc, test_inc],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, cuda_dependency, math_lib],
    c_args: ['-DHAVE_CUDA=1']
)
test('test_cuda_speed_singular_parity', test_cuda_speed_singular_parity,
    suite : ['fast', 'gpu'],
    is_parallel : false,
    timeout : 90)

# lanczos4 prescale (T-GPU-SPEED-LANCZOS4-PRESCALE-DRIFT-2026-09-30): the scale
# kernel reads the weights the host evaluates with the CPU scaler's own
# routine instead of approximating the fp64 sines in fp32. A gradient fixture,
# prescale 0.5, 2.0 and 1.0, both twins against the CPU extractors. The table
# itself is checked without a device by test_speed_lanczos4_weights. One
# source for every backend; the SYCL executable is built further down.
test_cuda_speed_lanczos4_parity = executable('test_cuda_speed_lanczos4_parity',
    ['test.c', 'test_gpu_speed_lanczos4_parity.c'],
    include_directories : [libvmaf_inc, test_inc],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, cuda_dependency, math_lib],
    c_args: ['-DHAVE_CUDA=1']
)
test('test_cuda_speed_lanczos4_parity', test_cuda_speed_lanczos4_parity,
    suite : ['fast', 'gpu'],
    is_parallel : false,
    timeout : 120)

# ADR-0947 — CUDA kernel parity coverage, round 3. Closes the float-path
# twin gap (float_psnr, float_vif, float_ms_ssim, float_moment) plus the
# new ssimulacra2_cuda kernel.  Each test asserts CPU-vs-CUDA agreement
# at places=4 / 1e-4 (ADR-0214 cross-backend gate) on a 256x144 YUV420P
# fixture, 3 frames, score read at index 1.  GPU-gated: skips cleanly
# when no CUDA device is visible (mirrors test_cuda_motion3_parity).
#
# ADR-1455 — float_psnr_cuda returns the CPU's score bit for bit: the kernel
# forms the CPU's float square of each difference as an integer and the
# blocks and the host add integers. One binary runs full-range noise at 8,
# 10, 12 and 16 bits, `uncapped`, a bright 16-bit 1920x1080 pair and
# identical frames with ==, and a 16-bit 2560x1440 frame whose sum is past
# 2^53, where the CPU's own sum rounds, against the derived bound
# (float_psnr_twin_parity.h). The `_large` variant below runs the noise cases
# at 960x540. The device-free half is test_cuda_float_psnr_exact_contract,
# registered for every build.
test_cuda_float_psnr_parity = executable('test_cuda_float_psnr_parity',
    ['test.c', 'test_cuda_float_psnr_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, cuda_dependency, math_lib],
    c_args: ['-DHAVE_CUDA=1']
)
test('test_cuda_float_psnr_parity', test_cuda_float_psnr_parity,
    suite : ['fast', 'gpu'],
    is_parallel : false,
    timeout : 120)

test_cuda_float_vif_parity = executable('test_cuda_float_vif_parity',
    ['test.c', 'test_cuda_float_vif_parity.c'],
    include_directories : [libvmaf_inc, test_inc],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, cuda_dependency, math_lib],
    c_args: ['-DHAVE_CUDA=1']
)
test('test_cuda_float_vif_parity', test_cuda_float_vif_parity,
    suite : ['fast', 'gpu'],
    is_parallel : false)

test_cuda_float_ms_ssim_parity = executable('test_cuda_float_ms_ssim_parity',
    ['test.c', 'test_cuda_float_ms_ssim_parity.c'],
    include_directories : [libvmaf_inc, test_inc],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, cuda_dependency, math_lib],
    c_args: ['-DHAVE_CUDA=1']
)
test('test_cuda_float_ms_ssim_parity', test_cuda_float_ms_ssim_parity,
    suite : ['fast', 'gpu'],
    is_parallel : false)

# T-CUDA-FLOAT-MS-SSIM-FRAME-SUM-ORDER-2026-10-02 — float_ms_ssim_cuda adds the
# l, c and s terms of every scale in iqa_ssim()'s raster order (ADR-1465).
# Scores the frame pair of float_ms_ssim_order_frame.h (shared with the HIP and
# SYCL twin tests), on which per-block sums of the same terms round
# float_ms_ssim_c_scale1 to the neighbouring float, and a second pair it
# rebuilds from a formula (float_ms_ssim_l_scale0), and compares all sixteen
# outputs with the CPU extractor's and the recorded CPU bits. GPU-gated: exits
# 77 when no CUDA device is visible.
test_cuda_float_ms_ssim_order = executable('test_cuda_float_ms_ssim_order',
    ['test.c', 'test_cuda_float_ms_ssim_order.c'],
    include_directories : [libvmaf_inc, test_inc],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, cuda_dependency, math_lib],
    c_args: ['-DHAVE_CUDA=1']
)
test('test_cuda_float_ms_ssim_order', test_cuda_float_ms_ssim_order,
    suite : ['fast', 'gpu'],
    is_parallel : false)

# The same contract without a device: ms_ssim_vert_lcs stores every window's
# terms at its raster position and the host adds each scale's three sums in
# index order.
test('test_cuda_float_ms_ssim_exact_contract',
    import('python').find_installation(),
    args : files('test_cuda_float_ms_ssim_exact_contract.py'),
    suite : ['fast'],
    timeout : 30,
)

# ADR-1453 — float_moment_cuda returns the CPU's four moments bit for bit
# (first added as a places=4 test at 8 and 10 bpc, ADR-0947 / ADR-1212). One
# binary runs noise at 8, 10, 12 and 16 bits and a bright 16-bit 1920x1080
# frame with ==, and a bright 16-bit 2560x1440 frame, where the CPU's own sum
# is past 2^53 and rounds, against the derived bound
# (float_moment_twin_parity.h). The `_large` variant below runs the noise
# cases at 960x540. The device-free half is
# test_cuda_float_moment_exact_contract, registered for every build.
test_cuda_float_moment_parity = executable('test_cuda_float_moment_parity',
    ['test.c', 'test_cuda_float_moment_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, cuda_dependency, math_lib],
    c_args: ['-DHAVE_CUDA=1']
)
test('test_cuda_float_moment_parity', test_cuda_float_moment_parity,
    suite : ['fast', 'gpu'],
    is_parallel : false,
    timeout : 120)

test_cuda_ssimulacra2_parity = executable('test_cuda_ssimulacra2_parity',
    ['test.c', 'test_cuda_ssimulacra2_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, cuda_dependency, math_lib],
    c_args: ['-DHAVE_CUDA=1']
)
test('test_cuda_ssimulacra2_parity', test_cuda_ssimulacra2_parity,
    suite : ['fast', 'gpu'],
    is_parallel : false)

# ADR-1433: ssimulacra2_cuda returns the CPU's score bit for bit -- the sums
# of the per-pixel terms are the sums of the CPU's loops (chunks of pixels in
# raster order, increments composed in pixel order, one walk per sum with a
# term-by-term fallback), not a tree. Device-free; the arithmetic check is
# test_ordered_sum, the device check is test_cuda_ssimulacra2_parity.
test('test_cuda_ssimulacra2_exact_contract',
    import('python').find_installation(),
    args : files('test_cuda_ssimulacra2_exact_contract.py'),
    suite : ['fast'],
    timeout : 30,
)

# Round-2 GPU-kernel coverage gap-fill — adm2/adm3 CPU vs. CUDA parity
# (places=4). Round-1 (PR #351) covered psnr_cuda + ciede_cuda; ADM was
# the highest-impact remaining CUDA gap (default model component).
# GPU-gated: skips cleanly when no CUDA device is visible.
test_cuda_adm_parity = executable('test_cuda_adm_parity',
    ['test.c', 'test_cuda_adm_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, cuda_dependency, math_lib],
    c_args: ['-DHAVE_CUDA=1']
)
test('test_cuda_adm_parity', test_cuda_adm_parity,
    suite : ['fast', 'gpu'],
    is_parallel : false)

# ADR-1167 — ADM small-frame border parity test (i == 0 && top <= 0 branch).
test_cuda_adm_small_border = executable('test_cuda_adm_small_border',
    ['test.c', 'test_adm_small_border.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, cuda_dependency, math_lib],
    c_args: ['-DHAVE_CUDA=1']
)
test('test_cuda_adm_small_border', test_cuda_adm_small_border,
    suite : ['fast', 'gpu'],
    is_parallel : false)

# T-GPU-ADM-TINY-FRAME-SHIFT-2026-09-18 — integer ADM on frames 17 to 32 pixels
# wide (scale-0 cube shift 0) against scalar CPU, and rejection below 17x17.
test_cuda_adm_tiny_frames = executable('test_cuda_adm_tiny_frames',
    ['test.c', 'test_gpu_adm_tiny_frames.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, cuda_dependency, math_lib],
    c_args: ['-DHAVE_CUDA=1']
)
test('test_cuda_adm_tiny_frames', test_cuda_adm_tiny_frames,
    suite : ['fast', 'gpu'],
    is_parallel : false)

# T-CUDA-MOTION-BLUR-THEN-DIFF-2026-09-29 / ADR-1372 — motion_cuda and
# motion_v2_cuda against the scalar CPU motion / motion_v2 with == on 3x3 to
# 1283x723 frames at 8, 10 and 16 bits (ten frames cross motion_cuda's
# eight-frame readback batch), the fps-weighted debug score, and rejection
# below 3x3, which needs no device.
test_cuda_motion_tiny_frames = executable('test_cuda_motion_tiny_frames',
    ['test.c', 'test_cuda_motion_tiny_frames.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, cuda_dependency, math_lib],
    c_args: ['-DHAVE_CUDA=1']
)
test('test_cuda_motion_tiny_frames', test_cuda_motion_tiny_frames,
    suite : ['fast', 'gpu'],
    is_parallel : false,
    timeout : 120)

# T-UPSTREAM-1305-CUDA-VIF-ACCUM-STREAM-2026-10-01 (Netflix/vmaf#1305) — four
# libvmaf CUDA instances on one CUcontext score the same pictures as one
# instance does, bit for bit. integer_vif_cuda reset its accumulators on a
# stream the scale 0 kernels did not wait for; skips (77) with no CUDA device.
test_cuda_multi_instance = executable('test_cuda_multi_instance',
    ['test.c', 'test_cuda_multi_instance.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, cuda_dependency, math_lib],
    c_args: ['-DHAVE_CUDA=1']
)
test('test_cuda_multi_instance', test_cuda_multi_instance,
    suite : ['fast', 'gpu'],
    is_parallel : false)

# ADR-1431 / Netflix#1420 — an out-of-memory while the first frame is read
# returns an error and leaves the picture pool whole: the test takes the
# device's memory itself, submits a pair from a pool of one pair, then submits
# the same index again. Skips (77) without a CUDA device.
test_cuda_oom_pictures_released = executable('test_cuda_oom_pictures_released',
    ['test.c', 'test_cuda_oom_pictures_released.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, cuda_dependency, math_lib],
    c_args: ['-DHAVE_CUDA=1']
)
test('test_cuda_oom_pictures_released', test_cuda_oom_pictures_released,
    suite : ['fast', 'gpu'],
    is_parallel : false)

# T-CUDA-DEVICE-INPUT-CHROMA-NOT-DOWNLOADED-2026-10-01 (Netflix/vmaf#1613) — a
# CPU extractor scores device-resident input on all three planes: the download
# into the host picture copied luma only. Skips (77) without a CUDA device.
test_cuda_device_input_host_extractors = executable('test_cuda_device_input_host_extractors',
    ['test.c', 'test_cuda_device_input_host_extractors.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, cuda_dependency, math_lib],
    c_args: ['-DHAVE_CUDA=1']
)
test('test_cuda_device_input_host_extractors', test_cuda_device_input_host_extractors,
    suite : ['fast', 'gpu'],
    is_parallel : false)

# T-GPU-INTEGER-VIF-MIN-DIM-TWINS-2026-09-29 / ADR-1374 — vif_cuda declares its
# 16-pixel minimum through the ADR-1324 gate: model dispatch falls back to the
# CPU `vif` below it (bit-identical) and a direct request fails init. The
# declaration and the direct rejection need no device.
test_cuda_vif_min_dim = executable('test_cuda_vif_min_dim',
    ['test.c', 'test_cuda_vif_min_dim.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, cuda_dependency, math_lib],
    c_args: ['-DHAVE_CUDA=1']
)
test('test_cuda_vif_min_dim', test_cuda_vif_min_dim,
    suite : ['fast', 'gpu'],
    is_parallel : false)

# T-BUG048-GPU-OPTION-PARITY-REMAINDER-2026-09-26 (CUDA part) / ADR-1373 —
# psnr_cuda, integer_ssim_cuda, float_ssim_cuda and float_motion_cuda take
# their CPU extractor's options. The option-table checks run without a
# device; the parity checks skip (77) when no CUDA device is visible.
test_cuda_twin_option_parity = executable('test_cuda_twin_option_parity',
    ['test.c', 'test_cuda_twin_option_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, cuda_dependency, math_lib],
    c_args: ['-DHAVE_CUDA=1']
)
test('test_cuda_twin_option_parity', test_cuda_twin_option_parity,
    suite : ['fast', 'gpu'],
    is_parallel : false)

# ADR-1167 — ADM wide-frame row-level rounding parity test (spans 60 warps).
test_cuda_adm_wide_rounding = executable('test_cuda_adm_wide_rounding',
    ['test.c', 'test_adm_wide_rounding.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, cuda_dependency, math_lib],
    c_args: ['-DHAVE_CUDA=1']
)
test('test_cuda_adm_wide_rounding', test_cuda_adm_wide_rounding,
    suite : ['fast', 'gpu'],
    is_parallel : false)

# Round-2 GPU-kernel coverage gap-fill — motion_v2 CPU vs. CUDA parity
# (places=4). Covers the SAD-weighted motion variant used by the
# "vmaf_v0.6.1neg" model lineage. Distinct from test_cuda_motion3_parity
# (which exercises the motion3 post-process, not the SAD reduction).
test_cuda_motion_v2_parity = executable('test_cuda_motion_v2_parity',
    ['test.c', 'test_cuda_motion_v2_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, cuda_dependency, math_lib],
    c_args: ['-DHAVE_CUDA=1']
)
test('test_cuda_motion_v2_parity', test_cuda_motion_v2_parity,
    suite : ['fast', 'gpu'],
    is_parallel : false)

# Round-2 GPU-kernel coverage gap-fill — cambi CPU vs. CUDA parity
# (places=4). Fixture is 256x256 to satisfy CAMBI_MIN_WIDTH_HEIGHT=216.
# Locks the banding-detector parity used by libvmaf-2.x.x neg + 4k models.
test_cuda_cambi_parity = executable('test_cuda_cambi_parity',
    ['test.c', 'test_cuda_cambi_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, cuda_dependency, math_lib],
    c_args: ['-DHAVE_CUDA=1']
)
test('test_cuda_cambi_parity', test_cuda_cambi_parity,
    suite : ['fast', 'gpu'],
    is_parallel : false)

# psnr_hvs CPU vs. CUDA: bit-identical scores (ADR-1397) on every plane and
# the combined score, at 8 to 12 bits, in 4:0:0 / 4:2:0 / 4:2:2 / 4:4:4, and
# at 3840x2160, where a twin that sums per block is 1e-2 dB from the CPU
# (third_party/xiph/psnr_hvs.c vs. integer_psnr_hvs/psnr_hvs_score.cu).
test_cuda_psnr_hvs_parity = executable('test_cuda_psnr_hvs_parity',
    ['test.c', 'test_cuda_psnr_hvs_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, cuda_dependency, math_lib],
    c_args: ['-DHAVE_CUDA=1']
)
test('test_cuda_psnr_hvs_parity', test_cuda_psnr_hvs_parity,
    suite : ['fast', 'gpu'],
    is_parallel : false)

# Round-2 GPU-kernel coverage gap-fill — integer_ssim CPU vs. CUDA
# parity (places=4). Covers the 11x11 Gaussian-windowed SSIM kernel
# (integer_ssim.c vs. integer_ssim/integer_ssim_score.cu).
test_cuda_ssim_parity = executable('test_cuda_ssim_parity',
    ['test.c', 'test_cuda_ssim_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, cuda_dependency, math_lib],
    c_args: ['-DHAVE_CUDA=1']
)
test('test_cuda_ssim_parity', test_cuda_ssim_parity,
    suite : ['fast', 'gpu'],
    is_parallel : false)

# ADR-1424: integer_ssim_cuda returns the CPU's score bit for bit -- the kernel
# stores one double term per pixel instead of reducing them, and the host adds
# the plane in calc_ssim()'s raster order. Device-free; the device check is
# test_cuda_ssim_parity.
test('test_cuda_ssim_exact_contract',
    import('python').find_installation(),
    args : files('test_cuda_ssim_exact_contract.py'),
    suite : ['fast'],
    timeout : 30,
)

# GPU-kernel coverage gap-fill — psnr CPU vs. CUDA parity (places=4).
# Pre-existing CUDA tests covered motion3 + vif; psnr_cuda
# (integer_psnr_cuda.c + integer_psnr/psnr_score.cu) was uncovered.
# GPU-gated: skips cleanly when no CUDA device is visible.
test_cuda_psnr_parity = executable('test_cuda_psnr_parity',
    ['test.c', 'test_cuda_psnr_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, cuda_dependency, math_lib],
    c_args: ['-DHAVE_CUDA=1']
)
test('test_cuda_psnr_parity', test_cuda_psnr_parity,
    suite : ['fast', 'gpu'],
    is_parallel : false)

# ADR-1215 — the same TU at 10 bpc with non-flat chroma. The 16-bpc CUDA PSNR
# kernel read data[0] for every plane, so high-bit-depth chroma PSNR was a luma
# value; every psnr fixture was 8-bit and flat-chroma, which is how it survived.
test_cuda_psnr_parity_10bit = executable('test_cuda_psnr_parity_10bit',
    ['test.c', 'test_cuda_psnr_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, cuda_dependency, math_lib],
    c_args: ['-DHAVE_CUDA=1', '-DFIXTURE_BPC=10u']
)
test('test_cuda_psnr_parity_10bit', test_cuda_psnr_parity_10bit,
    suite : ['fast', 'gpu'],
    is_parallel : false)

# GPU-kernel coverage gap-fill — ciede2000 CPU vs. CUDA parity (places=4).
# Covers integer_ciede_cuda.c + integer_ciede/ciede_score.cu, which had
# no cross-backend gate before. GPU-gated: skips when no CUDA device.
test_cuda_ciede_parity = executable('test_cuda_ciede_parity',
    ['test.c', 'test_cuda_ciede_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, cuda_dependency, math_lib],
    c_args: ['-DHAVE_CUDA=1']
)
test('test_cuda_ciede_parity', test_cuda_ciede_parity,
    suite : ['fast', 'gpu'],
    is_parallel : false)

# ADR-1426: ciede_cuda runs ciede.c's arithmetic -- fp64 math calls with the
# promotions C applies written out, float stores where the reference stores a
# float, no device reduction, the host adding the plane in raster order.
# Device-free; the arithmetic check is test_ciede_device_math, the device
# check is test_cuda_ciede_parity.
test('test_cuda_ciede_exact_contract',
    import('python').find_installation(),
    args : files('test_cuda_ciede_exact_contract.py'),
    suite : ['fast'],
    timeout : 30,
)

# ADR-1206 — large-fixture parity variants (resolution blind spot).
#
# Every CUDA parity test above pins a single small fixture (256x144 and
# friends). min(w,h) < 384 there, so the shared SSIM/MS-SSIM auto-scale
# formula `max(1, round(min(w,h) / 256))` always resolves to 1 and every
# resolution-dependent branch in the GPU twins stays unreachable. Two real
# divergences hid behind exactly that gap: the speed_chroma 4K bug and the
# float_ssim scale asymmetry. These variants rebuild the same TUs against a
# 960x540 fixture (min dim 540 -> auto-scale 2, and 540 is not a multiple of
# the 16/32-wide kernel blocks, so tail-bound handling is exercised too).
cuda_parity_large_fixture_tests = [
    'test_cuda_adm_parity',
    'test_cuda_cambi_parity',
    'test_cuda_ciede_parity',
    'test_cuda_float_adm_parity',
    'test_cuda_float_moment_parity',
    'test_cuda_float_motion_parity',
    'test_cuda_float_ms_ssim_parity',
    'test_cuda_float_psnr_parity',
    'test_cuda_float_ssim_parity',
    'test_cuda_float_vif_parity',
    'test_cuda_motion3_parity',
    'test_cuda_motion_v2_parity',
    'test_cuda_psnr_hvs_parity',
    'test_cuda_psnr_parity',
    'test_cuda_ssim_parity',
    'test_cuda_ssimulacra2_parity',
]
foreach parity_test : cuda_parity_large_fixture_tests
    parity_large_exe = executable(parity_test + '_large',
        ['test.c', parity_test + '.c'],
        include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
        link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
        dependencies: [pthread_dependency, cuda_dependency, math_lib],
        c_args: ['-DHAVE_CUDA=1', '-DFIXTURE_W=960u', '-DFIXTURE_H=540u']
    )
    test(parity_test + '_large', parity_large_exe,
        suite : ['fast', 'gpu'],
        is_parallel : false)
endforeach

endif

# T6-2b / ADR-0247 — vmaf-roi sidecar core helpers (per-CTU mean reduce
# + saliency-to-QP-offset). Header-only test that compiles
# core/tools/vmaf_roi_core.h and exercises clamp / sign / monotonicity
# without needing the full binary.
test_vmaf_roi_libm = cc.find_library('m', required : false)
test_vmaf_roi = executable('test_vmaf_roi',
    ['test.c', 'test_vmaf_roi.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../tools/')],
    dependencies: [pthread_dependency, test_vmaf_roi_libm],
)
test('test_vmaf_roi', test_vmaf_roi, suite : ['fast'])

# Exercise the production private ROI reader and placeholder, not copied math.
test_vmaf_roi_bounds = executable('test_vmaf_roi_bounds',
    ['test.c', 'test_vmaf_roi_bounds.c'],
    include_directories : [libvmaf_inc, test_inc],
    dependencies : [test_vmaf_roi_libm],
)
test('test_vmaf_roi_bounds', test_vmaf_roi_bounds, suite : ['fast'])

# picture pool test - exercises vmaf_preallocate_pictures / vmaf_fetch_preallocated_picture.
# These symbols are always linked into libvmaf (no build gate), so this test
# always runs.
test_pic_preallocation = executable('test_pic_preallocation',
    ['test.c', 'test_pic_preallocation.c'],
    include_directories : [libvmaf_inc, test_inc],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, thread_lib],
)
# Runs the vmaf_v0.6.1 model on 1080p frames of the preallocated pools: about
# 9 s of CPU in the ASan + UBSan debug build (46 % in the AVX-512 VIF kernel,
# unoptimised). Above meson's default 30 s on a
# hosted runner (Sanitizers ASan+UBSan, 2026-10-02; three runs). Measured cost:
# docs/state.md T-CI-MASTER-FIRST-FULL-RUN-2026-10-02, item 8.
test('test_pic_preallocation', test_pic_preallocation, suite : ['fast'], timeout : 180)

# ADR-1072/ADR-1073 thread-safety regression tests for threaded_extract_batch_func.
# Uses vmaf_picture_alloc (not vmaf_preallocate_pictures) and 64x64 frames so
# TSan shadow overhead is trivial. Eligible for all sanitizer runs: ASan, UBSan, TSan.
test_thread_safety_batch = executable('test_thread_safety_batch',
    ['test.c', 'test_thread_safety_batch.c'],
    include_directories : [libvmaf_inc, test_inc],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [math_lib, pthread_dependency, thread_lib],
)
test('test_thread_safety_batch', test_thread_safety_batch, suite : ['fast'])

# ADR-0960 (round-25 audit A.2 + A.3) — picture_pool.c error-path fixes:
# missing pthread_cond_signal on return_to_pool and dangling pic->priv after
# free(priv). Compiles against the internal picture_pool.c so it can call the
# vmaf_picture_pool_{init,fetch,close} symbols directly without going through
# the high-level vmaf_preallocate_pictures API.
test_picture_pool_error_paths = executable('test_picture_pool_error_paths',
    ['test.c', 'test_picture_pool_error_paths.c',
     '../src/picture_pool.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : test_picture_lib,
    dependencies: [stdatomic_dependency, pthread_dependency, thread_lib],
)
test('test_picture_pool_error_paths', test_picture_pool_error_paths, suite : ['fast'])

test_picture_pool_cpp_error_paths = executable('test_picture_pool_cpp_error_paths',
    ['test.c', 'test_picture_pool_error_paths.c',
     '../src/picture_pool.cpp', '../src/picture.c', '../src/mem.cpp', '../src/ref.cpp'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    dependencies: [stdatomic_dependency, pthread_dependency, thread_lib],
)
test('test_picture_pool_cpp_error_paths', test_picture_pool_cpp_error_paths, suite : ['fast'])

# ADR-0152 — monotonic-index guard on vmaf_read_pictures (Netflix#910).
test_read_pictures_monotonic = executable('test_read_pictures_monotonic',
    ['test.c', 'test_read_pictures_monotonic.c'],
    include_directories : [libvmaf_inc, test_inc],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, thread_lib],
)
test('test_read_pictures_monotonic', test_read_pictures_monotonic, suite : ['fast'])

# ADR-1431 / Netflix#1420 — vmaf_read_pictures() releases the pictures it is
# given on every return, so a rejected or failed call leaves the picture pool
# whole (the CLI used to hang in vmaf_close() after an out-of-memory).
test_read_pictures_failure_ownership = executable('test_read_pictures_failure_ownership',
    ['test.c', 'test_read_pictures_failure_ownership.c'],
    include_directories : [libvmaf_inc, test_inc],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, thread_lib],
)
test('test_read_pictures_failure_ownership', test_read_pictures_failure_ownership,
    suite : ['fast'])

# ADR-1478 / Netflix a2b59b77 — motion_five_frame_window on the CPU extractors
# motion and motion_v2: the SAD against frame n-2 (prev_prev_ref), motion2 and
# motion3 over the five-frame window, serial and on worker threads, and a
# picture pool of four pictures.
test_motion_five_frame_window = executable('test_motion_five_frame_window',
    ['test.c', 'test_motion_five_frame_window.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    c_args : [vmaf_cflags_common, '-DJSON_MODEL_PATH="'+join_paths(meson.project_source_root(), '../model/')+'"'],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, thread_lib],
)
test('test_motion_five_frame_window', test_motion_five_frame_window,
    suite : ['fast'])

# Regression: validate_pic_params bpc check used && instead of || — ref/dist
# bpc mismatch was silently accepted on frame 0 (Fable finding
# libvmaf-bpc-validation-and-or).
test_validate_pic_params_bpc = executable('test_validate_pic_params_bpc',
    ['test.c', 'test_validate_pic_params_bpc.c'],
    include_directories : [libvmaf_inc, test_inc],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, thread_lib],
)
test('test_validate_pic_params_bpc', test_validate_pic_params_bpc, suite : ['fast'])

# ADR-0154 — score_pooled returns -EAGAIN instead of -EINVAL for
# not-yet-written features (Netflix#755).
test_score_pooled_eagain = executable('test_score_pooled_eagain',
    ['test.c', 'test_score_pooled_eagain.c'],
    include_directories : [libvmaf_inc, test_inc],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [math_lib, pthread_dependency, thread_lib],
)
test('test_score_pooled_eagain', test_score_pooled_eagain, suite : ['fast'])

# ADR-1182 — Windows UTF-8 path contract (Netflix#1568).
test_path_utf8 = executable('test_path_utf8',
    ['test.c', 'test_path_utf8.c', '../src/compat/path_utf8.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    c_args : vmaf_cflags_common,
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [math_lib, pthread_dependency, thread_lib],
)
test('test_path_utf8', test_path_utf8, suite : ['fast'])

# ADR-1118 — Pelorus perceptual spatial-pooling weighting. Golden-gate
# isolation proof: pooling is bit-exact without side-data (and when only one of
# the two opt-in conditions holds), and shifts in the expected direction once
# weighting is enabled AND a synthetic banding blob is registered. Links the
# full libvmaf and builds blobs via the vendored interop ABI; no model, no YUV.
test_perceptual_weight = executable('test_perceptual_weight',
    ['test.c', 'test_perceptual_weight.c'],
    include_directories : [libvmaf_inc, test_inc],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [math_lib, pthread_dependency, thread_lib],
)
test('test_perceptual_weight', test_perceptual_weight, suite : ['fast'])

# ADR-0153 — float_ms_ssim init rejects <176x176 (Netflix#1414).
test_float_ms_ssim_min_dim = executable('test_float_ms_ssim_min_dim',
    ['test.c', 'test_float_ms_ssim_min_dim.c', '../src/mem.cpp', '../src/picture.c',
     '../src/ref.cpp', '../src/dict.cpp',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp', dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib,
                    predict_test_dependencies, dnn_deps],
    objects : [
      common_cuda_objects,
      platform_specific_cpu_objects,
      libvmaf_feature_static_lib.extract_all_objects(recursive: true),
      log_cpp23_test_objects,
      libvmaf_cpu_static_lib.extract_all_objects(recursive: true),
      libsvm_static_lib.extract_all_objects(recursive: true),
    ] + wave8_opt_only_objects
)
test('test_float_ms_ssim_min_dim', test_float_ms_ssim_min_dim, suite : ['fast'])

# integer_adm init() must reject min(w,h) <= 16 with -EINVAL (SIGSEGV guard):
# the 4-level DWT2 pyramid requires at least 17 pixels in each dimension.
test_integer_adm_min_dim = executable('test_integer_adm_min_dim',
    ['test.c', 'test_integer_adm_min_dim.c', '../src/mem.cpp', '../src/picture.c',
     '../src/ref.cpp', '../src/dict.cpp',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp', dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib,
                    predict_test_dependencies, dnn_deps],
    objects : [
      common_cuda_objects,
      platform_specific_cpu_objects,
      libvmaf_feature_static_lib.extract_all_objects(recursive: true),
      log_cpp23_test_objects,
      libvmaf_cpu_static_lib.extract_all_objects(recursive: true),
      libsvm_static_lib.extract_all_objects(recursive: true),
    ] + wave8_opt_only_objects
)
test('test_integer_adm_min_dim', test_integer_adm_min_dim, suite : ['fast'])

# Research-0094 regression: motion init() must reject frames < 3x3 with -EINVAL
# instead of reading out-of-bounds via the reflect-101 mirror-padding formula.
test_motion_min_dim = executable('test_motion_min_dim',
    ['test.c', 'test_motion_min_dim.c', '../src/mem.cpp', '../src/picture.c',
     '../src/ref.cpp', '../src/dict.cpp',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp', dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib,
                    predict_test_dependencies, dnn_deps],
    objects : [
      common_cuda_objects,
      platform_specific_cpu_objects,
      libvmaf_feature_static_lib.extract_all_objects(recursive: true),
      log_cpp23_test_objects,
      libvmaf_cpu_static_lib.extract_all_objects(recursive: true),
      libsvm_static_lib.extract_all_objects(recursive: true),
    ] + wave8_opt_only_objects
)
test('test_motion_min_dim', test_motion_min_dim, suite : ['fast'])

# Drives float_motion through the PUBLIC API so the AVX2/AVX-512 convolution is
# actually reached. test_motion_min_dim only calls init(), and
# test_convolution_edge_small calls the scalar kernels directly, so neither can
# observe the dispatched SIMD path where the out-of-bounds writes lived.
test_motion_convolution_oob = executable('test_motion_convolution_oob',
    ['test.c', 'test_motion_convolution_oob.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, math_lib],
)
test('test_motion_convolution_oob', test_motion_convolution_oob, suite : ['fast'])

# Netflix/vmaf#1582 + #1581 regression: the float convolution border helpers
# must stay inside the plane for every size, including planes smaller than the
# filter (reachable through float_motion's motion_add_uv chroma planes and the
# float_vif multi-scale ladder), and must remain bit-identical above it.
test_convolution_edge_small = executable('test_convolution_edge_small',
    ['test.c', 'test_convolution_edge_small.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies : [math_lib, pthread_dependency, sycl_dependency],
)
test('test_convolution_edge_small', test_convolution_edge_small, suite : ['fast'])

# Exercise the configured convolution objects directly, including shared builds.
# No float-feature dependency: float motion also consumes these common kernels.
if is_asm_enabled and host_machine.cpu_family().startswith('x86')
    convolution_horizontal_objects = [
        x86_avx2_static_lib.extract_objects(feature_src_dir + 'common/convolution_avx.c'),
        libvmaf_cpu_static_lib.extract_all_objects(recursive: true),
    ]
    if is_avx512_enabled
        convolution_horizontal_objects += x86_avx512_static_lib.extract_objects(
            feature_src_dir + 'common/convolution_avx512.c')
    endif
    test_convolution_horizontal = executable('test_convolution_horizontal',
        ['test.c', 'test_convolution_horizontal.c', '../src/feature/common/alignment.c'],
        include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
        c_args : vmaf_cflags_common,
        dependencies : [math_lib, thread_lib],
        objects : convolution_horizontal_objects,
    )
    test('test_convolution_horizontal', test_convolution_horizontal, suite : ['fast'])
endif

# Netflix/vmaf#1551 (retracting Netflix/vmaf#1422): the MSVC __builtin_clz shim
# must derive the leading-zero count from BSR, not from the ungated LZCNT
# instruction. The arithmetic is portable and unit-tested here; the "must not
# be reimplemented with __lzcnt" half is the shell guard registered below.
test_compat_clz = executable('test_compat_clz',
    ['test.c', 'test_compat_clz.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    dependencies : [pthread_dependency],
)
test('test_compat_clz', test_compat_clz, suite : ['fast'])

# Static source-content check, so one platform is enough — and on Windows it
# cannot run at all. meson invokes the script through its shebang interpreter,
# and on the MinGW64 runner `bash` resolves to Windows' own WSL bash.exe, which
# has no installed distribution: the leg failed with "Windows Subsystem for
# Linux has no installed distributions" before the script ever executed. Linux
# and macOS both run it in the fast suite, and the lint lane runs it too, so
# skipping the Windows hosts loses no coverage.
if host_machine.system() != 'windows'
    check_msvc_clz_shim = find_program(meson.project_source_root() / '..' / 'scripts' / 'ci' / 'check-msvc-clz-shim.sh')
    test('check_msvc_clz_shim', check_msvc_clz_shim,
         args : [meson.project_source_root() / '..'],
         suite : ['fast'])
endif

# float_vif min-dimension guard — regression test for double-free / OOB-read
# when min(w,h) <= 6 triggers the 17-tap Gaussian filter at scale 0.
# Fixed by inserting a w < 7 || h < 7 guard in float_vif init() before any
# scratch-buffer allocation (PR fix/float-vif-min-dimension-guard).
test_float_vif_min_dim = executable('test_float_vif_min_dim',
    ['test.c', 'test_float_vif_min_dim.c', '../src/mem.cpp', '../src/picture.c',
     '../src/ref.cpp', '../src/dict.cpp',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp', dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib,
                    predict_test_dependencies, dnn_deps],
    objects : [
      common_cuda_objects,
      platform_specific_cpu_objects,
      libvmaf_feature_static_lib.extract_all_objects(recursive: true),
      log_cpp23_test_objects,
      libvmaf_cpu_static_lib.extract_all_objects(recursive: true),
      libsvm_static_lib.extract_all_objects(recursive: true),
    ] + wave8_opt_only_objects
)
test('test_float_vif_min_dim', test_float_vif_min_dim, suite : ['fast'])

# Coverage round 4 — float extractor CPU-path gap-fill.
# All float_* extractors previously had tests only in GPU-gated CUDA/SYCL
# parity suites, meaning CPU coverage was 0% in non-GPU CI. These tests
# add end-to-end init→extract→close coverage on the CPU path (fast suite).
# Each test uses the same link recipe as test_motion_min_dim / test_float_vif_min_dim.
_float_fex_test_sources_common = [
    '../src/mem.cpp', '../src/picture.c', '../src/ref.cpp',
    '../src/dict.cpp',
    '../src/metadata_handler.cpp', '../src/thread_locale.cpp',
    '../src/gpu_picture_pool.cpp',
]

# float_ssim and float_ms_ssim are compiled unconditionally (no enable_float gate
# in src/meson.build lines 1545-1546), so their tests build without the flag.
test_float_ssim_coverage = executable('test_float_ssim_coverage',
    ['test.c', 'test_float_ssim_coverage.c'] + _float_fex_test_sources_common + dnn_sources,
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib,
                    predict_test_dependencies, dnn_deps],
    objects : [
        common_cuda_objects,
        platform_specific_cpu_objects,
        libvmaf_feature_static_lib.extract_all_objects(recursive: true),
        log_cpp23_test_objects,
        libvmaf_cpu_static_lib.extract_all_objects(recursive: true),
        libsvm_static_lib.extract_all_objects(recursive: true),
    ] + wave8_opt_only_objects
)
test('test_float_ssim_coverage', test_float_ssim_coverage, suite : ['fast'])

test_float_ms_ssim_coverage = executable('test_float_ms_ssim_coverage',
    ['test.c', 'test_float_ms_ssim_coverage.c'] + _float_fex_test_sources_common + dnn_sources,
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib,
                    predict_test_dependencies, dnn_deps],
    objects : [
        common_cuda_objects,
        platform_specific_cpu_objects,
        libvmaf_feature_static_lib.extract_all_objects(recursive: true),
        log_cpp23_test_objects,
        libvmaf_cpu_static_lib.extract_all_objects(recursive: true),
        libsvm_static_lib.extract_all_objects(recursive: true),
    ] + wave8_opt_only_objects
)
test('test_float_ms_ssim_coverage', test_float_ms_ssim_coverage, suite : ['fast'])

if get_option('enable_float')
# float_psnr, float_moment, float_vif, float_adm, float_motion are all compiled
# only when enable_float=true (src/meson.build:1583). Gate their tests identically.
test_float_psnr_coverage = executable('test_float_psnr_coverage',
    ['test.c', 'test_float_psnr_coverage.c'] + _float_fex_test_sources_common + dnn_sources,
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib,
                    predict_test_dependencies, dnn_deps],
    objects : [
        common_cuda_objects,
        platform_specific_cpu_objects,
        libvmaf_feature_static_lib.extract_all_objects(recursive: true),
        log_cpp23_test_objects,
        libvmaf_cpu_static_lib.extract_all_objects(recursive: true),
        libsvm_static_lib.extract_all_objects(recursive: true),
    ] + wave8_opt_only_objects
)
test('test_float_psnr_coverage', test_float_psnr_coverage, suite : ['fast'])

test_float_moment_coverage = executable('test_float_moment_coverage',
    ['test.c', 'test_float_moment_coverage.c'] + _float_fex_test_sources_common + dnn_sources,
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib,
                    predict_test_dependencies, dnn_deps],
    objects : [
        common_cuda_objects,
        platform_specific_cpu_objects,
        libvmaf_feature_static_lib.extract_all_objects(recursive: true),
        log_cpp23_test_objects,
        libvmaf_cpu_static_lib.extract_all_objects(recursive: true),
        libsvm_static_lib.extract_all_objects(recursive: true),
    ] + wave8_opt_only_objects
)
test('test_float_moment_coverage', test_float_moment_coverage, suite : ['fast'])

test_float_vif_coverage = executable('test_float_vif_coverage',
    ['test.c', 'test_float_vif_coverage.c'] + _float_fex_test_sources_common + dnn_sources,
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib,
                    predict_test_dependencies, dnn_deps],
    objects : [
        common_cuda_objects,
        platform_specific_cpu_objects,
        libvmaf_feature_static_lib.extract_all_objects(recursive: true),
        log_cpp23_test_objects,
        libvmaf_cpu_static_lib.extract_all_objects(recursive: true),
        libsvm_static_lib.extract_all_objects(recursive: true),
    ] + wave8_opt_only_objects
)
test('test_float_vif_coverage', test_float_vif_coverage, suite : ['fast'])

# Exercise internal VIF symbols with the same float gate/object recipe as coverage,
# including shared-only builds where these helpers are not public exports.
test_vif_lifecycle = executable('test_vif_lifecycle',
    ['test.c', 'test_vif_lifecycle.c'] + _float_fex_test_sources_common + dnn_sources,
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib,
                    predict_test_dependencies, dnn_deps],
    objects : [
        common_cuda_objects,
        platform_specific_cpu_objects,
        libvmaf_feature_static_lib.extract_all_objects(recursive: true),
        log_cpp23_test_objects,
        libvmaf_cpu_static_lib.extract_all_objects(recursive: true),
        libsvm_static_lib.extract_all_objects(recursive: true),
    ] + wave8_opt_only_objects
)
test('test_vif_lifecycle', test_vif_lifecycle, suite : ['fast'])

# ADR-1412 — the arithmetic float_vif_cuda's and float_vif_hip's kernels run
# (src/feature/float_vif_gpu_common.h, through the CUDA spelling header
# src/feature/cuda/float_vif/float_vif_device.h), compiled for the host and
# compared bit for bit with vif_statistic_s() and compute_vif(). Device-free,
# so it runs in the fast suite of every build, with or without CUDA. The
# reference it pins is the contraction-free one: the header's host operators
# round like the device only without FMA contraction (hence the flag), and
# vif_tools.c is contraction-free by construction on x86-64 only (its baseline
# has no FMA), so the test is registered there.
if host_machine.cpu_family() == 'x86_64'
test_float_vif_device_math = executable('test_float_vif_device_math',
    ['test.c', 'test_float_vif_device_math.c'] + _float_fex_test_sources_common + dnn_sources,
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines + vmaf_strict_fp_args,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib,
                    predict_test_dependencies, dnn_deps],
    objects : [
        common_cuda_objects,
        platform_specific_cpu_objects,
        libvmaf_feature_static_lib.extract_all_objects(recursive: true),
        log_cpp23_test_objects,
        libvmaf_cpu_static_lib.extract_all_objects(recursive: true),
        libsvm_static_lib.extract_all_objects(recursive: true),
    ] + wave8_opt_only_objects
)
test('test_float_vif_device_math', test_float_vif_device_math, suite : ['fast'])
endif

test_float_adm_coverage = executable('test_float_adm_coverage',
    ['test.c', 'test_float_adm_coverage.c'] + _float_fex_test_sources_common + dnn_sources,
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib,
                    predict_test_dependencies, dnn_deps],
    objects : [
        common_cuda_objects,
        platform_specific_cpu_objects,
        libvmaf_feature_static_lib.extract_all_objects(recursive: true),
        log_cpp23_test_objects,
        libvmaf_cpu_static_lib.extract_all_objects(recursive: true),
        libsvm_static_lib.extract_all_objects(recursive: true),
    ] + wave8_opt_only_objects
)
test('test_float_adm_coverage', test_float_adm_coverage, suite : ['fast'])

# ADR-1420 — the arithmetic float_adm_cuda's and float_adm_hip's kernels run
# (src/feature/float_adm_gpu_common.h, through the CUDA spelling header
# src/feature/cuda/float_adm/float_adm_device.h), compiled for the host and
# compared bit for bit with adm_decouple_s(), adm_csf_s(),
# adm_csf_den_scale_s() and adm_cm_s(), plus the decouple against the IEEE
# quotient on inputs where a reciprocal estimate differs (ADR-1442).
# Device-free, so it runs in the fast suite of every
# build, with or without CUDA. The header's host operators round like the
# device only without FMA contraction (hence the flag), and adm_tools.c is
# contraction-free by construction on x86-64 only (its baseline has no FMA),
# so the test is registered there.
if host_machine.cpu_family() == 'x86_64'
test_float_adm_device_math = executable('test_float_adm_device_math',
    ['test.c', 'test_float_adm_device_math.c'] + _float_fex_test_sources_common + dnn_sources,
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines + vmaf_strict_fp_args,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib,
                    predict_test_dependencies, dnn_deps],
    objects : [
        common_cuda_objects,
        platform_specific_cpu_objects,
        libvmaf_feature_static_lib.extract_all_objects(recursive: true),
        log_cpp23_test_objects,
        libvmaf_cpu_static_lib.extract_all_objects(recursive: true),
        libsvm_static_lib.extract_all_objects(recursive: true),
    ] + wave8_opt_only_objects
)
test('test_float_adm_device_math', test_float_adm_device_math, suite : ['fast'])
endif

test_float_motion_coverage = executable('test_float_motion_coverage',
    ['test.c', 'test_float_motion_coverage.c'] + _float_fex_test_sources_common + dnn_sources,
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib,
                    predict_test_dependencies, dnn_deps],
    objects : [
        common_cuda_objects,
        platform_specific_cpu_objects,
        libvmaf_feature_static_lib.extract_all_objects(recursive: true),
        log_cpp23_test_objects,
        libvmaf_cpu_static_lib.extract_all_objects(recursive: true),
        libsvm_static_lib.extract_all_objects(recursive: true),
    ] + wave8_opt_only_objects
)
test('test_float_motion_coverage', test_float_motion_coverage, suite : ['fast'])
endif

# GPU dispatch-runtime coverage (host-only). Targets the shared
# `gpu_dispatch_env.cpp` + `gpu_dispatch_parse.h` helpers and the
# host-buildable subset of the per-backend dispatch_strategy TUs
# (CUDA + HIP — both compile without their respective SDKs because
# they only depend on stdlib + pthread + opaque forward-declared
# context types). No GPU driver required at runtime — purely
# exercises pure-function dispatch logic.
#
# Coverage gap discovered by the 2026-05-31 runtime-files audit:
# the dispatch_strategy selectors and shared env-snapshot helpers
# had no direct unit test before this file.
test_gpu_dispatch_runtime = executable('test_gpu_dispatch_runtime',
    ['test.c', 'test_gpu_dispatch_runtime.c',
     '../src/cuda/dispatch_strategy.c',
     '../src/hip/dispatch_strategy.c',
     '../src/sycl/dispatch_strategy.cpp'],
    include_directories : [libvmaf_inc, test_inc,
                           include_directories('../src/'),
                           include_directories('../src/feature/')],
    dependencies: [pthread_dependency, stdatomic_dependency],
    # ADR-0858 Wave: gpu_dispatch_env converted .c → .cpp; link via the
    # isolated C++23 static lib instead of direct-compiling the source.
    # (The orphaned legacy .c was removed once the .cpp became canonical.)
    objects : [gpu_dispatch_env_cpp23_lib.extract_all_objects(recursive: true),
               log_cpp23_test_objects],
)
test('test_gpu_dispatch_runtime', test_gpu_dispatch_runtime, suite : ['fast'])

# R2-9 regression: a transient allocation failure while snapshotting a *set*
# GPU-dispatch env variable must not permanently cache the slot as "unset".
# The legacy gpu_dispatch_env.c committed the populated marker before the
# value strdup, so a strdup OOM poisoned the slot for the process lifetime;
# the shipped gpu_dispatch_env.cpp publishes via a ready flag stored LAST.
# The C++ test replaces global operator new to force the value-string
# allocation to throw, then asserts the next get recovers the real value.
# Links the same isolated C++23 static lib as test_gpu_dispatch_runtime.
test_gpu_dispatch_env_oom = executable('test_gpu_dispatch_env_oom',
    ['test.c', 'test_gpu_dispatch_env_oom.cpp'],
    include_directories : [libvmaf_inc, test_inc,
                           include_directories('../src/')],
    # No -Wno-write-strings hack: this C++ TU holds its `mu_assert` messages in
    # `static char[]` buffers (see the .cpp) instead of returning string
    # literals as `char *`, so there is nothing to suppress — and the old flag
    # was both unnecessary on GCC/Clang and broke the MSVC build (cl rejected
    # `/Wno-write-strings` with D8021, and the underlying literal->char*
    # conversion was a hard C2440 there anyway).
    dependencies: [pthread_dependency, stdatomic_dependency],
    objects : [gpu_dispatch_env_cpp23_lib.extract_all_objects(recursive: true)],
)
test('test_gpu_dispatch_env_oom', test_gpu_dispatch_env_oom, suite : ['fast'])

# Public-API smoke: vmaf_version() returns non-empty, printable, whitespace-free
# string. Closes the orphan-source gap surfaced by the 2026-05-29 test-meson
# audit — the source already existed at core/test/test_version.c but was never
# wired into the build. Links against libvmaf only.
test_version = executable('test_version',
    ['test.c', 'test_version.c'],
    include_directories : [libvmaf_inc, test_inc],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, gpu_all_deps],
)
test('test_version', test_version, suite : ['fast'])

# integer_vif vif_skip_scale0 option (CPU path). Source already existed at
# core/test/test_vif_skip_scale0.c but was never registered. Public-API only,
# so it links against libvmaf without pulling in feature internals.
test_vif_skip_scale0 = executable('test_vif_skip_scale0',
    ['test.c', 'test_vif_skip_scale0.c'],
    include_directories : [libvmaf_inc, test_inc],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [math_lib, pthread_dependency, thread_lib, gpu_all_deps],
)
test('test_vif_skip_scale0', test_vif_skip_scale0, suite : ['fast'])

if get_option('enable_sycl')
test_sycl = executable('test_sycl',
    ['test.c', 'test_sycl.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, sycl_dependency],
    c_args: ['-DHAVE_SYCL=1']
)

test_sycl_pic_preallocation = executable('test_sycl_pic_preallocation',
    ['test.c', 'test_sycl_pic_preallocation.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, sycl_dependency],
    c_args: ['-DHAVE_SYCL=1']
)

test('test_sycl', test_sycl,
    suite : ['fast', 'gpu', 'sycl'],
    is_parallel : false)
test('test_sycl_pic_preallocation', test_sycl_pic_preallocation,
    suite : ['fast', 'gpu', 'sycl'],
    is_parallel : false)

# ADR-1364: the device images must be registered in a program that links
# libvmaf. Needs no GPU, so it runs on the device-less Windows MSVC+SYCL lane.
test_sycl_kernel_registration = executable('test_sycl_kernel_registration',
    ['test.c', 'test_sycl_kernel_registration.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, sycl_dependency],
    c_args: ['-DHAVE_SYCL=1']
)
test('test_sycl_kernel_registration', test_sycl_kernel_registration,
    suite : ['fast', 'sycl'])

# ADR-1468: every SYCL translation unit of this build compiles ahead of time
# for every target of the default sycl_icpx_aot_targets list, whatever list
# this build was configured with (a lane build compiles for one device or for
# none, and so never saw a kernel that only an Xe2 target rejects). Needs
# ocloc and no device; skips (77) without ocloc. It runs ocloc for 19 targets
# per unit, minutes in all, so it is not in `fast`:
#     meson test -C <build> --suite sycl-aot
# VMAF_SYCL_AOT_JOBS sets its parallel compiles (default 4).
if not is_sycl_acpp
    test('test_sycl_aot_default_targets',
        import('python').find_installation(),
        args : [files('test_sycl_aot_default_targets.py'),
                '--build-dir', meson.project_build_root()],
        suite : ['slow', 'sycl-aot'],
        is_parallel : false,
        timeout : 3600,
    )
endif

# ADR-1367: device fp32 arithmetic of the SYCL feature line. The probe TU is
# compiled with the extractors' own arguments (sycl_toolchain_args +
# sycl_feature_tail_args) and linked through sycl_dependency, so both device
# image paths are covered: AOT images built at compile time and the SPIR-V JIT
# image built at the link. link.exe never wraps device code, so under the MSVC
# device link (ADR-1364) the probe gets its own explicit device link with
# libvmaf's arguments. AdaptiveCpp's line has no precision flags.
if not is_sycl_acpp
    sycl_fp_arith_probe = custom_target('sycl_fp_arith_probe',
        output : 'sycl_fp_arith_probe.o',
        input : 'test_sycl_fp_arith_probe.cpp',
        # ADR-1488: the probe also runs sqrt_rn() of this header.
        depend_files : files('../src/feature/sycl/sycl_exact_fp.h'),
        command : [icpx] + vmaf_cflags_common + ['-c'] + sycl_toolchain_args
                  + sycl_feature_tail_args + ['@INPUT@', '-o', '@OUTPUT@'],
    )
    sycl_fp_arith_probe_objects = [sycl_fp_arith_probe]
    if sycl_msvc_device_link
        sycl_fp_arith_probe_objects += custom_target('sycl_fp_arith_probe_device_link',
            input : sycl_fp_arith_probe,
            output : 'sycl_fp_arith_probe_device_link.obj',
            command : [icpx] + sycl_device_link_args + sycl_strict_fp_args
                      + ['@INPUT@', '-o', '@OUTPUT@'],
        )
    endif
    test_sycl_fp_arith_contract = executable('test_sycl_fp_arith_contract',
        ['test.c', 'test_sycl_fp_arith_contract.c'] + sycl_fp_arith_probe_objects,
        include_directories : [test_inc],
        dependencies : [math_lib, sycl_dependency],
        # The host references are single fp64 operations rounded once; the
        # project-wide strict policy (ADR-1461) already turns icx's fast model
        # (reciprocal division) off.
        link_language : 'cpp',
    )
    test('test_sycl_fp_arith_contract', test_sycl_fp_arith_contract,
        suite : ['fast', 'gpu', 'sycl'],
        is_parallel : false)
endif

# ADR-1422: the arithmetic float_vif_sycl's kernels run
# (src/feature/sycl/sycl_float_vif_math.h) against vif_statistic_s(), bit for
# bit, on the host and in a kernel on the default GPU. The probe TU is compiled
# with the extractors' own arguments, like the ADR-1367 probe above, and the C
# side links the CPU reference from the static libraries: vif_statistic_s() is
# not exported. The reference is contraction-free on x86-64 only (its baseline
# has no FMA), so the test is registered there.
if not is_sycl_acpp and host_machine.cpu_family() == 'x86_64'
    sycl_float_vif_math_probe = custom_target('sycl_float_vif_math_probe',
        output : 'sycl_float_vif_math_probe.o',
        input : 'test_sycl_float_vif_math_probe.cpp',
        depend_files : files('../src/feature/sycl/sycl_exact_fp.h',
                             '../src/feature/sycl/sycl_float_vif_math.h',
                             '../src/feature/sycl/sycl_compat.h'),
        command : [icpx] + vmaf_cflags_common + ['-c'] + sycl_toolchain_args
                  + sycl_feature_tail_args + ['@INPUT@', '-o', '@OUTPUT@'],
    )
    sycl_float_vif_math_probe_objects = [sycl_float_vif_math_probe]
    if sycl_msvc_device_link
        sycl_float_vif_math_probe_objects += custom_target('sycl_float_vif_math_probe_device_link',
            input : sycl_float_vif_math_probe,
            output : 'sycl_float_vif_math_probe_device_link.obj',
            command : [icpx] + sycl_device_link_args + sycl_strict_fp_args
                      + ['@INPUT@', '-o', '@OUTPUT@'],
        )
    endif
    test_sycl_float_vif_math = executable('test_sycl_float_vif_math',
        ['test.c', 'test_sycl_float_vif_math.c'] + sycl_float_vif_math_probe_objects
        + _float_fex_test_sources_common + dnn_sources,
        include_directories : [libvmaf_inc, test_inc, include_directories('../src/'), dnn_inc],
        c_args : vmaf_cflags_common + dnn_defines + vmaf_strict_fp_args,
        dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib,
                        predict_test_dependencies, dnn_deps, sycl_dependency],
        objects : [
            common_cuda_objects,
            platform_specific_cpu_objects,
            libvmaf_feature_static_lib.extract_all_objects(recursive: true),
            log_cpp23_test_objects,
            libvmaf_cpu_static_lib.extract_all_objects(recursive: true),
            libsvm_static_lib.extract_all_objects(recursive: true),
        ] + wave8_opt_only_objects,
        link_language : 'cpp',
    )
    test('test_sycl_float_vif_math', test_sycl_float_vif_math,
        suite : ['fast', 'gpu', 'sycl'],
        is_parallel : false)
endif

# ADR-1432: the gain terms vif_sycl's kernels compute
# (src/feature/sycl/sycl_integer_vif_math.h) against the reference's fp64
# expressions, on the host and in a kernel on the default GPU. The probe TU
# is compiled with the extractors' own arguments, like the probes above. The
# reference lines are fp64 arithmetic the test compiles itself, so it needs
# the strict FP flags and no library object.
if not is_sycl_acpp
    sycl_integer_vif_math_probe = custom_target('sycl_integer_vif_math_probe',
        output : 'sycl_integer_vif_math_probe.o',
        input : 'test_sycl_integer_vif_math_probe.cpp',
        depend_files : files('../src/feature/sycl/sycl_soft_double.h',
                             '../src/feature/sycl/sycl_integer_vif_math.h',
                             '../src/feature/sycl/sycl_compat.h'),
        command : [icpx] + vmaf_cflags_common + ['-c'] + sycl_toolchain_args
                  + sycl_feature_tail_args + ['@INPUT@', '-o', '@OUTPUT@'],
    )
    sycl_integer_vif_math_probe_objects = [sycl_integer_vif_math_probe]
    if sycl_msvc_device_link
        sycl_integer_vif_math_probe_objects += custom_target(
            'sycl_integer_vif_math_probe_device_link',
            input : sycl_integer_vif_math_probe,
            output : 'sycl_integer_vif_math_probe_device_link.obj',
            command : [icpx] + sycl_device_link_args + sycl_strict_fp_args
                      + ['@INPUT@', '-o', '@OUTPUT@'],
        )
    endif
    test_sycl_integer_vif_math = executable('test_sycl_integer_vif_math',
        ['test.c', 'test_sycl_integer_vif_math.c'] + sycl_integer_vif_math_probe_objects,
        include_directories : [test_inc],
        dependencies : [math_lib, sycl_dependency],
        c_args : vmaf_strict_fp_args,
        link_language : 'cpp',
    )
    test('test_sycl_integer_vif_math', test_sycl_integer_vif_math,
        suite : ['fast', 'gpu', 'sycl'],
        is_parallel : false)
endif

# ADR-1436: the arithmetic ciede_sycl's kernel runs -- the fp32 pair functions
# of src/feature/ff_math.h against the host's extended-precision math
# library, and the ciede2000 pixel of src/feature/ciede_ff_math.h against the
# same statements in fp64 (cuda/integer_ciede/ciede_device.h compiled for the
# host), both through the SYCL primitives of src/feature/sycl/sycl_ff_math.h
# and sycl_ciede_math.h -- on the host and in kernels on the default GPU. The
# probe TU is compiled with the extractors' own arguments, like the probes
# above. The fp64 statements are compiled by the test itself, so it needs the
# strict FP flags and no library object.
if not is_sycl_acpp
    sycl_ciede_math_probe = custom_target('sycl_ciede_math_probe',
        output : 'sycl_ciede_math_probe.o',
        input : 'test_sycl_ciede_math_probe.cpp',
        depend_files : files('../src/feature/sycl/sycl_exact_fp.h',
                             '../src/feature/sycl/sycl_ff_math.h',
                             '../src/feature/sycl/sycl_ciede_math.h',
                             '../src/feature/sycl/sycl_compat.h',
                             '../src/feature/ff_math.h',
                             '../src/feature/ciede_ff_math.h',
                             'sycl_ciede_math_probe.h'),
        command : [icpx] + vmaf_cflags_common + ['-c'] + sycl_toolchain_args
                  + sycl_feature_tail_args + ['@INPUT@', '-o', '@OUTPUT@'],
    )
    sycl_ciede_math_probe_objects = [sycl_ciede_math_probe]
    if sycl_msvc_device_link
        sycl_ciede_math_probe_objects += custom_target('sycl_ciede_math_probe_device_link',
            input : sycl_ciede_math_probe,
            output : 'sycl_ciede_math_probe_device_link.obj',
            command : [icpx] + sycl_device_link_args + sycl_strict_fp_args
                      + ['@INPUT@', '-o', '@OUTPUT@'],
        )
    endif
    test_sycl_ciede_math = executable('test_sycl_ciede_math',
        ['test.c', 'test_sycl_ciede_math.c'] + sycl_ciede_math_probe_objects,
        include_directories : [test_inc, include_directories('../src/')],
        dependencies : [math_lib, sycl_dependency],
        c_args : vmaf_strict_fp_args,
        link_language : 'cpp',
    )
    test('test_sycl_ciede_math', test_sycl_ciede_math,
        suite : ['fast', 'gpu', 'sycl'],
        is_parallel : false)
endif

# ADR-1443: the fp64 operations integer_ssim_sycl's kernel runs in 64-bit
# integers (src/feature/sycl/sycl_soft_signed.h) and the per-pixel term they
# form (src/feature/sycl/sycl_integer_ssim_math.h) against the reference's
# fp64 expression, on the host and in a kernel on the default GPU. The probe
# TU is compiled with the extractors' own arguments, like the probes above.
# The reference lines are fp64 arithmetic the test compiles itself, so it
# needs the strict FP flags and no library object.
if not is_sycl_acpp
    sycl_integer_ssim_math_probe = custom_target('sycl_integer_ssim_math_probe',
        output : 'sycl_integer_ssim_math_probe.o',
        input : 'test_sycl_integer_ssim_math_probe.cpp',
        depend_files : files('../src/feature/sycl/sycl_soft_double.h',
                             '../src/feature/sycl/sycl_soft_signed.h',
                             '../src/feature/sycl/sycl_integer_ssim_math.h',
                             '../src/feature/sycl/sycl_compat.h'),
        command : [icpx] + vmaf_cflags_common + ['-c'] + sycl_toolchain_args
                  + sycl_feature_tail_args + ['@INPUT@', '-o', '@OUTPUT@'],
    )
    sycl_integer_ssim_math_probe_objects = [sycl_integer_ssim_math_probe]
    if sycl_msvc_device_link
        sycl_integer_ssim_math_probe_objects += custom_target(
            'sycl_integer_ssim_math_probe_device_link',
            input : sycl_integer_ssim_math_probe,
            output : 'sycl_integer_ssim_math_probe_device_link.obj',
            command : [icpx] + sycl_device_link_args + sycl_strict_fp_args
                      + ['@INPUT@', '-o', '@OUTPUT@'],
        )
    endif
    test_sycl_integer_ssim_math = executable('test_sycl_integer_ssim_math',
        ['test.c', 'test_sycl_integer_ssim_math.c'] + sycl_integer_ssim_math_probe_objects,
        include_directories : [test_inc],
        dependencies : [math_lib, sycl_dependency],
        c_args : vmaf_strict_fp_args,
        link_language : 'cpp',
    )
    test('test_sycl_integer_ssim_math', test_sycl_integer_ssim_math,
        suite : ['fast', 'gpu', 'sycl'],
        is_parallel : false)
endif

# ADR-1434: what float_adm_sycl's kernels run per work-item
# (src/feature/sycl/sycl_float_adm_math.h) against adm_tools.c's own routines,
# bit for bit, on the host and in kernels on the default GPU. The probe TU is
# compiled with the extractors' own arguments, like the probes above, and the
# C side links the CPU reference from the static libraries: adm_decouple_s()
# and its companions are not exported. The reference is contraction-free on
# x86-64 only (its baseline has no FMA), so the test is registered there.
if not is_sycl_acpp and host_machine.cpu_family() == 'x86_64'
    sycl_float_adm_math_probe = custom_target('sycl_float_adm_math_probe',
        output : 'sycl_float_adm_math_probe.o',
        input : 'test_sycl_float_adm_math_probe.cpp',
        depend_files : files('../src/feature/sycl/sycl_exact_fp.h',
                             '../src/feature/sycl/sycl_soft_double.h',
                             '../src/feature/sycl/sycl_float_adm_math.h',
                             '../src/feature/sycl/sycl_compat.h',
                             'sycl_float_adm_math_probe.h'),
        command : [icpx] + vmaf_cflags_common + ['-c'] + sycl_toolchain_args
                  + sycl_feature_tail_args + ['@INPUT@', '-o', '@OUTPUT@'],
    )
    sycl_float_adm_math_probe_objects = [sycl_float_adm_math_probe]
    if sycl_msvc_device_link
        sycl_float_adm_math_probe_objects += custom_target('sycl_float_adm_math_probe_device_link',
            input : sycl_float_adm_math_probe,
            output : 'sycl_float_adm_math_probe_device_link.obj',
            command : [icpx] + sycl_device_link_args + sycl_strict_fp_args
                      + ['@INPUT@', '-o', '@OUTPUT@'],
        )
    endif
    test_sycl_float_adm_math = executable('test_sycl_float_adm_math',
        ['test.c', 'test_sycl_float_adm_math.c'] + sycl_float_adm_math_probe_objects
        + _float_fex_test_sources_common + dnn_sources,
        include_directories : [libvmaf_inc, test_inc, include_directories('../src/'), dnn_inc],
        c_args : vmaf_cflags_common + dnn_defines + vmaf_strict_fp_args,
        dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib,
                        predict_test_dependencies, dnn_deps, sycl_dependency],
        objects : [
            common_cuda_objects,
            platform_specific_cpu_objects,
            libvmaf_feature_static_lib.extract_all_objects(recursive: true),
            log_cpp23_test_objects,
            libvmaf_cpu_static_lib.extract_all_objects(recursive: true),
            libsvm_static_lib.extract_all_objects(recursive: true),
        ] + wave8_opt_only_objects,
        link_language : 'cpp',
    )
    test('test_sycl_float_adm_math', test_sycl_float_adm_math,
        suite : ['fast', 'gpu', 'sycl'],
        is_parallel : false)
endif

# ADR-1446: the ordered sum a SYCL twin forms without an fp64 type
# (src/feature/sycl/sycl_ordered_sum.h on src/feature/ordered_sum.h) against
# the loop `s += x[i]` it stands for, with honest and with wrong plans, on the
# host and with the walk in a kernel on the default GPU. The loop is fp64
# arithmetic the test compiles itself, so it needs the strict FP flags.
if not is_sycl_acpp
    sycl_ordered_sum_probe = custom_target('sycl_ordered_sum_probe',
        output : 'sycl_ordered_sum_probe.o',
        input : 'test_sycl_ordered_sum_probe.cpp',
        depend_files : files('../src/feature/sycl/sycl_soft_double.h',
                             '../src/feature/sycl/sycl_soft_signed.h',
                             '../src/feature/sycl/sycl_compat.h',
                             '../src/feature/sycl/sycl_ordered_sum.h',
                             '../src/feature/ordered_sum.h',
                             'sycl_ordered_sum_probe.h'),
        command : [icpx] + vmaf_cflags_common + ['-c'] + sycl_toolchain_args
                  + sycl_feature_tail_args + ['@INPUT@', '-o', '@OUTPUT@'],
    )
    sycl_ordered_sum_probe_objects = [sycl_ordered_sum_probe]
    if sycl_msvc_device_link
        sycl_ordered_sum_probe_objects += custom_target(
            'sycl_ordered_sum_probe_device_link',
            input : sycl_ordered_sum_probe,
            output : 'sycl_ordered_sum_probe_device_link.obj',
            command : [icpx] + sycl_device_link_args + sycl_strict_fp_args
                      + ['@INPUT@', '-o', '@OUTPUT@'],
        )
    endif
    test_sycl_ordered_sum = executable('test_sycl_ordered_sum',
        ['test.c', 'test_sycl_ordered_sum.c'] + sycl_ordered_sum_probe_objects,
        include_directories : [test_inc],
        dependencies : [math_lib, sycl_dependency],
        c_args : vmaf_strict_fp_args,
        link_language : 'cpp',
    )
    test('test_sycl_ordered_sum', test_sycl_ordered_sum,
        suite : ['fast', 'gpu', 'sycl'],
        is_parallel : false)
endif

# ADR-1446: the six fp64 terms ssimulacra2_sycl's kernels form per sample in
# 64-bit integers (src/feature/sycl/sycl_ssimulacra2_math.h) against the
# lines of ssimulacra2.c::ssim_map() and ::edge_diff_map(), on the host and in
# a kernel on the default GPU.
if not is_sycl_acpp
    sycl_ssimulacra2_math_probe = custom_target('sycl_ssimulacra2_math_probe',
        output : 'sycl_ssimulacra2_math_probe.o',
        input : 'test_sycl_ssimulacra2_math_probe.cpp',
        depend_files : files('../src/feature/sycl/sycl_soft_double.h',
                             '../src/feature/sycl/sycl_soft_signed.h',
                             '../src/feature/sycl/sycl_compat.h',
                             '../src/feature/sycl/sycl_ssimulacra2_math.h'),
        command : [icpx] + vmaf_cflags_common + ['-c'] + sycl_toolchain_args
                  + sycl_feature_tail_args + ['@INPUT@', '-o', '@OUTPUT@'],
    )
    sycl_ssimulacra2_math_probe_objects = [sycl_ssimulacra2_math_probe]
    if sycl_msvc_device_link
        sycl_ssimulacra2_math_probe_objects += custom_target(
            'sycl_ssimulacra2_math_probe_device_link',
            input : sycl_ssimulacra2_math_probe,
            output : 'sycl_ssimulacra2_math_probe_device_link.obj',
            command : [icpx] + sycl_device_link_args + sycl_strict_fp_args
                      + ['@INPUT@', '-o', '@OUTPUT@'],
        )
    endif
    test_sycl_ssimulacra2_math = executable('test_sycl_ssimulacra2_math',
        ['test.c', 'test_sycl_ssimulacra2_math.c'] + sycl_ssimulacra2_math_probe_objects,
        include_directories : [test_inc, include_directories('../src/')],
        dependencies : [math_lib, sycl_dependency],
        c_args : vmaf_strict_fp_args,
        link_language : 'cpp',
    )
    test('test_sycl_ssimulacra2_math', test_sycl_ssimulacra2_math,
        suite : ['fast', 'gpu', 'sycl'],
        is_parallel : false)
endif

# ADR-1451: the SYCL twins declared exact as a group (adm, motion, motion_v2,
# psnr, float_ssim, cambi) return the CPU extractor's bits: four frames at 8
# and at 10 bits, `==` on every output. Exits 77 when no SYCL GPU is visible.
test_sycl_exact_twins = executable('test_sycl_exact_twins',
    ['test.c', 'test_sycl_exact_twins.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, sycl_dependency, math_lib],
    c_args: ['-DHAVE_SYCL=1']
)
test('test_sycl_exact_twins', test_sycl_exact_twins,
    suite : ['fast', 'gpu', 'sycl'],
    is_parallel : false,
    timeout : 120)

# motion_sycl emits the CPU `motion` extractor's whole output set: the SAD
# score of every frame (VMAF_integer_feature_motion_sad_score) next to
# motion2 / motion3 and the debug score. Eleven frames, every output of every
# frame compared with == at 8 and 10 bits, also with debug, with
# motion_force_zero (where no motion score may exist without debug) and with
# a weight and a cap. Fails on a twin without the SAD score.
test_sycl_motion_sad_score = executable('test_sycl_motion_sad_score',
    ['test.c', 'test_sycl_motion_sad_score.c'],
    include_directories : [libvmaf_inc, test_inc],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, sycl_dependency, math_lib],
    c_args: ['-DHAVE_SYCL=1']
)
test('test_sycl_motion_sad_score', test_sycl_motion_sad_score,
    suite : ['fast', 'gpu', 'sycl'],
    is_parallel : false,
    timeout : 60)

# ADR-1491: motion_sycl and motion_v2_sycl compute motion_five_frame_window
# with the CPU's bits. Every output of every frame, `==`, for 11, 1, 2 and 3
# frames at 8 and 10 bits, the option alone and with the other motion options
# (motion_five_frame_twin_parity.h). Fails on a twin without the window.
test_sycl_motion_five_frame_window = executable('test_sycl_motion_five_frame_window',
    ['test.c', 'test_sycl_motion_five_frame_window.c'],
    include_directories : [libvmaf_inc, test_inc],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, sycl_dependency, math_lib],
    c_args: ['-DHAVE_SYCL=1']
)
test('test_sycl_motion_five_frame_window', test_sycl_motion_five_frame_window,
    suite : ['fast', 'gpu', 'sycl'],
    is_parallel : false,
    timeout : 120)

# ADR-1395: SYCL kernels use no scratch memory (private arrays in memory,
# register spills) on Intel GPUs, where the Linux xe driver returns wrong
# values from them. Builds every registered kernel for the default GPU and
# fails on one that uses scratch memory and is not in the ratchet list;
# listed kernels that became clean print a remove-me note. Skips without a GPU.
test_sycl_kernel_scratch = executable('test_sycl_kernel_scratch',
    ['test.c', 'test_sycl_kernel_scratch.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, sycl_dependency],
    c_args: ['-DHAVE_SYCL=1', '-DVMAF_SYCL_SCRATCH_RATCHET="'
             + (meson.current_source_dir() / '..' / 'src' / 'sycl' / 'scratch_ratchet.txt') + '"']
)
test('test_sycl_kernel_scratch', test_sycl_kernel_scratch,
    suite : ['fast', 'gpu', 'sycl'],
    timeout : 300,
    is_parallel : false)

# BUG-040: this compile combination used CUDA's host-picture cleanup early
# return, bypassing the SYCL upload wait on the serial path. The worker path
# can independently drop its counted references before the caller returns.
# The public-API regressions poison each released 4K host picture in both
# modes and prove psnr_sycl saw the pre-release pixels. A SYCL device is
# required at runtime; no CUDA device is.
if get_option('enable_cuda')
test_sycl_cuda_serial_upload_lifetime = executable('test_sycl_cuda_serial_upload_lifetime',
    ['test.c', 'test_sycl_cuda_serial_upload_lifetime.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : libvmaf_public_link,
    dependencies : [math_lib, pthread_dependency, gpu_all_deps],
    c_args : ['-DHAVE_CUDA=1', '-DHAVE_SYCL=1'],
)
test('test_sycl_cuda_serial_upload_lifetime', test_sycl_cuda_serial_upload_lifetime,
    suite : ['slow', 'gpu', 'sycl'],
    is_parallel : false,
    timeout : 120)
endif

# BUG-048 section E — deterministic, device-free init-failure unwind gate.
# GNU ld wrapping sees calls from libvmaf.a only with LTO disabled; this is the
# same constraint as test_registration_partial_copy above.  The dedicated gate
# replaces USM allocation, dictionary construction, and graph registration so
# every partial-init path can be exercised on hosts without a SYCL device.
if host_machine.system() == 'linux' and get_option('default_library') != 'shared' \
        and not get_option('b_lto')
    test_sycl_init_unwind = executable('test_sycl_init_unwind',
        ['test.c', 'test_sycl_init_unwind.cpp'],
        include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
        link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
        dependencies : [pthread_dependency, sycl_dependency, math_lib],
        cpp_args : ['-DHAVE_SYCL=1'],
        link_args : [
            '-Wl,--wrap=vmaf_sycl_malloc_device',
            '-Wl,--wrap=vmaf_sycl_malloc_host',
            '-Wl,--wrap=vmaf_sycl_free',
            '-Wl,--wrap=vmaf_feature_name_dict_from_provided_features',
            '-Wl,--wrap=vmaf_dictionary_free',
            '-Wl,--wrap=vmaf_sycl_shared_frame_init',
            '-Wl,--wrap=vmaf_sycl_shared_chroma_init',
            '-Wl,--wrap=vmaf_sycl_graph_register',
            '-Wl,--wrap=vmaf_sycl_graph_unregister',
        ],
    )
    test('test_sycl_init_unwind', test_sycl_init_unwind, suite : ['fast', 'sycl'])
endif

test_integer_cambi_sycl = executable('test_integer_cambi_sycl',
    ['test.c', 'test_integer_cambi_sycl.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, sycl_dependency],
    c_args: ['-DHAVE_SYCL=1']
)
test('test_integer_cambi_sycl', test_integer_cambi_sycl,
    suite : ['fast', 'gpu', 'sycl'],
    is_parallel : false)

# T3-15(c) / ADR-0219 / ADR-0214 — motion3 CPU vs. SYCL parity gate.
# The motion3 post-process (host-side moving-average over blended motion2)
# is implemented independently in integer_motion.c (CPU) and
# integer_motion_sycl.cpp (SYCL). No cross-backend assertion existed before
# this test; boundary-condition drift would silently pollute CHUG-extracted
# motion3_mean/std columns. SYCL-gated: skips cleanly when no SYCL device
# is visible (mirrors test_sycl.c pattern).
test_sycl_motion3_parity = executable('test_sycl_motion3_parity',
    ['test.c', 'test_sycl_motion3_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, sycl_dependency, math_lib],
    c_args: ['-DHAVE_SYCL=1']
)
test('test_sycl_motion3_parity', test_sycl_motion3_parity,
    suite : ['fast', 'gpu', 'sycl'],
    is_parallel : false)

# ADR-0989 — motion_add_uv SYCL parity test.
# Asserts (1) UV contribution is positive (UV-enabled score > Y-only)
# and (2) the SYCL Y-only and Y+U+V scores match a scalar fixed-point
# oracle within a derived host-double roundoff bound (ADR-1326).
# Skips cleanly when no SYCL device is visible.
test_sycl_motion_add_uv_parity = executable('test_sycl_motion_add_uv_parity',
    ['test.c', 'test_sycl_motion_add_uv_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, sycl_dependency, math_lib],
    c_args: ['-DHAVE_SYCL=1']
)
# Root cause fixed (ADR-1099): the SIGSEGV was caused by missing -fsycl at
# link time for test executables linking libvmaf.a.  Without -fsycl, icpx
# skips clang-offload-wrapper and ProgramManager::getDeviceKernelInfo
# null-dereferences at the first q.submit().  Fix: -fsycl is now embedded
# in sycl_dependency.link_args so every SYCL consumer gets it automatically.
# The feature-name aliasing bug (scores stored as integer_motion2_mau but
# queried as VMAF_integer_feature_motion2_score) was also corrected in the
# test source.  should_fail removed — the test must now pass.
test('test_sycl_motion_add_uv_parity', test_sycl_motion_add_uv_parity,
    suite : ['fast', 'gpu', 'sycl'],
    is_parallel : false)

# ADR-0946 — SYCL kernel coverage round 3. Five new CPU vs. SYCL
# parity gates filling the float-family + PSNR-HVS gaps left after
# rounds 1 (#351: integer psnr / vif) and 2 (#376: integer adm /
# ciede / ssim / ms_ssim / motion_v2). Each test mirrors the
# round-1 / round-2 pattern: 256x144 synthetic YUV420P fixture,
# places=4 (1e-4) ADR-0214 tolerance, skip-on-no-device.
#
# ADR-1450 — float_psnr_sycl returns the CPU's score bit for bit: the kernel
# forms the CPU's float square of each difference as an integer and the
# work-groups and the host add integers. One binary runs full-range noise at
# 8, 10, 12 and 16 bits, `uncapped`, a bright 16-bit 1920x1080 pair and
# identical frames with ==, and a 16-bit 2560x1440 frame whose sum is past
# 2^53, where the CPU's own sum rounds, against the derived bound. The
# `_large` variant below runs the noise cases at 960x540.
# test_sycl_float_psnr_exact_contract is the device-free half.
test('test_sycl_float_psnr_exact_contract',
    import('python').find_installation(),
    args : files('test_sycl_float_psnr_exact_contract.py'),
    suite : ['fast'],
    timeout : 30,
)
test_sycl_float_psnr_parity = executable('test_sycl_float_psnr_parity',
    ['test.c', 'test_sycl_float_psnr_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, sycl_dependency, math_lib],
    c_args: ['-DHAVE_SYCL=1']
)
test('test_sycl_float_psnr_parity', test_sycl_float_psnr_parity,
    suite : ['fast', 'gpu', 'sycl'],
    is_parallel : false,
    timeout : 120)

test_sycl_float_adm_parity = executable('test_sycl_float_adm_parity',
    ['test.c', 'test_sycl_float_adm_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, sycl_dependency, math_lib],
    c_args: ['-DHAVE_SYCL=1']
)
test('test_sycl_float_adm_parity', test_sycl_float_adm_parity,
    suite : ['fast', 'gpu', 'sycl'],
    is_parallel : false)

test_sycl_float_vif_parity = executable('test_sycl_float_vif_parity',
    ['test.c', 'test_sycl_float_vif_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, sycl_dependency, math_lib],
    c_args: ['-DHAVE_SYCL=1']
)
test('test_sycl_float_vif_parity', test_sycl_float_vif_parity,
    suite : ['fast', 'gpu', 'sycl'],
    is_parallel : false)

test_sycl_float_motion_parity = executable('test_sycl_float_motion_parity',
    ['test.c', 'test_sycl_float_motion_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, sycl_dependency, math_lib],
    c_args: ['-DHAVE_SYCL=1']
)
test('test_sycl_float_motion_parity', test_sycl_float_motion_parity,
    suite : ['fast', 'gpu', 'sycl'],
    is_parallel : false)

# psnr_hvs CPU vs. SYCL: bit-identical scores (ADR-1397, ADR-1401) on every
# plane and the combined score, at 8 to 12 bits, in 4:2:0 / 4:2:2 / 4:4:4, and
# at 3840x2160 (third_party/xiph/psnr_hvs.c vs. integer_psnr_hvs_sycl.cpp).
# The fixtures and the comparison are psnr_hvs_twin_parity.h's, shared with
# the CUDA and HIP tests.
test_sycl_psnr_hvs_parity = executable('test_sycl_psnr_hvs_parity',
    ['test.c', 'test_sycl_psnr_hvs_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, sycl_dependency, math_lib],
    c_args: ['-DHAVE_SYCL=1']
)
test('test_sycl_psnr_hvs_parity', test_sycl_psnr_hvs_parity,
    suite : ['fast', 'gpu', 'sycl'],
    is_parallel : false)
# T-SYCL-PSNR-HVS-B580-SIGSEGV-2026-09-29: the Intel GPU compiler crashed
# (host SIGSEGV) building the psnr_hvs kernel at SIMD32 for Xe2 while the
# DCT ran in work-item 0's private memory. IGC picks the SIMD width itself,
# so the same executable runs again with SIMD32 forced; the persistent
# compiler cache is off so a cached SIMD16 binary cannot stand in for it.
# Other drivers and toolchains ignore both variables.
test('test_sycl_psnr_hvs_parity_simd32', test_sycl_psnr_hvs_parity,
    env : ['IGC_ForceOCLSIMDWidth=32', 'NEO_CACHE_PERSISTENT=0'],
    suite : ['fast', 'gpu', 'sycl'],
    is_parallel : false)

# ADR-1369: psnr_sycl, psnr_hvs_sycl and motion_v2_sycl read the planes the
# SYCL state uploads once per frame (luma shared frame + opt-in shared
# chroma). Runs the three twins in one context over frames whose chroma
# changes every frame, at an odd width (pitched 2-D chroma copy), against the
# CPU extractors, and pins the helper contract (geometry / ordering errors,
# once-per-frame upload, double-buffered slots).
test_sycl_shared_planes = executable('test_sycl_shared_planes',
    ['test.c', 'test_sycl_shared_planes.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, sycl_dependency, math_lib],
    c_args: ['-DHAVE_SYCL=1']
)
test('test_sycl_shared_planes', test_sycl_shared_planes,
    suite : ['fast', 'gpu', 'sycl'],
    is_parallel : false)

# Sticky geometry regression test (T-SYCL-SHARED-FRAME-STICKY-GEOMETRY-2026-09-29)
test_sycl_shared_frame_sticky_geometry = executable('test_sycl_shared_frame_sticky_geometry',
    ['test.c', 'test_sycl_shared_frame_sticky_geometry.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, sycl_dependency, math_lib],
    c_args: ['-DHAVE_SYCL=1']
)
test('test_sycl_shared_frame_sticky_geometry', test_sycl_shared_frame_sticky_geometry,
    suite : ['fast', 'gpu', 'sycl'],
    is_parallel : false)

# ADR-0957 — SYCL kernel coverage round 4. Four CPU vs. SYCL
# parity gates closing the last uncovered SYCL extractors after
# round 3 (#446: float family + PSNR-HVS): float_moment_sycl,
# speed_chroma_sycl, speed_temporal_sycl, ssimulacra2_sycl.
#
# ADR-1449 — float_moment_sycl returns the CPU's four moments bit for bit
# (first added as a places=4 test at 8 and 10 bpc, ADR-0957 / ADR-1212). One
# binary runs noise at 8, 10, 12 and 16 bits and a bright 16-bit 1920x1080
# frame with ==, and a bright 16-bit 2560x1440 frame, where the CPU's own sum
# is past 2^53 and rounds, against the derived bound. The `_large` variant
# below runs the noise cases at 960x540. test_sycl_float_moment_exact_contract
# is the device-free half: the kernel adds moment_float_square(), one fp32
# product.
test('test_sycl_float_moment_exact_contract',
    import('python').find_installation(),
    args : files('test_sycl_float_moment_exact_contract.py'),
    suite : ['fast'],
    timeout : 30,
)
test_sycl_float_moment_parity = executable('test_sycl_float_moment_parity',
    ['test.c', 'test_sycl_float_moment_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, sycl_dependency, math_lib],
    c_args: ['-DHAVE_SYCL=1']
)
test('test_sycl_float_moment_parity', test_sycl_float_moment_parity,
    suite : ['fast', 'gpu', 'sycl'],
    is_parallel : false,
    timeout : 120)

# ADR-0884 — SYCL kernel coverage round 2.
# Five additional CPU-vs-SYCL parity gates: adm, ciede, integer_ssim,
# float_ms_ssim, motion_v2. Round 1 (ADR-0868 / PR #351) added psnr +
# vif; this round closes the remaining numerical-correctness gap for
# the high-value SYCL kernels (adm is the dominant feature in every
# shipping VMAF model; ms_ssim's 5-scale exponent stack is the most
# delicate; ciede covers the Intel-Arc Lab-colour path that the Vulkan
# backend cannot reach at places=4 per T-VK-CIEDE-F32-F64). All five
# tests skip cleanly when no SYCL device is visible.
test_sycl_adm_parity = executable('test_sycl_adm_parity',
    ['test.c', 'test_sycl_adm_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, sycl_dependency, math_lib],
    c_args: ['-DHAVE_SYCL=1']
)
test('test_sycl_adm_parity', test_sycl_adm_parity,
    suite : ['fast', 'gpu', 'sycl'],
    is_parallel : false)

# T-GPU-ADM-TINY-FRAME-SHIFT-2026-09-18 — integer ADM on frames 17 to 32 pixels
# wide (scale-0 cube shift 0) against scalar CPU, and rejection below 17x17.
test_sycl_adm_tiny_frames = executable('test_sycl_adm_tiny_frames',
    ['test.c', 'test_gpu_adm_tiny_frames.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, sycl_dependency, math_lib],
    c_args: ['-DHAVE_SYCL=1']
)
test('test_sycl_adm_tiny_frames', test_sycl_adm_tiny_frames,
    suite : ['fast', 'gpu', 'sycl'],
    is_parallel : false)

# T-SYCL-MOTION-TINY-FRAME-PARITY-2026-09-29 — motion_sycl and motion_v2_sycl
# against the scalar CPU motion / motion_v2 with == on 3x3 to 1283x723 frames
# (the last one replays the combined graph) at 8, 10 and 16 bits, and
# rejection below 3x3.
test_sycl_motion_tiny_frames = executable('test_sycl_motion_tiny_frames',
    ['test.c', 'test_sycl_motion_tiny_frames.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, sycl_dependency, math_lib],
    c_args: ['-DHAVE_SYCL=1']
)
test('test_sycl_motion_tiny_frames', test_sycl_motion_tiny_frames,
    suite : ['fast', 'gpu', 'sycl'],
    is_parallel : false)

# T-INTEGER-VIF-TINY-FRAME-GUARD-2026-09-29 — vif_sycl declares its 16-pixel
# minimum through the ADR-1324 gate: model dispatch falls back to the CPU `vif`
# below it (bit-identical), a direct request fails init, and 16x16 stays on
# the device within the VIF parity tolerance.
test_sycl_vif_min_dim = executable('test_sycl_vif_min_dim',
    ['test.c', 'test_sycl_vif_min_dim.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, sycl_dependency, math_lib],
    c_args: ['-DHAVE_SYCL=1']
)
test('test_sycl_vif_min_dim', test_sycl_vif_min_dim,
    suite : ['fast', 'gpu', 'sycl'],
    is_parallel : false)

test_sycl_ciede_parity = executable('test_sycl_ciede_parity',
    ['test.c', 'test_sycl_ciede_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, sycl_dependency, math_lib],
    c_args: ['-DHAVE_SYCL=1']
)
test('test_sycl_ciede_parity', test_sycl_ciede_parity,
    suite : ['fast', 'gpu', 'sycl'],
    is_parallel : false)

# ADR-1443: fixed-point ssim CPU vs. SYCL, equality. The cases
# (ssim_twin_parity.h) cover 8 to 16 bits, frames narrower than the window,
# negative terms and identical frames in dB; twelve of the fifteen fail on
# the fp32, per-block twin this replaced (the identical frames do not).
test_sycl_ssim_parity = executable('test_sycl_ssim_parity',
    ['test.c', 'test_sycl_ssim_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, sycl_dependency, math_lib],
    c_args: ['-DHAVE_SYCL=1']
)
test('test_sycl_ssim_parity', test_sycl_ssim_parity,
    suite : ['fast', 'gpu', 'sycl'],
    is_parallel : false)

test_sycl_ms_ssim_parity = executable('test_sycl_ms_ssim_parity',
    ['test.c', 'test_sycl_ms_ssim_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, sycl_dependency, math_lib],
    c_args: ['-DHAVE_SYCL=1']
)
test('test_sycl_ms_ssim_parity', test_sycl_ms_ssim_parity,
    suite : ['fast', 'gpu', 'sycl'],
    is_parallel : false)

# float_ssim SYCL parity: every output == the CPU extractor's (ADR-1463; first
# added with a 5e-4 tolerance, Research-0985). Runs the constructed pair of
# float_ssim_order_frame.h and noise pairs on which a sum in another order
# moves the float mean, on the device and, device-free, through the twin's
# host hook.
test_sycl_float_ssim_parity = executable('test_sycl_float_ssim_parity',
    ['test.c', 'test_sycl_float_ssim_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, sycl_dependency, math_lib],
    c_args: ['-DHAVE_SYCL=1']
)
test('test_sycl_float_ssim_parity', test_sycl_float_ssim_parity,
    suite : ['fast', 'gpu', 'sycl'],
    is_parallel : false)

test_sycl_motion_v2_parity = executable('test_sycl_motion_v2_parity',
    ['test.c', 'test_sycl_motion_v2_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, sycl_dependency, math_lib],
    c_args: ['-DHAVE_SYCL=1']
)
test('test_sycl_motion_v2_parity', test_sycl_motion_v2_parity,
    suite : ['fast', 'gpu', 'sycl'],
    is_parallel : false)

# GPU-kernel coverage gap-fill — psnr CPU vs. SYCL parity (places=4).
# Covers integer_psnr_sycl.cpp, which previously had no cross-backend
# parity gate (only motion3 + cambi had one for SYCL). SYCL-gated:
# skips cleanly when no SYCL device is visible.
test_sycl_psnr_parity = executable('test_sycl_psnr_parity',
    ['test.c', 'test_sycl_psnr_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, sycl_dependency, math_lib],
    c_args: ['-DHAVE_SYCL=1']
)
test('test_sycl_psnr_parity', test_sycl_psnr_parity,
    suite : ['fast', 'gpu', 'sycl'],
    is_parallel : false)

# CPU-option parity for the SYCL twins (T-BUG048-GPU-OPTION-PARITY-REMAINDER,
# SYCL part): psnr_sycl enable_mse / enable_apsnr / reduced_hbd_peak / min_sse,
# integer_ssim_sycl + float_ssim_sycl enable_db / clip_db, float_ssim_sycl
# enable_lcs, float_motion_sycl motion_max_val / motion_force_zero. The
# option-table checks run without a device; the parity checks skip (77)
# when no SYCL device is visible.
test_sycl_twin_option_parity = executable('test_sycl_twin_option_parity',
    ['test.c', 'test_sycl_twin_option_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, sycl_dependency, math_lib],
    c_args: ['-DHAVE_SYCL=1']
)
test('test_sycl_twin_option_parity', test_sycl_twin_option_parity,
    suite : ['fast', 'gpu', 'sycl'],
    is_parallel : false)

# GPU-kernel coverage gap-fill — VIF scale0 CPU vs. SYCL parity (places=3).
# Covers integer_vif_sycl.cpp, the SYCL Gaussian filter + statistical
# accumulator path that had no cross-backend assertion before.
# SYCL-gated: skips cleanly when no SYCL device is visible.
test_sycl_vif_parity = executable('test_sycl_vif_parity',
    ['test.c', 'test_sycl_vif_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, sycl_dependency, math_lib],
    c_args: ['-DHAVE_SYCL=1']
)
test('test_sycl_vif_parity', test_sycl_vif_parity,
    suite : ['fast', 'gpu', 'sycl'],
    is_parallel : false)
# ADR-1395: an Intel GPU runs the SIMD-32 VIF kernels only when told to; the
# same parity test runs them here. They needed the 256-register file to stop
# spilling, and a spilling kernel scored every frame 0/0 on an Arc A380 under
# the xe driver.
test('test_sycl_vif_parity_sg32', test_sycl_vif_parity,
    env : ['VMAF_SYCL_VIF_SUBGROUP_SIZE=32'],
    suite : ['fast', 'gpu', 'sycl'],
    is_parallel : false)

# ADR-0964 — speed_chroma + speed_temporal CPU vs. SYCL parity gates.
# Pin the cross-backend tolerance (places=4) for the SpEED-family GPU
# twins that this PR wires.  Both tests skip cleanly when no SYCL device
# is visible (mirrors test_sycl_motion3_parity.c).

test_sycl_speed_chroma_parity = executable('test_sycl_speed_chroma_parity',
    ['test.c', 'test_sycl_speed_chroma_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, sycl_dependency, math_lib],
    c_args: ['-DHAVE_SYCL=1']
)
test('test_sycl_speed_chroma_parity', test_sycl_speed_chroma_parity,
    suite : ['fast', 'gpu', 'sycl'],
    is_parallel : false)

test_sycl_speed_temporal_parity = executable('test_sycl_speed_temporal_parity',
    ['test.c', 'test_sycl_speed_temporal_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, sycl_dependency, math_lib],
    c_args: ['-DHAVE_SYCL=1']
)
test('test_sycl_speed_temporal_parity', test_sycl_speed_temporal_parity,
    suite : ['fast', 'gpu', 'sycl'],
    is_parallel : false)

# lanczos4 prescale on SYCL (T-GPU-SPEED-LANCZOS4-PRESCALE-DRIFT-2026-09-30):
# the source of test_cuda_speed_lanczos4_parity, built for the SYCL twins.
test_sycl_speed_lanczos4_parity = executable('test_sycl_speed_lanczos4_parity',
    ['test.c', 'test_gpu_speed_lanczos4_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, sycl_dependency, math_lib],
    c_args: ['-DHAVE_SYCL=1', '-DLZ_BACKEND_SYCL=1']
)
test('test_sycl_speed_lanczos4_parity', test_sycl_speed_lanczos4_parity,
    suite : ['fast', 'gpu', 'sycl'],
    is_parallel : false,
    timeout : 120)

test_sycl_speed_singular_parity = executable('test_sycl_speed_singular_parity',
    ['test.c', 'test_sycl_speed_singular_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, sycl_dependency, math_lib],
    c_args: ['-DHAVE_SYCL=1']
)
test('test_sycl_speed_singular_parity', test_sycl_speed_singular_parity,
    suite : ['fast', 'gpu', 'sycl'],
    is_parallel : false,
    timeout : 90)

test_sycl_ssimulacra2_parity = executable('test_sycl_ssimulacra2_parity',
    ['test.c', 'test_sycl_ssimulacra2_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, sycl_dependency, math_lib],
    c_args: ['-DHAVE_SYCL=1']
)
test('test_sycl_ssimulacra2_parity', test_sycl_ssimulacra2_parity,
    suite : ['fast', 'gpu', 'sycl'],
    is_parallel : false)

# ADR-1001 — CAMBI CPU vs. SYCL parity (round 5). Fills the gap left by
# test_integer_cambi_sycl.c which is explicitly a smoke test, not a
# numerical-correctness gate. Uses a 256×256 quantised-gradient fixture
# (above the 216×216 CAMBI_MIN_WIDTH_HEIGHT threshold) at places=4 (1e-4).
test_sycl_cambi_parity = executable('test_sycl_cambi_parity',
    ['test.c', 'test_sycl_cambi_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, sycl_dependency, math_lib],
    c_args: ['-DHAVE_SYCL=1']
)
test('test_sycl_cambi_parity', test_sycl_cambi_parity,
    suite : ['fast', 'gpu', 'sycl'],
    is_parallel : false)
# ADR-1206 — large-fixture parity variants (resolution blind spot).
#
# Same rationale as the CUDA family above: every parity test here pins one
# small fixture, so the shared SSIM/MS-SSIM auto-scale
# `max(1, round(min(w, h) / 256))` never leaves 1 and the ADM border crop
# `(int)(dim * 0.1 - 0.5)` never leaves 0. Both of the defects fixed in
# ADR-1204 / ADR-1205 lived in exactly that unreachable region. These variants
# rebuild the same TUs against 960x540 (auto-scale 2, and 540 is not a multiple
# of the 16/32-wide kernel blocks so tail bounds are exercised too).
# `test_sycl_motion_add_uv_parity` uses a scalar fixed-point oracle rather than
# the differently rounded CPU float extractor (ADR-1326), so the same tight
# contract is valid at both fixture sizes.
sycl_parity_large_fixture_tests = [
    'test_sycl_adm_parity',
    'test_sycl_cambi_parity',
    'test_sycl_ciede_parity',
    'test_sycl_float_adm_parity',
    'test_sycl_float_moment_parity',
    'test_sycl_float_motion_parity',
    'test_sycl_float_psnr_parity',
    'test_sycl_float_ssim_parity',
    'test_sycl_float_vif_parity',
    'test_sycl_motion3_parity',
    'test_sycl_motion_add_uv_parity',
    'test_sycl_motion_v2_parity',
    'test_sycl_ms_ssim_parity',
    'test_sycl_psnr_hvs_parity',
    'test_sycl_psnr_parity',
    'test_sycl_speed_chroma_parity',
    'test_sycl_speed_temporal_parity',
    'test_sycl_ssim_parity',
    'test_sycl_ssimulacra2_parity',
    'test_sycl_vif_parity',
]
foreach parity_test : sycl_parity_large_fixture_tests
    parity_large_exe = executable(parity_test + '_large',
        ['test.c', parity_test + '.c'],
        include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
        link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
        dependencies: [pthread_dependency, sycl_dependency,  math_lib],
        c_args: ['-DHAVE_SYCL=1', '-DFIXTURE_W=960u', '-DFIXTURE_H=540u']
    )
    test(parity_test + '_large', parity_large_exe,
        suite : ['fast', 'gpu', 'sycl'],
        is_parallel : false)
endforeach


endif

# ADR-0209 / T5-2: embedded MCP scaffold smoke. Pins the -ENOSYS
# contract on every public entry point. T5-2b runtime
# (ADR-0209 § "Status update 2026-05-08") flipped init/start_stdio/
# close to a working dispatcher; the test now drives a real JSON-RPC
# round-trip over pipes. SSE/UDS expectations remain pinned at -ENOSYS.
if get_option('enable_mcp')
test_mcp_smoke = executable('test_mcp_smoke',
    ['test.c', 'test_mcp_smoke.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency],
    c_args: ['-DHAVE_MCP=1']
)
test('test_mcp_smoke', test_mcp_smoke, suite : ['slow'], timeout: 60)

# Coverage push for core/src/mcp/* — complements test_mcp_smoke.c by
# pinning the dispatcher error envelopes, lifecycle state-machine
# edges (validate_config, double-start -EBUSY, empty/overlong UDS+SSE
# paths, stop-then-close), and transport-level error paths
# (stdio oversize-line overflow envelope, SSE 404 / health / malformed
# request / POST without Content-Length). See
# changelog.d/added/core-mcp-coverage-push.md and the matching
# docs/research/ digest.
test_mcp_coverage = executable('test_mcp_coverage',
    ['test.c', 'test_mcp_coverage.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency],
    c_args: ['-DHAVE_MCP=1']
)
test('test_mcp_coverage', test_mcp_coverage, suite : ['slow'], timeout: 60)

# Regression: PR #460 audit follow-up #5. Verifies that
# vmaf_mcp_stop() is idempotent across multiple invocations
# (was: SIGSEGV on the third call due to unconditional
# atomic_exchange mutating 0 -> 2). See
# changelog.d/fixed/vmaf-mcp-stop-double-join-segv.md.
test_mcp_stop_idempotent = executable('test_mcp_stop_idempotent',
    ['test.c', 'test_mcp_stop_idempotent.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency],
    c_args: ['-DHAVE_MCP=1']
)
test('test_mcp_stop_idempotent', test_mcp_stop_idempotent, suite : ['fast'], timeout: 30)

# R2-4 (security): the in-process C MCP compute_vmaf binding must
# allowlist + canonicalise caller paths before stat()/open(), matching
# the Python/Go MCP servers. White-box test #include's compute_vmaf.c to
# reach the static validate_path(); the public entry point is macro-renamed
# in the test to avoid a duplicate-symbol clash with the linked libvmaf.
# See changelog.d/fixed/ and core/src/mcp/AGENTS.md.
test_mcp_compute_vmaf_allowlist = executable('test_mcp_compute_vmaf_allowlist',
    ['test.c', 'test_mcp_compute_vmaf_allowlist.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency],
    c_args: ['-DHAVE_MCP=1']
)
test('test_mcp_compute_vmaf_allowlist', test_mcp_compute_vmaf_allowlist, suite : ['fast'])
endif

# ADR-0212 / T7-10: HIP (AMD ROCm) backend smoke.
# Scaffold-shape sub-tests pin the public-API contract; the T7-10b
# runtime sub-tests probe `hipGetDeviceCount` and skip the device-
# resident assertions when no AMD GPU is visible to the runtime.
# The singular-covariance notice is shared by the CPU extractor and every GPU
# twin, so its counting contract is tested once, on the CPU, with no device.
test_speed_singular_tally = executable('test_speed_singular_tally',
    ['test.c', 'test_speed_singular_tally.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, math_lib],
)
test('test_speed_singular_tally', test_speed_singular_tally, suite : ['fast'])

# The score clamp is shared the same way, and is the one place a non-finite
# score could be published as speed_*_max_val instead of failing the frame.
test_speed_clamp_score = executable('test_speed_clamp_score',
    ['test.c', 'test_speed_clamp_score.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, math_lib],
)
test('test_speed_clamp_score', test_speed_clamp_score, suite : ['fast'])

if get_option('enable_hip') == true
test_hip_smoke = executable('test_hip_smoke',
    ['test.c', 'test_hip_smoke.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, hip_deps],
    c_args: ['-DHAVE_HIP=1']
)
test('test_hip_smoke', test_hip_smoke,
    suite : ['fast', 'gpu'],
    is_parallel : false)

# T3-15(c) / ADR-0219 / ADR-0214 — motion3 CPU vs. HIP parity gate.
# The motion3 post-process (host-side moving-average over blended motion2)
# is computed independently in integer_motion.c (CPU) and
# integer_motion_hip.c (HIP path).  This gate asserts the two paths agree
# to within places=4 (1e-4) at frame index 1 on a 256x144 synthetic fixture.
# HIP-gated: skips cleanly when no AMD GPU is visible.
test_hip_motion3_parity = executable('test_hip_motion3_parity',
    ['test.c', 'test_hip_motion3_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, hip_deps],
    c_args: ['-DHAVE_HIP=1']
)
test('test_hip_motion3_parity', test_hip_motion3_parity,
    suite : ['fast', 'gpu'],
    is_parallel : false,
    timeout : 60)

# ADR-1106 / ADR-0214 — motion_v2 CPU vs. HIP parity gate.
# Asserts the integer_motion_v2 SAD, motion2_v2, and motion3_v2 scores agree
# between CPU and HIP paths to within places=4 (1e-4) on a synthetic fixture.
# HIP-gated: skips cleanly when no AMD GPU is visible.
test_hip_motion_v2_parity = executable('test_hip_motion_v2_parity',
    ['test.c', 'test_hip_motion_v2_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, hip_deps],
    c_args: ['-DHAVE_HIP=1']
)
test('test_hip_motion_v2_parity', test_hip_motion_v2_parity,
    suite : ['fast', 'gpu'],
    is_parallel : false,
    timeout : 60)

# ADR-0539 — integer ADM CPU vs. HIP parity gate (places=4).
# Verifies the per-thread atomicAdd reduction pattern adopted by
# adm_csf_den.hip + adm_cm.hip produces bit-exact-tolerant scores
# vs. the CPU integer_adm extractor on a synthetic fixture.
# HIP-gated: skips cleanly when no AMD GPU is visible.
test_hip_adm_parity = executable('test_hip_adm_parity',
    ['test.c', 'test_hip_adm_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, hip_deps],
    c_args: ['-DHAVE_HIP=1']
)
# The picture-staging deferral (ADR-1154) ended with ADR-1211; the parity
# tests below pass on a gfx1036 device.
test('test_hip_adm_parity', test_hip_adm_parity,
    suite : ['fast', 'gpu'],
    is_parallel : false,
    timeout : 60)

# T-HIP-INIT-UNWIND-REPORTS-SUCCESS-2026-09-22 -- the HIP integer-ADM init failure path.
#
# integer_adm_hip.c is compiled straight into this target rather than linked
# from libvmaf, and test_hip_adm_init_unwind.c defines every symbol the TU
# references: the ~17 hip* entry points, the four HSACO blobs, and seven
# libvmaf helpers (the shared-frame plane source of ADR-1408 among them). `vmaf_feature_name_dict_from_provided_features` is the
# injection point and returns NULL, which drives the one init path that used
# to release every resource and then report success (hipSuccess through the
# unwind ladder's terminal hip_rc()), leaking the two luma stagers on the way
# out. The stubs mean no AMD device is needed, so this runs in the `fast`
# suite on any host -- it is NOT in the `gpu` suite and never skips.
#
# It is gated on enable_hipcc as well as enable_hip because the failure path
# only exists under HAVE_HIPCC; the scaffold build returns -ENOSYS from init
# long before the dictionary is built. `hip_deps` is here for the ROCm
# include directory (hipError_t and friends); the stub definitions in the
# target's own objects are what the calls resolve to.
if get_option('enable_hipcc') == true
test_hip_adm_init_unwind = executable('test_hip_adm_init_unwind',
    ['test.c', 'test_hip_adm_init_unwind.c', '../src/feature/hip/integer_adm_hip.c'],
    # Same include set the library build gives this TU. `../src/cuda` is in it
    # because integer_adm_hip.c's `#include "common.h"` resolves there -- there
    # is no common.h under src/ or src/feature/, and dropping the directory
    # breaks the compile. Keep it until that include is disambiguated.
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'),
                           include_directories('../src/feature/'),
                           include_directories('../src/feature/common/'),
                           include_directories('../src/cuda/')],
    dependencies: [pthread_dependency, math_lib, hip_deps],
    c_args: ['-DHAVE_HIP=1', '-DHAVE_HIPCC']
)
test('test_hip_adm_init_unwind', test_hip_adm_init_unwind, suite : ['fast'])

# T-HIP-TWIN-PRIVATE-PLANE-UPLOADS-2026-09-29 / ADR-1408 -- the frame planes a
# context uploads once and every HIP twin reads (core/src/hip/shared_frame.c).
# Same device-free interposition as the ADM gate above: shared_frame.c is
# compiled into the target against stubs of the three hip* calls and the
# waiting upload it uses, so the contract of shared_frame.h (one upload per
# plane per frame, pictures read before an acquire returns and never after
# the frame ended, a held slot is not overwritten under a running kernel)
# runs in the `fast` suite on any host.
test_hip_shared_frame = executable('test_hip_shared_frame',
    ['test.c', 'test_hip_shared_frame.c', '../src/hip/shared_frame.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    dependencies: [pthread_dependency, hip_deps],
    c_args: ['-DHAVE_HIP=1', '-DHAVE_HIPCC']
)
test('test_hip_shared_frame', test_hip_shared_frame, suite : ['fast'])

# ADR-1407: `hip_strict_fp_args` gives every HIP kernel the CPU reference's
# fp32 arithmetic. The probe kernel is compiled by hipcc with that list, as
# the feature kernels in src/meson.build are, and run over a million random
# operand triples and the boundary combinations; a contracted `a * b + c` or
# an approximate `/` or sqrtf() on the device fails the test. The host side
# is test_sycl_fp_arith_contract.c built for the HIP probe (one set of
# operands and references for both backends). Skips (77) without a device.
hip_fp_arith_probe_hsaco = custom_target('hip_fp_arith_probe_hsaco',
    input : 'test_hip_fp_arith_probe.hip',
    output : 'test_hip_fp_arith_probe.hsaco',
    command : [hipcc_exe, '--genco'] + hip_offload_arch_flags + hip_include_flags
              + hip_strict_fp_args + ['@INPUT@', '-o', '@OUTPUT@'],
)
hip_fp_arith_probe_hsaco_c = custom_target('hip_fp_arith_probe_hsaco_c',
    input : hip_fp_arith_probe_hsaco,
    output : 'test_hip_fp_arith_probe_hsaco.c',
    capture : true,
    command : [xxd_hip, '-i', '-n', 'test_hip_fp_arith_probe_hsaco', '@INPUT@'],
)
test_hip_fp_arith_contract = executable('test_hip_fp_arith_contract',
    ['test.c', 'test_sycl_fp_arith_contract.c', 'test_hip_fp_arith_probe.c',
     hip_fp_arith_probe_hsaco_c],
    include_directories : [test_inc],
    dependencies : [math_lib, hip_deps],
    # The host references are single fp64 operations rounded once; the
    # project-wide strict policy (ADR-1461) already turns icx's fast model
    # (reciprocal division) off.
    c_args : ['-DFP_ARITH_PROBE=vmaf_test_hip_fp_arith', '-DFP_ARITH_DEVICE="HIP"'],
)
test('test_hip_fp_arith_contract', test_hip_fp_arith_contract,
    suite : ['fast', 'gpu'],
    is_parallel : false,
    timeout : 120)

# ADR-1458: the arithmetic float_adm_hip's kernels run
# (src/feature/float_adm_gpu_common.h in the spelling of
# src/feature/hip/float_adm/float_adm_hip_math.h), value by value on a device
# against the same statements on the host: a million samples through the
# decouple with its division, the CSF, a masking sum and the terms, at the
# default options, at a gain limit that is not an fp32 value and at
# adm_p_norm = 1. The probe kernel is compiled by hipcc with the list every
# feature kernel gets. test_float_adm_device_math holds the same header to
# the CPU extractor without a device. Skips (77) without a device.
hip_float_adm_math_probe_hsaco = custom_target('hip_float_adm_math_probe_hsaco',
    input : 'test_hip_float_adm_math_probe.hip',
    output : 'test_hip_float_adm_math_probe.hsaco',
    depend_files : files('hip_float_adm_math_sample.h',
                         '../src/feature/float_adm_gpu_common.h',
                         '../src/feature/hip/float_adm/float_adm_hip_math.h'),
    command : [hipcc_exe, '--genco'] + hip_offload_arch_flags + hip_include_flags
              + hip_strict_fp_args
              + ['-I', meson.current_source_dir(),
                 '-I', join_paths(meson.current_source_dir(), '../src'),
                 '@INPUT@', '-o', '@OUTPUT@'],
)
hip_float_adm_math_probe_hsaco_c = custom_target('hip_float_adm_math_probe_hsaco_c',
    input : hip_float_adm_math_probe_hsaco,
    output : 'test_hip_float_adm_math_probe_hsaco.c',
    capture : true,
    command : [xxd_hip, '-i', '-n', 'test_hip_float_adm_math_probe_hsaco', '@INPUT@'],
)
test_hip_float_adm_math = executable('test_hip_float_adm_math',
    ['test.c', 'test_hip_float_adm_math.c', hip_float_adm_math_probe_hsaco_c],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies : [math_lib, hip_deps, pthread_dependency],
    # The host side is the reference's arithmetic only without contraction.
    c_args : vmaf_strict_fp_args + ['-DHAVE_HIP=1'],
)
test('test_hip_float_adm_math', test_hip_float_adm_math,
    suite : ['fast', 'gpu'],
    is_parallel : false,
    timeout : 180)

# T-HIP-INIT-UNWIND-REPORTS-SUCCESS-2026-09-22, second site -- the two SSIMULACRA2 HIP allocator failure branches.
# Same device-free interposition as the ADM gate above; here the injection is
# an allocator that fails on a chosen call index, so both ss2h_alloc_device()
# and ss2h_alloc_pinned() can be driven independently.
test_hip_ssimulacra2_init_unwind = executable('test_hip_ssimulacra2_init_unwind',
    ['test.c', 'test_hip_ssimulacra2_init_unwind.c', '../src/feature/hip/ssimulacra2_hip.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'),
                           include_directories('../src/feature/'),
                           include_directories('../src/feature/common/'),
                           include_directories('../src/cuda/')],
    dependencies: [pthread_dependency, math_lib, hip_deps],
    c_args: ['-DHAVE_HIP=1', '-DHAVE_HIPCC']
)
test('test_hip_ssimulacra2_init_unwind', test_hip_ssimulacra2_init_unwind, suite : ['fast'])
endif

# ADR-1167 — ADM small-frame border parity test (i == 0 && top <= 0 branch).
test_hip_adm_small_border = executable('test_hip_adm_small_border',
    ['test.c', 'test_adm_small_border.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, hip_deps],
    c_args: ['-DHAVE_HIP=1']
)
test('test_hip_adm_small_border', test_hip_adm_small_border,
    suite : ['fast', 'gpu'],
    is_parallel : false,
    timeout : 60)

# T-GPU-ADM-TINY-FRAME-SHIFT-2026-09-18 — integer ADM on frames 17 to 32 pixels
# wide (scale-0 cube shift 0) against scalar CPU, and rejection below 17x17.
test_hip_adm_tiny_frames = executable('test_hip_adm_tiny_frames',
    ['test.c', 'test_gpu_adm_tiny_frames.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, hip_deps],
    c_args: ['-DHAVE_HIP=1']
)
test('test_hip_adm_tiny_frames', test_hip_adm_tiny_frames,
    suite : ['fast', 'gpu'],
    is_parallel : false,
    timeout : 60)

# T-HIP-MOTION-BLUR-THEN-DIFF-2026-09-29 / ADR-1377 — motion_hip and
# motion_v2_hip against the scalar CPU motion / motion_v2 with == on 3x3 to
# 1283x723 frames at 8, 10 and 16 bits (the debug motion score included), and
# rejection below 3x3. Skips (77) without a HIP device.
test_hip_motion_tiny_frames = executable('test_hip_motion_tiny_frames',
    ['test.c', 'test_hip_motion_tiny_frames.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, hip_deps, math_lib],
    c_args: ['-DHAVE_HIP=1']
)
test('test_hip_motion_tiny_frames', test_hip_motion_tiny_frames,
    suite : ['fast', 'gpu'],
    is_parallel : false,
    timeout : 120)

# T-HIP-INTEGER-SSIM-TINY-IDENTICAL-DB-2026-09-30 / ADR-1400, ADR-1438 —
# integer_ssim_hip against the CPU `ssim` with == and `enable_db` from 1x1
# frames up (identical 1x1 and 2x2 included; 65x64 to 322x182, which the twin
# summed per block before ADR-1438), and rejection of an empty frame. Skips
# (77) without a HIP device.
test_hip_ssim_tiny_frames = executable('test_hip_ssim_tiny_frames',
    ['test.c', 'test_hip_ssim_tiny_frames.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, hip_deps, math_lib],
    c_args: ['-DHAVE_HIP=1']
)
test('test_hip_ssim_tiny_frames', test_hip_ssim_tiny_frames,
    suite : ['fast', 'gpu'],
    is_parallel : false,
    timeout : 180)

# T-GPU-INTEGER-VIF-MIN-DIM-TWINS-2026-09-29 / ADR-1381 — vif_hip declares its
# 16-pixel minimum through the ADR-1324 gate (model dispatch runs the CPU `vif`
# below it) and rejects smaller direct requests. The declaration and init
# checks need no device; the model boundary cases skip (77) without one.
test_hip_vif_min_dim = executable('test_hip_vif_min_dim',
    ['test.c', 'test_hip_vif_min_dim.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, hip_deps, math_lib],
    c_args: ['-DHAVE_HIP=1']
)
test('test_hip_vif_min_dim', test_hip_vif_min_dim,
    suite : ['fast', 'gpu'],
    is_parallel : false,
    timeout : 60)

# T-BUG048-GPU-OPTION-PARITY-REMAINDER-2026-09-26 (HIP part) / ADR-1382 — the
# CPU psnr / ssim / float_ssim / float_motion / motion options on the HIP twins,
# per option against the CPU. The option-table checks need no device; the
# parity cases skip (77) without one.
test_hip_twin_option_parity = executable('test_hip_twin_option_parity',
    ['test.c', 'test_hip_twin_option_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, hip_deps, math_lib],
    c_args: ['-DHAVE_HIP=1']
)
test('test_hip_twin_option_parity', test_hip_twin_option_parity,
    suite : ['fast', 'gpu'],
    is_parallel : false,
    timeout : 120)

# ADR-1167 — ADM wide-frame row-level rounding parity test (spans 60 warps).
test_hip_adm_wide_rounding = executable('test_hip_adm_wide_rounding',
    ['test.c', 'test_adm_wide_rounding.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, hip_deps],
    c_args: ['-DHAVE_HIP=1']
)
test('test_hip_adm_wide_rounding', test_hip_adm_wide_rounding,
    suite : ['fast', 'gpu'],
    is_parallel : false,
    timeout : 60)

# ADR-0958 round-4 — HIP kernel parity gap-fill (2 extractors).
# Closes the next batch after round-3 (PR #443 / ADR-0945): ssimulacra2_hip,
# float_ssim_hip.  Each test follows the same template as the round-1/2/3
# parity tests: CPU reference vs. HIP score on a synthetic fixture,
# places=3 (1e-3) for filtered / pooled features (ssimulacra2 multi-scale
# pyramid, float_ssim windowed pooling).  HIP-gated: skips cleanly when
# no AMD GPU is visible OR when the HIP scaffold returns -ENOSYS
# (enable_hipcc=false).
#
# Note: speed_chroma_hip / speed_temporal_hip parity gates were planned
# for round 4 (ADR-0958) but deferred due to a latent link defect —
# `speed_internal_init_dimensions` / `speed_internal_float_stride` were
# declared in `speed_internal.h` but lacked an implementation.  That
# defect was resolved by ADR-0964 / PR #465 (`core/src/feature/speed_internal.c`).
# The gates are now shipped in round 5 (ADR-1004) below.

# ADR-1445: ssimulacra2_hip returns the CPU's score bit for bit -- the terms
# are the CPU's fp64 expressions and the sums the bits of the CPU's loops
# (feature/ordered_sum.h), formed on the device from integer increments per
# binade as on the CUDA twin (ADR-1433). test_hip_ssimulacra2_parity compares
# with == (seven cases, 256x144 and 960x540); the contract below is the
# device-free half, the shared arithmetic is test_ordered_sum's.
test('test_hip_ssimulacra2_exact_contract',
    import('python').find_installation(),
    args : files('test_hip_ssimulacra2_exact_contract.py'),
    suite : ['fast'],
    timeout : 30,
)

test_hip_ssimulacra2_parity = executable('test_hip_ssimulacra2_parity',
    ['test.c', 'test_hip_ssimulacra2_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, hip_deps, math_lib],
    c_args: ['-DHAVE_HIP=1']
)
test('test_hip_ssimulacra2_parity', test_hip_ssimulacra2_parity,
    suite : ['fast', 'gpu'],
    is_parallel : false,
    timeout : 60)

test_hip_float_ssim_parity = executable('test_hip_float_ssim_parity',
    ['test.c', 'test_hip_float_ssim_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, hip_deps, math_lib],
    c_args: ['-DHAVE_HIP=1']
)
test('test_hip_float_ssim_parity', test_hip_float_ssim_parity,
    suite : ['fast', 'gpu'],
    is_parallel : false,
    timeout : 60)

# ADR-0945 round-3 — HIP kernel parity gap-fill (4 extractors).
# Closes the next batch after round-2 (PR #372 / ADR-0883): cambi_hip,
# float_adm_hip, float_motion_hip, float_psnr_hip.  Each test follows
# the same template as the round-1 / round-2 parity tests: CPU
# reference vs. HIP score on a synthetic fixture, places=4 (1e-4) for
# unfiltered reductions (float_psnr, float_motion, float_adm scales)
# and places=3 (1e-3) for cambi (multi-scale pooling).  HIP-gated:
# skips cleanly when no AMD GPU is visible OR when the HIP scaffold
# returns -ENOSYS (enable_hipcc=false).

test_hip_cambi_parity = executable('test_hip_cambi_parity',
    ['test.c', 'test_hip_cambi_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, hip_deps, math_lib],
    c_args: ['-DHAVE_HIP=1']
)
test('test_hip_cambi_parity', test_hip_cambi_parity,
    suite : ['fast', 'gpu'],
    is_parallel : false,
    timeout : 60)

# ADR-1437: the HIP twins declared exact (motion, motion_v2, psnr,
# float_ms_ssim, cambi) return the CPU extractor's bits: four frames at 8 and
# at 10 bits, `==` on every output. Exits 77 when no AMD GPU is visible.
test_hip_exact_twins = executable('test_hip_exact_twins',
    ['test.c', 'test_hip_exact_twins.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, hip_deps, math_lib],
    c_args: ['-DHAVE_HIP=1']
)
test('test_hip_exact_twins', test_hip_exact_twins,
    suite : ['fast', 'gpu'],
    is_parallel : false,
    timeout : 120)

test_hip_float_adm_parity = executable('test_hip_float_adm_parity',
    ['test.c', 'test_hip_float_adm_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, hip_deps, math_lib],
    c_args: ['-DHAVE_HIP=1']
)
test('test_hip_float_adm_parity', test_hip_float_adm_parity,
    suite : ['fast', 'gpu'],
    is_parallel : false,
    timeout : 180)

test_hip_float_motion_parity = executable('test_hip_float_motion_parity',
    ['test.c', 'test_hip_float_motion_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, hip_deps, math_lib],
    c_args: ['-DHAVE_HIP=1']
)
test('test_hip_float_motion_parity', test_hip_float_motion_parity,
    suite : ['fast', 'gpu'],
    is_parallel : false,
    timeout : 60)

# ADR-1440: float_psnr CPU vs. HIP with == on full-range noise at 8, 10, 12
# and 16 bits (the input on which a fp32 block sum rounds). Exits 77 when no
# AMD GPU is visible.
test_hip_float_psnr_parity = executable('test_hip_float_psnr_parity',
    ['test.c', 'test_hip_float_psnr_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, hip_deps, math_lib],
    c_args: ['-DHAVE_HIP=1']
)
test('test_hip_float_psnr_parity', test_hip_float_psnr_parity,
    suite : ['fast', 'gpu'],
    is_parallel : false,
    timeout : 60)

# ADR-1444: float_vif_hip returns the CPU's outputs bit for bit. The cases are
# float_vif_twin_parity.h's (default, debug, model options, skip_scale0, the
# per-scale floors, 10 bit, a 50x38 frame), compared with ==; a `_large`
# variant runs them at 960x540. The device-free halves are
# test_float_vif_device_math (the arithmetic) and
# test_hip_float_vif_exact_contract (the design).
test_hip_float_vif_parity = executable('test_hip_float_vif_parity',
    ['test.c', 'test_hip_float_vif_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, hip_deps, math_lib],
    c_args: ['-DHAVE_HIP=1']
)
test('test_hip_float_vif_parity', test_hip_float_vif_parity,
    suite : ['fast', 'gpu'],
    is_parallel : false,
    timeout : 120)
# ADR-1447 — float_moment_hip returns the CPU's four moments bit for bit
# (first added as a places=4 test at 8 and 10 bpc, ADR-1212). One binary runs
# noise at 8, 10, 12 and 16 bits and a bright 16-bit 1920x1080 frame with ==,
# and a bright 16-bit 2048x1100 frame, where the CPU's own sum is past 2^53
# and rounds, against the derived bound. A `_large` variant runs the noise
# cases at 960x540. test_hip_float_moment_exact_contract is the device-free
# half: the 16-bit kernel adds moment_float_square(), one fp32 product.
test('test_hip_float_moment_exact_contract',
    import('python').find_installation(),
    args : files('test_hip_float_moment_exact_contract.py'),
    suite : ['fast'],
    timeout : 30,
)
test_hip_float_moment_parity = executable('test_hip_float_moment_parity',
    ['test.c', 'test_hip_float_moment_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, hip_deps, math_lib],
    c_args: ['-DHAVE_HIP=1']
)
test('test_hip_float_moment_parity', test_hip_float_moment_parity,
    suite : ['fast', 'gpu'],
    is_parallel : false,
    timeout : 120)

# ADR-0883 round-2 — HIP kernel parity gap-fill (5 extractors).
# Closes the gaps left after PR #351 (psnr_hip + vif_hip) and the
# pre-existing adm/motion3 tests.  Each test follows the same template:
# CPU reference vs. HIP score on a synthetic fixture, places=4 (1e-4)
# for unfiltered reductions (ciede, psnr_hvs, motion) and places=3
# (1e-3) for filtered ones (ssim, ms_ssim).  HIP-gated: skips cleanly
# when no AMD GPU is visible.

# ADR-1448: ciede_hip runs ciede.c's arithmetic in fp32 pairs
# (src/feature/ciede_ff_math.h, the header of the SYCL twin, on the
# primitives of src/feature/hip/integer_ciede/ciede_hip_math.h) and adds the
# per-pixel values in the CPU's order. The cases are ciede_twin_parity.h's
# (four bit depths, odd sizes incl. the 577x325 of ADR-1213, 4:2:2, 4:4:4,
# 1080p) at 1e-8; a `_large` variant runs at 960x540. Device-free halves:
# test_hip_ciede_math (the arithmetic) and test_hip_ciede_exact_contract.
test('test_hip_ciede_exact_contract',
    import('python').find_installation(),
    args : files('test_hip_ciede_exact_contract.py'),
    suite : ['fast'],
    timeout : 30,
)
test_hip_ciede_parity = executable('test_hip_ciede_parity',
    ['test.c', 'test_hip_ciede_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, hip_deps, math_lib],
    c_args: ['-DHAVE_HIP=1']
)
test('test_hip_ciede_parity', test_hip_ciede_parity,
    suite : ['fast', 'gpu'],
    is_parallel : false,
    timeout : 180)

# ADR-1448 — the arithmetic ciede_hip's kernels run, on the host: the fp32
# pair functions of src/feature/ff_math.h against the host's
# extended-precision math library, and the ciede2000 pixel of
# src/feature/ciede_ff_math.h against the same statements in fp64
# (cuda/integer_ciede/ciede_device.h compiled for the host), both on the
# HIP twin's primitives. It is test_sycl_ciede_math.c with the host half of
# its probe for HIP; no device is needed, so it runs in the `fast` suite of
# every HIP build. Both TUs are compiled without contraction, like the
# kernels.
test_hip_ciede_math = executable('test_hip_ciede_math',
    ['test.c', 'test_sycl_ciede_math.c', 'test_hip_ciede_math_probe.cpp'],
    include_directories : [test_inc, include_directories('../src/')],
    dependencies : [math_lib],
    c_args : vmaf_strict_fp_args + ['-DVMAF_TEST_CIEDE_MATH_HOST_ONLY=1'],
    cpp_args : vmaf_strict_fp_args,
)
test('test_hip_ciede_math', test_hip_ciede_math,
    suite : ['fast'],
    timeout : 120)

# psnr_hvs CPU vs. HIP: bit-identical scores (ADR-1397, ADR-1401), the same
# cases as test_cuda_psnr_hvs_parity through psnr_hvs_twin_parity.h
# (third_party/xiph/psnr_hvs.c vs. integer_psnr_hvs/psnr_hvs_score.hip).
test_hip_psnr_hvs_parity = executable('test_hip_psnr_hvs_parity',
    ['test.c', 'test_hip_psnr_hvs_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, hip_deps, math_lib],
    c_args: ['-DHAVE_HIP=1']
)
test('test_hip_psnr_hvs_parity', test_hip_psnr_hvs_parity,
    suite : ['fast', 'gpu'],
    is_parallel : false,
    timeout : 60)

test_hip_motion_parity = executable('test_hip_motion_parity',
    ['test.c', 'test_hip_motion_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, hip_deps, math_lib],
    c_args: ['-DHAVE_HIP=1']
)
test('test_hip_motion_parity', test_hip_motion_parity,
    suite : ['fast', 'gpu'],
    is_parallel : false,
    timeout : 60)

# ADR-1491: motion_hip and motion_v2_hip compute motion_five_frame_window
# with the CPU's bits. Every output of every frame, `==`, for 11, 1, 2 and 3
# frames at 8 and 10 bits, the option alone and with the other motion options
# (motion_five_frame_twin_parity.h). Fails on a twin without the window.
test_hip_motion_five_frame_window = executable('test_hip_motion_five_frame_window',
    ['test.c', 'test_hip_motion_five_frame_window.c'],
    include_directories : [libvmaf_inc, test_inc],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, hip_deps, math_lib],
    c_args: ['-DHAVE_HIP=1']
)
test('test_hip_motion_five_frame_window', test_hip_motion_five_frame_window,
    suite : ['fast', 'gpu'],
    is_parallel : false,
    timeout : 120)

# ADR-0564 — integer_ssim CPU vs. HIP parity, places=4 on every frame of an
# 8-frame pooled clip. The kernel is the CPU's 9-tap int64 algorithm, so the
# deltas are summation-order rounding (~1e-14). Registered at 8 bpc, at 10 bpc
# (the 16-bit horizontal kernel) and at an odd size (a truncated window in a
# partial last block); the 960x540 variant is in the large-fixture list below.
foreach variant : [['', []], ['_10bit', ['-DFIXTURE_BPC=10u']],
                   ['_odd', ['-DFIXTURE_W=577u', '-DFIXTURE_H=323u']]]
    hip_ssim_exe = executable('test_hip_ssim_parity' + variant[0],
        ['test.c', 'test_hip_ssim_parity.c'],
        include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
        link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
        dependencies: [pthread_dependency, hip_deps, math_lib],
        c_args: ['-DHAVE_HIP=1'] + variant[1]
    )
    test('test_hip_ssim_parity' + variant[0], hip_ssim_exe,
        suite : ['fast', 'gpu'],
        is_parallel : false,
        timeout : 60)
endforeach

# T-HIP-PAGEABLE-UPLOAD-RACE-2026-09-18: every HIP extractor that uploads a host
# picture, over the pooled multi-frame fixture (hip_pooled_fixture.h) and with
# both pictures refilled the moment submit() returns. 576x324 is the Netflix
# pair's size; an upload outlives submit() more often the larger the plane.
test_hip_upload_race = executable('test_hip_upload_race',
    ['test.c', 'test_hip_upload_race.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, hip_deps, math_lib],
    c_args: ['-DHAVE_HIP=1', '-DFIXTURE_W=576u', '-DFIXTURE_H=324u']
)
test('test_hip_upload_race', test_hip_upload_race,
    suite : ['fast', 'gpu'],
    is_parallel : false,
    timeout : 120)

# ADR-1423 -- adm_hip returns the CPU extractor's bits: every output of nine
# cases compared with ==, the per-scale sums of debug=true included. The
# low-detail, 3840x2160 and shift-boundary cases fail on a twin that folds the
# denominator per thread or derives a shift on the device. HIP-gated.
test_hip_adm_exact = executable('test_hip_adm_exact',
    ['test.c', 'test_hip_adm_exact.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, hip_deps, math_lib],
    c_args: ['-DHAVE_HIP=1']
)
test('test_hip_adm_exact', test_hip_adm_exact,
    suite : ['fast', 'gpu'],
    is_parallel : false,
    timeout : 180)

# ADR-1427: a HIP frame uploads its planes, then clears its accumulators, then
# launches its kernels; a clear queued ahead of the upload is lost in a later,
# larger context of a process. Device-free source contract over every HIP
# twin, with planted regressions; the device check is
# test_hip_first_frame_clear.
test('test_hip_clear_after_upload_contract',
    import('python').find_installation(),
    args : files('test_hip_clear_after_upload_contract.py'),
    suite : ['fast'],
    timeout : 30,
)

# ADR-1427 -- the first frame of a HIP context that follows a smaller one in
# the same process is the CPU's: one frame at 640x360, then one at 3840x2160.
# A twin that queues its accumulator clear ahead of the frame's upload adds
# onto the smaller context's sums there. Only the first larger context of a
# process is exposed, so every twin gets its own binary. HIP-gated.
hip_first_frame_twins = [
    'adm_hip',
    'float_moment_hip',
    'psnr_hip',
    'float_psnr_hip',
    'vif_hip',
    'float_vif_hip',
    'float_adm_hip',
    'ciede_hip',
    'integer_ssim_hip',
    'float_ssim_hip',
    'integer_ms_ssim_hip',
    'psnr_hvs_hip',
    'cambi_hip',
    'ssimulacra2_hip',
]
foreach twin : hip_first_frame_twins
    test_hip_first_frame_clear = executable('test_hip_first_frame_clear_' + twin,
        ['test.c', 'test_hip_first_frame_clear.c'],
        include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
        link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
        dependencies: [pthread_dependency, hip_deps, math_lib],
        c_args: ['-DHAVE_HIP=1', '-DFIRST_FRAME_TWIN="' + twin + '"']
    )
    test('test_hip_first_frame_clear_' + twin, test_hip_first_frame_clear,
        suite : ['fast', 'gpu'],
        is_parallel : false,
        timeout : 180)
endforeach

test_hip_ms_ssim_parity = executable('test_hip_ms_ssim_parity',
    ['test.c', 'test_hip_ms_ssim_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, hip_deps, math_lib],
    c_args: ['-DHAVE_HIP=1']
)
test('test_hip_ms_ssim_parity', test_hip_ms_ssim_parity,
    suite : ['fast', 'gpu'],
    is_parallel : false,
    timeout : 60)

# GPU-kernel coverage gap-fill — psnr CPU vs. HIP parity (places=4).
# Covers integer_psnr_hip.c + integer_psnr/psnr_score.hip, which had
# no cross-backend parity assertion (only smoke + motion3 + ADM).
# HIP-gated: skips cleanly when no AMD GPU is visible.
test_hip_psnr_parity = executable('test_hip_psnr_parity',
    ['test.c', 'test_hip_psnr_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, hip_deps, math_lib],
    c_args: ['-DHAVE_HIP=1']
)
test('test_hip_psnr_parity', test_hip_psnr_parity,
    suite : ['fast', 'gpu'],
    is_parallel : false,
    timeout : 60)

# ADR-1435: vif CPU vs. HIP, every output bit for bit (the four scale scores
# and the debug sums; 8, 10 and 12 bits; vif_enhn_gain_limit and
# vif_skip_scale0). Covers integer_vif_hip.c + integer_vif/vif_statistics.hip.
# HIP-gated: exits 77 when no AMD GPU is visible.
test_hip_vif_parity = executable('test_hip_vif_parity',
    ['test.c', 'test_hip_vif_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, hip_deps, math_lib],
    c_args: ['-DHAVE_HIP=1']
)
test('test_hip_vif_parity', test_hip_vif_parity,
    suite : ['fast', 'gpu'],
    is_parallel : false,
    timeout : 60)

# ADR-1004 round 5 — speed_chroma + speed_temporal HIP parity gates.
# Deferred from round 4 (ADR-0958) because `speed_internal_init_dimensions`
# and `speed_internal_float_stride` had no .c implementation at the time.
# ADR-0964 / PR #465 introduced `core/src/feature/speed_internal.c`,
# unblocking these tests.  Both tests link against libvmaf (which compiles
# speed_internal.c via src/meson.build `speed_internal` entry) and follow
# the round-1/2/3/4 template: synthetic fixture, CPU reference vs. HIP
# score, skip cleanly on `[skip: no HIP device]` or
# `[skip: HIP scaffold ENOSYS]`.  speed_temporal: places=4 (1e-4) per
# ADR-0214.  speed_chroma: its three scores at every frame to one part in
# a million on a fixture with a regular covariance (ADR-1452: what is left
# is the CPU's log2f; speed_chroma_twin_parity.h, shared with CUDA).
test_hip_speed_chroma_parity = executable('test_hip_speed_chroma_parity',
    ['test.c', 'test_hip_speed_chroma_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, hip_deps, math_lib],
    c_args: ['-DHAVE_HIP=1']
)
test('test_hip_speed_chroma_parity', test_hip_speed_chroma_parity,
    suite : ['fast', 'gpu'],
    is_parallel : false,
    timeout : 60)

test_hip_speed_temporal_parity = executable('test_hip_speed_temporal_parity',
    ['test.c', 'test_hip_speed_temporal_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, hip_deps, math_lib],
    c_args: ['-DHAVE_HIP=1']
)
test('test_hip_speed_temporal_parity', test_hip_speed_temporal_parity,
    suite : ['fast', 'gpu'],
    is_parallel : false,
    timeout : 60)

# lanczos4 prescale on HIP (T-GPU-SPEED-LANCZOS4-PRESCALE-DRIFT-2026-09-30):
# the source of test_cuda_speed_lanczos4_parity, built for the HIP twins.
test_hip_speed_lanczos4_parity = executable('test_hip_speed_lanczos4_parity',
    ['test.c', 'test_gpu_speed_lanczos4_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, hip_deps, math_lib],
    c_args: ['-DHAVE_HIP=1', '-DLZ_BACKEND_HIP=1']
)
test('test_hip_speed_lanczos4_parity', test_hip_speed_lanczos4_parity,
    suite : ['fast', 'gpu'],
    is_parallel : false,
    timeout : 180)

test_hip_speed_singular_parity = executable('test_hip_speed_singular_parity',
    ['test.c', 'test_hip_speed_singular_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, hip_deps, math_lib],
    c_args: ['-DHAVE_HIP=1']
)
test('test_hip_speed_singular_parity', test_hip_speed_singular_parity,
    suite : ['fast', 'gpu'],
    is_parallel : false,
    timeout : 90)

# ADR-1378 / ADR-1384 -- device-free replays of the device-resident HIP CAMBI
# and SpEED kernels. Each compiles the kernels' per-work-item header
# (integer_cambi/cambi_hip_device.h, speed/speed_hip_device.h) for the host,
# with contraction off like the kernel build, and replays a frame's chain
# work-item by work-item against the CPU extractor. No AMD device is needed,
# so both run in the `fast` suite of every HIP build, with or without hipcc;
# the `gpu`-suite parity tests above remain the on-device gate.
hip_device_math_args = ['-DHAVE_HIP=1'] + cc.get_supported_arguments(['-ffp-contract=off'])
test_hip_cambi_device_math = executable('test_hip_cambi_device_math',
    ['test.c', 'test_hip_cambi_device_math.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/feature/'),
                           include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, hip_deps, math_lib],
    c_args: hip_device_math_args
)
test('test_hip_cambi_device_math', test_hip_cambi_device_math,
    suite : ['fast'],
    timeout : 120)

# ADR-1409 — float_motion_hip's row sums (float_motion/float_motion_rows.h,
# the lines the row kernels and the extractor compile) against
# motion.c::compute_motion(), bit for bit, with and without
# motion_add_scale1 and at odd sizes. No AMD device is needed.
test_hip_float_motion_rows = executable('test_hip_float_motion_rows',
    ['test.c', 'test_hip_float_motion_rows.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/feature/'),
                           include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, hip_deps, math_lib],
    c_args: hip_device_math_args
)
test('test_hip_float_motion_rows', test_hip_float_motion_rows,
    suite : ['fast'],
    timeout : 120)

# ADR-1405 — float_ssim_hip's device decimation (float_ssim/ssim_decimate.h,
# the lines the kernel compiles) against the CPU's iqa_decimate(), byte for
# byte: scales 1 to 10 and 128, 8 to 16 bits, odd sizes, windows wider than
# the plane. No AMD device is needed.
test_hip_float_ssim_decimate = executable('test_hip_float_ssim_decimate',
    ['test.c', 'test_hip_float_ssim_decimate.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/feature/'),
                           include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, hip_deps, math_lib],
    c_args: hip_device_math_args
)
test('test_hip_float_ssim_decimate', test_hip_float_ssim_decimate,
    suite : ['fast'],
    timeout : 120)

# ADR-1403 — float_ms_ssim_hip's per-sample arithmetic
# (integer_ms_ssim/ms_ssim_arith.h, the lines the kernels and the extractor
# compile) replayed on the host against the CPU extractor: all 16 outputs of
# `enable_lcs`, bit for bit, at 8 and 10 bits and an odd size. No AMD device
# is needed.
test_hip_ms_ssim_arith = executable('test_hip_ms_ssim_arith',
    ['test.c', 'test_hip_ms_ssim_arith.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/feature/'),
                           include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, hip_deps, math_lib],
    c_args: hip_device_math_args
)
test('test_hip_ms_ssim_arith', test_hip_ms_ssim_arith,
    suite : ['fast'],
    timeout : 120)

test_hip_speed_device_math = executable('test_hip_speed_device_math',
    ['test.c', 'test_hip_speed_device_math.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/feature/'),
                           include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, hip_deps, math_lib],
    c_args: hip_device_math_args +
            ['-DSPEED_TESTDATA_DIR="' + join_paths(meson.project_source_root(), '../testdata') + '"']
)
test('test_hip_speed_device_math', test_hip_speed_device_math,
    suite : ['fast'],
    timeout : 180)
# ADR-1206 — large-fixture parity variants (resolution blind spot).
# Same rationale as the CUDA and SYCL families. Verified on a gfx1030 once
# ADR-1211 made the HIP ADM extractor able to run at all.
hip_parity_large_fixture_tests = [
    'test_hip_adm_parity',
    'test_hip_cambi_parity',
    'test_hip_ciede_parity',
    'test_hip_float_adm_parity',
    'test_hip_float_moment_parity',
    'test_hip_float_motion_parity',
    'test_hip_float_psnr_parity',
    'test_hip_float_ssim_parity',
    'test_hip_float_vif_parity',
    'test_hip_motion3_parity',
    'test_hip_motion_parity',
    'test_hip_motion_v2_parity',
    'test_hip_ms_ssim_parity',
    'test_hip_psnr_hvs_parity',
    'test_hip_psnr_parity',
    'test_hip_speed_chroma_parity',
    'test_hip_speed_temporal_parity',
    'test_hip_ssim_parity',
    'test_hip_ssimulacra2_parity',
    'test_hip_vif_parity',
]
foreach parity_test : hip_parity_large_fixture_tests
    parity_large_exe = executable(parity_test + '_large',
        ['test.c', parity_test + '.c'],
        include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
        link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
        dependencies: [pthread_dependency, hip_deps, math_lib],
        c_args: ['-DHAVE_HIP=1', '-DFIXTURE_W=960u', '-DFIXTURE_H=540u']
    )
    test(parity_test + '_large', parity_large_exe,
        suite : ['fast', 'gpu'],
        is_parallel : false,
        timeout : 60)
endforeach

endif

# ADR-0338 / T8-1: Metal (Apple Silicon) backend scaffold smoke. Same
# pattern as the HIP / Vulkan scaffolds above — pins the -ENOSYS
# contract for every public C-API entry point until the runtime PR
# (T8-1b) lands. Built whenever the parent meson resolved
# is_metal_enabled to true (auto on macOS, or `enabled` explicitly).
# The Metal parity tests: test_metal_<name>_parity.c each (ADR-1496).
metal_parity_tests = [
  'float_adm', 'float_moment', 'float_motion', 'float_ms_ssim', 'float_psnr',
  'float_ssim', 'float_vif', 'integer_adm', 'integer_cambi', 'integer_ciede',
  'integer_motion', 'integer_psnr', 'integer_psnr_hvs', 'integer_ssim',
  'integer_vif', 'motion_v2', 'ssimulacra2', 'twin_option',
]
metal_test_opt = get_option('enable_metal')
if metal_test_opt.enabled() or (metal_test_opt.auto() and host_machine.system() == 'darwin')
test_metal_smoke = executable('test_metal_smoke',
    ['test.c', 'test_metal_smoke.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency],
    c_args: ['-DHAVE_METAL=1']
)
test('test_metal_smoke', test_metal_smoke,
    suite : ['fast', 'gpu'],
    is_parallel : false)

# ADR-0437 — install-gate: verify libvmaf_metal.h is reachable via the
# installed include path and that vmaf_metal_import_state + the IOSurface
# sub-API are declared with the correct prototypes (compile+link check only;
# no Metal runtime calls, so safe on runners without Apple-Family-7 devices).
# Gated to macOS only: the header requires Apple SDK types even in stub mode.
if host_machine.system() == 'darwin'
test_metal_install_header = executable('test_metal_install_header',
    ['test.c', 'test_metal_install_header.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency],
    c_args: ['-DHAVE_METAL=1']
)
test('test_metal_install_header', test_metal_install_header,
    suite : ['fast', 'gpu'],
    is_parallel : false)
endif

# Metal twin parity tests (ADR-0421 rounds 2 to 4, ADR-1496). Each compares
# a Metal twin with its CPU extractor on synthetic fixtures with `==` (the
# ciede twin at the LIBM_TWINS bound), runs every case after a failure too and
# prints one `@case <name> pass|fail|skip` line per case
# (core/test/metal_twin.h), which the macOS tester bundle reports per state
# row. Without a Metal device every case skips and the run exits 77. The same
# sources build as device-free self-tests below the Metal block.
foreach _m : metal_parity_tests
  _exe = executable('test_metal_' + _m + '_parity',
      ['test.c', 'test_metal_' + _m + '_parity.c'],
      include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
      link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
      dependencies: [pthread_dependency, math_lib],
      c_args: ['-DHAVE_METAL=1'])
  test('test_metal_' + _m + '_parity', _exe,
      suite : ['fast', 'gpu'],
      is_parallel : false,
      timeout : 900)
endforeach

# Metal kernel coverage round 4 (closeout) — meta-audit regression guard
# (ADR-0959). Enumerates the 8 .mm kernel basenames under
# core/src/feature/metal/ and asserts each has a registered
# `<basename>_metal` extractor plus a row in the dispatch strategy
# table. CPU-side registration probe runs on every host; the dispatch
# probe skips with -ENODEV on non-Apple-Family-7 runners. Coverage
# closeout for PR #351 (round 1) + PR #379 (round 2) + PR #447 (round
# 3).
test_metal_kernel_coverage_audit = executable('test_metal_kernel_coverage_audit',
    ['test.c', 'test_metal_kernel_coverage_audit.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency],
    c_args: ['-DHAVE_METAL=1']
)
test('test_metal_kernel_coverage_audit', test_metal_kernel_coverage_audit,
    suite : ['fast', 'gpu'],
    is_parallel : false)

# GPU-kernel coverage gap-fill — extractor-registration audit for the
# 8 Metal extractors (ADR-0361 T8-1c/d). Registration is compile-time
# so this test runs on Linux dev hosts too whenever the Metal build is
# enabled; the only ones the pre-existing test_metal_smoke spot-asserts
# are motion_v2_metal + float_psnr_metal.
test_metal_kernel_registration = executable('test_metal_kernel_registration',
    ['test.c', 'test_metal_kernel_registration.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency],
    c_args: ['-DHAVE_METAL=1']
)
test('test_metal_kernel_registration', test_metal_kernel_registration,
    suite : ['fast', 'gpu'],
    is_parallel : false)

endif

# ADR-1496: the Metal parity tests as self-tests on every host. The CPU
# extractor stands in for the twin (VMAF_METAL_TWIN_SELFTEST,
# core/test/metal_twin.h), so every `==` case compares the CPU with itself and
# a failure is a wrong fixture, key or option string in the test, which the
# tester's device run should not be the first to find.
foreach _m : metal_parity_tests
  _exe = executable('test_metal_selftest_' + _m,
      ['test.c', 'test_metal_' + _m + '_parity.c'],
      include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
      link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
      dependencies: [pthread_dependency, math_lib],
      c_args: ['-DVMAF_METAL_TWIN_SELFTEST'])
  test('test_metal_selftest_' + _m, _exe,
      suite : ['fast', 'metal-selftest'],
      timeout : 900)
endforeach

test('test_context', test_context, suite : ['fast'])
test('test_pool_percentile', test_pool_percentile, suite : ['fast'])
test('test_log', test_log, suite : ['fast'])
test('test_picture', test_picture, suite : ['fast'])
test('test_picture_v2', test_picture_v2, suite : ['fast'])
test('test_feature_collector', test_feature_collector, suite : ['fast'])
test('test_feature_backend_twin', test_feature_backend_twin, suite : ['fast'])

# BUG-048 section E: the collector has one C++ implementation authority.  This
# source/build-graph red-cap prevents the memory-safety C twin from being
# resurrected beside the C++ TU again.
test('test_feature_collector_source_authority',
    import('python').find_installation(),
    args : files('test_feature_collector_source_authority.py'),
    suite : ['fast'],
)
test('test_flush_context_ordering', test_flush_context_ordering, suite : ['fast'])
test('test_async_extractor_thread_pool', test_async_extractor_thread_pool, suite : ['fast'])

# The Windows thread shim defines a subset of pthread: a call outside it links
# on every POSIX host and fails on MSVC only (LNK2001).
test('test_win32_pthread_shim_contract',
    import('python').find_installation(),
    args : files('test_win32_pthread_shim_contract.py'),
    suite : ['fast'],
    timeout : 30,
)
test('test_output', test_output, suite : ['fast'])
test('test_public_api_score', test_public_api_score, suite : ['fast'])
test('test_thread_pool', test_thread_pool, suite : ['fast'])
if has_cond_timedwait
  test('test_thread_pool_backpressure', test_thread_pool_backpressure, suite : ['fast'])
endif
test('test_pdjson', test_pdjson, suite : ['fast'])
test('test_model', test_model, suite : ['fast'])
test('test_model_libsvm_dup_key', test_model_libsvm_dup_key, suite : ['fast'])
test('test_model_feature_overload_ownership', test_model_feature_overload_ownership, suite : ['fast'])
test('test_model_registration_ownership', test_model_registration_ownership, suite : ['fast'])
test('test_model_collection_api', test_model_collection_api, suite : ['fast'])
test('test_predict', test_predict, suite : ['fast'])
test('test_predict_nonfinite_log_output',
    import('python').find_installation(),
    args : [files('test_predict_nonfinite_log_output.py'), test_predict],
    suite : ['fast'],
    timeout : 30,
)
test('test_svm_api', test_svm_api, suite : ['fast'])
test('test_svm_parser', test_svm_parser, suite : ['fast'])
test('test_svm_multiclass', test_svm_multiclass, suite : ['fast'])
test('test_iqa_helpers', test_iqa_helpers, suite : ['fast'])
test('test_feature_extractor', test_feature_extractor, suite : ['fast'])
test('test_fex_ctx_vector', test_fex_ctx_vector, suite : ['fast'])
test('test_fex_ctx_vector_public', test_fex_ctx_vector_public, suite : ['fast'])
test('test_dict', test_dict, suite : ['fast'])
test('test_opt', test_opt, suite : ['fast'])
test('test_cpu', test_cpu, suite : ['fast'])

# An inline-assembly statement that clobbers a ymm or zmm register names the
# xmm register too: clang drops the wide register's clobber in a function not
# compiled for it, and a link-time-optimised build then zeroes a caller's
# value (vmaf_init_cpu()'s AVX-512 warm-up did). Reads the sources only;
# test_cpu runs the warm-up on a host with AVX-512.
test('test_inline_asm_clobber_contract',
    import('python').find_installation(),
    args : files('test_inline_asm_clobber_contract.py'),
    suite : ['fast'],
    timeout : 30,
)
test('test_gpu_dispatch_parse', test_gpu_dispatch_parse, suite : ['fast'])
test('test_ref', test_ref, suite : ['fast'])
test('test_feature', test_feature, suite : ['fast'])
test('test_ciede', test_ciede, suite : ['fast'])
test('test_delta_e_itp', test_delta_e_itp, suite : ['fast'])
test('test_pu21', test_pu21, suite : ['fast'])
test('test_pelorus_interop', test_pelorus_interop, suite : ['fast'])
test('test_lpips', test_lpips, suite : ['fast'])
test('test_dists', test_dists, suite : ['fast'])
test('test_fastdvdnet_pre', test_fastdvdnet_pre, suite : ['fast'])
test('test_mobilesal', test_mobilesal, suite : ['fast'])
test('test_transnet_v2', test_transnet_v2, suite : ['fast'])
test('test_cambi', test_cambi, suite : ['fast'])

if cambi_simd_test_archs.contains(host_machine.cpu_family())
test('test_cambi_simd', test_cambi_simd, suite : ['fast', 'simd'])
test('test_cambi_spatial_mask_simd', test_cambi_spatial_mask_simd, suite : ['fast', 'simd'])
test('test_cambi_stage_simd', test_cambi_stage_simd, suite : ['fast', 'simd'])
endif
if get_option('enable_float')
test('test_speed', test_speed, suite : ['fast'])
test('test_speed_frame_buffers', test_speed_frame_buffers, suite : ['fast'])
endif
test('test_speed_qa', test_speed_qa, suite : ['fast'])
test('test_luminance_tools', test_luminance_tools, suite : ['fast'])
test('test_cli_parse', test_cli_parse, suite : ['fast'])
test('test_spinner', test_spinner, suite : ['fast'])
if host_machine.system() != 'windows'
    test('test_y4m_411_oob', test_y4m_411_oob, suite : ['fast'])
    test('test_video_input_odd_dims', test_video_input_odd_dims, suite : ['fast'])
    test('test_cli_parse_long_only_args', test_cli_parse_long_only_args, suite : ['fast'])
    test('test_y4m_alloc_failure', test_y4m_alloc_failure, suite : ['fast'])
endif
test('test_psnr', test_psnr, suite : ['fast'])
test('test_integer_psnr_coverage', test_integer_psnr_coverage, suite : ['fast'])
test('test_psnr_uncapped', test_psnr_uncapped, suite : ['fast'])
test('test_integer_motion_coverage', test_integer_motion_coverage, suite : ['fast'])
test('test_integer_motion_v2_coverage', test_integer_motion_v2_coverage, suite : ['fast'])
test('test_ssim_coverage', test_ssim_coverage, suite : ['fast'])
test('test_adm_coverage', test_adm_coverage, suite : ['fast'])
test('test_adm_csf_representable', test_adm_csf_representable, suite : ['fast'])
test('test_integer_adm_cm_budget', test_integer_adm_cm_budget, suite : ['fast'])
test('test_psnr_hvs_coverage', test_psnr_hvs_coverage, suite : ['fast'])
test('test_psnr_hvs_score', test_psnr_hvs_score, suite : ['fast'])
test('test_ssimulacra2_coverage', test_ssimulacra2_coverage, suite : ['fast'])
test('test_niqe', test_niqe, suite : ['fast'])
test('test_brisque', test_brisque, suite : ['fast'])
test('test_y_funque_plus', test_y_funque_plus, suite : ['fast'])
test('test_integer_vif_log2', test_integer_vif_log2, suite : ['fast'])
test('test_iqa_convolve_coverage', test_iqa_convolve_coverage, suite : ['fast'])
test('test_barten_csf_coverage', test_barten_csf_coverage, suite : ['fast'])
test('test_ms_ssim_decimate_coverage', test_ms_ssim_decimate_coverage, suite : ['fast'])
test('test_pixel_format_edge_coverage',
    test_pixel_format_edge_coverage, suite : ['fast'])
test('test_framesync', test_framesync, suite : ['fast'])
test('test_framesync_init_failure', test_framesync_init_failure, suite : ['fast'])
test('test_propagate_metadata', test_propagate_metadata, suite : ['fast'])
test('test_adm_csf', test_adm_csf, suite : ['fast'])
test('test_barten_csf', test_barten_csf, suite : ['fast'])
test('test_float_adm_csf_upstream', test_float_adm_csf_upstream, suite : ['fast'])
test('test_float_adm_csf_upstream_contract',
    import('python').find_installation(),
    args : files('test_float_adm_csf_upstream_contract.py'),
    suite : ['fast'],
    timeout : 30,
)
if ms_ssim_decimate_test_archs.contains(host_machine.cpu_family())
test('test_ms_ssim_decimate', test_ms_ssim_decimate, suite : ['fast', 'simd'])
endif

if float_enabled and moment_simd_test_archs.contains(host_machine.cpu_family())
test('test_moment_simd', test_moment_simd, suite : ['fast', 'simd'])
endif

if float_enabled and speed_simd_test_archs.contains(host_machine.cpu_family())
test('test_speed_simd', test_speed_simd, suite : ['fast', 'simd'])
endif

# (test_ansnr_simd removed — ADR-0865 / ADR-0720)

if iqa_convolve_test_archs.contains(host_machine.cpu_family())
test('test_iqa_convolve', test_iqa_convolve, suite : ['fast', 'simd'])
endif

if psnr_hvs_avx2_test_archs.contains(host_machine.cpu_family())
test('test_psnr_hvs_avx2', test_psnr_hvs_avx2, suite : ['fast', 'simd'])
endif

if vif_simd_test_archs.contains(host_machine.cpu_family())
test('test_vif_simd', test_vif_simd, suite : ['fast', 'simd'])
endif

if psnr_hvs_simd_test_archs.contains(host_machine.cpu_family())
test('test_psnr_hvs_simd', test_psnr_hvs_simd, suite : ['fast', 'simd'])
endif

if integer_adm_decouple_test_archs.contains(host_machine.cpu_family())
test('test_integer_adm_simd', test_integer_adm_simd, suite : ['fast', 'simd'])
endif

if integer_adm_simd_test_archs.contains(host_machine.cpu_family())
test('test_adm_dwt2_x86', test_adm_dwt2_x86, suite : ['fast', 'simd'])
test('test_float_adm_x86', test_float_adm_x86, suite : ['fast', 'simd'])
endif

if integer_ssim_simd_test_archs.contains(host_machine.cpu_family())
test('test_integer_ssim_simd', test_integer_ssim_simd, suite : ['fast', 'simd'])
endif

if ciede_simd_parity_test_archs.contains(host_machine.cpu_family())
test('test_ciede_simd_parity', test_ciede_simd_parity, suite : ['fast', 'simd'])
endif

if motion_v2_simd_test_archs.contains(host_machine.cpu_family())
test('test_motion_v2_simd', test_motion_v2_simd, suite : ['fast', 'simd'])
endif

if motion_avx512_parity_test_archs.contains(host_machine.cpu_family()) and is_avx512_enabled
test('test_motion_avx512_parity', test_motion_avx512_parity, suite : ['fast', 'simd'])
endif


if psnr_hvs_neon_test_archs.contains(host_machine.cpu_family())
test('test_psnr_hvs_neon', test_psnr_hvs_neon, suite : ['fast', 'simd'])
test('test_vif_neon', test_vif_neon, suite : ['fast', 'simd'])
test('test_ssim_neon', test_ssim_neon, suite : ['fast', 'simd'])
test('test_float_adm_neon', test_float_adm_neon, suite : ['fast', 'simd'])
test('test_float_adm_dwt2_neon', test_float_adm_dwt2_neon, suite : ['fast', 'simd'])
test('test_motion_neon', test_motion_neon, suite : ['fast', 'simd'])
test('test_motion_pipeline_neon', test_motion_pipeline_neon, suite : ['fast', 'simd'])
test('test_float_motion_neon', test_float_motion_neon, suite : ['fast', 'simd'])
test('test_psnr_neon', test_psnr_neon, suite : ['fast', 'simd'])
test('test_ciede_neon', test_ciede_neon, suite : ['fast', 'simd'])
test('test_adm_dwt2_neon', test_adm_dwt2_neon, suite : ['fast', 'simd'])
endif

if ssimulacra2_simd_test_archs.contains(host_machine.cpu_family())
test('test_ssimulacra2_simd', test_ssimulacra2_simd, suite : ['fast', 'simd'])
endif

test('test_locale_handling', test_locale_handling, suite : ['fast'])

subdir('dnn')
subdir('fuzz')

# Only wired in when enable_rust_features=true (same gate as the extractor).
if is_rust_enabled
    test_tad_rust = executable('test_tad_rust',
        ['test.c', 'test_tad_rust.c'],
        include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
        link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    )
    test('test_tad_rust', test_tad_rust, suite : ['fast'])
endif

# Coverage round 3 — core/src/feature/integer_motion.h `edge_16` mirror
# extension (5-tap filter, both axes).  Pushes line coverage from 58 %
# towards 100 % by driving the four mirror branches directly.
test_integer_motion_edge16_coverage = executable('test_integer_motion_edge16_coverage',
    ['test.c', 'test_integer_motion_edge16_coverage.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies : [pthread_dependency, sycl_dependency],
)
test('test_integer_motion_edge16_coverage', test_integer_motion_edge16_coverage, suite : ['fast'])

# Coverage round 3 — core/src/feature/adm_csf_tools.h `adm_native_csf`
# CSF curve (theta=0 H/V branch + theta=45 diagonal branch).  Drives
# the inline body directly with a closed-form reference cross-check.
test_adm_csf_tools_coverage = executable('test_adm_csf_tools_coverage',
    ['test.c', 'test_adm_csf_tools_coverage.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies : [math_lib, pthread_dependency, sycl_dependency],
)
test('test_adm_csf_tools_coverage', test_adm_csf_tools_coverage, suite : ['fast'])

# Coverage round 3 — core/src/feature/feature_collector.cpp branch
# coverage push (62.5 % -> 80 %+).  Hits NULL guards, duplicate-key
# rejects, and unmount-not-found paths via the internal header.
# feature_collector.cpp is compiled directly into this test so the
# non-static internal helpers (aggregate_vector_*, feature_vector_*)
# declared in feature_collector_internal.h are resolvable at link time.
# The archive member is not extracted when these direct definitions already
# satisfy the collector references — see test_predict for the same pattern.
test_feature_collector_coverage = executable('test_feature_collector_coverage',
    ['test.c', 'test_feature_collector_coverage.c',
     '../src/feature/feature_collector.cpp'],
    include_directories : [libvmaf_inc, test_inc,
                           include_directories('../src/feature/'),
                           include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies : [pthread_dependency, sycl_dependency],
)
test('test_feature_collector_coverage', test_feature_collector_coverage, suite : ['fast'])

# Coverage push: core/src/feature/mkdirp.c had 0/35 line coverage in the
# 2026-05-30 Coverage Gate baseline because the only in-tree caller is
# cambi.c's `heatmaps_path` sidecar, which no unit test exercises.  This
# binary drives the public `mkdirp` symbol directly with NULL guard,
# single-level mkdir, EEXIST idempotence, and double-slash normalization.
# Compiles `mkdirp.cpp` and its UTF-8 path dependency into the binary directly.
# The file is part of
# libvmaf_feature_static_lib so linking against libvmaf would also work,
# but a TU-local compile keeps the test self-contained and avoids
# coupling to libvmaf's link graph.
test_mkdirp = executable('test_mkdirp',
    ['test.c', 'test_mkdirp.c', '../src/feature/mkdirp.cpp', '../src/compat/path_utf8.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    dependencies: [pthread_dependency, sycl_dependency],
)
test('test_mkdirp', test_mkdirp, suite : ['fast'])

# Research-2045: configured private integer-VIF objects also work in shared builds.
if is_asm_enabled and host_machine.cpu_family().startswith('x86')
test_integer_vif_avx2_stages = executable('test_integer_vif_avx2_stages',
    # dnn_sources pulled in directly so feature_lpips.c can resolve its
    # libvmaf/dnn.h references (session_open/run/close,
    # vmaf_tensor_from_rgb_imagenet) without linking against libvmaf.so.
    ['test.c', 'test_integer_vif_avx2_stages.c', '../src/mem.cpp', '../src/picture.c', '../src/ref.cpp',
     '../src/dict.cpp',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp',
     dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib, predict_test_dependencies,
                    dnn_deps],
    objects : [
      common_cuda_objects,
      platform_specific_cpu_objects,
      libvmaf_feature_static_lib.extract_all_objects(recursive: true),
      log_cpp23_test_objects,
      libvmaf_cpu_static_lib.extract_all_objects(recursive: true),
      libsvm_static_lib.extract_all_objects(recursive: true),
    ] + wave8_opt_only_objects
)
# The sweep behind these two tests is 12 widths x 4 heights x 9 bit depths
# x 4 scales x 2 patterns = 3456 scalar/SIMD kernel pairs, each followed by a
# full-fixture memcmp. That is cheap in an optimised build (~2 s on a modern
# desktop) but the Coverage Gate runs it under --buildtype=debug -Db_coverage=true
# with -fprofile-update=atomic, where every basic block carries a locked counter
# increment. On the slower GitHub hosted runner classes that pushes both tests
# past Meson's 30 s default: master runs 35499436272 (both TIMEOUT) and
# 35505304660 (29.94 s / TIMEOUT) failed the gate on exactly this. Give them a
# budget that fits the instrumented build instead of trimming the sweep -- the
# exhaustive comparison is the bit-exactness guarantee itself.
test('test_integer_vif_avx2_stages', test_integer_vif_avx2_stages,
    suite : ['fast', 'simd'], timeout : 180)
endif

# Research-2046: configured private integer-VIF objects also work in shared builds.
if is_avx512_enabled and host_machine.cpu_family().startswith('x86')
test_integer_vif_avx512_stages = executable('test_integer_vif_avx512_stages',
    # dnn_sources pulled in directly so feature_lpips.c can resolve its
    # libvmaf/dnn.h references (session_open/run/close,
    # vmaf_tensor_from_rgb_imagenet) without linking against libvmaf.so.
    ['test.c', 'test_integer_vif_avx512_stages.c', '../src/mem.cpp', '../src/picture.c', '../src/ref.cpp',
     '../src/dict.cpp',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp',
     dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib, predict_test_dependencies,
                    dnn_deps],
    objects : [
      common_cuda_objects,
      platform_specific_cpu_objects,
      libvmaf_feature_static_lib.extract_all_objects(recursive: true),
      log_cpp23_test_objects,
      libvmaf_cpu_static_lib.extract_all_objects(recursive: true),
      libsvm_static_lib.extract_all_objects(recursive: true),
    ] + wave8_opt_only_objects
)
# The sweep behind these two tests is 12 widths x 4 heights x 9 bit depths
# x 4 scales x 2 patterns = 3456 scalar/SIMD kernel pairs, each followed by a
# full-fixture memcmp. That is cheap in an optimised build (~2 s on a modern
# desktop) but the Coverage Gate runs it under --buildtype=debug -Db_coverage=true
# with -fprofile-update=atomic, where every basic block carries a locked counter
# increment. On the slower GitHub hosted runner classes that pushes both tests
# past Meson's 30 s default: master runs 35499436272 (both TIMEOUT) and
# 35505304660 (29.94 s / TIMEOUT) failed the gate on exactly this. Give them a
# budget that fits the instrumented build instead of trimming the sweep -- the
# exhaustive comparison is the bit-exactness guarantee itself.
test('test_integer_vif_avx512_stages', test_integer_vif_avx512_stages,
    suite : ['fast', 'simd'], timeout : 180)
endif
