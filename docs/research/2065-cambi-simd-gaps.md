<!-- markdownlint-disable MD013 -->
# Research-2065: CAMBI SIMD gaps — AVX-512 and NEON for every stage AVX2 accelerates

- **Status**: Active
- **Workstream**: [ADR-1256](../adr/1256-cambi-spatial-mask-simd-dispatch.md) (measure before dispatch)
- **Last updated**: 2026-09-18

## Question

CAMBI's CPU extractor dispatches seven stages to AVX2: the anti-dithering
filter, the derivative row, the spatial-mask dp and mask rows, decimate, the
mode filter and the frame-level c-values driver (histogram range updates plus
the c-values row). AVX-512 and NEON covered only part of that, and several of
their kernels were built but never called. Which AVX-512 and NEON kernels beat
the path they would replace (AVX2 on an AVX-512 host, scalar on aarch64), and
are they bit-exact?

## Starting state

| Stage | AVX2 | AVX-512 | NEON |
| --- | --- | --- | --- |
| Anti-dithering filter | dispatched | missing | missing |
| Derivative row | dispatched | built, not called | built, not called |
| dp / mask rows | dispatched | dispatched ([Research-2062](2062-cambi-spatial-mask-simd.md)) | dp dispatched, mask kept undispatched |
| Decimate | dispatched | missing | missing |
| Mode filter | dispatched | missing | missing |
| c-values | dispatched | range updaters and row kernel built, not called | range updaters and row kernel built, not called |

The AVX-512 and NEON kernels went dead in `e3fd1c88a`, the port of upstream's
CAMBI optimisation batch. It replaced `init()`'s dispatch block with upstream's
AVX2-only version and removed the `CAMBI_CALC_C_VALUES_BODY` macro that had
built the AVX-512 and NEON c-values drivers. The commit records no correctness
reason; the kernels already used the compact histogram layout the batch
introduced.

`test_cambi.c`'s `test_calculate_c_values_scalar_avx2_parity` never compared
anything: it gated on `vmaf_get_cpu_flags()`, which returns 0 until
`vmaf_init_cpu()` runs, and nothing in that binary runs it. A breakpoint on
`calculate_c_values_avx2` is never hit in the pre-change build.

## Method

**x86.** A throwaway harness includes `cambi.c` (compiled with the release
flags of `feature_cambi.c.o`) for the scalar stages and links the build's
`libvmaf.a` for the SIMD kernels. One real frame is pushed through the scalar
pipeline to build the inputs every variant then sees: the 10-bit converted
image before and after anti-dithering, the spatial mask, and per scale the
image before and after the mode filter. Each kernel runs on identical inputs,
variants interleaved, best of 5 to 40 passes, pinned to one core of an AMD
Ryzen 9 9950X3D (Zen 5); three runs, minimum reported. Builds: GCC 16.2,
Clang 22.1 and icx 2026.0 (the compiler of the published container), all
`-O3`, no LTO.

Inputs: the Netflix `src01` 576x324 fixture (8-bit, and its 10-bit version);
BVI-DVC CloudsStatic and SkyscraperBangkok at 1920x1088 and SkyscraperBangkok
at 3840x2176 (the corpus copies are 8-bit although their names say 10bit: 64
frames per file, samples 14..241); and `src01` 10-bit upscaled to 1920x1080
and 3840x2160 with a lanczos filter at 10-bit precision, for 10-bit content at
the large window sizes.

The machine was shared (load average about 8). Run-to-run spread was 1–15 %,
and identical code linked at a different address moved the branchy c-values
walk by up to 20–30 %. Decisions below rest only on margins well outside that,
and hold under all three compilers.

**aarch64.** No hardware. Each kernel runs under `qemu-aarch64` with a small
TCG plugin that counts executed guest instructions; the count per pass is
(count with N passes − count with N no-op passes) / N, so setup and input
restores cancel. GCC 16.1 and Clang 22.1 cross builds. This measures work, not
time; it is the same criterion ADR-1256 applies to NEON.

## Findings

### x86: every AVX-512 stage beats AVX2

1920x1088 8-bit (CloudsStatic), µs per frame pass (speed-up of AVX-512 over
AVX2):

