<!-- markdownlint-disable MD013 MD060 -->
# ADR-1226: Size the CUDA AIM CM launch by SM count, not by a fixed rows-per-thread

- **Status**: Accepted
- **Date**: 2026-09-07
- **Deciders**: Lusoris
- **Tags**: cuda, performance, adm, fork-local

## Context

`adm_cm_aim_line_kernel_8` is the fork's most register-hungry CUDA kernel. On
sm_89 `nvcc -Xptxas -v` reports 255 registers, a 344-byte stack frame and
412/404 bytes of spill stores/loads. With a 128-thread block that allows only
two blocks per SM — 16.7% theoretical occupancy — which is what flagged it for
this pass.

The obvious remedy is `__launch_bounds__`. Measuring it first produced two
surprises, and they changed the fix.

## Decision

We will pick the kernel's `rows_per_thread` at launch from the device's SM
count — 4 when that yields at least one block per two SMs, otherwise 2 —
instead of the fixed 8, and we will **not** add `__launch_bounds__`.

The AIM CM kernel's only parallelism is one block per `BLOCKY *
rows_per_thread` rows of the `buffer_h`-row band, times the three orientation
bands. There is no x-decomposition, and there cannot cheaply be one: each row's
warp reduction has to cover the whole row before the single
`>> shift_inner_accum` rounding step, so splitting a row across blocks would
round differently from the CPU reference. `rows_per_thread` is therefore the
only knob that changes how much of the GPU the launch uses — and at 8 it left
most of it idle.

Halving it costs nothing arithmetically: each row is still reduced across all
of its columns inside one block, so the emitted score is bit-identical.

## Measurements

All on an RTX 4090 (128 SMs), CUDA 13.3, 48 frames, `vmaf_bench --gpu-only`,
warm (first three runs discarded).

**Register pressure and spill**, sm_89, `nvcc -Xptxas -v`:

| variant | registers | stack | spill st / ld |
| --- | --- | --- | --- |
| `rows=8` (before) | 255 | 344 B | 412 / 404 B |
| `rows=8` + `__launch_bounds__(128, 3)` | 168 | 696 B | 828 / 1020 B |
| `rows=8` + `__launch_bounds__(128, 4)` | 128 | 848 B | 1028 / 1380 B |
| `rows=8` + `__launch_bounds__(128, 5)` | 96 | 8 B | 16 / 16 B |
| `rows=4` | 255 | 16 B | 32 / 32 B |
| `rows=2` | 189 | 0 | 0 / 0 |
| `rows=1` | 148 | 0 | 0 / 0 |

**First surprise**: `__launch_bounds__(128, 5)` does not trade spill for
occupancy. Capped at 96 registers it spills *less* than the unconstrained
build (8 B stack vs 344 B), because at 255 registers ptxas keeps the whole
unrolled theta x row working set live rather than rematerialising it.

**Second surprise**: that bought nothing. Kernel time per call, measured with
a host clock around the launch plus a stream synchronise:

| variant | 1920x1080 |
| --- | --- |
| `rows=8` (before) | 0.803 ms |
| `rows=8` + `__launch_bounds__(128, 5)` | 0.808 ms |

Occupancy was never the constraint. A 1080p frame gives `buffer_h = 434`, so
at `rows=8` the launch is `ceil(434 / 32) x 3 = 42` blocks — against 128 SMs.
Two thirds of the device is idle by construction, and blocks-per-SM is
irrelevant when there is less than one block per SM to place.

**`rows_per_thread` sweep**, mean ms per kernel call:

| resolution | rows=8 | rows=4 | rows=2 | rows=1 |
| --- | --- | --- | --- | --- |
| 1920x1080 | 0.801 | **0.553** | 0.586 | 0.622 |
| 640x480 | 0.299 | 0.203 | **0.159** | — |
| 576x324 | 0.300 | 0.178 | **0.141** | — |

The optimum tracks block count rather than frame size: 4 wins once the frame
is large enough to keep roughly half the SMs busy, 2 wins below that, and 1
regresses even at 1080p (327 blocks) — past the point where more blocks pay
for the extra per-thread setup. Hence the SM-count-relative rule rather than a
resolution threshold.

Stacking `__launch_bounds__(128, 4)` on top of `rows=4` made it slower again
(0.572 ms vs 0.553 ms), which is why the attribute is absent and the kernel
carries a comment saying so.

**Whole-feature effect**, `adm (CUDA)` mean ms per frame:

| resolution | before | after | delta |
| --- | --- | --- | --- |
| 1920x1080 | 1.31 | 1.06 | **-19.1%** |
| 640x480 | 0.46 | 0.33 | **-28.3%** |
| 576x324 | 0.39 | 0.27 | **-30.8%** |

**Numerics**: `vmaf_bench --validate` reports identical CPU-vs-CUDA
`max_diff` values before and after, to the last digit, for `adm2_score` and
all four `integer_adm_scale*` outputs (5.42e-08 / 8.26e-08 / 3.15e-08 /
1.13e-07 / 4.65e-08). The change moves work between threads; it does not
reorder any accumulation.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| `__launch_bounds__(128, 5)` on the existing kernel | The obvious register-pressure fix; genuinely cuts registers 255 → 96 and spill 344 B → 8 B | Measured at 0.808 ms vs 0.803 ms — no effect, because the kernel is grid-limited, not occupancy-limited. Stacked on the chosen fix it is a 3.4% regression | Rejected on measurement; the kernel carries a comment recording this so it is not "helpfully" re-added |
| Fixed `rows_per_thread = 4` | One instantiation, no device query | Leaves 21% on the table at 576x324 and 27% at 640x480, where the frame is too small for 4 to fill the SMs | Rejected — the device query is one call at init |
| Fixed `rows_per_thread = 2` | Best at both small resolutions | 6% slower than 4 at 1080p, and the gap widens with resolution — the wrong way round for a video-quality tool | Rejected |
| Decompose the x dimension across blocks | Would remove the grid ceiling entirely and scale to any SM count | Each row's warp reduction must cover the full row before the single `>> shift_inner_accum` rounding; splitting it changes the rounding and breaks bit-exactness with the CPU reference | Rejected — a correctness change dressed as an optimisation. Worth revisiting only with a two-pass reduction that keeps the shift at row granularity |
| A resolution threshold (`buffer_h >= 300 ? 4 : 2`) | No device query | Fits three data points on one GPU; wrong on any device with a different SM count, which is most of them | Rejected in favour of the SM-relative rule |

## Consequences

- **Positive**: 19-31% off the CUDA ADM feature depending on resolution, with
  bit-identical output. The rule adapts to the device instead of being tuned
  to a 4090.
- **Negative**: two kernel instantiations instead of one in `adm_cm.cu`
  (`rows_per_thread` 2 and 4), so the ADM CM fatbin carries one extra entry
  point per architecture. One `cuDeviceGetAttribute` call per feature-extractor
  init.
- **Neutral / follow-ups**: the SYCL, HIP and Metal AIM CM twins have the same
  fixed `rows_per_thread = 8` shape and are likely equally grid-starved, but
  their occupancy arithmetic differs per device and none was measured here.
  The x-decomposition option above remains the only route past the grid
  ceiling on very large frames.

## References

- CUDA C++ Programming Guide, §"Launch Bounds" and §"Occupancy Calculator" —
  <https://docs.nvidia.com/cuda/cuda-c-programming-guide/>
- req: the user asked for the ADM register-pressure item to ship as a separate,
  measured PR.
- [ADR-0746](0746-cuda-integer-adm3-aim-parity.md) — introduced the AIM CM GPU kernels.
- [ADR-0214](0214-gpu-parity-ci-gate.md) — the GPU parity gate this change
  leaves untouched.
