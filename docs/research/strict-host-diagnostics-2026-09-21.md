# Strict host and libsvm diagnostics cleanup

## Scope

The cleanup started from a complete Intel oneAPI host-command replay, then
removed the file-wide analyzer cordon from `core/src/svm.cpp` and rebuilt the
fast suite with Clang ASan+UBSan. Those three measurements exposed distinct
sets of diagnostics:

| Measurement | Findings | Sources |
| --- | ---: | --- |
| Initial oneAPI host replay | 60 | feature-context growth, model and SSIMULACRA2 tests, libsvm overrides, SYCL CLI config |
| Strict `svm.cpp` clang-tidy run after removing its cordon | 544 | bundled libsvm implementation |
| Follow-up oneAPI builds | 2 | feature-extractor pool allocation bounds |
| Clang sanitizer build | 3 | integer-PSNR option sentinel and two SSIMULACRA2 matrix fallthroughs |
| Strict CUDA clang-tidy on `speed_chroma_cuda.c` | 13 | oversized pipeline/resource functions, widening conversions, host-pointer selection |
| Whole-tree HISS replay | 1,307 | pre-cleanup tree, including 35 real findings in files owned by this batch |

The counts are measurements, not a unique total: the three libsvm override
diagnostics from the compiler also occur in the strict analyzer set. No warning
was disabled, no baseline was increased, and no Netflix golden assertion or
numeric tolerance changed.

## Root causes and repairs

### Same-width allocation bounds

Both feature-context growth implementations compared a 32-bit `unsigned`
capacity directly with a `SIZE_MAX`-derived allocation limit. On 64-bit hosts
the byte limit is larger than every possible capacity, so the comparison is
provably false and the compiler reports it.

The implementations now derive the smaller of the count limit and byte limit,
then compare values of the same width. This retains the meaningful byte guard
on 32-bit targets and the `UINT_MAX / 2` doubling guard everywhere. The pool's
per-thread context allocation uses the same pattern.

### Complete initialization and explicit control flow

- All default `VmafModelConfig` values use `{0}`, which initializes the full
  aggregate and any future members.
- The integer-PSNR option table terminates with a full `{0}` sentinel.
- Full-range BT.709 and BT.601 switch arms assign their coefficients and exit
  explicitly in both the SSIMULACRA2 implementation and its scalar reference
  test. The arithmetic and `limited = 0` state are unchanged.
- The CLI explicitly disables SYCL profiling, and its local configuration is
  const after initialization.
- Solver virtual replacements state `override`; host-only temporaries and
  return-code variables are initialized or scoped to their first use.

### libsvm is in whole-tree scope

`svm.cpp` is no longer treated as an untouched upstream mirror. Removing its
whole-file `NOLINT` block exposed 544 findings rather than making them disappear.
The source was repaired to the same profile as the rest of the tree:

- internal types and helpers have anonymous-namespace linkage; owning classes
  are non-copyable, solver state is private, and virtual replacements are
  explicit;
- the `Malloc` compatibility surface is backed by a typed, overflow-checked,
  zero-initializing allocator; zero-count allocations still return usable
  storage and every failure is reported before aborting;
- model parsing validates header order, duplicate vectors, dimensions,
  support-vector counts and terminators before publishing either storage plane;
  exception paths retain one owner and free partial state;
- model saving checks every formatted write and always closes the stream,
  including a stream-error path that previously could skip `fclose()` because
  of short-circuit evaluation;
- cross-validation shuffling uses a thread-local `std::mt19937` seeded from
  `std::random_device`, eliminating process-global `rand()` state and modulo
  bias; bounds remain inclusive;
- sigmoid training uses `log1p(exp(x))` for the actual `log(1 + exp(x))`
  operation, avoiding avoidable cancellation; loaded-model prediction and
  feature-score arithmetic are unchanged;
- oversized solver, trainer, probability and parser blocks are split at their
  existing algorithmic phases. Solver update expressions and their order are
  preserved.

The stale inline Cppcheck false-positive suppression on the `fdopen()` failure
path was removed as well. The corrected POSIX model used by current CI models
descriptor ownership correctly.

### Structured control flow and CUDA SpEED ownership

The touched feature-extractor, SSIMULACRA2, CUDA SpEED, SIMD test and CLI paths
no longer rely on oversized functions, cleanup jumps, or function-size
exceptions. Resource ownership is explicit at the subsystem boundaries:

- feature-extractor context and pool creation use named failure helpers and
  return through structured paths;
- SSIMULACRA2 color conversion separates matrix selection and pixel emission
  while preserving the exact `fmaf` sequence in production and the byte-exact
  scalar SIMD reference;
- CUDA SpEED allocation, launch, download and teardown are split by resource
  class; a failed context pop is retried once before its original error is
  returned, preserving the prior context-stack recovery behavior;
