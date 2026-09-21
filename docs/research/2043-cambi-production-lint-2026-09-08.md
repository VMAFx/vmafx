# Research-2043: CAMBI production lint without numerical changes

## 2026-09-21 strict-clean follow-up

The earlier 2026-09-08 cleanup documented ten narrow `unusedFunction`
annotations and callback/null-pointer suppressions in `cambi.c`. That posture
is superseded: the file now has zero `NOLINT` and zero Cppcheck suppression
markers. The production fixes make every retained helper live, bound every
search loop, and express the shared callback constraint in code while keeping
the CAMBI score path byte-for-byte stable.

### Findings and code fixes

| Finding | Root cause | Strict-clean implementation |
| --- | --- | --- |
| `modernize-use-nullptr` | Clang treats this C23 translation unit as supporting `nullptr`, while MSVC's C mode still needs `NULL`. | `CAMBI_NULL_POINTER` selects `nullptr` except under `_MSC_VER`, where it selects `NULL`. |
| `misc-use-internal-linkage` | The extractor registry definition had no prior external declaration in this translation unit. | A matching `extern VmafFeatureExtractor vmaf_fex_cambi` declaration records the cross-TU contract. |
| `constStatement` | The TVI bisection's exhaustive flag handling ended in an impossible `(void)0` branch. | A fixed 16-step bisection has only the two state updates and the successful return. |
| `constParameterCallback` | The shared extractor ABI supplies mutable picture pointers although CAMBI only reads them. | `read_only_picture_view()` adapts the ABI arguments to const views before validation and preprocessing. |
| `unusedFunction` on ten private exports | Seven helpers were only called by optional GPU translation units and three were scaffold-only. | CPU/reference paths now use the same ten wrappers directly; the wrappers remain available to CUDA, HIP, SYCL, and Metal twins. |
| HISS-04 long function | The public option table occupied most of the translation unit's largest declaration. | `CAMBI_OPTION` expresses the same option records compactly, without changing names, aliases, defaults, limits, help text, or order. |

Three unbounded constructs were also made finite without changing the legal
score path:

- TVI bisection performs 16 iterations, enough to exhaust the `uint16_t`
  sample domain after its existing endpoint checks.
- VLT scans through `UINT16_MAX` and returns that endpoint when no sample can
  meet the requested luminance. The previous `uint16_t` increment wrapped to
  zero forever for an unreachable threshold.
- Quick-select bounds its outer work by `n` and each partition scan by the
  initial partition span. Pivot choice, comparison direction, and swap order
  are unchanged.

Initialization now returns through `fail_init()` instead of `goto`, and the
`SWAP_FLOATS` statement macro is an inline function. `ceil_log2()` is a
32-step `for` loop. These are control-flow changes only; constants,
accumulation order, option semantics, feature names, and public callback types
are unchanged. This is an internal bug/lint cleanup, so no new ADR, public
usage document, or FFmpeg patch-stack refresh is required.

### Alternatives considered

| Alternative | Decision | Reason |
| --- | --- | --- |
| Keep narrow analyzer suppressions | Rejected | The whole-tree standard requires touched source to express the invariant in code. |
| Delete helpers unused by the CPU build | Rejected | They are the stable host seam for optional GPU twins and documented rebase invariants. |
| Change the shared extractor callback ABI to const pointers | Rejected | That would fan out across every extractor for a CAMBI-local read-only contract. |
| Bound the existing algorithms and route CPU work through retained helpers | Chosen | It removes the warning causes locally and preserves calculation order and cross-backend seams. |

### Focused regression coverage

`core/test/test_cambi.c` directly includes the shipped implementation and now
adds three termination/order checks:

- threshold extrema across differences 1, 2, 4, 8, 16, and 32 exercise the
  bounded TVI search and verify the returned transition;
- an infinite luminance threshold proves VLT terminates at `UINT16_MAX`;
- all-equal and descending inputs pin quick-select progress and partition
  order.

The test binary reports 25/25 passing cases. Before the VLT bound was added,
the infinite-threshold regression timed out, so the test distinguishes the
fixed implementation from the old wraparound loop.

### Validation receipt

The strict-clean tree was compared with base `e0d31c5bc` using the repository's
48-frame 576x324 fixture, with prediction disabled and `--precision max`.
Both dispatched CPU and `--cpumask 0` scalar runs produce mean CAMBI score
`0.51441210777008473`. After removing volatile `version` and `fps` fields,
all four base/current JSON outputs are byte-identical with SHA-256
`2fed4234f9f8c018ac9af0810dbf43a0c7a30765bee00e4e55c23de983a1a523`.

Validation commands:

```text
ninja -C build test/test_cambi tools/vmaf
meson test -C build test_cambi --print-errorlogs
clang-tidy -p build core/src/feature/cambi.c --extra-arg=-flto
cppcheck --enable=all --check-level=exhaustive --inline-suppr \
  --library=posix --library=scripts/ci/cppcheck-public-entrypoints.cfg \
  --suppressions-list=.cppcheck-suppressions.txt \
  --project=build/compile_commands.json \
  --file-filter='*/core/src/feature/cambi.c' --error-exitcode=1
praetorctl audit -touched core/src/feature/cambi.c,core/test/test_cambi.c
```

Clang-tidy and Cppcheck report zero findings in `cambi.c`; the reduced-context
Cppcheck command still reports the unrelated inline helper in untouched
`feature_collector.h`, demonstrating that `unusedFunction` remains enabled.
The touched HISS audit is clean at 1,330 active findings against the 1,411
baseline. No baseline, Netflix golden assertion, or snapshot was changed.

Durable run receipts belong under
`.workingdir/evidence/2026-09-21-cambi-strict-clean/`; `.workingdir2` is not a
repository interface and is not referenced by the new workflow.

## Historical 2026-09-08 receipt

The original cleanup made five const/shadow corrections and retained ten
private helper trampolines using exact annotations because the CPU profile did
not call them. It measured byte-identical `.text`/`.rodata` and zero focused
clang-tidy findings with the then-current suppressions. That evidence remains
useful as the starting point, but its annotation policy and `.workingdir2`
location are historical and no longer describe the source.
