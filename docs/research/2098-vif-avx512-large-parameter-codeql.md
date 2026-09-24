# Research-2098: Resolution of CodeQL AVX-512 large-parameter alerts

Date: 2026-09-24. Base: `4e6916d16ac57647105d14a47a6680117d6b5738`.
Scope: `core/src/feature/x86/vif_avx512.c` and
`core/test/test_integer_vif_avx512_stages.c`.

## Diagnosis

GitHub Code Scanning flagged five instances of `cpp/large-parameter` in
`core/src/feature/x86/vif_avx512.c`:

- **Alert 1108** (`vif_vertical_energy16`, line 619): parameter
  `VifPair512 weighted` (128 bytes).
- **Alert 1109** (`vif_vertical_store8`, line 518): parameter
  `VifPair512 acc` (128 bytes).
- **Alert 1110** (`vif_vertical_energy8`, line 497): parameter
  `VifTaps8 a` (256 bytes).
- **Alert 1111** (`vif_vertical_energy8`, line 497): parameter
  `VifTaps8 b` (256 bytes).
- **Alert 1112** (`vif_vertical_mean8`, line 487): parameter
  `VifTaps8 t` (256 bytes).

These helpers were extracted in commit `6800d0f011bb` (Research-2046) to
decompose oversized kernels under Clang `readability-function-size`. During
helper extraction, large vector aggregate structures (`VifPair512` holding two
`__m512i` registers = 128 bytes; `VifTaps8` holding four `__m512i` registers =
256 bytes; `VifEnergy512` holding four `__m512i` registers = 256 bytes) were
passed by value.

### ABI Analysis: Accidental Copies vs Register Passing

Under both the System V AMD64 ABI (§3.2.3 Parameter Passing) and the Microsoft
x64 ABI, aggregate objects whose size exceeds 64 bytes (eight eightbytes) cannot
be passed in vector registers (`%zmm0`–`%zmm7`). The System V ABI classifies
any aggregate larger than eight eightbytes as class `MEMORY`. When an aggregate
is passed by value out-of-line:

1. The caller must allocate stack space and copy the aggregate into memory.
2. The callee reads from that stack copy.

These parameters were therefore **not deliberately register-passed**; they were
accidental memory-copy parameters under the ABI whenever out-of-line calling
conventions apply. Passing them by `const *` passes a single 8-byte pointer in
a general-purpose register (`%rdi`, `%rsi`, `%rdx`, `%rcx`, etc.), eliminating
caller stack allocation and copy overhead without changing semantics.

### Pointer preconditions, aliasing, and lifetime

The pointer conversion does not introduce a new externally observable
precondition. Every helper is private, forced-inline, and called only with the
address of a live automatic object in its caller. No pointer is stored,
returned, or used after that object's lifetime. Every aggregate input is
`const`, including the intentional same-object calls
`vif_vertical_energy8(&acc.ref, &r, &r, ...)` and
`vif_vertical_energy8(&acc.dis, &d, &d, ...)`; those aliases are read-only and
therefore safe. All enumerated call sites pass `&object`, so no null path exists
inside the private contract and no hot-path null branch is required.

## Implementation

Internal static forced-inline helpers in `core/src/feature/x86/vif_avx512.c`
were converted to accept aggregate vector structs via `const *`:

1. `vif_horizontal_energy_pack512(const VifPair512 *acc)`
2. `vif_vertical_mean8(VifPair512 *acc, const VifTaps8 *t, __m512i coeff)`
   (Alert 1112)
3. `vif_vertical_energy8(VifPair512 *acc, const VifTaps8 *a, const VifTaps8 *b,`
   `__m512i f0, __m512i f1)` (Alerts 1110, 1111)
4. `vif_vertical_store8(uint32_t *dst, const VifPair512 *acc)` (Alert 1109)
5. `vif_vertical_store_mean8(uint32_t *dst, const VifPair512 *acc)`
6. `vif_vertical_energy16(VifEnergy512 *acc, const VifPair512 *w, __m512i pix)`
   (Alert 1108)
7. `vif_vertical_store_mean16(uint32_t *dst, const VifPair512 *acc, int r,`
   `int s)`
