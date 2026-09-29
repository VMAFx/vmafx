<!-- markdownlint-disable MD013 MD060 -->
# Research-2122: Device-resident CAMBI on SYCL — c-values and exact top-K pooling on the GPU

- **Status**: Active
- **Workstream**: [ADR-1357](../adr/1357-sycl-cambi-device-resident.md)
- **Last updated**: 2026-09-29

## Question

Can `cambi_sycl` run every stage on the device — including the sliding-histogram c-values and the top-K spatial pooling that [ADR-0205](../adr/0205-cambi-gpu-feasibility.md) kept on the host as "precision-sensitive" — without changing the score the CPU extractor defines, and how fast is it then on the fork's Intel devices?

## Sources

- `core/src/feature/cambi.c` (`calculate_c_values`, `c_value_pixel`, `quick_select`, `average_topk_elements`, `cambi_preprocessing`, `filter_mode`, `get_spatial_mask_for_index`) and `core/src/feature/cambi.h` (`update_histogram_*`, `uh_slide`, `reciprocal_lut`).
- [Research-0020](0020-cambi-gpu-strategies.md): Strategy III (fully on-device c-values), documented there and deferred.
- The previous SYCL twin (`integer_cambi_sycl.cpp` at `c894eb9d0`) and its ADRs ([ADR-0415](../adr/0415-cambi-sycl-port.md), [ADR-0458](../adr/0458-sycl-cambi-ssim-slm-staging.md), [ADR-0489](../adr/0489-cambi-sycl-event-chain.md)).
- The combined-graph machinery in `core/src/sycl/common.cpp` (`vmaf_sycl_graph_register`, `sycl_apply_input_barriers`, `record_combined_graphs`).

## Findings

### What the CPU score actually depends on

- **Window counts are unique.** Every histogram update in `calculate_c_values` is a modular `uint16` increment or decrement, and the updates for a row all land before that row's c-values are read. The final cell value is therefore the true count of that level in the clipped `(2 pad + 1)^2` window whatever order the updates run in, as long as no count reaches 65536. It never gets close: `cambi.c`'s init rejects any configuration whose adjusted window — at the encode resolution or the source resolution, after the high-res speed-up — has `window^2 >= CAMBI_RECIPROCAL_LUT_SIZE` (4226), with -EINVAL and "cambi: window_size %d too large for reciprocal LUT"; upstream Netflix has the same guard. `cambi_sycl` applies the identical check at the same point of its init (after the TVI tables), so the two accept and reject exactly the same configurations and the largest window either runs is 65 x 65, i.e. at most 4225 pixels. Any device algorithm that produces true window counts is bit-exact on the histogram.
- **The c-value formula is float-deterministic.** `(float)(w * p0 * pm) * reciprocal_lut[pm + p0]` is one int-to-float conversion and one multiply; with the counts equal and the same table, the device reproduces it bit for bit (`-fp-model=precise` keeps icpx from contracting anything else nearby).
- **The reciprocal table is not `1.0f / i`.** 42 of its 4226 literals (for example `reciprocal_lut[82] = 0.012195122f`) parse one ulp away from the correctly rounded reciprocal; they were printed to nine decimal digits from the double value. A twin that divided on the device would differ on every pixel that hits one of those entries. `cambi.c` now exports the table (`vmaf_cambi_reciprocal_lut()`), and `test_cambi` asserts both that the accessor returns it and that it differs from `1.0f / i`.
- **Every non-zero c-value lies in [0.5, 2^14).** `w * p0 * pm / (p0 + pm)` with `p0, pm >= 1` and `w >= 1` is at least `1/2`, reached only at `p0 = pm = w = 1` where the table entry `0.5` is exact; the next value is `2/3`. The maximum is `9 * window^2 / 4 <= 9 * 4225 / 4 < 2^14`, and `w * p0 * pm` stays below 2^26, far inside `int`. Every such float is an integer multiple of 2^-24, so a fixed-point sum in units of 2^-24 is exact and fits a 128-bit accumulator for any frame.
- **The CPU pooled sum is the inexact one.** `spatial_pooling` sums the top-k in double in quick-select order. That sum is exact while it stays below `2^53 * 2^-24 = 2^29` (and often well above that when the values are coarse). A scratch harness that runs `cambi.c`'s own stages on one frame and pools it both ways (quick-select + double sum, and sort + exact fixed-point sum) found the difference exactly where predicted: BBB 4K frame 5, scale 0, top-K sum 1.31e9 — CPU mean `262.27441095985182`, exact `262.27441095985131`; final CPU score `2.3467517467372354`, exact `2.3467517467372332`, which is what `cambi_sycl` returns on both devices. On a synthetic heavily banded 4K clip (scale-0 sum 3.95e9) the gap is 6.2e-14 on a score of 9.26.

