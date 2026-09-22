# Research-2075: High-signal non-CUDA Cppcheck cleanup

## Scope and diagnosis

At integration head `e0d31c5bcb43a7aab7fb845a592488635b061566`, a
fresh GCC CPU build with DNN and float extractors enabled reproduced twelve
owned exhaustive Cppcheck findings across eight native files: one
`unassignedVariable`, three `unreadVariable`, two `variableScope`, two
`knownConditionTrueFalse`, two `clarifyCondition`, one `usleepCalled`, and
one `redundantInitialization`. Other whole-tree findings are outside this
bounded batch and remain visible; no suppression or baseline changes are
used.

Those findings came from configuration-wide variable lifetimes, assignments
overwritten before their first read, a high-bit-depth shift whose zero case
returned earlier, Xiph loop initializers parsed as assignment-plus-comparison
expressions, and an obsolete POSIX delay call in a contention test. The SpEED
`ak` locals are aliases of `ap1`; storing `ap1` directly preserves the same
float value and write order in both mirrored CPU helper TUs. Backend and
pooled-score error locals move to the smallest scopes that consume them. The
benchmark GPU initializer remains compiled and checked only when CUDA or SYCL
support exists.

The first ordinary commit attempt then exposed 32 baselined HISS findings in
the touched files. Project policy now requires touched-file zero, so the batch
expanded rather than using a debt exception. The findings were four classes:

- file-wide C++ linkage blocks and a C++ `extern "C"` wrapper that the scanner
  treated as functions;
- long declarative SpEED option tables;
- oversized CLI, benchmark, validation and picture-pool test functions; and
- cleanup `goto` ladders in `vmaf` and `vmaf_bench`.

The DNN template contains only macros and `static inline` definitions, so its
outer C-linkage wrapper had no linkage effect and was removed. `vmaf.cpp` now
uses short, reopened anonymous-namespace blocks plus `CliRunState` /
`CliRunGuard`; the blocks preserve internal linkage while remaining below the
scanner's 60-line limit. Teardown still runs in the established order
(context, GPU state, input readers, files, CLI settings, model arrays).
Pixel-copy branches retain their original loop and sample order. GPU
initialisation remains SYCL, CUDA, HIP, then Metal.
`vmaf_bench` uses one cleanup helper for benchmark/validation resources and a
separate SYCL profiling owner. The compact option rows retain their original
registration order, names, aliases, defaults and bounds.

No alternatives: these are direct expression, lifetime and ownership
cleanups under existing contracts. No new architecture or policy is added, so
no ADR is needed. The updated package invariant records the structured cleanup
ownership, and the bench guide records failure-status behavior.

## Preservation controls

The scalar Xiph DCT, SpEED arithmetic expressions, pooling ranges, backend
selection order and picture-pool delay durations remain unchanged. Existing
DISTS high-bit-depth normalisation, SpEED, motion-minimum,
picture-preallocation and AVX2 PSNR-HVS tests exercise the touched paths. The
benchmark now propagates SYCL frame/flush errors and validation flush errors
instead of overwriting them with later success. The before/after proof uses
the same generated Meson compile database and Cppcheck 2.22.0 command:

```sh
cppcheck --enable=all --check-level=exhaustive --inline-suppr \
  --library=posix --library=scripts/ci/cppcheck-public-entrypoints.cfg \
  --suppressions-list=.cppcheck-suppressions.txt \
  --project=build/compile_commands.json --error-exitcode=1
```

The expected whole-tree Cppcheck exit remains non-zero for unrelated findings.
Success for this batch means all twelve owned Cppcheck records disappear and
`standardsctl audit` reports every touched file clean. The final governance
scan reports 1304 active findings within the 1411-entry baseline, down from
1336 while exposing no new touched-file debt. This does not claim that
excluded CUDA, framesync, CAMBI, model-tooling, libsvm, cJSON, Python or
release findings are closed.
