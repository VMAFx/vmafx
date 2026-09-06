if not get_option('enable_tests')
    subdir_done()
endif

# ADR-0707: rust_tad_dep is available here (defined in src/meson.build via
# subdir('src') before subdir('test')). Only test_tad_rust uses it directly —
# other tests link against libvmaf.so (shared) which already carries the Rust
# archive, so no further propagation is needed.

test_inc = include_directories('.')

# Aggregate of all GPU/compute backend dependency objects so test
# binaries that must link against ANY enabled backend can reference
# one variable instead of repeating the per-backend list.
gpu_all_deps = [
  cuda_dependency,
  sycl_dependency,
  hip_deps,
  metal_deps,
]

libvmaf_public_link = get_option('default_library') == 'both' ? libvmaf.get_shared_lib() : libvmaf
test_output_lto_override = host_machine.system() == 'darwin' ? ['b_lto=false'] : []

# ADR-0708 Wave 1: log is compiled in log_cpp23_lib (cpp_std=c++23 override).
# Every test executable that previously direct-compiled '../src/log.c' must
# now pick up the log symbols via this object aggregate so that
# vmaf_log / vmaf_set_log_level resolve at link time.
log_cpp23_test_objects = log_cpp23_lib.extract_all_objects(recursive: true)
# ADR-0761: opt.cpp and predict.cpp are compiled as isolated C++23 static libs
# in src/meson.build. Tests that previously listed these files as direct sources
# now pull them in via extract_all_objects() so the compilation honours
# cpp_std=c++23 instead of the project-wide c++11 default.
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
# Intel icx (meson id 'intel-llvm') additionally ignores
# #pragma STDC FP_CONTRACT OFF unless -fp-model=precise is also on the
# command line, so we detect icx and append that flag. See ADR-0161.
_cc_id = cc.get_id()
_simd_strict_fp_args = ['-ffp-contract=off']
if _cc_id == 'intel-llvm' or _cc_id == 'intel-llvm-cl'
    _simd_strict_fp_args += ['-fp-model=precise']
endif

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

test_context = executable('test_context',
    ['test.c', 'test_context.c'],
    include_directories : [libvmaf_inc, test_inc],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies:[stdatomic_dependency, pthread_dependency, gpu_all_deps],
)