### Device algorithm, and what each iteration measured

Per-stage wall clock with a queue wait after every stage (`VMAF_SYCL_DISPATCH=cambi_sycl:direct`, BBB 3840x2160, Arc B580 / UHD 770; the waits add launch latency to every row, so the totals are above the real graph-replay figures):

| Version | c-values (5 scales) | spatial mask | whole frame |
| --- | --- | --- | --- |
| v1: column-owned window histogram, 130 row-segment reads per column per row; 7x7 mask with 147 loads per pixel | 22.3 / 129.3 ms | 2.0 / 44.9 ms | 31.2 / 197.9 ms |
| v2: per-row *change* mask (skip rows whose leaving and entering segments agree) and *run* mask (apply a segment one run of equal levels at a time); separable mask; 32-bit indexing | 4.2 / 22.9 ms | 0.9 / 13.8 ms | 11.5 / 74.0 ms |
| v3: radix pass 0 and a per-group sum folded into the c-values kernel; later passes skip on device once the threshold resolves to 0 | 5.7 / 23.1 ms | 0.9 / 14.0 ms | 12.5 / 61.4 ms |
| v4 (shipped): one local-memory tile for the 7x7 mask; row masks through local memory; chunk tuning | 5.1 / 22.6 ms | 0.7 / 5.9 ms | 11.6 / 49.8 ms |

- **Chunking.** A work-item walks one column of one row chunk, so each chunk is a latency chain. `compute_units * 512` target work-items and at least 32 rows per chunk measured best on both devices (c-values s0 2.76 ms on the B580 against 4.86 ms at `* 32`); the per-chunk column histograms are capped at 64 MiB.
- **What did not help.** `[[sycl::reqd_sub_group_size(32)]]` on the row-mask kernel was slower on both devices; a work-group-wide `reduce_over_group` of 32 bits per word was slower on the B580 than one item per word (1.4 against 0.9 ms) but three times faster on the UHD 770, and the local-memory version beats both. 32-bit index arithmetic did not move the UHD 770 mask time (9.1 ms either way); the UHD 770's cost is dominated by 16-bit gathers, not 64-bit address math.
- **Where the UHD 770 time goes now.** c-values at scale 0 (about 15 ms of 53 ms): scattered 16-bit histogram reads and read-modify-writes, which Xe-LP's data port serves slowly. Everything else is a handful of image passes of 1-5 ms each.

### Before and after (wall clock, no per-stage waits)

ms/frame, `--no_prediction`, before = `c894eb9d0`, after = the shipped kernels. 4K rows: `t(22) - t(2)` over 20 frames of Big Buck Bunny 3840x2160, median of 5 runs (after: 9 runs). 576x324 rows: `t(48) - t(2)` over 46 frames of `src01`, median of 9 runs, because start-up noise swamps a 20-frame difference at a few ms a frame. CPU = `--backend cpu --threads 16 --feature cambi`; GPU = `--backend sycl --feature cambi_sycl`; default model = no `--model` and no `--feature`.

| Configuration | Before | After |
| --- | --- | --- |
| cambi, 4K, CPU (code unchanged) | 10.6 | 11.6 |
| cambi, 4K, Arc B580 | 140.0 | 9.3 |
| cambi, 4K, UHD 770 | 944.0 | 42.2 |
| cambi, 576x324, CPU | 0.2 | 0.0 (below resolution) |
| cambi, 576x324, Arc B580 | 6.7 | 1.8 |
| cambi, 576x324, UHD 770 | 147.4 | 4.2 |
| default model, 4K, Arc B580 | 122.5 | 71.3 |
| default model, 4K, UHD 770 | 976.3 | 102.8 |
| default model, 576x324, Arc B580 | 11.2 | 4.4 |
| default model, 576x324, UHD 770 | 137.7 | 19.7 |

A second after-run of the 4K rows (5 repetitions, before the final lint-only edits) gave 7.6 ms on the B580, 40.5 ms on the UHD 770 and 58.4 / 105.3 ms for the default model; the spread comes from reading two 12 MB frames per frame from the container volume.

### Parity evidence (per-frame `Cambi_feature_cambi_score`, `--precision max`)

