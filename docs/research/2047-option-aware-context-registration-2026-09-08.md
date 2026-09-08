<!-- markdownlint-disable MD013 -->
# Research-2047: Option-aware feature context registration

## Reproduction and scope

Base: `2143174c7bf933f97aec610f423c559a885b8e9a`. Both
`vmaf_use_feature()` and `vmaf_use_features_from_model()` construct contexts
with parsed options before calling `feature_extractor_vector_append()`.
The old vector compares advertised base feature names and destroys the second
context before considering its options. Real `motion_force_zero=false` and
`true` contexts generate different SAD keys, but registration returns success
and keeps only one. The existing 18-case feature-extractor suite passes because
its twin-overlap test uses equivalent defaults.

This is an active registration defect, independent of GPU availability. The
public regression registers both motion variants, extracts a blank frame,
flushes, and reads both `VMAF_integer_feature_motion2_score` and
`integer_motion2_force_0`. The existing name builder aliases the nondefault
motion2 key; no naming or numerical formula changes are part of this fix.

## Correction and invariants

For each shared advertised base, compare canonical keys generated from each
extractor's parsed feature parameters. Preserve ADR-0385's first shared match
and first-registration ownership, including equivalent CPU/GPU twins. The
existing absent-list fallback still compares extractor-name/options keys.
Defaults and aliases normalize through the same existing name builder that
extractors use to populate their feature-name dictionaries.

Comparison strings have local RAII ownership with the C allocator's deleter.
Failure to allocate either returns `-ENOMEM`, leaving the incoming context
with the caller. Pointer-table doubling checks both its `unsigned` capacity
and `size_t` byte limit before arithmetic or allocation. Failure leaves the
existing pointer, count, capacity and contexts unchanged. The same helper is
tested directly at its representable bounds; no huge allocation is required.

The published pointer array still uses `malloc`/`realloc`/`free`, preserving
the existing C-visible layout, order and normal doubling. The prologue's
obsolete `std::vector`/`try`/`catch` claims were removed. Its five no-malloc
markers suppressed a check not enabled by the current profile and are removed.
No public declarations, feature calculation, callback type or Netflix golden
assertion changes.

## Regression controls

- Fourteen private Linux cases: distinct real motion options; explicit
  defaults; canonical/alias equality; identical custom options; equivalent
  and distinct mock backend twins; absent-list fallback; count/byte boundary
  arithmetic; real 8→16→32 growth; both comparison allocation failures and
  actual `realloc` failure with retry. Portable hosts run the eleven cases
  that do not depend on GNU link wrapping.
- One public case lives in a separate executable linked only to libvmaf.
  A combined private/public executable can export internal vector symbols
  and interpose on an ELF shared library, invalidating an old-library control.
  The separate target avoids that test-harness defect.
- GNU `__wrap_*` entry points exist only in the Linux test target. Their exact
  declaration markers preserve the linker ABI; their `unusedFunction` markers
  cover entry points invoked by linker substitution. No production test hook,
  diagnostic category exclusion or global suppression is introduced.
- Existing `test_feature_extractor` remains unchanged with all 18 cases.

Validation receipts, exact commands, source hashes, failed harness iterations
and limits are retained under
`.workingdir2/evidence/2026-09-08-fex-context-vector/`.
Disposable builds and logs are under
`.workingdir2/cache/fex-context-vector-20260908/`.

## Validation results

- GCC 15.2 release CPU build with default LTO: 14 private cases, one public
  shared-library case, and the unchanged 18-case feature-extractor suite pass.
- ASan/UBSan with leak detection: the same 33 cases pass in a separate debug
  build with LTO disabled for instrumentation.
- Exact base-vector controls fail at the intended checks: private registration
  count and public option-specific score retrieval. The shared-only executable
  resolves the retained old library through `LD_LIBRARY_PATH`; its loader
  receipt is preserved alongside the full link commands.
- clang-tidy 22.1.8: production warnings 3→0, uncited markers 5→0; both new
  test TUs and the private helper are clean. Cppcheck 2.21.1 with the official
  POSIX model, all checks and exhaustive configured analysis exits zero for
  all touched native sources and their real test caller. The scoped writer
  tightens only the measured production allowance and registers the clean tests.
- A 32-bit compile check was attempted but this image lacks the 32-bit
  libstdc++ `bits/c++config.h`. No 32-bit execution is claimed; native boundary
  tests cover the actual host limits, and the source checks byte and count
  limits independently before doubling. No toolchain was downloaded.

## Reproduce

In a configured CPU build with generated dependencies available:

```sh
ninja -C build test/test_feature_extractor test/test_fex_ctx_vector test/test_fex_ctx_vector_public
meson test -C build --no-rebuild --print-errorlogs \
  test_feature_extractor test_fex_ctx_vector test_fex_ctx_vector_public
```

Use the retained old-object/old-library adapter for negative controls: it
compiles the exact base vector with current compatible build flags, then
substitutes only that vector in a disposable executable/shared library.
Normal insertion and numerical kernels remain from the configured build.

No new ADR is needed: this restores option identity while preserving the
existing ADR-0385/ADR-0723 registration and ownership design. Alternatives
such as comparing raw dictionary spelling or backend-name prefixes would
misidentify defaults/aliases or require a separate backend list.

## Limits

CPU-only tools container; no GPU device/runtime acceptance, native Windows
acceptance or full-tree lint claim. SIMD and numerical kernels are unchanged.
The parent integration owns complete native and golden gates. The separate
model-registration dictionary failure path remains outside this commit.
