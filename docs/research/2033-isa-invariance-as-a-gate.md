<!-- markdownlint-disable MD013 MD060 -->

# Research-2033: Testing the property instead of the proxy (2026-09-06)

## The shape of the problem

The fork asserts that every SIMD kernel is bit-exact with its scalar reference.
Ten `test_<feature>_simd.c` files assert exactly that, and all of them pass.
Two defects still shipped in which a score depended on the host instruction
set (ADR-1205, ADR-1208).

The reason is worth stating plainly, because it generalises well beyond this
repo: **those tests do not compare the two things that ship.** The shipped
scalar functions are `static`, so each test defines its own scalar reference
and compares the SIMD kernel against that. The test therefore asserts

> kernel == test's idea of the reference

when the property that matters is

> shipped SIMD path == shipped scalar path

Those coincide only while someone keeps the test's private reference in sync
with the shipped one by hand. Twice, someone did not — and in both cases the
test kept passing, because both halves of *its* comparison had been updated.

## What replaced it

`core/test/test_feature_isa_invariance.c` runs each feature end-to-end through
the public API twice over one fixture: once with the host's real ISA, and once
with `VmafConfiguration.cpumask` set to disable every SIMD flag — the same
switch the CLI exposes as `--cpumask`. It then `memcmp`s the two `double`
scores.

It touches no internal symbol, so there is nothing to keep in sync. It states
the property directly rather than a proxy for it.

It found ADR-1208 on its first run. Nine of the ten features already passed,
which is itself useful: the contract was real, and only one implementation was
violating it.

## The defect it found

`edge_diff_map` computes `ed = |img - blur(img)|` per pixel. Scalar:

```c
double ed1 = fabs((double)r1[i] - (double)rm1[i]);   /* exact */
```

All four SIMD kernels:

```c
const __m512 d1 = |_mm512_sub_ps(a1, am1)|;          /* rounds to float */
double ed1 = (double)d1f[k];
```

Both operands are `float`, so the double subtraction is exact and the float one
is not. Each kernel's own scalar tail used the correct form, so one call could
mix conventions depending on where a pixel fell relative to the vector width.

`test_edge` compares kernel against private reference at 33x21 and passes —
on that fixture's pseudo-random inputs the float subtraction happens to be
exact. On real XYB data it is not.

## Why 1 ULP mattered

`ssimulacra2` is ill-conditioned twice over: the edge term is a catastrophic
cancellation between two nearly equal quantities, and pooling takes a 4-norm,
which weights the few largest survivors. The end-to-end effect of the rounding
was 1.6e-09 — small, but a contract violation, and the same mechanism turned a
1 ULP seed into 2.6e-03 in ADR-1205. For this metric, "tiny input difference,
tiny output difference" is an assumption to verify, not a given.

## How it was localised

Dump every scale-0 intermediate under both dispatch modes and find the first
that differs. Here **none** of `lin`, `xyb`, `dxyb`, `mu1`, `mu2`, `s11`,
`s22`, `s12` differed — all bit-identical — while the accumulators did. That
narrowed it to a function consuming identical inputs and producing different
output. Diffing the per-slot accumulators then showed only the `e*` (edge)
slots moving and never the `s*` (ssim) ones, which named the function outright.

## Transferable rules

- Test the property, not a proxy for it. If a test needs a private copy of the
  thing under test, ask what happens when the copies diverge.
- A passing bit-exactness test says nothing if both sides of the comparison
  were updated together.
- Promote before you subtract. `(double)a - (double)b` is exact for floats;
  `(double)(a - b)` is not.
- A vector operation inside an otherwise-scalar loop buys nothing and can cost
  correctness.
- When inputs are bit-identical and outputs are not, the defect is in the
  function, not the pipeline — stop bisecting upstream.

## References

- [ADR-1207](../adr/1207-feature-isa-invariance-gate.md),
  [ADR-1208](../adr/1208-ssimulacra2-edge-diff-double-subtract.md).
- [Research-2032](2032-gpu-parity-resolution-blind-spot.md) — the companion
  cross-backend blind spot, found the same week by the same method.