- Netflix `src01_hrc01_576x324`, 48 frames: bit-exact on the B580 and the UHD 770, in graph and in direct dispatch.
- BBB 3840x2160, 50 frames: 47/50 bit-exact on both devices; frames 4, 5 and 6 differ by 6.7e-16, 2.2e-15 and 8.9e-16, the CPU's summation rounding (see above).
- Default model (`cambi` with `hrs=1080`, `vlt=0.06`, `cmxv=17`), BBB 4K, 20 frames: bit-exact on both devices.
- Option sweep on synthetic banded clips, both devices: 10-bit 4K, 12-bit 1080p, `enc_width`/`enc_height` resize (even and odd ratios, with `enc_bitdepth=8` anti-dither), `cambi_high_res_speedup=1080`, `topk=1.0`, `cambi_topk=0.001`, `max_log_contrast` 0 and 5, `eotf=pq`, `window_size` 15 and 127: all bit-exact. The 8-bit 4K clip (scores near 9.26) differs by up to 6.2e-14, again the CPU's summation.
- Out-of-range 10-bit samples: the device validation flags them and `collect()` fails the frame, as `cambi.c::validate_image` does.
- `cambi_sycl` sharing the combined queue with `psnr_sycl`, `motion_sycl` and `vif_sycl` (plus `ciede_sycl` on the primary queue) leaves every other feature byte-identical.

### Reproduce

Environment: the `vmaf-dev-mcp` image with oneAPI 2026.1, `meson setup build core -Denable_sycl=true -Denable_cuda=false -Dsycl_icpx_aot_targets=bmg-g21,adl-s --buildtype=release`, devices picked with `ONEAPI_DEVICE_SELECTOR=level_zero:0` (Arc B580) or `level_zero:1` (UHD 770). The Big Buck Bunny fixture is a raw 8-bit 4:2:0 3840x2160 decode, 200 frames.

```bash
# Per-frame parity: CPU reference, then the twin by its registered name.
vmaf -r ref.yuv -d dis.yuv -w 3840 -h 2160 -p 420 -b 8 --frame_cnt 50 \
    --backend cpu --threads 16 --no_prediction --feature cambi \
    --precision max --json -o cpu.json
vmaf -r ref.yuv -d dis.yuv -w 3840 -h 2160 -p 420 -b 8 --frame_cnt 50 \
    --backend sycl --no_prediction --feature cambi_sycl \
    --precision max --json -o sycl.json
python3 -c 'import json,sys; a,b=(json.load(open(f))["frames"] for f in sys.argv[1:]); print(max(abs(x["metrics"]["cambi"]-y["metrics"]["cambi"]) for x,y in zip(a,b)))' cpu.json sycl.json

# Timing: ms/frame = (t(22 frames) - t(2 frames)) / 20, median of five runs.
for n in 2 22; do
    /usr/bin/time -f "%e s for $n frames" vmaf -r ref.yuv -d dis.yuv -w 3840 -h 2160 \
        -p 420 -b 8 --frame_cnt "$n" --backend sycl --no_prediction \
        --feature cambi_sycl -o /dev/null --json -q
done
```

`--feature cambi` without the suffix runs the CPU extractor under any `--backend`; the default model (no `--model`) picks `cambi_sycl` by itself. The debug-only per-stage profile above comes from a build that waits on the queue after every stage and forces `VMAF_SYCL_DISPATCH=cambi_sycl:direct`; it is not part of the tree.

## Alternatives explored

- Direct per-pixel window scans, per-level summed-area tables and Perreault-style column strips — see the ADR's table; all cost more work or memory than the column-owned slide once unchanged rows and runs are skipped.
- Reproducing the CPU's quick-select order on the device to also match its rounding: a sequential pass over millions of elements, and the device may not use fp64 (ADR-0220). The exact sum is the better-defined value; the CPU is the side with error.

## Open questions

- The CUDA, HIP and Metal twins do not apply `cambi.c`'s window guard, so a window above 65 x 65 reaches their host residual and `c_value_pixel()` would index past the reciprocal table; recorded in their RC3 rows.
- The UHD 770 remains about four times slower than the 16-thread CPU at 4K. Packing the level histogram or processing two columns per work-item to avoid 16-bit gathers are the next candidates.

## Related

- [ADR-1357](../adr/1357-sycl-cambi-device-resident.md), [ADR-0214](../adr/0214-gpu-parity-ci-gate.md), [ADR-0220](../adr/0220-sycl-fp64-fallback.md).
- `docs/state.md` rows `T-CUDA-CAMBI-HOST-RESIDUAL-2026-09-29`, `T-HIP-CAMBI-HOST-RESIDUAL-2026-09-29`, `T-METAL-CAMBI-HOST-RESIDUAL-2026-09-29`.
