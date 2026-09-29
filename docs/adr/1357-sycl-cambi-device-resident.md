<!-- markdownlint-disable MD013 MD060 -->
# ADR-1357: Run the SYCL CAMBI extractor entirely on the device

- **Status**: Accepted
- **Date**: 2026-09-29
- **Deciders**: lusoris
- **Tags**: sycl, gpu, cambi, performance, numerics, rc3, fork-local

## Context

`cambi_sycl` ([ADR-0415](0415-cambi-sycl-port.md), refined by [ADR-0458](0458-sycl-cambi-ssim-slm-staging.md) and [ADR-0489](0489-cambi-sycl-event-chain.md)) followed the Strategy II hybrid of [ADR-0205](0205-cambi-gpu-feasibility.md): it preprocessed the distorted picture on the host, uploaded it row by row, ran the spatial mask, decimation and mode filter on the GPU, and then, for each of the five scales, waited for the queue, copied the image and mask back, and ran `vmaf_cambi_calculate_c_values` (the sliding-histogram pass) and `vmaf_cambi_spatial_pooling` (quick-select top-K) on the host. That is nine to ten host waits and ten device-to-host copies per frame, with the GPU idle while the host works, and a `DIRECT` dispatch hint because the host residual serialised frames anyway.

Measured on this fork's two Intel devices with the twin selected by its registered name (`--feature cambi_sycl`), the hybrid cost 140 ms a frame on an Arc B580 and 944 ms a frame on a UHD 770 at 3840x2160, against 10.6 ms for the CPU extractor on 16 threads. The earlier baseline that circulated for this work (69.9 ms on the B580) measured the CPU extractor single-threaded: `--feature cambi` registers `cambi` by exact name whatever `--backend` says (see [the SYCL overview](../backends/sycl/overview.md#known-gaps)).

The user asked for the round trips to go: "there shouldnt be any gpu cpu rountrips" and "remove the host roundtrips and then test again". The score is pinned by the CPU extractor, which the Netflix golden gate protects, so the device version has to reproduce `cambi.c`'s integer counts and float c-values exactly and must not drift in the pooled mean.

## Decision

We will compute every CAMBI stage on the device and read back once per frame. `cambi_sycl` registers with the combined SYCL graph (`vmaf_sycl_graph_register`, dispatch hint `AUTO`), reads the distorted luma plane from the shared frame buffer the upload already fills, and enqueues, per frame: preprocessing (10-bit conversion, optional resize through init-time index tables, anti-dither), input validation, a local-memory-tiled spatial mask, and for each scale decimation, the horizontal and vertical mode filter (the vertical pass also writes a compact level map), per-row run/change bit masks, the c-values, and a top-K pool. `post_fn` copies one 88-byte block (five 128-bit per-scale sums and a status word) to pinned host memory; `collect()` turns the sums into the score with the CPU's own `vmaf_cambi_weight_scores_per_scale`.

The c-values keep `cambi.c`'s data structure. One work-item owns one column of the per-row-chunk level histogram and slides the window down its rows; rows whose leaving and entering segments agree are skipped, and a changed segment is applied one run of equal levels at a time. Updates are modular `uint16` and commute, and no window count exceeds 4225, so every cell holds the true window count whatever order the device applies them in. `cambi.c`'s init rejects any configuration whose adjusted window — at the encode resolution or the source resolution, after the high-res speed-up — has `window^2 >= CAMBI_RECIPROCAL_LUT_SIZE` (4226), with -EINVAL and "cambi: window_size %d too large for reciprocal LUT"; upstream Netflix has the same guard. `cambi_sycl` applies the identical check at the same point of its init (after the TVI tables), so the two accept and reject exactly the same configurations and the largest window either runs is 65 x 65. That also bounds every table index `p0 + pm` by 4225 and every product `w * p0 * pm` by 9 x 2112 x 2113, far inside `int`. The per-pixel formula is `c_value_pixel()`'s, float for float, multiplying by the same reciprocal table: `cambi.c` now exports it as `vmaf_cambi_reciprocal_lut()`, because 42 of its 4226 decimal literals parse one ulp away from `1.0f / i` and a twin that divided would drift.

Top-K pooling selects the k-th largest c-value T with a three-pass radix select over the IEEE bit patterns (monotonic for non-negative floats) and sums `sum(v > T) + (k - #(v > T)) * T` exactly as a 128-bit integer in units of 2^-24. Every non-zero c-value is at least 0.5 (`w * p0 * pm / (p0 + pm)` with counts of at least one) and below 2^14 (at most 9 x 4225 / 4), so it is an integer multiple of 2^-24 and the sum is exact. The top-k multiset is unique even with ties, so this is the sum of the same elements the CPU's quick-select keeps. The c-values kernel also counts radix pass 0 and a per-work-group sum; when the threshold lands in bin 0 (only exact zeros), the remaining passes return at once and that sum is the answer. No kernel uses fp64 ([ADR-0220](0220-sycl-fp64-fallback.md)).

