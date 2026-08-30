<!-- markdownlint-disable MD013 -->
# Research-0011: `_iqa_convolve` AVX2 — bit-exactness via `__m256d`, kernel invariants, Amdahl

- **Status**: Active
- **Workstream**: [ADR-0138](../adr/0138-iqa-convolve-avx2-bitexact-double.md)
- **Last updated**: 2026-04-21

## Question

After MS-SSIM decimate went SIMD ([ADR-0125](../adr/0125-ms-ssim-decimate-simd.md)),
a 2026-04-20 1080p CPU profile of MS-SSIM shows `_iqa_convolve`
owning **51.4 % self time** (+ `_iqa_filter_pixel` 36.0 %; decimate
dropped to 0.6 %). Can we accelerate `_iqa_convolve` on x86 with
AVX2 while remaining byte-identical to the scalar reference, without
modifying the vendored BSD-2011 Tom Distler `iqa/` subtree? What
accumulator width and reduction order does bit-exactness require?

## Sources

- Profile data:
  - `build/profiles/2026-04-20/ms_ssim_1080p_cpu.callgrind`
  - `build/profiles/2026-04-20/ms_ssim_1080p_cpu_topN.txt`
  - `build/profiles/2026-04-20/ms_ssim_1080p_cpu.svg` (flamegraph)
