<!-- markdownlint-disable MD013 -->

# Research digest 2030 — CPU hot path under the default model: SpEED `matrix_mul` and CAMBI

- **Date**: 2026-09-06
- **Author**: Lusoris
- **Scope**: epic #1245 item 3 — profile the CPU (and, if the tooling exists, GPU) hot paths of the default model and separate the wins that can be proven bit-identical from the ones that cannot.
- **Related**: [ADR-1196](../adr/1196-speed-matmul-simd-dispatch.md)

## 1. What was measured, and on what

| Item | Value |
| --- | --- |
| Host | `ryzen-4090-arc`, 32 logical cores, AVX-512 present |
| Branch point | `cd52f2670` (`origin/master` at the time the worktree was cut; `ce7ff4f6f` adds only docs + a shell gate, no C) |
| Build | `meson setup core --buildtype release -Db_ndebug=true -Dc_args='-g -fno-omit-frame-pointer'`, CUDA/SYCL/HIP off |
| Profiler | `perf` 7.2.3-1, `perf record -F 4999 -g --call-graph=fp` |
| Fixture | the Netflix src01 pair, `python/test/resource/yuv/src01_hrc00_576x324.yuv` vs `src01_hrc01_576x324.yuv`, 576x324, 48 frames |
| Profiling fixture | the same pair concatenated 30x = 1440 frames, so a single run samples for ~2.5 s instead of ~0.09 s |
| Model | the **default**, `vmaf_v1.0.16_3d0h` (no `--model` flag); `model/vmaf_v0.6.1.json` used as the no-SpEED control |
| Threads | none passed (`--threads` aborts on GPU backends per `T-GPU-CLI-THREADS-CTX-SYNC-2026-09-06`; the CPU path is single-threaded here for a stable profile) |

Every number below came from a command run on this host during this session.
A container rebuild and a sibling agent's `clang-tidy` + link jobs were
competing for the machine throughout, so each timing states the 1-minute load
average it ran under, and each is a median of at least 7 interleaved
repetitions after a discarded warm-up.

### GPU profilers are not installed on this host

```console
$ which perf ncu nsys vtune rocprof
/usr/bin/perf
ncu not found
nsys not found
vtune not found
rocprof not found
$ find / -name "ncu" -o -name "nsys" 2>/dev/null | head
$
```

`/opt/cuda/bin` ships `nvcc`, `cuda-gdb`, `compute-sanitizer` and
`cuobjdump`, but no Nsight Compute and no Nsight Systems. `valgrind` is
absent too. **No CUDA, SYCL or HIP kernel profile was collected** — the
`profile-hotpath` skill's documented degradation path (`perf` fallback plus a
clear note) applies, and `perf` cannot see inside a GPU kernel. A GPU profile
would in any case have been blocked by
`T-GPU-MOTION-FLUSH-DOUBLE-EMIT-2026-09-06`: on this commit no GPU backend
completes a scored run longer than one motion batch.

## 2. Baseline CPU profile, default model

`perf record -F 4999 -g --call-graph=fp -o perf_default.data -- ./build-prof/tools/vmaf -r ref1440.yuv -d dis1440.yuv -w 576 -h 324 -p 420 -b 8 --no_cuda --no_sycl --no_hip --json -o p1440.json`
— 13 289 samples, 0 lost, load average 20.47 at start / 19.63 at end.

Self-time, `perf report --no-children -g none`:

| # | Symbol | Self | Note |
| --- | --- | --- | --- |
| 1 | `preprocess_and_extract_cambi` | 23.52 % | CAMBI; `quick_select_partition` 10.22 %, `anti_dithering_filter` 4.98 %, `average_topk_elements` 3.70 % (inlined children) |
| 2 | `matrix_mul` | 20.78 % | SpEED QR / linear solve, `speed.c` |
| 3 | `calculate_c_values_avx2` | 9.09 % | CAMBI, already SIMD |
| 4 | `convolution_f32_avx_s` | 5.88 % | |
| 5 | `est_params` | 4.97 % | SpEED, excluding the `matrix_mul` it calls |
| 6 | `calculate_c_values_row_avx2.constprop.0` | 3.91 % | |
| 7 | `adm_decouple_avx512` | 3.25 % | |
| 8 | `motion_score_pipeline_8_avx512` | 2.99 % | |
| 9 | `adm_cm_avx512` | 2.91 % | |
| 10 | `adm_decouple_s123_avx512` | 2.77 % | |

Source-line attribution (`perf report -s sym,srcline`) puts 19.55 of
`matrix_mul`'s 20.78 % on three lines — `speed.c:188`, `:189`, `:190`, the
i-k-j triple loop — and 0.81 % on `speed.c:156`, `matrix_zero`.