test_picture = executable('test_picture',
    ['test.c', 'test_picture.c', '../src/picture.c', '../src/mem.cpp', '../src/ref.cpp', '../src/thread_pool.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    dependencies:[stdatomic_dependency, pthread_dependency, thread_lib, gpu_all_deps],
)

# ADR-0928 cycle N+1: picture_v2 unit tests
test_picture_v2 = executable('test_picture_v2',
    ['test.c', 'test_picture_v2.c',
     '../src/picture.c', '../src/picture_v2.c',
     '../src/mem.cpp', '../src/ref.cpp', '../src/thread_pool.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    dependencies:[stdatomic_dependency, pthread_dependency, thread_lib, gpu_all_deps],
)

test_propagate_metadata = executable('test_propagate_metadata',
    ['test.c', 'test_propagate_metadata.c', '../src/metadata_handler.cpp'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    dependencies: [pthread_dependency, sycl_dependency],
)

test_feature_collector = executable('test_feature_collector',
    ['test.c', 'test_feature_collector.c', '../src/predict.c',
     '../src/metadata_handler.cpp', rev_target],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/feature/'), include_directories('../src')],
    # ADR-0708 Wave 1: log is now compiled in log_cpp23_lib; link rather than
    # direct-compile the source.
    link_with : [get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
                 log_cpp23_lib],
    dependencies: [pthread_dependency, gpu_all_deps],
    objects : [libsvm_static_lib.extract_all_objects(recursive: true)] + wave8_cpp23_objects,
)

# Finding R2-10: flush_context terminal-flush ordering. White-box test that
# includes libvmaf.c directly (same pattern as test_feature_collector) so it can
# call the static flush_context_threaded() / flush_context() helpers and assert
# the inner threaded flush no longer sets vmaf->flushed prematurely — the bug
# that let a CUDA-flush error skip flush_context_sycl with no retry possible.
test_flush_context_ordering = executable('test_flush_context_ordering',
    ['test.c', 'test_flush_context_ordering.c', '../src/predict.c',
     '../src/metadata_handler.cpp', rev_target],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/feature/'), include_directories('../src')],
    link_with : [get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
                 log_cpp23_lib],
    dependencies: [math_lib, pthread_dependency, gpu_all_deps],
    objects : [libsvm_static_lib.extract_all_objects(recursive: true)] + wave8_cpp23_objects,
)

# Coverage follow-up to PR #313 (R3 from
# docs/development/coverage-gap-analysis-2026-05-02.md): exercise
# libvmaf/src/output.c writers (XML / JSON / CSV / SUB) end-to-end.
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
# Addresses .workingdir/audit-test-coverage-2026-05-16.md §2.
test_public_api_score = executable('test_public_api_score',
    ['test.c', 'test_public_api_score.c'],
    include_directories : [libvmaf_inc, test_inc],
    link_with : libvmaf_public_link,
    dependencies : [pthread_dependency, thread_lib, gpu_all_deps],
)

test_thread_pool = executable('test_thread_pool',
    ['test.c', 'test_thread_pool.c', '../src/thread_pool.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    dependencies : [pthread_dependency, thread_lib, sycl_dependency],
)

test_model = executable('test_model',
    ['test.c', 'test_model.c', '../src/dict.cpp', '../src/pdjson.c', json_model_c_sources],
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
    ['test.c', 'test_model_libsvm_dup_key.c', '../src/dict.cpp', '../src/pdjson.c',
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
    ['test.c', 'test_model_feature_overload_ownership.c', '../src/dict.cpp', '../src/pdjson.c', json_model_c_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    c_args : [vmaf_cflags_common, '-DJSON_MODEL_PATH="'+join_paths(meson.project_source_root(), '../model/')+'"'],
    dependencies : [pthread_dependency, thread_lib, gpu_all_deps],
    objects : [libsvm_static_lib.extract_all_objects(recursive: true),
               read_json_model_cpp23_lib.extract_all_objects(recursive: true)],
)

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
     '../src/pdjson.c', json_model_c_sources, '../src/feature/feature_name.cpp', '../src/feature/feature_extractor.cpp',],    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
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
     '../src/dict.cpp', '../src/predict.c',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp', dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'),
                           include_directories('../src/feature/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib,
                    gpu_all_deps, dnn_deps],
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
     '../src/dict.cpp', '../src/predict.c',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp',
     '../src/fex_ctx_vector.cpp',
     dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib, gpu_all_deps,
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
    ['test.c', 'test_feature.cpp', '../src/feature/alias.c', '../src/dict.cpp'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    dependencies: [pthread_dependency, sycl_dependency],
)

test_ciede = executable('test_ciede',
    ['test.c', 'test_ciede.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, gpu_all_deps],
)

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
     '../src/dict.cpp', '../src/predict.c',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp', dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib, gpu_all_deps,
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
     '../src/dict.cpp', '../src/predict.c',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp', dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib, gpu_all_deps,
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
     '../src/dict.cpp', '../src/predict.c',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp', dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib, gpu_all_deps,
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
     '../src/dict.cpp', '../src/predict.c',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp', dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib, gpu_all_deps,
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
     '../src/dict.cpp', '../src/predict.c',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp', dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib, gpu_all_deps,
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
    ['test.c', 'test_cambi.c', '../src/picture.c', '../src/mem.cpp', '../src/ref.cpp'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, gpu_all_deps],
)

cambi_simd_test_archs = ['x86_64', 'x86', 'aarch64', 'arm64']
if cambi_simd_test_archs.contains(host_machine.cpu_family())
test_cambi_simd = executable('test_cambi_simd',
    ['test.c', 'test_cambi_simd.c',
     '../src/mem.cpp', '../src/picture.c', '../src/ref.cpp',
     '../src/dict.cpp', '../src/predict.c',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp', dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'),
                           include_directories('../src/feature/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib,
                    gpu_all_deps, dnn_deps],
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
# Both extractors live in libvmaf/src/feature/speed.c, which is only
# compiled when -Denable_float=true (see libvmaf/src/meson.build); the
# corresponding entries in feature_extractor.c's extractor list are
# wrapped in `#if VMAF_FLOAT_FEATURES`. Gate the test on the same option
# so a default `meson setup` (enable_float=false) doesn't fail on a
# config that legitimately omits these extractors.
if get_option('enable_float')
test_speed = executable('test_speed',
    ['test.c', 'test_speed.c', '../src/mem.cpp', '../src/picture.c', '../src/ref.cpp',
     '../src/dict.cpp', '../src/predict.c',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp', dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib, gpu_all_deps,
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
endif
# SpEED-QA extractor tests -- unconditional (speed_qa.c is not float-gated).
# ADR-0253 real-impl: registration, vtable, flat/noise entropy, temporal positivity.
test_speed_qa = executable('test_speed_qa',
    ['test.c', 'test_speed_qa.c', '../src/mem.cpp', '../src/picture.c', '../src/ref.cpp',
     '../src/dict.cpp', '../src/predict.c',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp', dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib, gpu_all_deps,
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
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, sycl_dependency],
)

test_cli_parse = executable('test_cli_parse',
    ['test.c', 'test_cli_parse.c', '../tools/cli_parse.cpp'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'), include_directories('../tools/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    c_args : [compat_cflags],
    cpp_args : [compat_cflags],
    # libvmaf_cpu_cpp_std (core/src/meson.build): 'vc++latest' on MSVC,
    # 'c++latest' on clang-cl / icx-cl, 'c++23' elsewhere. The plain 'c++23'
    # spelling is REJECTED by MSVC's meson and fails configure on all three
    # Windows MSVC legs with "None of values ['c++23'] are supported by the
    # CPP compiler" — the same trap ADR-0755 / ADR-0860 documents for cpu.cpp.
    override_options : ['cpp_std=' + libvmaf_cpu_cpp_std],
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
        # libvmaf_cpu_cpp_std (core/src/meson.build): 'vc++latest' on MSVC,
    # 'c++latest' on clang-cl / icx-cl, 'c++23' elsewhere. The plain 'c++23'
    # spelling is REJECTED by MSVC's meson and fails configure on all three
    # Windows MSVC legs with "None of values ['c++23'] are supported by the
    # CPP compiler" — the same trap ADR-0755 / ADR-0860 documents for cpu.cpp.
    override_options : ['cpp_std=' + libvmaf_cpu_cpp_std],
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
     '../src/dict.cpp', '../src/predict.c',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp',
     dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib, gpu_all_deps,
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
     '../src/dict.cpp', '../src/predict.c',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp',
     dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib, gpu_all_deps,
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
     '../src/dict.cpp', '../src/predict.c',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp',
     dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib, gpu_all_deps,
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
     '../src/ref.cpp', '../src/dict.cpp', '../src/predict.c',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp',
     dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib, gpu_all_deps,
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
     '../src/dict.cpp', '../src/predict.c',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp',
     dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib, gpu_all_deps,
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
     '../src/dict.cpp', '../src/predict.c',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp',
     dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib, gpu_all_deps,
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

# ADR-1191 — integer-ADM CSF fixed-point representability guard: the
# configurations init() must reject (adm_csf_mode=1 at the default
# adm_csf_scale; a blend mode at an untabulated viewing geometry) and the
# ones it must keep accepting (default, mode 1 with the fork's small scale
# coefficients, modes 2 and 3 at 1080@3H).
test_adm_csf_representable = executable('test_adm_csf_representable',
    ['test.c', 'test_adm_csf_representable.c', '../src/mem.cpp', '../src/picture.c',
     '../src/ref.cpp', '../src/dict.cpp', '../src/predict.c',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp',
     dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib, gpu_all_deps,
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

# Coverage round 3 — psnr_hvs.c: bpc>12 rejection, YUV400P chroma-disable,
# enable_chroma=false option, and 10-bit HBD path.
test_psnr_hvs_coverage = executable('test_psnr_hvs_coverage',
    ['test.c', 'test_psnr_hvs_coverage.c', '../src/mem.cpp', '../src/picture.c', '../src/ref.cpp',
     '../src/dict.cpp', '../src/predict.c',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp',
     dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib, gpu_all_deps,
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
     '../src/dict.cpp', '../src/predict.c',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp',
     dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib, gpu_all_deps,
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

# NIQE (no-reference, ADR-1112): niqe_math.h unit oracles + an end-to-end
# score oracle driven through the public extractor-context API against frame 0
# of the natural fixture testdata/ref_576x324_48f.yuv. NIQE_TESTDATA_DIR points
# the test at the in-tree YUV (mirrors the JSON_MODEL_PATH precedent in
# test_model). Reuses the libvmaf_feature_static_lib object-extraction pattern
# from test_ssimulacra2_coverage so the niqe extractor + collector are linked.
test_niqe = executable('test_niqe',
    ['test.c', 'test_niqe.c', '../src/mem.cpp', '../src/picture.c', '../src/ref.cpp',
     '../src/dict.cpp', '../src/predict.c',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp',
     dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines +
             ['-DNIQE_TESTDATA_DIR="' + join_paths(meson.project_source_root(), '../testdata') + '"'],
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib, gpu_all_deps,
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
     '../src/dict.cpp', '../src/predict.c',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp',
     dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines +
             ['-DBRISQUE_TESTDATA_DIR="' + join_paths(meson.project_source_root(), '../testdata') + '"'],
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib, gpu_all_deps,
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
     '../src/dict.cpp', '../src/predict.c',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp',
     dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib, gpu_all_deps,
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
     '../src/dict.cpp', '../src/predict.c',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp', dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'),
                           include_directories('../src/feature/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib,
                    gpu_all_deps, dnn_deps],
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
     '../src/dict.cpp', '../src/predict.c',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp', dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'),
                           include_directories('../src/feature/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib,
                    gpu_all_deps, dnn_deps],
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
     '../src/dict.cpp', '../src/predict.c',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp', dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'),
                           include_directories('../src/feature/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib,
                    gpu_all_deps, dnn_deps],
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
     '../src/dict.cpp', '../src/predict.c',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp', dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'),
                           include_directories('../src/feature/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines + _simd_strict_fp_args,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib,
                    gpu_all_deps, dnn_deps],
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

integer_adm_simd_test_archs = ['x86_64', 'x86']
if integer_adm_simd_test_archs.contains(host_machine.cpu_family())
test_integer_adm_simd = executable('test_integer_adm_simd',
    ['test.c', 'test_integer_adm_simd.c',
     '../src/mem.cpp', '../src/picture.c', '../src/ref.cpp',
     '../src/dict.cpp', '../src/predict.c',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp', dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'),
                           include_directories('../src/feature/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib,
                    gpu_all_deps, dnn_deps],
    objects : [
      common_cuda_objects,
      platform_specific_cpu_objects,
      libvmaf_cpu_static_lib.extract_all_objects(recursive: true),
      libvmaf_feature_static_lib.extract_all_objects(recursive: true),
      log_cpp23_test_objects,
      libsvm_static_lib.extract_all_objects(recursive: true),
    ] + wave8_opt_only_objects,
)

test_adm_dwt2_x86 = executable('test_adm_dwt2_x86',
    ['test.c', 'test_adm_dwt2_x86.c',
     '../src/mem.cpp', '../src/picture.c', '../src/ref.cpp',
     '../src/dict.cpp', '../src/predict.c',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp', dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'),
                           include_directories('../src/feature/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib,
                    gpu_all_deps, dnn_deps],
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
     '../src/dict.cpp', '../src/predict.c',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp', dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'),
                           include_directories('../src/feature/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib,
                    gpu_all_deps, dnn_deps],
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

ciede_simd_parity_test_archs = ['x86_64', 'x86']
if ciede_simd_parity_test_archs.contains(host_machine.cpu_family())
test_ciede_simd_parity = executable('test_ciede_simd_parity',
    ['test.c', 'test_ciede_simd_parity.c',
     '../src/mem.cpp', '../src/picture.c', '../src/ref.cpp',
     '../src/dict.cpp', '../src/predict.c',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp', dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'),
                           include_directories('../src/feature/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib,
                    gpu_all_deps, dnn_deps],
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
     '../src/dict.cpp', '../src/predict.c',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp', dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'),
                           include_directories('../src/feature/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib,
                    gpu_all_deps, dnn_deps],
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
     '../src/dict.cpp', '../src/predict.c',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp', dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'),
                           include_directories('../src/feature/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib,
                    gpu_all_deps, dnn_deps],
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
# _simd_strict_fp_args = [-ffp-contract=off] on GCC/Clang;
#                        [-ffp-contract=off, -fp-model=precise] on icx.
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
test_ssimulacra2_simd = executable('test_ssimulacra2_simd',
    ['test.c', 'test_ssimulacra2_simd.c',
     '../src/mem.cpp', '../src/picture.c', '../src/ref.cpp',
     '../src/dict.cpp', '../src/predict.c',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp', dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'),
                           include_directories('../src/feature/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines + _simd_strict_fp_args + _ssimulacra2_test_x86_fma_args,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib,
                    gpu_all_deps, dnn_deps],
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
     '../src/dict.cpp', '../src/predict.c',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp', dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'),
                           include_directories('../src/feature/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib,
                    gpu_all_deps, dnn_deps],
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
     '../src/dict.cpp', '../src/predict.c',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp', dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'),
                           include_directories('../src/feature/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib,
                    gpu_all_deps, dnn_deps],
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
     '../src/dict.cpp', '../src/predict.c',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp', dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'),
                           include_directories('../src/feature/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib,
                    gpu_all_deps, dnn_deps],
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
     '../src/dict.cpp', '../src/predict.c',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp', dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'),
                           include_directories('../src/feature/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines + _simd_strict_fp_args,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib,
                    gpu_all_deps, dnn_deps],
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
     '../src/dict.cpp', '../src/predict.c',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp', dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'),
                           include_directories('../src/feature/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines + _simd_strict_fp_args,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib,
                    gpu_all_deps, dnn_deps],
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
     '../src/dict.cpp', '../src/predict.c',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp', dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'),
                           include_directories('../src/feature/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib,
                    gpu_all_deps, dnn_deps],
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
     '../src/dict.cpp', '../src/predict.c',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp', dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'),
                           include_directories('../src/feature/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib,
                    gpu_all_deps, dnn_deps],
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
     '../src/dict.cpp', '../src/predict.c',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp', dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'),
                           include_directories('../src/feature/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib,
                    gpu_all_deps, dnn_deps],
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
     '../src/dict.cpp', '../src/predict.c',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp', dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'),
                           include_directories('../src/feature/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib,
                    gpu_all_deps, dnn_deps],
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
     '../src/dict.cpp', '../src/predict.c',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp', dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'),
                           include_directories('../src/feature/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib,
                    gpu_all_deps, dnn_deps],
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
     '../src/dict.cpp', '../src/predict.c',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp', dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'),
                           include_directories('../src/feature/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib,
                    gpu_all_deps, dnn_deps],
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

