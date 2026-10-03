<!-- markdownlint-disable MD060 -->
# CIEDE2000 — colour-difference metric

CIEDE2000 converts both YCbCr frames to CIELAB and computes the ΔE per pixel,
averaged over the frame. It captures chroma distortion that luma-only metrics
miss: chroma subsampling, colour-space conversion errors and 4:2:0 against
4:4:4 differences. Run it with `--feature ciede`; [features](features.md) lists
every extractor.

## Run it

- CLI: `--feature ciede`.
- C API: `vmaf_use_feature(ctx, "ciede", NULL)`.

**Output metrics** — `ciede2000`.

**Output range** — `[0, ~100]`. Smaller is better.

- `< 1` — imperceptible difference.
- `1–5` — perceptible on close inspection.
- `> 15` — obviously different colour.

**Input formats** — YUV 4:2:0 / 4:2:2 / 4:4:4, 8 / 10 / 12 / 16 bpc. Requires
chroma — **does not accept 4:0:0**.

**Options** — none.

**Limitations** — Assumes BT.709 YCbCr → RGB → CIELAB. No override for
BT.2020 or BT.601 input yet. Ported from the `av-metrics` Rust crate.

## Backends

| Path | Name | Device measured | Frames identical to the CPU (of 180) | Largest difference | Time per 3840x2160 frame |
| --- | --- | --- | --- | --- | --- |
| CPU | `ciede` (AVX2, AVX-512, NEON) | — | — | — | 136 to 222 ms on 16 threads (two hosts), 2.5 s on one thread |
| CUDA | `ciede_cuda` | RTX 4090 | 127 | 5.2e-12 | 32.7 ms |
| SYCL | `ciede_sycl` | Arc A380 | 124 | 5.2e-12 | 50.3 ms |
| HIP | `ciede_hip` | gfx1036 (ROCm 7.2.4) | 127 | 5.2e-12 | 210 ms |
| Metal | `integer_ciede_metal` | not measured | — | — | — |

The three device twins are not bit-identical: the C library's `powf` is not
correctly rounded, so a few pixels per frame round to the neighbouring `float`.
The parity gate bounds each of them at `1e-9` through `LIBM_TWINS["ciede"]`
(`scripts/ci/cross_backend_calibration.py`), not at zero, and no
`exact_twins.d` fragment exists for `ciede`. The Metal twin is not listed in
`LIBM_TWINS`, so its cell uses the gate's default for `ciede`, `5e-3`; it has
not been run on a device.

Frame counts and differences are measured at `--precision max` against a GCC
build's `--backend cpu` since
[ADR-1467](../adr/1467-ciede-squares-as-products.md); the timings are from the
per-twin sections below and were measured before that change.

## Agreement with the CPU across compilers

A GCC build and a clang build return the same `ciede2000` on x86-64 and on
aarch64 ([ADR-1467](../adr/1467-ciede-squares-as-products.md)). This fork
writes the squares as products in the source, so the value no longer depends on
the compiler or on the C library.

Why it was needed:

- The formula squares a `float` in its rotation term and 13 values in
  `double`. Upstream writes those as `powf(x, 2)` and `pow(x, 2)`; GCC calls the
  C library for them and clang multiplies.
- For the `double` squares that makes no difference. glibc's `powf(x, 2)`
  returns the other neighbouring `float` on about 0.12 % of the arguments, so
  the two builds used to differ on 65 of 180 measured frames, by at most
  2.0e-11.

What changed for your scores:

- Scores from a GCC build moved by up to 2.0e-11 (on the Netflix 576x324 pair:
  frame 35, by 6.9e-13). Scores from a clang or icx build did not.
- A GCC build takes about a third less time in this extractor, since 14 library
  calls per pixel are gone: 563 against 837 ms per 1920x1080 frame on one busy
  core of a Ryzen 9 9950X3D.
