<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1141: Rework the upstream-mirror integer ADM to the fork lint profile, bit-exact

- **Status**: Accepted
- **Date**: 2026-09-02
- **Deciders**: lusoris
- **Tags**: `lint`, `refactor`, `adm`, `simd`, `rebase`, `quality-gate`

## Context

`core/src/feature/integer_adm.c` (3643 LOC) and `core/src/feature/adm_tools.c`
(1403 LOC) are Netflix-header upstream-mirror files. ADR-0278 closed the
T7-5 NOLINT sweep by *citing* rather than refactoring the
`integer_adm.c` cluster, on the argument that reshaping Netflix `966be8d5`
kernels would multiply rebase pain and risk SIMD-vs-scalar drift. The
result was the worst lint debt in the tree: 24 NOLINT sites in
`integer_adm.c` (twenty upstream `ADM_CM_THRESH_S_*` / `ACCUM_ROUND`
macros under one `bugprone-macro-parentheses` bracket, twelve
`readability-function-size` suppressions on functions of 95 to 330 lines,
four dead `hicpp-signed-bitwise` markers for a check the fork never
enables) hiding 375 findings (335 `bugprone-macro-parentheses`, 22
`bugprone-implicit-widening-of-multiplication-result`, 15
`readability-function-size`, 2 `readability-non-const-parameter`,
1 `misc-use-internal-linkage`; measured by neutralising the markers), plus
65 live findings in `adm_tools.c`
(a 550-line `adm_cm_s`, 33 non-isolated declarations, 23 int-multiplied
pointer offsets, three externally-linked dead functions).

The c-rework wave changes the policy: upstream-mirror files that the fork
owns are brought to the ADR-0141 profile like fork-local code. Four
constraints bound how far this file may move:

1. **Bit-exactness.** The Netflix golden pairs (ADR-0024) and the
   AVX2 / AVX-512 / NEON twins (`meson test --suite=fast`) are compared
   against this scalar code. No arithmetic expression, integer width,
   rounding term or summation order may change — including the
   sign-negated ADR-0155 rounding term (Netflix#955) and the float
   summation order of the contrast-masking neighbourhood.
2. **Frozen dispatch prototypes.** `AdmState`'s stage function pointers
   and `VmafFeatureExtractor::extract` are shared with the SIMD twins in
   `x86/adm_avx2.h`, `x86/adm_avx512.h`, `arm64/adm_neon.h`; the scalar
   twin cannot constify a parameter on its own.
3. **The ADR-1057 fp-contract bracket** on `adm_dwt2_s` must keep the
   four-tap accumulation of the float DWT2 on a multiply-then-add contract
   on every compiler; the x86 golden gate cannot observe an ARM-only
   contraction change.
4. **C TUs keep `NULL`** (ADR-1138).

## Decision

We rework both files mechanically and keep every kernel expression
verbatim:

- The eighteen `ADM_CM_THRESH_S_*` / `I4_ADM_CM_THRESH_S_*` corner / edge /
  interior macros collapse into two `static inline` functions
  (`adm_cm_thresh`, `i4_adm_cm_thresh`) that derive the mirrored 3x3
  neighbourhood from `(i, j)` — the row/column before the first edge
  mirrors to index 1, the one past the last edge clamps to the last index,
  which is exactly the nine index patterns the macros hard-coded — and add
  the nine terms in the macro order. `ADM_CM_ACCUM_ROUND` /
  `I4_ADM_CM_ACCUM_ROUND` become `adm_cm_accum_round` /
  `i4_adm_cm_accum_round` over an `AdmCmBand` parameter struct. The float
  twin in `adm_tools.c` (`adm_cm_thresh3x3_s`) does the same for the
  header macros it used.
- The four-way border branch of `adm_cm` / `i4_adm_cm` / `adm_cm_s`
  becomes one `*_row` helper driven by `left_edge = left <= 0` and
  `right_edge = right > w - 1` (the same predicates the branch tested), a
  per-sample `*_accum_px` helper and a shared row fold.
- Repeated prologues become helpers: `adm_csf_factors` /
  `adm_csf_rfactor_s` (CSF weights per mode), `adm_csf_rfactor_scale0`
  (fixed-point scale-0 weights), `adm_border` / `adm_border_filt` (frame
  border with and without filter taps), `i4_adm_round_terms` (the ADR-0155
  rounding terms, still `int32_t`), `adm_den_scale_finalise`,
  `adm_num_scale`, `adm_half_shift`.
- Oversized functions split along their existing phase boundaries: the
  DWTs into vertical / horizontal passes over a tap-4 helper,
  `dwt2_src_indices_filt` into a 1-D helper called twice,
  `integer_compute_adm` into `integer_adm_scale0` / `integer_adm_scale_s123`
  reading the parameters from `AdmState`, `init` into
  `init_dispatch_scalar` / `init_dispatch_simd` / `init_buffers` /
  `free_buffers` (no `goto`), `extract` into `extract_debug_features`.
- Pointer-offset products widen to `ptrdiff_t` before the pointer add
  (the value is unchanged for every addressable buffer); the numeric
  kernel keeps its `int` / `int32_t` / `int64_t` widths.
- Two latent-UB sites are closed without changing any produced value:
  the unguarded `pow(2, shift - 1)` rounding terms in `adm_cm` /
  `i4_adm_cm` now go through `adm_half_shift` (the guard
  `adm_csf_den_s123` and `i4_adm_cm` already used for one of their shifts;
  for a zero shift x86-64 already produced 0), and the unreachable
  `rfactor[i * src_stride + w - 1]` out-of-bounds read in the dead
  "left border within frame, right outside" branch of `i4_adm_cm`
  disappears with the branch (its predicate `left > 0 && right > w - 1`
  contradicts `right = w - left`).
- Dead upstream code is removed: `adm_dwt2_lo_d` and `adm_buffer_copy`
  (no caller anywhere in the tree; not declared in `adm_tools.h`); the
  four `hicpp-signed-bitwise` markers (check not enabled).
- Exactly four suppressions survive, each citing the invariant that
  forces it: the file-scoped `modernize-use-nullptr` bracket (ADR-1138),
  `readability-non-const-parameter` + `cppcheck constParameterCallback`
  on `adm_decouple` / `adm_decouple_s123`'s `lut` (frozen SIMD dispatch
  prototype), `cppcheck constParameterCallback` on `extract`'s pictures
  (frozen `VmafFeatureExtractor::extract` prototype), and
  `readability-function-size` on `adm_dwt2_s` (ADR-1057 bracket, see
  Alternatives). The cross-TU registry `misc-use-internal-linkage`
  marker on `vmaf_fex_integer_adm` is unchanged (ADR-0278 form).

Bit-exactness is proven by a before / after CLI output matrix rather than
by the golden gate alone: 62 `vmaf --precision max` runs (the three Netflix
pairs with the `vmaf_v0.6.1` model, `float_adm`, 10-bit inputs, every
`adm` / `float_adm` option that changes a code path, and deterministic
18x20 .. 64x48 synthetic frames whose scales 1..3 reach the `left <= 0` /
`right > w-1` border branches), each with SIMD dispatch on and with
`--cpumask 4294967295` (scalar only), compare equal on 21 396 per-frame
metric values; `meson test --suite=fast` (the SIMD-twin gate) passes.

