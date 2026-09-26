<!-- markdownlint-disable MD013 MD060 -->
# Research-2111: Integer-ADM row-rounding observability — 2026-09-25

**Status:** Complete

**Authority inspected:** exact starting commit
`84f8ab38133325d90042329e89f2b24719d252e7`, ADR-1167, the CPU integer-ADM
fold, and every CUDA, HIP, SYCL and Metal contrast-masking reduction shape.

**Scope:** close
`T-ADM-CM-ROUNDING-PLACEMENT-UNOBSERVABLE-2026-09-19` with a private raw
accumulator seam and device-free regression coverage for the scalar CPU
reference, x86 SIMD paths, and all four GPU twins. No score arithmetic, public
ABI, installed header, CLI, model, snapshot, Netflix golden assertion, FFmpeg
patch, benchmark, tuning or retraining change.

## Why score parity cannot guard the invariant

ADR-1167 requires the inner contrast-masking shift to happen once after a
complete row reduction. The rounding operation is non-distributive:

```text
round(sum(partitions)) != sum(round(partition))
```

The emitted ADM score is too late to observe a one-unit error in this raw
integer accumulator. CPU score conversion divides the accumulator by
`2^(52 - shift_cub - shift_inner_accum)` and converts it to `float`; the
previous gfx1036 mutation experiment recorded bit-identical scores for
per-pixel rounding, per-pixel truncation and a post-shift `+1` mutation.

The smallest deterministic witness uses four partition totals of `4`, a
rounding bias of `4`, and a three-bit shift:

| Placement | Raw result |
|---|---:|
| Complete row: `(16 + 4) >> 3` | 2 |
| Round each partition: `4 * ((4 + 4) >> 3)` | 4 |
| Truncate each partition: `4 * (4 >> 3)` | 0 |
| Complete row, then add one | 3 |

All four values become unobservable later in the score pipeline, but they are
distinct at the row-fold boundary.

## Live implementation inventory

| Backend | Complete-row value | Guarded folds |
|---|---|---:|
| Scalar CPU | `inner[k]` in `adm_cm_fold()` | 1 shape |
| AVX2 / AVX-512 | `accum_inner_{h,v,d}` plus vector-tail `res_{h,v,d}` | 4 functions / 72 band sites |
| CUDA | block `row_total`; scale-0 `warp_reduce(accum_row[row])` | 2 |
| HIP | scale-0 shared `row_total`; scales 1-3 shared `s_row[0]` | 2 |
| SYCL | subgroup totals merged into `total_cm` | 1 |
| Metal | threadgroup reductions `total_cm` / `total` | 4 |

Every live implementation already performed the arithmetic in the correct
place. The defect was observability, so changing score tolerances or score
fixtures would have created false confidence rather than a regression guard.

## Implemented seam and contract

`core/src/feature/adm_cm_accumulator.h` owns the private, host/device inline
`adm_cm_round_row_total()` primitive. Scalar CPU, CUDA and HIP call it only
after their complete-row reduction. AVX2 and AVX-512 retain their exact inline
expressions: each of the four DLM/I4 functions has six row paths and three band
folds, for 72 guarded sites. SYCL likewise retains the exact inline expression
under the same source contract; merely adding a shared-header include would
mark its seven inherited HISS-04 functions as touched without changing
behavior. Metal keeps a byte-equivalent MSL-local twin because Metal shader
code cannot include a C/CUDA/HIP header. The helper is not installed or
exported, and the expression it replaces is identical. Its rounding term is
signed: CUDA scales 1-3 deliberately carry ADR-0155's `INT32_MIN` term, and a
raw regression plus source mutation guard prevent an unsigned cast from
reversing that term.

Two fast, device-free tests provide complementary evidence:

- `test_adm_cm_row_rounding.c` executes the raw `int64_t` seam and asserts the
  worked `2 / 4 / 0 / 3` distinctions before float conversion.
- `test_adm_cm_row_rounding_contract.py` binds every call shape and x86 band
  site listed above to its complete-row value and contains mutation controls
  for early per-partition folding, x86 shift placement, missing rounding bias,
  post-shift increment, Metal pre-reduction folding, signed-term drift, and
  helper arithmetic drift.

The CUDA and HIP fatbin/HSACO dependency inventories include the new private
header, so changing the fold cannot leave embedded device objects stale.

## Alternatives considered

| Alternative | Decision | Reason |
|---|---|---|
| Tighten score tolerances | Rejected | The mutations are bit-identical after float conversion. |
| Add a public/debug score output | Rejected | It expands ABI or user-visible output solely for a regression test. |
| Duplicate arithmetic only in a test | Rejected | It would test a model of production code, not the production seam. |
| Private raw helper plus all-backend placement contract | Chosen | It observes exact integers, changes no public surface, and runs without devices. |

No new architectural decision is introduced: this is executable evidence for
ADR-1167's existing row-level rounding requirement, so a new ADR is not
warranted.

## Verification

- TDD red: direct C compile failed because
  `feature/adm_cm_accumulator.h` did not yet exist.
- Raw seam: `test_adm_cm_row_rounding` passes four worked-value cases:
  the `2 / 4 / 0 / 3` placement witness, zero-shift behavior, the `2` versus
  `1` missing-bias witness, and CUDA's signed `INT32_MIN` rounding term.
- All-backend contract: eight tests pass, including seven mutation controls.
- The complete CPU fast suite passes: 168 tests pass and one expected test is
  skipped; no test fails.
- Device-free CUDA fatbin and HIP HSACO builds compile the changed kernels and
  their new header dependency. Metal cannot compile on this Linux host, while
  the SYCL production source is intentionally unchanged; both remain covered
  by the placement/mutation contract.
- Targeted clang-format, Ruff, clang-tidy, cppcheck and Semgrep checks pass;
  `reuse lint` reports all 9406 files licensed.
- Praetor's touched-file audit passes with all 18 touched files clean. State-row,
  documentation-fragment and generated source-ADR citation checks also pass.