| Stage | GCC scalar / AVX2 / AVX-512 | Clang scalar / AVX2 / AVX-512 | icx scalar / AVX2 / AVX-512 |
| --- | --- | --- | --- |
| Anti-dithering filter | 1196 / 177 / 86 (2.07x) | 1177 / 148 / 84 (1.75x) | 981 / 166 / 134 (1.23x) |
| Derivative row (all rows) | 1204 / 71 / 54 (1.31x) | 1055 / 71 / 58 (1.24x) | 94 / 73 / 58 (1.25x) |
| Decimate (scales 1–4) | 166 / 35 / 27 (1.30x) | 120 / 35 / 27 (1.29x) | 269 / 118 / 82 (1.44x) |
| Mode filter (5 scales) | 851 / 216 / 152 (1.42x) | 852 / 217 / 158 (1.37x) | 878 / 324 / 296 (1.09x) |
| c-values (5 scales) | 1931 / 1796 / 605 (2.97x) | 3581 / 4454 / 584 (7.63x) | 3592 / 4453 / 751 (5.93x) |

Range of the AVX-512 speed-up over AVX2 across all seven inputs:

| Stage | GCC | Clang | icx |
| --- | --- | --- | --- |
| Anti-dithering filter | 2.07–2.44x | 1.73–2.03x | 1.23–2.47x |
| Derivative row | 1.31–1.79x | 1.12–1.50x | 1.17–1.45x |
| Decimate | 1.30–1.47x | 1.25–1.32x | 1.35–1.44x |
| Mode filter | 1.37–1.42x | 1.30–1.37x | 1.08–1.18x |
| c-values | 2.02–3.20x | 4.32–8.81x | 3.84–6.72x |

A whole CAMBI frame, single thread, CLI `--feature cambi`, frames per second
(best of 3):

| Input | GCC before | GCC now | Clang AVX2 only / now | icx AVX2 only / now |
| --- | --- | --- | --- | --- |
| src01 576x324 8-bit | 2024 | 2668 (1.32x) | 1410 / 2896 | 1446 / 2917 |
| Clouds 1920x1088 8-bit | 191 | 252 (1.32x) | 131 / 273 | 134 / 271 |
| src01 1920x1080 10-bit | 267 | 339 (1.27x) | 185 / 370 | 189 / 371 |
| Skyscraper 3840x2176 8-bit | 46.8 | 60.5 (1.29x) | 34.0 / 64.6 | 34.8 / 65.1 |
| src01 3840x2160 10-bit | 89.2 | 109.9 (1.23x) | 62.1 / 122.6 | 63.6 / 124.2 |

"Before" is the pre-change binary's default dispatch on this AVX-512 host.

What made the picture kernels fast: 32 lanes and, above all, **masked row
tails**. The first AVX-512 derivative row stopped its vector loop one vector
early and left up to 32 scalar columns per row; it measured 0.89x of AVX2 at
576x324. With a masked tail it is 1.5–1.8x there. The anti-dithering filter
also vectorises its last row (`(a & b) + ((a ^ b) >> 1)`, the exact floor
average). The 2x2 average stays in 16 bits without widening:
`floor(sum / 4) = sum(x >> 2) + floor(sum(x & 3) / 4)`, exact for any uint16
input. Decimate with one `vpermt2w` and with two `vpmovdw` measured the same.

### The c-values walk was bound by per-column bookkeeping, not by vector width

Wiring the old AVX-512 kernels into the scalar walk (per-column, as before
`e3fd1c88a`) gave 1.05–1.17x over AVX2, and within that the 512-bit range
updaters and the 16-lane row kernel were each within a few percent of their
256-bit / 8-lane counterparts. Hardware counters showed why. Over 20 passes at
1080p the upstream AVX2 walk retires 0.80 G instructions under GCC and 2.46 G
under Clang, and the hottest instructions under Clang are `uh_slide`'s
early-out: load mask and value for two rows, test the band, compare, return.
On flat, banding-prone content almost every column of the walk is masked out,
out of band, or equal to the pixel it replaces, so the loop is dominated by
columns that do nothing.

