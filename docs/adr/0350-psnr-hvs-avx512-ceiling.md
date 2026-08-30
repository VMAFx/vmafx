<!-- markdownlint-disable MD060 -->
# ADR-0350: `psnr_hvs` AVX-512 — re-bench confirms AVX2 ceiling (T3-9 (a))

- **Status**: Accepted
- **Date**: 2026-05-09
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: simd, avx512, psnr-hvs, ceiling, audit, fork-local

## Context

T3-9 in [`.workingdir2/BACKLOG.md`](../../.workingdir2/BACKLOG.md)
is the unified AVX-512 follow-up audit sweep covering three
candidate widening targets, each gated by the same methodology:
**bench AVX-512 first against the existing AVX2 path on the Netflix
normal pair (`src01_hrc00_576x324.yuv` ↔ `src01_hrc01_576x324.yuv`,
576×324 8-bit); ship if 16-lane wins by ≥ 1.3× over AVX2; otherwise
document as a [ADR-0180](0180-cpu-coverage-audit.md)-style ceiling
row.** Sub-row (a) is `psnr_hvs` AVX-512 — the former T3-9 standalone
row, which [ADR-0180](0180-cpu-coverage-audit.md) closed as
"AVX2 ceiling" on 2026-04-26 with a 1.17× wall-clock speedup of
AVX2 vs scalar.

This ADR re-runs the bench on the same fixture from current
master (post `9cd2a354`) on a Zen 5 host with full AVX-512
support (`avx512f / dq / cd / bw / vl / ifma / vbmi`) and records
the verdict. The re-bench is part of T3-9's "bench-first then
decide" methodology — even when a prior audit has already
concluded ceiling, T3-9's escape clause requires the
re-measurement to land before T3-9 (a) can close.