8. `vif_vertical_store_energy16(uint32_t *dst, const VifEnergy512 *acc, int r,`
   `int s)`

All call sites in `vif_horizontal_energies512`,
`vif_vertical_statistics8_block`, and `vif_vertical_statistics16_block` pass
pointers (`&`) directly.

### Public ABI and Header Integrity

`core/src/feature/x86/vif_avx512.h` exports only:

- `vif_subsample_rd_8_avx512`
- `vif_subsample_rd_16_avx512`
- `vif_statistic_8_avx512`
- `vif_statistic_16_avx512`

None of the modified functions are declared in headers or exported from the
static library `libx86_avx512.a`. The public ABI and library ABI are strictly
unchanged.

## Compiler and Assembly Verification

Because the internal helpers are marked `FORCE_INLINE` (`static inline`
`__attribute__((always_inline))`), GCC `-O3` folds pointer dereferences
directly during SSA optimization.

Measurement conditions: GCC 16.2.1, x86-64, System V ABI, `-O3`, with
`origin/master` and this branch built independently under matching Meson
options. In
`build/src/libx86_avx512.a.p/feature_x86_vif_avx512.c.o`:

- The complete hot `.text` section is byte-for-byte identical: both files are
  `0x4f04` bytes and have SHA-256
  `80b48e27e202ca98c3a124351740fdecba1e19df53adeebbcd237889fe44374c`.
- Every public AVX-512 symbol retains the same address and size.
- `.text.unlikely` has the same instructions and relocations except for six
  `__assert_fail` line-number immediates, each shifted by six source lines.

The structural stack-alignment scanner also reports zero findings on the
host-built SysV object. That is useful regression evidence, but it is not a
Win64 proof: no MinGW cross compiler is installed locally, and the scanner's
definitive target is the MinGW-built object from the hosted Windows job. Hosted
Win64 acceptance therefore remains pending.

## Focused Test Harness & Bit-Exactness

`core/test/test_integer_vif_avx512_stages.c` was enhanced with:

1. The `test_integer_vif_avx512_stages_red_check` case inside the
   `test_integer_vif_avx512_stages` executable:
   - Asserts baseline bit-exactness across scalar, AVX2, and AVX-512.
   - Proves red-capability by introducing a 1-bit perturbation in reference
     input pixels and asserting that the harness detects the discrepancy.
   - Proves red-capability by introducing a 1-bit perturbation in intermediate
     stage plane buffers and asserting detection.
2. Tri-way comparison:
   - Evaluates scalar vs AVX-512 vs AVX2 across 9 bit depths (8–16 bpc), 12
     widths (9, 15, 16, 17, 31, 32, 33, 63, 64, 65, 127, 257), 4 heights
     (7, 17, 18, 24), 4 scales (0–3), and 2 synthetic patterns (3,456
     combinations).
   - Compares exact floating-point result bits and 5 intermediate plane
     buffers.

### Verification Results

- Focused VIF set: 5/5 pass, including
  `test_integer_vif_avx512_stages` and its red-check case.
- Configured CPU fast suite: 147 pass, 0 failures.
- Fresh CodeQL replay: CodeQL CLI 2.27.0, C++ query pack 1.8.3, and
  `Critical/LargeParameter.ql` over a fresh focused extraction of
  `vif_avx512.c`. The branch CSV has **0 rows**; the same exact query against
  the master-derived baseline database has the five rows matching alerts
  1108–1112.
- AddressSanitizer + UndefinedBehaviorSanitizer (`build-san`): 5/5 clean across
  `test_integer_vif_avx512_stages`, `test_integer_vif_log2`,
  `test_vif_skip_scale0`, `test_vif_simd`, and `test_integer_vif_avx2_stages`.
- Netflix CPU golden gate: 271 passed, 12 skipped, 0 failed. No Netflix golden
  assertion or fixture is changed. The hosted Windows build remains the
  post-push Win64 acceptance check.