# Speed_chroma covariance-sum SIMD parity (AVX2 + AVX-512, upstream 30f472b14).
# Gated on float_enabled (speed.c is float-only) and x86/x86_64 (no NEON path yet).
speed_simd_test_archs = ['x86_64', 'x86']
if float_enabled and speed_simd_test_archs.contains(host_machine.cpu_family())
test_speed_simd = executable('test_speed_simd',
    ['test.c', 'test_speed_simd.c',
     '../src/mem.cpp', '../src/picture.c', '../src/ref.cpp',
     '../src/dict.cpp', '../src/predict.c',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp', dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'),
                           include_directories('../src/feature/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib,
                    gpu_all_deps, dnn_deps],
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
     '../src/dict.cpp', '../src/predict.c',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp', dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'),
                           include_directories('../src/feature/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines + _simd_strict_fp_args,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib,
                    gpu_all_deps, dnn_deps],
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

test('test_gpu_picture_pool', test_gpu_picture_pool, suite : ['fast', 'gpu'])
test('test_cuda_pic_preallocation', test_cuda_pic_preallocation, suite : ['fast', 'gpu'])
test('test_cuda_drain_batch', test_cuda_drain_batch, suite : ['fast'])

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
test('test_cuda_buffer_alloc_oom', test_cuda_buffer_alloc_oom, suite : ['fast', 'gpu'])

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
    suite : ['fast', 'gpu'])

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
test('test_cuda_preallocation_leak', test_cuda_preallocation_leak, suite : ['fast', 'gpu'])

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
    suite : ['fast', 'gpu'])

# ADR-0541 — integer_vif CPU vs. CUDA parity gate.  Pins the luma-only
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
    suite : ['fast', 'gpu'])

# ADR-0956 — CUDA kernel parity coverage round 4. Closes the last
# three float-path CPU twins (`float_adm_cuda`, `float_motion_cuda`,
# `float_ssim_cuda`) with cross-backend parity assertions at
# `places=4` / 1e-4 (ADR-0214). Each follows the
# `test_cuda_motion3_parity.c` template: 256x144 YUV420P 8-bpc
# synthetic fixture, 3 frames, score at frame index 1,
# `[skip: no CUDA device]` guard when vmaf_cuda_state_init fails.
test_cuda_float_adm_parity = executable('test_cuda_float_adm_parity',
    ['test.c', 'test_cuda_float_adm_parity.c'],
    include_directories : [libvmaf_inc, test_inc],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, cuda_dependency, math_lib],
    c_args: ['-DHAVE_CUDA=1']
)
test('test_cuda_float_adm_parity', test_cuda_float_adm_parity,
    suite : ['fast', 'gpu'])

test_cuda_float_motion_parity = executable('test_cuda_float_motion_parity',
    ['test.c', 'test_cuda_float_motion_parity.c'],
    include_directories : [libvmaf_inc, test_inc],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, cuda_dependency, math_lib],
    c_args: ['-DHAVE_CUDA=1']
)
test('test_cuda_float_motion_parity', test_cuda_float_motion_parity,
    suite : ['fast', 'gpu'])

