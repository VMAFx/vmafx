<!-- markdownlint-disable MD013 MD060 -->
# Research-0054: `iqa_convolve` block-of-N tap widen — bit-exactness post-mortem

- **Status**: Active (failed-attempt write-up)
- **Workstream**: [ADR-0138](../adr/0138-iqa-convolve-avx2-bitexact-double.md)
- **Last updated**: 2026-05-03

## Question

Can the per-tap `vcvtps2pd` widen in the SIMD `iqa_convolve` paths
(`convolve_avx2.c`, `convolve_avx512.c`, `convolve_neon.c`) be amortised
over a block of N taps — i.e. compute N float products, sum them in
float, widen once, and add to the per-pixel `double` accumulator — to
cut the widen count by 3.7× on the 11-tap Gaussian without breaking
the Netflix CPU golden gate?

The hypothesis sits on top of the most recent post-merge CPU profile,
where `iqa_convolve_avx512` is the largest hot-spot at ~39.5 %
self-time, and the per-tap `vcvtps2pd` + `vaddpd` chain is the
dominant cost. The expected end-to-end gain on `float_ssim` /
`float_ms_ssim` was +6–10 %.

## Sources

- [ADR-0138](../adr/0138-iqa-convolve-avx2-bitexact-double.md) —
  bit-exact `__m{128,256,512}` float-mul → widen → `__m{256,512}d`
  add contract, no FMA, mirrors scalar
  `sum += img[i] * k[j]` under `FLT_EVAL_METHOD == 0`.
- [ADR-0140](../adr/0140-simd-dx-framework.md) — bit-exactness lockstep
  between AVX2 / AVX-512 / NEON paths.
- Scalar reference: [`core/src/feature/iqa/convolve.c`](../../core/src/feature/iqa/convolve.c)
  inner loop `sum += img[img_offset + u] * k->kernel_h[k_offset];`
  with `double sum`.