Two observations decided the rest of the work.

**`matrix_mul` was running on 128-bit vectors on an AVX-512 machine.**
`perf annotate -s matrix_mul` shows the inner loop as `movups`/`mulps`/
`addps`/`movups` on `%xmm`, wrapped in `setae` alias-check versioning, with a
scalar `mulss`/`addss` remainder path taking a further ~8 % of the function.
That is the x86-64 baseline codegen: `speed.c` lives in the generic C library,
which is compiled without `-mavx2` / `-mavx512f`, and only the files under
`core/src/feature/x86/` get the wide flags.

**The callers explain the shape.** `perf report -S matrix_mul -g` attributes
essentially all of it to `matrix_qr_decomposition` (11.03 % via
`extract_chroma` → `speed_extract_score` → `est_params` for one chroma
channel, 9.53 % for the other), not to the final rectangular solve. With
`SPEED_INTERNAL_BLOCK_SIZE = 5` the covariance system is 25 x 25, so the QR
does 24 iterations x 2 products x 25³ ≈ 750 k multiply-adds per call, against
25 x 25 x `num_blocks` ≈ 280 k for the single `Q^T B` product. The inner `j`
loop is therefore 25 elements wide: six SSE steps plus one scalar element, and
a store-to-load round trip on `dst[i][*]` for every one of the 25 `k` steps.

## 3. The one win that is provably bit-identical

`dst[i][j] += x[i][k] * y[k][j]`: `j` is an **output** index. Widening it
changes how many independent output elements an instruction updates; it cannot
change the order in which one output element accumulates over `k`. So a wider
kernel is bit-identical as long as (a) it keeps the `k` order and (b) nothing
fuses the multiply and the add into an FMA, which would round once where the
reference rounds twice.

That was checked before any production code was touched, with a standalone
microbenchmark (`gcc -O3` versus `gcc -O3 -ffp-contract=off`, same source,
`memcmp` on the two result buffers):

```text
# gcc -O3                       (contraction allowed)
N=25 C=25  reps=200000  bitexact=NO   scalar=0.5747s avx512=0.1092s speedup=5.26x
N=25 C=448 reps=20000   bitexact=NO   scalar=0.3057s avx512=0.0727s speedup=4.20x

# gcc -O3 -ffp-contract=off
N=25 C=25  reps=200000  bitexact=YES  scalar=0.5513s avx512=0.0929s speedup=5.93x
N=25 C=448 reps=20000   bitexact=YES  scalar=0.3116s avx512=0.0741s speedup=4.21x
```

`bitexact=NO` under default flags is the whole risk in one line: GCC contracts
even explicitly-written `_mm512_add_ps(a, _mm512_mul_ps(...))` pairs into
`VFMADD` unless told not to. The tree already knows this — `x86_ssim_avx2`,
`x86_psnr_hvs_avx2`, `x86_ms_ssim_decimate_avx2`, `x86_float_adm_avx2` and the
two `ssimulacra2` libraries are all `-ffp-contract=off` carve-outs for exactly
this reason — so the two new kernels join that list rather than inventing a
mechanism.

Implementation, per [ADR-1196](../adr/1196-speed-matmul-simd-dispatch.md):
`speed_matmul_scalar` (exported from `speed.c`) plus `speed_matmul_avx2` and
`speed_matmul_avx512`, runtime-selected from `vmaf_get_cpu_flags()` in
`speed_dispatch_cpu_kernel()` alongside the existing `compute_cov_kernel_*`
family. Both SIMD kernels hold the destination row in vector registers for the
whole `k` loop, which removes the per-`k` store/load; AVX-512 finishes the
25-column case in one 16-lane step plus one 9-lane masked step.

### 3.1 Bit-exactness evidence

`--precision=max` (`%.17g`) JSON, unmodified binary versus patched binary,
same fixture, on every dispatch path (`--cpumask` bits are the flags to
*disable*: 56 = AVX2+AVX512+AVX512ICL off → scalar, 48 → AVX2, 0 → AVX-512):

```text
cpumask=0  (AVX-512): BIT-IDENTICAL   pooled=82.81605876127689
cpumask=48 (AVX2)   : BIT-IDENTICAL   pooled=82.81605876127689
cpumask=56 (scalar) : BIT-IDENTICAL   pooled=82.81605876127689
```

The only two lines that differ between the baseline and patched JSON in any of
those runs are `"version"` (different commit) and `"fps"` (the point of the
exercise). The v0.6.1 control is bit-identical too
(`76.66783086300072` before and after), as it must be — that model has no
SpEED feature, so `matrix_mul` is never reached.

