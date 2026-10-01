<!-- markdownlint-disable MD013 MD060 -->
# Research-1424: integer_ssim_cuda and the CPU's frame sum — what an exact sum costs, and how it could be made parallel

- **Status**: Active
- **Workstream**: [ADR-1424](../adr/1424-cuda-ssim-cpu-frame-sum.md), [ADR-1400](../adr/1400-hip-integer-ssim-raster-sum-small-frames.md), [ADR-0214](../adr/0214-gpu-parity-ci-gate.md)
- **Last updated**: 2026-10-01

## Question

`integer_ssim_cuda` computes the CPU's int64 moments and the CPU's double
term per pixel, and differs from the CPU `ssim` only in the order of the
frame sum. How large is that, what does the CPU's order cost on the device,
and is there an exact form that keeps the sum on the device?

## Sources

- CPU: `core/src/feature/integer_ssim.c` (`calc_ssim()`,
  `ssim_reduce_row_range()`).
- CUDA: `core/src/feature/cuda/ssim_cuda.c` and
  `integer_ssim/integer_ssim_score.cu` at master `5c8b9e9c7` (before; the
  same files at `855af3c7a`) and on `fix/cuda-ssim-cpu-arithmetic` (after).
- Host `zeus`: RTX 4090, CUDA 13.4, gcc 16.2.1, Ryzen 9 9950X3D. `meson setup
  build-cuda core -Denable_cuda=true -Denable_sycl=false --buildtype=release
  -Db_lto=false`.
- Fixtures, 4:2:0, `--precision max`: the Netflix pair at 8 bits (48 frames)
  and at 10, 12 and 16 bits (3 frames each), both 1920x1080 checkerboard
  pairs (3 frames each), the first 50 frames of BBB 3840x2160.

## Findings

### 1. The sum is the only difference

`ssim_reduce_row_range()` adds one term per pixel into `*ssim`:

```c
*ssim += m.w * (2 * mxy + c1) * (c2 + 2 * (m.xy * w_d - mxy)) /
         ((mx2 + my2 + c1) * (m.x2 * w_d - mx2 + m.y2 * w_d - my2 + c2));
```

`calc_ssim()` calls it row after row with the same accumulator: one double
through the whole frame. The kernel's `issim_term()` is that expression
(built without contraction, ADR-1403), and its constants `0.0001` and
`0.0009` are the doubles that `0.01 * 0.01` and `0.03 * 0.03` evaluate to.
Summing the kernel's own terms in raster order gives the CPU's score on every
frame, so nothing else differs.

| Fixture | Identical before | Largest difference | In dB (`enable_db`) |
|---|---:|---:|---:|
| Netflix 576x324, 8 bit | 0 / 48 | 2.3e-14 | 7.3e-13 |
| Checkerboard 1 px | 0 / 3 | 1.6e-12 | 1.9e-11 |
| Checkerboard 10 px | 0 / 3 | 1.1e-11 | 3.0e-11 |
| BBB 3840x2160 | 0 / 50 | 5.6e-13 | 3.6e-10 |
| Netflix 10, 12, 16 bit | 0 / 3 each | 1.8e-14 | 6.9e-13 (10 bit) |

After: 113 of 113 identical, 107 of 107 with `enable_db` and with
`enable_db:clip_db`, and identical on random pairs of 1x1, 2x2, 4x3, 7x5,
9x9, 16x16, 17x33, 63x65 and 322x182.

### 2. What the CPU's order costs

A run of the twin alone at 3840x2160, `(t(52) - t(2)) / 50`, seven
alternating pairs against master `5c8b9e9c7`, host load average 20: 2.18 ms
per frame before, 9.72 ms after, paired difference +7.61 ms (quartiles +7.08
to +8.99). Per frame the twin now reads back 8 294 400 doubles (66 MB) and
adds them one after the other. The adds alone take 3.2 ms on this host (a
loop over a plane of that size, best of nine, timed on its own), which
leaves about 4.4 ms for the read-back. At 576x324 (186 624 terms) the paired
difference is +0.02 ms.

### 3. An exact sum that stays on the device (not built)

A sequential double sum is not associative, but most of it is integer
arithmetic in disguise. While the running sum `S` stays in one binade
`[2^k, 2^(k+1))` it is a multiple of `u = 2^(k-52)`, and `fl(S + t)` is `S`
plus `t` rounded to a multiple of `u`. Sums of multiples of `u` are exact and
associative, so they can be formed per row in parallel. What a row thread
would return for a given `k`:

- the sum of the rounded terms, as an integer count of `u`;
- that sum for both parities of the starting count, because a term exactly
  halfway between two multiples rounds to the even neighbour of `S + t`,
  which depends on the parity of the running count;
- the smallest and largest prefix of that integer sum.

The host then walks the rows with the exact `S`: if `S` is in binade `k` and
`S / u` plus the row's smallest and largest prefix stay inside
`[2^52 + 1, 2^53 - 1]`, every step of the row stayed in the binade and the
row's result is `S` plus the integer sum for the parity of `S / u`.
Otherwise the row crossed a binade, or the unit was guessed for the wrong
binade, and the host adds that row's terms one by one.

The unit has to be guessed before `S` is known: a first pass can add each
row on its own and a prefix over the rows gives `S` at each row start to
about 1e-13, which names the binade except next to a power of two. At
3840x2160 the sum runs from about 2^16 to 2^39, so about 23 crossings fall
into about a dozen rows (the first row alone crosses twelve binades). Those
rows need their 3840 terms on the host, either read back on demand in
`collect()` or copied into a small buffer by a compaction pass.

This would return the read-back to a few values per row and the host loop to
one step per row. It is three device passes instead of one and has a
fallback path that must be tested as hard as the fast one, which is why
ADR-1424 takes the plain raster sum first.

## Reproduce

```bash
ninja -C build-cuda
build-cuda/test/test_cuda_ssim_parity            # needs a CUDA device
python3 core/test/test_cuda_ssim_exact_contract.py
python3 scripts/ci/cross_backend_parity_gate.py \
    --vmaf-binary build-cuda/tools/vmaf \
    --reference testdata/bbb/ref_3840x2160_200f.yuv \
    --distorted testdata/bbb/dis_3840x2160_200f.yuv \
    --width 3840 --height 2160 --features ssim --backends cpu cuda
```