The fork's AVX-512 and NEON drivers therefore use a shared walk,
`core/src/feature/cambi_c_values_frame.h`. It calls the same `cambi.h` update
helpers as the scalar walk, but a per-ISA scan first tests up to 256 columns
with vector compares and returns one bit per column that may need an update;
the helper runs only for those. The scan may over-flag (the helper re-checks)
and never under-flags. Histogram updates within a row are modular uint16
increments and decrements that all land before that row's c-values are
computed, so the order does not matter and the histogram each row sees is the
scalar one: the output is byte-identical.

With the scan, the c-values speed-up over AVX2 is 2.0–3.2x under GCC and
3.8–8.8x under Clang and icx. A control that runs the same walk with an
all-columns scan is 0.57–0.61x of AVX2, so the gain is the scan, not the
vectors. Within the scanned walk the range updaters' width makes no difference
(512-bit masked, 256-bit masked and AVX2's 256-bit-plus-scalar all within
±3 %), and the AVX-512 row kernel is 3–6 % faster than the AVX2 row kernel
under every compiler.

**Register pressure (ADR-1254).** With the scans inlined, Clang hoisted their
two broadcast constants out of the row loop and spilled them around every call
to the row kernel (`vmovdqu64 %zmm2,0xf0(%rsp)`); a wide spill is what faults
under the Win64 ABI with MinGW GCC. The scans are therefore `noinline` and
build their constants per call, and each call covers a 256-column block to
keep the call count down. That costs 2–21 % (GCC 4–21 %, Clang 2–10 %) against the inlined, spilling
version and leaves no zmm/ymm stack access in the GCC or Clang object. The
MinGW object itself could not be built here; `check-win64-stack-alignment.py`
reports nothing on the Linux object.

### aarch64: NEON removes most instructions except in the mode filter

Instructions per frame pass, NEON / scalar (1920x1088 8-bit; the other inputs
agree within ±0.01 except c-values, which is content-dependent):

| Stage | GCC | Clang | Scalar code the compiler emits |
| --- | --- | --- | --- |
| Anti-dithering filter | 5.57 M / 35.5 M (0.157) | 4.56 M / 33.5 M (0.136) | not vectorised (in place, 17 insns/px) |
| Derivative row | 3.00 M / 33.4 M (0.090) | 3.25 M / 29.2 M (0.111) | not vectorised (16 insns/px) |
| Decimate | 0.79 M / 4.86 M (0.163) | 0.97 M / 4.17 M (0.232) | not vectorised (7 insns/output) |
| Mode filter | 10.6 M / 11.3 M (0.934) | 12.1 M / 11.8 M (1.024) | vectorised: `cmeq`, `umin`, `bit`, same as the NEON kernel |
| c-values | 17.3 M / 39.4 M (0.44) | 22.8 M / 99.3 M (0.23) | range updates vectorised; per-column walk scalar |

c-values across the five inputs: 0.40–0.44 (GCC), 0.21–0.23 (Clang).
Attribution, 1080p, GCC / Clang: the old per-column walk with the NEON range
updaters and row kernel is 1.31 / 1.35x *more* instructions than scalar; the
same walk with a scalar scan is 0.83 / 0.45x; with the NEON scan 0.44 / 0.23x.
The NEON row kernel's eight-column mask skip saves 20–47 % against the scalar
row. The NEON range updaters were 0.3–0.9 % *more* instructions than the plain
C loops, which both compilers turn into the same eight-lane adds.

### Decisions

| Kernel | AVX-512 | NEON | Evidence |
| --- | --- | --- | --- |
| `anti_dithering_filter_*` (new) | dispatched | dispatched | x86 1.23–2.47x vs AVX2; NEON 0.14–0.16 of scalar |
| `get_derivative_data_for_row_*` (dead) | re-wired, rewritten with a masked tail | re-wired (tidied) | 1.12–1.79x vs AVX2; NEON 0.09–0.12 |
| `decimate_*` (new) | dispatched | dispatched | 1.25–1.47x; NEON 0.16–0.24 |
| `filter_mode_*` (new) | dispatched | **not dispatched**, parity-tested | 1.08–1.42x; NEON 0.93 (GCC) / 1.02 (Clang) |
| `calculate_c_values_*` frame driver (new) | dispatched | dispatched | 2.0–8.8x; NEON 0.21–0.44 |
| `calculate_c_values_row_*` (dead) | re-wired inside the driver | re-wired inside the driver | 3–6 % over the AVX2 row; NEON saves 20–47 % |
| `cambi_increment/decrement_range_avx512` (dead) | re-wired inside the driver (masked tail) | — | no worse than any 256-bit alternative (±3 %) |
| `cambi_increment/decrement_range_neon` (dead) | — | **retired** | 0.3–0.9 % more instructions than plain C |