test_cuda_float_ssim_parity = executable('test_cuda_float_ssim_parity',
    ['test.c', 'test_cuda_float_ssim_parity.c'],
    include_directories : [libvmaf_inc, test_inc],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, cuda_dependency, math_lib],
    c_args: ['-DHAVE_CUDA=1']
)
test('test_cuda_float_ssim_parity', test_cuda_float_ssim_parity,
    suite : ['fast', 'gpu'])

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
    suite : ['fast', 'gpu'])

test_cuda_speed_temporal_smoke = executable('test_cuda_speed_temporal_smoke',
    ['test.c', 'test_cuda_speed_temporal_smoke.c'],
    include_directories : [libvmaf_inc, test_inc],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, cuda_dependency, math_lib],
    c_args: ['-DHAVE_CUDA=1']
)
test('test_cuda_speed_temporal_smoke', test_cuda_speed_temporal_smoke,
    suite : ['fast', 'gpu'])

# ADR-0965 — speed_chroma + speed_temporal CPU vs. CUDA parity gates.
# Pin the cross-backend tolerance (places=4) for the CUDA SpEED twins wired
# in this repair PR.  Both tests skip cleanly when no CUDA device is visible
# (mirrors test_cuda_motion3_parity.c).
test_cuda_speed_chroma_parity = executable('test_cuda_speed_chroma_parity',
    ['test.c', 'test_cuda_speed_chroma_parity.c'],
    include_directories : [libvmaf_inc, test_inc],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, cuda_dependency, math_lib],
    c_args: ['-DHAVE_CUDA=1']
)
test('test_cuda_speed_chroma_parity', test_cuda_speed_chroma_parity,
    suite : ['fast', 'gpu'])

test_cuda_speed_temporal_parity = executable('test_cuda_speed_temporal_parity',
    ['test.c', 'test_cuda_speed_temporal_parity.c'],
    include_directories : [libvmaf_inc, test_inc],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, cuda_dependency, math_lib],
    c_args: ['-DHAVE_CUDA=1']
)
test('test_cuda_speed_temporal_parity', test_cuda_speed_temporal_parity,
    suite : ['fast', 'gpu'])

# ADR-0947 — CUDA kernel parity coverage, round 3. Closes the float-path
# twin gap (float_psnr, float_vif, float_ms_ssim, float_moment) plus the
# new ssimulacra2_cuda kernel.  Each test asserts CPU-vs-CUDA agreement
# at places=4 / 1e-4 (ADR-0214 cross-backend gate) on a 256x144 YUV420P
# fixture, 3 frames, score read at index 1.  GPU-gated: skips cleanly
# when no CUDA device is visible (mirrors test_cuda_motion3_parity).
test_cuda_float_psnr_parity = executable('test_cuda_float_psnr_parity',
    ['test.c', 'test_cuda_float_psnr_parity.c'],
    include_directories : [libvmaf_inc, test_inc],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, cuda_dependency, math_lib],
    c_args: ['-DHAVE_CUDA=1']
)
test('test_cuda_float_psnr_parity', test_cuda_float_psnr_parity,
    suite : ['fast', 'gpu'])

test_cuda_float_vif_parity = executable('test_cuda_float_vif_parity',
    ['test.c', 'test_cuda_float_vif_parity.c'],
    include_directories : [libvmaf_inc, test_inc],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, cuda_dependency, math_lib],
    c_args: ['-DHAVE_CUDA=1']
)
test('test_cuda_float_vif_parity', test_cuda_float_vif_parity,
    suite : ['fast', 'gpu'])

test_cuda_float_ms_ssim_parity = executable('test_cuda_float_ms_ssim_parity',
    ['test.c', 'test_cuda_float_ms_ssim_parity.c'],
    include_directories : [libvmaf_inc, test_inc],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, cuda_dependency, math_lib],
    c_args: ['-DHAVE_CUDA=1']
)
test('test_cuda_float_ms_ssim_parity', test_cuda_float_ms_ssim_parity,
    suite : ['fast', 'gpu'])

test_cuda_float_moment_parity = executable('test_cuda_float_moment_parity',
    ['test.c', 'test_cuda_float_moment_parity.c'],
    include_directories : [libvmaf_inc, test_inc],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, cuda_dependency, math_lib],
    c_args: ['-DHAVE_CUDA=1']
)
test('test_cuda_float_moment_parity', test_cuda_float_moment_parity,
    suite : ['fast', 'gpu'])

test_cuda_ssimulacra2_parity = executable('test_cuda_ssimulacra2_parity',
    ['test.c', 'test_cuda_ssimulacra2_parity.c'],
    include_directories : [libvmaf_inc, test_inc],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, cuda_dependency, math_lib],
    c_args: ['-DHAVE_CUDA=1']
)
test('test_cuda_ssimulacra2_parity', test_cuda_ssimulacra2_parity,
    suite : ['fast', 'gpu'])

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
    suite : ['fast', 'gpu'])

# ADR-1167 — ADM small-frame border parity test (i == 0 && top <= 0 branch).
test_cuda_adm_small_border = executable('test_cuda_adm_small_border',
    ['test.c', 'test_adm_small_border.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, cuda_dependency, math_lib],
    c_args: ['-DHAVE_CUDA=1']
)
test('test_cuda_adm_small_border', test_cuda_adm_small_border,
    suite : ['fast', 'gpu'])

# ADR-1167 — ADM wide-frame row-level rounding parity test (spans 60 warps).
test_cuda_adm_wide_rounding = executable('test_cuda_adm_wide_rounding',
    ['test.c', 'test_adm_wide_rounding.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, cuda_dependency, math_lib],
    c_args: ['-DHAVE_CUDA=1']
)
test('test_cuda_adm_wide_rounding', test_cuda_adm_wide_rounding,
    suite : ['fast', 'gpu'])

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
    suite : ['fast', 'gpu'])

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
    suite : ['fast', 'gpu'])

# Round-2 GPU-kernel coverage gap-fill — psnr_hvs CPU vs. CUDA parity
# (places=4). Covers the HVS contrast-sensitivity-weighted DCT path
# (third_party/xiph/psnr_hvs.c vs. integer_psnr_hvs/psnr_hvs_score.cu).
test_cuda_psnr_hvs_parity = executable('test_cuda_psnr_hvs_parity',
    ['test.c', 'test_cuda_psnr_hvs_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, cuda_dependency, math_lib],
    c_args: ['-DHAVE_CUDA=1']
)
test('test_cuda_psnr_hvs_parity', test_cuda_psnr_hvs_parity,
    suite : ['fast', 'gpu'])

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
    suite : ['fast', 'gpu'])

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
    suite : ['fast', 'gpu'])

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
    suite : ['fast', 'gpu'])

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
        suite : ['fast', 'gpu'])
endforeach

endif

# T6-2b / ADR-0247 — vmaf-roi sidecar core helpers (per-CTU mean reduce
# + saliency-to-QP-offset). Header-only test that compiles
# libvmaf/tools/vmaf_roi_core.h and exercises clamp / sign / monotonicity
# without needing the full binary.
test_vmaf_roi_libm = cc.find_library('m', required : false)
test_vmaf_roi = executable('test_vmaf_roi',
    ['test.c', 'test_vmaf_roi.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../tools/')],
    dependencies: [pthread_dependency, test_vmaf_roi_libm],
)
test('test_vmaf_roi', test_vmaf_roi, suite : ['fast'])