- An icx build still linked Intel's math library and differed from a GCC build
  through the other functions of the formula, by at most 9.7e-12 on the same
  frames. Since [ADR-1495](../adr/1495-icx-system-libm.md) it links glibc's and
  returns the GCC build's values.

Check two builds with:

```bash
for cc in gcc clang; do
  build-$cc/tools/vmaf -r ref.yuv -d dis.yuv -w 1920 -h 1080 -p 420 -b 8 \
      --no_prediction --feature ciede --precision max --json -o ciede-$cc.json
done
diff <(grep -v '"fps"' ciede-gcc.json) <(grep -v '"fps"' ciede-clang.json)
```

## Agreement with Netflix's `ciede`

`ciede2000()` forms two products of `float` values in `float`, as Netflix's
source does ([ADR-1476](../adr/1476-ciede-upstream-expression.md)):

- `c_prime_1 * c_prime_2` under a square root;
- `r_sub_t * chroma * hue` in the final sum.

Between May and October 2026 this fork widened their first operand to
`double`, which kept the products exact and moved the score away from
Netflix's by up to 1.3e-9 (1.0e-8 on frames of 24x24 and smaller).

Measured against Netflix master (`9e48141b`, GCC 16.2.1, glibc 2.44) at
`--precision max` on 327 frames from 8x8 to 3840x2160: 153 are identical now
(7 before). The others differ for three recorded reasons:

| Reason | Frames | Largest difference |
| --- | --- | --- |
| The `float` square, which upstream writes as `powf(x, 2)` | 119 | 2.2e-11 with glibc 2.44 |
| 4:2:2 input: upstream reads the chroma planes with the two subsampling flags swapped | 48 | 0.153 |
| Odd frame sizes with subsampled chroma: upstream rounds the chroma size down | 7 | 0.198 |

With glibc 2.43, a GCC 15 build and a clang 22 build of upstream return this
fork's values on all 96 frames of the Netflix pair and Big Buck Bunny at
1920x1080.

The CUDA, SYCL and HIP twins form the same two `float` products. Against a GCC
build's `--backend cpu` on 153 frames (the Netflix 576x324 pair at 8 and 10
bits and as 10-bit 4:2:2, both 1920x1080 checkerboard pairs, 48 frames of Big
Buck Bunny at 3840x2160), `ciede_cuda` (RTX 4090) is identical on 109 frames,
`ciede_sycl` (Arc A380) on 107 and `ciede_hip` (gfx1036) on 108, and none
differs by more than 2.1e-12. The per-twin figures further down were measured
before this change.

## The twins since ADR-1467

The three twins compute the squares as products, as the CPU now does. Measured
at `--precision max` against a GCC build's `--backend cpu` on 180 frames (the
Netflix 576x324 pair at 8, 10, 12 and 16 bits and as 10-bit 4:2:2, Sparks,
both 1920x1080 checkerboard pairs, BBB 1920x1080 and 3840x2160):

| Twin | Device | Frames identical | Largest difference | Before ADR-1467 |
| --- | --- | --- | --- | --- |
| `ciede_cuda` | RTX 4090 | 127 | 5.2e-12 | 113 frames, 2.0e-11 |
| `ciede_sycl` | Arc A380 | 124 | 5.2e-12 | 111 frames, 2.0e-11 |
| `ciede_hip` | gfx1036 | 127 | 5.2e-12 | 113 frames, 2.0e-11 |

The Netflix pair at 8 bits is identical on all 48 frames for all three. What is
left is the C library's `powf(x, 7)`, which the twins round correctly and glibc
does not always, and on SYCL and HIP the precision of a pair.

## Per-twin measurements

The figures in the three sections below were measured before ADR-1467.

### `ciede_cuda` and the CPU

The CUDA twin evaluates the CPU's formula in the CPU's precision (double, with
`float` stores where the CPU has them) and adds the per-pixel values in the
CPU's order ([ADR-1426](../adr/1426-cuda-ciede-cpu-arithmetic.md)).

