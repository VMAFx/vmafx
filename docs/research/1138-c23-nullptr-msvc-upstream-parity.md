<!-- markdownlint-disable MD013 -->
# Research: `modernize-use-nullptr` on C23 translation units, and the c-rework-core lint pass

**Date**: 2026-09-02
**Author**: c-rework-core unit (ADR-1138 implementation pass)

## Summary

The first ADR-0141 rework of the library-core plumbing files (`core/src/libvmaf.c`,
`core/src/predict.c`, `core/src/feature/feature_collector.c`,
`core/src/read_json_model.cpp`) surfaced 116 clang-tidy findings
(`clang-tidy -p build --quiet`, LLVM 22.1.8, CPU-only compile database —
`libvmaf.c` 55, `predict.c` 5, `feature_collector.c` 11,
`read_json_model.cpp` 45): 54 `modernize-use-nullptr` on the three C files,
11 `readability-function-size`, 4 `bugprone-multi-level-implicit-pointer-conversion`,
1 `readability-isolate-declaration`, 1 `misc-use-internal-linkage`,
31 `misc-use-anonymous-namespace` and 14 `misc-const-correctness` — plus 22
cppcheck style findings (`constParameterPointer`, `constVariablePointer`,
`knownConditionTrueFalse`, `redundantAssignment`, `redundantInitialization`,
`unreadVariable`, `variableScope`). This digest records what was verified
before deciding how to treat the `nullptr` class (ADR-1138) and how the
oversized functions were split without changing the public C ABI or the
Netflix golden scores.

## Why `modernize-use-nullptr` fires on C files here

- `core/meson.build` injects `-std=c23` (GCC 14+ / Clang 18+), `-std=c2x` on
  GCC 13, and `/std:clatest` on MSVC (ADR-0692).
- clang-tidy's `UseNullptrCheck::isLanguageVersionSupported` returns true for
  `LangOpts.C23` since LLVM 18; CI installs clang-tidy 22 (the same major as the
  dev host), so local and CI behaviour match.
- Only *typed* null constants trigger it: `VmafContext *p = NULL` involves a
  `CK_NullToPointer` implicit cast of glibc's `((void *)0)`, whereas
  `void *data = NULL` does not. That is why e.g. `core/src/picture.c` (three
  `void *` NULLs) reports nothing while `libvmaf.c` reported 41 (both
  re-measured on the final tree with clang-tidy 22.1.8).
- The CI lane runs `clang-tidy-22 -p build --quiet <file> || touch /tmp/tidy.fail`;
  clang-tidy exits non-zero only for `WarningsAsErrors` and compile errors, so
  these findings never failed a build — they were simply never discharged.

## Can C sources use `nullptr` on every required compiler?

| Compiler / lane | C `nullptr` | Evidence |
| --- | --- | --- |
| GCC 13 (`-std=c2x`, Ubuntu 24.04 lanes) | yes | GCC 13 release notes (C2X `nullptr`) |
| GCC 14+ / Clang 16+ (`-std=c23`) | yes | local build (Clang 22) |
| MSVC `/std:clatest` (required `Build — Windows (MSVC + CUDA)`) | **undocumented** | Microsoft Learn `/std` reference lists the C11/C17 feature set and describes `/std:clatest` only as "all currently implemented ... features proposed in the next draft C standard"; the conformance table's C section does not mention `nullptr` (both pages retrieved 2026-09-02) |

`find core -name '*.c' | xargs grep -l nullptr` returns nothing: no C TU in the
tree has ever relied on it, so the MSVC lane has never exercised it. Combined
with the upstream-parity cost (Netflix sources spell `NULL`; every sync would
re-conflict on pointer initialisers), the keyword rewrite was rejected —
see ADR-1138 for the decision matrix. The chosen mechanism is a file-scoped,
cited `NOLINTBEGIN/END(modernize-use-nullptr)` bracket per touched C TU;
`.clang-tidy` is untouched so the `.cpp` ratchet from ADR-0915 is preserved.

## Function-size splits (readability-function-size, 60-line budget)

