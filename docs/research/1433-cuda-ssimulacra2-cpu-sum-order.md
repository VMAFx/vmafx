<!-- markdownlint-disable MD013 MD060 -->
# Research-1433: Forming the CPU's sequential double sums on a GPU, bit for bit

- **Status**: Active
- **Workstream**: [ADR-1433](../adr/1433-cuda-ssimulacra2-cpu-sum-order.md), [ADR-1391](../adr/1391-cuda-ssimulacra2-device-resident.md), [ADR-1424](../adr/1424-cuda-ssim-cpu-frame-sum.md)
- **Last updated**: 2026-10-01

## Question

`ssimulacra2_cuda` computes the CPU's per-pixel terms and adds them in a
tree, which leaves it up to 7.3e-11 from the CPU. Can the device return the
bits of the CPU's loop, `for (i = 0; i < n; i++) s += x[i];`, without
reading 600 MB of terms back per 4K frame and without adding 8.3 million
terms on one thread? What does it cost?

## Sources

- CPU: `core/src/feature/ssimulacra2.c::ssim_map()` and `::edge_diff_map()`
  (six running doubles per channel). The comparison ran against the AVX-512
  path this host selects, which accumulates per pixel in the same order as
  the scalar functions (`x86/ssimulacra2_avx512.c`, ADR-1208).
- CUDA: `core/src/feature/cuda/ssimulacra2_cuda.c`,
  `ssimulacra2/ssimulacra2_device.cu` at master `5c8b9e9c7` (before) and on
  `fix/cuda-ssimulacra2-cpu-sum-order` (after).
- Reference workstation: RTX 4090, CUDA 13.4, gcc 16.2.1, glibc 2.44, Ryzen 9 9950X3D.
  `meson setup build-cuda core -Denable_cuda=true -Denable_sycl=false
  --buildtype=release -Db_lto=false`.
- Fixtures: Netflix 576x324 at 8 bits (48 frames) and 10, 12, 16 bits (3
  each), both 1080p checkerboard pairs (3 each), BBB 3840x2160 (50 frames;
  200 through the gate).

## Findings

1. **The whole difference was the order.** Before: 8 of 113 frames
   identical; largest difference 1.3e-13 on the Netflix pair, 3.4e-13 and
   7.3e-11 on the 1 px and 10 px checkerboards, 1.5e-12 at 3840x2160. After:
   113 of 113, and 0 on 200 BBB frames through the gate. No term changed.
   Random frames at 8x8, 9x17, 33x31, 577x323 and 1921x1081 (4:2:0, 8 bit),
   64x48 (4:2:2, 10 bit) and 65x47 (4:4:4, 12 bit), two frames each, are
   identical as well, and so is the Netflix pair with `yuv_matrix` 1, 2
   and 3.

2. **Inside a binade the loop is an integer sum.** A double in
   `[2^e, 2^(e+1))` is `m * u` with `u = 2^(e-52)` and `m` in
   `[2^52, 2^53)`. For `x >= 0`, `s + x` rounded to nearest is
   `(m + r) * u`, where `r` is `x / u` rounded to an integer, as long as the
   result is at most `2^(e+1)`. So a run of adds that stays in the binade is
   `m + r_1 + r_2 + ...` in integers, and integer adds can be grouped at
   will. `r` comes from the term's own bits: its 53-bit mantissa shifted
   right by the difference of the two exponents, rounded on the bits shifted
   out. No floating-point operation is involved.

3. **Ties need the parity of the running integer.** When `x / u` is exactly
   half way, the add rounds to the even result, so `r` is `floor` or
   `floor + 1` depending on whether `m + floor` is even. A term is therefore
   a pair of increments (for `m` even, for `m` odd), and so is a run of
   terms; two runs compose by picking the second run's increment for the
   parity the first run leaves. The composition is associative, not
   commutative, so runs must be composed in order. ssimulacra2 needs this:
   its term `d = 1 - q` is a multiple of 2^-53 when `q` is in `[0.5, 1)`,
   and every second such term ties while the sum is in `[1, 2)`.

