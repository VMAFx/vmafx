# Known upstream bugs

This page tracks what the fork knows about defects in Netflix/vmaf
(`upstream/master`): the pull requests the fork has sent upstream, defects
verified against the fork, and the upstream commit the fork is at parity with.
Sections carry their own as-of date, and the page is a dated log, not a list of
currently open bugs. The heading "Upstream head the fork is at parity with"
is the parity pin: `scripts/ci/upstream_parity_pin.py` reads it, so exactly one
such heading must exist (see
[licence-provenance-check.md](licence-provenance-check.md)).

Each entry records the reproducer, the evidence that it is upstream, and the
fix. When a fork-local PR touches the same file, prefer to fix the bug in that
PR and reference the entry in the commit. If the PR does not touch the file,
file a follow-up ticket and link to it here.

| Section | As of |
| --- | --- |
| [Open pull requests sent upstream](#open-pull-requests-this-fork-has-sent-upstream) | 2026-10-08 |
| Upstream defects verified against the fork | 2026-10-01 |
| [Upstream GPU defects checked against the fork](#upstream-gpu-defects-checked-against-the-fork-2026-10-05) | 2026-10-05 |
| Parity pin (upstream head the fork is at parity with) | 2026-10-02 |
| [Reported upstream on 2026-09-19](#reported-upstream-on-2026-09-19) | 2026-09-19 |
| Individual defects (ADM rounding, AVX-512 LTO SEGV, AIM clipping, `KBND_SYMMETRIC`) | per entry |

---

## Open pull requests this fork has sent upstream

Forty-two are open on 2026-10-08 (#1588 to #1668). The fork itself closed
pull requests #1605, #1606 and #1634; #1484 and #1536 were opened against the
wrong repository in April and May and closed. One (#1602) has drawn a review comment;
none has been approved or merged. No CI has run on any of them: every workflow
run sits at `action_required`, waiting for a maintainer to approve a
first-time contributor's run. Checked against upstream `9cb9479f2` on
2026-10-08: none is superseded or obsolete. Four needed a rebase (#1588, #1627
and #1642 conflicted only in `test_feature_extractor.c`; #1636 had to be
reworked, see below), and all four were rebased onto `9cb9479f2` the same day.
The table lists the first fifteen; #1631 to #1668 are in `docs/state.md` under
"Confirmed not-affected".

The right-hand column says whether the fork's own tree carries each fix,
checked on 2026-10-01 against the fork's code at master `591d53449`; the
evidence for each row is in [`docs/state.md`](../state.md) under "Confirmed
not-affected".

| Upstream PR | What it fixes | In the fork |
| --- | --- | --- |
| [#1588](https://github.com/Netflix/vmaf/pull/1588) | option dictionaries leak when an overload or a registration fails | Fixed and tested (`T-UPSTREAM-1242-FEATURE-DICT-OWNERSHIP-2026-09-03`) |
| [#1589](https://github.com/Netflix/vmaf/pull/1589) | median and percentile pooling on the C API | Present and tested (`T-UPSTREAM-818-POOLING-ENUM-NO-PERCENTILES-2026-09-03`) |
| [#1590](https://github.com/Netflix/vmaf/pull/1590) | a failed model-collection growth loses the collection | Fixed; test added by PR #1663 |
| [#1591](https://github.com/Netflix/vmaf/pull/1591) | thread-pool creation ignores `pthread_create()` errors | Fixed and tested; a partly started pool stays usable here |
| [#1599](https://github.com/Netflix/vmaf/pull/1599) | scale-3 DWT reads index -1 for frame dimensions 17 to 32 | Fixed and tested (`T-ADM-SCALE3-TINY-FRAME-OOB-READ-2026-09-18`) |
| [#1600](https://github.com/Netflix/vmaf/pull/1600) | $2^{\mathrm{shift} - 1}$ (`pow(2, shift - 1)`) with a shift of 0 in `adm_cm` | Fixed and tested (`T-ADM-AVX512-SMALL-WIDTH-SCALE0-2026-09-18`) |
| [#1601](https://github.com/Netflix/vmaf/pull/1601) | signed overflow in the 16-bit vertical DWT, and a left shift of negative taps | Fixed and tested, by widening to int64 (`T-ADM-DWT2-16BIT-INT32-OVERFLOW-2026-09-18`) |
| [#1602](https://github.com/Netflix/vmaf/pull/1602) | `adm_cm` SIMD and scalar disagree above a coefficient of 15360 | Second revision taken in every implementation ([ADR-1402](../adr/1402-adm-cm-centre-tap-int32.md)); the fork differs from upstream master on such content until upstream merges it |
| [#1603](https://github.com/Netflix/vmaf/pull/1603) | checkasm passes wrong strides to the ADM DWT tests | Not affected: no `checkasm` tree |
| [#1604](https://github.com/Netflix/vmaf/pull/1604) | frames with an odd width or height lose framing; a reader error becomes a crash | Not affected; test added by PR #1664 |
| [#1606](https://github.com/Netflix/vmaf/pull/1606) | a zero-length variable-length array with `--no_prediction` | Not affected: no VLA (ADR-0809) |
| [#1620](https://github.com/Netflix/vmaf/pull/1620) | SpEED initialises on frames too small for one block | Fixed and tested |
| [#1621](https://github.com/Netflix/vmaf/pull/1621) | a bit-depth mismatch between reference and distorted is accepted | Fixed and tested (`test_validate_pic_params_bpc`) |
| [#1627](https://github.com/Netflix/vmaf/pull/1627) | `speed_temporal` overruns its buffers at `speed_prescale` above 1 | Ported, fork PR #1643 (`T-SPEED-TEMPORAL-PRESCALE-UP-OVERFLOW-2026-09-30`) |
| [#1629](https://github.com/Netflix/vmaf/pull/1629) | `cambi` walks outside frames shorter than its window | Ported, fork PR #1642 (`T-CAMBI-SHORT-FRAME-OOB-2026-09-30`) |

Upstream PR #1602 is incomplete on arm64 since 8bc5a5c6a / b41d2340a:
upstream `adm_cm_neon()` keeps the int16 centre-tap wrap that #1602 removes
elsewhere (see the arm64 note in `docs/rebase-notes.md`).

### Where the fork differs from these pull requests

Three of these differ from what the fork carries, which matters at the next
sync.

**#1601 takes a cheaper fix than the fork's.**

- Fork: widens the accumulator to int64 (PR #1477). Upstream measured that at
  3.5 to 6 % of throughput.
- Upstream: starts the sum from the normalization offset instead, which adds
  no operation and measures within noise.
- Action: both are correct. That approach is worth bringing back to the fork
  as a performance change; it has not been done.

**#1602 changed direction after review, and the fork followed.**

- First revision: SIMD follows the scalar int16 wrap of the masking centre tap.
- Second revision (2026-09-21): removes the wrap from the scalar, x86 edge and
  CUDA code instead, after a reviewer there called the wrap wrong.
- Measured in the fork on 2026-10-01: the wrap is an artefact. A flat reference
  with isolated impairments scored `integer_adm_scale0` above 1 where
  `float_adm` gives exactly 1.
- Fork: removed the wrap from the scalar, AVX2, AVX-512, CUDA, HIP, SYCL and
  Metal code the same day ([ADR-1402](../adr/1402-adm-cm-centre-tap-int32.md)),
  after checking that no Netflix golden assertion moves.
- Until upstream merges the pull request the fork's integer ADM differs from
  upstream master on content that reaches a centre coefficient of 15360, such
  as full-range noise. The `docs/state.md` row
  `T-ADM-CM-CENTRE-TAP-WRAP-ABOVE-ONE-2026-10-01` has the measured deltas.

**#1591 and #1588 are narrower than the fork.**

- The fork keeps a thread pool that started at least one worker.
- Its `vmaf_use_feature()` consumes the dictionary on a failed copy too.
- Action: keep both at a sync.

Upstream PR [#1494](https://github.com/Netflix/vmaf/pull/1494) (open since
April, by an upstream maintainer, last updated 2026-10-01) refactors the same
ADM functions. It does not touch the lines above. cffd5b77d (2026-10-07, shared
computation across viewing distances, option `adm_norm_view_dist_extra`)
rewrote `integer_compute_adm()` and `extract()` in `integer_adm.c`: #1636 no
longer built against it and was reworked on 2026-10-08; the other ADM pull
requests merge clean. Whichever of #1494 and the fork's pull requests lands
first leaves the other needing a rebase.

## Upstream defects verified on `6ec23e8f2`, checked against the fork (2026-10-01)

As of 2026-10-01. Fifteen defects were reproduced on upstream master `6ec23e8f2`
while answering
Netflix issues on 2026-10-01. Each was run against the fork's `master` with the
reproducer from the upstream report, on this host (RTX 4090, gfx1036, Arc
A380 under `xe`; GCC 16.2.1, Clang 22.1.8). Three reproduced and are fixed;
twelve do not. The reproducers are kept by the maintainer outside the tree, one
directory
per issue.

| Upstream | Defect | On the fork |
| --- | --- | --- |
| [#1305](https://github.com/Netflix/vmaf/issues/1305) | Several CUDA instances in one process give wrong `motion2` and non-finite `vif` scales: an accumulator reset on the extractor stream, the kernels on the picture stream | **Reproduced in `integer_vif_cuda`, fixed** by PR #1750. Four instances on one context: wrong values in 15 of 15 runs before, none in 105 after. Every other CUDA reset (`motion_sad`, `adm`, `float_adm`, `float_psnr`, `psnr`, `cambi`, `kernel_template.h`) is on the kernels' stream; four instances of each of 19 CUDA extractors give identical output, and so do four HIP instances of the default model and twelve HIP extractors |
| [#1300](https://github.com/Netflix/vmaf/issues/1300) | Every init/close cycle leaks device memory and host memory (`cuModuleLoadData` without unload, streams not destroyed) | **Not affected.** Upstream's `repro1300`, 30 cycles of 1080p: device memory +0 MiB for the default model and for each of 19 extractors (upstream master +676 MiB), host memory flat after the first cycle (+1.2 MB over 29 cycles, upstream +763 MB); with `malloc_trim(0)` after every cycle about 4 KiB per cycle. HIP, 30 cycles: +0 MiB device after the first cycle, +80 KiB host over 29 cycles. [ADR-0157](../adr/0157-cuda-preallocation-leak-netflix-1300.md) and `test_cuda_module_lifecycle_contract.py` pin the unloads |
| [#1420](https://github.com/Netflix/vmaf/issues/1420) | `vmaf_cuda_buffer_alloc()` asserts on out-of-memory | **The allocator does not assert** (`CHECK_CUDA` returns `-ENOMEM`; `test_cuda_buffer_alloc_oom`). **A worse failure was found end to end and fixed** by PR #1752: after the allocation failed the CLI hung in `vmaf_close()` holding the device lock, because a failed `vmaf_read_pictures()` kept its pictures. Not reproducible on HIP: the iGPU allocates from system memory |
| [#1180](https://github.com/Netflix/vmaf/issues/1180), [#755](https://github.com/Netflix/vmaf/issues/755) | A per-frame score asked for before the flush fails or races | **By design, documented** by PR #1753. The call returns the value or `-EAGAIN`; with the multi-instance harness, queries made 2 and 3 frames behind the newest picture never returned a different number |
| [#910](https://github.com/Netflix/vmaf/issues/910) | `vmaf_read_pictures()` accepts a non-increasing or gapped index and scores wrong | **A repeated or earlier index is rejected** with `-EINVAL` ([ADR-0152](../adr/0152-vmaf-read-pictures-monotonic-index.md)). A gap is accepted and its effect on the motion scores is documented ([ADR-1429](../adr/1429-read-pictures-index-gaps-accepted.md), PR #1753); no wrong value is returned without an error |
| [#761](https://github.com/Netflix/vmaf/issues/761) | `--model path=C:\...` and `C:/...` split at the drive-letter colon | **Not affected.** `cli_split()` keeps a drive-letter colon in the value ([ADR-1190](../adr/1190-cli-option-string-escape-grammar.md), [ADR-1355](../adr/1355-cli-option-value-backslashes.md)); `vmaf -m 'path=C:\VMAF_evaluation\model\vmaf_v0.6.1.json'` and the `C:/` form reach the model loader as one path (`could not read model from path: "C:\VMAF_evaluation\..."` on Linux, where upstream says `bad option string`). `test_model_path_windows_drive_letter` pins it |
| [#1414](https://github.com/Netflix/vmaf/issues/1414) | `float_ms_ssim` below 176x176 fails late with a confusing message | **Not affected.** The CPU extractor refuses at `init()`: `float_ms_ssim: input resolution 176x144 is too small; the 5-level 11-tap MS-SSIM pyramid requires at least 176x176`, exit 234, no output file; 176x176 scores. The CUDA, HIP and SYCL twins say the same at 176x144 (exit 234) and score 176x176 (measured on all three). `test_float_ms_ssim_min_dim`. With `enable_chroma` the CPU and every twin also refuse a chroma plane below 176 (CUDA and HIP did not until `T-MS-SSIM-GPU-CHROMA-OPTION-DRIFT-2026-09-06` was closed on 2026-10-03; measured on the 576x324 4:2:0 pair, exit 234 on CPU, CUDA, SYCL and HIP) |
| [#1568](https://github.com/Netflix/vmaf/issues/1568) | `vmaf_write_output()` opens a UTF-8 path with narrow `fopen` on Windows | **Not affected.** `vmaf_write_output()` opens through `vmaf_open_utf8()`, models and inputs through `vmaf_fopen_utf8()`; both convert UTF-8 to wide on Windows (`core/src/compat/path_utf8.c`, `test_path_utf8`). Read from the source: no Windows host was run. Two narrow `fopen` calls remain, in the vendored `pelorus_qp_report_csv.c` (fix in pelorus, then re-vendor) and in the `VIF_OPT_DEBUG_DUMP` dump, which writes a fixed ASCII path |
| [best15](https://github.com/Netflix/vmaf/pull/1605) | The AVX2 `get_best15_from32()` shifts by a negative count and calls `clz(0)` on lanes the blend discards | **Not affected.** `decouple_s123_best15_avx2()` calls it only for magnitudes of 32768 and above (`adm_avx2.c`). `vmaf --feature adm --cpumask 16` (AVX2 only) and the default dispatch on the Netflix pair and the 1080p checkerboard pair under ASan and UBSan: no runtime error; the `fast` suite of that build: 215 of 215. Upstream: the fork's #1605 was closed on 2026-09-21 in favour of #1584 (still open on 2026-10-08), which removes the 24 per-lane call sites; the smaller fix is #1635 (open). UBSan on upstream `9cb9479f2` still reports `adm_avx2.c:1350` |
| aim-uninit | `score_aim` is read uninitialised when the ADM denominator is 0 (`adm_noise_weight=0`, flat reference) | **Not affected.** A flat reference with `adm_noise_weight=0` makes the frame fail with `integer_adm: undefined or non-finite aggregate at frame 0 (num=0 den=0 ...)` (and the float twin the same), the extractor returns the error before it reads the score, and no `aim` or `adm3` is emitted. Both ratios are written on every success path (`vmaf_adm_scale_ratios()`, `vmaf_adm_finalize_scores()`). Run under ASan and UBSan; not under MSan. Upstream since cffd5b77d: the integer extractor zero-initialises its result, so the integer AIM is a defined 0 (adm3 1) for a zero denominator; the float extractor still reads an uninitialised value (`float_adm.c:359`, `:376`). #1636 was reworked against that on 2026-10-08 |
| [#1562](https://github.com/Netflix/vmaf/issues/1562) | The CUDA motion `mirror()` is off by one against the CPU | **Not affected.** Netflix pair at `--precision max`, `--backend cpu` against `--backend cuda`: `integer_motion2`, `integer_motion3` and `vmaf` identical on 48 of 48 frames at 8 bits and at 10 bits (ADR-1372: one shared kernel) |
| [#1606](https://github.com/Netflix/vmaf/pull/1606) (`master-bagging.log`) | A half-built model leaks when a collection file is read as a single model | **Not affected.** `vmaf --model version=vmaf_b_v0.6.3` and `--model path=model/vmaf_b_v0.6.3.json` exit 0 under ASan and UBSan with no LeakSanitizer report; the `path=` form logs a warning where upstream logs an error |
| [#1553](https://github.com/Netflix/vmaf/pull/1553), [#1583](https://github.com/Netflix/vmaf/pull/1583) | CUDA with `--threads N` (N >= 2) exits 234, `context could not be synchronized`: `motion_cuda` is flushed twice | **Not affected.** The default model on the Netflix pair, `--threads 0`, `2`, `4`, `8`: exit 0 on CUDA, HIP and SYCL (A380), every output identical across thread counts (672 values on CUDA and SYCL, 720 on HIP; pooled `vmaf` 82.816058734212 on CUDA) |
| [#1612](https://github.com/Netflix/vmaf/pull/1612) | `motion_cuda` reads its previous-blur buffer uninitialised on frame 0 and leaks 8 bytes per init/close | **Not affected.** `compute-sanitizer --tool initcheck` on 5 frames: 0 errors for `motion_cuda` and `float_motion_cuda`; `--leak-check full`, 3 init/close cycles: 0 bytes leaked for both |
| [#1613](https://github.com/Netflix/vmaf/pull/1613) | With CUDA device input a chroma plane never reaches the host picture (`psnr_cb` at the 60 dB cap) | **Reproduced, fixed** by PR #1754: the device-to-host copy took the luma plane only. A CUDA run on host input is not affected (`psnr` y/cb/cr equal the CPU at `--threads 0` and `4`; same on HIP, and on SYCL to 7e-15) |

The remaining Netflix change since `6ec23e8f2`, `8e7a1ac4e` ("integer_vif:
restore `void *` cursor in `vif_buffer_alloc`", 2026-10-01 18:05 UTC),
reverts `6ec23e8f2` (PR #1476), which broke the build with GCC 14 and later
and with Clang 22 ([#1630](https://github.com/Netflix/vmaf/issues/1630)). The
fork needs nothing from it: it never took #1476, and its
`vif_buffers_alloc()` in `core/src/feature/integer_vif.c` walks a byte cursor
with typed casts. `integer_vif.c` compiles with GCC 16.2.1 and with Clang
22.1.8 under `-Werror=incompatible-pointer-types`. That makes seven upstream
commits since the September port that the fork does not need, up to
`8e7a1ac4e`; `docs/state.md` ("Confirmed not-affected") lists the first six.

## Upstream GPU defects checked against the fork (2026-10-05)

As of 2026-10-05, on fork master `cf4e474be`. Two upstream reports about the
GPU twins that the section above does not list. The fork has neither defect.
It had the three defects of #1564 and fixed them before this check. Each row
names the test that holds the fix.

| Upstream | Defect | On the fork |
| --- | --- | --- |
| [#1566](https://github.com/Netflix/vmaf/issues/1566), fixed upstream by [#1552](https://github.com/Netflix/vmaf/pull/1552) (merged 2026-07-31) | The CUDA motion kernel for samples above 8 bits advanced a 16-bit element pointer by the picture's byte stride. It read row `2 * y` for row `y` and, for the lower half of the picture, memory past the plane. Every motion score above 8 bits was wrong, and with it the VMAF score | **Not affected.** Both CUDA motion twins run one SAD kernel that reads a row through a byte pointer and casts the row: `load_sample()` in `core/src/feature/cuda/integer_motion_v2/motion_v2_score.cu` ([ADR-1372](../adr/1372-cuda-motion-diff-first-pipeline.md)). At `--precision max` it returns the CPU's bits in `test_cuda_exact_twins` (8 and 10 bits) and `test_cuda_motion_tiny_frames` (tiny and odd frames, 16 bits). It also does so in every `motion` and `motion_v2` cell of the depth and layout matrix of PR #2172 (8, 10, 12 and 16 bits, 4:2:0, 4:2:2 and 4:4:4, odd width), on CUDA, SYCL and HIP. With the upstream form planted in `load_sample()`, every cell above 8 bits fails, and compute-sanitizer memcheck reports 26993 invalid global reads on one 16-bit run (0 without the plant). The static check of PR #2177 refuses that form in every CUDA, HIP, SYCL and Metal source |
| [#1564](https://github.com/Netflix/vmaf/issues/1564) bug 1 | The CUDA integer ADM masking kernel took the wrong border rows and columns at a band edge. It mirrored where the CPU mirrors on one side and replicates on the other, and read one csf row past the region the csf pass wrote. Frames smaller than about 150 pixels scored differently from the CPU | **Was affected, fixed.** Rows and columns are clamped to the CPU's pattern: `s0_cm_row()` is `min(abs(pos), h - 1)`, the columns are `{abs(x - 1), x, min(x + 1, w - 1)}`, and scales 1 to 3 clamp the same way (`core/src/feature/cuda/integer_adm/adm_cm.cu`; `docs/state.md` row `T-UPSTREAM-1564-ADM-CM-GPU-BORDER-AND-ROUNDING-2026-09-03`). Tests: `test_gpu_adm_tiny_frames` (17 to 64 pixels), `test_cuda_adm_small_border` |
| [#1564](https://github.com/Netflix/vmaf/issues/1564) bug 2 | The CUDA masking and denominator reductions applied the CPU's per-row rounding shift to each warp's partial sum. Rounding does not distribute over addition, so a row wider than one warp drifted, and a near-zero sum could double | **Was affected, fixed** ([ADR-1416](../adr/1416-cuda-adm-cpu-row-rounding.md)). Each row is summed whole and rounded once: `adm_cm_round_row_total()` and `adm_csf_den_round_row_total()` (`core/src/feature/adm_cm_accumulator.h`). Tests: `test_adm_cm_row_rounding`, `test_adm_cm_row_rounding_contract.py`, `test_cuda_adm_wide_rounding`, and `test_cuda_adm_parity` (`==` against the CPU) |
| [#1564](https://github.com/Netflix/vmaf/issues/1564) bug 3 | The x86 vector loop of the integer ADM DWT computed the mirrored last output column with taps that are not mirrored, when half the width was 1 more than a multiple of the vector length. It also loaded a few elements past the row | **Was affected, fixed** in fork commit `0ed57f9f1` (PR #1339). The vector loop stops at `half_w >= 2 ? half_w - 1 - ((half_w - 2) % N) : 1`, so the last column always goes through the scalar tail. That applies in every AVX2 and AVX-512 DWT kernel (`core/src/feature/x86/adm_avx2.c`, `adm_avx512.c`). Test: `test_adm_dwt2_x86` (the guard pattern at the production stride) |
| [#1564](https://github.com/Netflix/vmaf/issues/1564) follow-ups | The report's further findings: a neighbour clamp at 17 to 28 pixels, the one-degree angle test on exact integers against float, and a denominator shift taken from a device logarithm | **Fixed.** The angle test is `adm_angle_flag.h` (`test_adm_angle_flag`). The denominator shift comes from the host and no logarithm is left in `adm_csf_den.cu`. One device logarithm remains: the masking kernel's cube shifts, `__float2uint_ru(__log2f(w))` in `adm_cm.cu`, which equals the CPU's for 1 to 131071 (`core/src/feature/cuda/AGENTS.d/adm.md`) |

<!-- The Licence Provenance check reads the heading below
     (scripts/ci/upstream_parity_pin.py, licence-provenance-check.md), and
     the upstream parity guard builds the commit it names
     (scripts/dev/upstream_parity.py, upstream-parity.md).
     Keep exactly one heading of this form; a port or sync moves its
     commit id, and runs `make upstream-parity-full` against the new one. -->

## Upstream head the fork is at parity with: `9e48141b` (2026-10-02)

Upstream master moved from `8e7a1ac4e` to `cea2b4d83` with
[Netflix/vmaf#1653](https://github.com/Netflix/vmaf/pull/1653), three commits
on SpEED:

| Upstream commit | What | On the fork |
| --- | --- | --- |
| `76ea5f03` | `speed`: the scalar anti-alias filter and the 16x decimation fused into `vif_filter1d_dec16_s()`, called on every target but x86 | **Ported.** Bit-identical to `vif_filter1d_s()` + `vif_dec16_s()` in `core/test/test_speed_filter.c` (GCC 16.1 and clang 22.1 for aarch64 under `qemu-aarch64`, GCC on x86); x86 reports are byte-identical before and after |
| `cea2b4d8` | checkasm case for the fused filter | **No checkasm tree here.** Its sizes and layouts are rows of `test_speed_filter.c` |
| `15297286` | `arm64`: NEON covariance kernel for SpEED | **Not ported: SIMD not bit-exact.** Eight partial sums with fused multiply-adds against the scalar kernel's one running sum: 4061 of 18480 sums differ in the last bits, by up to 3.5e-12 relative (upstream tests it to 1e-10). Not an upstream defect; the fork's SIMD kernels have to return the scalar's bits. The fork has its own NEON kernel instead, which keeps one lane per covariance sum and is bit-identical ([ADR-1459](../adr/1459-speed-cov-kernel-exact.md)); upstream's AVX2 and AVX-512 kernels (`30f472b14`) left the tree with the same decision |

`docs/rebase-notes.md` has the mechanics and the measurements. Upstream branch
`speed-fused-avx2` landed on 2026-10-07 as `ad42c532` (the fused filter on x86
too) and `9cb9479f` (its AVX2 vertical pass); both are ported, with x86 scores
byte-identical before and after at every dispatch level. The heading above
stays at `9e48141b` until the upstream commits between it and `9cb9479f` are
all in.

`9e48141b` ("adm: add NEON scale-zero decoupling",
[Netflix/vmaf#1656](https://github.com/Netflix/vmaf/pull/1656)) followed on
2026-10-02 and is **ported**: `adm_decouple_neon` returns the scalar kernel's
bits at every enhancement gain limit (integral limits on the vector path,
fractional limits on the scalar kernel, so ADR-1413's truncated product holds).
The measurements are in `docs/rebase-notes.md`.

Upstream master moved to `9cb9479f2` on 2026-10-07. The pin stays at
`9e48141b` until the port pull requests land. New since the pin: 33e5f0aca
and cffd5b77d (merge callback and `adm_norm_view_dist_extra`; a new CPU option
the parity guard's option matrix has not seen), ad42c5320 and 9cb9479f2
(SpEED, above), 8bc5a5c6a and b41d2340a (arm64 ADM, see the arm64 note in
`docs/rebase-notes.md`), the MSVC series and the HDR groundwork.

Upstream master moved to `700124a4c` on 2026-10-09: `9f4bd165f` (integer VIF
copied a whole stride per row) is **ported** with a guard-page test;
`4068ee3b5` (CAMBI 10-bit same-size copy ignored the stride) was **already
fixed** here (`T-CAMBI-10BIT-FULLREF-WIDE-SOURCE-ROWS-2026-10-05`);
`700124a4c` (`vmaf_picture_wrap()`) is ported in its own pull request.

`8bc5a5c6a` ("adm: add NEON scale-zero contrast masking") and `b41d2340a`
("adm: extend NEON processing across scales") are
**ported in the fork's form**: every NEON kernel returns the fork's scalar
bits, including the int32 centre tap and int64 excess of ADR-1402, which
upstream's NEON narrows to int16 as upstream's scalar does. No upstream
defect was found on the way.

## Reported upstream on 2026-09-19

Six defects found on `86da14d0` while validating the pull requests above were
reported on 2026-09-19, each reproduced on upstream first. The right-hand
column is this fork's own status, checked against the fork's tree the same day
rather than inferred from upstream's.

| Upstream | What | This fork |
| --- | --- | --- |
| [#1603](https://github.com/Netflix/vmaf/pull/1603) (PR) | checkasm's `check_adm_dwt2` passes a byte stride where `adm_dwt2_16()` indexes samples, and a second site passes a band stride as the source stride; ASan: heap over-read | **Not affected** — the fork carries no `checkasm` tree |
| [#1604](https://github.com/Netflix/vmaf/pull/1604) (PR) | The direct YUV and y4m readers read floor-sized chroma rows where the file stores ceil-sized ones, and `fetch_picture()` returns `!ret`, turning a reader error into "usable picture" and a crash | **Not affected** on both counts: `picture_compute_geometry()` allocates ceiling chroma, `USE_DIRECT_READ` is never defined so the buffered reader runs, the CLI refuses odd 4:2:0 dimensions outright, and `finish_unread_picture()` maps errors to `-1`. The one piece upstream left open — two failed reads classified as a clean end of stream, and every read failure exiting 0 — **was live here** and is fixed by ADR-1262 |
| [#1605](https://github.com/Netflix/vmaf/pull/1605) (PR) | AVX2 `get_best15_from32()` shifts by a negative count on every lane before the blend discards it | **Not affected** — the AVX2 helper has returned early below 32768 since PR #792; scalar, AVX-512, CUDA, HIP, Metal and SYCL guard at the call site. The fork closed this pull request on 2026-09-21 as superseded by #1584 (open); the same change is #1635 (open) |
| [#1606](https://github.com/Netflix/vmaf/pull/1606) (PR) | A zero-length variable-length array when `--no_prediction` leaves `model_cnt` at 0 | **Not affected** — `ModelArrays::allocate()` returns before allocating when the count is 0 (ADR-0809) |
| [#1607](https://github.com/Netflix/vmaf/issues/1607) (issue) | Frames of 16 px and below crash integer ADM: `(uint32_t)ceil(log2(w) - 4)` converts a negative double, and `h_half - 2` underflows an unsigned bound in `dwt2_src_indices_filt()`. Upstream #1599 and #1600 do not fix it | **Not affected** — the extractor refuses the input with `integer_adm requires width >= 17 and height >= 17` instead of running; measured at 8, 12 and 16 px. Whether to refuse or support such frames is the decision upstream was asked to make |
| [#1608](https://github.com/Netflix/vmaf/issues/1608) (issue) | The SIMD `adm_cm` narrows `accum_h` to `float` before dividing where its siblings and scalar do not | **Same cast present** (`adm_avx512.c`), **no effect**: the divisor is an exact power of two, all 108 values over the three reference pairs are bit-identical, and 2,000,000 random integers in $[2^{53}, 2^{62})$ show no difference |

The earlier note on the last item — that the cast "likely explains upstream's
1e-4 checkasm tolerance" — was wrong and is withdrawn: instrumenting the three
tolerance sites over a full `checkasm --test=adm` run gives 90 comparisons, 87
exactly equal, and a worst relative deviation of `1.910e-07`, roughly 500 times
inside the tolerance.

---

## Integer ADM vector decouple rounds the gain-limited sample — fixed in this fork

Found 2026-10-01; present on `upstream/master` `6ec23e8f2`; not reported
upstream.

The scalar decouple stores `MIN(rst * adm_enhn_gain_limit, t)` (or `MAX`), a
double, in an integer, which truncates toward zero. Upstream's vector kernels
convert the product with rounding conversions: `_mm256_cvtpd_epi32` in
`adm_decouple_avx2` (`adm_avx2.c:854`), `_mm512_cvtpd_epi32` in
`adm_decouple_avx512` (`adm_avx512.c:964`) and `_mm512_cvtpd_epi64` in
`adm_decouple_s123_avx512` (`adm_avx512.c:1407`). With an integral limit (the
default 100, the 1 of the NEG models) the product is integral and nothing
shows. With a non-integer limit the vector result is one off wherever the
product has a fraction of one half or more.

Reproduce on any x86 host:

```bash
vmaf -r src01_hrc00_576x324.yuv -d src01_hrc01_576x324.yuv -w 576 -h 324 -p 420 -b 8 \
     --feature 'adm=adm_enhn_gain_limit=1.2' --json -o simd.json
vmaf ... --feature 'adm=adm_enhn_gain_limit=1.2' --json -o scalar.json --cpumask 4294967295
```

`integer_adm_scale0` differs by up to 1.2e-6 per frame on that pair, 6.2e-6 on
the 352x288 `akiyo` pair and 3.5e-5 on blurred blocks.

**Fix applied in this fork:** the truncating conversions (`_mm256_cvttpd_epi32`,
`_mm512_cvttpd_epi32`, `_mm512_cvttpd_epi64`), so every dispatch level returns
the scalar's sample. See
[ADR-1413](../adr/1413-adm-gain-limit-truncated-double-product.md). Until
upstream changes its kernels, the fork's AVX2 / AVX-512 output under a
non-integer limit equals upstream's scalar output, not upstream's vector
output.

## `adm_decouple_s123_avx512` LTO+release SEGV — fixed in this fork

**Status:** fixed in this fork (PR #69 follow-up commit), still present
upstream.

**Symptom:** `test_pic_preallocation` aborts with
`AddressSanitizer: SEGV on unknown address` inside
`adm_decouple_s123_avx512` when the binary is built with
`--buildtype=release -Db_lto=true -Db_sanitize=address`. The debug
ASan build used by CI (`--buildtype=debug -Db_lto=false`) does not
reproduce the crash.

Reproduce with:

```bash
meson setup build-asan-lto core \
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
`_Alignas(64)`. The unaligned load remains correct, and the LTO-promoted
aligned form is now also correct. The array has since left the tree: the
`angle_flag` predicate is computed by the shared helpers in
`core/src/feature/adm_angle_flag.h`, and `adm_avx512.c` holds no such stack
array.

**Related issue surfaced during triage:**
`test_picture_pool_basic`, `test_picture_pool_small`, and
`test_picture_pool_yuv444` loaded a `VmafModel` via
`vmaf_model_load` and never called `vmaf_model_destroy`, so
LeakSanitizer reported 208 bytes direct + 23 KiB indirect leaks per
test. Pairing `vmaf_model_destroy(model)` with each load is also
landed in PR #69 (same commit).

---

## Integer AIM is not clipped at 1, float AIM is — kept as upstream has it

Found 2026-10-01; present on `upstream/master` `6ec23e8f2`; not reported
upstream.

The two ADM extractors finish the AIM ratio differently:

```c
/* upstream libvmaf/src/feature/integer_adm.c:3006-3007 */
// normalize AIM score by the DLM denominator
*score_aim = aim_num / den;

/* upstream libvmaf/src/feature/adm.c:322-323 */
// normalize AIM score by the DLM denominator and clip values larger than 1
*score_aim = MIN(aim_num / aim_den, 1.0f);
```

On a reference without detail the denominator is the noise floor alone, and
any visible additive impairment takes the ratio above 1. Upstream prints
`integer_aim` 3.175585 and `aim` 1.000000 for a flat grey 64x64 reference
against the same picture with isolated 4x2 patches; `adm3` follows
(`integer_adm3` 0.0, `adm3` 0.5 at the default weight).

It is not known which line upstream intends. The shipped `vmaf_v1.0.16` models
read the integer feature. **The fork keeps both lines as they are**
([ADR-1417](../adr/1417-integer-aim-unclipped-upstream-parity.md)) and
documents the two ranges in [features](../metrics/features.md#aim-above-1);
`test_integer_adm_aim_unclipped` pins both. If upstream adds the clip to the
integer extractor, port it and change that test in the same PR.

## ADM viewing-distance merge (`cffd5b77d`) — two gaps fixed in this fork

Found 2026-10-08 while porting Netflix/vmaf `33e5f0aca` and `cffd5b77d`;
present on `upstream/master` `9cb9479f2`; not reported upstream.

Upstream folds an `adm` context that differs only in `adm_norm_view_dist` into
a registered one as its second distance (`adm_try_merge_view_dist()`). Two
cases go wrong, both measured on the 576x324 test pair with the shipped
`vmaf_v1.0.16` models, upstream built at `9cb9479f2` and at its parent
`cffd5b77d~1`:

| Case | Upstream before the merge | Upstream `9cb9479f2` | This fork |
| --- | --- | --- | --- |
| `--model 3d0h --model 5d0h --model 5d0h` (second name) | exit 0 | exit 234: the third model's `adm` is declined (the context already has a second distance) and registered apart; both write `integer_adm2_..._nvd_5` and the collector refuses the second write | exit 0; the 216 values equal upstream's before the merge |
| `--model 3d0h --feature adm=<5d0h options>:debug=true` | 10 `integer_adm_num*` / `_den*` scores | none: the debug context is folded in and only the first distance's debug scores exist | 10, as before the merge |

**In the fork** ([ADR-2795](../adr/2795-adm-shared-viewing-distances.md)):
`adm_merge_view_dist()` absorbs a context at the distance the existing one
already evaluates second, and declines one with `debug` set. The two-model run
(`3d0h` + `5d0h`) scores the same 1352 values as upstream master.
`test_adm_view_merge` pins both rules.

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