- [`core/src/feature/iqa/convolve.c`](../../core/src/feature/iqa/convolve.c)
  — scalar reference. 1-D separable branch is active by default (see
  `IQA_CONVOLVE_1D` in
  [`iqa/iqa_options.h:25`](../../core/src/feature/iqa/iqa_options.h#L25)).
- [`core/src/feature/iqa/ssim_tools.c`](../../core/src/feature/iqa/ssim_tools.c)
  — the only hot caller of `_iqa_convolve`: five calls per
  `_iqa_ssim` (lines 170, 171, 187, 188, 189).
- [`core/src/feature/iqa/ssim_tools.h`](../../core/src/feature/iqa/ssim_tools.h)
  — defines `GAUSSIAN_LEN = 11`, `SQUARE_LEN = 8`, and the
  static `const float g_gaussian_window_{h,v}` /
  `g_square_window_{h,v}` 1-D coefficient arrays used by MS-SSIM.
- [`core/src/feature/ms_ssim.c`](../../core/src/feature/ms_ssim.c)
  — drives `_iqa_ssim` for 5 pyramid scales, so 25
  `_iqa_convolve` calls per MS-SSIM frame.
- [`core/src/feature/x86/ms_ssim_decimate_avx2.c`](../../core/src/feature/x86/ms_ssim_decimate_avx2.c)
  — established dispatch + border-scalar precedent.
- [`core/src/feature/x86/ssim_avx2.c`](../../core/src/feature/x86/ssim_avx2.c)
  / [`ssim_avx512.c`](../../core/src/feature/x86/ssim_avx512.c)
  — existing SSIM inner-primitive SIMD paths wired through
  `_iqa_ssim_set_dispatch`; the same pattern applies here.
- IEEE 754-2008 §4.3 — associativity of addition (not guaranteed).

## Findings

### The 36 % `_iqa_filter_pixel` self-time is *not* on the convolve path

The vmaf_bench profile runs both SSIM and MS-SSIM features. SSIM's
scalar `_iqa_decimate` (in
[`ssim.c:158`](../../core/src/feature/ssim.c#L158)) calls
`_iqa_filter_pixel` per output pixel. MS-SSIM uses the
already-SIMD `ms_ssim_decimate` path and does **not** call
`_iqa_filter_pixel`. The 36 % cost therefore belongs to SSIM's
decimate — a separate T3 workstream. This ADR targets only the
51.4 % `_iqa_convolve` gap.

### Bit-exactness contract: `__m256d` with separate mul + add

The scalar loop at
[`convolve.c:159-161`](../../core/src/feature/iqa/convolve.c#L159-L161)
is:

```c
double sum = 0.0;
for (u = -uc; u <= uc; ++u, ++k_offset) {
    sum += img[img_offset + u] * k->kernel_h[k_offset];
}
img_cache[img_offset] = (float)(sum * scale);  // scale == 1.0f for normalised kernels
```

Three observations drive the bit-exactness argument:

1. **The scalar multiply-add is *unfused*.** The expression
   `sum += a * b` is written as a `*` followed by a `+=`. Under the
   fork's default build flags (`-O3`, no explicit `-march=haswell`
   / `-mfma` in the default meson configuration), GCC does not
   contract to FMA. Each tap therefore rounds twice: once after the
   multiply, once after the add. The SIMD path must reproduce the
   same two-rounding pattern to stay bit-identical.
2. **Associativity is only needed within a single output lane.**
   Across-lane reductions (e.g. `_mm_hadd_pd`) would change the
   summation order. We avoid them: each of the 4 output pixels in a
   `__m256d` accumulator only ever sums its own 11 taps in the same
   order as the scalar inner loop.
3. **Promotion order matches.** The scalar does `float * float →
   double` at the multiply site (operator promotion) and `double +
   double` at the add site. `_mm256_mul_pd(_mm256_cvtps_pd(img4),
   _mm256_set1_pd((double)kh[u]))` does the same: widen the 4 float
   inputs to doubles before multiply, then add doubles.

With (1), (2), (3) in place, **lane `i` of the `__m256d` accumulator
computes exactly the same sequence of IEEE-754 operations as the
scalar inner loop for output column `x + i`.** The store path
(`_mm256_cvtpd_ps` followed by `_mm_storeu_ps`) produces the same
`(float)(sum * 1.0f)` cast as the scalar `img_cache[...] = (float)(sum

- scale)`.

**Bit-identical output by construction.** No tolerance check
required.

### Why not `__m256` (8-lane float)?

Single-precision FMA — `_mm256_fmadd_ps(img, k, sum)` — is one
rounding per tap, not two, and uses float accumulation instead of
double. This produces a different (and in a strict numerical sense
*better*) answer from the scalar. To ship an 8-lane-float AVX2 path
we would need to either:

- Modify the scalar in `iqa/convolve.c` to use `fmaf()` and
  single-precision accumulation (the `ms_ssim_decimate` approach
  from [ADR-0125](../adr/0125-ms-ssim-decimate-simd.md)). This
  mutates vendored BSD-2011 code and creates a one-time golden
  shift.
- Accept the deviation and hope Netflix's `assertAlmostEqual(places=N)`
  tolerance absorbs it. The SSIM golden at
  `places=3` (~5e-4) and MS-SSIM's informal `places=4` (~5e-5)
  might or might not cover it; a 11-tap single-vs-double accumulator
  can differ by ~2e-7 per tap, which cascades unpredictably through
  5 convolves × 5 scales of MS-SSIM.

Keeping the vendored file untouched AND landing an AVX2 fast path at
the same time requires the double-accumulator route. **This is the
decision in ADR-0138.**

### Kernel invariant set

The AVX2 specialisation assumes MS-SSIM / SSIM invariants and
falls back to scalar otherwise. Enforced at dispatch:

- `k->w == k->h` (square kernel)
- `k->w ∈ { GAUSSIAN_LEN (11), SQUARE_LEN (8) }`
- `k->normalized == 1` → `_calc_scale(k) == 1.0f` (no post-scale)
- `k->kernel_h != NULL && k->kernel_v != NULL` (separable form)
- `IQA_CONVOLVE_1D` is defined at compile time (it is in
  `iqa_options.h:25`)
- `w`, `h` ≥ 4 (for safe `__m128`-load scaffolding)
- `img`, `img_cache`, `result` are 32-byte-aligned (guaranteed by
  `malloc` on 64-bit Linux; `_mm_loadu_ps` is used anyway so
  misalignment is safe if the invariant slips)

Any violation short-circuits to the scalar `_iqa_convolve`.

### Border handling — scalar fallback, matches the decimate precedent

The separable scalar path at
[`convolve.c:152-179`](../../core/src/feature/iqa/convolve.c#L152-L179)
only touches **interior pixels** — output columns
`0..dst_w-1 = w - k->w + 1` on rows `-vc..dst_h+vc-1`. The border
pixels (the first and last `uc` / `vc` columns / rows) are skipped;
`_iqa_convolve` relies on the caller having already established that
the kernel fits entirely inside the image. No `KBND_SYMMETRIC`
reflection is involved on the `_iqa_convolve` hot path.

This is different from the `ms_ssim_decimate` case, which *did*
need mirror reflection. Here the border handling is trivial: we
simply skip the output tail columns that aren't a multiple of 4
output lanes wide, and call the scalar inner loop on the last few.

Matches the
[`core/src/feature/x86/adm_avx2.c`](../../core/src/feature/x86/adm_avx2.c)
precedent (5-tap separable CSF filter) and
[`ms_ssim_decimate_avx2.c`](../../core/src/feature/x86/ms_ssim_decimate_avx2.c)
(scalar-border, SIMD-inner).

### FLOP accounting + expected speed-up

Per `_iqa_convolve` call on an `w × h` input with an `N`-tap
separable kernel:

- Horizontal pass: `(h + N-1) × (w - N + 1) × N` MACs. For 1080p +
  N=11: `1090 × 1910 × 11 ≈ 22.9 M` MACs.
- Vertical pass: `(h - N + 1) × (w - N + 1) × N` MACs. For 1080p +
  N=11: `1070 × 1910 × 11 ≈ 22.5 M` MACs.
- **Total per call**: ≈ 45.4 M MACs at 1080p for N=11 Gaussian.
- **Per MS-SSIM frame**: 45.4 M × 5 convolves × Σ(scale-downsampled
  area) ≈ 80 M MACs (dominated by scale 0; scales 1–4 add only ~½
  again).
- Scalar double-accumulator throughput on Zen / Skylake: ≈ 1 MAC /
  cycle in the critical path (one FMUL port + one FADD port, 4-cycle
  latency on the chain, unfused). At 4 GHz → 80 M MACs × 1 cycle / 4
  GHz ≈ 20 ms / frame just on double-path MACs. Matches the 51 %
  of 43.6 fps = 11 ms roughly (discrepancy from the scalar actually
  being *unfused*: 2 rounding ops not 1, plus cache pressure).
- AVX2 `__m256d` throughput: 4 lanes × 1 double-MAC / cycle ≈ 4
  MACs / cycle — ideal 4× speed-up. At memory-bandwidth limit
  (~25 GB/s L3 on Zen, ~40 GB/s on Skylake server) the
  float-to-double expansion roughly halves the speed-up ceiling:
  real-world ~3×.
- **Amdahl on end-to-end MS-SSIM**: closing 51 % of scalar time at
  4× ideal yields `1 / (0.486 + 0.514/4) = 1.63×` total speedup.
  At the 3× realistic ceiling: `1 / (0.486 + 0.514/3) = 1.54×`.
  End-to-end MS-SSIM should land at ~67 fps (vs. current 43.6 fps)
  at 1080p.

### Prior art / existing SIMD convolution patterns in this tree

- [`core/src/feature/x86/adm_avx2.c`](../../core/src/feature/x86/adm_avx2.c)
  — separable 5-tap CSF filter. Structural template. Uses
  `_mm256_fmadd_ps` (float path) because ADM is not bit-exactness
  constrained — different rounding from the scalar is inside the
  documented golden tolerance.
- [`core/src/feature/x86/ms_ssim_decimate_avx2.c`](../../core/src/feature/x86/ms_ssim_decimate_avx2.c)
  — most recent fork precedent. Scalar-border, SIMD-inner. Uses
  `fmaf()` for the scalar reference and `_mm256_fmadd_ps` for SIMD,
  both in single-precision. That path was written fresh — we had
  the freedom to choose single-precision end-to-end. `_iqa_convolve`
  is vendored, so we do not.
- [`core/src/feature/common/convolution.c`](../../core/src/feature/common/convolution.c)
  — integer-typed (uint16_t); not reusable.
- Public Gaussian-blur SIMD in libjxl
  (`lib/jxl/gauss_blur.cc`) uses 4-lane double recursion for the
  IIR variant. Not the same algorithm as the 11-tap FIR here but
  confirms double-precision lane parallelism is an established
  pattern for bit-exact Gaussian blur.

## Alternatives explored

- **`__m256` (8-lane float) + scalar rewrite** — rejected. Requires
  mutating `iqa/convolve.c`, a vendored BSD-2011 file. The
  one-time golden-score shift would need empirical tolerance
  verification per existing SSIM + MS-SSIM assertion. Fork rule
  (ADR-0125 §Ground rules) is "don't touch vendored iqa/ unless we
  have to". Double-accumulator at 3× realistic gives most of the
  speedup without the risk.
- **`__m256` float, accept deviation** — rejected. Golden gate is
  non-negotiable (CLAUDE §12 rule 1). Not worth the risk for a
  2× marginal speedup over the double path.
- **`__m512d` (AVX-512, 8-lane double) in same PR** — deferred. The
  vmaf_bench workload is already memory-bandwidth-limited; the extra
  4 lanes on AVX-512 provide less than linear improvement and AVX-512
  clock throttling on older Skylake-X could *lose* wall time. A
  standalone AVX-512 follow-up PR that benchmarks against the AVX2
  path on specific hardware (Zen4, Sapphire Rapids) is the right
  granularity.
- **Fuse the 5 sequential convolves inside `_iqa_ssim` into one
  multi-output pass** — rejected for scope. Would require refactoring
  `_iqa_ssim` to produce all 5 convolved outputs in a single cache-hot
  sweep. Big architectural change, big diff, hard to keep bit-exact.
  Potential follow-up if 3× realistic doesn't land.
- **Pre-allocate `img_cache` into a per-extractor scratch slot to
  cut the per-frame `malloc`/`free`** — deferred. The profile
  showed `brk` at 1.1 %, so allocator cost is non-trivial but small.
  Separate S-effort follow-up if the measured AVX2 gain is
  memory-bound.

## Open questions

- Does the hardware FMA port on Zen4 / Sapphire Rapids provide the
  same 4× ceiling as on Zen3 / Skylake for double-MAC? Tests on the
  benchmarking host should confirm.
- If the measured speedup is memory-bandwidth-limited rather than
  ALU-limited, the AVX-512 follow-up has diminishing returns and we
  should instead invest in the `img_cache` pre-allocation follow-up
  to cut the 40–50 MB/frame allocator pressure. Settle this in the
  AVX2 PR's benchmark section.
- SSIM's scalar `_iqa_decimate` at 36 % self-time is a parallel T3
  workstream — single-kernel, but with proper `KBND_SYMMETRIC`
  reflection (unlike `_iqa_convolve`'s interior-only scope). The
  kernel specialisation here could potentially be reused.

## Related

- ADR: [ADR-0138](../adr/0138-iqa-convolve-avx2-bitexact-double.md)
- Predecessor ADR: [ADR-0125](../adr/0125-ms-ssim-decimate-simd.md)
- Predecessor digest:
  [Research-0008](0008-ms-ssim-decimate-simd.md)
- PR: VMAFx/vmafx#TBD
- Profile artifacts:
  `build/profiles/2026-04-20/ms_ssim_1080p_cpu.callgrind` (and
  `.svg`, `_topN.txt`).
