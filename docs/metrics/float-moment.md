<!-- markdownlint-disable MD060 -->
# Float moment — first and second statistical moments

`float_moment` computes the mean (first moment) and mean of squares (second
moment) of the reference and distorted luma planes. It is a building block for
higher-level statistical metrics and a sanity-check extractor when validating
decoder output. Run it with `--feature float_moment`; [features](features.md)
lists every extractor.

## Run it

- CLI: `--feature float_moment`.
- ffmpeg: `libvmaf=feature=name=float_moment`.
- C API: `vmaf_use_feature(ctx, "float_moment", NULL)`.

Output metrics:

- `float_moment_ref1st`, `float_moment_dis1st` — first moment (mean).
- `float_moment_ref2nd`, `float_moment_dis2nd` — second moment (mean of
  squares).

**Output range** — for 8-bit luma, `[0, 255]` for the first moment and
`[0, 65 025]` for the second; scales with `2^bpc - 1`.

**Input formats** — YUV 4:2:0 / 4:2:2 / 4:4:4, 8 / 10 / 12 / 16 bpc. Y plane
only.

**Options** — none.

**Limitations** — Stateless per frame. Float pipeline: the picture plane is
copied to float32 before the moments are computed. A fixed-point twin is not
shipped.

## Backends

| Path | Name | Exactness against the CPU |
| --- | --- | --- |
| CPU | `float_moment` (scalar, AVX2, AVX-512, NEON, SVE2) | reference |
| CUDA | `float_moment_cuda` | `exact` (`exact_twins.d/float_moment.cuda`) |
| SYCL | `float_moment_sycl` | `exact` |
| HIP | `float_moment_hip` | `exact` |
| Metal | `float_moment_metal` | not declared exact |

With `--backend cuda`, `sycl`, `hip` or `metal`, `--feature float_moment` runs
that backend's twin, and the `feature_backends` list of the JSON output names
it:

```shell
vmaf ... --backend cuda --feature float_moment   # runs float_moment_cuda
```

## How the twins match the CPU

The GPU kernels accumulate four integer sums per frame in a single dispatch.
The CPU squares each sample in fp32, which rounds above 12 bits.
`float_moment_cuda`
([ADR-1453](../adr/1453-cuda-float-moment-cpu-float-squares.md)),
`float_moment_hip` (ADR-1447) and `float_moment_sycl`
([ADR-1449](../adr/1449-sycl-float-moment-cpu-float-squares.md)) add that
fp32 square as an integer.

On a 16-bit frame of more than 2 097 152 pixels the sum of squares can pass
2^53 units, and from there the CPU's own `double` rounds as it adds. Since
[ADR-1497](../adr/1497-float-moment-twins-cpu-sum-past-2-53.md) the twins form
that rounded sum on the device and equal the CPU extractor at 8, 10, 12 and
16 bits on every frame.

## How the CPU SIMD paths match the scalar path

The CPU kernels return the same bits whichever instruction set runs: the
AVX2, AVX-512, NEON and SVE2 kernels square in vectors and add the values
into one `double` one after the other, in the scalar loop's order. The second
moment of a 16-bit 4K frame is therefore the same on x86 and aarch64 and on
every SVE vector length. `test_moment_simd` has the cases.

## History

- 2026-10-03 — [ADR-1500](../adr/1500-arm-float-moment-scalar-order.md): the
  NEON and SVE2 kernels used to add in lanes and could return a different last
  digit on 16-bit 4K frames; they now add in the scalar loop's order.
- 2026-10-01 — `--backend <gpu> --feature float_moment` computed the feature on
  the CPU and warned that the backend had no twin; the twin ran only when
  named (`--feature float_moment_cuda`). It now runs the twin.