# picture pool test - exercises vmaf_preallocate_pictures / vmaf_fetch_preallocated_picture.
# These symbols are always linked into libvmaf (no build gate), so this test
# always runs.
test_pic_preallocation = executable('test_pic_preallocation',
    ['test.c', 'test_pic_preallocation.c'],
    include_directories : [libvmaf_inc, test_inc],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, thread_lib],
)
test('test_pic_preallocation', test_pic_preallocation, suite : ['fast'])

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
     '../src/picture_pool.c', '../src/picture.c', '../src/mem.cpp', '../src/ref.cpp'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    dependencies: [stdatomic_dependency, pthread_dependency, thread_lib],
)
test('test_picture_pool_error_paths', test_picture_pool_error_paths, suite : ['fast'])

test_picture_pool_cpp_error_paths = executable('test_picture_pool_cpp_error_paths',
    ['test.c', 'test_picture_pool_error_paths.c',
     '../src/picture_pool.cpp', '../src/picture.c', '../src/mem.cpp', '../src/ref.cpp'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    override_options : ['cpp_std=' + libvmaf_cpu_cpp_std],
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
     '../src/ref.cpp', '../src/dict.cpp', '../src/predict.c',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp', dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib,
                    gpu_all_deps, dnn_deps],
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
     '../src/ref.cpp', '../src/dict.cpp', '../src/predict.c',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp', dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib,
                    gpu_all_deps, dnn_deps],
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
     '../src/ref.cpp', '../src/dict.cpp', '../src/predict.c',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp', dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib,
                    gpu_all_deps, dnn_deps],
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
     '../src/ref.cpp', '../src/dict.cpp', '../src/predict.c',
     '../src/metadata_handler.cpp', '../src/thread_locale.cpp', '../src/gpu_picture_pool.cpp', dnn_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib,
                    gpu_all_deps, dnn_deps],
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
    '../src/dict.cpp', '../src/predict.c',
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
                    gpu_all_deps, dnn_deps],
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
                    gpu_all_deps, dnn_deps],
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
                    gpu_all_deps, dnn_deps],
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
                    gpu_all_deps, dnn_deps],
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
                    gpu_all_deps, dnn_deps],
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

test_float_adm_coverage = executable('test_float_adm_coverage',
    ['test.c', 'test_float_adm_coverage.c'] + _float_fex_test_sources_common + dnn_sources,
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib,
                    gpu_all_deps, dnn_deps],
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

test_float_motion_coverage = executable('test_float_motion_coverage',
    ['test.c', 'test_float_motion_coverage.c'] + _float_fex_test_sources_common + dnn_sources,
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/'), dnn_inc],
    c_args : vmaf_cflags_common + dnn_defines,
    dependencies : [math_lib, stdatomic_dependency, pthread_dependency, thread_lib,
                    gpu_all_deps, dnn_deps],
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

# integer_vif vif_skip_scale0 option (CPU path) — closes the gap noted in
# .workingdir/audit-test-coverage-2026-05-16.md §1. Source already existed at
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

test('test_sycl', test_sycl, suite : ['fast', 'gpu', 'sycl'])
test('test_sycl_pic_preallocation', test_sycl_pic_preallocation, suite : ['fast', 'gpu', 'sycl'])

test_integer_cambi_sycl = executable('test_integer_cambi_sycl',
    ['test.c', 'test_integer_cambi_sycl.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, sycl_dependency],
    c_args: ['-DHAVE_SYCL=1']
)
test('test_integer_cambi_sycl', test_integer_cambi_sycl, suite : ['fast', 'gpu', 'sycl'])

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
    suite : ['fast', 'gpu', 'sycl'])

# ADR-0989 — motion_add_uv SYCL parity test.
# Asserts (1) UV contribution is positive (UV-enabled score > Y-only)
# and (2) SYCL motion_sycl(motion_add_uv=true) matches CPU
# float_motion(motion_add_uv=true) within places=4 (1e-4).
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
    suite : ['fast', 'gpu', 'sycl'])

