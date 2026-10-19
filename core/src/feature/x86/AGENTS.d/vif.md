---
paths:
  - core/src/feature/x86/vif_avx2.c
  - core/src/feature/x86/vif_avx512.c
  - core/src/feature/x86/vif_statistic_avx2.c
  - core/src/feature/vif.c
  - core/src/feature/integer_vif.c
invariant: Private forced-inline helpers in vif_avx512.c pass aggregate vector structs by const * rather than by value.
---
# VIF SIMD Stages and Parameter Conventions

| Group | TUs that move in lockstep |
| --- | --- |
| **VIF SIMD8** (ADR-0146) | `vif_statistic_avx2.c` (`vif_stat_simd8_compute` + `vif_stat_simd8_reduce` halves around `struct vif_simd8_lane`) + scalar `../vif.c`. Per-lane scalar-float reduction via 32-byte aligned `tmp_n[8]` / `tmp_d[8]` is load-bearing for ADR-0139. |

| Group | TUs that move in lockstep |
| --- | --- |
| **`vif_subsample_rd_8` noinline helpers** (ADR-0503) | `vif_avx512.c` (`vif_subsample_rd_8_vert_j` + `vif_subsample_rd_8_horiz_j`). These are `static __attribute__((noinline))` helpers carved from `vif_subsample_rd_8_avx512` to eliminate a ~30-ZMM live-set spill cluster. **Do NOT mark them `inline`, `always_inline`, or remove `noinline` — doing so re-merges vertical and horizontal register live-sets back into caller frame and restores spill cluster.** Any change to accumulation order inside these helpers breaks ADR-0138 / 0139 bit-exactness. |

- `float_vif` filters = run-time `vif_get_filter()` since ADR-0416
  (#758); `vif_filter1d_table_s` no longer exists. AVX2 convolution
  (`../common/convolution_avx.c`) = scalar order, no contraction: each
  tap one fp32 product + one fp32 add. `float_vif_cuda` mirrors exactly
  that (ADR-1412); keep it so.

- [ADR-0146](../../../../../docs/adr/0146-nolint-sweep-function-size.md) —
  IQA / VIF SIMD helper decomposition.

## Integer VIF AVX2 private stages (Research-2045)

`vif_avx2.c` keeps original packed 64-bit mean additions over 32-bit
products, distinct 8/16-bit moment packing and shifts, tap order, scalar tails
and final copy/padding order. Never merge those distinct lane layouts during
rebases. Keep GCC-only no-unroll constraint on 8-bit second-moment tap loop;
removing it reproduced full unrolling/spills and repeatable slowdown. This
constraint does not change arithmetic or apply to other compilers.
Four exported statistic/subsample signatures remain in
`vif_avx2.h`; statistic callbacks must continue to match `VifState` in
`integer_vif.c`. Two precise Cppcheck const-parameter annotations preserve
that shared function-pointer contract. Re-run native scalar/AVX2 stage
test and same-ISA old/new numerical comparisons after changing these stages.
See [Research-2045](../../../../../docs/research/2045-integer-vif-avx2-stages-2026-09-08.md).

## Integer VIF AVX-512 stages (Research-2046)

Keep private forced-inline statistic/subsample stages' per-accumulator tap
order, lane permutations, rounding constants and scalar tails. Preserve
fused two-channel horizontal mean and three-channel energy tap loops and their
GCC unroll bounds; separate channel loops caused measured 8-bit regression.
Two
ADR-0503 8-bit subsample block helpers remain noinline/noclone; their call
boundaries control register pressure. Statistic callbacks keep mutable
`VifPublicState *` type required by `VifState` dispatch, despite only reading
that struct and writing through its buffer pointers. Run
`test_integer_vif_avx512_stages` after rebasing these kernels; preserve all five
vertical planes and bit-exact numerator/denominator checks. 8-bit vertical
vector extent rounded down to 16 samples but loads/stores 32 at time;
production scratch padding owns those extra lanes, while scalar tails overwrite
valid residual pixels. See [Research-2046](../../../../../docs/research/2046-integer-vif-avx512-stage-lint.md).

## Integer VIF AVX-512 stage helper parameter convention (Research-2098)

Private forced-inline helpers in `vif_avx512.c`
(`vif_horizontal_energy_pack512`, `vif_vertical_mean8`, `vif_vertical_energy8`,
`vif_vertical_store8`, `vif_vertical_store_mean8`, `vif_vertical_energy16`,
`vif_vertical_store_mean16`, `vif_vertical_store_energy16`) pass aggregate
vector structs (`VifPair512`, `VifTaps8`, `VifEnergy512`) by `const *` rather
than by value. Under System V AMD64 and Windows x64 ABIs, aggregates > 64 bytes
cannot be passed in vector registers; passing them by value forces caller stack
allocation and copies when out of line, triggering CodeQL `cpp/large-parameter`
alerts 1108–1112. Under GCC 16 x86-64 System V ABI `-O3`, inlining folds
pointer dereferences without changing hot `.text` section (verified by
byte-for-byte comparison against independently built `origin/master`
object). host structural stack scanner is clean, while definitive Win64
acceptance remains hosted MinGW build because no cross compiler is
installed locally. Do not revert these internal parameters to pass-by-value on
rebase. Public API signatures in `vif_avx512.h` remain unchanged. See
[Research-2098](../../../../../docs/research/2098-vif-avx512-large-parameter-codeql.md).