- `vmaf.cpp` uses `CliRunState` plus one explicit `cleanup_cli_run()` call after
  `execute_cli()`, replacing 16 cleanup jumps. Model destruction still follows
  VMAF/GPU/input teardown. The state retains the original CUDA allocation so
  cleanup can honor the public `vmaf_close()` then `vmaf_cuda_state_free()`
  lifetime contract; the old CLI lost that allocation after importing a copy
  and leaked it on every CUDA invocation. VMAF-close, CUDA-state-free, pending
  input-close and JSON-amendment failures are now reported and propagated
  rather than discarded. The Windows console guard remains static so
  `cli_parse()`'s direct `exit()` paths restore console state;
- typed picture-copy templates remove the CLI's per-frame function-size
  exception without adding indirect calls or changing row/sample arithmetic.

This lowered the same whole-tree HISS measurement from 1,307 to 1,272, a
35-finding reduction. The 13 reports still emitted for touched translation
units are parser defects, not accepted debt: HISS currently mistakes C/C++
namespace and `extern "C"` blocks plus large constant-array initializers for
functions. No source was distorted and no suppression or baseline was added to
hide those reports; the scanner repair is a separate integration dependency.

### CUDA gpumask smoke-test workload

`test_vmaf_cuda_gpumask` previously ran four complete 1920x1080 two-frame
scores under one 10-second Meson hang-detector budget. In a full concurrent
suite the first CUDA score completed, then the healthy CPU fallback invocation
was killed at 10.09 seconds. The test asserts dispatch selection, not
throughput, so its fixture is now 576x324 with the same model, two-frame
temporal path and four gpumask/PSNR variants. The existing `nvidia-smi -L`
no-device guard remains responsible for skipping unavailable hardware; the
timeout remains a hang detector rather than a performance assertion.

## Behavior boundaries

There is no public libvmaf ABI or CLI-schema change. Valid serialized models
load and score through the same prediction code. Malformed models are rejected
earlier, allocation failure is consistently fatal inside libsvm's C API, and a
failed model save now closes its descriptor and returns `-1` reliably.

Two training-only details intentionally improve behavior: cross-validation
folds no longer depend on global `rand()` state, and probability calibration
uses the numerically stable standard-library primitive. Neither path is used
when VMAF loads a shipped model for scoring.

## Alternatives considered

| Option | Result | Decision |
| --- | --- | --- |
| Keep the libsvm file-wide analyzer cordon | Leaves 544 findings unchecked solely because of source origin | Rejected |
| Raise tidy/Cppcheck baselines | Makes the diagnostics invisible without fixing any cause | Rejected |
| Delete allocation-byte guards | Removes 64-bit warnings but loses real 32-bit overflow protection | Rejected |
| Add fallthrough or warning annotations | Preserves warning-prone control flow and adds suppression-like metadata | Rejected |
| Retain the CLI cleanup jump spine and function-size exceptions | Leaves executable control-flow debt behind a historical justification | Rejected |
| Raise the gpumask test timeout around four 1080p scores | Keeps throughput and runner contention inside a dispatch-smoke contract | Rejected |
| Keep C varargs and unchecked stdio | Retains format and short-write blind spots | Rejected |
| Refactor in source and preserve tested contracts | Removes the diagnostic causes and keeps checks executable | Chosen |

No new ADR is required. ADR-1142 already decides that the strict standards
apply to the whole tree; this change implements that decision and fixes defects
found while doing so.

## Validation

- Intel oneAPI 2026.0 rebuilds of the affected host translation units complete
  without their prior compiler diagnostics.
- Strict clang-tidy reports zero warnings, zero uncited `NOLINT` directives and
  zero compile failures for `svm.cpp`, `feature_extractor.cpp`, `vmaf.cpp`,
  `integer_psnr.c`, `ssimulacra2.c`, `test_model.c`, and
  `test_ssimulacra2_simd.c`; CUDA-lane runs are likewise zero for
  `speed_chroma_cuda.c` and `vmaf.cpp`.
- Cppcheck 2.22 with the CI configuration reports zero findings for
  `svm.cpp`; the same focused run is clean for `vmaf.cpp`.
- A Clang `address,undefined` build passes the complete fast suite: 144/144.
- The focused model, parser, prediction, feature-context, SSIMULACRA2 SIMD and
  integer-PSNR tests pass under ASan+UBSan.
- ShellCheck and `sh -n` accept the gpumask script. Its two CPU variants at
  576x324 complete in 0.008 and 0.005 seconds on the workstation; the CUDA
  variant is validated only when the device is not occupied by another job.

## Delivery declarations

- Human-facing documentation: no user-visible surface was added; behavior and
  failure-boundary changes are recorded here and in the changelog.
- ADR: existing ADR-1142 governs whole-tree enforcement; no new architecture or
  public contract was selected.
- Agent invariant: `core/src/AGENTS.md` forbids restoring the libsvm cordon;
  CUDA and CLI package instructions record structured ownership, context-pop,
  console-restoration and gpumask fixture invariants.
- Reproducer: run the strict tidy ratchet on each touched translation unit,
  Cppcheck on `svm.cpp`, and `meson test --suite fast` in an ASan+UBSan build.
- Rebase impact: recorded in `docs/rebase-notes.md`.