`core/test/test_speed_simd.c` gains eight `memcmp`-exact cases, four per ISA,
covering the native 25x25x25 QR shape, the 25x25x448 rectangular solve, a
`cols=41` shape that exercises every tail branch, and a `cols=3, inner=1`
degenerate shape:

```text
test_matmul_avx2_speed_native: pass    test_matmul_avx512_speed_native: pass
test_matmul_avx2_rect:         pass    test_matmul_avx512_rect:         pass
test_matmul_avx2_tails:        pass    test_matmul_avx512_tails:        pass
test_matmul_avx2_narrow:       pass    test_matmul_avx512_narrow:       pass
16 tests run, 16 passed
```

### 3.2 Speedup

`os.wait4` `ru_utime + ru_stime` of the child process, base and patched
binaries interleaved run-for-run so any drift in machine load hits both sides
equally.

Final round, taken on the merged tree after the last edit, with the machine
close to idle:

| Fixture | Model | Reps | Load (1 min) | Base median | New median | Speedup |
| --- | --- | --- | --- | --- | --- | --- |
| src01 pair, 48 f | default `v1.0.16_3d0h` | 11 | 4.10 | 0.0829 s (spread 4.7 %) | 0.0689 s (spread 1.8 %) | **1.202x** |
| src01 pair x30, 1440 f | default `v1.0.16_3d0h` | 9 | 2.81 | 2.4820 s (spread 4.3 %) | 2.0674 s (spread 5.0 %) | **1.201x** |
| src01 pair x30, 1440 f | `vmaf_v0.6.1.json` | 9 | 2.25 | 2.1293 s (spread 8.3 %) | 2.1212 s (spread 8.1 %) | 1.004x — no change |

Corroborating rounds taken earlier while a container rebuild and a sibling
agent's `clang-tidy` + link jobs were competing for the machine: 1.216x /
1.202x (load 5.2–5.7), 1.188x (load 15.4), 1.134x (load 15.2) on the default
model. The direction is stable across every round; the magnitude compresses
under contention, which is why the idle round above is the one to quote. The
v0.6.1 control was run four times in total and returned 1.004x, 0.992x, 0.977x
and 0.938x — the two low figures came from rounds whose spread was 24–50 % and
whose *minima* were 2.2074 s base against 2.1931 s new, i.e. the patched side
was marginally faster at its best. **Every v0.6.1 number is inside the noise of
no change**, which is the expected result and is the reason the control exists:
that model carries no SpEED feature, so these kernels are never reached.

### 3.3 Profile after the change

Same command, patched binary, 10 522 samples, load average 41.30 (a sibling
agent's build was running; sample *shares* are unaffected by load, only the
wall time is):

| # | Symbol | Self, before | Self, after |
| --- | --- | --- | --- |
| 1 | `preprocess_and_extract_cambi` | 23.52 % | **28.17 %** |
| 2 | `calculate_c_values_avx2` | 9.09 % | 11.12 % |
| 3 | `convolution_f32_avx_s` | 5.88 % | 6.66 % |
| 4 | `speed_matmul_avx512` (was `matrix_mul`) | 20.78 % | **5.67 %** |
| 5 | `est_params` | 4.97 % | 5.48 % |

`matrix_mul` 20.78 % → `speed_matmul_avx512` 5.67 % is a 15.1-point reduction
in total samples, which predicts `1 / (1 − 0.151) = 1.18x`; the measured
1.20x agrees.

## 4. Wins that were found and deliberately **not** landed

The brief for this pass was bit-identical wins only. Each of these is real,
and each changes at least one score bit, so each is written down here instead
of being implemented.

### 4.1 Householder rank-1 update instead of a full matrix product — the big one

`matrix_qr_decomposition` builds the reflector explicitly as
`tmp_q = I − 2vvᵀ` (`matrix_identity_minus_v_vt`) and then does two full
25x25x25 products with it. The textbook form applies the reflector as a
rank-1 update, `Q ← Q − 2v(vᵀQ)`, which is O(n²) per iteration instead of
O(n³) — a factor of ~25 on this matrix size, against the ~4x a vector-width
change buys. Even after the SIMD win, `speed_matmul_avx512` plus `est_params`
is still 11 % of the run, so this is the largest single remaining CPU item in
SpEED.

Why it is not in this PR: the summation order changes completely, so scores
move. It needs its own PR with a snapshot-regeneration justification and a
cross-backend re-parity pass against the CUDA / SYCL / HIP twins, whose host
side runs the ADR-0964 duplicate of the same algorithm in `speed_internal.c`.