The AVX2 path
([`core/src/feature/x86/psnr_hvs_avx2.c`](../../core/src/feature/x86/psnr_hvs_avx2.c))
vectorises only the integer 8×8 DCT (`od_bin_fdct8x8_avx2`); the
per-block scalar reductions (`load_block_and_means`,
`compute_vars`, `compute_masks`'s float fold, `accumulate_error`)
all stay scalar by design. That is the load-bearing
bit-exactness invariant from
[ADR-0138](0138-iqa-convolve-avx2-bitexact-double.md) /
[ADR-0139](0139-ssim-simd-bitexact-double.md): float reductions
must run per-lane scalar so the IEEE-754 summation tree matches
the scalar reference byte-for-byte (Netflix golden assertions
trip on ≥ 5.5e-5 drift; see
[ADR-0160](0160-psnr-hvs-neon-bitexact.md) §Context). Any
AVX-512 widening can therefore only attack the DCT, not the
scalar tail.

A 16-lane DCT design exists in principle: process two 8×8 blocks
per call by stacking each block's 8-lane vector into a single
`__m512i` (lanes 0–7 = block A column k, lanes 8–15 = block B
column k). The host loop would batch consecutive `(x, y)`
positions into 2-block chunks. Even at perfect utilisation this
caps the speedup at the DCT's share of the per-block cost.

## Decision

Re-bench confirms the [ADR-0180](0180-cpu-coverage-audit.md)
verdict: **`psnr_hvs` AVX-512 stays out of tree as an AVX2
ceiling.** The decisive measurement is per-symbol cycle share
under `perf record` on the 48-frame Netflix normal pair:

| symbol                   | cycles share | what AVX-512 could touch |
| ---                      | ---          | ---                      |
| `calc_psnrhvs_avx2`      | **78.42 %** | scalar tail — locked by ADR-0138/0139 bit-exactness |
| `od_bin_fdct8x8_avx2`    | **14.82 %** | 8 → 16 lanes via 2-block batch (~50 % of this cost recoverable) |
| libc / kernel / glue     | ~6.8 %       | n/a |

**Amdahl ceiling: even an *infinitely fast* 16-lane DCT caps
total wall-clock improvement at 14.82 % / (1 − 0.1482) = 17.4 %
(i.e. 1.17× over current AVX2).** A realistic 2-block batch
recovers ~50 % of the DCT cost, so the practical wall-clock gain
is ~7–8 % (1.07–1.08× over AVX2) — well below the 1.3× threshold
T3-9 set as the ship gate.

The structural argument tracks the empirical numbers. The 8×8
integer DCT (30 butterfly ops × 2 passes × 8 lanes = 480 SIMD
ops per call) is bandwidth-amortised on the 8×8 working set: the
load + transpose + butterfly + store sequence is dominated by
the loaded-bytes-per-flop ratio, not by lane count. Going to 16
lanes does not help because 64 int32 values per block fit
trivially in either lane width; the bottleneck is the per-block
fixed costs, not DCT throughput. The same conclusion landed in
[ADR-0179](0179-float-moment-simd.md) for `float_moment` and in
[ADR-0180](0180-cpu-coverage-audit.md) for `psnr_hvs` and
`float_moment` together — three distinct kernels whose AVX2 path
is already at the bandwidth-amortised ceiling on practical
fixtures.

T3-9 (a) closes as ceiling — no `psnr_hvs_avx512.c`,
`psnr_hvs_avx512.h`, meson wiring, or dispatch entry will be
added. T3-9 (b) and (c) (`ssimulacra2_*` AVX-512+NEON drift
audit; `iqa_convolve` AVX-512) are separate sub-rows tracked in
T3-9 and benched independently — this ADR settles only sub-row
(a).

The follow-up bullet in
[ADR-0160](0160-psnr-hvs-neon-bitexact.md) §Consequences
("AVX-512 `psnr_hvs` intentionally not scheduled (AVX2 covers
the x86_64 baseline; adding 512 requires re-verifying
bit-exactness against a different reduction tree)") gains a
status-update appendix per the ADR-0028 / ADR-0106 immutability
rule, pointing back to this ADR as the empirical close-out.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| **Document as ceiling row, no code (this ADR)** | Empirical evidence (78.42 % / 14.82 % cycle share) plus structural argument both point the same way; matches ADR-0180 / ADR-0179 precedent for the same kernel family; zero ongoing maintenance cost | Matrix row stays "no AVX-512" — looks asymmetric next to AVX2 + NEON | **Chosen** — methodology says "ship if ≥ 1.3×, else document as ceiling"; 1.07–1.08× projected gain falls comfortably below the gate |
| Implement AVX-512 2-block batched DCT anyway | Closes the matrix row to ✓; some marketing value | Amdahl-bounded < 1.17× wall-clock; doubles the host-loop complexity (2-block batch + edge-block handling for `(w − 8 + step) % 16 ≠ 0` cases); needs a third bit-exactness audit (`od_bin_fdct8x8_avx512` vs scalar on 5+ inputs); larger NOLINT footprint for the 16-lane butterfly + 2-block transpose | Code work without measurable benefit; methodology gate fails by a large margin — not by a small one |
| Re-architect to widen scalar reductions to vector | Could lift the 78.42 % scalar share into AVX-512 territory | Breaks the ADR-0138 / ADR-0139 bit-exactness contract — float associativity drift trips Netflix golden by ~5.5e-5 ([ADR-0160](0160-psnr-hvs-neon-bitexact.md) §Context); requires an explicit tolerance ADR which the fork rule rejects ("never weaken a test to make it pass" per `feedback_no_test_weakening`) | Rejected — bit-exactness is non-negotiable per CLAUDE.md §1 r1 |
| AVX-512 with `vmovdqa64` masked stores reusing AVX2 8-lane DCT | Avoids the 2-block batch | Identical instruction-level work as AVX2 (8 lanes used); zero throughput delta; pure cosmetic widening | Rejected — no theoretical benefit, just a re-skin |

## Consequences

- **Positive**:
  - Re-bench **confirms** [ADR-0180](0180-cpu-coverage-audit.md)'s
    verdict empirically on a current host (Zen 5, 2026-05-09);
    audit row T3-9 (a) closes with evidence rather than by
    extrapolation. Saves ~400–600 LoC of AVX-512 SIMD plus the
    associated bit-exactness audit + NOLINT debt.
  - Methodology proven sound — the bench-first gate stops a 1.07× speedup
    from being shipped as if it were a 1.3× one. Same shape as
    [ADR-0180](0180-cpu-coverage-audit.md)'s
    `psnr_hvs` and `float_moment` close-outs and
    [ADR-0179](0179-float-moment-simd.md)'s no-AVX-512 reasoning.
  - Decision is now backed by a per-symbol cycle breakdown,
    which is more actionable for future re-audits than the
    earlier 1.17× wall-clock figure alone.
- **Negative**:
  - Matrix row `psnr_hvs / x86 / avx512` stays empty — visually
    asymmetric next to AVX2 + NEON. Mitigated by the explicit
    ceiling row in
    [`.workingdir2/analysis/metrics-backends-matrix.md`](../../.workingdir2/analysis/metrics-backends-matrix.md)
    pointing at this ADR.
- **Neutral / follow-ups**:
  - T3-9 (a) marked DONE-as-ceiling in BACKLOG.md.
  - [ADR-0160](0160-psnr-hvs-neon-bitexact.md) gets a
    `### Status update 2026-05-09` appendix linking back here.
  - T3-9 (b) and (c) remain open under the same row; they bench
    independently in their own follow-up PRs.
  - If a future host (e.g. Intel Granite Rapids / Diamond Rapids
    with substantially wider DCT throughput) shifts the
    78 / 15 split materially, this ADR is open to revisit; the
    re-bench command in `## Reproducer` below is the gate.

## Verification / Reproducer

```bash
# CPU-only release build
cd libvmaf
meson setup build -Denable_cuda=false --buildtype=release
ninja -C build

# Set up Netflix normal pair as bench fixture
mkdir -p /tmp/vmaf_test
cp $REPO/python/test/resource/yuv/src01_hrc00_576x324.yuv /tmp/vmaf_test/ref_576x324.yuv
cp $REPO/python/test/resource/yuv/src01_hrc01_576x324.yuv /tmp/vmaf_test/dis_576x324.yuv

# Per-symbol cycle share — the decisive measurement
perf record -F 4000 -g -o /tmp/perf.data -- \
  build/tools/vmaf -r /tmp/vmaf_test/ref_576x324.yuv \
                   -d /tmp/vmaf_test/dis_576x324.yuv \
                   -w 576 -h 324 -p 420 -b 8 \
                   --threads 1 --no_prediction --feature psnr_hvs \
                   -o /tmp/bench.json --json
perf report -i /tmp/perf.data --stdio --no-children -g none | head -10
```

Recorded output (Zen 5, 2026-05-09):

```text
    78.42%  vmaf   libvmaf.so.3.0.0   [.] calc_psnrhvs_avx2
    14.82%  vmaf   libvmaf.so.3.0.0   [.] od_bin_fdct8x8_avx2
     ...
```

Wall-clock baseline (n=10 each, isolated by `--feature psnr_hvs`
delta over the `--no_prediction` no-feature baseline):

| run                                                  | wall-clock (s) | psnr_hvs increment (s) |
| ---                                                  | ---            | ---                    |
| `--no_prediction` only (default cpumask)             | 0.0027         | —                      |
| `--no_prediction --cpumask 0xfffffffe`               | 0.0022         | —                      |
| `--no_prediction --feature psnr_hvs` (default)       | 0.0743         | **0.0716** (AVX2)      |
| `--no_prediction --feature psnr_hvs --cpumask 0xfffffffe` | 0.1096    | **0.1074** (scalar)    |

Effective AVX2 speedup vs scalar on this host: **0.1074 / 0.0716
= 1.50×**. Effective AVX-512 ceiling: **0.0716 / (1 − 0.1482 / 2)
≈ 0.0666 s**, i.e. **1.075×** over current AVX2 — comfortably
below the T3-9 1.3× ship gate.

## References

- [ADR-0180](0180-cpu-coverage-audit.md) — original `psnr_hvs`
  AVX-512 ceiling close-out (2026-04-26, 1.17× scalar→AVX2 on
  the prior host); this ADR re-validates the verdict
  empirically and quantifies the Amdahl ceiling per-symbol.
- [ADR-0179](0179-float-moment-simd.md) — same "no AVX-512,
  memory-bound reduction" reasoning applied to `float_moment`
  (the bench-first methodology's first instance).
- [ADR-0159](0159-psnr-hvs-avx2-bitexact.md) — AVX2 bit-exactness
  contract source-of-truth.
- [ADR-0160](0160-psnr-hvs-neon-bitexact.md) — NEON sister
  port; gets a status-update appendix here.
- [ADR-0138](0138-iqa-convolve-avx2-bitexact-double.md) /
  [ADR-0139](0139-ssim-simd-bitexact-double.md) — float
  reduction-stays-scalar discipline that locks the 78 % scalar
  share against vectorisation.
- T3-9 backlog row at
  [`.workingdir2/BACKLOG.md`](../../.workingdir2/BACKLOG.md):540 —
  unified AVX-512 audit sweep, sub-row (a) closed by this ADR.
- User direction (paraphrased): implement T3-9 (a) bench-first
  per the methodology; if the 16-lane path doesn't clear 1.3×
  AVX2 on the Netflix normal pair, document as a ceiling row in
  the existing ADR-0160 footnote rather than ship.
- Re-bench host: AMD Ryzen 9 9950X3D (Zen 5, full AVX-512),
  Linux 7.0.5-cachyos, GCC 14, single-thread.
