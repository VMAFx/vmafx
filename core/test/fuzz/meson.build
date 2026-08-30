# libFuzzer harnesses for parser surfaces.
#
# Opt-in: only built when `-Dfuzz=true` is passed to `meson setup`. Requires
# clang as the compiler — gcc does not ship libFuzzer. Pairs with
# AddressSanitizer (`-Db_sanitize=address`) for heap-error coverage.
#
# See ADR-0270 (initial scaffold), ADR-0311 (yuv_input + cli_parse
# expansion), and the follow-up json_model + dnn_sidecar additions
# tracked under docs/research/0083-libfuzzer-harness-expansion-target-survey.md
# (deferred targets #3 + #4). Operator runbook in
# docs/development/fuzzing.md.

if not get_option('fuzz')
    subdir_done()
endif

if cc.get_id() != 'clang'
    error('-Dfuzz=true requires clang (libFuzzer is a clang feature). ' +
          'Re-run meson setup with CC=clang.')
endif

# `-fsanitize=fuzzer` ships both libFuzzer's runtime (the `main`
# replacement that calls LLVMFuzzerTestOneInput) and the SanitizerCoverage
# instrumentation libFuzzer needs. The library under test is built with
# `-fsanitize=fuzzer-no-link` (instrumentation only, no `main`); we apply
# that via meson's `b_sanitize` only on the harness target's c_args /
# link_args so it doesn't infect the main libvmaf build.
fuzz_flags = ['-fsanitize=fuzzer']

# Tool-source set shared by every harness that drives the vidinput stack.
fuzz_vidinput_sources = [
    '../../tools/y4m_input.c',
    '../../tools/yuv_input.c',
    '../../tools/vidinput.c',
]

fuzz_y4m_input = executable('fuzz_y4m_input',
    ['fuzz_y4m_input.c'] + fuzz_vidinput_sources,
    include_directories : [libvmaf_inc, test_inc, include_directories('../../tools/')],
    c_args : fuzz_flags + compat_cflags,
    link_args : fuzz_flags,
)

# fuzz_yuv_input — wraps the headerless raw-YUV reader. Same vidinput
# source set; the harness fixes (width, height, pix_fmt, bitdepth) at
# compile time and lets the fuzzer vary only the frame body bytes.
fuzz_yuv_input = executable('fuzz_yuv_input',
    ['fuzz_yuv_input.c'] + fuzz_vidinput_sources,
    include_directories : [libvmaf_inc, test_inc, include_directories('../../tools/')],
    c_args : fuzz_flags + compat_cflags,
    link_args : fuzz_flags,
)

# fuzz_cli_parse — wraps the argv-tokenising CLI parser. Links against
# libvmaf for `vmaf_version()` and the feature-dictionary symbols. Uses
# `-Wl,--wrap=exit` so the harness can intercept `usage()`'s `exit(1)`
# via a `__wrap_exit` longjmp shim instead of terminating the fuzzer
# process. See fuzz_cli_parse.c for the longjmp / setjmp contract.
fuzz_cli_parse_link_target = (get_option('default_library') == 'both'
    ? libvmaf.get_shared_lib()
    : libvmaf)

fuzz_cli_parse = executable('fuzz_cli_parse',
    ['fuzz_cli_parse.c', '../../tools/cli_parse.c'],
    include_directories : [libvmaf_inc, test_inc, vmaf_include,
                           include_directories('../../tools/')],
    dependencies : [getopt_dependency],
    c_args : fuzz_flags + compat_cflags,
    link_args : fuzz_flags + ['-Wl,--wrap=exit'],
    link_with : fuzz_cli_parse_link_target,
)

# fuzz_json_model — wraps the libvmaf SVM model JSON parser
# (core/src/read_json_model.c) via its public buffer entry points
# `vmaf_read_json_model_from_buffer` /
# `vmaf_read_json_model_collection_from_buffer`. Those symbols are
# fork-local (not VMAF_EXPORTed) and libvmaf is built with
# `-fvisibility=hidden` (ADR-0379), so the harness links the parser
# source directly — mirroring how `test_model` is wired upstream.
# The libsvm static archive supplies `svm_load_model_from_string`
# called from the embedded SV-block parser.
fuzz_json_model = executable('fuzz_json_model',
    ['fuzz_json_model.c',
     '../../src/read_json_model.c',
     '../../src/pdjson.c',
     '../../src/dict.c',
     '../../src/log.c',
     json_model_c_sources],
    include_directories : [libvmaf_inc, test_inc, include_directories('../../src')],
    link_with : get_option('default_library') == 'both' ? libvmaf.get_static_lib() : libvmaf,
    c_args : fuzz_flags + compat_cflags,
    link_args : fuzz_flags,
    dependencies : [pthread_dependency, thread_lib, gpu_all_deps],
    objects : libsvm_static_lib.extract_all_objects(recursive: true),
)

# fuzz_dnn_sidecar — wraps the tiny-AI sidecar loader
# (core/src/dnn/model_loader.c — `vmaf_dnn_sidecar_load`). The
# loader takes an `.onnx` path and derives `<basename>.json` next
# to it; the harness writes the fuzzer-controlled bytes into a
# per-process tempfile pair (`/tmp/vmaf-fuzz-sidecar-<pid>.json`)
# each iteration and calls the entry point. The sidecar API is
# fork-internal (not VMAF_EXPORTed) and libvmaf hides non-exported
# symbols at link time, so the harness compiles the same dnn-source
# set that `test_model_loader` does, mirroring the precedent in
# core/test/dnn/meson.build.
fuzz_dnn_sidecar = executable('fuzz_dnn_sidecar',
    ['fuzz_dnn_sidecar.c',
     '../../src/dnn/model_loader.c',
     '../../src/dnn/onnx_scan.c',
     '../../src/dnn/op_allowlist.c'],
    include_directories : [libvmaf_inc, test_inc, include_directories('../../src'),
                           include_directories('../../src/dnn')],
    c_args : fuzz_flags + dnn_defines,
    link_args : fuzz_flags,
)