# ADR-0946 — SYCL kernel coverage round 3. Five new CPU vs. SYCL
# parity gates filling the float-family + PSNR-HVS gaps left after
# rounds 1 (#351: integer psnr / vif) and 2 (#376: integer adm /
# ciede / ssim / ms_ssim / motion_v2). Each test mirrors the
# round-1 / round-2 pattern: 256x144 synthetic YUV420P fixture,
# places=4 (1e-4) ADR-0214 tolerance, skip-on-no-device.
test_sycl_float_psnr_parity = executable('test_sycl_float_psnr_parity',
    ['test.c', 'test_sycl_float_psnr_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, sycl_dependency, math_lib],
    c_args: ['-DHAVE_SYCL=1']
)
test('test_sycl_float_psnr_parity', test_sycl_float_psnr_parity,
    suite : ['fast', 'gpu', 'sycl'])

test_sycl_float_adm_parity = executable('test_sycl_float_adm_parity',
    ['test.c', 'test_sycl_float_adm_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, sycl_dependency, math_lib],
    c_args: ['-DHAVE_SYCL=1']
)
test('test_sycl_float_adm_parity', test_sycl_float_adm_parity,
    suite : ['fast', 'gpu', 'sycl'])

test_sycl_float_vif_parity = executable('test_sycl_float_vif_parity',
    ['test.c', 'test_sycl_float_vif_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, sycl_dependency, math_lib],
    c_args: ['-DHAVE_SYCL=1']
)
test('test_sycl_float_vif_parity', test_sycl_float_vif_parity,
    suite : ['fast', 'gpu', 'sycl'])

test_sycl_float_motion_parity = executable('test_sycl_float_motion_parity',
    ['test.c', 'test_sycl_float_motion_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, sycl_dependency, math_lib],
    c_args: ['-DHAVE_SYCL=1']
)
test('test_sycl_float_motion_parity', test_sycl_float_motion_parity,
    suite : ['fast', 'gpu', 'sycl'])

test_sycl_psnr_hvs_parity = executable('test_sycl_psnr_hvs_parity',
    ['test.c', 'test_sycl_psnr_hvs_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, sycl_dependency, math_lib],
    c_args: ['-DHAVE_SYCL=1']
)
test('test_sycl_psnr_hvs_parity', test_sycl_psnr_hvs_parity,
    suite : ['fast', 'gpu', 'sycl'])

# ADR-0957 — SYCL kernel coverage round 4. Four new CPU vs. SYCL
# parity gates closing the last uncovered SYCL extractors after
# round 3 (#446: float family + PSNR-HVS): float_moment_sycl,
# speed_chroma_sycl, speed_temporal_sycl, ssimulacra2_sycl. The
# first three use places=4 (1e-4) ADR-0214 tolerance; ssimulacra2
# uses 5e-3 to match the ADR-0214 `FEATURE_TOLERANCE` entry for
# its multi-stage XYB + IIR + SSIM-combine pipeline.
test_sycl_float_moment_parity = executable('test_sycl_float_moment_parity',
    ['test.c', 'test_sycl_float_moment_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, sycl_dependency, math_lib],
    c_args: ['-DHAVE_SYCL=1']
)
test('test_sycl_float_moment_parity', test_sycl_float_moment_parity,
    suite : ['fast', 'gpu', 'sycl'])

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
    suite : ['fast', 'gpu', 'sycl'])

test_sycl_ciede_parity = executable('test_sycl_ciede_parity',
    ['test.c', 'test_sycl_ciede_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, sycl_dependency, math_lib],
    c_args: ['-DHAVE_SYCL=1']
)
test('test_sycl_ciede_parity', test_sycl_ciede_parity,
    suite : ['fast', 'gpu', 'sycl'])

test_sycl_ssim_parity = executable('test_sycl_ssim_parity',
    ['test.c', 'test_sycl_ssim_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, sycl_dependency, math_lib],
    c_args: ['-DHAVE_SYCL=1']
)
test('test_sycl_ssim_parity', test_sycl_ssim_parity,
    suite : ['fast', 'gpu', 'sycl'])

test_sycl_ms_ssim_parity = executable('test_sycl_ms_ssim_parity',
    ['test.c', 'test_sycl_ms_ssim_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, sycl_dependency, math_lib],
    c_args: ['-DHAVE_SYCL=1']
)
test('test_sycl_ms_ssim_parity', test_sycl_ms_ssim_parity,
    suite : ['fast', 'gpu', 'sycl'])

# float_ssim SYCL parity — Research-0985. Tolerance is places=3 (5e-04) to
# accommodate the CPU/GPU formula difference (L×C×S vs combined) and Arc A380
# class fp32 accumulation drift. See Research-0985 §3.4.
test_sycl_float_ssim_parity = executable('test_sycl_float_ssim_parity',
    ['test.c', 'test_sycl_float_ssim_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, sycl_dependency, math_lib],
    c_args: ['-DHAVE_SYCL=1']
)
test('test_sycl_float_ssim_parity', test_sycl_float_ssim_parity,
    suite : ['fast', 'gpu', 'sycl'])

test_sycl_motion_v2_parity = executable('test_sycl_motion_v2_parity',
    ['test.c', 'test_sycl_motion_v2_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, sycl_dependency, math_lib],
    c_args: ['-DHAVE_SYCL=1']
)
test('test_sycl_motion_v2_parity', test_sycl_motion_v2_parity,
    suite : ['fast', 'gpu', 'sycl'])

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
    suite : ['fast', 'gpu', 'sycl'])

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
    suite : ['fast', 'gpu', 'sycl'])

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
    suite : ['fast', 'gpu', 'sycl'])

test_sycl_speed_temporal_parity = executable('test_sycl_speed_temporal_parity',
    ['test.c', 'test_sycl_speed_temporal_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, sycl_dependency, math_lib],
    c_args: ['-DHAVE_SYCL=1']
)
test('test_sycl_speed_temporal_parity', test_sycl_speed_temporal_parity,
    suite : ['fast', 'gpu', 'sycl'])

test_sycl_ssimulacra2_parity = executable('test_sycl_ssimulacra2_parity',
    ['test.c', 'test_sycl_ssimulacra2_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, sycl_dependency, math_lib],
    c_args: ['-DHAVE_SYCL=1']
)
test('test_sycl_ssimulacra2_parity', test_sycl_ssimulacra2_parity,
    suite : ['fast', 'gpu', 'sycl'])

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
    suite : ['fast', 'gpu', 'sycl'])
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
if get_option('enable_hip') == true
test_hip_smoke = executable('test_hip_smoke',
    ['test.c', 'test_hip_smoke.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, hip_deps],
    c_args: ['-DHAVE_HIP=1']
)
test('test_hip_smoke', test_hip_smoke, suite : ['fast', 'gpu'])

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
# Deferred per ADR-1154 (T-GAP-HIP-INTEGER-ADM-PICTURE-STAGING-DEFERRED-2026-09-02):
# integer_adm_hip passes CPU host memory directly to device kernels without staging
# buffers, causing GPU access faults until staging buffers (~350 LOC) or device picture pool (T7-10c) lands.
test('test_hip_adm_parity', test_hip_adm_parity,
    suite : ['fast', 'gpu'],
    should_fail : true,
    timeout : 60)

# ADR-1167 — ADM small-frame border parity test (i == 0 && top <= 0 branch).
# Deferred per ADR-1154 (T-GAP-HIP-INTEGER-ADM-PICTURE-STAGING-DEFERRED-2026-09-02) on HIP.
test_hip_adm_small_border = executable('test_hip_adm_small_border',
    ['test.c', 'test_adm_small_border.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, hip_deps],
    c_args: ['-DHAVE_HIP=1']
)
test('test_hip_adm_small_border', test_hip_adm_small_border,
    suite : ['fast', 'gpu'],
    should_fail : true,
    timeout : 60)

# ADR-1167 — ADM wide-frame row-level rounding parity test (spans 60 warps).
# Deferred per ADR-1154 (T-GAP-HIP-INTEGER-ADM-PICTURE-STAGING-DEFERRED-2026-09-02) on HIP.
test_hip_adm_wide_rounding = executable('test_hip_adm_wide_rounding',
    ['test.c', 'test_adm_wide_rounding.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, hip_deps],
    c_args: ['-DHAVE_HIP=1']
)
test('test_hip_adm_wide_rounding', test_hip_adm_wide_rounding,
    suite : ['fast', 'gpu'],
    should_fail : true,
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

test_hip_ssimulacra2_parity = executable('test_hip_ssimulacra2_parity',
    ['test.c', 'test_hip_ssimulacra2_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, hip_deps, math_lib],
    c_args: ['-DHAVE_HIP=1']
)
test('test_hip_ssimulacra2_parity', test_hip_ssimulacra2_parity,
    suite : ['fast', 'gpu'],
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
    timeout : 60)

test_hip_float_adm_parity = executable('test_hip_float_adm_parity',
    ['test.c', 'test_hip_float_adm_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, hip_deps, math_lib],
    c_args: ['-DHAVE_HIP=1']
)
test('test_hip_float_adm_parity', test_hip_float_adm_parity,
    suite : ['fast', 'gpu'],
    timeout : 60)

test_hip_float_motion_parity = executable('test_hip_float_motion_parity',
    ['test.c', 'test_hip_float_motion_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, hip_deps, math_lib],
    c_args: ['-DHAVE_HIP=1']
)
test('test_hip_float_motion_parity', test_hip_float_motion_parity,
    suite : ['fast', 'gpu'],
    timeout : 60)

test_hip_float_psnr_parity = executable('test_hip_float_psnr_parity',
    ['test.c', 'test_hip_float_psnr_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, hip_deps, math_lib],
    c_args: ['-DHAVE_HIP=1']
)
test('test_hip_float_psnr_parity', test_hip_float_psnr_parity,
    suite : ['fast', 'gpu'],
    timeout : 60)

# ADR-0883 round-2 — HIP kernel parity gap-fill (5 extractors).
# Closes the gaps left after PR #351 (psnr_hip + vif_hip) and the
# pre-existing adm/motion3 tests.  Each test follows the same template:
# CPU reference vs. HIP score on a synthetic fixture, places=4 (1e-4)
# for unfiltered reductions (ciede, psnr_hvs, motion) and places=3
# (1e-3) for filtered ones (ssim, ms_ssim).  HIP-gated: skips cleanly
# when no AMD GPU is visible.

test_hip_ciede_parity = executable('test_hip_ciede_parity',
    ['test.c', 'test_hip_ciede_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, hip_deps, math_lib],
    c_args: ['-DHAVE_HIP=1']
)
test('test_hip_ciede_parity', test_hip_ciede_parity,
    suite : ['fast', 'gpu'],
    timeout : 60)

test_hip_psnr_hvs_parity = executable('test_hip_psnr_hvs_parity',
    ['test.c', 'test_hip_psnr_hvs_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, hip_deps, math_lib],
    c_args: ['-DHAVE_HIP=1']
)
test('test_hip_psnr_hvs_parity', test_hip_psnr_hvs_parity,
    suite : ['fast', 'gpu'],
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
    timeout : 60)

test_hip_ssim_parity = executable('test_hip_ssim_parity',
    ['test.c', 'test_hip_ssim_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, hip_deps, math_lib],
    c_args: ['-DHAVE_HIP=1']
)
# Deferred per ADR-1154 (T-GAP-HIP-INTEGER-SSIM-FLOAT-KERNEL-DEFERRED-2026-09-02):
# integer_ssim_score.hip implements the 11-tap float Gaussian instead of the 9-tap int64 kernel,
# causing 4.5e-3 divergence vs CPU ground truth. Preserved CPU fallback per ADR-0564 until int64 kernel lands.
test('test_hip_ssim_parity', test_hip_ssim_parity,
    suite : ['fast', 'gpu'],
    should_fail : true,
    timeout : 60)

test_hip_ms_ssim_parity = executable('test_hip_ms_ssim_parity',
    ['test.c', 'test_hip_ms_ssim_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, hip_deps, math_lib],
    c_args: ['-DHAVE_HIP=1']
)
test('test_hip_ms_ssim_parity', test_hip_ms_ssim_parity,
    suite : ['fast', 'gpu'],
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
    timeout : 60)

# GPU-kernel coverage gap-fill — VIF scale0 CPU vs. HIP parity (places=3).
# Covers integer_vif_hip.c + integer_vif/vif_statistics.hip, the AMD
# Gaussian filter + M1..M4 statistical accumulator path.
# HIP-gated: skips cleanly when no AMD GPU is visible.
test_hip_vif_parity = executable('test_hip_vif_parity',
    ['test.c', 'test_hip_vif_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, hip_deps, math_lib],
    c_args: ['-DHAVE_HIP=1']
)
test('test_hip_vif_parity', test_hip_vif_parity,
    suite : ['fast', 'gpu'],
    timeout : 60)

# ADR-1004 round 5 — speed_chroma + speed_temporal HIP parity gates.
# Deferred from round 4 (ADR-0958) because `speed_internal_init_dimensions`
# and `speed_internal_float_stride` had no .c implementation at the time.
# ADR-0964 / PR #465 introduced `core/src/feature/speed_internal.c`,
# unblocking these tests.  Both tests link against libvmaf (which compiles
# speed_internal.c via src/meson.build `speed_internal` entry) and follow
# the round-1/2/3/4 template: synthetic fixture, CPU reference vs. HIP
# score, skip cleanly on `[skip: no HIP device]` or
# `[skip: HIP scaffold ENOSYS]`.  Tolerance: places=4 (1e-4) per ADR-0214
# — SpEED's QR / eigensolver runs on CPU for both backends.
test_hip_speed_chroma_parity = executable('test_hip_speed_chroma_parity',
    ['test.c', 'test_hip_speed_chroma_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, hip_deps, math_lib],
    c_args: ['-DHAVE_HIP=1']
)
test('test_hip_speed_chroma_parity', test_hip_speed_chroma_parity,
    suite : ['fast', 'gpu'],
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
    timeout : 60)
endif

# ADR-0338 / T8-1: Metal (Apple Silicon) backend scaffold smoke. Same
# pattern as the HIP / Vulkan scaffolds above — pins the -ENOSYS
# contract for every public C-API entry point until the runtime PR
# (T8-1b) lands. Built whenever the parent meson resolved
# is_metal_enabled to true (auto on macOS, or `enabled` explicitly).
metal_test_opt = get_option('enable_metal')
if metal_test_opt.enabled() or (metal_test_opt.auto() and host_machine.system() == 'darwin')
test_metal_smoke = executable('test_metal_smoke',
    ['test.c', 'test_metal_smoke.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency],
    c_args: ['-DHAVE_METAL=1']
)
test('test_metal_smoke', test_metal_smoke, suite : ['fast', 'gpu'])

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
test('test_metal_install_header', test_metal_install_header, suite : ['fast', 'gpu'])
endif

# Metal kernel coverage round 3 — per-extractor parity tests for the four
# extractors not covered by round 2 (PR #379): integer_motion, float_motion,
# float_moment, float_ms_ssim. Same pattern as round 2 — synthetic YUV420P
# fixture fed through both the CPU twin and the Metal extractor, places=4
# (1e-4) parity per ADR-0214; float_ms_ssim uses the 1e-3 SSIM-family bound
# from ADR-0589. Each test skips cleanly when `vmaf_metal_state_init`
# returns -ENODEV (Linux / Windows / Intel Mac); on Apple-Family-7+ macOS
# CI lanes they exercise the live kernel.
test_metal_integer_motion_parity = executable('test_metal_integer_motion_parity',
    ['test.c', 'test_metal_integer_motion_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, math_lib],
    c_args: ['-DHAVE_METAL=1']
)
test('test_metal_integer_motion_parity', test_metal_integer_motion_parity, suite : ['fast', 'gpu'])

test_metal_float_motion_parity = executable('test_metal_float_motion_parity',
    ['test.c', 'test_metal_float_motion_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, math_lib],
    c_args: ['-DHAVE_METAL=1']
)
test('test_metal_float_motion_parity', test_metal_float_motion_parity, suite : ['fast', 'gpu'])

test_metal_float_moment_parity = executable('test_metal_float_moment_parity',
    ['test.c', 'test_metal_float_moment_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, math_lib],
    c_args: ['-DHAVE_METAL=1']
)
test('test_metal_float_moment_parity', test_metal_float_moment_parity, suite : ['fast', 'gpu'])

test_metal_float_ms_ssim_parity = executable('test_metal_float_ms_ssim_parity',
    ['test.c', 'test_metal_float_ms_ssim_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, math_lib],
    c_args: ['-DHAVE_METAL=1']
)
test('test_metal_float_ms_ssim_parity', test_metal_float_ms_ssim_parity, suite : ['fast', 'gpu'])

# Metal kernel coverage round 2 — per-extractor parity tests (ADR-0421 /
# ADR-0589 / ADR-0214). Extend PR #351's registration audit with real CPU
# vs. Metal score comparisons on synthetic YUV420P fixtures. Each test
# skips cleanly when `vmaf_metal_state_init` returns -ENODEV (Linux,
# Windows, Intel Mac); on Apple-Family-7+ macOS CI lanes they exercise
# the live kernel and assert places=4 (1e-4) parity per ADR-0214 — except
# float_ssim which uses the 1e-3 SSIM-specific bound from ADR-0589.
test_metal_motion_v2_parity = executable('test_metal_motion_v2_parity',
    ['test.c', 'test_metal_motion_v2_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, math_lib],
    c_args: ['-DHAVE_METAL=1']
)
test('test_metal_motion_v2_parity', test_metal_motion_v2_parity,
    suite : ['fast', 'gpu'],
    should_fail : false,
    protocol : 'exitcode',
    verbose : true,
)

# integer_ssim_metal — fixed-point SSIM (feature "ssim") on Metal; CPU vs.
# Metal parity at places=4 (1e-4), skips cleanly when no Metal device.
test_metal_integer_ssim_parity = executable('test_metal_integer_ssim_parity',
    ['test.c', 'test_metal_integer_ssim_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, math_lib],
    c_args: ['-DHAVE_METAL=1']
)
test('test_metal_integer_ssim_parity', test_metal_integer_ssim_parity, suite : ['fast', 'gpu'])

# float_vif_metal — 4-scale VIF pyramid (feature "float_vif"); CPU vs. Metal
# parity, skips cleanly when no Metal device.
test_metal_float_vif_parity = executable('test_metal_float_vif_parity',
    ['test.c', 'test_metal_float_vif_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, math_lib],
    c_args: ['-DHAVE_METAL=1']
)
test('test_metal_float_vif_parity', test_metal_float_vif_parity, suite : ['fast', 'gpu'])

# float_adm_metal — 4-scale DWT+CSF+CM (feature "float_adm"); CPU vs. Metal
# parity, skips cleanly when no Metal device.
test_metal_float_adm_parity = executable('test_metal_float_adm_parity',
    ['test.c', 'test_metal_float_adm_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, math_lib],
    c_args: ['-DHAVE_METAL=1']
)
test('test_metal_float_adm_parity', test_metal_float_adm_parity, suite : ['fast', 'gpu'])

# integer_vif_metal — 4-scale fixed-point VIF pyramid (feature "vif"); CPU vs.
# Metal parity, skips cleanly when no Metal device.
test_metal_integer_vif_parity = executable('test_metal_integer_vif_parity',
    ['test.c', 'test_metal_integer_vif_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, math_lib],
    c_args: ['-DHAVE_METAL=1']
)
test('test_metal_integer_vif_parity', test_metal_integer_vif_parity, suite : ['fast', 'gpu'])

# integer_adm_metal — fixed-point 4-scale DWT+CSF+CM (feature "adm"); CPU vs.
# Metal parity, skips cleanly when no Metal device.
test_metal_integer_adm_parity = executable('test_metal_integer_adm_parity',
    ['test.c', 'test_metal_integer_adm_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, math_lib],
    c_args: ['-DHAVE_METAL=1']
)
test('test_metal_integer_adm_parity', test_metal_integer_adm_parity, suite : ['fast', 'gpu'])

# Standalone-metric Metal parity tests (ciede2000, psnr_hvs, cambi, ssimulacra2).
foreach _m : ['integer_ciede', 'integer_psnr_hvs', 'integer_cambi', 'ssimulacra2']
  _exe = executable('test_metal_' + _m + '_parity',
      ['test.c', 'test_metal_' + _m + '_parity.c'],
      include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
      link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
      dependencies: [pthread_dependency, math_lib],
      c_args: ['-DHAVE_METAL=1'])
  test('test_metal_' + _m + '_parity', _exe, suite : ['fast', 'gpu'])
endforeach

test_metal_integer_psnr_parity = executable('test_metal_integer_psnr_parity',
    ['test.c', 'test_metal_integer_psnr_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, math_lib],
    c_args: ['-DHAVE_METAL=1']
)
test('test_metal_integer_psnr_parity', test_metal_integer_psnr_parity, suite : ['fast', 'gpu'])

test_metal_float_psnr_parity = executable('test_metal_float_psnr_parity',
    ['test.c', 'test_metal_float_psnr_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, math_lib],
    c_args: ['-DHAVE_METAL=1']
)
test('test_metal_float_psnr_parity', test_metal_float_psnr_parity, suite : ['fast', 'gpu'])

test_metal_float_ssim_parity = executable('test_metal_float_ssim_parity',
    ['test.c', 'test_metal_float_ssim_parity.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    dependencies: [pthread_dependency, math_lib],
    c_args: ['-DHAVE_METAL=1']
)
test('test_metal_float_ssim_parity', test_metal_float_ssim_parity, suite : ['fast', 'gpu'])

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
test('test_metal_kernel_coverage_audit', test_metal_kernel_coverage_audit, suite : ['fast', 'gpu'])

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
    suite : ['fast', 'gpu'])
endif

test('test_context', test_context, suite : ['fast'])
test('test_log', test_log, suite : ['fast'])
test('test_picture', test_picture, suite : ['fast'])
test('test_picture_v2', test_picture_v2, suite : ['fast'])
test('test_feature_collector', test_feature_collector, suite : ['fast'])
test('test_flush_context_ordering', test_flush_context_ordering, suite : ['fast'])
test('test_output', test_output, suite : ['fast'])
# ADR-0708 Wave 1: test_log registered as part of the log.c → log.cpp pilot
# (mirrors DRAFT PR #315's orphan-test wiring). The executable was built but
# never test()-registered, so the formatter / log-level filtering coverage
# was silently dead. Wire it into the fast suite.
test('test_log', test_log, suite : ['fast'])
test('test_public_api_score', test_public_api_score, suite : ['fast'])
test('test_thread_pool', test_thread_pool, suite : ['fast'])
test('test_model', test_model, suite : ['fast'])
test('test_model_libsvm_dup_key', test_model_libsvm_dup_key, suite : ['fast'])
test('test_model_feature_overload_ownership', test_model_feature_overload_ownership, suite : ['fast'])
test('test_model_collection_api', test_model_collection_api, suite : ['fast'])
test('test_predict', test_predict, suite : ['fast'])
test('test_svm_api', test_svm_api, suite : ['fast'])
test('test_svm_parser', test_svm_parser, suite : ['fast'])
test('test_svm_multiclass', test_svm_multiclass, suite : ['fast'])
test('test_iqa_helpers', test_iqa_helpers, suite : ['fast'])
test('test_feature_extractor', test_feature_extractor, suite : ['fast'])
test('test_dict', test_dict, suite : ['fast'])
test('test_opt', test_opt, suite : ['fast'])
test('test_cpu', test_cpu, suite : ['fast'])
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
endif
if get_option('enable_float')
test('test_speed', test_speed, suite : ['fast'])
endif
test('test_speed_qa', test_speed_qa, suite : ['fast'])
test('test_luminance_tools', test_luminance_tools, suite : ['fast'])
test('test_cli_parse', test_cli_parse, suite : ['fast'])
test('test_spinner', test_spinner, suite : ['fast'])
if host_machine.system() != 'windows'
    test('test_y4m_411_oob', test_y4m_411_oob, suite : ['fast'])
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
test('test_psnr_hvs_coverage', test_psnr_hvs_coverage, suite : ['fast'])
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
test('test_propagate_metadata', test_propagate_metadata, suite : ['fast'])
test('test_adm_csf', test_adm_csf, suite : ['fast'])
test('test_barten_csf', test_barten_csf, suite : ['fast'])
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

if integer_adm_simd_test_archs.contains(host_machine.cpu_family())
test('test_integer_adm_simd', test_integer_adm_simd, suite : ['fast', 'simd'])
test('test_adm_dwt2_x86', test_adm_dwt2_x86, suite : ['fast', 'simd'])
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
# libvmaf includes only feature_collector.c (static helpers); the
# duplicate-free compilation here is safe — see test_predict for the
# same pattern.
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
# Compiles `mkdirp.c` into the binary directly — the file is part of
# libvmaf_feature_static_lib so linking against libvmaf would also work,
# but a TU-local compile keeps the test self-contained and avoids
# coupling to libvmaf's link graph.
test_mkdirp = executable('test_mkdirp',
    ['test.c', 'test_mkdirp.c', '../src/feature/mkdirp.cpp'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../src/')],
    dependencies: [pthread_dependency, sycl_dependency],
)
test('test_mkdirp', test_mkdirp, suite : ['fast'])