### 4.2 Skipping the identity block that `matrix_minor` writes

At QR iteration `k`, `matrix_minor(tmp_z, k)` forces the leading `k`x`k` block
to the identity, so a large and growing fraction of the products are
`0.0f * y`. Skipping them is *value*-identical but not *bit*-identical: the
sign of a zero result can flip, and `get_sign(A->data[k*size+k])` two lines
later is sign-sensitive. Not provable within this PR's evidence budget.

### 4.3 CAMBI is now the top entry — three separable sub-costs

`preprocess_and_extract_cambi` at 28.17 % breaks down (from the baseline
profile's inlined-child attribution and `-s sym,srcline`) as:

| Sub-cost | Baseline share of total | Bit-exact win available? |
| --- | --- | --- |
| `quick_select_partition` (the top-k selection over the c-values array) | 10.22 % | **No.** A different selection algorithm leaves the top-k in a different *order*, and `average_topk_elements` accumulates them into a `double` in array order, so the sum changes. |
| `anti_dithering_filter` (`cambi.c:1007`) | 4.98 % | **Yes.** It is a 2x2 box filter on `uint16_t` with `>>2`, done in place — and the in-place update has no read-after-write hazard, because row `i` only ever reads columns `j`, `j+1` of row `i` (never yet written) and row `i+1` (written in the next outer iteration). Widening it to AVX2 via 32-bit unpack/add/shift/pack is integer-exact by construction. Left out of this PR only to keep it to one subsystem; it is the obvious next increment. |
| `average_topk_elements` (`cambi.c:1502`) | 3.70 % | **No** for the same ordering reason as the selection above. |

### 4.4 `si_mat_mul` in `speed_internal.c`

The ADR-0964 duplicate of the same i-k-j loop, used by the host side of the
CUDA / SYCL / HIP SpEED twins. Routing it through the same dispatch would be
bit-exact by the identical argument. It is not in this PR because the benefit
cannot be *measured* on this host: no GPU backend currently completes a scored
run longer than one motion batch
(`T-GPU-MOTION-FLUSH-DOUBLE-EMIT-2026-09-06`). Landing an unmeasurable change
would be a guess.

## 5. How to reproduce

```bash
# build with symbols and frame pointers
meson setup build-prof core -Denable_cuda=false -Denable_sycl=false -Denable_hip=false \
      --buildtype=release -Db_ndebug=true \
      -Dc_args='-g -fno-omit-frame-pointer' -Dcpp_args='-g -fno-omit-frame-pointer'
ninja -C build-prof

# a profiling-length input: the Netflix src01 pair, 30x
for i in $(seq 30); do cat python/test/resource/yuv/src01_hrc00_576x324.yuv; done > /tmp/ref1440.yuv
for i in $(seq 30); do cat python/test/resource/yuv/src01_hrc01_576x324.yuv; done > /tmp/dis1440.yuv

perf record -F 4999 -g --call-graph=fp -o perf.data -- \
  ./build-prof/tools/vmaf -r /tmp/ref1440.yuv -d /tmp/dis1440.yuv \
    -w 576 -h 324 -p 420 -b 8 --no_cuda --no_sycl --no_hip --json -o /dev/null
perf report -i perf.data --no-children -g none --percent-limit 0.3 --stdio
perf report -i perf.data --no-children -g none -s sym,srcline --percent-limit 0.7 --stdio

# bit-exactness gate: identical on every dispatch path
for mask in 0 48 56; do
  ./build-prof/tools/vmaf -r python/test/resource/yuv/src01_hrc00_576x324.yuv \
      -d python/test/resource/yuv/src01_hrc01_576x324.yuv -w 576 -h 324 -p 420 -b 8 \
      --no_cuda --no_sycl --no_hip --cpumask $mask --json --precision=max -o out-$mask.json
done   # every out-*.json must agree with the pre-change run except for "fps"

# SIMD parity unit tests
./build-prof/test/test_speed_simd
```

## 6. Bottom line

- One win landed: 1.20x on the default-model CPU path, bit-identical on the
  scalar, AVX2 and AVX-512 dispatch paths, `memcmp`-gated in unit tests, and
  no change at all to the no-SpEED control.
- The larger algorithmic win (Householder rank-1 update, ~25x on the same
  code) is documented but not taken, because it moves scores.
- CAMBI's `anti_dithering_filter` is the next bit-exact increment, worth
  ~5 % of the run; the other two CAMBI sub-costs are order-sensitive and are
  not.
- No GPU profile exists: Nsight Compute, Nsight Systems, VTune and rocprof are
  all absent from this host, and the GPU backends cannot complete a scored run
  on this commit anyway.
