# Known upstream bugs

Bugs that reproduce on `upstream/master` (Netflix/vmaf) as well as this fork's
`master`, discovered during fork work but out of scope for the PR that found
them. Each entry records the reproducer, the evidence it is upstream, and the
suggested fix.

When a fork-local PR touches the same file, prefer to fix the bug in that PR
and reference this entry in the commit. If the PR does not touch the file,
file a follow-up ticket and link to it here.

---

## Open pull requests this fork has sent upstream

Eight, all open on 2026-09-19 and all validated against upstream
`86da14d0306a138fd3f01319860b905169746516`. No CI has ever run on any of them:
every workflow on the upstream repository sits at `action_required`, waiting for
a maintainer to approve a first-time contributor's run. Each was rebased onto
that revision and re-validated locally on 2026-09-19.

| Upstream PR | What it fixes | Where the fork tracks it |
| --- | --- | --- |
| [#1588](https://github.com/Netflix/vmaf/pull/1588) | `vmaf_model_feature_overload()` leaks the caller's dictionary when a merge fails | `T-UPSTREAM-1242-FEATURE-DICT-OWNERSHIP-2026-09-03` |
| [#1589](https://github.com/Netflix/vmaf/pull/1589) | percentile pooling methods on the C API | `T-UPSTREAM-818-POOLING-ENUM-NO-PERCENTILES-2026-09-03` |
| [#1590](https://github.com/Netflix/vmaf/pull/1590) | model-collection allocation failure handling | ADR-1166 harvest |
| [#1591](https://github.com/Netflix/vmaf/pull/1591) | thread-pool creation error paths | ADR-1166 harvest |
| [#1599](https://github.com/Netflix/vmaf/pull/1599) | scale-3 DWT reads index -1 for frame dimensions 17 to 32 | `T-ADM-SCALE3-TINY-FRAME-OOB-READ-2026-09-18` |
| [#1600](https://github.com/Netflix/vmaf/pull/1600) | `pow(2, shift - 1)` with a shift of 0 in `adm_cm` | `T-ADM-AVX512-SMALL-WIDTH-SCALE0-2026-09-18` |
| [#1601](https://github.com/Netflix/vmaf/pull/1601) | signed overflow in the 16-bit vertical DWT, and a left shift of negative taps | `T-ADM-DWT2-16BIT-INT32-OVERFLOW-2026-09-18` |
| [#1602](https://github.com/Netflix/vmaf/pull/1602) | the SIMD `adm_cm` centre tap keeps 32 bits where scalar wraps to int16 | `T-ADM-CM-SIMD-NOISE-NOT-BIT-EXACT-2026-09-18` |

Two of these differ from what the fork carries, which matters at the next sync:

- **#1601 takes a cheaper fix than the fork's.** The fork widens the accumulator
  to int64 (PR #1477). Upstream measured that at 3.5 to 6 % of throughput, so
  the upstream patch starts the sum from the normalization offset instead, which
  adds no operation and measures within noise. **That approach is worth bringing
  back to the fork.**
- **#1602 makes SIMD follow scalar**, as the fork does, and says plainly that
  `float_adm` is closer to the unwrapped vector value than to scalar, so the
  wrap is an artefact of the scalar reference. If upstream decides to remove the
  int16 cast everywhere instead, the fork's PR #1474 needs revisiting.

Upstream PR [#1494](https://github.com/Netflix/vmaf/pull/1494) (open since April,
by an upstream maintainer) refactors the same ADM functions. It does not touch
the lines above, but whichever lands first leaves the other needing a rebase.

## Reported upstream on 2026-09-19

Six defects found on `86da14d0` while validating the pull requests above were
reported on 2026-09-19, each reproduced on upstream first. The right-hand
column is this fork's own status, checked against the fork's tree the same day
rather than inferred from upstream's.

| Upstream | What | This fork |
| --- | --- | --- |
| [#1603](https://github.com/Netflix/vmaf/pull/1603) (PR) | checkasm's `check_adm_dwt2` passes a byte stride where `adm_dwt2_16()` indexes samples, and a second site passes a band stride as the source stride; ASan: heap over-read | **Not affected** — the fork carries no `checkasm` tree |
| [#1604](https://github.com/Netflix/vmaf/pull/1604) (PR) | The direct YUV and y4m readers read floor-sized chroma rows where the file stores ceil-sized ones, and `fetch_picture()` returns `!ret`, turning a reader error into "usable picture" and a crash | **Not affected** on both counts: `picture_compute_geometry()` allocates ceiling chroma, `USE_DIRECT_READ` is never defined so the buffered reader runs, the CLI refuses odd 4:2:0 dimensions outright, and `finish_unread_picture()` maps errors to `-1`. The one piece upstream left open — two failed reads classified as a clean end of stream, and every read failure exiting 0 — **was live here** and is fixed by ADR-1262 |
| [#1605](https://github.com/Netflix/vmaf/pull/1605) (PR) | AVX2 `get_best15_from32()` shifts by a negative count on every lane before the blend discards it | **Not affected** — the AVX2 helper has returned early below 32768 since PR #792; scalar, AVX-512, CUDA, HIP, Metal and SYCL guard at the call site |
| [#1606](https://github.com/Netflix/vmaf/pull/1606) (PR) | A zero-length variable-length array when `--no_prediction` leaves `model_cnt` at 0 | **Not affected** — `ModelArrays::allocate()` returns before allocating when the count is 0 (ADR-0809) |
| [#1607](https://github.com/Netflix/vmaf/issues/1607) (issue) | Frames of 16 px and below crash integer ADM: `(uint32_t)ceil(log2(w) - 4)` converts a negative double, and `h_half - 2` underflows an unsigned bound in `dwt2_src_indices_filt()`. Upstream #1599 and #1600 do not fix it | **Not affected** — the extractor refuses the input with `integer_adm requires width >= 17 and height >= 17` instead of running; measured at 8, 12 and 16 px. Whether to refuse or support such frames is the decision upstream was asked to make |
| [#1608](https://github.com/Netflix/vmaf/issues/1608) (issue) | The SIMD `adm_cm` narrows `accum_h` to `float` before dividing where its siblings and scalar do not | **Same cast present** (`adm_avx512.c`), **no effect**: the divisor is an exact power of two, all 108 values over the three reference pairs are bit-identical, and 2,000,000 random integers in `[2^53, 2^62)` show no difference |

The earlier note on the last item — that the cast "likely explains upstream's
1e-4 checkasm tolerance" — was wrong and is withdrawn: instrumenting the three
tolerance sites over a full `checkasm --test=adm` run gives 90 comparisons, 87
exactly equal, and a worst relative deviation of `1.910e-07`, roughly 500 times
inside the tolerance.

---

## `adm_decouple_s123_avx512` LTO+release SEGV — fixed in this fork

**Status:** fixed in this fork (PR #69 follow-up commit), still present upstream.

**Symptom:** `test_pic_preallocation` aborts with
`AddressSanitizer: SEGV on unknown address` inside
`adm_decouple_s123_avx512` when the binary is built with
`--buildtype=release -Db_lto=true -Db_sanitize=address`. The debug
ASan build used by CI (`--buildtype=debug -Db_lto=false`) does not
reproduce the crash.

Reproduce with:

```bash
meson setup build-asan-lto libvmaf \
  -Denable_cuda=false -Denable_sycl=false \
  -Db_sanitize=address --buildtype=release -Db_lto=true
ninja -C build-asan-lto test/test_pic_preallocation
ASAN_OPTIONS=detect_leaks=1 ./build-asan-lto/test/test_pic_preallocation
```

**Evidence it is upstream, not fork-local:** the same reproducer on
`origin/master` (no fork-local patches applied) produces the same
crash. The faulting instruction is
`vmovdqa64 zmm2, ZMMWORD PTR [rdi-0xc0]`, a 64-byte-aligned AVX-512
load served a 32-byte-aligned address.

**Why CI does not catch it:** CI's sanitizer job uses
`--buildtype=debug -Db_lto=false`, which keeps every
`_mm512_loadu_si512` as `vmovdqu64` (unaligned) and so runs fine. The
`--suite=unit` filter in `tests-and-quality-gates.yml` also matches
zero tests in `core/test/meson.build`, so the job reports green
even if the link succeeds. Tracked separately — the suite filter
needs to be corrected.

**Root cause:** the stack array `int64_t angle_flag[16]` inside
`adm_decouple_s123_avx512` is loaded via
`_mm512_loadu_si512(&angle_flag[0])` and
`_mm512_loadu_si512(&angle_flag[8])`. Under LTO, link-time
alignment inference promotes the unaligned loads to the aligned
`vmovdqa64` form. The C-level default stack alignment for an
`int64_t[16]` is 8 bytes, so the promoted aligned load faults on
every other 64-byte slot.

**Fix applied in this fork:** annotate the stack array with
`_Alignas(64)` at
[`core/src/feature/x86/adm_avx512.c:1317`](../../core/src/feature/x86/adm_avx512.c#L1317).
The unaligned load remains correct, and the LTO-promoted aligned
form is now also correct.

**Related issue surfaced during triage:**
`test_picture_pool_basic`, `test_picture_pool_small`, and
`test_picture_pool_yuv444` loaded a `VmafModel` via
`vmaf_model_load` and never called `vmaf_model_destroy`, so
LeakSanitizer reported 208 bytes direct + 23 KiB indirect leaks per
test. Pairing `vmaf_model_destroy(model)` with each load is also
landed in PR #69 (same commit).

---

## `KBND_SYMMETRIC` single-reflection at sub-kernel-radius input sizes

**Status:** fixed in this fork (PR #69), still present upstream.

**Symptom:** For a 2-D convolution with a 9-tap kernel on inputs
smaller than the kernel half-width (n ≤ 3 for `LPF_HALF = 4`),
upstream's `KBND_SYMMETRIC` reflects the index only once; the
reflected index is still out of bounds, causing an out-of-bounds read.

Reproduce with:

```c
/* With upstream KBND_SYMMETRIC, idx=-4, n=1 reflects to 3 (OOB for n=1). */
float v = KBND_SYMMETRIC(img_1x1, 1, 1, -4, 0, 0.0f);  /* reads img[3] */
```

**Why it is latent upstream:** MS-SSIM pyramids never decimate below
~60×34 in practice, and SSIM / ADM similarly never feed a 1×1 input
through the convolver. Nothing in Netflix/vmaf's test corpus exercises
the regime.

**Fix applied in this fork:** `KBND_SYMMETRIC` and
`ms_ssim_decimate_mirror` (scalar + AVX2 + AVX-512 + NEON) are
rewritten in the period-based (`period = 2*n`) form that bounces
correctly for any offset. See
[`docs/adr/0125-ms-ssim-decimate-simd.md`](../adr/0125-ms-ssim-decimate-simd.md)
and the inline comment in
[`core/src/feature/iqa/convolve.c`](../../core/src/feature/iqa/convolve.c).