The ADR-0278 cluster row for `integer_adm.c` ("upstream-mirror parity") is
superseded by this ADR; ADR-0278 itself stays Accepted.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| **Mechanical rework, kernels verbatim, four cited suppressions (chosen)** | 375 hidden + 6 visible + 65 findings discharged; 5 063 LOC → 3 199; every border case in one closed form instead of nine macro copies; UB sites closed with identical output | Large diff on two Netflix-header files; future upstream hunks re-port by hand (rebase note) | **Decision** — the unit brief; bit-exactness is provable by the output matrix, so the ADR-0278 drift argument no longer holds |
| Cite-only (ADR-0278 status quo) | Zero rebase cost | Keeps the worst debt in the tree; the macro bracket hides real `bugprone-macro-parentheses` findings; four markers are dead | Rejected — contradicts the c-rework policy and ADR-0141 refactor-first precedence |
| Rework *and* correct the arithmetic (ADR-0155 rounding sign, `int32_t` narrowing in `adm_decouple`) | Mathematically cleaner | Moves the Netflix golden scores (hard rule #1) | Rejected — out of scope by definition; ADR-0155 stays deferred |
| Constify `int32_t *adm_div_lookup` across the scalar and SIMD prototypes | Removes two suppressions | Touches `adm_avx2.[ch]`, `adm_avx512.[ch]`, `adm_neon.h` and the CUDA/SYCL mirrors — each falls under ADR-0141 and carries its own macro debt; no parity benefit | Rejected — sibling units own the SIMD twins; suppression with citation is the ADR-0278 form |
| Split `adm_dwt2_s` with the `optimize("-ffp-contract=off")` attribute on every helper | Removes the last `readability-function-size` marker | Relies on GCC inlining across differing `optimize` attributes; the effect is ARM-only and unobservable on the x86 golden gate (the ADR-1057 history shows how that ends) | Rejected — one cited marker beats an untestable ARM risk |

## Consequences

- **Positive**:
  - clang-tidy: `integer_adm.c` 6 visible + 375 NOLINT-hidden → 0;
    `adm_tools.c` 65 → 0. cppcheck (`--enable=all`, file-filtered):
    31 → 0 and 35 → 0 findings on the two files (the remaining lines are
    file-filter `unusedFunction` artefacts of headers / cross-TU callers).
  - NOLINT markers: 24 → 5 lines in `integer_adm.c` (four suppressions),
    1 → 1 in `adm_tools.c`; every survivor cites its invariant.
  - The nine border variants of the contrast-masking threshold are one
    function; a future upstream change to the neighbourhood lands in one
    place instead of eighteen macro bodies.
- **Negative**:
  - `integer_adm.c` and `adm_tools.c` no longer line up textually with
    Netflix/vmaf; an upstream hunk to either file must be re-ported by hand
    (the rebase note maps every moved function).
  - The SIMD twins `x86/adm_avx2.c` / `x86/adm_avx512.c` still carry their
    own copies of the `ADM_CM_THRESH_S_*` macros; they were deliberately
    not touched here.
- **Neutral / follow-ups**:
  - The `ADM_CM_THRESH_S_*` float macros in `adm_tools.h` are now unused
    by any TU; removing them is a header change left to a follow-up.
  - `adm_dwt2_d` is kept although no C caller exists — the Cython
    extension `compat/python-vmaf/core/adm_dwt2_cy.pyx` binds it.

## References

- [ADR-0141](0141-touched-file-cleanup-rule.md) — touched-file lint-clean rule,
  refactor-first, NOLINT only for load-bearing invariants.
- [ADR-0278](0278-t7-5-nolint-sweep.md) — the cite-only closeout this ADR
  supersedes for `integer_adm.c`.
- [ADR-0155](0155-adm-i4-rounding-deferred-netflix-955.md) — the preserved
  Netflix#955 rounding term.
- [ADR-1057](1057-revert-float-adm-simd-dispatch-neon-fma.md) — the
  `adm_dwt2_s` fp-contract bracket.
- [ADR-1138](1138-c-translation-units-keep-null.md) — C TUs keep `NULL`.
- [ADR-0024](0024-netflix-golden-preserved.md) — golden gate.
- Research digest:
  [1141-integer-adm-upstream-mirror-rework.md](../research/1141-integer-adm-upstream-mirror-rework.md).
- Maintainer direction of 2026-08-31 (verbatim): "we still have upstream code that isnt reworked to our standards -> do it, nothing is save anymore as long as the goldens pass".
- Source: `req` (unit brief, paraphrased: rework `integer_adm.c` and
  `adm_tools.c` to the fork's standards bit-exact — extract helpers, split
  oversized functions, check returns, cite or remove NOLINTs — without
  changing arithmetic order, types, or rounding; the SIMD twins and the
  goldens must not move).