### Bit-exactness

- `test_cambi_stage_simd` compares every kernel above, on AVX2, AVX-512 and
  NEON, against the shipped scalar stage, over widths covering every tail
  residue of 8-, 16- and 32-lane loops, 8-bit, 10-bit, full-range and
  low-entropy content, guard bands and sentinel padding, `max_log_contrast` 0
  to 5, BT.1886 and PQ, windows 3 to 65, and the final histogram as well as the
  c-values. Deliberately broken kernels (tail overruns, a dropped remainder, a
  wrong tie-break, a scan that misses columns, a derivative that reads past the
  row) all fail it.
- `test_cambi_dispatch_invariance` drives the extractor through the public API
  at each dispatch level and requires bit-identical per-frame scores.
- CLI `--feature cambi --precision max` JSON (minus `fps` and `version`) is
  byte-identical across default, AVX2-only (`--cpumask 48`) and scalar
  (`--cpumask 65535`) dispatch, across GCC, Clang and icx builds, and against
  the pre-change binary, on 18 inputs and option sets: 8- and 10-bit, 4:2:0 and
  odd-size 4:4:4 (575x323, 1283x721), full reference, `max_log_contrast` 0 and
  5, window 33, PQ with a visibility threshold, `enc_width` downscaling and
  `cambi_high_res_speedup` 1080 and 2160. Under `qemu-aarch64`, NEON and scalar
  JSON are identical for the same 18 cases, GCC and Clang builds.

### Outside this change

- **Under Clang and icx the dispatched AVX2 c-values driver is slower than
  scalar**: 4454 against 3581 µs at 1080p (0.80x), 20013 against 16714 µs at
  2160p (Clang; icx the same). The published container is built with icx, so
  an AVX2-only host runs this stage slower than the scalar code would. GCC
  gives 1.07x. The upstream AVX2 walk is also layout-sensitive: the same object
  code measured 1.05x of scalar in an earlier Clang build. Routing AVX2 through
  the shared walk with an AVX2 scan is the obvious fix; it is not measured
  here. Tracked in `docs/state.md`.
- The test that should have caught a divergence in the AVX2 c-values driver
  was not running (see *Starting state*); it now gates on CPUID.

## Alternatives explored

- **Re-wire the dead kernels as they were.** AVX-512 would gain 1.05–1.17x
  (inside the layout noise); NEON would execute 31–36 % more instructions than
  scalar. Rejected.
- **Put the column scan into the upstream AVX2 walk too.** Out of scope here
  and a change to upstream-mirror code; recorded as the fix for the AVX2 row in
  `docs/state.md`.
- **Keep the scans inlined.** 2–21 % faster, but Clang spills two zmm
  broadcasts around every row-kernel call (ADR-1254). Rejected.
- **512-bit range updaters vs 256-bit.** Measured equal; the AVX-512 TU keeps
  its masked 512-bit updaters.
- **Dispatch the NEON mode filter for symmetry.** No instruction-count gain on
  either compiler; kept built and parity-tested, like `compute_mask_row_neon`.
- **Keep the NEON range updaters undispatched but tested.** They are not a
  stage, only a helper the driver no longer needs; retired.

## Open questions

- Real aarch64 timing of the NEON kernels (instruction counts only).
- Intel AVX-512 cores (frequency licences, port layout); only Zen 5 measured.
- The MinGW-built AVX-512 object was not inspected locally; the Windows CI lane
  runs `check-win64-stack-alignment.py` on it.

## Related

- [ADR-1256](../adr/1256-cambi-spatial-mask-simd-dispatch.md), [Research-2062](2062-cambi-spatial-mask-simd.md)
- [ADR-1254](../adr/1254-win64-cannot-realign-the-stack.md) (wide vector spills on Win64)
- [ADR-1207](../adr/1207-feature-isa-invariance-gate.md) (scores must not depend on the ISA)
- [CAMBI CPU SIMD paths](../metrics/cambi.md#cpu-simd-paths)
