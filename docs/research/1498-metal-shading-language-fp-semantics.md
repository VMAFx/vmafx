<!-- markdownlint-disable MD013 -->
# Research-1498: Metal Shading Language floating-point semantics for exact twins

What a Metal kernel must do to return the CPU's bits, read from the Metal
Shading Language Specification, version 4.1 (Apple, dated 2026-06-04,
<https://developer.apple.com/metal/Metal-Shading-Language-Specification.pdf>,
retrieved 2026-10-03). Section numbers are that document's. Nothing here was
run on an Apple device; the facts are what the compiler and the GPU promise.

## Compiler options (section 1.6.3)

- The default is fast math: `-fmetal-math-mode=fast` (the compiler may
  assume no NaNs, no INFs, no signed zeros, use reciprocals for division,
  reassociate, and contract across statements) and
  `-fmetal-math-fp32-functions=fast`.
- `-fno-fast-math` equals `-fmetal-math-fp32-functions=precise
  -fmetal-math-mode=safe`. Safe mode prevents transformations that may change
  results but "sets the FP contract to on": `a * b + c` may still be fused
  within one statement.
- `-ffp-contract=off` (or `#pragma METAL fp contract(off)`) disables
  contraction. A twin needs both options; `core/src/metal/meson.build` gives
  every kernel `-fno-fast-math -ffp-contract=off` (ADR-1498).

## Accuracy with fast math off (section 8.4, Table 8.1)

Correctly rounded: `x + y`, `x - y`, `x * y`, `1.0 / x`, `x / y`, `sqrt`,
`rsqrt`, `fma`, `ceil`, `floor`, `fract`, `fdim`, `ldexp`, `rint`, `round`,
`trunc`. Exact (0 ulp): `fabs`, `copysign`, `fmax`, `fmin`, `fmod`,
`frexp`, `ilogb`, `modf`, `nextafter`. Not correctly rounded: `log2`, `log`,
`exp`, `exp2` (4 ulp), `cbrt`... `pow` and `powr` (16 ulp), `atan2` (6 ulp).
A twin may use the second group only as an estimate it then corrects (the
pair functions of `core/src/feature/ff_math.h` do), never as a value the CPU
computes with glibc.

## Number formats (sections 2.1, 8.1, 8.2)

- No `double`, `long long`, `unsigned long long` or `long double`. 64-bit
  integers exist as `long` / `int64_t` and `ulong` / `uint64_t`.
- fp32 subnormals passed to or produced by arithmetic "may be flushed to
  zero". A port must not depend on fp32 subnormals; where the CPU can produce
  one, only the device run tells.
- Rounding mode: round to nearest even or toward zero "may be supported";
  Apple GPUs round to nearest even (assumed, not measured here).

## Language (sections 1.5, 4, 5.3)

- Metal 4 is C++17-based, earlier revisions C++14-based; no C++ standard
  library. Pointers and references need an address space (`device`,
  `constant`, `threadgroup`, `thread`); program-scope variables live in
  `constant`; `static` applies to program-scope variables only, not to
  variables in functions.
- `as_type<T>()` reinterprets bits between types of the same size; `clz`,
  `popcount` and `mulhi` exist for integer types.
- `-std=metal3.1` is the revision of macOS 14 (section 1.6.10).

## Consequences for the ports

The CUDA twins' `__fmul_rn()` / `__fdiv_rn()` spelling and the HIP twins'
plain operators under `-ffp-contract=off` both map to plain MSL operators
under the strict list; an explicit `fma()` stands where the reference fuses.
The SYCL twins' fp64-free forms (exact fp32 pairs, integer replays of fp64
operations) are the template for every fp64 step, since Metal has no fp64 at
all. Device-independent proof is a host compile of the same arithmetic
header against the CPU reference.