- Kernel coefficients:
  [`core/src/feature/iqa/ssim_tools.h:77-83`](../../core/src/feature/iqa/ssim_tools.h#L77-L83) —
  11-tap Gaussian, `float` storage, all six distinct values.
- Netflix golden gate: [CLAUDE.md §8](../../CLAUDE.md), `places=4`
  for MS-SSIM averages, `places=10`–`places=17` for individual SSIM
  scores.

## Findings

**Block-of-N widen is mathematically incompatible with the scalar
reference's rounding pattern.** The scalar reduces 11 single-rounded
`float` products into a `double` accumulator with **11 separate widen

- add steps**:

```c
double sum = 0.0;
for (j = 0; j < 11; ++j) {
    sum += (double)(img[j] * kh[j]);   // widen each product, add
}
```

Any block reorder that widens fewer than 11 times per pixel must
either:

1. **Sum products in float first, then widen the sum** — collapses
   N intermediate doubles into one, changes rounding.
2. **Widen each product but tree-reduce in double before the
   serial accumulator add** — keeps 11 widens, doesn't reduce the
   widen count, doesn't deliver the targeted speedup.

A 10 M random-input Monte Carlo on the actual 11-tap Gaussian
coefficients (8-bit pixel range) measured the float-store divergence
rate of three reorder variants vs. scalar:

| Variant                             | Widens / pixel | Float-store mismatch rate |
| ----------------------------------- | -------------- | ------------------------- |
| Scalar reference                    | 11             | 0 %     (baseline)        |
| **Block-of-4 float-then-widen** (4+4+3) | 3        | **27.67 %**               |
| Block-of-2 float-then-widen (5×2 + 1)   | 6        | 16.68 %                   |
| Tree-reduce in double, serial accumulate | 11      | **0 %**                   |

At 1080p, the horizontal pass produces ≈ 2 073 600 interior pixels
per plane and runs on 5 pyramid scales × 5 convolves per `_iqa_ssim`
call × 2 SSIM/MS-SSIM extractors. A 27.67 % per-pixel float-store
mismatch rate with `places=4` MS-SSIM tolerance is **certain to bust
the Netflix golden gate**: even if individual ULP perturbations were
zero-mean, the per-frame averages absorb 600 k mismatches and visibly
shift in the fourth decimal.

**Conclusion:** the per-tap widen is load-bearing for bit-exactness.
Cutting widen count is only available by either (a) widening
upstream, e.g. carrying `__m512d` from the load (which costs the
same microarchitectural cycles as today's pattern), or (b)
rewriting the scalar reference to match a fused pattern — explicitly
out of scope under [ADR-0125 §Ground rules](../adr/0125-ms-ssim-decimate-simd.md)
(the `iqa/` subtree is a verbatim BSD-2011 import).

## Alternatives explored

### Block-of-4 float-mul-add then widen (the original proposal)

```c
float b0 = ((img[0]*kh[0] + img[1]*kh[1]) + (img[2]*kh[2] + img[3]*kh[3]));
sum += (double)b0;
// ... b1 (taps 4..7), b2 (taps 8..10) ...
```

Reduces widens 11 → 3. Mismatch rate on 10 M random inputs: 27.67 %
of pixels produce a different `(float)sum`. **Rejected — busts the
golden gate.**

### Block-of-2 float-mul-add then widen

```c
sum += (double)(img[0]*kh[0] + img[1]*kh[1]);
// ... 5 pairs + 1 tail ...
```

Reduces widens 11 → 6. Mismatch rate: 16.68 %. **Rejected — also
busts the golden gate, smaller win.**

### Widen each product, tree-reduce in double, serial accumulate

```c
double b0 = ((double)(img[0]*kh[0]) + (double)(img[1]*kh[1])) +
            ((double)(img[2]*kh[2]) + (double)(img[3]*kh[3]));
sum += b0;
```

Mismatch rate: **0 %** on 10 M inputs. But this **does not reduce the
widen count** (still 11 `cvtps2pd` per pixel), so it doesn't deliver
the targeted +6–10 % perf gain. The tree-reduction in double could
shave a serial-`addpd` dependency chain (latency 4 cycles on
Skylake-X), but the inner-loop critical path is currently
`prod -> add -> next add`, latency-bound at ~4 cycles per tap, and
existing measurements (PR #333 profile) show the bottleneck is the
**widen** (`vcvtps2pd` 7-cycle latency on SKX), not the add.

### Modify the scalar reference to use FMA / block reduction

Would let the SIMD paths follow. **Rejected** because
[`core/src/feature/iqa/`](../../core/src/feature/iqa/) is a
verbatim BSD-2011 Tom Distler import that the fork explicitly leaves
untouched for rebase hygiene
([ADR-0125 §Ground rules](../adr/0125-ms-ssim-decimate-simd.md)).
Changing the scalar would also require regenerating every
`assertAlmostEqual(places=N)` Netflix golden assertion — explicitly
forbidden by [CLAUDE.md §8](../../CLAUDE.md) global rule 1.

## Reproducer

The Monte-Carlo divergence test is small enough to inline. Save as
`/tmp/test_reorder.c` and build with the fork's default `-O2 -msse2`
(no `-mfma`):

```c
#include <stdio.h>
#include <stdlib.h>

static const float kh[11] = {0.001028f, 0.007599f, 0.036001f, 0.109361f,
                              0.213006f, 0.266012f, 0.213006f, 0.109361f,
                              0.036001f, 0.007599f, 0.001028f};

static double scalar_reduce(const float *img) {
    double sum = 0.0;
    for (int j = 0; j < 11; ++j) sum += (double)(img[j] * kh[j]);
    return sum;
}

static double block4_float(const float *img) {
    double sum = 0.0;
    float b0 = ((img[0]*kh[0] + img[1]*kh[1]) + (img[2]*kh[2] + img[3]*kh[3]));
    sum += (double)b0;
    float b1 = ((img[4]*kh[4] + img[5]*kh[5]) + (img[6]*kh[6] + img[7]*kh[7]));
    sum += (double)b1;
    float b2 = (img[8]*kh[8] + img[9]*kh[9]) + img[10]*kh[10];
    sum += (double)b2;
    return sum;
}

int main(void) {
    srand(42);
    long N = 10000000, mismatches = 0;
    for (long i = 0; i < N; ++i) {
        float img[11];
        for (int j = 0; j < 11; ++j)
            img[j] = (float)(rand() / (double)RAND_MAX) * 255.0f;
        if ((float)scalar_reduce(img) != (float)block4_float(img)) mismatches++;
    }
    printf("mismatches=%ld (%.4f%%)\n", mismatches, 100.0*mismatches/N);
    return 0;
}
```

Build / run:

```sh
gcc -O2 -mfpmath=sse -msse2 /tmp/test_reorder.c -o /tmp/test_reorder
/tmp/test_reorder
# mismatches=2766564 (27.6656%)
```

## Open questions

- **Is there a structural win that does not reorder taps?** The two
  candidates left are (1) widen earlier (e.g. carry `__m512d` from a
  pre-converted image cache, paying the widen on the h-pass output
  and reading double in the v-pass), and (2) eliminate one of the two
  passes by precomputing a `float` separable kernel cache. Both
  change the tape layout and need their own ADRs.
- **Could a small ULP budget for the SIMD path (e.g. `places=10` for
  individual SSIM scores instead of `places=17`) be negotiated to
  unlock the win?** This would require either a Netflix-side
  conversation or a fork-only ULP gate
  ([ADR-0140](../adr/0140-simd-dx-framework.md) supports per-path
  tolerance) and a doc-substance update to call out the divergence.
  Out of scope for this attempt; left as a research thread.
- **Does the AVX-512 `vfmadd` pattern (which we explicitly reject)
  match scalar bit-exactly on hardware where the compiler also
  auto-fuses the scalar?** Worth checking under
  `-march=skylake-avx512` builds with `-mfma`; if so, an
  ISA-conditional AVX-512 path could match scalar by elevating BOTH
  to FMA. Would not affect AVX2/NEON twins under default `-O3`.

## Related

- ADR: [ADR-0138](../adr/0138-iqa-convolve-avx2-bitexact-double.md)
- ADR: [ADR-0125](../adr/0125-ms-ssim-decimate-simd.md) — vendored
  `iqa/` subtree ground rule.
- ADR: [ADR-0140](../adr/0140-simd-dx-framework.md) — SIMD-DX
  bit-exactness lockstep.
- PR: this PR (failed-attempt write-up; no code change).
- Profile context: PR #333 post-merge CPU profile, 2026-05-03.
