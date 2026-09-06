<!-- markdownlint-disable MD013 -->

# Research-2030: an fp64-free, bit-identical integer-ADM `angle_flag`

- **Status**: Active
- **Workstream**: [ADR-1194](../adr/1194-adm-angle-flag-single-source.md)
- **Last updated**: 2026-09-06

## Question

The integer-ADM `angle_flag` test is frozen by the Netflix golden-data gate in
a form that narrows exact int64 operands to `float` and then compares in
`double`. Two of the fork's GPU backends cannot execute that form:
`integer_adm_sycl.cpp` must stay free of any binary64 instruction (Intel Arc
A-series and most iGPUs expose no fp64, and one fp64 instruction anywhere in a
SYCL translation unit makes the runtime reject the whole SPIR-V module), and
Metal Shading Language has no `double` type at all.

**Can the frozen predicate be evaluated bit-identically with no
floating-point arithmetic of any width, inside 64-bit integers?**

## Sources

- `core/src/feature/integer_adm.c` — `adm_angle_flag()`, the golden expression.
- [Netflix/vmaf#930](https://github.com/Netflix/vmaf/issues/930) and the
  T-UPSTREAM-930 row in [`docs/state.md`](../state.md).
- IEEE 754-2019 §4.3.1 (roundTiesToEven) — the default rounding attribute the
  `int64 -> float` conversion and the `double` multiply both use.
- `core/src/feature/sycl/integer_adm_sycl.cpp` — the `GainLimitQ31` comment
  block that documents the SPIR-V fp64 module-rejection constraint.
- Metal Shading Language Specification §2.1 — scalar data types; `double` is
  not among them.

## Findings

### 1. Four spellings of one predicate

Measured on 3 000 000 near-parallel int16 band quadruples (reference vector
uniform over the int16 square, distorted vector jittered by ±600 per
component, seed 1), counting disagreements against the golden CPU expression:

| Form | Where it lived | Disagreements / 3 M |
| --- | --- | --- |
| exact int64 products compared in `double` | CUDA / HIP `decouple_angle_flag_s0` | 117 |
| whole comparison in `float` | SYCL, both scales | 123 |
| exact int64 products narrowed to `float` | Metal `iadm_angle_flag_s0` | 158 |

All three constants (`0.99969541f`, `0.99969541789740297f`, and the CPU's
run-time `cos(M_PI/180)^2` narrowed to float) are the **same** binary32 value,
`0x3F7FEC0A` — the divergence is entirely in the evaluation, not the constant.
Concrete scale-0 quadruples where CPU and the CUDA `s0` form disagree:

```text
oh=-11037 ov=-15188 th=-12400 tv=-16452   CPU=1  CUDA=0
oh=-14118 ov=-1840  th=-12911 tv=-1454    CPU=1  CUDA=0
oh=13202  ov=-691   th=13542  tv=-472     CPU=0  CUDA=1
```

These are the corpus in `core/test/test_adm_angle_flag.c`.

### 2. The golden expression is an integer comparison in disguise

Write the three operands after their `float` narrowing as significand ×
power of two — every binary32 value is exactly that, with a 24-bit
significand:

```text
of = (float)ot_dp     = mp * 2^ep      mp in [2^23, 2^24)
om = (float)o_mag_sq  = mo * 2^eo
tm = (float)t_mag_sq  = mt * 2^et
c  = (float)cos(1deg)^2 = MC * 2^-24   MC = 16772106 exactly
```

The golden expression is
`(of/4096)^2 >= (c * (om/4096)) * (tm/4096)` evaluated in binary64. Both
sub-products on the right are formed left to right, so:

- `(of/4096)^2 = mp^2 * 2^(2*ep-24)` — a 48-bit significand, **exact** in
  binary64;
- `c * (om/4096) = MC*mo * 2^(eo-36)` — 48 bits, **exact**;
- multiplying that by `tm/4096` gives a 72-bit exact product, so the *only*
  inexact step in the whole expression is one round-to-nearest-even to 53 bits.

Because rounding commutes with scaling by a power of two, the comparison is
equivalent to the integer relation

```text
mp^2 * 2^sp  >=  round53(V),   sp = 2*ep - eo - et,   V = MC*mo*mt * 2^-24
```

`mp^2` and `V` both live in `[2^45.99, 2^48]`, so `sp >= 3` decides it true and
`sp <= -3` decides it false; only `sp` in `[-2, 2]` needs the exact path.

### 3. Keeping `MC*mo*mt` inside 64 bits

`mo*mt` is 48 bits — fine — but multiplying by the 24-bit `MC` needs 72. The
trick is that `MC` is *close to* `2^24`: with `D = 2^24 - MC = 5110` (13 bits),

```text
V = MC*G*2^-24 = G - D*G*2^-24,        G = mo*mt = q*2^24 + r
  = (G - D*q) - (D*r)*2^-24
  =  S        -  f,        S < 2^48,  0 <= f < D <= 2^13
```

`D*q < 2^37` and `D*r < 2^37`: every term fits. Rounding `V` to 53 significant
bits is then a round-to-nearest-even of `V * 2^p` to an integer, with
`p = 53 - bitlen(V)` in `[5, 8]`; `S << p < 2^56` and `(D*r) << p < 2^45`, both
comfortably inside `uint64`. The binade of `V` is one below that of `S` exactly
when `S - 2^(bitlen(S)-1) < f`, which is checked with the same integers.

The final comparison is `(mp*mp) << (sp+p) >= rounded`, and `sp+p` is in
`[3, 10]`, so the left-hand side is under `2^58`. No 128-bit arithmetic, no
floating point.

That is `adm_angle_flag_i64()` in `core/src/feature/adm_angle_flag.h`.

### 4. Verification

A standalone harness compared `adm_angle_flag_i64()` with the golden
expression compiled as C:

| Corpus | Samples | Mismatches |
| --- | --- | --- |
| Scale-0 band quadruples (int16, jittered) | 200 000 000 | 0 |
| Boundary walk: random `o_mag_sq`/`t_mag_sq` across all 62 magnitude classes, `ot_dp` stepped ±6 around the equality point, plus a 20×20×20 edge-operand cube (0, 2^23±1, 2^24±1, 2^31, 2^52, 2^53-1, 2^62, `INT64_MAX`, negatives, `INT64_MIN`) | 103 813 779 | 0 |
| Band widths 2, 4, 8, 12, 16, 20, 24, 28, 30, 31 bits | 2 000 000 each | 0 |

Repeated under `gcc` and `clang`, at `-O0`, `-O2 -ffp-contract=off` and
`-O3 -march=native` — identical results, which also confirms the golden
expression itself is contraction-immune (it contains no multiply-add).

A reduced version of the same corpus runs in CI as
`core/test/test_adm_angle_flag.c` (suite `fast`).

The CPU path was additionally proven untouched by compiling
`core/src/feature/integer_adm.c` from `origin/master` and from this branch with
identical flags and diffing `objdump -d`: byte-identical machine code.

### 5. How often real content hits it

The synthetic sweep above is adversarial. To find the rate on real pixels, the
scalar CPU `adm_decouple()` was instrumented (temporarily, on a throwaway
build) to evaluate all four historical forms per scale-0 pixel and count the
disagreements. The scalar path had to be forced — this workstation's dispatch
picks `adm_decouple_avx512`, which is why the first attempts reported zero
pixels.

| Clip | scale-0 pixels | operands > 2^24 | CUDA/HIP flips | SYCL flips | Metal flips |
| --- | --- | --- | --- | --- | --- |
| `src01_hrc00/hrc01_576x324` (the Netflix golden pair) | 1 540 608 | 11 397 | 0 | 2 | 3 |
| `sparks_ref/dis_480x270` 10-bit | 111 870 | 496 | 0 | 1 | 0 |
| synthetic full-contrast noise 1920x1080, ±1 LSB distortion | 1 017 036 | 725 516 | 11 | 11 | 18 |

So the divergence is reachable on the **golden Netflix clip itself** for the
SYCL and Metal forms — it is not a synthetic-only defect.

Whether a flipped flag moves the final score depends on
`adm_enhn_gain_limit`. At the shipped default of `1.0` the flag only gates the
`MIN(rst, t)` / `MAX(rst, t)` clamp, which is frequently a no-op, and an
end-to-end CUDA run over the synthetic noise clip produced byte-identical
`adm` scores before and after the fix. With `adm_enhn_gain_limit = 1.2` the
flag also selects whether `rst` is scaled by the gain, and the same clip shows
the change directly (CUDA, `%.17g` output):

```text
integer_adm2_egl_1.2        0.9993743386943251 -> 0.9993743655248627
integer_adm3_egl_1.2        0.9996871693471625 -> 0.9996871827624314
integer_adm_scale0_egl_1.2  0.999429676185588  -> 0.9994297761196714
```

Only scale 0 moves, which is exactly the site the CUDA/HIP change touches.

## Alternatives explored

- **Golden expression in binary64 on every backend.** Five-line diff, and the
  right answer for CUDA and HIP (which is what they now do). Not available for
  SYCL or Metal — see the Question.
- **Compensated binary32 arithmetic.** `sycl::fma`-based 2Product gives the
  exact `of^2` and `c*om` as two-float expansions, and `A*tm` as a four-term
  expansion. Comparing expansions exactly is possible, but reproducing
  binary64's *single* rounding of the 72-bit product still needs the same
  `round53` modelling — with more subtle code than the integer form. Dropped.
- **Exact real comparison, skipping the `round53` modelling.** About ten lines
  shorter and simpler to review: compare `mp^2 * 2^sp` against `V` itself. It
  differs from the golden predicate only when the double rounding moves `V`
  across the left-hand side, a relative window of `2^-53` — which no realistic
  input hits. Rejected on principle: "unreachable in practice" is exactly the
  reasoning that produced the four divergent forms in the first place, and the
  extra code is bounded and testable.
- **128-bit emulation of `MC*mo*mt` via 32-bit limbs.** Works, but needs a
  manual multi-limb multiply plus a multi-limb rounding step. The `D = 2^24 -
  MC` decomposition removes the need entirely.

## Open questions

- The AVX-512 vectorised scale-0 path forms the right-hand side as
  `(om*tm)*c` while the scalar path forms it as `(c*om)*tm`. Both are binary64
  and both narrow to float first, so they differ at most in the last ULP of the
  `double` product — a far smaller effect than the divergence fixed here, and
  entirely inside the golden-gated CPU lane. Not measured; not touched.
- HIP was changed identically to CUDA (the two files are byte-for-byte twins
  for this helper) but not executed: the AMD iGPU lane was not exercised in
  this PR.
- Metal cannot be built or executed on Linux; its MSL mirror of
  `adm_angle_flag_i64()` is verified only by review against the C header that
  the unit test pins.

## Related

- ADRs: [ADR-1194](../adr/1194-adm-angle-flag-single-source.md)
- Issues: [Netflix/vmaf#930](https://github.com/Netflix/vmaf/issues/930)
- State: `T-UPSTREAM-930-ADM-ANGLE-FLAG-PREDICATE-DIVERGENCE-2026-09-03`