- Format & lint:
  - Clang-format dry-run on both changed C files: pass.
  - Clang-tidy 22.1.8 with all diagnostics promoted to errors: no touched-file
    warning. The report contains two inherited-header diagnostics outside the
    changed files (`core/src/cpu.h` and `core/src/x86/cpu.h`).
  - Cppcheck 2.22.0 with the required CI POSIX/public-entrypoint models,
    exhaustive checking, and a file-filter-specific `unusedFunction`
    suppression: no source or test finding. The ordinary changed-files
    preflight also passes without that suppression.
  - `make verify-all` and `praetorctl audit -base origin/master`: governance
    audit, context compilation, HISS coverage, and deduplication all pass.
    The prior claim that touched files were clean under the 276 baseline was
    false; `vif_avx512.c` contained two baselined HISS-04 violations in
    `vif_subsample_rd_8_vert_j` (111 LOC) and `vif_subsample_rd_8_horiz_j`
    (132 LOC), which blocked `praetorctl audit -base origin/master`. Under
    ADR-1289 and ADR-1298, `-touched-debt-delta-reason` is rejected. The root
    cause is now resolved: both functions retain their outer boundary
    `static VMAF_NOINLINE_NOCLONE` (ADR-0503) and use bounded macros
    (`VIF_VERT_LOAD10_REF`, `VIF_VERT_LOAD10_DIS`, `VIF_VERT_MADD5`, and
    `VIF_HORIZ_TAP8`) to reduce function lengths to 45 LOC and 36 LOC
    (both $\le 60$ LOC). Forced-inline function helpers were proven to
    perturb GCC's register allocation due to address-taken vector pointers
    (swapping `%zmm3` and `%zmm13`), whereas macros preserve 100% byte-identical
    `.text` machine code (SHA-256:
    `80b48e27e202ca98c3a124351740fdecba1e19df53adeebbcd237889fe44374c`).
    Baseline debt is re-recorded from 276 to 274, the README governance block is
    synchronized, and all touched files are genuinely HISS clean.

## Decision Matrix — no alternatives: only-one-way fix

CodeQL `cpp/large-parameter` fires on an aggregate parameter exceeding 64 bytes.
The only compliant fix that preserves the `FORCE_INLINE`
inlining contract is to pass large aggregates via `const *`. Every alternative
was evaluated:

| Option | Outcome | Why not chosen |
| --- | --- | --- |
| Pass by `const *` (chosen) | Clears alerts; ABI-identical under inlining (GCC 16 SysV -O3 measured) | — |
| Pass by value (keep as-is) | Alerts persist on every CI push | Does not fix the issue |
| Suppress with `// NOLINTBEGIN(cpp/large-parameter)` | CodeQL uses SARIF suppress, not inline; not available here | Not a valid suppression mechanism |
| Merge helpers back into caller | Reverses ADR-0503 noinline fission; restores ~30-ZMM spill cluster | Contradicts ADR-0503 and re-opens HISS-04 |
| Split aggregates into scalar fields | Destroys type safety; unbounded call-site churn | No benefit; breaks ADR-0139 pin |

## Reproducer

To reproduce the original CodeQL alert state (before fix):

```sh
git show origin/master:core/src/feature/x86/vif_avx512.c \
  | rg -n "vif_vertical_mean8|vif_vertical_energy8|vif_vertical_energy16|vif_vertical_store8"
# Shows pass-by-value signatures for VifPair512/VifTaps8 aggregates
```

To verify the fix and confirm instruction equivalence:

```sh
# Build master and branch objects, diff disassembly:
ninja -C build src/libx86_avx512.a.p/feature_x86_vif_avx512.c.o
objdump -d build/src/libx86_avx512.a.p/feature_x86_vif_avx512.c.o \
  | rg 'vif_statistic_(8|16)_avx512'
# (GCC 16 x86-64 SysV ABI -O3 only; other toolchains not measured)

# Win64 stack check:
python3 scripts/ci/check-win64-stack-alignment.py \
  build/src/libx86_avx512.a.p/feature_x86_vif_avx512.c.o
# Expected: 0 violations

# Focused VIF tests:
meson test -C build test_integer_vif_avx512_stages \
                    test_vif_simd test_vif_skip_scale0
```