The numerical contract is therefore: bit-identical to `cambi.c` whenever `cambi.c`'s own sequential double sum is exact, which is guaranteed while every scale's top-K sum stays below 2^29 and holds in practice well beyond it; otherwise the device value is the exactly rounded mean and the CPU carries its accumulation error, a few ulp of the score.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep the hybrid, batch the copies and overlap host work with the GPU | Smallest change; the c-values stay the literal CPU code | Still a host residual and a wait per scale; the host pass alone costs more than the whole device pipeline | The user asked for no round trips; the residual is the bottleneck |
| Direct per-pixel window scan (count 9 levels over the (2 pad + 1)^2 window) | Trivially parallel, no state | 4225 reads per pixel at 4K, about 47 G reads a frame | Too slow on either device |
| Per-level summed-area tables | O(1) query per level | One table per level (up to about 1000) per scale | Memory and work proportional to the level count |
| Column-strip histograms with a horizontal sweep (Perreault/Hébert) | O(1) amortised updates | Needs a device-wide barrier per row or duplicated halo strips, and 585 reads per query | More complex than the column-owned slide once runs and unchanged rows are skipped |
| Column-owned sliding histogram, 130 reads per column per row (first implementation) | Mirrors `cambi.c` directly | 29 ms a frame on the B580 and 191 ms on the UHD 770 at 4K | Superseded by the run/change-mask version of the same structure |
| Replay `cambi.c`'s quick-select on the device to reproduce its summation order | Would match the CPU even when its double sum rounds | Sequential over millions of elements; a device double sum is forbidden by ADR-0220 anyway | Slow and not allowed; the exact sum is the better-defined value |
| Full device sort for top-K | Simple selection | O(N log N) passes over 33 MB at 4K | Radix select needs one to three histogram passes and usually one |

## Consequences

- **Positive**: one upload (the shared frame) and one 88-byte readback per frame; the extractor overlaps with the other combined-queue extractors through the normal `submit`/`collect` double buffering. At 3840x2160 the B580 goes from 140 ms to 9.3 ms a frame and the UHD 770 from 944 ms to 42 ms; the default model on the UHD 770 from 138 ms to 20 ms at 576x324 and from 976 ms to 103 ms at 4K (measurements in [Research-2122](../research/2122-sycl-cambi-device-resident.md)). Scores are bit-identical to the CPU on every fixture where the CPU sum is exact, and the multi-frame parity test now asserts bit-exactness.
- **Negative**: the device keeps per-chunk column histograms (bounded at 64 MiB) and a float c-value plane; the kernel count per frame is about 65, which graph replay absorbs. On heavily banded 4K content the device and CPU scores can differ in the last few ulp (measured up to 6.2e-14), the CPU being the rounded one.
- **Neutral / follow-ups**: the CUDA, HIP and Metal twins keep the hybrid residual; each has an RC3 row in [`docs/state.md`](../state.md) that points at this design. Configurations with a window above 65 x 65 (for example DCI 4K or 8K at the default `window_size`, or `window_size` above 65 at 4K without the speed-up) fail init on the CPU and on SYCL alike; `test_sycl_cambi_parity` pins the boundary. The CUDA, HIP and Metal twins lack that guard today and would pass such a window to the host residual; their RC3 rows record it. The accessor added to `cambi_internal.h` is additive.

## References

- Source: `req` — "there shouldnt be any gpu cpu rountrips" / "remove the host roundtrips and then test again" (user, 2026-09-29).
- [ADR-0205](0205-cambi-gpu-feasibility.md), [ADR-0345](0345-cambi-gpu-port-strategy.md), [ADR-0360](0360-cambi-cuda.md), [ADR-0415](0415-cambi-sycl-port.md), [ADR-0458](0458-sycl-cambi-ssim-slm-staging.md), [ADR-0489](0489-cambi-sycl-event-chain.md) — the hybrid this replaces for the SYCL twin (it supersedes their SYCL host-residual provisions; CUDA, HIP and Metal keep them).
- [ADR-0214](0214-gpu-parity-ci-gate.md) — cross-backend tolerance contract; [ADR-0220](0220-sycl-fp64-fallback.md) — fp64-free SYCL kernels.
- [Research-0020](../research/0020-cambi-gpu-strategies.md) — Strategy III (device c-values), deferred there and implemented here.
- [Research-2122](../research/2122-sycl-cambi-device-resident.md) — the investigation, profiles and measurements behind this decision.