- **Measured** on an RTX 4090 at `--precision max`: 62 of 113 frames identical
  to `--backend cpu`, the rest within 1.4e-11 (576x324 at 8 to 16 bits,
  1920x1080, 3840x2160).
- **Before ADR-1426** it was up to 1.1e-5 away, and stored `ciede_cuda` outputs
  differ from new ones by that much.
- **Not bit-identical** because the CPU calls the C library's math functions
  and the device calls CUDA's: a few pixels per million round to the
  neighbouring `float`.
- **Cost** — the double-precision math takes 32.7 ms per 3840x2160 frame on an
  RTX 4090 (2.8 ms before), against 222 ms for the CPU extractor on sixteen
  threads.

### `ciede_sycl` and the CPU

The SYCL twin runs the same statements. A SYCL kernel has no `double`, so every
`double` of the CPU's formula is a pair of `float` values and every math
function (`pow`, `cbrt`, `atan2`, `sin`, `cos`, `exp`) a routine on such pairs
([ADR-1436](../adr/1436-sycl-ciede-cpu-arithmetic.md)).

- **Measured** on an Arc A380 at `--precision max` against `--backend cpu`: the
  Netflix 576x324 pair identical on 47 of 48 frames (6.9e-13 on the other), its
  10-, 12- and 16-bit and 4:2:2 versions and both 1920x1080 checkerboard pairs
  identical on every frame, 3840x2160 within 1.4e-11 on 200 frames. Those are
  the CUDA twin's figures.
- **Before ADR-1436** the twin was up to 1.1e-5 away, and stored `ciede_sycl`
  outputs differ from new ones by that much.
- **Not bit-identical** for the reason the CUDA twin is not: the C library's
  `powf` is not correctly rounded, and on a 3840x2160 frame 18 to 64 of 8.3
  million pixels round to the neighbouring `float`.
- **Cost** — the pair arithmetic takes 50.3 ms per 3840x2160 frame on an Arc
  A380 (16.2 ms before) and 1.2 ms at 576x324 (0.45 before); the CPU extractor
  takes 2.5 s per 3840x2160 frame on one thread. The twin keeps one `float` per
  pixel on the device and on the host, 33 MB each at 3840x2160.

### `ciede_hip` and the CPU

The HIP twin runs the SYCL twin's statements, from the same source file, on an
AMD device ([ADR-1448](../adr/1448-hip-ciede-cpu-arithmetic.md)). An AMD device
has `double`, but its double-precision math functions took 17 times the frame
time on an integrated GPU, so the twin uses the pairs of `float` values
instead. The twin is bounded at `1e-9` (`LIBM_TWINS["ciede"]["hip"]`), not at
the `5e-3` default.

- **Measured** on a gfx1036 (ROCm 7.2.4) at `--precision max` against
  `--backend cpu`: 115 of 178 frames identical and the rest within 1.4e-11. The
  Netflix 576x324 pair is identical on 47 of 48 frames, its 10-, 12- and 16-bit
  versions and both 1920x1080 checkerboard pairs on every frame, 3840x2160
  within 1.4e-11.
- **Before ADR-1448** the twin was up to 1.1e-5 away, and stored `ciede_hip`
  outputs differ from new ones by that much.
- **Not bit-identical** for the reasons the SYCL twin is not: the C library's
  `powf` is not correctly rounded, which moves at most 74 of the 8.3 million
  pixels of a 3840x2160 frame to the neighbouring `float`, and a pair holds 48
  bits where a `double` holds 53, which moved 8 of 437 million pixels.
- **Cost** — 49.6 ms per 1920x1080 frame on a gfx1036 (18.6 ms before) and 210
  ms per 3840x2160 frame (75.6 ms before), where the CPU extractor takes 136 ms
  on sixteen threads (`T-HIP-CIEDE-EXACT-THROUGHPUT-2026-10-02`).

Check it with:

```shell
python3 scripts/dev/speed_gpu_parity.py --backend hip --feature ciede \
    --max-abs-diff 1e-9 --vmaf "$PWD/build-hip/tools/vmaf"
```
