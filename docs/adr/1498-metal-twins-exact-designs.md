<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1498: Metal twins take the exact designs of their CUDA, HIP and SYCL twins, on a strict FP kernel policy

- **Status**: Accepted
- **Date**: 2026-10-03
- **Deciders**: Lusoris
- **Tags**: metal, gpu, parity, numerics, fork-local

## Context

`v1.0.0-rc.3` waits for the Metal twins to be ported to the exact designs of
their CUDA, HIP and SYCL twins and proven on an Apple device by an outside
tester's report (ADR-1496). The CUDA, HIP and SYCL twins of `float_psnr`,
`float_moment`, `ssim`, `ciede`, `float_adm`, `float_motion`, `float_vif`,
`float_ms_ssim`, `float_ssim`, `motion` and `adm` reproduce the CPU's
arithmetic one rounding at a time; the open Metal rows of `docs/state.md`
record where the Metal twins do not.

Three constraints shape a Metal port. The Metal compiler's default is fast
math: no NaNs or INFs, reciprocals for division, reassociation and
contraction across statements; with fast math off, fp32 `+ - * /`, `sqrt`
and `fma` are correctly rounded, and `-ffp-contract=off` stops the
contraction the safe mode still allows within a statement (Metal Shading
Language Specification 4.1, sections 1.6.3 and 8.4;
[Research-1498](../research/1498-metal-shading-language-fp-semantics.md)).
Metal has no `double`, so the fp64 steps of the references take the SYCL
twins' fp64-free forms (exact fp32 pairs and fp64 operations replayed in
64-bit integers). And no Apple device runs anything here: a kernel is
compiled only by the hosted macOS runner and measured only by the tester.

## Decision

Every `.metal` kernel compiles with one strict list,
`-fno-fast-math -ffp-contract=off` (`metal_shader_strict_fp_args` in
`core/src/metal/meson.build`, the Metal counterpart of ADR-1403 and
ADR-1367), and may include the backend-neutral headers of `core/src/feature/`
and the Metal arithmetic headers of `core/src/feature/metal/`. A port moves
the per-sample arithmetic of a twin into a header written on
`core/src/feature/metal/metal_portable.h`, in the subset Metal Shading
Language and host C++ share (values in and out, no `double`, no `long long`,
no pointer without an address space), so the same header is the kernel's
arithmetic and is compiled on the host by a test that holds it against the
CPU reference value by value. Host-side steps call the CPU's own helpers
from the Objective-C++ (`psnr_score.h`, `float_motion_sad.h`,
`ciede_frame_sum.h`, `adm_float_reference.h`, `vif_get_filter()`,
`adm_frame_size_check()`, `motion_clip()`). Where a SYCL fp64-free header
cannot be included from Metal (it is written against `sycl::` and C++20),
the Metal header follows it statement for statement; the two are one
behaviour in two files until RC5 merges them
(`T-METAL-SYCL-FP64-FREE-ARITHMETIC-COPIES-2026-10-03`). Each port carries a
device-free source contract that fails on a planted return of the construct
it removes. A row closes only with the tester's report.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Make the SYCL fp64-free headers backend-neutral and include them from Metal | One file per behaviour now | They use `sycl::bit_cast`, `<bit>`, `<limits>`, C++17 inline variables and designated initializers; the change touches every exact SYCL twin, whose evidence (A380, 19 AOT targets) would need re-measuring, while other lanes edit the same files; whether Metal accepts the rest cannot be tried here | RC5 owns the merge, with the Metal evidence in hand |
| Keep the compiler's fast math and fix each kernel by hand | No build change | Fast math reorders and contracts behind any source-level care; reciprocal division and no-NaN assumptions break the references' arithmetic | The twins exist to return the CPU's bits |
| Port the kernels without host-side copies of the arithmetic | Less code | Nothing would check a port before the tester's device run, which happens once | The host test is the strongest evidence available without a device |
| Wait for an Apple device in the project | Ports could be run before review | No such device; the maintainer chose the tester's report | `Q`: wait for the ports and the report |

## Consequences

- **Positive**: the Metal twins compute what the CUDA, HIP and SYCL twins
  compute, and their arithmetic is tested on every host before the device
  run; the report of ADR-1496 measures the rest.
- **Negative**: until RC5, the fp64-free arithmetic exists twice (SYCL and
  Metal headers); a change to a CPU reference changes both in the same PR.
  Kernels without fast math may run slower; no Metal performance is measured
  before RC7.
- **Neutral / follow-ups**: fp32 subnormals may be flushed on an Apple GPU
  (specification section 8.1); the device run shows whether a port depends on
  them. After a passing report the maintainer closes the rows and adds the
  `.metal` exact-twin fragments.

## References

- [ADR-1496](1496-metal-gate-in-tester-bundle.md) (the report that measures
  the ports), [ADR-1403](1403-cuda-strict-fp-every-kernel.md),
  [ADR-1367](1367-sycl-strict-fp-every-feature-tu.md),
  [ADR-0220](0220-sycl-fp64-fallback.md).
- Metal Shading Language Specification 4.1 (Apple, 2026-06-04),
  <https://developer.apple.com/metal/Metal-Shading-Language-Specification.pdf>.
- Source: `Q` (popup 2026-10-03): "Wait for Metal ports + report".