4. **Where the sum leaves its binade, the terms are added one by one.** The
   walk knows the exact sum before each chunk. It adds the chunk's increment
   only if the sum is in the binade the increment was computed for and the
   result does not pass `2^(e+1)`; the terms are non-negative, so the result
   bounds every intermediate value. Otherwise the 1024 terms of that chunk
   are added in a plain loop. On BBB 3840x2160 that happens for 9 to 27 of
   the 8100 chunks of a sum at scale 0 (the first chunk with a non-zero
   term, and one per binade the sum crosses), 3 to 6 of 8 at scale 5.

5. **The plan cannot make the result wrong.** The binade of each chunk comes
   from the prefix of tree sums of the chunks, an approximation. The walk
   checks it against the exact sum, so a wrong plan costs a term-by-term
   chunk. Checked on the host with plans that are one binade too high on
   every chunk, "all zero" and one fixed binade (`test_ordered_sum`), and on
   the device by planning every chunk as term-by-term: 51 of 51 frames still
   identical, at 320 ms per 4K frame.

6. **Layout on the device.** A chunk is one block of 256 lanes, each lane
   taking 4 consecutive pixels. `ssimulacra2_chunk_sums` adds each chunk's
   terms in a tree (the plan's input). `ssimulacra2_chunk_plan` follows the
   prefix of those sums. `ssimulacra2_chunk_units` composes each lane's four
   terms in order and then the lanes in an ordered tree (lane `i` takes lane
   `i + step`). `ssimulacra2_ordered_totals` walks the chunks on lane 0,
   1024 staged in shared memory at a time; for a term-by-term chunk every
   lane computes its four terms into shared memory and lane 0 adds the 1024
   in order. `compute-sanitizer` reports 0 errors with `memcheck` and 0
   hazards with `racecheck` on `test_cuda_ssimulacra2_parity`.

7. **Cost.** A run of the twin alone, `(t(52) - t(2)) / 50` on BBB
   3840x2160, seven alternating pairs against master `5c8b9e9c7`, host load
   average 10 to 13: 7.83 ms before, 15.63 ms after, paired difference
   +7.81 ms (quartiles +7.76 to +8.14). Netflix 576x324, `(t(48) - t(2)) /
   46`: 0.41 and 1.73 ms, paired +1.34 (+1.13 to +1.42). The CPU extractor
   takes 510 ms per 4K frame on one thread and 126 ms on sixteen.

   Where the time goes at 3840x2160, from launching one kernel five times
   per scale instead of once (the kernels are idempotent) and dividing the
   added time by four:

   | Kernel | ms per frame |
   |---|---:|
   | `ssimulacra2_chunk_sums` | 3.0 |
   | `ssimulacra2_chunk_plan` | 1.4 |
   | `ssimulacra2_chunk_units` | 2.5 |
   | `ssimulacra2_ordered_totals` | 4.2 |
   | the tree they replace (Research-1391) | 2.8 |

   The two kernels with a sequential part are the expensive ones for what
   they do: one GPU lane steps through 8100 chunks at scale 0, and a
   term-by-term round costs two block barriers and 1024 dependent fp64 adds.
   Staging the chunks through shared memory took the frame from 18.9 to
   15.6 ms.

8. **Tests.** `test_ordered_sum` (host): the one-term rule against the
   floating-point add on 1.9 million samples, and the whole pipeline in the
   kernels' layout against the loop on ten kinds of input and four kinds of
   plan. Four mutations of the header (always round a tie up, drop the
   binade check, drop the upper bound check, ignore the parity when
   composing) each fail it. `test_cuda_ssimulacra2_parity` asserts `==` on
   three fixtures at two sizes and fails on the old twin (2.8e-14 and
   5.7e-14 on its fixtures).

## Open questions

- Throughput (`T-CUDA-SSIMULACRA2-EXACT-THROUGHPUT-2026-10-01`). Candidates,
  none built: keep each scale's plan from the previous frame and compute the
  increments under it while the same pass forms the tree sums, then redo
  only the chunks whose plan changed (removes one of the two fp64 passes on
  video); compute the plan's prefix with a scan across the lanes instead of
  on one lane; compose runs of chunks that share a binade so that the walk
  steps over 64 or 1024 chunks at a time.
- `ssimulacra2_sycl` and `ssimulacra2_hip` evaluate the terms as fp32 pairs
  (relative error about 2^-44), which is not the CPU's double, so the
  ordered sum alone would not make them exact
  (`T-GPU-SSIMULACRA2-SUM-ORDER-2026-10-01`).