| Function | Before | Split into | Semantics preserved by |
| --- | --- | --- | --- |
| `vmaf_init` | 86 lines, 7 gotos | `vmaf_ctx_thread_pools_init`, `vmaf_ctx_subsystems_init`, `vmaf_init` | same dependency order and reverse teardown; `*vmaf` is only assigned on success (it was already required NULL on entry) |
| `dnn_attach_nchw` / `dnn_attach_feature_vector` | 72 / 108 | `dnn_validate_nchw_shape`, `dnn_validate_feature_vector_shape`, `dnn_probe_extra_input`, `dnn_attach_commit` | identical validation order, log text, error codes and free-on-failure sets |
| `vmaf_ctx_dnn_run_frame_nchw` / `_feature_vector` | 71 / 65 | `dnn_fill_nchw_input`, `dnn_materialise_feature_vector`, shared `dnn_run_and_append` | output binding loop and `-ENOSPC → -ENOTSUP` mapping unchanged |
| `vmaf_close` | 63 | `vmaf_close_backends` | teardown order unchanged (ring buffer → drain stream → CUDA context) |
| `threaded_extract_batch_func` | 137, 2 gotos | `batch_thread_data_ensure`, `batch_extractor_skip`, `batch_ensure_fex_ctx`, `batch_extract_one` | ADR-0795 per-thread deep copy and ADR-1051 PREV_REF balance kept; the first error still stops the loop and the three snapshot unrefs still run exactly once |
| `read_pictures_dispatch_one` | 62 | `dispatch_gpu_double_buffer` | collect-then-submit order and prev_ref release on error unchanged |
| `vmaf_read_pictures` | 97, 2 gotos, 6 `#ifdef` islands | `ReadPicturesFrame` + `read_pictures_frame_{translate,select_host,cleanup,cleanup_after_batch}` | every `#ifdef HAVE_CUDA` branch moved verbatim into one helper; a single `#ifdef HAVE_CUDA` remains around the translate call (the helper has no CPU stub); syntax-checked with `-DHAVE_CUDA=1` and `-DHAVE_SYCL=1` |
| `vmaf_write_output_with_format` | 97 | `output_file_open`, `output_fps`, `output_write` | 0644 open, errno capture before `fprintf`, ADR-0606 fps guard, `fclose` → `-EIO` unchanged |
| `post_process_feature_from_another` (predict.c) | 68 | `GuidedFeatureScan` + `scan_match_feature` + `scan_feature` | the single-pass early-return order of the original loop is kept (a non-sentinel guided score still exits before later ambiguity checks) |

The repeated `if (fex->prev_ref.ref) { unref; memset }` idiom (six sites) is now
`fex_release_prev_ref()`, and the two subsample-skip copies share
`fex_subsample_skip()`.

## Behaviour deltas (all on invalid-input paths only)

- `bootstrap_transform_and_clip` (predict.c) now propagates a failing
  `transform()` (malformed piecewise-linear knot list) instead of discarding
  it — CERT ERR33-C. Valid models (all shipped `vmaf_b_*.json`) are unaffected.
- `piecewise_segment_apply` checks `find_linear_function_parameters`; the
  failure is unreachable for a segment that passed the ordering guard.
- `vmaf_predict_score_at_index_model_collection` rejects NULL arguments with
  `-EINVAL` instead of dereferencing them.
- `feature_collector` / `predict` `goto unlock` / `goto out` ladders were
  replaced by straight-line single-exit code; the ADR-0154 `-EAGAIN`
  contract lives in `feature_vector_read()`.

## cppcheck residue and how it was discharged

`make lint-c`'s cppcheck invocation (`--enable=all --inline-suppr
--suppressions-list=.cppcheck-suppressions.txt --project=build/compile_commands.json`)
restricted to the four TUs reported 22 style findings on master and 6 after the
first rework pass. The last six were closed as follows:

| Finding | Resolution |
| --- | --- |
| `knownConditionTrueFalse` on `if (err)` after `read_pictures_frame_translate()` | The CPU stub returned a constant 0; the helper now exists only under `HAVE_CUDA` and `vmaf_read_pictures()` guards the single call with `#ifdef HAVE_CUDA` (one island instead of the former six). |
| `constParameterPointer` on `bootstrap_transform_and_clip(score)` | False positive from the array-of-pointers loop; rewritten as five chained `transform_and_clip()` calls in the upstream order (same short-circuit on the first error). |
| `constParameterPointer` on `vmaf_feature_collector_get(vmaf)` | `const VmafContext *` in `libvmaf_priv.h` and the definition (internal test accessor, no twin). |
| `constParameterPointer` on `vmaf_context_get_backend(vmaf)` | Cited inline suppression: the exported prototype in `core/include/libvmaf/libvmaf.h` is frozen for this rework. |
| `constParameterPointer` on `read_pictures_validate_and_prep(ref, dist)` | Cited inline suppression: the SYCL build hands both pictures to `vmaf_sycl_shared_frame_upload()`, whose prototype takes mutable pictures; cppcheck only analyses the CPU configuration. |
| `constParameterPointer` on `vmaf_feature_collector_unmount_model(model)` | Cited inline suppression: the prototype in `feature_collector.h` is shared with the live C++ twin `feature_collector.cpp` (compiled into `test_predict` and the coverage tests), and an `extern "C"` declaration cannot differ between the two TUs. |

