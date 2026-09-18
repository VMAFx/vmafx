<!-- markdownlint-disable MD013 -->
# Research-2062: CAMBI spatial-mask row SIMD — what each ISA actually gains

- **Status**: Active
- **Workstream**: [ADR-1256](../adr/1256-cambi-spatial-mask-simd-dispatch.md)
- **Last updated**: 2026-09-18

## Question

Upstream Netflix/vmaf `86da14d03` adds AVX2 versions of CAMBI's two spatial-mask
row kernels, `compute_dp_row` (one summed-area-table row: a 1-D prefix sum of
the derivative row added to the previous dp row) and `compute_mask_row` (the
7x7 box sum from two dp rows, compared against `mask_index`). The fork keeps
AVX-512 and NEON twins of CAMBI kernels. Which variants are faster than the
scalar code the extractor would otherwise run, under the compilers that build
the fork, and are they bit-exact?

## Sources

- Upstream commit `86da14d03` ("feature/cambi: AVX2 vectorize spatial-mask dp
  row and mask row"), including its checkasm test and bench numbers
  (dp row 2.26x, mask row 1.34x, upstream's harness).
- Scalar reference: `compute_dp_row` / `compute_mask_row` in
  `core/src/feature/cambi.c`.
- Compiler output of the release build (`-O3`, scalar TU built without
  `-mavx2`), inspected with `objdump` / `llvm-objdump`.

## Findings

**Method.** A throwaway harness links the release build's own object files
(`feature_cambi.c.o`, `feature_x86_cambi_avx2.c.o`,
`feature_x86_cambi_avx512.c.o`) and replays `get_spatial_mask_for_index`'s
access pattern: a cyclic dp buffer of `2 * pad + 2` rows, pad 3 (the 7x7
production filter), binary derivative rows. It reports the best of 500 full
frame passes per kernel, variants interleaved, pinned to one core of an AMD
Ryzen 9 9950X3D (Zen 5). The same harness was linked against objects built by
GCC, Clang and Intel icx 2026.0 (the compiler the published container uses).

**1080p frame pass, µs (speed-up vs scalar of the same compiler):**

| Kernel | Compiler | Scalar | AVX2 (fork) | AVX-512 | AVX2 (upstream verbatim) |
| --- | --- | --- | --- | --- | --- |
| dp row | GCC | 486 | 216 (2.25x) | 153 (3.18x) | 394 (1.23x) |
| dp row | Clang | 428 | 236 (1.81x) | 165 (2.59x) | 577 (0.74x) |
| dp row | icx | 404 | 224 (1.81x) | 158 (2.55x) | 633 (0.64x) |
| mask row | GCC | 275 | 164 (1.68x) | 122 (2.26x) | 163 (1.69x) |
| mask row | Clang | 251 | 167 (1.51x) | 121 (2.08x) | 164 (1.53x) |
| mask row | icx | 246 | 166 (1.48x) | 120 (2.05x) | 164 (1.50x) |

576x324 and 3840x2160 give the same ordering (for example GCC dp row 2.33x /
3.32x at 576x324, 2.27x / 3.16x at 2160p).

**Upstream's AVX2 dp row is slower than scalar under Clang and icx.** Its
loop-carried state is the broadcast of the previous block's last lane, taken
from the scan *after* the carry was added, so every block waits on a
`vpermd` (3-cycle latency) plus an add. Clang additionally lowers the
`permute2x128` + `shuffle` + `blend` that moves the low half's total into the
high half as a `vinserti128` that reads the carry register as a don't-care
source, putting the whole prefix computation on the carried chain. The fork's
version computes the block total from the carry-free prefix and keeps only
`carry += broadcast(total)` on the chain (one add per block), and moves the low
half's total with `pshufd` + `vperm2i128` (zeroing form). AVX-512 and NEON use
the same structure.

**The scalar mask row is already vectorized.** GCC and Clang turn the scalar
loop into 4-lane SSE2 on x86-64 (the TU has no `-mavx2`), which is why AVX2 is
"only" 1.5–1.7x. On aarch64 both compilers turn it into eight columns per
iteration of `ldp`/`add`/`sub`/`cmhi`/`uzp1`/`and` — 19–23 instructions per
eight columns, the same sequence `compute_mask_row_neon` spells out (22). The
NEON mask row therefore removes no work and is not dispatched.

**The NEON dp row does remove work.** The scalar loop cannot be vectorized
(serial prefix) and compiles to seven (Clang) or ten (GCC, which routes the
prefix through a vector register and back) instructions per column, with a
chain of eight dependent adds per eight columns; the NEON loop is 26
instructions per eight columns (Clang) with one carried add. No timing is
available: the NEON path was run under `qemu-aarch64`, which verifies results
but says nothing about speed.

**Bit-exactness.** Both kernels are modular uint32 arithmetic. The dp row is
exact for every input on every ISA. The scalar mask row compares unsigned;
upstream's AVX2 uses the signed `vpcmpgtd`, which is only equal because a real
box sum is at most `(2 * pad + 1)^2` (49 for the 7x7 filter). The fork biases
both operands by 2^31 (one `vpxor`, no measurable cost — see the mask-row
columns above), AVX-512 uses `vpcmpud` and NEON `cmhi`, so all three are exact
for any `mask_index` and any dp contents. `test_cambi_spatial_mask_simd` checks
this with thresholds at 0, 2^31 - 1, 2^31 and 2^32 - 1 and box sums planted on
either side of them; upstream's signed compare fails it.

**Share of CAMBI.** On the 1080p checkerboard fixture, single thread, one CAMBI
frame took about 23 ms with every CAMBI kernel scalar (`--cpumask 65535`) and
about 21 ms with the default dispatch. The two row kernels are about 0.76 ms of
the scalar frame, so their own contribution is roughly 2 %.

## Alternatives explored

- **Port upstream's AVX2 verbatim.** Rejected for the dp row: a regression
  under the compilers that build the published artifacts. The mask row's
  signed compare was kept in spirit but made exact; the bias costs nothing
  measurable.
- **Dispatch the NEON mask row anyway (twin symmetry).** No op-count gain over
  the auto-vectorized scalar, and a dispatched hand kernel is one more thing to
  keep in sync with compiler output. Kept in-tree and parity-tested instead.
- **Masked AVX-512 tails.** The scalar tails cover at most 15 columns per row;
  not worth the extra code.

## Open questions

- Intel AVX-512 cores (frequency licences, 512-bit port layout) were not
  measured; only Zen 5.
- Real aarch64 timing of `compute_dp_row_neon` is unmeasured.
- The older AVX-512 and NEON CAMBI kernels (derivative row, c-values row,
  range updates) have not been dispatched since the upstream c-values layout
  port; whether to re-wire or retire them is a separate decision.

## Related

- [ADR-1256](../adr/1256-cambi-spatial-mask-simd-dispatch.md)
- [CAMBI CPU SIMD paths](../metrics/cambi.md#cpu-simd-paths)
- [ADR-0452](../adr/0452-cambi-calculate-c-values-avx512-neon.md) (earlier CAMBI SIMD twins)
