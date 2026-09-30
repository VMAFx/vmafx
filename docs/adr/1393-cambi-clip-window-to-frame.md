<!-- markdownlint-disable MD013 MD060 -->
# ADR-1393: CAMBI's c-values walks clip the window to the frame at every edge

- **Status**: Accepted
- **Date**: 2026-09-30
- **Deciders**: lusoris
- **Tags**: cambi, correctness, simd, numerics, fork-local

## Context

CAMBI counts, for every pixel, the levels inside a square window of `2 * pad_size + 1` pixels, at five scales that halve the frame each time. The window already stops at the frame edge wherever the frame is large enough: the walks in `calculate_c_values()` (`core/src/feature/cambi.c`), its upstream-mirror AVX2 copy `calculate_c_values_avx2()` (`x86/cambi_avx2.c`) and `cambi_calculate_c_values_frame()` (`cambi_c_values_frame.h`, run by the AVX2 scan, AVX-512 and NEON drivers) clip the histogram ranges at the left and right edges and handle the top and bottom rows in separate phases. Two of their loops assumed the frame was large enough:

- **Rows.** The first pass ran `pad_size` rows, the top edge `pad_size + 1` rows, and the bottom edge started at `height - pad_size`, whatever the height. At the coarsest scale a wide, short frame has at most `pad_size` rows; with the default `window_size` that is every height up to 176 at 1920 wide, 240 at 2560 wide and 352 at 3840 wide. The walks then read and wrote rows outside `c_values`, the image and the mask. ASan reports heap-buffer-overflows, and a release build aborts or segfaults (1920x64 and 1920x160 crash at the default dispatch). Netflix/vmaf#1628 reports it; Netflix/vmaf#1629 (open) clips the three loops, and #1628 names rejecting such frames in `init()` as the alternative.
- **Columns.** The scalar and AVX2-mirror walks ran their first column loop for `pad_size` columns whatever the width. On a frame with fewer than `pad_size` columns at some scale (widths up to 80 at 1080 high, 160 at 1920 high) they counted columns past the frame. `decimate()` shrinks each scale in place, so those columns still hold the previous scale's pixels, inside the stride: no sanitizer sees the read, but the score depends on it. The shared SIMD walk visits only columns below `width`, so `--cpumask 63` and the default dispatch disagreed (a 64x1920 vertical ramp: 14.975701 against 14.964394), and so did the CUDA, HIP and Metal twins, which run the scalar walk on the host through `vmaf_cambi_calculate_c_values()`. Upstream has the same scalar loops.

The SYCL twin computes c-values on the device (`cvals_prime()` / `cvals_column()` in `core/src/feature/sycl/integer_cambi_sycl.cpp`) and already clamps every window to rows `[0, height - 1]` and columns `[0, width - 1]`.

## Decision

Every c-values walk clips the window to the frame on all four edges. The scalar, AVX2-mirror and shared SIMD walks bound the first pass to `MIN(pad_size, height)` rows, the top edge to `MIN(pad_size + 1, height)` rows and start the bottom edge at `MAX(height - pad_size, 0)` (Netflix/vmaf#1629), and the scalar and AVX2-mirror walks bound their first column loops to `MIN(pad_size, width)`. Frames the old loops handled score exactly as before; frames that crashed now score; frames that completed while reading outside the frame score the clipped window, on every dispatch level and every twin. The fork keeps this whatever upstream decides.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Clip rows and columns in every walk (chosen) | Every geometry `init()` accepts is scored, with the same window rule CAMBI applies at every frame edge; all dispatch levels and all twins agree | Scores change for short frames that completed while reading outside the frame and for narrow frames on the C path, so the fork's C path differs from upstream's on narrow frames until upstream bounds its column loops | — |
| Reject frames that are too short (or too narrow) at the coarsest scale in `init()`, the alternative named in Netflix/vmaf#1628 | One check, no walk changes | Refuses inputs CAMBI can score: banners, letterbox strips and slices up to 176 rows at 1920 wide (352 at 3840), tall crops up to 160 wide at 1920 high; a model run that includes CAMBI fails on them | The window already clips at the frame edge; rejection would remove valid inputs to avoid applying that same rule at the corner |
| Port only upstream's row bounds and leave the column loops | Smallest diff, identical to upstream #1629 | The C path keeps counting pixels of another scale on narrow frames, and the host-walk GPU twins keep disagreeing with the CPU's default dispatch | Keeps a known wrong score |
| Make the SIMD walks read the extra columns too | C-path scores of narrow frames, and upstream's, stay unchanged | Reproduces reading pixels left over from another scale; the score then depends on decimation leftovers | Matching a defect is not parity |

## Consequences

- **Positive**: no out-of-bounds access on any frame size `init()` accepts; the scalar, AVX2 mirror, AVX2 scan, AVX-512 and NEON walks give bit-identical c-values on short and narrow frames; the CUDA, HIP and Metal twins (host walk) and the SYCL twin (device walk) compute the same window.
- **Negative**: scores change for the affected sizes (listed in [the CAMBI frame-size section](../metrics/cambi.md#frame-sizes) and measured in [Research-2132](../research/2132-cambi-short-and-narrow-frames.md)). The Netflix golden pairs (576x324, 1920x1080) are not affected.
- **Neutral / follow-ups**: `test_calculate_c_values_short_frame` and `test_calculate_c_values_narrow_frame` (`core/test/test_cambi.c`) pin both bounds on every driver the host has. An upstream sync that re-imports `calculate_c_values()` keeps both bounds (`docs/rebase-notes.md`, `core/src/feature/AGENTS.md`). The device c-values kernels of the CUDA and HIP twins in flight (PR #1639 and the HIP follow-up) need the same short- and narrow-frame check.

## References

- [Netflix/vmaf#1628](https://github.com/Netflix/vmaf/issues/1628) (the report and the reject-in-`init()` alternative) and [Netflix/vmaf#1629](https://github.com/Netflix/vmaf/pull/1629) (the row bounds).
- RC3 handoff task brief (2026-09-30, from the workflow orchestrator, not a direct user quote): "Bugs you find on the way are to be FIXED, not just recorded, even when they predate your change".
- Adversarial review of PR #1642 (2026-09-30), finding 7: the decision between clipping and rejecting needs an in-tree record, because upstream may still choose rejection.
- [Research-2132](../research/2132-cambi-short-and-narrow-frames.md) (measurements and input generators).