What remains in a four-file cppcheck run is `unusedFunction` on public entry
points (`vmaf_init`, `vmaf_preallocate_pictures`, …), which is a whole-program
check that cannot see the callers in `core/tools/` and `core/test/` when the
run is filtered to four files. The project-wide `make lint-c` invocation
(2 206 TUs; 619 pre-existing findings tree-wide, so cppcheck is not a passing
gate on `master`) clears all of those except six exported entry points defined
in `libvmaf.c` — `vmaf_ctx_dnn_attach`, `vmaf_ctx_dnn_has_session`,
`vmaf_ctx_dnn_set_codec_context`, `vmaf_ctx_dnn_set_resize_mode`,
`vmaf_ctx_dnn_is_codec_aware`, `vmaf_register_metadata_handler`. The five
`vmaf_ctx_dnn_*` entry points are called from `core/src/dnn/dnn_attach_api.c`,
which is compiled only with tiny-AI support (this host's build found no
onnxruntime, so the whole-program view never saw the caller);
`vmaf_register_metadata_handler` is the metadata-propagation entry point
declared in `core/src/metadata.h` with no in-tree caller. That residue predates
the rework and is configuration-dependent; a suppressions-list entry for
exported API definitions is a policy decision left to a follow-up ADR.

## Verification (measured on the final tree, 2026-09-02)

- `meson setup build core -Denable_cuda=false -Denable_sycl=false && ninja -C build`:
  `ninja exit=0`, no compiler warning in any touched file.
- `meson test -C build --suite=fast --print-errorlogs`: `Ok: 105  Fail: 0`.
- `clang-tidy -p build --quiet <file>` before → after: `libvmaf.c` 55 → 0,
  `predict.c` 5 → 0, `feature_collector.c` 11 → 0, `read_json_model.cpp`
  45 → 0 (0 errors throughout; the touched headers `libvmaf_priv.h` and
  `feature/feature_collector.h` report nothing through `HeaderFilterRegex`).
- cppcheck (four-file scope): 22 findings on master → 0 apart from the
  whole-program `unusedFunction` artefact described above.
- Bit-exactness: the four touched TUs were swapped to their `origin/master`
  content, `libvmaf.so.3` rebuilt from the same compile database, then the
  HEAD and master libraries were run through the same `build/tools/vmaf` on
  the three Netflix golden pairs, the identical-pair control and the three
  transform-model checkerboard cases from `vmafexec_test.py` at
  `--precision max --json`. Every output is byte-identical after dropping
  the wall-clock `fps` line:

  ```text
  pair1_src01_hrc00_vs_hrc01: diff=IDENTICAL vmaf.mean=76.66783086300072
  pair1b_src01_hrc00_vs_self: diff=IDENTICAL vmaf.mean=99.94642667522777
  pair2_checker_0_0_vs_1_0: diff=IDENTICAL vmaf.mean=35.0686714193046
  pair3_checker_0_0_vs_10_0: diff=IDENTICAL vmaf.mean=7.985899011514694
  pair3_transform_add40: diff=IDENTICAL vmat.mean=32.75743231560268
  pair3_transform_add40_outltein_noclip: diff=IDENTICAL vmat.mean=-7.2425676843973195
  pair3_transform_add40_piecewiselinear: diff=IDENTICAL vmat.mean=8.262601306670776
  ```

  Against the golden assertions (all `places=4`): `vmafexec_test.py`
  76.66783025 / 99.946416604585025 (pair 1 and its control),
  32.757433750978919 / −7.2425662490210838 / 8.262602639723815 (checkerboard
  0_0 vs 10_0 through the three transform models — the polynomial, the
  `out_lte_in` rectification and the piecewise-linear knot list in
  `predict.c`), and `quality_runner_test.py` 35.06866714286451 /
  7.985898744818505 (checkerboard 0_0 vs 1_0 and 0_0 vs 10_0 with
  `vmaf_v0.6.1.json`) — every value agrees to 4 decimal places.
- `libvmaf.c` also passes `cc -fsyntax-only` with `-DHAVE_CUDA=1
  -I/opt/cuda/include` and with `-DHAVE_SYCL=1` added to its compile command,
  so the backend islands the rework moved into helpers still parse; the
  full GPU builds run in CI.
- `scripts/ci/check-copyright.sh` (exit 0) and `scripts/ci/assertion-density.sh`
  (`PASS`, 255 asserts across 151 fork-added functions) on the touched files;
  `scripts/release/concat-changelog-fragments.sh --check` and
  `scripts/docs/concat-adr-index.sh --check` exit 0.
