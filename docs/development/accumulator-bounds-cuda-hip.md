<!-- markdownlint-disable MD001 MD004 MD013 MD024 MD029 MD032 MD036 MD060 -->

# Accumulator bounds: CUDA and HIP

Appendix of [integer accumulator bounds](accumulator-bounds.md): every integer accumulator, size product and offset of the CUDA and HIP feature twins, read on master `571565a47` (before the fixes the main page lists). Rows marked OVERFLOW or DEPENDS name their state row on the main page.

- Source: master `571565a47`.
- Mode: read only. Nothing was edited, built or run on a device. The only commands were `grep`, `sed` and `cat`, plus a few `python3` bound computations. The ADM replay is `scripts/dev/adm_cm_row_bound.py`.
- Scope:
  - every file under `core/src/feature/cuda/` and `core/src/feature/hip/` (130 files);
  - the shared `core/src/feature/*.h` headers those files include for GPU-side integer math;
  - `core/src/cuda/*.c`, `core/src/hip/*.c` and `core/src/cuda/cuda_helper.cuh`, for size products only.
- Envelope:
  - 16K: W ≤ 15360, H ≤ 8640, N = 132,710,400.
  - 8K DCI: 8192x4320.
  - Cap: W, H ≤ 32768, N ≤ $2^{30}$.
  - Samples up to 16 bit, 4:4:4.
  - Worst-case content: max-difference frames or constructed extremal patterns.

## How the audit was done

The audit was split into eight feature groups. Each group read every file of its group in full and derived every bound from the code it read. Each group reports its own constants and derivations. Every row that is not SAFE was then re-checked every row that is not SAFE against the source, and spot-checked a set of SAFE rows (listed at the end of this section).

- Feature sections: every feature has CUDA and HIP subsections.
- Integer ADM is the exception. Its HIP kernels are line-for-line ports of the CUDA kernels, so each ADM row cites both files and is tagged "CUDA …; HIP …".
- Rows on shared headers sit in their own subsection of the feature that uses them.

## Verdict totals (743 rows)

| verdict | rows |
|---|---|
| SAFE | 730 |
| OVERFLOW@16K | 2 |
| OVERFLOW@CAP-ONLY | 3 |
| DEPENDS | 8 |

Two verdicts changed from the first draft, both in integer ADM:
- **Scale-0 CM row total: DEPENDS → OVERFLOW@16K.** The envelope covers every W ≤ 15360, and W = 63–64 wraps at default options.
- **Scale-0 CM frame accumulator: DEPENDS → SAFE.** The frame sum cannot wrap for any int64 row value.

## Every row that is not SAFE

### OVERFLOW@16K

1. \*\*Integer ADM decouple, scales 1-3: double → int32 out of range (CUDA and HIP).\*\*
   - Location: `cuda/integer_adm/adm_decouple_inline.cuh:160`, `hip/integer_adm/adm_decouple_inline.hip:163`.
   - Expression: `int32_t rst = (int32_t)(((k * o_val) + 16384) >> 15) * adm_enhn_gain_limit;`
   - The `(int32_t)` cast binds to the shifted product only. The multiplication by the `double` gain yields a `double`, which is implicitly converted to `int32_t` \*before\* `min(rst, t_val)` / `max(rst, t_val)`.
   - Bound: |r| ≤ |o| ≤ 1.449e9 at scale 1 and ≤ 7.5e8 at scales 2-3. Those are the DWT band maxima; they agree with `adm_csf_fixed_point.h:88-90`.
   - The default gain is 100 (`adm_options.h:40`). With angle_flag set, |r| > 21,474,837 produces a `double` outside the int32 range, which is UB under C++ [conv.fpint].
   - This does not depend on frame size. It needs only a band value above about 1.5 % of the scale-1 maximum and a dis band within 1° of the ref band, which is ordinary content on high-quality encodes.
   - The CPU (`integer_adm_kernels.h:380-385`) takes `MIN(rst * gain, t)` in double first, which is defined.
   - The GPU scores match the CPU only because the hardware conversions saturate: `INT32_MAX` gives `min(INT32_MAX, t) = t`. This is consistent with the existing bit-exact parity results, but it is not guaranteed under LLVM (`fptosi` out of range is poison).
   - Fix: `rst = (int32_t)fmin((double)r * gain, (double)t)` and the mirrored `fmax`, as the CPU does.
2. \*\*Integer ADM scale-0 contrast-masking row total, int64 (CUDA, HIP, and the CPU reference).\*\*
   - Locations:
     - CUDA `adm_cm.cu:561, 894` (`accum_row`), `:515` (`warp_reduce`), `:519`.
     - HIP `adm_cm.hip:440, 719`, `:457`, `:465`.
     - CPU `integer_adm_kernels.h:1017-1019, 1079-1099`.
   - Term: `((x² + 2^28) >> 29) · x >> (ceil(log2 Wb) − 4)`, where Wb = W/2. The shift uses the band width (`integer_adm.c:711` passes `w2`; `integer_adm_kernels.h:989`).
   - Worst case: ref == dis, so a = 0 and thr = 0, and x = |band| · weight. With 4 pixel rows (0, 0, max, 0), the exact column-sign maximum comes from the DP in `scripts/dev/adm_cm_row_bound.py`. It was re-run for this page.
   - Row maximum as a fraction of INT64_MAX at the default Watson weights:

     | W | row max / INT64_MAX |
     |---|---|
     | 48 | 0.807 |
     | 56 | 0.878 |
     | \*\*64\*\* | \*\*1.021 (wraps)\*\* |
     | 128 | 0.973 |
     | 1080p (1920) | 0.857 |
     | 8K DCI | 0.912 |
     | 15360 | 0.855 |
     | 16384 | 0.912 |
     | cap | 0.912 |

   - The 8-bit maximum at W = 64 is 1.009 and the 10-bit maximum is 1.018.
   - W ≥ 17 is accepted (`ADM_MIN_FRAME_DIM`, `adm_csf_fixed_point.h:277`), so W = 63–64 runs.
   - With non-default CSF weights the row also overflows at 16K and at the cap: h/v ≥ 38,406 or d ≥ 60,965 at 16K, h/v ≥ 37,588 at the cap. Those weights are reachable through `adm_csf_scale` / `adm_csf_diag_scale` / the viewing-geometry options.
   - The bound is not monotonic in W, because the normalising shift is ceil(log2 Wb).
   - The CPU has the same signed int64 sum, so all backends share the defect: signed-overflow UB on the CPU, and a two's-complement wrap on the GPUs.

### OVERFLOW@CAP-ONLY

3. \*\*HIP PSNR-HVS prefix scan silently stops at 32,768 chunks (`hip/integer_psnr_hvs/psnr_hvs_score.hip:462-471`).**
   - Code: `limit = num_chunks < 32768u ? num_chunks : 32768u`.
   - num_chunks = ceil(total_blocks / 256), and total_blocks = Σ planes ((w−8)/7+1)·((h−8)/7+1) (`integer_psnr_hvs_hip.c:389-393`).
   - 16K 4:4:4: 3 · 2194 · 1234 = 8,122,188 blocks, so 31,728 chunks. That is under the cap, with a 3.2 % margin.
   - The cap is passed above 8,388,608 blocks, for example:
     - 16384x8640 4:4:4: 8,662,680 blocks.
     - luma-only frames above about 20.3K x 20.3K.
     - cap 4:4:4: 256,779 chunks.
   - Above that, `chunk_offsets[c ≥ 32768]` are never written. `d_scratch` is a `hipMalloc` buffer that is never cleared, so `hvs_compact_hip` writes `packed_terms + chunk_offsets[chunk] + intra` from stale or garbage offsets. The results are out-of-bounds device writes, a short `total_terms`, and wrong scores.
   - The CUDA twin (`psnr_hvs_score.cu:443`) scans every chunk.
   - The `uint32_t` sum itself is safe: ≤ 4,207,058,112 at the cap.
4. \*\*CUDA SpEED covariance divisor rounded to fp32 (`cuda/speed/speed_score.cu:1219`).**
   - Code: `static_cast<float>(g.sub_w * g.sub_h)`.
   - This is not a wrap: the `uint32_t` product is at most 67,010,596. The problem is the fp32 exact-integer range, $2^{24}$.
   - The count is exact at 16K for every option value: the largest is 3836 · 2156 = 8,270,416 at `speed_prescale` 4.
   - Above 16K with a prescale above about 2, the count can be inexact. For example, W = H = 32740 at prescale 4 gives sub 8181, and 8181² = 66,928,761 is odd and above $2^{24}$.
   - The CPU (`speed.c:847`) divides the double sum by the exact double count. The twin is then no longer bit-exact.
   - The mean divisor (`:1183`) rounds the count the same way the CPU's `compute_mean` (`speed.c:775`) does, so it is SAFE.
5. \*\*HIP SpEED covariance divisor (`hip/speed/speed_hip_device.h:624`).\*\* Same defect as row 4.

### DEPENDS

6. \*\*CUDA integer PSNR clip-level `apsnr_sse[p] += sse` (`cuda/integer_psnr_cuda.c:394`), `uint64_t`.**
   - One plane's frame SSE ≤ N · 65535² per frame.
   - Frames to wrap at 16-bit max-diff:

     | resolution | frames to wrap |
     |---|---|
     | 1080p | 2,072 |
     | 8K DCI | 122 |
     | 16K | 33 |
     | cap | 5 |

   - These come from $2^{64}$ / (N · 4,294,836,225) = 2071.4, 121.4, 32.4 and 4.0.
   - At 12 bit the counts are 530,502 / 31,085 / 8,290 / 1,025.
   - At 10 bit they are 8.5M / 498,076 / 132,821 / 16,417.
   - At 8 bit they are 136.8M / 8.0M / 2.1M / 264,205.
   - The wrap is silent, and apsnr comes out too high. CPU `integer_psnr.c` uses the same `uint64_t`.
7. \*\*HIP integer PSNR `apsnr_sse[p] += sse` (`hip/integer_psnr_hip.c:476`).\*\* Same as row 6.
8. \*\*HIP float PSNR 16bpc kernel at bpc 10/12 (`hip/float_psnr/float_psnr_score.hip:151,161,166`), `uint32_t` block sum of 256 float squares.**
   - In-range samples give 256 · 4095² = 4,292,870,400, which is 2,096,895 below UINT32_MAX.
   - uint16 codewords above $2^{\mathrm{bpc}} - 1$ can wrap the sum. Nothing in `picture.c` / `libvmaf.c` checks samples against bpc.
   - At 16 bit the kernel splits each square into 16-bit halves, so that case is safe.
9. \*\*HIP integer VIF horizontal `accum_mu1/mu2 += coeff * tmp.mu1[...]`, `uint32_t` (`hip/integer_vif/vif_statistics.hip:499-500, 530-534`).**
   - The HIP vertical pass stores `tmp.mu1` without the `(uint16_t)` truncation that the CPU (`integer_vif.c:450-451`) and CUDA (`filter1d.cu:603-604`) apply.
   - For bpc 9-15 with samples above $2^{\mathrm{bpc}} - 1$, the stored value reaches 8,388,480, and the 17-tap sum reaches 5.5e11. That wraps, and HIP then differs from the CPU.
   - In-range samples are SAFE: ≤ 4,294,901,760.
10. \*\*HIP integer VIF `vif_downsample_store` `accum_ref_rd` (`hip/integer_vif/vif_statistics.hip:506-507, 329-336`).\*\* Same cause as row 9: `ref_convol` is not truncated.
11. \*\*CUDA motion batch-boundary frame counter (`cuda/integer_motion_cuda.c:830`, `:533-534`, `:545`).**
    - Code: `last_batch_boundary = (int)index`.
    - Index $2^{31} - 1$ ≡ 7 (mod 8) is a boundary, so `last_batch_boundary + 1` is signed overflow (UB) at the flush.
    - Beyond $2^{31}$, `(int)index` is an implementation-defined narrowing.
    - It needs ≥ 2,147,483,647 frames: 414 days at 60 fps.
12. \*\*Integer ADM `flt = (4369·|i16| + 2048) >> 12` narrowed to int16 (CUDA and HIP).**
    - Locations: CUDA `adm_csf.cu:171`, `adm_cm.cu:844-847`; HIP `adm_csf.hip:193`, `adm_cm.hip:658`.
    - It wraps negative once |i16| ≥ 30,720, which needs an h/v CSF weight ≥ 43,900. The default is 36,453, which gives at most 27,212.
    - The CPU (`integer_adm_kernels.h:488-489`) narrows the same way.
    - It depends on options, not on frame size.
13. \*\*Integer ADM scale-0 `x_sq = (int32_t)((x*x + 2^28) >> 29)` (CUDA `adm_cm.cu:218`, HIP `adm_cm.hip:150`).**
    - SAFE while thr ≥ 0.
    - It wraps only when row 12's negative int16 makes thr negative, so it also depends on options.

## Asides (not integer overflow; found during the reading, worth a ticket)

- **A1 (CUDA `float_motion`, out-of-bounds read): verified.**
  - `fm_mirror()` (`cuda/float_motion/float_motion_score.cu:43-50`) reflects once without a clamp.
  - `fm_load_tile()` (`:85-90`) loads the full 20x20 tile for every block. For W or H in {3..9, 17}, an index goes negative (down to −13) and the load reads before the plane.
  - The HIP twin and the CUDA motion SAD kernels clamp through `vmaf_*_tile_index()`.
- **A2 (CUDA integer VIF, full-mask shuffle inside a divergent branch).**
  - `vif_hori_flush_accums()` → `warp_reduce()` runs `__shfl_down_sync(0xffffffff, …)` under `if (y < h && x_start < w)` (`cuda/integer_vif/filter1d.cu`, the kernel body around :513-535).
  - Lanes past the plane edge do not take part, which is undefined for a full mask.
  - The code shape is verified. The effect on hardware is not.
- **A3 (`core/src/cuda/cuda_helper.cuh:129-130`).\*\* `(x >> 32) << 32` on a negative `long long` is UB before C++20.
- **A4 (stale comments).** `hip/float_psnr/float_psnr_score.hip:107-111,139-141` describe a `partials[2*block]` layout, but the code writes one u64 per block.
- **A5 (outside scope, CPU).**
  - `third_party/xiph/psnr_hvs.c:311-317`: the `int` index reaches exactly INT_MAX for a >8-bit 32768² plane.
  - `integer_motion.c:193/243`: the `uint32_t row_sad` wraps for out-of-range samples (bpc 9, W ≥ 513).
  - `sycl/speed_sycl_pipeline.cpp:808`: the same fp32 covariance divisor as rows 4-5.
- **A6 (memory footprint only, no wrap).**
  - HIP integer ADM `tmp_accum` takes 24 B/pixel: 3.2 GB at 16K and 25.8 GB at the cap.
  - CUDA `vif_cuda` allocates 7 planes that no kernel reads: 3.2e9 B at 16K.
  - The integer SSIM twins need 7 · $2^{33}$ B at the cap.
  - SpEED at prescale 4 needs about 34 GB at 16K.
  - Every one of these is a `size_t` product, so allocation fails cleanly.
- **Common root of rows 8-10.** No libvmaf entry point checks that samples are ≤ $2^{\mathrm{bpc}}$ − 1. The CPU truncates or uses wide types in places where HIP does not.

## Verification log

- **Row 1:**
  - Read `adm_decouple_inline.cuh:125-170` and `adm_decouple_inline.hip:150-170`.
  - Read the CPU `integer_adm_kernels.h:355-385`.
  - Confirmed the gain default 100.0 at `adm_options.h:40`.
- **Row 2:**
  - Re-ran `scripts/dev/adm_cm_row_bound.py`; the output is the table in row 2.
  - Confirmed the term and shifts: `adm_cm.cu:216-220`, `:500-561`; `integer_adm_kernels.h:989-1003`.
  - Confirmed the band width `w2` (`integer_adm.c:711`) and the minimum frame size 17 (`adm_csf_fixed_point.h:277`).
  - For the frame accumulator: region rows ≤ 0.875 · $2^{\lceil \log_2 H_b \rceil}$, with the maximum at Hb = 16 or 32. So |Σ (row >> s)| < 0.875 · $2^{63}$ for any int64 row value.
- **Row 3:**
  - Read `psnr_hvs_score.hip:430-526` and `integer_psnr_hvs_hip.c:169-189`, `:375-394`.
  - Recomputed the 16K block and chunk counts.
- **Rows 4-5:**
  - Read `speed_score.cu:1175-1222` and `speed_hip_device.h:545-628`.
  - Read the CPU `speed.c:766-776`, `:835-850`, `:1285-1317`, and the option range `speed.c:1518-1523` (`speed_prescale` 0.1..4.0).
  - Recomputed sub at 16K at prescale 4: 3836 x 2156.
  - Found that 32768 at prescale 4 gives 8186² = 67,010,596, which is a multiple of 4 and so exact in fp32. Only some sizes above $2^{24}$ are inexact, for example 32740.
- **Rows 6-8:**
  - Read `integer_psnr_cuda.c:380-400`, the `apsnr_sse` sites in both twins, and `float_psnr_score.hip:1-181`.
  - Recomputed the wrap frames.
- **Rows 9-10:** read `vif_statistics.hip:455-540` against `filter1d.cu:585-612`.
- **Row 11:** read `integer_motion_cuda.c:525-550`, `:822-831`, and grepped both trees for other `(int)index` narrowings (none).
- **Spot-checked SAFE rows, each read in the source:**
  - CUDA `psnr_score.cu` (uint64 SSE, int64 diff, one atomic per block).
  - CUDA `motion_v2_score.cu`: |h| ≤ 65535; SAD ≤ N · 65535 = $2^{46}$ at the cap.
  - CUDA `moment_score.cu`: u64 sums; ref2 ≤ $2^{62}$ at the cap.
  - SSIMULACRA 2 CUDA `unsigned` index `2u * pixels + i`: ≤ 3 · $2^{30} - 1$ < $2^{32}$.
  - HIP SpEED `(uint32_t)plane_bytes`: $2^{31}$ at the cap, which fits.
  - CAMBI histogram cells: ≤ 65 · 65.
  - The runtime picture allocators: all `size_t`.

## Section index

| feature | CUDA rows | HIP rows |
|---|---|---|
| Integer ADM | G1 tables, "CUDA …" half of each row | G1 tables, "HIP …" half of each row (the arithmetic is identical) |
| Integer VIF, float VIF, float ADM | G2 "integer VIF: CUDA", "float VIF/ADM" (rows tagged CUDA) | G2 "integer VIF: HIP", "float VIF/ADM" (rows tagged HIP) |
| Motion, motion_v2, float_motion | G3a "… - CUDA" sections | G3a "… - HIP" sections |
| PSNR, float PSNR, moment, CIEDE, tile index | G3b "### CUDA" subsections | G3b "### HIP" subsections |
| SSIM, float SSIM, MS-SSIM | G4a "### CUDA" subsections | G4a "### HIP" subsections |
| SSIMULACRA 2, ordered_sum.h | G4b "CUDA twin" | G4b "HIP twin" |
| CAMBI, PSNR-HVS | G5 "### CUDA" subsections | G5 "### HIP" subsections |
| SpEED | G6 "CUDA pipeline host / kernels" | G6 "HIP pipeline host / device math" |
| Runtime picture and buffer sizes | G7 `core/src/cuda` rows | G7 `core/src/hip` rows |

---

## G1: integer ADM, CUDA and HIP twins (integer-overflow audit)

Repo: master at `571565a47`. All paths are under `core/src/feature/`.
Read-only. Nothing was built or run on a device. The bounds come from the code, the
composite-filter bounds per DWT scale of `core/src/feature/adm_csf_fixed_point.h` (held by
`test_integer_adm_cm_budget`), and `scripts/dev/adm_cm_row_bound.py` (the exact maximum of a scale-0 CM row over every column sign
pattern, found by dynamic programming over the overlapping 4-tap windows).

Constants used by every row:

- **DWT taps.** lo = {15826, 27411, 7345, -4240} and hi = {-4240, -7345, 27411, -15826}. Each
  has an absolute sum of 54822. The lo taps sum to 46342 (`integer_adm.h:187-191`; the GPU
  copies are in `adm_fixed_parameters()` and `adm_hip_fixed_params()`).
- **Band maxima** (half the absolute sum of the composite filter; they agree with
  `adm_csf_fixed_point.h:88-90`):
  - scale 0: every band ≤ 22,929.8.
  - scale 1: band_a ≤ 1,491,390,732 and h/v/d ≤ 1,448,979,042.
  - scale 2: ≤ 751,510,749.
  - scale 3: ≤ 748,642,077.
  - Every maximum is below INT32_MAX. They are per sample and do not depend on frame size.
- **CSF weight budgets** (ADR-1472, `adm_csf_fixed_point.h:118-127`):
  - scale 0 h/v: < 46,603.4.
  - scale 0 d: < 65,536 (the storage limit).
  - scale 1: < 279,958,309; scale 2: < 539,893,111; scale 3: < 546,406,567.
  - Defaults (Watson97, 3H, 1080): 36,453 for h/v and 49,417 for d (tabulated).
  - The options `adm_csf_mode` and `adm_csf_scale` / `adm_csf_diag_scale` (continuous doubles,
    0..50, applied multiplicatively in `barten_csf_tools.h:159-160`) can place a weight anywhere
    below its budget. So can a non-default `adm_norm_view_dist` or `adm_ref_display_height`.
- **Region.** `cols = Wb - 2*int(0.1*Wb - 0.5)`, where Wb is the band width (W/2 at scale 0).
  - 16K: Wb = 7680, cols = 6146.
  - 8K DCI: Wb = 4096, cols = 3278.
  - cap: Wb = 16384, cols = 13110.
  - Scale-0 rows at 16K: Hb = 4320, so 3458 rows.

### integer_adm: DWT (scale 0 fused kernel, scales 1-3 combined kernels)

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| CUDA `cuda/integer_adm/adm_dwt2.cu:227-232`; HIP `hip/integer_adm/adm_dwt2.hip:242-247` | `accum_lo` / `accum_hi`, 8-bit instantiation (`DwtVertAccum<uint8_t>`) | `int32_t` | 4 taps × sample 255; positive lo taps sum to 50582, then `- 46342*128` | ≤ 50582·255 = 12,898,410 | same (per sample) | SAFE ($2^{23.6}$ < $2^{31}$) |
| CUDA `adm_dwt2.cu:227-232`; HIP `adm_dwt2.hip:242-247` | `accum_lo` / `accum_hi`, 16-bit instantiation (`DwtVertAccum<uint16_t>`) | `int64_t` | 4 taps × 65535 | ≤ 50582·65535 = 3.31e9 | same | SAFE ($2^{31.6}$ < $2^{63}$; the former int32 overflow is fixed, see state row T-GPU-ADM-DWT2-16BIT-INT32-OVERFLOW) |
| CUDA `adm_dwt2.cu:236`; HIP `adm_dwt2.hip:251` | `(short)((accum + v_add_shift) >> v_shift)` narrowing of the normalised vertical output | `int64_t` / `int32_t` → `short` | Σc·(p−$2^{b-1}$)/$2^{b}$; abs-sum bound 0.5·54822 | ≤ 27,411 (16-bit: lo max 27411, min −27412) | same | SAFE (< 32767) |
| CUDA `adm_dwt2.cu:158-166` (`dwt_tap4`), used at `:281-293`; HIP `adm_dwt2.hip:173-181`, `:296-308` | `accum` of the scale-0 horizontal pass; `filter[k]*s` with int32 operands | `int32_t` | 4 taps × int16 tile value (≤ 27,412) | ≤ 54822·27412 + 32768 = 1.5028e9 (composite: band 22,930 · $2^{16}$) | same | SAFE (0.70·INT32_MAX) |
| CUDA `adm_dwt2.cu:73-83`; HIP `adm_dwt2.hip:88-98` | `accum_lo` / `accum_hi` of the s123 vertical pass | `int64_t` | 4 taps × int32 band_a (≤ 1.4914e9) | ≤ 54822·1.4914e9 = 8.2e13 | same | SAFE ($2^{46}$ < $2^{63}$) |
| CUDA `adm_dwt2.cu:77,83`; HIP `adm_dwt2.hip:92,98` | `tmplo[idx]` / `tmphi[idx] = (int32_t)((accum + add) >> shift)` | `int64_t` → `int32_t` | Composite vertical value. Scale 1 (shift 0) ≤ 1,058,572,481; scale 2 ≤ 1,058,676,611; scale 3 ≤ 529,530,363 | ≤ 1.0587e9 | same | SAFE (0.49·INT32_MAX) |
| CUDA `adm_dwt2.cu:115-141`; HIP `adm_dwt2.hip:130-156` | `accum` of the s123 horizontal pass, then `(int32_t)` band stores at `:120/127/134/141` (HIP `:135/142/149/156`) | `int64_t` → `int32_t` | 4 taps × tmp (≤ 1.0587e9). Shifts: scale 1 h15, scale 2 h16, scale 3 h15, which is the CPU's `i4_dwt2_round()` (`integer_adm_kernels.h:1370-1380`) | accum ≤ 4.9e13. Band ≤ 1,491,390,732 (scale-1 band_a) | same | SAFE (accum $2^{45.5}$ < $2^{63}$; band 0.6945·INT32_MAX) |
| CUDA `adm_dwt2.cu:182`; HIP `adm_dwt2.hip:197` | `d_picture[y_in * src_stride + x]` | `int` | Row index × element stride. CUDA: pitch/2 (16-bit) or pitch (8-bit) from `cuMemAllocPitch` (`integer_adm_cuda.c:1236-1240`). HIP: a packed plane, `luma_stride = w` (`integer_adm_hip.c:1113`) | ≤ 8639·15360 + 15359 = 1.327e8 | ≤ 32767·32768 + 32767 = 1,073,741,823 | SAFE (cap 0.50·INT32_MAX) |
| CUDA `adm_dwt2.cu:68-71, 120-141, 279`; HIP `adm_dwt2.hip:83-86, 135-156, 294` | `pixel.x * img_stride + idx`, `i * dst_stride + idx`, `y_out * dst_stride + x_out` | `int` | Band row × `buf_stride` = ALIGN_CEIL(Wb·4)/4 elements | ≤ 4319·7680 + 7679 = 3.32e7 | ≤ 16383·16384 + 16383 = 2.68e8 | SAFE |
| CUDA `adm_dwt2.cu:64,102`; HIP `adm_dwt2.hip:79,117` | `static_cast<ptrdiff_t>(w) * i * 2` (scratch row offset) | `ptrdiff_t` | w × row × 2 | 1.66e7 | 2.7e8 | SAFE |
| CUDA `cuda/integer_adm/adm_dwt2_rows.h:153-187`; HIP `hip/integer_adm/adm_dwt2_rows.h:44-68` | `2*y_out - 1 + i`, `2*(y_in - h) + 1`, `(2*upper) - idx - 1` (index helpers) | `int` | Row and tap indices | ≤ 2·H ≈ 17,282 | ≤ 65,537 | SAFE |

### integer_adm: decouple and CSF (inline in the CSF / CM kernels)

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| CUDA `cuda/integer_adm/adm_decouple_inline.cuh:119-121`; HIP `hip/integer_adm/adm_decouple_inline.hip:122-124` | `ot_dp`, `o_mag_sq`, `t_mag_sq` = `(int32_t)oh*th + (int32_t)ov*tv` (scale 0). The CPU forms these in int64 | `int32_t` | Two int16 products, each ≤ 22,930² | ≤ 2·22930² = 1.0516e9 | same | SAFE (0.49·INT32_MAX; it would wrap only for \|band\| > 32,767, which the filter bound excludes) |
| CUDA `adm_decouple_inline.cuh:90, 96-97`; HIP `adm_decouple_inline.hip:93, 99-100` | `(int64_t)adm_recip_q30(o)*t`; `k*(int32_t)o`; `(int32_t)(r)*gain` (scale 0) | `int64_t`; `int32_t`; `double` → `int32_t` | recip ≤ $2^{30}$ × \|t\| ≤ 22,930; k ≤ 32768; gain ≤ 100 (option max, `integer_adm_cuda.c:766`) | 2.46e13; 7.51e8; 2.29e6 | same | SAFE |
| CUDA `adm_decouple_inline.cuh:53`; HIP `adm_decouple_inline.hip:56` | `q * (int32_t)o` in `adm_recip_q30` | `int32_t` | q ≈ $2^{30}$/o, product $2^{30}$ ± 63 (per the comment) | ≤ 1,073,741,887 | same | SAFE |
| CUDA `adm_decouple_inline.cuh:152`, `:63`; HIP `adm_decouple_inline.hip:155`, `:66` | `(int64_t)(2^30/o_msb) * t * sign_o`; `(1 << (14 + kh_shift))`; `(1 << (k - 1))` | `int64_t`; `int` | $2^{30}$ × \|t\| ≤ 1.449e9. kh_shift = 17 − clz(\|o\|) ≤ 16 because \|o\| < $2^{31}$ | 1.556e18; 1<<30 | same | SAFE (5.9x below $2^{63}$; 1<<31 UB would need \|o\| ≥ $2^{31}$) |
| \*\*CUDA `adm_decouple_inline.cuh:160`; HIP `adm_decouple_inline.hip:163`\*\* | \*\*`int32_t rst = (int32_t)(((k*o)+16384)>>15) * adm_enhn_gain_limit;` (scales 1-3)\*\* | \*\*`double` → `int32_t` (implicit)\*\* | \|r\| ≤ 1.449e9 (scale 1), ≤ 7.5e8 (scales 2-3), × gain ≤ 100, converted to int32 \*before\* `min(rst, t)`. Out of range whenever angle_flag holds and \|r\| > $2^{31}$/gain = 21,474,837 (gain 100): 1.5 % of the scale-1 band maximum, 2.9 % at scales 2-3. Ordinary edge content does this. The CPU (`integer_adm_kernels.h:380`, `rst = MIN(rst*gain, t)`) takes the minimum in double first, which is defined | r·gain ≤ 1.449e11 ≫ INT32_MAX (any frame size) | same | \*\*OVERFLOW@16K (defect, independent of frame size).\*\* The conversion is UB under C++ [conv.fpint] (LLVM fptosi gives poison). The scores match the CPU only if the device conversion saturates, in which case `min(INT32_MAX, t) = t`. The parity tests are bit-exact on RTX 4090 and gfx1036 (state row T-ADM-AIM-BARTEN), which is consistent with saturation. The PTX/ISA wording was not verified. Fix: `rst = (int32_t)min((double)r*gain, (double)t)`, as the CPU does |
| CUDA `cuda/integer_adm/adm_csf.cu:169-170`, `cuda/integer_adm/adm_cm.cu:154-155, 690-691`; HIP `hip/integer_adm/adm_csf.hip:191-192`, `hip/integer_adm/adm_cm.hip:208-209` (HIP widens to int64 there) | `dst_val = i_rfactor * (uint32_t)a_val` (wraps mod $2^{32}$, then converted to int32), then `dst_val + i_shiftsadd[band]` | `uint32_t` → `int32_t`; `int32_t` | \|a\| ≤ 22,930 × weight ≤ 65,535 (d); add ≤ 65535 | true product ≤ 1.5027e9, plus 65535 | same | SAFE (exact; < INT32_MAX) |
| CUDA `adm_csf.cu:170`, `adm_cm.cu:155,691` (int16 return); HIP `adm_csf.hip:192`, `adm_cm.hip:209` | `i16_dst_val` / `csf_a` / `csf_r` = `(dst + add) >> 15` (h/v) or `>> 17` (d), narrowed to int16 | `int32_t` → `int16_t` | h/v: 22,930 × w < 46,603.4 → ≤ 32,611. d: ≤ 11,465 | ≤ 32,611 | same | SAFE (inside the budget; 0.5 % margin for h/v) |
| CUDA `adm_csf.cu:171`, AIM `adm_cm.cu:844-847`; HIP `adm_csf.hip:193`, AIM `adm_cm.hip:658` | `flt = (4369*\|i16\| + 2048) >> 12`, narrowed to int16 (csf_f and the AIM neighbour) | `int` → `int16_t` | ≤ 4369·32,611/4096 = 34,786. It wraps negative once \|i16\| ≥ 30,720, i.e. weight·\|band\| ≥ 1.0066e9, i.e. an h/v weight ≥ 43,900 at \|band\| 22,930. Default weight 36,453 gives \|i16\| ≤ 25,509 and flt ≤ 27,212. The CPU narrows identically (`integer_adm_kernels.h:488-489`) | default: 27,212 fits | same | DEPENDS: on the CSF weight option, not on frame size. At default options it is SAFE. With a non-default h/v weight in [43,900, 46,603) (reachable through `adm_csf_scale` in Barten modes, or through viewing geometry) the int16 wraps negative, the threshold falls, and the excess can pass the ADR-1472 square budget (next table). Shared bit-for-bit with the CPU |
| CUDA `adm_csf.cu:88`, `adm_cm.cu:86, 348, 660`; HIP `adm_csf.hip:129-130`, `adm_cm.hip:198` | `(i_rfactor * int64_t(a or r)) + 2^27 >> 28` (s123 CSF), result narrowed to int32 | `int64_t` → `int32_t` | weight < 5.47e8 × \|band\| ≤ 1.449e9; budget gives a product < 1,518,500,221·$2^{28}$ | product ≤ 4.08e17; result ≤ 1,518,500,221 | same | SAFE ($2^{58.5}$ < $2^{63}$; 0.71·INT32_MAX) |
| CUDA `adm_csf.cu:89`, `adm_cm.cu:272` (`i4_cm_weight`); HIP `adm_csf.hip:132`, `adm_cm.hip:290` | `((int64_t)143165577 or 286331153 * abs(v) + INT32_MIN) >> 32` (s123 flt and centre tap) | `int64_t` → `int32_t` | coefficient ≤ 2.87e8 × \|csf\| ≤ 1.5185e9 | 4.35e17; result ≤ 1.0124e8 | same | SAFE |

### integer_adm: CSF denominator (adm_csf_den)

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| CUDA `cuda/integer_adm/adm_csf_den.cu:102`; HIP `hip/integer_adm/adm_csf_den.hip:102` | `thread_sum += ((uint64_t)t*t)*t` (scale 0) | `unsigned long long` | t = \|band\| ≤ 22,930, so t³ ≤ 1.2056e13. ceil(cols/128) terms per thread | 49 terms → 5.9e14 | 103 terms → 1.24e15 | SAFE |
| CUDA `adm_csf_den.cu:56,66`; HIP `adm_csf_den.hip:70` | `warp_reduce((int64_t)thread_sum)`, `warp_sums`, `row_total` (CUDA); shared `thread_sums` → `row_total` (HIP) | `int64_t` / `unsigned long long` | One row of the region: cols × 1.2056e13 | 6146 → 7.41e16 | 13110 → 1.58e17 | SAFE (< $2^{63}$ for the int64 cast) |
| CUDA `adm_csf_den.cu:68` + `adm_cm_accumulator.h:49`; HIP `adm_csf_den.hip:71` | `atomicAdd(&adm_csf_den[0][band], (row_total + add) >> shift_accum)`, with shift_accum = ceil(log2(area) − 20) (`integer_adm_kernels.h:648-652`) | `unsigned long long` | Σ over rows; total ≤ area·1.2056e13 / $2^{\mathrm{shift}}$ ≤ $2^{20}$·1.2056e13 = 1.264e19 at any size | area 2.125e7, shift 5 → 8.0e18 | area 1.719e8, shift 8 → 8.09e18 | SAFE (2.3x below $2^{64}$ at 16K and cap; 1.46x in the absolute bound) |
| CUDA `adm_csf_den.cu:119-120`; HIP `adm_csf_den.hip:119-120` | `((t*t + add_sq) >> shift_sq) * t + add_cub >> shift_cub` (s123) | `uint64_t` | t ≤ 1.449e9: t² ≤ 2.1e18; >>31 → 9.78e8; ×t → 1.417e18 (scale 2: 3.95e17, scale 3: 1.91e17); then >> ceil(log2 cols) | per term ≤ 1.417e18/$2^{\mathrm{shift}}$ | same | SAFE |
| CUDA `adm_csf_den.cu:56-68`; HIP `adm_csf_den.hip:70-71` | s123 row total and frame atomic | `int64_t` cast / `unsigned long long` | Row ≤ cols/$2^{\lceil \log_2 \mathrm{cols} \rceil}$ × 1.417e18 ≤ 1.417e18. Frame ≤ rows/$2^{\lceil \log_2 \mathrm{rows} \rceil}$ × row | ≤ 1.417e18 | ≤ 1.417e18 | SAFE (6.5x below $2^{63}$) |

### integer_adm: contrast masking (DLM and AIM, scale 0 and scales 1-3)

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| CUDA `adm_cm.cu:468-472, 498, 848`; HIP `adm_cm.hip:355, 369-373` | Scale-0 `thr[]` (3 angles × 8 flt + centre `(8738*\|csf\| + 2048) >> 12`) | `int32_t` | flt ≤ 34,952 and centre ≤ 69,904 per angle | ≤ 3·(8·34,952 + 69,904) = 1,048,560 | same | SAFE |
| `adm_cm_accumulator.h:74-79` (called from CUDA `adm_cm.cu:559, 893`; HIP `adm_cm.hip:438, 718`) | `magnitude - (int64_t)thr * (1 << shift)`, clamped to INT32_MAX | `int64_t` → `int32_t` | thr ≤ 1.05e6 × $2^{12}$ | 4.3e9 | same | SAFE |
| CUDA `adm_cm.cu:559`, `:814`; HIP `adm_cm.hip:438`, `:652` | `int32_t(i_rfactor * sb)` / `abs(int32_t(i_rfactor * (uint32_t)a))` (scale-0 signal) | `uint32_t` → `int32_t` | \|r\| or \|a\| ≤ 22,930 × weight ≤ 65,535 | ≤ 1.5027e9 (exact) | same | SAFE |
| CUDA `adm_cm.cu:218`; HIP `adm_cm.hip:150` | `x_sq = (int32_t)((x*x + add_sq) >> shift_sq)`, scale 0 | `int64_t` → `int32_t` | Fits only for x ≤ $2^{30} - 1$ (h/v, shift 29) or x ≤ 1,518,500,249 (d, shift 30). With thr ≥ 0: x ≤ 22,930·46,603 = 1.0686e9 (h/v) and ≤ 22,930·65,535 = 1.5027e9 (d) | within budget at default and at every weight below the limit, provided thr ≥ 0 | same | DEPENDS: SAFE while thr ≥ 0. It wraps when a negative int16 flt (row "flt narrowed to int16" above; h/v weight ≥ 43,900) lowers thr below 0. Not frame-size related |
| CUDA `adm_cm.cu:218`; HIP `adm_cm.hip:150` | `x_sq`, scales 1-3 | `int64_t` → `int32_t` | x = \|csf\| − thr with thr ≥ −27 (flt ≥ −1 per sample, ADR-0155), so x ≤ 1,518,500,249 by the budget | ≤ 2,147,483,647 | same | SAFE |
| \*\*CUDA `adm_cm.cu:561, 894` (`accum_row[]`), `:515` (`warp_reduce` row_total), `:519` + `adm_cm_accumulator.h:33`; HIP `adm_cm.hip:440, 719` (`accum_row[]`), `:457` (shared tree), `:465`\*\* | \*\*Scale-0 DLM and AIM row accumulator: per-thread partial, then the int64 row total, then `(row_total + 2^(s-1)) >> s`\*\* | \*\*`int64_t`\*\* | \*\*Term:\*\* `cm_cube(x)` = ((x² + $2^{28}$) >> 29)·x >> (ceil(log2 Wb) − 4) for h/v, and the same with shifts 30 and −3 for d.; \*\*Worst case:\*\* thr = 0, which happens when ref == dis (DLM; a = 0 makes every csf_a and flt 0) or when ref is flat (AIM; r = 0, signal = t·w, csf_r = 0). Then x = \|band\|·weight.; \*\*Terms per row:\*\* cols ≈ 0.8·Wb, so row = R·16·T with R = cols/$2^{\lceil \log_2 W_b \rceil}$ ∈ [0.47, 0.875] and T = x_sq·x. R is 0.750 at 16K and 0.800 at 8K DCI, 16384 wide and the cap.; \*\*Content:\*\* 4 pixel rows (0, 0, max, 0), so the vertical hi output is 27,411 (16-bit) or 27,304 (8-bit), and the column sign pattern is maximised exactly (`scripts/dev/adm_cm_row_bound.py`). | \*\*Default weights:\*\* h/v row max = 0.855·INT64_MAX (W = 15360), d 0.533.; \*\*Non-default weight:\*\* h/v ≥ 38,406 or d ≥ 60,965 → overflow (budget limit 46,603 gives 1.79·INT64_MAX). | \*\*Default:\*\* 0.912·INT64_MAX (8K DCI, 16384, 32768).; \*\*Non-default:\*\* h/v weight ≥ 37,588 or d ≥ 59,667 → overflow (1.91x at the limit). | \*\*OVERFLOW@16K\*\* (integrator relabel from the reader's DEPENDS: the envelope covers every W ≤ 15360 and W = 63–64 wraps at default options; the bound is not monotonic in W. Reader's note: depends on the CSF weight option and the band-width class, not on growth in frame size.); \*\*At default options:\*\* SAFE at 1080p (0.857), 16K and cap. It \*\*overflows at W = 63–64 (Wb = 32, R = 0.875): 1.021·INT64_MAX at 16-bit, 1.009 at 8-bit, 1.018 at 10-bit.\*\* W = 128 gives 0.973.; \*\*With non-default CSF weights:\*\* it overflows at every resolution, 16K and cap included.; The int64 wrap makes the band numerator negative or wrong, so powf ends in NaN or a wrong score. The CPU reference has the identical int64 `inner` (`integer_adm_kernels.h:1017-1019, 1079-1099`), where it is signed-overflow UB, so all three backends share the defect. The ADR-1472 budget bounds only the square, not the row sum. |
| CUDA `adm_cm.cu:519`; HIP `adm_cm.hip:465` | Scale-0 frame accumulator `adm_cm[0][b]` / `adm_aim_cm[0][b]` (atomicAdd on the unsigned long long alias) | `int64_t` | Σ rows of (row >> ceil(log2 Hb)); rows/$2^{s}$ ≤ 0.81 | ≤ 0.81·row max | same | SAFE (integrator relabel from DEPENDS: region rows ≤ 0.875·$2^{s}$ with s = ceil(log2 Hb) (max at Hb = 16 or 32), each term ≤ $2^{63-s}$, so the sum stays below 0.875·$2^{63}$ for any int64 row value; a wrapped row makes the value wrong but cannot make this sum wrap) |
| CUDA `adm_cm.cu:310-329, 704-737`; HIP `adm_cm.hip:324, 609` | Scales 1-3 `thr` (27 weighted terms: 8×1/30 + 2/30 per angle, × 3 angles) | `int32_t` | ≤ \|csf\|·(10/30)·3 = \|csf\| ≤ 1,518,500,221 (+27 rounding) | 1.5185e9 | same | SAFE (0.71·INT32_MAX) |
| CUDA `adm_cm.cu:386, 781` (`thread_accum`), `:281, 291` (warp_reduce, warp_sums, row_total); HIP `adm_cm.hip:842` (AIM `thread_accum`), `:632`, and the DLM two-pass path `:772` (per-pixel int32 excess) → `:550` (`temp_value`) → `:558` | Scales 1-3 DLM and AIM row accumulator | `int64_t` (HIP scratch `int32_t`) | Term = ((x²+$2^{29}$)>>30)·x >> ceil(log2 Wb), with T ≤ 2,147,483,647 · 1,518,500,249 = 3.261e18. Row ≤ cols/$2^{\lceil \log_2 W_b \rceil}$ · T < T. HIP scratch value = excess ≤ 1.5185e9 | ≤ 3.27e18 | ≤ 3.27e18 | SAFE at every size and weight (2.8x below $2^{63}$) |
| CUDA `adm_cm.cu:293`; HIP `adm_cm.hip:568, 640` | Scales 1-3 frame atomic `adm_cm[s][b]` / `adm_aim_cm[s][b]` | `int64_t` | Σ rows of (row >> ceil(log2 Hb)) | ≤ 3.27e18 | ≤ 3.27e18 | SAFE |
| HIP `adm_cm.hip:772`, `:531-532` | `(blockIdx.z*buffer_h + blockIdx.y)*buffer_stride + tid_x`; `off = band*(buffer_h*buffer_stride)` (scratch of the s123 DLM path) | `unsigned int` / `int` | 3 × scale-1 region (≈ 0.8·Wb1 × 0.8·Hb1) | 3·3074·1728 = 1.59e7 | 3·6554² = 1.29e8 | SAFE |
| CUDA `adm_cm.cu:376, 492, 556, 778, 832, 890`; HIP `adm_cm.hip:306-308, 398, 435, 676, 716, 766, 836` | CM index math `row * src_stride + x` | `int` | Hb × buf_stride | ≤ 3.32e7 | ≤ 2.68e8 | SAFE |

### integer_adm: host buffers, strides and launch geometry

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| CUDA `cuda/integer_adm_cuda.c:1666-1669, 1610, 1615, 1620`; HIP `hip/integer_adm_hip.c:1390-1403` | `ALIGN_CEIL(w*sizeof(int32_t))`, `buf_sz_one = ind_size_x*((h+1)/2)`, data_buf = 16.5·buf_sz_one, tmp = integer_stride·4·Hb; HIP adds tmp_accum = 8·3·W·H | `size_t` | Byte sizes | data_buf 2.19e9 B; HIP tmp_accum 3.19e9 B | data_buf 1.77e10 B; tmp_ref 8.6e9 B; HIP tmp_accum 2.58e10 B | SAFE (all size_t; memory footprint only) |
| CUDA `integer_adm_cuda.c:1233-1240` → int kernel arguments; HIP `integer_adm_hip.c:1236, 1113` | `size_t` stride narrowed to an `int` kernel argument | `size_t` → `int` | Elements: picture pitch/2, band buf_stride, HIP packed w | ≤ 15,360 | ≤ 32,768 | SAFE |
| CUDA `integer_adm_cuda.c:184, 201-234, 265, 387, 453, 554, 671`; HIP `integer_adm_hip.c:608, 632, 649-684, 789, 809, 886` | gridDim.y: DWT (Hb+7)/8; s123 BLOCK_Y = (h+1)/2; csf_den rows; i4 CM buffer_h; scale-0 CM ceil(buffer_h/(4·rpt)); CSF ceil(rows/4) | `unsigned` | Must stay ≤ 65,535 | ≤ 4,320 | ≤ 13,110 | SAFE |

### shared headers (GPU-relevant parts)

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| `adm_gain_limit.h:75-89` (not included by any CUDA or HIP translation unit; used by SYCL, Metal and the CPU) | `a * g.m_lo`, `a * g.m_hi + (p_lo >> 32)` | `uint64_t` | a < $2^{31}$, m_lo < $2^{32}$, m_hi < $2^{21}$ | < $2^{63}$, < $2^{52}$ | same | SAFE |

### Coverage

| file | rows / note |
|---|---|
| `cuda/integer_adm_cuda.c` (1816 lines, read in full: 1-560 line by line, the rest by targeted reads and greps) | 3 host rows, plus the stride sources of the DWT index row. No integer host sums: conclusion is in double (`adm_dlm_terms`, `adm_aim_num`), and `tmp_res` is memset every frame, so \*\*no clip-level integer accumulator\*\* |
| `cuda/integer_adm_cuda.h` | none (types only) |
| `cuda/integer_adm/adm_cm.cu` (943, full) | 10 rows (shared with HIP) |
| `cuda/integer_adm/adm_csf.cu` (239, full) | 5 rows (shared) |
| `cuda/integer_adm/adm_csf_den.cu` (125, full) | 5 rows (shared) |
| `cuda/integer_adm/adm_decouple_inline.cuh` (184, full) | 5 rows (shared) |
| `cuda/integer_adm/adm_dwt2.cu` (366, full) | 10 rows (shared) |
| `cuda/integer_adm/adm_dwt2_rows.h` (88, full) | 1 row (index helpers) |
| `hip/integer_adm_hip.c` (1782; launch, buffer and conclusion parts read) | host rows shared with CUDA; tmp_accum size noted. No clip-level integer accumulator |
| `hip/integer_adm_hip.h` | none (types only) |
| `hip/integer_adm/adm_cm.hip` (848, full) | in the CM rows; plus the HIP-only scratch-index row and the two-pass s123 DLM reduction |
| `hip/integer_adm/adm_csf.hip` (223) | in the CSF rows |
| `hip/integer_adm/adm_csf_den.hip` (125, full) | in the csf_den rows (shared-memory `thread_sums` instead of warp_reduce) |
| `hip/integer_adm/adm_decouple_inline.hip` (187, full) | in the decouple rows (identical arithmetic, including the double → int32 defect) |
| `hip/integer_adm/adm_dwt2.hip` (381, full) | in the DWT rows (identical) |
| `hip/integer_adm/adm_dwt2_rows.h` (71) | in the index-helper row |
| `adm_cm_accumulator.h` (84, full) | `adm_cm_round_row_total` and `adm_csf_den_round_row_total` are folded into the CM and csf_den row rows; `adm_cm_excess_s0` has its own row |
| `integer_adm_kernels.h` (1463, full read of the GPU-relevant contexts, kernels and shifts) | CPU reference. It is cited for the shared scale-0 row-total overflow (`:1017-1019`, `:1079-1099`), the int16 flt narrowing (`:488-489`) and the defined `MIN` before conversion (`:380`). No GPU-only accumulator |
| `adm_csf_fixed_point.h` (307, full) | none (double limits; `adm_half_shift` is uint32 and bounded). Supplies the weight budgets used above |
| `adm_score.h` (99) | none (double only) |
| `adm_angle_flag.h` (264; fp64 path) | none for G1 (CUDA and HIP pass int64 operands bounded by the decouple rows; the int64 reformulation is SYCL/Metal only) |
| `adm_gain_limit.h` (91, full) | 1 row (not reachable from CUDA or HIP) |
| `integer_adm.c` | read only for the shifts (`i4_dwt2_round`, `adm_cm_ctx_init`) and the driver |

Row counts by verdict, 42 rows in total:

| verdict | rows |
|---|---|
| SAFE | 38 (integrator: frame accumulator relabelled to SAFE) |
| OVERFLOW@16K | 2 (the double → int32 gain conversion, size-independent; the scale-0 CM row total, relabelled by the integrator) |
| OVERFLOW@CAP-ONLY | 0 |
| DEPENDS | 2: the int16 flt narrowing and the scale-0 x_sq |

## G2 integer-overflow audit: float ADM, float VIF, integer VIF (CUDA + HIP)

Repo: master at `571565a47`. Read-only. Paths below are relative to
`core/src/feature/`.

Envelope: 16K N = 132,710,400 ($2^{26.98}$), cap N = $2^{30}$, W,H <= 32768. Samples <= 65535 (bpc 16) and
in range (<= $2^{\mathrm{bpc}} - 1$) unless a row says otherwise.

Basis for integer VIF, checked against the CPU reference (`integer_vif.h:39-46`,
`integer_vif.c:175-228, 254-341, 406-456`):

- Every row of `vif_filter1d_table` sums to exactly 65536. Widths are {17, 9, 5, 3}, and the largest
  tap is 43,728 (scale 3 centre). So any tap sum is at most 65536 \* (max input), whatever W and H are.
- Scale-0 vertical shifts at bpc b: `shift_VP = b`, `shift_VP_sq = 2(b-8)`. Scales 1-3 shift by 16.
  The horizontal shift is always 16. The GPU twins copy these values: `integer_vif_cuda.c:572-589` and
  `integer_vif_hip.c:334-341`.
- Inputs to scales 1-3 are the uint16 decimated planes: at most 65,535 for 16-bit input, at most
  65,280 for 8-bit input.
- The int64 accumulators hold the sums of one frame and one scale. They are cleared every frame
  (`integer_vif_cuda.c:811`, `integer_vif_hip.c:689`), so the frame count does not matter.

### integer VIF: CUDA (`vif_cuda`)

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| cuda/integer_vif/filter1d.cu:198-201 | 8-bit vertical `img_coeff_ref = fcoeff * ref_val`; `o.mu1/mu2 += img_coeff_ref` | uint32_t | 17 taps, fcoeff*v, sum of fcoeff = 65536, v <= 255 | 65536*255 = 16,711,680 | same (no W/H dependence) | SAFE ($2^{24}$ < $2^{32}$) |
| cuda/integer_vif/filter1d.cu:202-204 | 8-bit vertical `o.ref/dis/ref_dis += img_coeff_ref * ref_val` | uint32_t | 17 taps, fcoeff*$v^{2}$, term <= 7784*65025 | 65536*65025 = 4,261,478,400 | same | SAFE ($2^{32}$ - 33,488,896; tight but bounded) |
| cuda/integer_vif/filter1d.cu:207-210 | 8-bit vertical `o.ref_rd/dis_rd += fcoeff_rd * ref_val` (uint16 times uint32 gives unsigned) | uint32_t | 9 taps of filter[1] (sum 65536) times v <= 255 | 16,711,680 | same | SAFE |
| cuda/integer_vif/filter1d.cu:250-253 | `(o.mu1 + 128) >> 8` (and mu2, ref_rd, dis_rd) | uint32_t | rounding of the rows above | <= 65,280 | same | SAFE |
| cuda/integer_vif/filter1d.cu:322-323, 356-357, 394-395 | horizontal `s.mu1/mu2 += fcoeff * t.mu1[si]`; pair form `fcoeff * (t.mu1[lo] + t.mu1[hi])` | uint32_t | fw taps (sum 65536) times tmp.mu1 (<= 65,280 for 8-bit, <= 65,535 for 16-bit); pair sum <= 131,070 times <= 7,784 | 65536*65535 = 4,294,901,760 | same | SAFE ($2^{32}$ - 65,536) |
| cuda/integer_vif/filter1d.cu:324-326, 358-361, 396-398 | `s.ref_tmp/dis_tmp/ref_dis_tmp += fcoeff * (uint64_t)t.ref` | uint64_t | fw taps (sum 65536) times tmp.ref <= 4,294,836,225 | 2.81e14 ($2^{48.0}$) | same | SAFE ($2^{48}$ < $2^{64}$) |
| cuda/integer_vif/filter1d.cu:421-426 | `(uint32_t)((s.ref_tmp + add_shift_round_HP) >> shift_HP)` (shift 16) | uint64_t to uint32_t | narrows the row above | <= 4,294,836,225 (16-bit); 4,261,478,400 (8-bit) | same | SAFE ($2^{32}$ - 131,071) |
| cuda/integer_vif/filter1d.cu:333-334, 369-370, 402-403 | `s.ref_rd/dis_rd += fcoeff_rd * t.ref_convol` | uint32_t | taps of filter[scale+1] (sum 65536) times ref_convol (<= 65,280 or <= 65,535) | 4,294,901,760 | same | SAFE |
| cuda/integer_vif/filter1d.cu:463-464 | `(uint16_t)((s.ref_rd + 32768) >> 16)` | uint32_t add | 4,294,901,760 + 32,768 | 4,294,934,528 | same | SAFE (< $2^{32}$) |
| cuda/integer_vif/filter1d.cu:577-580 | 16-bit vertical `img_coeff_ref = fcoeff * imgcoeff_ref`; `o.mu1/mu2 +=` | uint32_t | term <= 43,728*65,535 = 2,865,714,480; sum of fw taps <= 65536*65535 | 4,294,901,760 | same | SAFE ($2^{32}$ - 65,536) |
| cuda/integer_vif/filter1d.cu:581-583 | `s.ref/dis/ref_dis += img_coeff_ref * (uint64_t)imgcoeff_ref` | uint64_t | sum of fcoeff*$v^{2}$ <= 65536*$65535^{2}$ | 2.81e14 ($2^{48.0}$) | same | SAFE |
| cuda/integer_vif/filter1d.cu:586-589 | `o.ref_rd/dis_rd += fcoeff_rd * imgcoeff_ref` | uint32_t | filter[scale+1] (sum 65536) times v | 4,294,901,760 | same | SAFE |
| cuda/integer_vif/filter1d.cu:603-604, 609-610 | `(uint16_t)((o.mu1 + add_shift_round_VP) >> shift_VP)` (and mu2, ref_rd, dis_rd) | uint32_t add, then narrow to uint16_t | pre-shift <= 4,294,901,760 + 32,768; post-shift <= ($2^{b} - 1$)*$2^{16-b}$ <= 65,535 | 4,294,934,528, then <= 65,535 | same | SAFE for in-range samples. Samples above $2^{\mathrm{bpc}} - 1$ at bpc 9-15 wrap modulo $2^{16}$ here, with the same cast as CPU `integer_vif.c:205-206, 450-451`, so CPU and CUDA agree and the accumulator itself never wraps. |
| cuda/integer_vif/filter1d.cu:605-607 | `(uint32_t)((s.ref + add_shift_round_VP_sq) >> shift_VP_sq)` | uint64_t to uint32_t | scale 0 at bpc b: <= $(2^{b} - 1)^{2}$ \* $2^{32-2b}$, i.e. b=16 4,294,836,225; b=10 4,286,582,784; b=9 4,278,206,464. Scales 1-3: <= $65535^{2}$ | <= 4,294,836,225 | same | SAFE (< $2^{32}$). Out-of-range samples wrap with the same cast as CPU `integer_vif.c:452-455`. |
| cuda/integer_vif/vif_statistics.cuh:127-132 | `((uint64_t)mu1 * mu1) + 2147483648` (also $\mathrm{mu2}^{2}$, mu1*mu2) | uint64_t | mu <= 4,294,901,760 (16-bit) or 4,278,190,080 (8-bit) | ($2^{32}$-$2^{16}$)^2 + $2^{31}$ = $2^{64}$ - 5.6e14 | same | SAFE (margin $2^{49}$) |
| cuda/integer_vif/vif_statistics.cuh:134-136 | `(int32_t)(xx_filt - mu1_sq_val)` (also sigma2_sq, sigma12) | uint32_t wrap, then int32_t | true value is a local (co)variance in [-$2^{30}$, $2^{30}$] (max variance $65535^{2}$/4 = 1,073,709,056) plus rounding | fits int32 | same | SAFE (the modular conversion is defined in C++20; nvcc runs with `--std c++20`, `src/meson.build:1553`) |
| cuda/integer_vif/vif_statistics.cuh:159 | `log_den_stage1 = (uint32_t)sigma_nsq + (uint32_t)sigma1_sq` | uint32_t | sigma1_sq in [$2^{17}$, $2^{31} - 1$] in this branch | <= $2^{31}$ + $2^{17}$ | same | SAFE |
| cuda/integer_vif/vif_statistics.cuh:187 | `numer1 = sv_sq + sigma_nsq` | uint32_t | sv_sq < $2^{31}$ (`vif_sv_sq` guard) | <= 2,147,614,719 | same | SAFE |
| cuda/integer_vif/vif_statistics.cuh:188 | `(int64_t)(g * g * sigma1_sq) + numer1` | double to int64_t, then int64_t | g <= vif_enhn_gain_limit <= 100 (option max, `integer_vif_cuda.c:126`); sigma1_sq <= $2^{31} - 1$ | <= 2.15e13 ($2^{44.3}$) | same | SAFE (< $2^{63}$) |
| cuda/integer_vif/vif_statistics.cuh:99-102, 173, 195 | `vif_cuda_log2_table[v & (VIF_LOG2_TABLE_SIZE - 1u)]` | uint16_t index | v from get_best16_from32/64 in [$2^{15}$, $2^{16}$) | index <= 32,767 | same | SAFE (32768-entry table) |
| cuda/integer_vif/vif_statistics.cuh:171-172, then filter1d.cu:439, 444 | `thread_accum.num_x++`, `.x += x`, reduced into the frame int64 | int64_t | x = -k, k = 16 - clz($2^{17}$ + sigma1_sq), in [3, 16]; 1 term per pixel, N per scale | abs(x) <= 16N = 2.12e9 ($2^{31.0}$) | $2^{34}$ | SAFE (< $2^{63}$) |
| cuda/integer_vif/vif_statistics.cuh:194 | `.x2 += (x2 - x1)` | int64_t (operands int) | x2 in [-16, -2] (numer1 < $2^{32}$); x1 in [-29, -2] (numer1_tmp < $2^{44.3}$); abs <= 27 per pixel | 27N = 3.58e9 ($2^{31.7}$) | $2^{34.8}$ | SAFE |
| cuda/integer_vif/vif_statistics.cuh:195-196, 200 | `.num_log += log2_lookup(numlog) - log2_lookup(denlog)` | int64_t (uint16 - uint16 promotes to int) | table values in [30720, 32768], difference in [-2048, 2048] | 2048N = $2^{38.0}$ | $2^{41}$ | SAFE |
| cuda/integer_vif/vif_statistics.cuh:197, 201 | `.den_log += den_val` | int64_t | <= 32,768 per pixel | $2^{42.0}$ | $2^{45}$ | SAFE |
| cuda/integer_vif/vif_statistics.cuh:205 | `.num_non_log += sigma2_sq` | int64_t | sigma2_sq in [0, $2^{31} - 1$]; realistic <= $2^{30}$ | $2^{58.0}$ (realistic $2^{57.0}$) | $2^{61.0}$ (realistic $2^{60}$) | SAFE (< $2^{63}$). Converting the total to double on the host (`/ 16384.0`) is inexact above $2^{53}$, the same as CPU `integer_vif.c:340`. |
| cuda/integer_vif/vif_statistics.cuh:206 | `.den_non_log += 1` | int64_t | count | <= $2^{27}$ | <= $2^{30}$ | SAFE |
| cuda/integer_vif/filter1d.cu:439 (cuda/cuda_helper.cuh:125-133) | `warp_reduce(int64_t)`: lo and hi 32-bit halves shuffled as 64-bit, OR-recombined, added | int64_t | 32 lanes, 2 pixels per thread | per warp <= 64 times the per-pixel max | same | SAFE (exact recombination, no carry loss; `<< 32` of a negative value is defined in C++20) |
| cuda/integer_vif/filter1d.cu:444 (cuda/cuda_helper.cuh:136-139) | `atomicAdd_int64` = `atomicAdd((unsigned long long *), (uint64)val)` | unsigned long long (two's complement) | frame totals of the accumulator rows above | as above | as above | SAFE (modular add is the int64 add) |
| cuda/integer_vif_cuda.c:703-707 | host `accum.x + (accum.num_x * 17)`; `num_log / 2048.0`, etc. | int64_t, then double | abs(x) <= 16N, num_x <= N | 33N = 4.4e9 | $2^{35.0}$ | SAFE |
| cuda/integer_vif/filter1d.cu:122-128 | `(ptrdiff_t)img_row * stride + x_block_start + col` | ptrdiff_t | input sample index | <= N | <= $2^{30}$ | SAFE (64-bit) |
| cuda/integer_vif/filter1d.cu:164-166 | `buffer_idx = y * stride_tmp + x_start + idx`, with `stride_tmp = buf.stride_tmp / 4` as int (ALIGN_CEIL(4W)/4) | int | tmp-plane element index | 8639*15360 + 15359 = 132,710,399 ($2^{27.0}$) | 32767*32768 + 32767 = $2^{30} - 1$ | SAFE (< $2^{31}$; the byte offset is formed by 64-bit pointer arithmetic) |
| cuda/integer_vif/filter1d.cu:505-506, 286-294 | `buf_row = y * stride_tmp`; `buf_row + img_col` | int | same index | 132,710,399 | $2^{30} - 1$ | SAFE |
| cuda/integer_vif/filter1d.cu:462-464 | `(y / 2) * rd_stride + (x / 2)` | ptrdiff_t | half-resolution plane index | <= N/4 | <= $2^{28}$ | SAFE |
| cuda/integer_vif_cuda.c:380-382 | `w * (1u << hbd) + tex_alignment - 1u` | unsigned | row bytes | 30,720 + 511 | 65,536 + 511 | SAFE |
| cuda/integer_vif_cuda.c:384-394 | `rd_w_bytes` (int); `stride_16/32/64/tmp = ALIGN_CEIL(w * sizeof)`; `rd_size`; `data_sz = 2*rd_size + 2*(h*stride_16) + 5*(h*stride_32) + 8*(stride_tmp*h)` | int; size_t; ptrdiff_t | byte sizes (unsigned h times ptrdiff_t is 64-bit) | data_sz 7,564,492,800 B | 61,203,283,968 B | SAFE (size_t; an allocation size, no wrap) |
| cuda/integer_vif_cuda.c:326-359 | carve offsets `data += h * s->buf.stride_16` (and the others) | CUdeviceptr (unsigned long long) | plane offsets | <= data_sz | <= data_sz | SAFE |
| cuda/integer_vif_cuda.c:528-531, 549-552, 616-621, 680-683 | grid: 8-bit vertical y = ceil(h/4), horizontal y = h; 16-bit `grid_vert_y = ceil(h/4)`, `grid_hori_y = h` | int to unsigned | grid dims | 8,640 | 32,768 | SAFE (<= 65535, the gridDim.y limit) |
| cuda/integer_vif_cuda.c:241-247; vif_statistics.cuh:86 | log2 transfer: `grid = 32768/256`; `i = blockIdx.x*blockDim.x + threadIdx.x` | unsigned | table index | <= 32,767 | same | SAFE |
| cuda/integer_vif_cuda.c:258 | `table_bytes = VIF_LOG2_TABLE_SIZE * sizeof(uint16_t)` | size_t | 65,536 | 65,536 | same | SAFE |

Asides (CUDA integer VIF, no overflow, no verdict):

- **Possible undefined-behaviour shuffle, unverified.** `filter1d.cu:513/535`: `vif_hori_flush_accums()`
  (`warp_reduce`, `__shfl_down_sync(0xffffffff, …)`) runs only inside `if (y < h && x_start < w)`.
  When the scale width is not a multiple of 64, lanes of the edge warp skip the shuffle that the full
  mask names. The CUDA guide calls this undefined, and the values read from those lanes are
  undefined. This is not an overflow. `test_integer_vif_cpu_cuda_parity` (256x144, so a 32-pixel
  width at scale 3 with 16 active lanes) passes places=4, so current hardware returns benign values.
  It is not checked on a device here.
- **Idle horizontal blocks.** `filter1d_16_grid` (`integer_vif_cuda.c:620`) sets `grid_hori_x = ceil(w/128)`, but a block
  covers 256 pixels (vpt = 2), so about half the x-blocks do nothing. There is no wrap.
- **Unused buffer memory.** `data_sz` also carves 2 stride_16 planes and 5 stride_32 planes (mu1, mu2,
  mu1_32, mu2_32, ref_sq, dis_sq, ref_dis) that no kernel reads. That is about 3.18e9 B at 16K and
  2.58e10 B at cap of device memory, which matters for memory, not for overflow.

### integer VIF: HIP (`vif_hip`)

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| hip/integer_vif/vif_statistics.hip:378-385 | 8-bit vertical `cr = coeff * r`; `accum_mu1 += cr`; `accum_ref += cr * r` (and dis, ref_dis) | uint32_t | 17 taps (sum 65536), v <= 255 | mu 16,711,680; sq 4,261,478,400 | same | SAFE (sq is $2^{32}$ - 33,488,896) |
| hip/integer_vif/vif_statistics.hip:387-391 | `accum_ref_rd += cr_rd * r` | uint32_t | 9 taps (sum 65536) times v <= 255 | 16,711,680 | same | SAFE |
| hip/integer_vif/vif_statistics.hip:395-401 | `(accum + 128u) >> 8` | uint32_t | rounding | <= 65,280 | same | SAFE |
| hip/integer_vif/vif_statistics.hip:427-431 | 8-bit horizontal `accum_mu1 += coeff * buf.tmp.mu1[row + kx]` | uint32_t | 17 taps (sum 65536) times tmp.mu1 <= 65,280 | 4,278,190,080 | same | SAFE ($2^{32}$ - 16,777,216) |
| hip/integer_vif/vif_statistics.hip:432-434, 535-537 | `accum_ref += (uint64_t)coeff * (uint64_t)buf.tmp.ref[...]` | uint64_t | taps (sum 65536) times tmp.ref <= 4,294,836,225 | $2^{48.0}$ | same | SAFE |
| hip/integer_vif/vif_statistics.hip:439-441, 542-544 | `(uint32_t)((accum_ref + 32768) >> 16)` | uint64_t to uint32_t | narrowing | <= 4,294,836,225 | same | SAFE |
| hip/integer_vif/vif_statistics.hip:478-489 | 16-bit vertical `cr = coeff * r`; `accum_mu1 += cr`; `accum_ref += (uint64_t)cr * r` | uint32_t / uint64_t | term <= 43,728*65,535; sum <= 65536*65535; sq <= 65536*$65535^{2}$ | 4,294,901,760 / $2^{48.0}$ | same | SAFE |
| hip/integer_vif/vif_statistics.hip:491-495 | `accum_ref_rd += cr_rd * r` | uint32_t | filter[scale+1] (sum 65536) times v | 4,294,901,760 | same | SAFE |
| hip/integer_vif/vif_statistics.hip:499-500, 506-507 | `(accum_mu1 + (uint32_t)add_shift_VP) >> shift_VP`, stored to uint32 tmp \*\*without\*\* the `(uint16_t)` cast that CPU (`integer_vif.c:205-206, 450-451`) and CUDA (`filter1d.cu:603-610`) apply | uint32_t | pre-shift <= 4,294,934,528 | <= 65,535 for in-range samples | same | SAFE for in-range samples (out-of-range: next two rows) |
| hip/integer_vif/vif_statistics.hip:499-500, then 530-534 | horizontal `accum_mu1 += coeff * buf.tmp.mu1[row + kx]` at scale 0, bpc 9-15, samples above $2^{\mathrm{bpc}} - 1$ | uint32_t | tmp.mu1 is not truncated: up to (65536*65535 + $2^{b-1}$) >> b = 8,388,480 (b=9) to 131,070 (b=15); 17 taps with sum 65536 | 5.5e11 (b=9) to 8.59e9 (b=15), wraps past $2^{32}$ | same (independent of W and H) | DEPENDS on the input sample range: only malformed samples above $2^{\mathrm{bpc}}$-1. CPU and CUDA truncate to 16 bits at the store, so their sum stays <= 4,294,901,760; HIP wraps instead, giving HIP != CPU. In-range input is SAFE. |
| hip/integer_vif/vif_statistics.hip:506-507, then 329-336 | `vif_downsample_store` `accum_ref_rd += coeff * buf.tmp.ref_convol[row + kx]` at scale 0, bpc 9-15, samples above $2^{\mathrm{bpc}} - 1$ | uint32_t | ref_convol is not truncated (CPU `integer_vif.c:205-206` truncates): up to 8,388,480; 9 taps (sum 65536) | up to 5.5e11, wraps past $2^{32}$ | same | DEPENDS on the input sample range, as the row above. HIP != CPU for malformed input; in-range input is SAFE (<= 4,294,901,760 + 32,768). |
| hip/integer_vif/vif_statistics.hip:501-504 | `(uint32_t)((accum_ref + (uint64_t)add_shift_VP_sq) >> shift_VP_sq)` | uint64_t to uint32_t | same as CUDA 605-607 | <= 4,294,836,225 | same | SAFE (out-of-range wraps exactly as CPU) |
| hip/integer_vif/vif_statistics.hip:329-336 | `vif_downsample_store` `accum_ref_rd += coeff * ref_convol`; `(accum + 32768u) >> 16` | uint32_t | filter (sum 65536) times <= 65,535 (in range) | 4,294,934,528 | same | SAFE |
| hip/integer_vif/vif_statistics.hip:201-207 | `((uint64_t)mu1 * mu1) + 2147483648ULL`; `(int32_t)(xx_filt - mu1_sq)` | uint64_t; uint32_t to int32_t | same as CUDA 127-136 | $2^{64}$ - 5.6e14; fits int32 | same | SAFE |
| hip/integer_vif/vif_statistics.hip:172, 185-186 | `log_den_stage1`, `numer1`, `(int64_t)(g * g * sigma1_sq) + (int64_t)numer1` | uint32_t; double to int64_t | g <= 100 (`integer_vif_hip.c:146`) | <= $2^{31}$ + $2^{17}$; <= $2^{44.3}$ | same | SAFE |
| hip/integer_vif/vif_statistics.hip:149-152 | `log2_table[v & (VIF_HIP_LOG2_TABLE_SIZE - 1u)]` | uint16_t index | v in [$2^{15}$, $2^{16}$) | <= 32,767 | same | SAFE |
| hip/integer_vif/vif_statistics.hip:176-178, 191-193, 219-220 | per-thread `thr.num_x/x/den_log/x2/num_log/num_non_log/den_non_log` (1 pixel each); `(int64_t)lookup - (int64_t)lookup` | int64_t | per-pixel bounds as in the CUDA rows | per pixel: abs <= $2^{31}$ | same | SAFE |
| hip/integer_vif/vif_statistics.hip:306-316 | `atomicAdd((unsigned long long *)&dst->field, (unsigned long long)src->field)`, 7 fields, every thread, frame accumulators | unsigned long long (two's complement int64) | N pixels per scale; x 16N, x2 27N, num_log 2048N, den_log 32768N, num_non_log ($2^{31} - 1$)N, counts N | $2^{31.0}$ / $2^{31.7}$ / $2^{38}$ / $2^{42}$ / $2^{58.0}$ / $2^{27}$ | $2^{34}$ / $2^{34.8}$ / $2^{41}$ / $2^{45}$ / $2^{61.0}$ / $2^{30}$ | SAFE (< $2^{63}$; cleared each frame, `integer_vif_hip.c:689`) |
| hip/integer_vif_hip.c:176-181 | host `accum.x + (accum.num_x * 17)`; `(double)num_non_log / 16384.0` | int64_t; double | as CUDA 703-707 | 4.4e9 | $2^{35}$ | SAFE |
| hip/integer_vif/vif_statistics.hip:376-377 | `(size_t)ky * (size_t)buf.stride + (size_t)x` | size_t | 8-bit input index | <= N | <= $2^{30}$ | SAFE |
| hip/integer_vif/vif_statistics.hip:471-481 | `(ptrdiff_t)ky * in_stride16 + x` | ptrdiff_t | 16-bit input index | <= N | <= $2^{30}$ | SAFE |
| hip/integer_vif/vif_statistics.hip:394-401, 421-422, 498-507, 524-525, 325-333 | `y * stmp + x`, `row + kx`, with `stmp = (int)(stride_tmp / 4)`, stride_tmp = round64(4W) | int | tmp-plane element index | 132,710,399 | $2^{30} - 1$ | SAFE (< $2^{31}$) |
| hip/integer_vif/vif_statistics.hip:340-342 | `(y / 2) * rd_stride16 + (x / 2)` | ptrdiff_t | half-resolution index | <= N/4 | <= $2^{28}$ | SAFE |
| hip/integer_vif_hip.c:405-421 | `stride = (ptrdiff_t)w * bpp`; `rd_stride`; `stride_16/32/64 = round64(w * sizeof)`; `rd_size`; data_sz | ptrdiff_t / size_t | byte sizes | 7,564,492,800 B | about 6.12e10 B | SAFE |
| hip/integer_vif_hip.c:438-458 | `plane_16/32/tmp = (size_t)h * (size_t)stride`; `ptr += plane` | size_t | plane offsets | <= data_sz | <= data_sz | SAFE |
| hip/integer_vif_hip.c:265-268, 283-285, 349-352, 366-368 | `GX_V = ceil(w/32)`, `GY_V = ceil(h/8)`, `GX_H = ceil(w/128)`, `GY_H = h` | int | grid dims | GY_H 8,640 | 32,768 | SAFE |
| hip/integer_vif_hip.c:474, 488 | `VIF_LOG2_TABLE_SIZE * sizeof(uint16_t)` | size_t | 65,536 | 65,536 | same | SAFE |

### integer VIF: shared headers

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| integer_vif_sv_sq.h:158-161 | `(uint32_t)sv` with `sv = sigma2_sq - g * sigma12` | double to uint32_t | guarded `sv > 0.0 && sv < 2147483648.0`, otherwise 0 | < $2^{31}$ | same | SAFE (no out-of-range conversion) |
| vif_log2_table.h:219-223 | `(uint16_t)roundf(log2f((float)(0x8000 + i)) * 2048)`, `unsigned i < 32768` | float to uint16_t | entry max round(log2(65535)*2048) = 32,768 | 32,768 | same | SAFE (< 65536) |

### float VIF: CUDA (`float_vif_cuda`) and HIP (`float_vif_hip`)

The accumulators are fp32 throughout: `fvif_row_sum` and `fvif_sum_rows`, as in the CPU. They are
never converted to an integer, so they are out of scope. Only index, size and float-to-int math is
listed. The only conversion is `(float)((int32_t)exponent - 127)`.

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| float_vif_gpu_common.h:136-139 | `fvif_term_index`: `((size_t)x * height + y) * 2` | size_t | term index | <= 2N = $2^{28}$ | $2^{31}$ | SAFE (size_t) |
| float_vif_gpu_common.h:186-190 | `(float)((int32_t)exponent - 127)` | uint32_t to int32_t | exponent <= 255 | [-127, 128] | same | SAFE |
| float_vif_gpu_common.h:272-275, 289-291 | loop `uint32_t x < width`; `(size_t)y * FVIF_TERM_FLOATS (+1)` | uint32_t; size_t | row and column indices | <= 15,360 / 2*8640 | <= $2^{16}$ | SAFE |
| cuda/float_vif/float_vif_score.cu:48-50 | `plane[y * stride_bytes + x]`, `plane + y * stride_bytes` | int times ptrdiff_t gives ptrdiff_t | raw byte offset (packed, stride = W*bpp) | <= 2HW = 2.65e8 | $2^{31}$ | SAFE (64-bit) |
| cuda/float_vif/float_vif_score.cu:72 | `(size_t)y * (size_t)in.stride + (size_t)x` | size_t | fp32 plane index | <= N/4 | <= $2^{28}$ | SAFE |
| cuda/float_vif/float_vif_score.cu:98-109, 118-142, 151 | tile `tile_oy = blockIdx.y*16 - hfw`; `i < tile_h * tile_w`; `tr * FVIF_MAX_TILE_W + tc`; shared-memory indices | int | tile <= 32*32 | small; tile_oy <= H | same | SAFE |
| cuda/float_vif/float_vif_score.cu:185-186, 195 | `gx/gy = blockIdx * 16 + threadIdx` | uint32_t | pixel coordinates | <= 15,375 | <= 32,783 | SAFE |
| cuda/float_vif/float_vif_score.cu:203, 211-212 | row kernel `y = blockIdx.x * 128 + threadIdx.x`; `(size_t)y * 2` | uint32_t; size_t | row index | <= 8,703 | <= 32,767 | SAFE |
| cuda/float_vif/float_vif_score.cu:222-233, 238-242, 253 | decimate `gx/gy` (int), `in_x = 2 * gx`, reflect indices, `(size_t)gy * out_width + gx` | int; size_t | <= W | 15,360 | 32,768 | SAFE |
| cuda/float_vif_cuda.c:236, 240, 249 | `raw_bytes = (size_t)w * h * bpp`; `fbytes`; `term_bytes = (size_t)w * h * 2 * sizeof(float)` | size_t | allocation bytes | term_bytes 1.06e9 | $2^{33}$ | SAFE (size_t) |
| cuda/float_vif_cuda.c:221-224, 381, 419, 426 | `rows_bytes`; `raw_stride = (size_t)s->width * bpp`; `in.stride = (int64_t)...` | size_t / int64_t | sizes and strides | small | small | SAFE |
| cuda/float_vif_cuda.c:444-445, 469-470, 481 | grid `ceil(dim/16)`, `row_grid = ceil(h/128)` | unsigned | grid dims | <= 960 / 540 / 68 | <= 2048 / 256 | SAFE |
| hip/float_vif/float_vif_score.hip:59-61, 83 | same as CUDA 48-50 and 72 | ptrdiff_t; size_t | raw and fp32 indices | 2.65e8 | $2^{31}$ | SAFE |
| hip/float_vif/float_vif_score.hip:109-153, 162, 196-197, 206, 214, 222-223, 232-243, 263 | tile, pixel, row and decimate indices (same expressions as CUDA) | int / uint32_t / size_t | as CUDA | as CUDA | as CUDA | SAFE |
| hip/float_vif_hip.c:280-282, 292-293, 360, 367 | `rows_bytes`; `fbytes`; `term_bytes = (size_t)s->width * s->height * 2 * sizeof(float)`; `in.stride` int64 | size_t / int64_t | sizes | 1.06e9 | $2^{33}$ | SAFE |
| hip/float_vif_hip.c:384-385, 407-408, 421 | grid dims | unsigned | as CUDA | as CUDA | as CUDA | SAFE |

### float ADM: CUDA (`float_adm_cuda`) and HIP (`float_adm_hip`)

The accumulators are fp32: the per-row sum `fadm_row_sum` and the frame fold `fadm_fold_rows`. They
are never converted to an integer. The device code has no float-to-int conversion; `fadm_abs` is a
bit mask. Integer math is index and size arithmetic only. Band planes at scale 0 are
`half_w0 = (W+1)/2` by `half_h0 = (H+1)/2`, with `buf_stride = (half_w0 + 3) & ~3`.

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| cuda/float_adm/float_adm_score.cu:66-68 | `plane[y * stride_bytes + x]`, `plane + y * stride_bytes` | int times ptrdiff_t gives ptrdiff_t | raw byte offset (packed W*bpp) | <= 2.65e8 | $2^{31}$ | SAFE (64-bit) |
| cuda/float_adm/float_adm_score.cu:85 | `band_buf[y * buf_stride + x]` (parent LL band) | int | <= half_h0 \* buf_stride | 33,177,600 | $2^{28}$ | SAFE |
| cuda/float_adm/float_adm_score.cu:134-137 | `out_stride = cur_w * 2`; `gy * out_stride + cur_w + gx` | int | dwt_tmp index < half_h \* 2 \* cur_w | 132,710,399 | $2^{30} - 1$ | SAFE (< $2^{31}$, margin 2x) |
| cuda/float_adm/float_adm_score.cu:151-153 | `stride = cur_w * 2`; `gy * stride + half_offset + x_sub` | int | same index | 132,710,399 | $2^{30} - 1$ | SAFE |
| cuda/float_adm/float_adm_score.cu:187-191 | `slice = buf_stride * half_h`; `3 * slice + gy * buf_stride + gx` | int | band index < 4 \* buf_stride \* half_h0 | 132,710,399 | $2^{30} - 1$ | SAFE (< $2^{31}$) |
| float_adm_gpu_common.h:177-180 | `fadm_band_index`: `((size_t)band * half_h + y) * buf_stride + x` | size_t | band index | < $2^{27}$ | < $2^{30}$ | SAFE |
| float_adm_gpu_common.h:185-194 | `fadm_term_index` / `fadm_row_index` (uint32 arguments widened to size_t first) | size_t | 9 slots times the region | <= 9 \* 33,177,600 | <= 9 \* $2^{28}$ | SAFE |
| float_adm_gpu_common.h:357-361, 367-371 | `terms[(size_t)x * stride]`; `rows[y]` with `uint32_t y < count` | size_t | indices | as above | as above | SAFE |
| cuda/float_adm/float_adm_score.cu:248-254 | `rx/ry` uint32; `a.left + (int)rx` | uint32_t; int | region coordinates | <= 7,680 | <= 16,384 | SAFE |
| cuda/float_adm/float_adm_score.cu:289-293 | `FADM_TERM_SLOTS * a.region_h`; `id`, `slot = id / region_h`, `y = id - slot * region_h` | uint32_t | 9 \* region_h | <= 38,880 | <= 147,456 | SAFE |
| cuda/float_adm_cuda.c:249-271 | `hw = (cw + 1u) / 2u`; `row_floats += (size_t)9 * (bottom - top)`; `buf_stride = (hw + 3u) & ~3u` | unsigned / size_t | dimensions | small | small | SAFE |
| cuda/float_adm_cuda.c:261, via adm_tools.c:77-84 (decl adm_float_reference.h:37) | `adm_border_s`: `(int)(w * border_factor - 0.5)`, `right = w - left` | double to int | border_factor = ADM_BORDER_FACTOR 0.1 (constant, `adm_options.h:24`) | <= 768 | <= 1,638 | SAFE |
| cuda/float_adm_cuda.c:386-399, 418, 424, 434-435 | `csf_bytes`, `term_bytes`, `raw_bytes = (size_t)w * h * bpp`, `dwt_bytes = (size_t)w * 2 * half_h0 * 4`, `band_bytes` | size_t | allocation bytes | dwt 5.3e8 | $2^{33}$ | SAFE (size_t) |
| cuda/float_adm_cuda.c:517 | `(ptrdiff_t)(s->width * bpp)` (unsigned times size_t) | size_t | raw stride | 30,720 | 65,536 | SAFE |
| cuda/float_adm_cuda.c:595-596, 642-643, 689-690, 717-718, 729-730 | grid dims; `sums = FADM_TERM_SLOTS * region_h` | unsigned | grid | small | small | SAFE |
| cuda/float_adm_cuda.c:865-867 | `1e-10 * (w * h)` with `int w, h` | int times int | frame area | 132,710,400 | $2^{30}$ = 1,073,741,824 | SAFE (< INT32_MAX, margin 2x) |
| cuda/float_adm_cuda.c:839-844, via adm_tools.c:66-68 (decl adm_float_reference.h:51) | `adm_pool_bands_s(..., region_w, region_h, ...)` calls `get_noise_constant`: `w * h * weight` | int times int (then double) | reduced band region <= half_w0 \* half_h0 | <= 33,177,600 | <= $2^{28}$ | SAFE |
| cuda/float_adm_cuda.c:833-836 | `(uint32_t)region_h`, `fadm_row_index(slot, 0u, region_h)` | uint32_t / size_t | row offsets | small | small | SAFE |
| hip/float_adm/float_adm_score.hip:81-83 | as CUDA 66-68 | ptrdiff_t | raw byte offset | 2.65e8 | $2^{31}$ | SAFE |
| hip/float_adm/float_adm_score.hip:100 | `band_buf[y * buf_stride + x]` | int | parent LL | 33,177,600 | $2^{28}$ | SAFE |
| hip/float_adm/float_adm_score.hip:149-152, 166-168 | `out_stride = cur_w * 2`; `gy * out_stride + cur_w + gx`; dwt_tmp read | int | dwt_tmp index | 132,710,399 | $2^{30} - 1$ | SAFE |
| hip/float_adm/float_adm_score.hip:202-206 | `slice = buf_stride * half_h`; `3 * slice + gy * buf_stride + gx` | int | band index | 132,710,399 | $2^{30} - 1$ | SAFE |
| hip/float_adm/float_adm_score.hip:263-269, 304-308 | rx/ry; `FADM_TERM_SLOTS * region_h` | uint32_t | as CUDA | as CUDA | as CUDA | SAFE |
| hip/float_adm_hip.c:213-231, 333, 356-357, 373-374, 410-411, 421-423, 436-437, 450-451 | per-scale dims; raw_stride; grids; region; `sums` | unsigned / size_t / ptrdiff_t | as CUDA | as CUDA | as CUDA | SAFE |
| hip/float_adm_hip.c:506-512, 525-526 | `dwt_bytes`, `csf_bytes`, `term_bytes`, `row_bytes`, `band_bytes` | size_t | allocation bytes | 5.3e8 | $2^{33}$ | SAFE |
| hip/float_adm_hip.c:722-724 | `1e-10 * (w * h)` with `int w, h` | int times int | frame area | 132,710,400 | $2^{30}$ | SAFE (margin 2x) |
| hip/float_adm_hip.c:687-702 | `adm_pool_bands_s(region_w, region_h)` to `get_noise_constant` `w * h` | int times int | region | <= 33,177,600 | <= $2^{28}$ | SAFE |

### Coverage

All 27 files in group G2 were read in full or every grep hit was read in context. Rows per file (111 rows in total: 109 SAFE, 2 DEPENDS, 0 OVERFLOW@16K, 0 OVERFLOW@CAP-ONLY):

- cuda/float_adm_cuda.c: 8
- cuda/float_adm_cuda.h: none (declares the PTX symbol only)
- cuda/float_adm/float_adm_device.h: none (macros and typedefs only)
- cuda/float_adm/float_adm_score.cu: 7
- cuda/float_vif_cuda.c: 3
- cuda/float_vif_cuda.h: none (declares the PTX symbol only)
- cuda/float_vif/float_vif_device.h: none (macros and typedefs only)
- cuda/float_vif/float_vif_score.cu: 6
- cuda/integer_vif_cuda.c: 7
- cuda/integer_vif_cuda.h: none (struct types only; `vif_accums` is 7 int64_t, covered under the .cuh rows)
- cuda/integer_vif/filter1d.cu: 20 (includes the `warp_reduce`/`atomicAdd_int64` rows)
- cuda/integer_vif/vif_statistics.cuh: 12
- hip/float_adm_hip.c: 4
- hip/float_adm_hip.h: none
- hip/float_adm/float_adm_hip_math.h: none (macros only)
- hip/float_adm/float_adm_score.hip: 5
- hip/float_vif_hip.c: 2
- hip/float_vif_hip.h: none
- hip/float_vif/float_vif_score.hip: 2
- hip/integer_vif_hip.c: 5
- hip/integer_vif_hip.h: none (`vif_accums_hip` is 7 int64_t, covered under the .hip rows)
- hip/integer_vif/vif_statistics.hip: 22 (2 DEPENDS)
- float_adm_gpu_common.h: 3
- float_vif_gpu_common.h: 3
- integer_vif_sv_sq.h: 1
- vif_log2_table.h: 1
- adm_float_reference.h: integer parts are `adm_border_s` and `adm_pool_bands_s` (whose body, `get_noise_constant`, uses `w * h`). Both are covered by the float ADM rows; 0 rows of its own.

Auxiliary files read to bound the terms:

- integer_vif.c and integer_vif.h: the CPU reference shifts, casts, filter table and log2_32/64.
- cuda/cuda_helper.cuh:125-139: `warp_reduce` and `atomicAdd_int64`.
- cuda/cuda_tile_index.h: the reflect/clamp index helpers.
- mem.h:26-31: `ALIGN_CEIL`, `MAX_ALIGN = 32`.
- cuda/common.h:54-55: warp size 32, cache line 128.
- adm_tools.c:66-99: `get_noise_constant` and `adm_border_s`.
- adm_options.h:24: `ADM_BORDER_FACTOR` = 0.1.
- src/meson.build:1553: CUDA builds with `--std c++20`.
- test_integer_vif_cpu_cuda_parity.c: the fixture is 256x144.

## G3a: motion family, CUDA + HIP twins (integer-overflow audit)

Repo: master at `571565a47`. Read-only.
Paths are relative to `core/src/feature/` unless they start with `core/`.

Envelope: 16K N = 132,710,400 (W 15360, H 8640); 8K DCI N = 35,389,440; 1080p N = 2,073,600;
cap W,H <= 32768, N <= $2^{30}$ (`core/src/picture.c:46`); bpc 8..16 (`core/src/picture.c:37-38`).

### Shared derivation: per-pixel motion SAD term

Taps `{3571, 16004, 26386, 16004, 3571}`, sum 65536 = $2^{16}$ (`cuda/integer_motion_v2/motion_v2_score.cu:39`,
`hip/integer_motion_v2/motion_v2_score.hip:67`, identical to CPU `integer_motion.h:26`). CPU reference
`integer_motion.c:164-257` (pipeline_8 / pipeline_16): vertical shift `bpc`, round $2^{\mathrm{bpc}-1}$; horizontal
shift 16, round $2^{15}$. Twins copy it exactly.

- diff `d = prev - cur`: |d| <= $2^{\mathrm{bpc}} - 1$ for in-range samples, <= 65535 for any uint16 sample.
- vertical `v = (sum f_k d_k + 2^(bpc-1)) >> bpc`: |sum| <= 65536 \* |d|max.
  In range: |v| <= floor(65536 ($2^{\mathrm{bpc}} - 1$) / $2^{\mathrm{bpc}}$ + 0.5) <= 65535 (bpc 16: 65535; bpc 8: 65280).
  Out-of-range samples (uint16 value > $2^{\mathrm{bpc}} - 1$, worst bpc = 9): |v| <= (65536 \* 65535 + 256) >> 9 = 8,388,480 (~$2^{23}$).
- horizontal `h = (sum f_k v_k + 2^15) >> 16`: |h| <= |v|max. In range |h| <= 65535 (reached by a uniform
  max-difference frame: ref 65535 / dis 0). Out-of-range worst |h| <= 8,388,480.
- Frame SAD = sum over N pixels of |h|. In range: 16K 8,697,176,064,000 ($2^{42.98}$); 8K 2,319,246,950,400 ($2^{41.08}$);
  1080p 135,893,376,000 ($2^{36.98}$); cap 70,367,670,435,840 ($2^{46.0}$).
  Out-of-range worst (bpc 9): 16K 1.113e15 ($2^{49.98}$); cap 9,007,061,815,787,520 (< $2^{53}$ = 9,007,199,254,740,992).
- The SAD accumulator is zeroed before every frame (CUDA `cuda/integer_motion_sad_cuda.c:177`, HIP
  `hip/integer_motion_sad_hip.c:172`), so there is no clip-level (cross-frame) integer accumulator in G3a;
  frames-to-overflow: not applicable.

### motion SAD kernel (motion_cuda + motion_v2_cuda) - CUDA

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| cuda/integer_motion_v2/motion_v2_score.cu:65 | `plane + ((ptrdiff_t)y * stride)` then `+ x` on `const T *` | `ptrdiff_t` byte offset, `int x` element index | row offset of a packed plane (pitch = W*bpp, `integer_motion_sad_cuda.c:179`) | 8639*30720 + 2*15359 = 265,420,798 B | 32767*65536 + 2*32767 = 2,147,483,646 B | SAFE ($2^{31}$ < $2^{63}$; would be OVERFLOW@CAP-ONLY in int32 byte math) |
| cuda/integer_motion_v2/motion_v2_score.cu:77-78 | `(int)(blockIdx.x * MV2_BLOCK_X) - MV2_RADIUS` | `unsigned` product cast to `int` before the subtract | tile origin | <= 15360 | <= 32768 | SAFE ($2^{15}$ < $2^{31}$) |
| cuda/integer_motion_v2/motion_v2_score.cu:85-88 (+ cuda/cuda_tile_index.h:37-61) | `vmaf_cuda_reflect_101`: `(2 * extent) - idx - 2`, then clamp | `int` | reflected index | <= 30718 | <= 65534 | SAFE ($2^{16}$ < $2^{31}$) |
| cuda/integer_motion_v2/motion_v2_score.cu:89-90 | `s_diff[ty][tx] = load_sample - load_sample` | `int` result stored in `int32_t` | one diff, uint16 - uint16 promoted to int | abs <= 65535 | abs <= 65535 | SAFE ($2^{16}$ < $2^{31}$) |
| cuda/integer_motion_v2/motion_v2_score.cu:112-115 (8bpc) | `blurred_y += (VAcc)mv2_filter_d[k] * (VAcc)s_diff[r+k][c]`, VAcc = int32_t | `int32_t` | 5 taps, term <= 26386*255; sum <= 65536*255 | <= 16,711,680 (+128 round) | same | SAFE ($2^{24}$ < $2^{31}$) |
| cuda/integer_motion_v2/motion_v2_score.cu:112-115 (16bpc, VAcc = int64_t, :213) | same, int64 accumulator | `int64_t` | 5 taps, term <= 26386*65535 = 1,729,206,510; sum <= 65536*65535 | <= 4,294,901,760 ($2^{32}$) | same | SAFE ($2^{32}$ < $2^{63}$; int32 would overflow, kernel comment :211) |
| cuda/integer_motion_v2/motion_v2_score.cu:116 | `s_v[r][c] = (int32_t)((blurred_y + round_y) >> shift_y)` | `int32_t` (narrowed from VAcc) | rounded vertical value | abs <= 65535 in range; <= 8,388,480 out-of-range bpc 9 | same | SAFE ($2^{23}$ < $2^{31}$) |
| cuda/integer_motion_v2/motion_v2_score.cu:126-129 | `blurred += (int64_t)mv2_filter_d[k] * (int64_t)s_v[..]` | `int64_t` | 5 taps, term <= 26386*abs(v); sum <= 65536*abs(v)max | <= 4,294,901,760 in range; <= 549,747,425,280 ($2^{39}$) out-of-range | same | SAFE ($2^{39}$ < $2^{63}$) |
| cuda/integer_motion_v2/motion_v2_score.cu:130-131 | `h = (blurred + round_x) >> 16`, `h < 0 ? -h : h` | `int64_t` | per-pixel term | <= 65535 (out-of-range 8,388,480) | same | SAFE |
| cuda/integer_motion_v2/motion_v2_score.cu:143-145 | `v += __shfl_down_sync(.., v, off)` | `unsigned long long` | 32 lanes, term <= 65535 | <= 2,097,120 (out-of-range $2^{28}$) | same | SAFE ($2^{28}$ < $2^{64}$) |
| cuda/integer_motion_v2/motion_v2_score.cu:142,148-153 | `s_warp[lid >> 5] = v`; second shuffle over 8 warp sums | `unsigned long long` | 256 px per block (8 warps), term <= 65535 | <= 16,776,960 ($2^{24}$); out-of-range 2,147,450,880 ($2^{31}$) | same | SAFE ($2^{31}$ < $2^{64}$) |
| cuda/integer_motion_v2/motion_v2_score.cu:155 | `atomicAdd(sad, v)` into `*sad` | `unsigned long long` (device buffer read back as `uint64_t`) | one add per block, ceil(W/16)*ceil(H/16) blocks; sum over N px of abs(h) | 8,697,176,064,000 ($2^{42.98}$); out-of-range 1.113e15 ($2^{49.98}$) | 70,367,670,435,840 ($2^{46.0}$); out-of-range 9.007e15 (< $2^{53}$) | SAFE ($2^{46}$ < $2^{64}$; zeroed per frame, no cross-frame sum) |
| cuda/integer_motion_v2/motion_v2_score.cu:177-178 | `x = blockIdx.x * blockDim.x + threadIdx.x`, same for y | `unsigned` | global pixel index | <= 15375 | <= 32783 | SAFE |
| cuda/integer_motion_sad_cuda.c:109-112 | `(size_t)width * height * ((bpc <= 8u) ? 1u : 2u)` | `size_t` (cast before first multiply) | packed luma plane bytes, ring slots | 265,420,800 B | $2^{31}$ B | SAFE (size_t 64-bit; uint32 would hold, int32 would not) |
| cuda/integer_motion_sad_cuda.c:155, :179 | `row_bytes = (size_t)width * bpp`; `pitch = (ptrdiff_t)width * bpp` | `size_t` / `ptrdiff_t` (kernel arg `ptrdiff_t`) | row pitch | 30720 | 65536 | SAFE |
| cuda/integer_motion_sad_cuda.c:186-187 | `DIV_ROUND_UP(width, 16u)` (`core/src/cuda/cuda_helper.cuh:40`) grid dims | `unsigned` | grid_x / grid_y | 960 x 540 | 2048 x 2048 | SAFE (gridDim.y 2048 < 65535) |

### motion_v2_cuda host - CUDA

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| cuda/integer_motion_v2_cuda.c:288 | `s->plane_bytes = vmaf_cuda_motion_sad_plane_bytes(w, h, bpc)` | `size_t` | ring plane bytes (2 or 3 slots) | 265,420,800 B | $2^{31}$ B | SAFE |
| cuda/integer_motion_v2_cuda.c:240 | readback `sizeof(uint64_t)` | `size_t` | one SAD slot | 8 | 8 | SAFE |
| cuda/integer_motion_v2_cuda.c:374-375 | `(double)*sad_host / 256.0 / ((double)s->frame_w * s->frame_h)` | `uint64_t` -> `double`; `double * unsigned` | frame SAD normalisation | SAD $2^{42.98}$ exact in double; w*h exact | SAD $2^{46.0}$ (out-of-range < $2^{53}$) exact | SAFE (< $2^{53}$, no rounding of the integer) |
| cuda/integer_motion_v2_cuda.c:326-330 | `index % s->ring`, `(index + 1u) % s->ring` | `unsigned` | ring slot from frame index | n/a | n/a | SAFE (wraps only at $2^{32}$ frames, the framework's own `unsigned index` limit) |

### motion_cuda host - CUDA

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| cuda/integer_motion_cuda.c:393 | `plane_bytes = vmaf_cuda_motion_sad_plane_bytes(w, h, bpc)` | `size_t` | raw ring plane bytes | 265,420,800 B | $2^{31}$ B | SAFE |
| cuda/integer_motion_cuda.c:402, :407-408 | `sizeof(uint64_t)`; `MOTION_BATCH_DEPTH * sizeof(uint64_t)` | `size_t` | 8 SAD slots, 64 B pinned host | 64 | 64 | SAFE |
| cuda/integer_motion_cuda.c:469-476 | `(double)sad / 256.0 / ((double)w * (double)h)` | `uint64_t` -> `double` | frame SAD normalisation (slot of `sad_host[]`) | exact ($2^{42.98}$) | exact ($2^{46.0}$; out-of-range < $2^{53}$) | SAFE (< $2^{53}$) |
| cuda/integer_motion_cuda.c:830 + :533-534 + :545 | `s->last_batch_boundary = (int)index`; `pending_start = s->last_batch_boundary + 1`; `(int)s->index < pending_start`; `(unsigned)pending_start` | `int` (narrowed from `unsigned index`) | frame counter of the last batch-boundary collect (boundary when `index % 8 == 7`) | n/a (frame count) | n/a (frame count) | DEPENDS (frame count: index $2^{31} - 1$ is a batch boundary (== 7 mod 8), so `last_batch_boundary = INT_MAX` and flush's `INT_MAX + 1` is signed-overflow UB; index >= $2^{31}$ makes `(int)index` an implementation-defined narrowing. Needs >= 2,147,483,647 frames: 414 days at 60 fps, 828 days at 30 fps) |
| cuda/integer_motion_cuda.c:93, :720, :791, :828, :857 | `frame_index++`, `s->frame_index = i + 1`, guard `frame_index > 2` (:267) | `unsigned` | frames processed | n/a | n/a | SAFE (unsigned wrap at $2^{32}$ frames, the framework's own `unsigned index` limit) |
| cuda/integer_motion_cuda.c:809-811, :558, :724 | `index - MOTION_BATCH_DEPTH + 1` (guarded `index >= 8`), `(flush_start - 1u) % 8` (guarded `> 1`), `(i - 1) % 8` (i > batch_start) | `unsigned` | batch slot math | no wrap | no wrap | SAFE (every subtract is guarded) |

### float_motion_cuda - CUDA

Float blur and fp32 row sums are not integer accumulators and are never converted to an integer (skipped per brief).

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| cuda/float_motion/float_motion_score.cu:59 | `plane[gy * stride + gx]` | `int gy` promoted to `ptrdiff_t` (stride `ptrdiff_t`) | 8-bit sample byte offset (pitch = W) | 8639*15360 + 15359 = 132,710,399 | 32767*32768 + 32767 = 1,073,741,823 | SAFE (64-bit) |
| cuda/float_motion/float_motion_score.cu:71 | `plane + gy * stride`, then `[gx]` on `const uint16_t *` | `ptrdiff_t` byte offset | 16-bit row offset (pitch = 2W, `float_motion_cuda.c:407`) | 265,390,080 B | 2,147,418,112 B (+2*32767) | SAFE (would be OVERFLOW@CAP-ONLY in int32) |
| cuda/float_motion/float_motion_score.cu:83-84 | `const int tile_ox = blockIdx.x * FM_BX - FM_RADIUS;` | `unsigned` arithmetic (wraps to 4294967294u at block 0) then converted to `int` | tile origin | -2 .. 15342 | -2 .. 32750 | SAFE (unsigned wrap is defined; the int conversion is implementation-defined before C++20 and modular on nvcc; the motion SAD kernel and the HIP twin cast to int first) |
| cuda/float_motion/float_motion_score.cu:43-50 | `fm_mirror`: `2 * (sup - 1) - idx` | `int` | reflected index | <= 30716 | <= 65532 | SAFE as arithmetic; see Aside A (unclamped negative index) |
| cuda/float_motion/float_motion_score.cu:122 | `cur_blur[(size_t)y * width + (size_t)x]` | `size_t` element index on `float *` | blurred plane write | 132,710,399 elems (530,841,596 B) | $2^{30} - 1$ elems ($2^{32}$ B) | SAFE (64-bit) |
| cuda/float_motion/float_motion_score.cu:159, :162-163 | `y = blockIdx.x * blockDim.x + threadIdx.x`; `cur_blur + (size_t)y * width` | `unsigned`, `size_t` | row base of the row-SAD kernel | 132,695,040 | 1,073,709,056 | SAFE |
| cuda/float_motion_cuda.c:232-235 | `plane_bytes = (size_t)w * h * bpp`; `blur_bytes = (size_t)w * h * sizeof(float)`; `pbytes = (size_t)h * sizeof(float)` | `size_t` | buffer sizes | 265,420,800 / 530,841,600 / 34,560 B | $2^{31}$ / $2^{32}$ / 131,072 B | SAFE (size_t 64-bit; blur bytes overflow uint32 at cap, not used) |
| cuda/float_motion_cuda.c:407, :423-424, :442 | `plane_pitch = (ptrdiff_t)s->frame_w * bpp`; `(size_t)plane_pitch`; `(size_t)s->frame_h * sizeof(float)` | `ptrdiff_t` / `size_t` | pitch, DtoH size | 30720 / 34,560 | 65536 / 131,072 | SAFE |
| cuda/float_motion_cuda.c:357-358, :385 | grid `(frame_w + 15u) / 16`, `(frame_h + 127u) / 128` | `unsigned` | grid dims | 960 x 540; 68 | 2048 x 2048; 256 | SAFE |
| float_motion_sad.h:163 (called from cuda/float_motion_cuda.c:450) | `(float)(int)(w * h)` | `unsigned` product, cast to `int` | pixel count divisor | 132,710,400 | $2^{30}$ | SAFE ($2^{30}$ < $2^{31}$; product formed in unsigned, no wrap) |

### motion SAD kernel (motion_hip + motion_v2_hip) - HIP

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| hip/integer_motion_v2/motion_v2_score.hip:102 | `prev + gy * prev_stride`, `cur + gy * cur_stride`, then `[col]` on `const T *` | `int gy` promoted to `ptrdiff_t`; `int col` | packed row offset (pitch = W*bpp, `integer_motion_sad_hip.c:178`) | 265,420,798 B | 2,147,483,646 B | SAFE (64-bit; would be OVERFLOW@CAP-ONLY in int32) |
| hip/integer_motion_v2/motion_v2_score.hip:89-90 | `(int)(blockIdx.x * MV2_BLOCK_X) - MV2_RADIUS` | `unsigned` cast to `int` first | tile origin | <= 15360 | <= 32768 | SAFE |
| hip/integer_motion_v2/motion_v2_score.hip:97-100 (+ hip/hip_tile_index.h:38-55) | `vmaf_hip_reflect_101` + `vmaf_hip_tile_index` | `int` | reflected, clamped index | <= 30718 | <= 65534 | SAFE |
| hip/integer_motion_v2/motion_v2_score.hip:101-102 | `s_diff[ty][tx] = mv2_sample - mv2_sample` | `int` stored in `int32_t` | one diff | abs <= 65535 | same | SAFE |
| hip/integer_motion_v2/motion_v2_score.hip:113-116 (8bpc, VAcc int32_t, :178) | `sum += (VAcc)mv2_filter_d[yf] * (VAcc)s_diff[..]` | `int32_t` | 5 taps, sum <= 65536*255 | <= 16,711,680 (+128) | same | SAFE ($2^{24}$ < $2^{31}$) |
| hip/integer_motion_v2/motion_v2_score.hip:113-116 (16bpc, VAcc int64_t, :187) | same | `int64_t` | 5 taps, sum <= 65536*65535 | <= 4,294,901,760 | same | SAFE ($2^{32}$ < $2^{63}$) |
| hip/integer_motion_v2/motion_v2_score.hip:118-120 | `(int32_t)((sum + round_y) >> shift_y)` | `int32_t` | rounded vertical value | abs <= 65535; out-of-range 8,388,480 | same | SAFE ($2^{23}$ < $2^{31}$) |
| hip/integer_motion_v2/motion_v2_score.hip:135-139 | `blurred += (int64_t)mv2_filter_d[xf] * (int64_t)v` | `int64_t` | 5 taps | <= 4,294,901,760; out-of-range 5.5e11 | same | SAFE ($2^{39}$ < $2^{63}$) |
| hip/integer_motion_v2/motion_v2_score.hip:141-142 | `h = (blurred + (1 << 15)) >> 16`, abs | `int64_t` | per-pixel term | <= 65535 (out-of-range 8,388,480) | same | SAFE |
| hip/integer_motion_v2/motion_v2_score.hip:149-150 | `abs_h += __shfl_down(abs_h, off)` | `int64_t` | one wave: 32 (RDNA) or 64 (GCN/CDNA) lanes, term <= 65535 | <= 4,194,240 ($2^{22}$); out-of-range $2^{29}$ | same | SAFE ($2^{29}$ < $2^{63}$) |
| hip/integer_motion_v2/motion_v2_score.hip:153-154 | `atomicAdd((unsigned long long *)sad, (unsigned long long)abs_h)` | `unsigned long long` (buffer `uint64_t`) | one add per wave (4 or 8 per 256-thread block); sum over N px | 8,697,176,064,000 ($2^{42.98}$); out-of-range $2^{49.98}$ | $2^{46.0}$; out-of-range < $2^{53}$ | SAFE ($2^{46}$ < $2^{64}$; zeroed per frame `integer_motion_sad_hip.c:172`) |
| hip/integer_motion_v2/motion_v2_score.hip:128-129 | `(int)(blockIdx.x * blockDim.x + threadIdx.x)` | `int` | global x / y | <= 15375 | <= 32783 | SAFE |
| hip/integer_motion_sad_hip.c:109-112, :219 | `(size_t)width * height * bpp` (plane bytes, D2D copy size) | `size_t` | packed luma plane | 265,420,800 B | $2^{31}$ B | SAFE |
| hip/integer_motion_sad_hip.c:178 | `(ptrdiff_t)plane_bytes(width, 1u, bpc)` | `ptrdiff_t` | row pitch kernel arg | 30720 | 65536 | SAFE |
| hip/integer_motion_sad_hip.c:189-190 | `(w + 15u) / 16u`, `(h + 15u) / 16u` | `unsigned` | grid dims | 960 x 540 | 2048 x 2048 | SAFE |

### motion_hip / motion_v2_hip host - HIP

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| hip/integer_motion_hip.c:442 | `s->plane_bytes = (size_t)w * h * (bpc <= 8u ? 1u : 2u)` | `size_t` | prev_luma plane (1 or 2) | 265,420,800 B | $2^{31}$ B | SAFE |
| hip/integer_motion_v2_hip.c:299 | same | `size_t` | prev_luma plane | 265,420,800 B | $2^{31}$ B | SAFE |
| hip/integer_motion_hip.c:211-214 | `(double)(sad / 256.) / ((double)w * (double)h)` | `uint64_t` -> `double` | frame SAD normalisation | exact | exact (< $2^{53}$) | SAFE |
| hip/integer_motion_v2_hip.c:380-381 | `(double)*sad_host / 256.0 / ((double)frame_w * (double)frame_h)` | `uint64_t` -> `double` | frame SAD normalisation | exact | exact | SAFE |
| hip/integer_motion_hip.c:124, :547, :593, :650 | `frame_index++`, guard `> 2u`, `== 0u` | `unsigned` | frames processed | n/a | n/a | SAFE (wraps at $2^{32}$ frames, the framework's own `unsigned index` limit) |
| hip/integer_motion_hip.c:330; hip/integer_motion_v2_hip.c:243 | `index % s->depth` | `unsigned` | ring slot | n/a | n/a | SAFE |

### float_motion_hip - HIP

Float blur, fp32 transposed differences and fp32 row sums are not integer accumulators (skipped per brief).

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| hip/float_motion/float_motion_score.hip:132 | `ref + gy * ref_stride`, then `[gx]` | `int gy` promoted to `ptrdiff_t` | packed row offset (pitch W*bpp, `float_motion_hip.c:453`) | 265,420,798 B | 2,147,483,646 B | SAFE (64-bit) |
| hip/float_motion/float_motion_score.hip:122-123, :101-104 | `(int)(blockIdx.x * FM_BX) - FM_RADIUS`; `fm_tile_index` (reflect + clamp) | `int` | tile origin / index | <= 30718 | <= 65534 | SAFE |
| hip/float_motion/float_motion_score.hip:165-170 | `off = (size_t)y * width + (size_t)x` | `size_t` | blur plane element index | 132,710,399 | $2^{30} - 1$ | SAFE |
| hip/float_motion/float_motion_rows.h:57-62 | `groups * width * VMAF_HIP_FLOAT_MOTION_ROW_GROUP`, `groups = ((size_t)height + 63) / 64` | `size_t` | transposed diff plane floats | 135*15360*64 = 132,710,400 | 512*32768*64 = $2^{30}$ ($2^{32}$ B at `float_motion_hip.c:612`) | SAFE (size_t; bytes overflow uint32 at cap, not used) |
| hip/float_motion/float_motion_rows.h:66-71 | `((group * width) + x) * 64 + lane` | `size_t` | transposed index | < 132,710,400 | < $2^{30}$ | SAFE |
| hip/float_motion/float_motion_rows.h:86-89 | `diff + diff_index(0, y, width)`; `row[(size_t)j * 64]` | `size_t` | row walk of one row sum | < $2^{27}$ | < $2^{30}$ | SAFE |
| hip/float_motion/float_motion_rows.h:110-120 | `(int)vmaf_hip_float_motion_reflect(floorf(x), right)`; `(size_t)y1 * width` | float -> `int`; `size_t` | bilinear sample index (scale-1) | x1,y1 <= 15359 | <= 32767 | SAFE (float values <= 32767.5, int conversion in range) |
| hip/float_motion_hip.c:368-369 | `(unsigned)((double)w * 0.5 + 0.5)` | `double` -> `unsigned` | scaled plane size | 7680 | 16384 | SAFE |
| hip/float_motion_hip.c:371-373, :382, :396 | `p->off1 = offset + h`; `return p->off1 + p->rows1`; `s->row_count` | `unsigned` | readback rows: luma h + sh, + 2 chroma (h + sh) with motion_add_uv | <= 8640+4320+2*(8640+4320) = 38,880 | <= 147,456 | SAFE |
| hip/float_motion_hip.c:595, :626, :689 | `(size_t)s->row_count * sizeof(float)` | `size_t` | readback bytes | 155,520 | 589,824 | SAFE |
| hip/float_motion_hip.c:607-615 | `pixels = (size_t)p->w * p->h`; `pixels * sizeof(float)`; `diff_count(..) * sizeof(float)` | `size_t` | blur ping-pong + diff planes per plane | 530,841,600 B each | $2^{32}$ B each | SAFE (size_t 64-bit) |
| hip/float_motion_hip.c:453, :558 | `(ptrdiff_t)((size_t)p->w * bpp)`; `(size_t)s->plane[c].w * bpp` | `ptrdiff_t` / `size_t` | pitch / upload row bytes | 30720 | 65536 | SAFE |
| hip/float_motion_hip.c:450-451, :484-485, :507 | `(w + 15u) / 16`, `(sw + 15u) / 16`, `(height + 63u) / 64` | `unsigned` | grid dims | 960 x 540; 135 | 2048 x 2048; 512 | SAFE |
| float_motion_sad.h:163 (via hip/float_motion/float_motion_rows.h:161, :164) | `(float)(int)(w * h)` for luma, chroma and scale-1 planes | `unsigned` product cast to `int` | pixel count divisor | 132,710,400 | $2^{30}$ | SAFE ($2^{30}$ < $2^{31}$) |

### Shared headers

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| integer_motion.h:53 (included by cuda/integer_motion_cuda.h:25; `edge_16` is not called by any G3a twin) | `accum += filter[k] * src[i_tap * stride + j_tap]` | `uint16_t * uint16_t` promotes to `int`; `uint32_t accum`; `int` index | 5 taps, term <= 26386*65535 = 1,729,206,510 (< INT32_MAX, no UB); sum <= 65536*65535 | accum 4,294,901,760; index 132,710,399 (stride ~ W) | accum same (< UINT32_MAX); index 1,073,741,823 (stride ~ W) | SAFE (not used by the twins; recorded for completeness; index stays < $2^{31}$ only while the stride in samples stays < 65536) |

### Aside A: CUDA float_motion tile index can go negative (out-of-bounds read, not an integer overflow)

`cuda/float_motion/float_motion_score.cu:43-50` (`fm_mirror`) reflects once and does not clamp, and
`fm_load_tile` (:83-90) loads a fixed 20x20 tile for every block, padding threads included. The largest tile
index on an axis of extent `sup` is `16 * (ceil(sup / 16) - 1) + 17`; it maps below zero when that exceeds
`2 * (sup - 1)`, i.e. for `sup` in {3..9, 17}. Width or height 3 gives index -13 (13 rows before the
`ref_in` allocation when it is the row axis), 17 gives -1. Those samples feed no valid output (the
consumed taps stay in range from 3x3 up), but the load reads outside the buffer. The HIP twin clamps
(`hip/float_motion/float_motion_score.hip:101-104`, ADR-1381), and the CUDA motion SAD kernel clamps
(`cuda/cuda_tile_index.h`). `core/test/test_cuda_motion_tiny_frames.c:67` covers 3x3 and 17x17 for motion_cuda
and motion_v2_cuda only, not float_motion_cuda. The init guard only refuses below 3x3
(`cuda/float_motion_cuda.c:262`).

### Aside B: CPU reference row accumulator (outside G3a, for the CPU group)

`integer_motion.c:193` and `:243` hold a row's SAD in `uint32_t row_sad`. In range it is <= 32768 \* 65535 =
2,147,450,880 (< $2^{32}$), so it is safe up to the cap. If a 16-bit container carries samples above $2^{\mathrm{bpc}} - 1$
(bpc 9..15; sample range against bpc was not checked in this audit), `abs(h)` reaches 8,388,480 at bpc 9 and
the row sum wraps from W >= 513 (bpc 10: abs(h) <= 4,194,240, W >= 1025). The GPU twins add in 64 bits
and would then differ from the CPU.

### Coverage

All paths under `core/src/feature/`. Every file was read in full, or grepped with the brief's pattern
list and every hit read in context (for the 600-900-line host files).

| file | rows |
|---|---|
| cuda/float_motion_cuda.c | 3 (+ float_motion_sad.h call) |
| cuda/float_motion_cuda.h | none (PTX symbol only) |
| cuda/float_motion/float_motion_score.cu | 6 (+ Aside A) |
| cuda/integer_motion_cuda.c | 6 |
| cuda/integer_motion_cuda.h | none (includes integer_motion.h; see Shared headers) |
| cuda/integer_motion_sad_cuda.c | 3 |
| cuda/integer_motion_sad_cuda.h | none (declarations; `sad` documented as uint64) |
| cuda/integer_motion_v2_cuda.c | 4 |
| cuda/integer_motion_v2_cuda.h | none (PTX symbol only) |
| cuda/integer_motion_v2/motion_v2_score.cu | 13 |
| cuda/cuda_tile_index.h (helper, read for index math) | folded into the motion_v2_score.cu reflect row |
| hip/float_motion_hip.c | 6 |
| hip/float_motion_hip.h | none (HSACO symbol only) |
| hip/float_motion/float_motion_rows.h | 4 (+ float_motion_sad.h call) |
| hip/float_motion/float_motion_score.hip | 3 |
| hip/integer_motion_hip.c | 4 |
| hip/integer_motion_sad_hip.c | 3 |
| hip/integer_motion_sad_hip.h | none (declarations) |
| hip/integer_motion_v2_hip.c | 3 (2 shared with integer_motion_hip.c rows) |
| hip/integer_motion_v2_hip.h | none (HSACO symbol only) |
| hip/integer_motion_v2/motion_v2_score.hip | 12 |
| hip/hip_tile_index.h (helper, read for index math) | folded into the reflect rows |
| float_motion_sad.h | 1 (used by both CUDA and HIP float twins) |
| integer_motion.h | 1 (edge_16, not called by the twins; `filter[]` taps match the twins') |
| motion_tools.h | none (float filter constants only) |
| motion_blend_tools.h | none (double blend only) |
| integer_motion.c (CPU reference, tap values and shifts) | Aside B only |
| core/src/hip/shared_frame.{h,c} (checked: HIP luma planes are packed, so the packed pitch the SAD kernel gets is right) | none |

## G3b: PSNR (integer + float), moment, CIEDE, tile-index headers (CUDA + HIP)

Repo: master @ 571565a47. Paths are relative to `core/src/feature/`.
Envelope: 16K N = 132,710,400 ($2^{26.98}$); cap N = $2^{30}$. Max 16-bit term $65535^{2}$ = 4,294,836,225. Its fp32
square is 4,294,836,224 (the twins' float-square terms). Both values are below $2^{32}$. Row W <= 32768 = $2^{15}$.

### float_psnr

#### CUDA (cuda/float_psnr/float_psnr_score.cu, cuda/float_psnr_cuda.c, float_psnr_rows.h)

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| cuda/float_psnr/float_psnr_score.cu:47-48 | `(unsigned long long)__fmul_rn(diff, diff)` (`diff = (float)(ref - dis)`, int subtraction) | `float` -> `unsigned long long` | one term, the fp32 square of a sample difference <= 65535: <= 4,294,836,224 | 4,294,836,224 | same | SAFE (< $2^{32}$, conversion in range) |
| cuda/float_psnr/float_psnr_score.cu:59-61 | `v += __shfl_down_sync(..., v, off)` (warp) | `unsigned long long` | 32 terms < $2^{32}$ | < $2^{37}$ | same | SAFE ($2^{37}$ < $2^{64}$) |
| cuda/float_psnr/float_psnr_score.cu:66-70 | `total += s_warps[i]` (block, FPSNR_BX*FPSNR_BY = 256 threads, 8 warps) | `unsigned long long` | 256 terms < $2^{32}$ (one row segment) | < $2^{40}$ | same | SAFE ($2^{40}$ < $2^{64}$) |
| cuda/float_psnr/float_psnr_score.cu:82-83,86-88 | `x`, `y`, `ref + y * ref_stride`, `[x]` | `int`, `int * ptrdiff_t` -> `ptrdiff_t` | index math: x < gridDim.x*256 <= W+255, byte offset y*pitch | offset 8640*30720 = 265,420,800 | 32768*65536 = $2^{31}$ (64-bit) | SAFE (64-bit offset; int x <= 33,023) |
| cuda/float_psnr/float_psnr_score.cu:94 | `block_idx = blockIdx.y * gridDim.x + blockIdx.x` | `unsigned` | block count H \* ceil(W/256) | 8640\*60 = 518,400 | 32768\*128 = 4,194,304 | SAFE ($2^{22}$ < $2^{32}$) |
| cuda/float_psnr_cuda.c:164 | `plane_bytes = (size_t)w * h * bpp` | `size_t` | staged plane size | 265,420,800 B | $2^{31}$ B | SAFE (size_t) |
| cuda/float_psnr_cuda.c:165-168 | `wg_count = gx * gy`, `partials_bytes = (size_t)wg_count * 8` | `unsigned`, `size_t` | block count, readback bytes | 518,400 / 4,147,200 B | 4,194,304 / 33,554,432 B | SAFE |
| cuda/float_psnr_cuda.c:261 | `plane_pitch = (ptrdiff_t)frame_w * 2` | `ptrdiff_t` | packed row pitch passed as the kernel's `ptrdiff_t` stride | 30,720 | 65,536 | SAFE |
| float_psnr_rows.h:244-246 | `row += segments[(size_t)y * per_row + x]` (host, called at float_psnr_cuda.c:310) | `uint64_t` (index `size_t`) | one row: W terms < $2^{32}$ (per_row = ceil(W/256) segments < $2^{40}$) | 15360*4,294,836,224 = 6.6e13 ($2^{45.9}$) | 32768 terms -> < $2^{47}$ | SAFE ($2^{47}$ < $2^{64}$; also < $2^{53}$, so `(double)row` at :247 is exact) |
| cuda/float_psnr_cuda.c:311 | `1u << (s->bpc - 8u)` | `unsigned` | scaler, bpc in {8,10,12,16} (checked at :137-153) | 256 | 256 | SAFE |

Grid aside: gridDim.y = H (FPSNR_BY = 1), <= 32768, below the 65535 y-dimension limit.

#### HIP (hip/float_psnr/float_psnr_score.hip, hip/float_psnr_hip.c)

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| hip/float_psnr/float_psnr_score.hip:99-103 | `(uint32_t)(diff * diff)` (fp32 product, `diff = (float)(ref - dis)`) | `float` -> `uint32_t` | one term <= 4,294,836,224 | 4,294,836,224 | same | SAFE (< $2^{32}$, conversion in range) |
| hip/float_psnr/float_psnr_score.hip:66-72, 77-93 (8bpc use :121-131) | `v += __shfl_down(v, off)`; `total += s_warps[i]` | `uint32_t` | 256 terms <= $255^{2}$ = 65,025 | 16,646,400 | same | SAFE (< $2^{24}$ < $2^{32}$) |
| hip/float_psnr/float_psnr_score.hip:151,161,166 (16bpc kernel, `split = bpc > 12u` false, bpc 10/12) | `my_lo = square`; `total_lo = fpsnr_block_sum(...)` | `uint32_t` | 256 terms; in contract each <= $(2^{\mathrm{bpc}} - 1)^{2}$ <= $4095^{2}$ = 16,769,025 | 256*$4095^{2}$ = 4,292,870,400 | same (per block, not size dependent) | DEPENDS: safe only if every sample <= $2^{\mathrm{bpc}} - 1$. The margin to UINT32_MAX is 2,096,895. No sample-range check exists in picture.c or libvmaf.c (grepped). A 10- or 12-bit picture whose uint16 codewords differ by >= 4096 across all 256 pixels of a row segment wraps the block sum (256*$4096^{2}$ = $2^{32}$) with no error. The CUDA twin (uint64) and the CPU (double) do not wrap |
| hip/float_psnr/float_psnr_score.hip:151,161-162,166,173 (bpc 16, split) | `my_lo = square & 0xFFFF`, `my_hi = square >> 16`; two `fpsnr_block_sum` | `uint32_t` | 256 terms: lo <= 65,535, hi <= 65,533 | 16,776,960 each | same | SAFE (< $2^{24}$) |
| hip/float_psnr/float_psnr_score.hip:131,177 | `(unsigned long long)total_lo + ((unsigned long long)total_hi << 16)` | `unsigned long long` | recombined block sum | < $2^{24}$ + $2^{40}$ | same | SAFE (< $2^{41}$) |
| hip/float_psnr/float_psnr_score.hip:118-119,124,149-150,158-159 | `x`, `y` (`int`), `(ptrdiff_t)y * ref_stride + x` | `int`, `ptrdiff_t` | index math | 265,420,800 B offset | $2^{31}$ B (64-bit) | SAFE |
| hip/float_psnr/float_psnr_score.hip:130,176 | `block_idx = blockIdx.y * gridDim.x + blockIdx.x` | `unsigned` | H \* ceil(W/256) | 518,400 | 4,194,304 | SAFE |
| hip/float_psnr_hip.c:174,200-201,316 | `wg_count = gx*gy`, `(size_t)wg_count * 8` | `unsigned`, `size_t` | block count, bytes | 518,400 / 4,147,200 B | 4,194,304 / 33,554,432 B | SAFE |
| hip/float_psnr_hip.c:222 | `(ptrdiff_t)(s->frame_w * bpp)` (`bpp` is `size_t`) | `size_t` -> `ptrdiff_t` | packed pitch | 30,720 | 65,536 | SAFE |
| hip/float_psnr_hip.c:394-397 -> float_psnr_rows.h:244-246 | per-row exact `uint64_t` row sum; `1u << (bpc - 8u)` | `uint64_t`, `unsigned` | as the CUDA row above | < $2^{46}$ | < $2^{47}$ | SAFE |

Note: the kernel doc comments (float_psnr_score.hip:107-111, 139-141) describe a `partials[2 * block]` / `[2 * block + 1]`
layout. The code writes one u64 per block (`partials[block_idx]`), and the host reads one u64 per block. The comments are
out of date. This does not cause an overflow.

### psnr (integer)

#### CUDA (cuda/integer_psnr/psnr_score.cu, cuda/integer_psnr_cuda.c, psnr_score.h)

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| cuda/integer_psnr/psnr_score.cu:98-99 | `diff = (int64_t)ref - (int64_t)dis; se += (uint64_t)(diff * diff)` | `int64_t` product, `uint64_t` sum | per thread PSNR_COLS_PER_THREAD = 8 terms <= 4,294,836,225 | < $2^{35}$ | same | SAFE (product in int64: no `uint16*uint16 -> int` promotion) |
| cuda/integer_psnr/psnr_score.cu:46-48 | `v += __shfl_down_sync(...)` (warp) | `unsigned long long` | 32 threads x 8 = 256 terms | < $2^{40}$ | same | SAFE |
| cuda/integer_psnr/psnr_score.cu:53-56 | 2nd-level shuffle over the 8 warp sums | `unsigned long long` | 2048 terms per block (32x8 threads x 8 cols) | 8,795,824,588,800 (< $2^{43}$) | same | SAFE |
| cuda/integer_psnr/psnr_score.cu:58 | `atomicAdd(sse, v)` per-plane accumulator, one atomic per block | `unsigned long long` | N terms <= 4,294,836,225 (4:4:4: every plane N) | 569,969,433,354,240,000 ($2^{58.98}$) | 4,611,545,282,012,774,400 (< $2^{62}$) | SAFE (< $2^{62}$ < $2^{64}$) |
| cuda/integer_psnr/psnr_score.cu:78,87,92,96 | `(size_t)y * stride`; `y`, `x0`, `x` | `size_t` / `unsigned` | row byte offset, column index <= W+255 | 265,420,800 B | $2^{31}$ B (64-bit) | SAFE |
| cuda/integer_psnr_cuda.c:180-181,241-242 | grid dims `DIV_ROUND_UP(w,256)`, `DIV_ROUND_UP(h,8)`; `cw = (w+ss)>>ss` | `unsigned` | launch geometry | 60 x 1080 | 128 x 4096 | SAFE (gridDim.y <= 4096 < 65535) |
| cuda/integer_psnr_cuda.c:392-394 | `s->apsnr_sse[p] += sse` (clip level, `enable_apsnr`) | `uint64_t` | one per-plane frame SSE per frame, each <= N*$65535^{2}$ | per frame 5.70e17 ($2^{58.98}$); u64 wraps on frame 33 | per frame 4.61e18 (< $2^{62}$); wraps on frame 5 | DEPENDS (frame count): 16-bit max-diff wraps on frame 2072 at 1080p ($2^{52.98}$/frame), frame 122 at 8K DCI ($2^{57.08}$), frame 33 at 16K, frame 5 at cap. Max-diff wrap frame at 12 bit: 530,502 / 31,085 / 8,290 / 1,025. At 10 bit: 8.5M / 498,076 / 132,821 / 16,417. At 8 bit: 136.8M / 8.0M / 2.1M / 264,205. The wrap gives no error, and the `apsnr_*` aggregate comes out too high. The CPU's `integer_psnr.c:64,211,249` uses the same `uint64_t`, so the twin and the CPU wrap identically |
| cuda/integer_psnr_cuda.c:395 | `s->apsnr_n_pixels[p] += (uint64_t)height * width` | `uint64_t` | N per frame | $2^{64}$/N = 1.39e11 frames | $2^{34}$ frames (1.7e10) | SAFE (>= $2^{34}$ frames at cap, about 18 years at 30 fps) |
| psnr_score.h:33,46 | `255u << (bpc - 8u)`, `(1u << bpc) - 1u`, `(6u * bpc) + 12u` | `uint32_t` / `unsigned` | constants, bpc <= 16 | 65,535 / 108 | same | SAFE |

`vmaf_psnr_aggregate` (psnr_score.h:89-97) and the `mse` at :397 convert `uint64_t` to `double`. They have no integer
math.

#### HIP (hip/integer_psnr/psnr_score.hip, hip/integer_psnr_hip.c)

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| hip/integer_psnr/psnr_score.hip:63-66,106-109 | `diff = r - d` (`int64_t`), `my_se = (uint64_t)(diff * diff)` | `int64_t` / `uint64_t` | one term per thread <= 4,294,836,225 | 4,294,836,225 | same | SAFE |
| hip/integer_psnr/psnr_score.hip:71-81,112-122 | wave reduce on lo/hi uint32 halves rebuilt to `uint64_t sum = self + other` | `uint64_t` (`uint32_t` halves) | <= 64 lanes (wave64) | < $2^{38}$ | same | SAFE (the halves carry the full 64-bit value) |
| hip/integer_psnr/psnr_score.hip:87,128 | `atomicAdd(sse, warp_sum)` per wave | `unsigned long long` | N terms | $2^{58.98}$ | < $2^{62}$ | SAFE |
| hip/integer_psnr/psnr_score.hip:58-59,63-64,101-102,106-107 | `x`, `y` (`int`), `(ptrdiff_t)y * ref_stride + x` | `int` / `ptrdiff_t` | index math | 265,420,800 B | $2^{31}$ B (64-bit) | SAFE |
| hip/integer_psnr_hip.c:227-229,295,351-352 | `(ptrdiff_t)pw * bpp`, grid `(pw+15)/16`, `(size_t)width*bpp`, `cw/ch` | `ptrdiff_t`/`unsigned`/`size_t` | pitch, grid | 30,720 / 960x540 | 65,536 / 2048x2048 | SAFE |
| hip/integer_psnr_hip.c:476 | `s->apsnr_sse[p] += sse` | `uint64_t` | per-plane frame SSE per frame | wraps on frame 33 | wraps on frame 5 | DEPENDS (frame count): same derivation and wrap frames as CUDA integer_psnr_cuda.c:392-394 (1080p 2072, 8K 122, 16K 33, cap 5 at 16-bit max-diff). The CPU's integer_psnr.c uses the same type |
| hip/integer_psnr_hip.c:477 | `s->apsnr_n_pixels[p] += (uint64_t)height * width` | `uint64_t` | N per frame | 1.39e11 frames | $2^{34}$ frames | SAFE |

### float_moment

#### CUDA (cuda/integer_moment/moment_score.cu, cuda/integer_moment_cuda.c)

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| cuda/integer_moment/moment_score.cu:106-111 | `moment_float_square(v)`: `(unsigned long long)__fmul_rn((float)v, (float)v)` | `float` -> `unsigned long long` | one term, v <= 65535: <= 4,294,836,224 | 4,294,836,224 | same | SAFE (< $2^{32}$) |
| cuda/integer_moment/moment_score.cu:117-120 | `(unsigned long long)v * (unsigned long long)v` (8 bpc) | `unsigned long long` | <= 65,025 | 65,025 | same | SAFE |
| cuda/integer_moment/moment_score.cu:153-156 | `m.v[0..1] += r/d`; `m.v[2..3] += sample_square(r/d)` per thread | `unsigned long long` | 8 terms; first <= 65535, second <= 4,294,836,224 | < $2^{19}$ / < $2^{35}$ | same | SAFE |
| cuda/integer_moment/moment_score.cu:80-82 | warp shuffle per sum | `unsigned long long` | 256 terms | < $2^{24}$ / < $2^{40}$ | same | SAFE |
| cuda/integer_moment/moment_score.cu:92-96 | `sum += s_warp[lid][w]`; `atomicAdd(&acc[lid], sum)` (4 accumulators, one atomic per block) | `unsigned long long` | block 2048 terms; frame N terms | ref1/dis1 8,697,176,064,000 ($2^{42.98}$); ref2/dis2 569,969,433,221,529,600 ($2^{58.98}$) | 70,367,670,435,840 ($2^{46}$); 4,611,545,280,939,032,576 (< $2^{62}$) | SAFE (< $2^{62}$ < $2^{64}$) |
| cuda/integer_moment/moment_score.cu:131-132,141,146,149 | `(size_t)y * pic.stride[0]`; `y`, `x0`, `x` | `size_t` / `unsigned` | index math | 265,420,800 B | $2^{31}$ B (64-bit) | SAFE |
| cuda/integer_moment_cuda.c:83-84 | grid `DIV_ROUND_UP(w,256)`, `DIV_ROUND_UP(h,8)` | `unsigned` | launch geometry | 60 x 1080 | 128 x 4096 | SAFE |
| cuda/integer_moment_cuda.c:119,141-146 | `(size_t)2 * frame_h * 8`; `rows * sizeof(uint64_t/int/2*int64_t)` | `size_t` | per-row buffers | 2*8640 rows, 276 KB | 2*32768 rows, 1 MiB | SAFE |
| cuda/integer_moment_cuda.c:123,127 | grid `(frame_h, 2)` x 256 lanes for row_totals / row_units | `unsigned` launch dims | gridDim.x = H | 8640 | 32768 | SAFE (x-dim limit $2^{31} - 1$) |

`(double)sums_host[k]` at integer_moment_cuda.c:329-332 converts uint64 to double. It is not integer math. Above $2^{53}$,
exactness is a parity question that ADR-1497 handles; it does not cause a wrap.

#### HIP (hip/float_moment/moment_score.hip, hip/float_moment_hip.c)

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| hip/float_moment/moment_score.hip:86-91 | `moment_float_square(v)`: `(uint64_t)(sample * sample)` | `float` -> `uint64_t` | <= 4,294,836,224 | 4,294,836,224 | same | SAFE |
| hip/float_moment/moment_score.hip:120-125 | `r * r`, `d * d` (8 bpc) | `uint64_t` | <= 65,025 | 65,025 | same | SAFE |
| hip/float_moment/moment_score.hip:64-78 (used :128-131,172-175) | wave reduce on lo/hi uint32 halves rebuilt to `uint64_t` | `uint64_t` | <= 64 lanes, one term each | < $2^{22}$ / < $2^{38}$ | same | SAFE |
| hip/float_moment/moment_score.hip:135-138,179-182 | `atomicAdd(&sums[k], ...)` per wave | `unsigned long long` | N terms | $2^{42.98}$ / $2^{58.98}$ | $2^{46}$ / < $2^{62}$ | SAFE |
| hip/float_moment/moment_score.hip:114-115,120-121,156-157,162-165 | `x`, `y` (`int`), `(ptrdiff_t)y * ref_stride + x` | `int` / `ptrdiff_t` | index math | 265,420,800 B | $2^{31}$ B | SAFE |
| hip/float_moment_hip.c:175-181,200,222-226,240-241,259,261 | `rows * sizeof(...)`, `frame_h > sum_rows` guard, grid `(frame_h, 2)`, `(ptrdiff_t)(frame_w * bpp)`, `sums_bytes` | `size_t`/`unsigned`/`ptrdiff_t` | sizes, geometry | 1,080x... / 30,720 | 32768 / 65,536 | SAFE |

#### Shared (float_moment_sum.h, float_moment_sum_gpu.h; ordered_sum.h read for the units helpers). Used by the CUDA and HIP twins

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| float_moment_sum.h:105-110 | `(uint64_t)w * (uint64_t)h > (2^53 >> (2u * bpc))` | `uint64_t` | N vs threshold; bpc in (8,16], shift <= 32 | 132,710,400 | $2^{30}$ | SAFE |
| float_moment_sum.h:114-123 | `vmaf_moment_sum_shift`: loop `shift++` bounded by 11 | `unsigned` | bit length - 53 of x < $2^{64}$ | <= 6 | <= 9 | SAFE (11 suffices for any uint64) |
| float_moment_sum.h:143-150 | `exact = sum + (uint64_t)term`; `(uint64_t)round_shifted(exact, shift).even << shift` | `uint64_t`; `int64_t` -> `uint64_t` | the CPU's running sum (< $2^{62}$) plus one term < $2^{32}$; rebuilt mantissa <= $2^{53}$ shifted by shift <= 9 | < $2^{59}$ | <= $2^{62}$ | SAFE (< $2^{64}$) |
| float_moment_sum.h:168-178 | `after = before + totals[i]` (plan prefix) | `uint64_t` | exact frame sum: N terms < $2^{32}$ | $2^{58.98}$ | < $2^{62}$ | SAFE |
| float_moment_sum.h:183-188 | `round_shifted((uint64_t)term, plan - 52)` | `uint64_t` -> `int64_t` | term < $2^{32}$, shift >= 1 | < $2^{31}$ | same | SAFE |
| float_moment_sum.h:197-209 -> ordered_sum.h:249-262 | `vmaf_ordsum_then`: `a.even + b.*`, `a.odd + b.*`, capped at VMAF_ORDSUM_UNFIT = $2^{54}$ | `int64_t` | 256-run tree composition of increments, each capped | pre-cap <= $2^{55}$ | same | SAFE ($2^{55}$ < $2^{63}$) |
| float_moment_sum.h:214-221 | `len = (width + 255)/256`; `lo = lane * len`; `lo + len` | `unsigned` | run bounds, lane < 256 | lo <= 255*60 | <= 255*128 + 128 = 32,768 | SAFE |
| float_moment_sum.h:230-251 | `s + total` (guarded `total <= 2^53 - s`); `end = m + units`; `end << shift` | `uint64_t` | m < $2^{53}$, units <= $2^{54}$ (capped), end <= $2^{53}$ checked before the shift, shift = plan - 52 <= 10 | <= $2^{59}$ | <= $2^{62}$ (plan <= 61 since sum < $2^{62}$); <= $2^{63}$ even at plan 62 | SAFE (< $2^{64}$) |
| float_moment_sum.h:329 | `luma + (size_t)row * stride` | `size_t` | row offset | 265,420,800 B | $2^{31}$ B | SAFE |
| float_moment_sum.h:337-339 | `total += (uint64_t)VMAF_MOMENT_SQUARE(line[x])` (lane stride 256) | `uint64_t` | ceil(W/256) <= 128 terms < $2^{32}$ | < $2^{38}$ | < $2^{39}$ | SAFE |
| float_moment_sum.h:348-353 | `term = (uint32_t)VMAF_MOMENT_SQUARE(...)`; `total += term` | `uint64_t` -> `uint32_t`; `uint64_t` | narrowing from <= 4,294,836,224 (fits u32); run <= 128 terms | < $2^{38}$ | < $2^{39}$ | SAFE (no truncation) |
| float_moment_sum.h:445-446 | `rows - first < 256 ? rows : first + 256` | `unsigned` | first <= rows by construction | <= 8640+256 | <= 33,024 | SAFE |
| float_moment_sum_gpu.h:61-66 | `totals[lane] += totals[lane + step]` (256-lane tree) | `uint64_t` | one row: W terms < $2^{32}$ | < $2^{46}$ | < $2^{47}$ | SAFE |
| float_moment_sum_gpu.h:69,82,90,96,113,117-118,129-130,153 | `(size_t)plane * a.height + row`, `base + first + lane`, `(size_t)2u * at` | `size_t` | buffer indices <= 2*H*2 | < $2^{16}$ | < $2^{18}$ | SAFE |
| float_moment_sum_gpu.h:83,93 | `prefix` via `vmaf_moment_sum_plan_batch` | `uint64_t` | exact frame sum | $2^{58.98}$ | < $2^{62}$ | SAFE |
| float_moment_sum_gpu.h:162,171-172,183,185 | `rounds = h + h/256 + 2`; `what * 256`; `2u * base`; `what + 1u` | `unsigned` / `size_t` | loop bound, batch row index | 8,675 | 32,898 | SAFE |
| float_moment_sum_gpu.h:159,181,188,194 | `sum` (the CPU's rounded running sum, stored to `a.sums[2+plane]`) | `uint64_t` | <= exact sum rounded up by < 1 ulp | <= $2^{59}$ | <= $2^{62}$ | SAFE |

### ciede

#### CUDA (cuda/integer_ciede/ciede_score.cu, ciede_device.h, cuda/integer_ciede_cuda.c)

The per-pixel float plane has no integer accumulator. Only index and size math is listed.

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| cuda/integer_ciede/ciede_score.cu:39-40,44-45,62-63,67-68 | `x`, `y`, `cx = x>>ss`, `cy = y>>ss` | `unsigned` | index | <= W+15 | <= 32,783 | SAFE |
| cuda/integer_ciede/ciede_score.cu:46-51,69-80 | `(size_t)y * ref.stride[k]` | `size_t` | row byte offset | 265,420,800 B | $2^{31}$ B | SAFE |
| cuda/integer_ciede/ciede_score.cu:53,82 | `terms[(size_t)y * width + x]` | `size_t` | float-plane index <= N-1 | 132,710,399 | $2^{30} - 1$ | SAFE |
| cuda/integer_ciede/ciede_device.h:109 | `(double)(1 << (bpc - 8u))` | `int` | scale, bpc <= 16 | 256 | 256 | SAFE |
| cuda/integer_ciede_cuda.c:73-76 | `grid_dim_x/y = DIV_ROUND_UP(width/height, 16)` (to `int`) | `int` | grid | 960 x 540 | 2048 x 2048 | SAFE |
| cuda/integer_ciede_cuda.c:145,147,177,200,216 | `(size_t)w * h`, `* sizeof(float)` | `size_t` | readback size | 530,841,600 B | $2^{32}$ B (> UINT32_MAX, fits size_t) | SAFE |
| cuda/integer_ciede_cuda.c:217 | `de00_sum / (s->frame_w * s->frame_h)` | `unsigned * unsigned` (32-bit) -> `double` | pixel count | 132,710,400 | $2^{30}$ | SAFE ($2^{30}$ < $2^{32}$, 4x margin; same expression as ciede.c:603) |

#### HIP (hip/integer_ciede/ciede_score.hip, hip/ciede_hip.c)

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| hip/integer_ciede/ciede_score.hip:72-74 | `(size_t)y * p.y_stride`, `(size_t)cy * p.c_stride` | `size_t` | row offsets | 265,420,800 B | $2^{31}$ B | SAFE |
| hip/integer_ciede/ciede_score.hip:83-88,91 | `x`, `y`, `cx`, `cy`; `terms[(size_t)y * width + x]` | `unsigned` / `size_t` | plane index <= N-1 | 132,710,399 | $2^{30} - 1$ | SAFE |
| hip/integer_ciede/ciede_score.hip:93 | `kCiedeConstants[depth & 3u]` | `unsigned` | table index masked to 0..3 | 3 | 3 | SAFE |
| hip/ciede_hip.c:198-199,223-224,247-248,251-252 | chroma dims, `(size_t)w * bpp`, grid, `(ptrdiff_t)(w * bpp)` (size_t) | `unsigned`/`size_t`/`ptrdiff_t` | geometry | 30,720 / 960x540 | 65,536 / 2048x2048 | SAFE |
| hip/ciede_hip.c:288,331,337,366,391 | `(size_t)w * h * sizeof(float)` | `size_t` | readback size and count | 530,841,600 B | $2^{32}$ B | SAFE |
| hip/ciede_hip.c:392 | `de00_sum / (s->frame_w * s->frame_h)` | `unsigned * unsigned` (32-bit) -> `double` | pixel count | 132,710,400 | $2^{30}$ | SAFE ($2^{30}$ < $2^{32}$) |

### tile-index headers

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| cuda/cuda_tile_index.h:204 | `(2 * extent) - idx - 2` | `int` | reflect index, extent <= dim | 2*15360 | 2*32768 = 65,536 | SAFE (< $2^{31}$) |
| hip/hip_tile_index.h:268 | `(2 * extent) - idx - 2` | `int` | same | 30,720 | 65,536 | SAFE |

### Coverage

| file | rows |
|---|---|
| cuda/float_psnr_cuda.c | 4 |
| cuda/float_psnr_cuda.h | none (FPSNR_BX = 256, FPSNR_BY = 1 macros) |
| cuda/float_psnr/float_psnr_score.cu | 5 |
| cuda/integer_psnr_cuda.c | 3 (+ the grid row) |
| cuda/integer_psnr_cuda.h | none (geometry macros 32x8x8) |
| cuda/integer_psnr/psnr_score.cu | 5 |
| cuda/integer_moment_cuda.c | 3 |
| cuda/integer_moment_cuda.h | none (geometry macros 32x8x8, MOMENT_SUMS = 4) |
| cuda/integer_moment/moment_score.cu | 6 |
| cuda/integer_ciede_cuda.c | 3 |
| cuda/integer_ciede_cuda.h | none |
| cuda/integer_ciede/ciede_device.h | 1 (the rest is fp math) |
| cuda/integer_ciede/ciede_score.cu | 3 |
| cuda/cuda_tile_index.h | 1 |
| hip/float_psnr_hip.c | 3 |
| hip/float_psnr_hip.h | none |
| hip/float_psnr/float_psnr_score.hip | 7 |
| hip/integer_psnr_hip.c | 3 |
| hip/integer_psnr_hip.h | none |
| hip/integer_psnr/psnr_score.hip | 4 |
| hip/float_moment_hip.c | 1 |
| hip/float_moment_hip.h | none |
| hip/float_moment/moment_score.hip | 5 |
| hip/ciede_hip.c | 3 |
| hip/ciede_hip.h | none |
| hip/integer_ciede/ciede_hip_math.h | none (macro and namespace glue) |
| hip/integer_ciede/ciede_score.hip | 3 |
| hip/hip_tile_index.h | 1 |
| float_psnr_rows.h | 1 (shared by the CUDA and HIP rows) |
| float_moment_sum.h | 11 |
| float_moment_sum_gpu.h | 5 |
| ciede_frame_sum.h | none (`size_t` loop index < $2^{30}$, `double` sum) |
| psnr_score.h | 1 |
| ciede_ff_math.h | none (integer parts: `1 << (bpc - 8u)` <= 256 at :390/:392 and the `size_t` xyz loop at :410; `e.k` at :296 is an ldexp exponent from ff_math.h, not an accumulator) |
| ordered_sum.h (read in part, :97-118, :160-192, :245-262, for the units bounds) | counted under float_moment_sum.h |
| integer_psnr.c (CPU, grepped for parity only) | not counted (uses the same `uint64_t` apsnr types at :64-65, :211-212, :249-250) |

Verdict totals: SAFE 80, DEPENDS 3, OVERFLOW@16K 0, OVERFLOW@CAP-ONLY 0.

## G4a: integer-overflow audit of SSIM (integer and float) and MS-SSIM, CUDA and HIP twins

Repo: master at `571565a47`. This audit was read-only. Every path below is relative to `core/src/feature/`.
Envelope: 16K N = 132,710,400; cap N = $2^{30}$ (W, H <= 32768); 16-bit samples, 4:4:4.

### Derived term maxima (shared by every integer-SSIM row)

- Integer Gaussian `gaussian_filter_init(1.5, 5)` (integer_ssim.c:60-96) has kernel_len = floor(1.5*sqrt(-2 ln(sqrt(pi/2)*1.5/256))) = floor(4.70) = 4, so it has 9 taps `[2,9,28,55,68,55,28,9,2]` with a sum of exactly 256. The CUDA kernel (`ISSIM_KERNEL`, integer_ssim_score.cu:70) and the HIP kernel (integer_ssim_score.hip:84) copy it.
- Horizontal moments (a window of at most 9 taps, at most 65535 per sample):
  - mux and muy: at most 256*65535 = 16,776,960 ($2^{24}$).
  - x2, xy and y2: at most 256*$65535^{2}$ = 1,099,478,073,600 (< $2^{40}$).
  - w: at most 256.
- Vertical moments (9 taps over the horizontal moments):
  - mux and muy: at most 65536*65535 = 4,294,901,760 (< $2^{32}$).
  - x2, xy and y2: at most 65536*$65535^{2}$ = 281,466,386,841,600 (< $2^{48}$).
  - w: at most 65,536 ($2^{16}$).
- The bounds above are local to one window and do not depend on the frame size. Frame-level weight sums are Σ m.w ≤ 65536*N: 8,697,308,774,400 ($2^{42.98}$) at 16K and $2^{46}$ at the cap.
- The float_ssim decimation is a fixed-point sum in units of $2^{-52}$ over a scale x scale window, with tap = fl(1/$\mathrm{scale}^{2}$) and sample ≤ 65535/256 = 255.996. A window therefore sums to at most about 256, which is ≤ 255.996*$2^{52}$ ≈ $2^{59.99998}$ < $2^{60}$. This bound holds for every scale. The cap of 128 on the scale (SSIM_MAX_EXACT_SCALE / VMAF_HIP_SSIM_MAX_EXACT_SCALE) is there for exactness, not for overflow.
- No accumulator in G4a spans more than one frame (clip level). Every sum is a local in collect() or a per-frame device buffer, so no frames-to-overflow figure applies.

### integer SSIM (`ssim`)

#### CPU reference (integer_ssim.c), used only to derive the term maxima

| file:line | variable / expression | type | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| integer_ssim.c:146-147 | `s = src[off*2] + (src[off*2+1] << 8)` | `signed` (int) | one 16-bit LE sample | 65535 | 65535 | SAFE ($2^{16}$ < $2^{31}$) |
| integer_ssim.c:153-154 | `m.mux/m.muy += (int64_t)window * s` | `int64_t` | 9 taps, term ≤ 68*65535 | 16,776,960 | same | SAFE ($2^{24}$ < $2^{63}$) |
| integer_ssim.c:155-157 | `m.x2/xy/y2 += (int64_t)window * s * s` | `int64_t` (widened before the first multiply, no int32 product) | 9 taps, term ≤ 68*$65535^{2}$ | 1,099,478,073,600 | same | SAFE ($2^{40}$ < $2^{63}$) |
| integer_ssim.c:158 | `m.w += window` | `int64_t` | 9 taps | 256 | 256 | SAFE |
| integer_ssim.c:247-252 | `m.* += window * buf->*` (vertical) | `int` * `int64_t` -> `int64_t` | 9 taps over the horizontal moments | x2 ≤ 2.81e14; mux ≤ 4.29e9; w ≤ 65536 | same | SAFE ($2^{48}$ < $2^{63}$) |
| integer_ssim.c:300 | `(size_t)line_sz * (size_t)w * sizeof(*line_buf)` | `size_t` | ring buffer, 16 rows x W x 48 B | 11.8 MB | 25.2 MB | SAFE |
| integer_ssim.c:315,420 | `_systride` (int param) <- `ref_pic->stride[0]` (ptrdiff_t) | narrowing to `int` | one row pitch in bytes | ≤ 30720 + alignment | ≤ 65536 + alignment | SAFE ($2^{16}$ < $2^{31}$) |
| integer_ssim.c:323 | `samplemax = (1 << depth) - 1` | `int` | — | 65535 | 65535 | SAFE (squared in double at :228/256, R2-1 fix) |

#### CUDA (cuda/integer_ssim/integer_ssim_score.cu, cuda/ssim_cuda.c)

| file:line | variable / expression | type | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| cuda/integer_ssim/integer_ssim_score.cu:169-170 | `ref[(ptrdiff_t)y * ref_stride + src_x]` (8bpc) | `ptrdiff_t` byte offset | row offset | ≤ 8639*pitch ≈ 1.3e8 | ≤ 32767*pitch ≈ $2^{30}$ | SAFE ($2^{30}$ < $2^{63}$) |
| cuda/integer_ssim/integer_ssim_score.cu:216-218 | `ref + (ptrdiff_t)y * ref_stride` (16bpc), then `[src_x]` (int) | `ptrdiff_t` | row offset of a 16-bit plane; pitch is size_t -> ptrdiff_t | ≤ 8639*30720+ = 2.65e8 | ≤ 32767*65536+ = 2.15e9 | SAFE ($2^{31}$ < $2^{63}$) |
| cuda/integer_ssim/integer_ssim_score.cu:172-173, 220-221 | `mux/muy += wk * s` | `int64_t` (wk, s both int64) | 9 taps, term ≤ 68*65535 | 16,776,960 | same | SAFE ($2^{24}$ < $2^{63}$) |
| cuda/integer_ssim/integer_ssim_score.cu:174-176, 222-224 | `x2/xy/y2 += wk * s * s` | `int64_t` (no int32 intermediate) | 9 taps, term ≤ 68*$65535^{2}$ = 2.92e11 | 1,099,478,073,600 | same | SAFE ($2^{40}$ < $2^{63}$) |
| cuda/integer_ssim/integer_ssim_score.cu:177, 225 | `w += wk` | `int64_t` | 9 taps | 256 | 256 | SAFE |
| cuda/integer_ssim/integer_ssim_score.cu:180, 228 | `idx = y * width + x` | `unsigned` (32-bit) | plane index | ≤ N-1 = 132,710,399 | ≤ $2^{30} - 1$ | SAFE ($2^{30}$ < $2^{32}$) |
| cuda/integer_ssim/integer_ssim_score.cu:97 | `hidx = (unsigned)src_y * width + x` | `unsigned` (32-bit) | plane index, src_y ≥ 0 by k_min | ≤ N-1 | ≤ $2^{30} - 1$ | SAFE ($2^{30}$ < $2^{32}$) |
| cuda/integer_ssim/integer_ssim_score.cu:99-104 | `m.mux/muy/x2/xy/y2/w += vk * d_*_h[hidx]` | `int64_t` | 9 vertical taps over the horizontal moments | x2 ≤ 281,466,386,841,600; mux ≤ 4,294,901,760; w ≤ 65,536 | same | SAFE ($2^{48}$ < $2^{63}$) |
| cuda/integer_ssim/integer_ssim_score.cu:116-124 | `(double)m.w/.mux/.x2/...` | int64 -> double | moment conversion | ≤ $2^{48}$ | ≤ $2^{48}$ | SAFE (exact: $2^{48}$ < $2^{53}$) |
| cuda/integer_ssim/integer_ssim_score.cu:282 | `terms[(size_t)y * width + x]` | `size_t` | term plane index | ≤ N-1 | ≤ $2^{30} - 1$ | SAFE |
| cuda/integer_ssim/integer_ssim_score.cu:287 | `warp_reduce(my_weight)` (cuda_helper.cuh:125-133: shuffles the lo and hi 32-bit halves and rebuilds the int64) | `int64_t` | 32 lanes, each ≤ 65536 | 2,097,152 | same | SAFE ($2^{21}$ < $2^{63}$) |
| cuda/integer_ssim/integer_ssim_score.cu:289-291 | `tid = threadIdx.y * (int)blockDim.x + threadIdx.x` | `int` | thread id | ≤ 127 | ≤ 127 | SAFE |
| cuda/integer_ssim/integer_ssim_score.cu:297-299 | `block_wgt += s_wgt[i]` | `int64_t` | 4 warp sums | ≤ 128*65536 = 8,388,608 | same | SAFE ($2^{23}$ < $2^{63}$) |
| cuda/integer_ssim/integer_ssim_score.cu:300 | `blk = blockIdx.y * gridDim.x + blockIdx.x` | `unsigned` | partial index | ≤ 1,036,799 | ≤ $2^{23} - 1$ | SAFE ($2^{23}$ < $2^{32}$) |
| cuda/ssim_cuda.c:204-206 | `grid_x`, `grid_y`, `block_count = grid_x * grid_y` | `unsigned` | launch geometry | 960 x 1080 = 1,036,800 | 2048 x 4096 = $2^{23}$ | SAFE (gridDim.y 4096 ≤ 65535) |
| cuda/ssim_cuda.c:208 | `int64_plane_bytes = (size_t)w * h * sizeof(int64_t)` | `size_t` | one of 6 moment planes | 1,061,683,200 B | $2^{33}$ B (would overflow uint32; is size_t) | SAFE ($2^{33}$ < $2^{64}$) |
| cuda/ssim_cuda.c:209 | `term_plane_bytes = (size_t)w * h * sizeof(double)` | `size_t` | double term plane | 1,061,683,200 B | $2^{33}$ B | SAFE |
| cuda/ssim_cuda.c:210 | `(size_t)block_count * sizeof(int64_t)` | `size_t` | partials | 8,294,400 B | $2^{26}$ B | SAFE |
| cuda/ssim_cuda.c:299 | `samplemax = (int64_t)((1u << bpc) - 1u)` | `unsigned` -> `int64_t` | — | 65535 | 65535 | SAFE (the kernel squares it in double) |
| cuda/ssim_cuda.c:374, 377 | DtoH sizes `(size_t)w * h * sizeof(double)`, `(size_t)block_count * 8` | `size_t` | readback bytes | 1.06e9 B | $2^{33}$ B | SAFE |
| cuda/ssim_cuda.c:407 | `issim_frame_sum(terms, (size_t)w * h)` count and loop | `size_t` | term count | N | $2^{30}$ | SAFE |
| cuda/ssim_cuda.c:408-410 | `total_wgt += wgt_partials[i]` (loop `unsigned i < block_count`) | `int64_t` | Σ block partials = Σ_pixels m.w, at most 65536 per pixel | 8,697,308,774,400 ($2^{42.98}$) | 70,368,744,177,664 ($2^{46}$) | SAFE ($2^{46}$ < $2^{63}$; `(double)total_wgt` exact, < $2^{53}$) |

#### HIP (hip/integer_ssim/integer_ssim_score.hip, hip/integer_ssim_hip.c)

| file:line | variable / expression | type | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| hip/integer_ssim/integer_ssim_score.hip:117-118 | `ref + (ptrdiff_t)y * ref_stride` | `ptrdiff_t` | packed-row offset (stride = width*bps, host :303) | ≤ 8639*30720 = 265,390,080 | ≤ 32767*65536 = 2,147,418,112 | SAFE ($2^{31}$ < $2^{63}$) |
| hip/integer_ssim/integer_ssim_score.hip:130-131 | `const int64_t s = ref_row[src_x]` | uint8/uint16 -> `int64_t` | sample (no int promotion on the multiply) | 65535 | 65535 | SAFE |
| hip/integer_ssim/integer_ssim_score.hip:133-134 | `mux/muy += wk * s` | `int64_t` | 9 taps, term ≤ 68*65535 | 16,776,960 | same | SAFE ($2^{24}$ < $2^{63}$) |
| hip/integer_ssim/integer_ssim_score.hip:135-137 | `x2/xy/y2 += wk * s * s` | `int64_t` | 9 taps, term ≤ 2.92e11 | 1,099,478,073,600 | same | SAFE ($2^{40}$ < $2^{63}$) |
| hip/integer_ssim/integer_ssim_score.hip:138 | `w += wk` | `int64_t` | 9 taps | 256 | 256 | SAFE |
| hip/integer_ssim/integer_ssim_score.hip:141 | `idx = (size_t)y * width + x` | `size_t` | plane index | ≤ N-1 | ≤ $2^{30} - 1$ | SAFE |
| hip/integer_ssim/integer_ssim_score.hip:181 | `hidx = (size_t)((int)y - 4 + k) * width + x` | `int` (≥ 0 by k_min) -> `size_t` | plane index | ≤ N-1 | ≤ $2^{30} - 1$ | SAFE |
| hip/integer_ssim/integer_ssim_score.hip:183-188 | `m.* += vk * d_*_h[hidx]` | `int64_t` | 9 vertical taps | x2 ≤ 2.81e14; mux ≤ 4.29e9; w ≤ 65536 | same | SAFE ($2^{48}$ < $2^{63}$) |
| hip/integer_ssim/integer_ssim_score.hip:199-208 | `(double)m.*` | int64 -> double | moment conversion | ≤ $2^{48}$ | ≤ $2^{48}$ | SAFE (exact: < $2^{53}$) |
| hip/integer_ssim/integer_ssim_score.hip:273 | `terms[(size_t)y * width + x]` | `size_t` | term index | ≤ N-1 | ≤ $2^{30} - 1$ | SAFE |
| hip/integer_ssim/integer_ssim_score.hip:278 | `tid = threadIdx.y * blockDim.x + threadIdx.x` | `unsigned` | thread id | ≤ 127 | ≤ 127 | SAFE |
| hip/integer_ssim/integer_ssim_score.hip:279-285 | `s_weight[tid] += s_weight[tid + half]` (shared tree, 128 slots) | `int64_t` | 128 weights, each ≤ 65536 | 8,388,608 | same | SAFE ($2^{23}$ < $2^{63}$) |
| hip/integer_ssim/integer_ssim_score.hip:287 | `blockIdx.y * gridDim.x + blockIdx.x` | `unsigned` | partial index | ≤ 1,036,799 | ≤ $2^{23} - 1$ | SAFE ($2^{23}$ < $2^{32}$) |
| hip/integer_ssim_hip.c:155-158 | `grid_x`, `grid_y` (unsigned); `term_count = (size_t)w * h`; `block_count = (size_t)grid_x * grid_y` | `unsigned` / `size_t` | geometry | 960 x 1080; N; 1,036,800 | 2048 x 4096; $2^{30}$; $2^{23}$ | SAFE |
| hip/integer_ssim_hip.c:159 | `(double)((1u << bpc) - 1u)` | `unsigned` | samplemax | 65535 | 65535 | SAFE |
| hip/integer_ssim_hip.c:213 | `plane_bytes = (size_t)w * h * sizeof(int64_t)` | `size_t` | one of 6 moment planes | 1,061,683,200 B | $2^{33}$ B | SAFE |
| hip/integer_ssim_hip.c:277, 280, 337, 341 | `term_count * sizeof(double)`, `block_count * sizeof(int64_t)` | `size_t` | readback bytes | 1.06e9 B / 8.3e6 B | $2^{33}$ B / $2^{26}$ B | SAFE |
| hip/integer_ssim_hip.c:303 | `stride = (ptrdiff_t)width * (ptrdiff_t)bps` | `ptrdiff_t` | packed row bytes | 30720 | 65536 | SAFE |
| hip/integer_ssim_hip.c:400-401 | term-sum loop `size_t i < term_count` | `size_t` | loop index | N | $2^{30}$ | SAFE (double accumulator itself skipped) |
| hip/integer_ssim_hip.c:402-403 | `total_weight += block_weights[i]` | `int64_t` | Σ block partials, at most 65536 per pixel | 8,697,308,774,400 ($2^{42.98}$) | $2^{46}$ | SAFE ($2^{46}$ < $2^{63}$; double conversion exact) |

### float SSIM (`float_ssim`)

#### CUDA (cuda/integer_ssim/ssim_score.cu, cuda/integer_ssim_cuda.c)

| file:line | variable / expression | type | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| cuda/integer_ssim/ssim_score.cu:73 | `data[0] + y * pic.stride[0]` (8bpc, scale 1) | `unsigned` * `ptrdiff_t` -> `ptrdiff_t` | row offset | ≤ 8639*pitch | ≤ 32767*pitch ≈ $2^{30}$ | SAFE ($2^{31}$ < $2^{63}$) |
| cuda/integer_ssim/ssim_score.cu:80-81 | `data[0] + y * pic.stride[0]` (16bpc, scale 1) | `ptrdiff_t` | row offset | ≤ 2.65e8 | ≤ 2.15e9 | SAFE ($2^{31}$ < $2^{63}$) |
| cuda/integer_ssim/ssim_score.cu:167 | `src_idx = (y + v) * w_horiz + x` | `unsigned` | horizontal-plane index; y+v ≤ H-1 | < H*(W-10) < N | < $2^{30}$ | SAFE ($2^{30}$ < $2^{32}$; scale 1 is reachable at any size through the `scale` option, 1..10) |
| cuda/integer_ssim/ssim_score.cu:213, 217 | `y * width + x` (decimated planes) | `unsigned` | dec-plane index | < dec_w*dec_h ≤ N | < $2^{30}$ | SAFE |
| cuda/integer_ssim/ssim_score.cu:257 | `dst_idx = y * w_horiz + x` | `unsigned` | horizontal-plane index | < N | < $2^{30}$ | SAFE |
| cuda/integer_ssim/ssim_score.cu:293-297 | `period = 2 * extent`; `position % period` | `int` | mirror index | 2*15360 | 65536 | SAFE |
| cuda/integer_ssim/ssim_score.cu:311-312 | `data[0] + src_y * pic.stride[0]` | `int` * `ptrdiff_t` -> `ptrdiff_t` | decimate row offset | ≤ 2.65e8 | ≤ 2.15e9 | SAFE |
| cuda/integer_ssim/ssim_score.cu:308-318 | `long long sum += __float2ll_rz(__fmul_rn(product, 0x1p52f))` | `long long` (fixed point, $2^{-52}$ units) | $\mathrm{scale}^{2}$ terms (auto scale 34 at 16K = 1156 terms; at cap 128 = 16384 terms), each ≤ 255.996*fl(1/$\mathrm{scale}^{2}$)*$2^{52}$ | < $2^{60}$ (window value ≤ 255.996 ≈ $2^{59.99998}$ units) | < $2^{60}$ | SAFE ($2^{60}$ < $2^{63}$; the bound does not depend on scale) |
| cuda/integer_ssim/ssim_score.cu:317 | `__float2ll_rz(product * 2^52)` per term | float -> `long long` | one term | ≤ $2^{58}$ (scale 2) | ≤ $2^{58}$ | SAFE (in range) |
| cuda/integer_ssim/ssim_score.cu:335-336 | `centre_x = (int)x * g.scale` | `int` | decimation centre | ≤ W + scale | ≤ 32768 + 128 | SAFE |
| cuda/integer_ssim/ssim_score.cu:337 | `dst_idx = y * g.out_width + x` | `unsigned` | dec-plane index | ≤ N/$\mathrm{scale}^{2}$ | ≤ $2^{30}$ (scale 1 not dispatched here) | SAFE |
| cuda/integer_ssim/ssim_score.cu:490 | `terms[(size_t)y * w_final + x]` | `size_t` | term index | < N | < $2^{30}$ | SAFE |
| cuda/integer_ssim/ssim_score.cu:511 | `((size_t)y * w_final + x) * LCS_TERMS` | `size_t` | lcs term index (4 per window) | < 4N | < $2^{32}$ | SAFE (size_t) |
| cuda/integer_ssim_cuda.c:176 | `round_to_int((float)min_int(w, h) / 256.0f)` | float -> `int` | auto scale | 34 | 128 | SAFE |
| cuda/integer_ssim_cuda.c:183 | `iqa_decimate_dim((int)extent, scale)` | `int` | dec extent | ≤ 15360 | ≤ 32768 | SAFE |
| cuda/integer_ssim_cuda.c:328 | `(float)(s->scale * s->scale)` | `int` | tap denominator | 1156 | 16384 | SAFE |
| cuda/integer_ssim_cuda.c:339 | `n_windows = (size_t)w_final * h_final` | `size_t` | window count (scale 1 worst) | 132,470,500 | 1,073,086,564 | SAFE |
| cuda/integer_ssim_cuda.c:347 | `n_windows * n_sums * sizeof(double)` | `size_t` | term plane bytes (enable_lcs, scale 1) | 4,239,056,000 B (> INT32_MAX; is size_t) | 34,338,770,048 B ($2^{35}$) | SAFE ($2^{35}$ < $2^{64}$) |
| cuda/integer_ssim_cuda.c:355, 364 | `horiz_bytes`, `dec_bytes` = `(size_t)a * b * sizeof(float)` | `size_t` | plane bytes | 5.3e8 B | $2^{32}$ B (would overflow uint32; is size_t) | SAFE |
| cuda/integer_ssim_cuda.c:532-533, 593-594, 607-608 | grid_x/grid_y = `(extent + 15) / 16`, `(extent + 7) / 8` | `unsigned` | launch geometry | y ≤ 1080 | y ≤ 4096 | SAFE (gridDim.y ≤ 65535) |
| cuda/integer_ssim_cuda.c:654, 674-675 | sum loops `size_t i`; `terms + (i * FLOAT_SSIM_LCS_SUMS)` | `size_t` | term index | < 4N | < $2^{32}$ | SAFE (double accumulators skipped) |

#### HIP (hip/float_ssim/ssim_decimate.h, hip/float_ssim/ssim_score.hip, hip/float_ssim_hip.c)

| file:line | variable / expression | type | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| hip/float_ssim/ssim_decimate.h:64-69 | `period = 2 * extent`; `position % period` | `int` | mirror index | 30720 | 65536 | SAFE |
| hip/float_ssim/ssim_decimate.h:78-79 | `low = (size_t)x * 2u`; `raw = row[low] \| row[low+1] << 8u` | `size_t` / `unsigned` | 16-bit LE sample | 65535 | 65535 | SAFE |
| hip/float_ssim/ssim_decimate.h:89 | `(int64_t)(product * 2^52f)` | float -> `int64_t` | one term | ≤ $2^{58}$ | ≤ $2^{58}$ | SAFE (in range) |
| hip/float_ssim/ssim_decimate.h:101, 105 | `row_bytes = (size_t)width * sample_bytes`; `(size_t)source_y * row_bytes` | `size_t` | packed-row offset | ≤ 2.65e8 | ≤ 2.15e9 | SAFE |
| hip/float_ssim/ssim_decimate.h:102-110 | `int64_t sum += vmaf_hip_ssim_fixed(product)` | `int64_t` ($2^{-52}$ units) | $\mathrm{scale}^{2}$ terms (≤ 16384), window value ≤ 255.996 | < $2^{60}$ | < $2^{60}$ | SAFE ($2^{60}$ < $2^{63}$; scale ≤ 128 is enforced on the host :257 and in the kernel guard ssim_score.hip:271) |
| hip/float_ssim/ssim_score.hip:201 | `(size_t)y * w_horiz + x` | `size_t` | vertical first index | < N | < $2^{30}$ | SAFE |
| hip/float_ssim/ssim_score.hip:241-242 | `ref + (ptrdiff_t)y * ref_stride` | `ptrdiff_t` | row offset (stride host :459-460) | ≤ 2.65e8 (16-bit) / ≤ 5.3e8 (f32 dec) | ≤ 2.15e9 | SAFE |
| hip/float_ssim/ssim_score.hip:254 | `dst_idx = y * w_horiz + x` | `unsigned` | horizontal-plane index | < N | < $2^{30}$ | SAFE ($2^{30}$ < $2^{32}$) |
| hip/float_ssim/ssim_score.hip:275-276 | `centre_x = (int)x * scale` | `int` | decimation centre | ≤ W + scale | ≤ 32896 | SAFE |
| hip/float_ssim/ssim_score.hip:277 | `index = (size_t)y * out_width + x` | `size_t` | dec index | ≤ N | ≤ $2^{30}$ | SAFE |
| hip/float_ssim/ssim_score.hip:393, 413-418 | `(size_t)y * w_final + x`; `windows = (size_t)w_final * h_final`; `(size_t)k * windows + window` | `size_t` | term / lcs indices | < 3N | < 3*$2^{30}$ | SAFE |
| hip/float_ssim_hip.c:243 | `ssim_hip_round_to_int((float)min / 256.0f)` | float -> `int` | auto scale | 34 | 128 | SAFE |
| hip/float_ssim_hip.c:313 | `windows = (size_t)w_final * h_final` | `size_t` | window count | 132,470,500 | 1,073,086,564 | SAFE |
| hip/float_ssim_hip.c:380-381 | `horiz_bytes`, `dec_bytes` (size_t products) | `size_t` | plane bytes | 5.3e8 B | $2^{32}$ B | SAFE |
| hip/float_ssim_hip.c:428-430 | grid (unsigned); `(float)(s->scale * s->scale)` | `unsigned` / `int` | geometry, tap denominator | 1156 | 16384 | SAFE |
| hip/float_ssim_hip.c:453-454, 481-482 | grid_x / grid_y | `unsigned` | launch geometry | y ≤ 1080 | y ≤ 4096 | SAFE |
| hip/float_ssim_hip.c:459-460 | `stride = (ptrdiff_t)width * (ptrdiff_t)(4 or bps)` | `ptrdiff_t` | row bytes | ≤ 30720 | ≤ 65536 | SAFE |
| hip/float_ssim_hip.c:521, 525, 589, 592 | `windows * sizeof(double)`, `3u * windows * sizeof(double)` | `size_t` | readback bytes (lcs, scale 1) | 3,179,292,000 B | 25,754,077,536 B | SAFE (size_t) |
| hip/float_ssim_hip.c:687-698 | loops `size_t i < windows`; `l + s->windows` | `size_t` | term index | < N | < $2^{30}$ | SAFE (double accumulators skipped) |

### float MS-SSIM (`float_ms_ssim`)

#### CUDA (cuda/integer_ms_ssim/ms_ssim_score.cu, cuda/integer_ms_ssim_cuda.c)

| file:line | variable / expression | type | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| cuda/integer_ms_ssim/ms_ssim_score.cu:83-89 | `period = 2 * n`; `idx % period` | `int` | mirror index | 30720 | 65536 | SAFE |
| cuda/integer_ms_ssim/ms_ssim_score.cu:146-147 | `x_src = (int)x_out * 2` | `int` | source column | ≤ W+1 | ≤ 32769 | SAFE |
| cuda/integer_ms_ssim/ms_ssim_score.cu:154 | `src_buf[yi * (int)w + xi]` | `int` (signed 32-bit) | level-0 plane index (scale 0 is the full luma or chroma plane) | ≤ 8639*15360+15359 = 132,710,399 | ≤ 32767*32768+32767 = $2^{30} - 1$ | SAFE ($2^{30}$ < $2^{31}$; 1 bit of margin; signed, so a plane > $2^{31}$ samples would be UB, which VMAF_PIC_DIM_MAX rules out) |
| cuda/integer_ms_ssim/ms_ssim_score.cu:158 | `dst_buf[y_out * w_out + x_out]` | `unsigned` | level i+1 index | ≤ N/4 | ≤ $2^{28}$ | SAFE |
| cuda/integer_ms_ssim/ms_ssim_score.cu:191 | `src_idx = y * width + (x + u)` | `unsigned` | horizontal input index | < N | < $2^{30}$ | SAFE ($2^{30}$ < $2^{32}$) |
| cuda/integer_ms_ssim/ms_ssim_score.cu:201 | `dst_idx = y * w_horiz + x` | `unsigned` | horizontal output index | < N | < $2^{30}$ | SAFE |
| cuda/integer_ms_ssim/ms_ssim_score.cu:246 | `src_idx = first + (unsigned)v * stride` | `unsigned` | vertical tap index, ≤ (H-1)*w_horiz + x | < N | < $2^{30}$ | SAFE |
| cuda/integer_ms_ssim/ms_ssim_score.cu:311 | `y * w_horiz + x` | `unsigned` | vertical first index | < N | < $2^{30}$ | SAFE |
| cuda/integer_ms_ssim/ms_ssim_score.cu:313 | `window = (size_t)y * w_final + x` | `size_t` | term index | < N | < $2^{30}$ | SAFE |
| cuda/integer_ms_ssim_cuda.c:282 | `min_dim = 11u << 4` | `unsigned` | — | 176 | 176 | SAFE |
| cuda/integer_ms_ssim_cuda.c:304-306 | `peak = (1u << bpc) - 1u`; `0.5 / (w * h)`; `peak * peak / mse` | `unsigned` (both products 32-bit before double) | w*h; $\mathrm{peak}^{2}$ | w*h = 132,710,400; $\mathrm{peak}^{2}$ = 4,294,836,225 | w*h = $2^{30}$; $\mathrm{peak}^{2}$ = 4,294,836,225 | SAFE (both < $2^{32}$; $\mathrm{peak}^{2}$ has a margin of 131,070 below UINT32_MAX; the shared double helper vmaf_metal_ms_ssim_max_db is not used here) |
| cuda/integer_ms_ssim_cuda.c:318-319 | `scale_w[i] = w/2 + (w & 1)` | `unsigned` | pyramid extents | ≤ 15360 | ≤ 32768 | SAFE |
| cuda/integer_ms_ssim_cuda.c:326-330 | grid_x/grid_y; `scale_window_count = (size_t)w_final * h_final` | `unsigned` / `size_t` | geometry, window count | 132,470,500 | 1,073,086,564 | SAFE |
| cuda/integer_ms_ssim_cuda.c:383, 389-393 | `plane_bytes = (size_t)w*h*sizeof(float)`; `windows * sizeof(double/float)` | `size_t` | pyramid and term planes | 530,841,600 B; 1,059,764,000 B | $2^{32}$ B; 8,584,692,512 B (both > UINT32_MAX; are size_t) | SAFE |
| cuda/integer_ms_ssim_cuda.c:402-403, 419, 444 | `horiz_bytes_max`, `input_bytes`, `raw_input_bytes` | `size_t` | buffer bytes | ≤ 5.3e8 B | ≤ $2^{32}$ B | SAFE |
| cuda/integer_ms_ssim_cuda.c:494-499 | `srcPitch = (size_t)stride`, `dstPitch`/`WidthInBytes = (size_t)width * bpc_bytes`, `Height = pl->height` | `size_t` | 2D copy | 30720 B/row | 65536 B/row | SAFE |
| cuda/integer_ms_ssim_cuda.c:517, 520, 527 | `(ptrdiff_t)((size_t)width * bpc_bytes)`; `(size_t)width * sizeof(float)`; `bytes` | `size_t` / `ptrdiff_t` | stride and copy bytes | ≤ 5.3e8 B | ≤ $2^{32}$ B | SAFE |
| cuda/integer_ms_ssim_cuda.c:563-564, 597-598, 616 | grid dims | `unsigned` | launch geometry | y ≤ 1080 | y ≤ 4096 | SAFE (≤ 65535) |
| cuda/integer_ms_ssim_cuda.c:622-626, 684-687 | DtoH `windows * sizeof(...)`; sum loop `size_t j` | `size_t` | readback bytes, term index | 1.06e9 B | 8.58e9 B | SAFE (double accumulators skipped) |

#### HIP (hip/integer_ms_ssim/ms_ssim_arith.h, hip/integer_ms_ssim/ms_ssim_score.hip, hip/integer_ms_ssim_hip.c)

| file:line | variable / expression | type | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| hip/integer_ms_ssim/ms_ssim_arith.h:81-86 | `period = 2 * n`; `idx % period` | `int` | mirror index | 30720 | 65536 | SAFE |
| hip/integer_ms_ssim/ms_ssim_arith.h:115-116 | `x_src = x_out * 2`, `y_src = y_out * 2` | `int` | source position | ≤ W+1 | ≤ 32769 | SAFE |
| hip/integer_ms_ssim/ms_ssim_arith.h:120 | `src + (size_t)yi * (size_t)w` | `size_t` | row offset | < N | < $2^{30}$ | SAFE |
| hip/integer_ms_ssim/ms_ssim_arith.h:211 | `idx = first + (size_t)v * stride` | `size_t` | vertical tap index | < N | < $2^{30}$ | SAFE |
| hip/integer_ms_ssim/ms_ssim_score.hip:353 | `(size_t)y_out * w_out + x_out` | `size_t` | decimate output index | ≤ N/4 | ≤ $2^{28}$ | SAFE |
| hip/integer_ms_ssim/ms_ssim_score.hip:370, 372 | `src_idx`, `dst_idx` = `(size_t)y * w + x` | `size_t` | horizontal indices | < N | < $2^{30}$ | SAFE |
| hip/integer_ms_ssim/ms_ssim_score.hip:404, 406-410 | `(size_t)y * w_horiz + x`; `windows`; `window`; `2u * windows + window` | `size_t` | term indices [l\|c\|s] | < 3N | < 3*$2^{30}$ | SAFE |
| hip/integer_ms_ssim_hip.c:260, 276 | `min_dim = 11u << 4` | `unsigned` | — | 176 | 176 | SAFE |
| hip/integer_ms_ssim_hip.c:300-310 | `scale_w[i]`, grid dims, `scale_windows = (size_t)w_final * h_final` | `unsigned` / `size_t` | pyramid geometry | 132,470,500 windows | 1,073,086,564 | SAFE |
| hip/integer_ms_ssim_hip.c:374 | `3u * scale_windows[i] * sizeof(double)` | `size_t` | term bytes, scale 0 | 3,179,292,000 B | 25,754,077,536 B | SAFE (size_t) |
| hip/integer_ms_ssim_hip.c:397, 415, 429-430, 496, 499 | `lvl`, `level0_bytes`, `horiz_max`, picture_copy stride, `float_bytes` | `size_t` | buffer bytes | ≤ 5.3e8 B | ≤ $2^{32}$ B | SAFE |
| hip/integer_ms_ssim_hip.c:508-509, 523-524 | grid dims | `unsigned` | launch geometry | y ≤ 1080 | y ≤ 4096 | SAFE |
| hip/integer_ms_ssim_hip.c:617-620 | `peak = (1u << bpc) - 1u`; `0.5 / (w * h)`; `peak * peak / mse` | `unsigned` | w*h; $\mathrm{peak}^{2}$ | 132,710,400; 4,294,836,225 | $2^{30}$; 4,294,836,225 | SAFE (both < $2^{32}$; margin 131,070 on $\mathrm{peak}^{2}$) |
| hip/integer_ms_ssim_hip.c:805-816 | `l + windows`, `c + windows`; loop `size_t j` | `size_t` | term index | < N | < $2^{30}$ | SAFE (double accumulators skipped) |

### Shared integer helpers

| file:line | variable / expression | type | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| iqa/decimate_dim.h:86 | `n / factor + (n & 1)` | `int` | decimated extent | ≤ 15360 | ≤ 32768 | SAFE |
| iqa/decimate.c:53 | `dst_offset = y * sw` (then `++dst_offset`) | `int` | CPU decimated-plane index | < sw*sh ≤ N/4 (scale ≥ 2) | < $2^{28}$ | SAFE (≤ $2^{30}$ < $2^{31}$ even at factor 1) |
| iqa/decimate.c:55 | `x * factor`, `y * factor` | `int` | source position | ≤ W | ≤ 32768 | SAFE |

### Asides (verdicts unchanged)

- **Device memory at the cap:** the CUDA and HIP integer SSIM twins allocate 6 int64 moment planes plus 1 double term plane, 7 x $2^{33}$ B = 56 GiB. Every size is computed in `size_t` and passed to `size_t` allocator parameters (cuda/common.h:148, 273; cuda/kernel_template.h:187-188; hip/kernel_template.h:125). At that size the allocation fails cleanly; no size wraps.
- **`warp_reduce(int64_t)` (cuda_helper.cuh:129-130, outside this group):\*\* the helper left-shifts the shuffled signed high half (`<< 32`), which is UB for negative values before C++20. That is unreachable here: the weights are ≥ 0 and below $2^{23}$, so the high half is 0.
- **Grid dimensions:** the largest gridDim.y in the group is ceil(32768/8) = 4096, well under 65535. No grid expression wraps.
- **`picture_copy()` (picture_copy.cpp, outside this group):\*\* the CUDA and HIP MS-SSIM hosts call it. It walks rows with `ptrdiff_t` pointer increments and `unsigned` loop counters, and has no integer product.
- **Tightest margin in the group:** the signed `int` index at cuda/integer_ms_ssim/ms_ssim_score.cu:154 ($2^{30} - 1$ against INT32_MAX). Next is `peak * peak` in `unsigned` at cuda/integer_ms_ssim_cuda.c:306 and hip/integer_ms_ssim_hip.c:620 (131,070 below UINT32_MAX). Both are safe under the envelope.

### Coverage

| file | rows |
|---|---|
| cuda/float_ssim_cuda.h | none (extern PTX symbol only) |
| cuda/integer_ssim_cuda.c | 8 |
| cuda/integer_ssim_cuda.h | none (extern PTX symbol only) |
| cuda/integer_ssim/integer_ssim_score.cu | 14 |
| cuda/integer_ssim/ssim_score.cu | 13 |
| cuda/ssim_cuda.c | 8 |
| cuda/ssim_cuda.h | none (extern PTX symbol only) |
| cuda/integer_ms_ssim_cuda.c | 10 |
| cuda/integer_ms_ssim_cuda.h | none (extern PTX symbol only) |
| cuda/integer_ms_ssim/ms_ssim_score.cu | 9 |
| hip/float_ssim_hip.c | 8 |
| hip/float_ssim_hip.h | none (extern HSACO symbols only) |
| hip/float_ssim/ssim_decimate.h | 5 |
| hip/float_ssim/ssim_score.hip | 6 |
| hip/integer_ssim_hip.c | 7 |
| hip/integer_ssim_hip.h | none (extern HSACO symbols only) |
| hip/integer_ssim/integer_ssim_score.hip | 13 |
| hip/integer_ms_ssim_hip.c | 7 |
| hip/integer_ms_ssim_hip.h | none (extern HSACO symbols only) |
| hip/integer_ms_ssim/ms_ssim_arith.h | 4 (the rest is fp32/fp64 arithmetic, no integer accumulator) |
| hip/integer_ms_ssim/ms_ssim_score.hip | 3 |
| iqa/decimate_dim.h | 1 |
| integer_ssim.h | none (type definition: six `int64_t` fields, used by the CPU rows) |
| integer_ssim.c (CPU reference) | 8 |
| iqa/decimate.c (CPU reference) | 2 (the window sum is a double inside iqa_filter_pixel, so it is skipped) |
| also read for derivation: cuda_helper.cuh:125-139, cuda/kernel_template.h:187-200, cuda/common.h:37-40,148,273, hip/kernel_template.h:125, metal/float_ms_ssim_option_semantics.h:26-58, nonfinite_score.h:336-343, picture_copy.cpp:52-103, include/libvmaf/picture.h:90-92 | no rows (outside the group) |

Verdict totals (126 rows): SAFE 126; OVERFLOW@16K 0; OVERFLOW@CAP-ONLY 0; DEPENDS 0.

## G4b: SSIMULACRA 2, CUDA + HIP twins, ordered-sum header (integer-overflow audit)

Repo: master @ `571565a47`. Read-only; no build, no device run.
All paths below are relative to `core/src/feature/`.

Envelope inputs taken from the code:
- `VMAF_PIC_DIM_MAX 32768u` (`core/src/picture.c:46`, checked at :185), `VMAF_PIC_BPC_MIN/MAX 8u/16u` (`picture.c:37-38`, checked at :181).
- Both twins refuse w or h < 8 (`cuda/ssimulacra2_cuda.c:788`, `hip/ssimulacra2_hip.c:931`). That makes pixels >= 64 and chunks >= 1.
- Chunk = 256 lanes x 4 px = 1024 px (`SS2C_CHUNK_PIXELS`, `SS2H_CHUNK_PIXELS`). A batch is 1024 chunks (`SS2C_BATCH`, `SS2H_BATCH`).
- Scale pyramid: `scale_w[i] = (scale_w[i-1]+1)/2`. The pyramid stops before the first scale with a side below 8. Chunks are counted per plane (one channel); `ceil` applies.

| scale | 16K W x H | N | chunks | cap W x H | N | chunks |
|---|---|---|---|---|---|---|
| 0 | 15360x8640 | 132,710,400 | 129,600 | 32768x32768 | $2^{30}$ | $2^{20}$ = 1,048,576 |
| 1 | 7680x4320 | 33,177,600 | 32,400 | $16384^{2}$ | $2^{28}$ | 262,144 |
| 2 | 3840x2160 | 8,294,400 | 8,100 | $8192^{2}$ | $2^{26}$ | 65,536 |
| 3 | 1920x1080 | 2,073,600 | 2,025 | $4096^{2}$ | $2^{24}$ | 16,384 |
| 4 | 960x540 | 518,400 | 507 | $2048^{2}$ | $2^{22}$ | 4,096 |
| 5 | 480x270 | 129,600 | 127 | $1024^{2}$ | $2^{20}$ | 1,024 |

Device buffers are sized for scale 0. That scale has the most chunks.

The brief assumed fp64 term planes (6 terms x 3 channels x N x 8 B). These do not exist. Every kernel recomputes the six fp64 terms from the five blurred fp32 planes and the two XYB fp32 planes (`ss2c_terms` / `ss2h_terms`). Per scale, the code stores only three per-(channel, sum, chunk) arrays:
- `chunk_sums`: double, 8 B
- `plan`: int16, 2 B
- `units`: 2 x int64, 16 B

Neither twin has a clip-level (cross-frame) integer accumulator. Each frame's score is appended to the collector on its own, so no row is DEPENDS.

### ordered_sum (shared, `ordered_sum.h`, ADR-1433). Used by the CUDA and HIP twins

The increment of one term in binade e is the term divided by u = $2^{e-52}$, rounded. In integers this is `mantissa >> (e - ex)`:
- mantissa is in [$2^{52}$, $2^{53}$).
- shift = 0 gives an increment of at most $2^{53}$ - 1.
- shift >= 1 gives at most $2^{52}$ after rounding up.
- shift > 53 gives 0.
- A term that does not fit (higher binade, negative, inf or NaN) gives `VMAF_ORDSUM_UNFIT` = $2^{54}$.

Every increment is >= 0 (`round_shifted` returns `whole + {0,1}`).

The composition `vmaf_ordsum_then` saturates at $2^{54}$. The int64 range therefore does not depend on the term count, the chunk size or N. For comparison, an uncapped sum would wrap after 512 UNFIT terms (512 x $2^{54}$ = $2^{63}$). With 1024 terms at the maximum increment it would reach 1024 x ($2^{53} - 1$) = $2^{63}$ - 1024, which still fits.

The cap never changes a result. A capped value (>= $2^{54}$) always fails `total > VMAF_ORDSUM_BINADE_END` in `add_chunk`, and that chunk is then added term by term.

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| ordered_sum.h:180 | `whole = (int64_t)(mantissa >> shift)` | `int64_t` (from `uint64_t`) | mantissa < $2^{53}$, shift in [1,53] (guards :217, :220) | < $2^{52}$ | < $2^{52}$ (N-independent) | SAFE ($2^{52}$ < $2^{63}$) |
| ordered_sum.h:181-182 | `((uint64_t)1 << shift) - 1u`, `(uint64_t)1 << (shift - 1)` | `uint64_t` | shift in [1,53] | shift <= 53 < 64 | same | SAFE (no shift >= width) |
| ordered_sum.h:184, :188 | `whole + (rest > half)`, `whole + odd_whole`, `whole + 1 - odd_whole` | `int64_t` | whole < $2^{52}$, +1 | <= $2^{52}$ | <= $2^{52}$ | SAFE ($2^{52}$ < $2^{63}$) |
| ordered_sum.h:214 | `shift = e - ((int)field - VMAF_ORDSUM_EXP_BIAS)` | `int` | e in [-900,900] (plan checked by `plan_is_binade`, :230), field in [1,2046] (2047 removed by `term_is_unfit`, 0 handled at :212) | [-1923, 1922] | same | SAFE (<< $2^{31}$) |
| ordered_sum.h:219-221 | `mantissa = (bits & FRAC) \| 2^52`; `(int64_t)mantissa` | `uint64_t` -> `int64_t` | one term's significand | < $2^{53}$ | < $2^{53}$ | SAFE ($2^{53}$ < $2^{63}$) |
| ordered_sum.h:249-251, :259-261 | `vmaf_ordsum_then`: `a.even + (b.odd\|b.even)`, `a.odd + ...`, then `vmaf_ordsum_cap` | `int64_t` | both operands in [0, 2^54]: an increment (<= $2^{53} - 1$) or UNFIT ($2^{54}$), or an earlier `then` result capped to $2^{54}$. Term count: 4 per lane plus an 8-level lane tree = 1024 per chunk; it does not matter because of the cap | pre-cap <= $2^{55}$; stored <= $2^{54}$ | same (N-independent) | SAFE ($2^{55}$ < $2^{63}$; saturating) |
| ordered_sum.h:320-321 | `total = m + ((m & 1) ? units.odd : units.even)` | `int64_t` | m in [$2^{52}$, $2^{53}$) (running sum's significand), units <= $2^{54}$ | < $2^{53}$ + $2^{54}$ < $2^{55}$ | same | SAFE ($2^{55}$ < $2^{63}$) |
| ordered_sum.h:293-296 | `(uint64_t)(e + 1 + 1023) << 52`, `(uint64_t)(e + 1023) << 52 \| (total & FRAC)` | `int` -> `uint64_t` | e in [-900,900] (`binade_bits(s) == plan`, :318); total in [$2^{52}$, $2^{53}$] (:322) | exponent field in [123, 1924] < 2047; no carry into the sign bit | same | SAFE |
| ordered_sum.h:125, :144 | `(bits >> 52) & 0x7ff`; `(int)field - 1023` | `unsigned`, `int` | 11-bit field | [-1023, 1024] | same | SAFE |
| ordered_sum.h:92-97 to `cuda/.../ssimulacra2_device.cu:394, :401`, `hip/.../ssimulacra2_device.hip:654, :662` | plan `int` -> `(short)` -> `int16_t` | `short` / `int16_t` | plan values: [-900, 900], `PLAN_ZERO` 32767, `PLAN_TERMS` -32768 | all values in int16 range (endpoints exact) | same | SAFE (no narrowing loss) |

### SSIMULACRA 2: CUDA twin (`cuda/ssimulacra2_cuda.c`, `cuda/ssimulacra2_cuda.h`, `cuda/ssimulacra2/ssimulacra2_device.cu`, `cuda/ssimulacra2/ssimulacra2_blur.cu`)

There are no integer accumulators besides the ordered-sum units. The rows below are size products, index and offset math, counters and grid dims.

Input pictures are `cuMemAllocPitch` planes. `pitch` is carried as `size_t` (`ssimulacra2_cuda.h:71`, set at `ssimulacra2_cuda.c:441` from `(size_t)stride`) and multiplied in `size_t` (device.cu:82).

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| CUDA ssimulacra2_cuda.c:327 | `peak = (float)((1u << bpc) - 1u)` | `unsigned` | bpc in [8,16] (picture.c:181) | 65,535 | 65,535 | SAFE (< $2^{16}$) |
| CUDA ssimulacra2_cuda.c:349-350, :359-360 | `(width + sh) >> sh`; `(scale_w[i-1] + 1u) / 2u` | `unsigned` | dims <= 32768 | <= 15,361 | <= 32,769 | SAFE |
| CUDA ssimulacra2_cuda.c:370-373 | `ss2c_chunks`: `(unsigned)((pixels + 1023) / 1024)` | `size_t` -> `unsigned` | N / 1024 per plane | 129,600 | $2^{20}$ | SAFE ($2^{20}$ < $2^{32}$) |
| CUDA ssimulacra2_cuda.c:380 | `(size_t)cw * (size_t)ch` | `size_t` | W x H | 132,710,400 | $2^{30}$ | SAFE |
| CUDA ssimulacra2_cuda.c:454-457 | yuv grid `gx = (W+15)/16`, `gy = (H+7)/8`, z = 2 | `unsigned` | gridDim.y limit 65535 | gx 960, gy 1,080 | gx 2,048, gy 4,096 | SAFE (gy <= 65535) |
| CUDA ssimulacra2_cuda.c:469 | `unsigned pixels = scale_w[scale] * scale_h[scale]` | `unsigned` x `unsigned` | W x H of the scale; passed as a kernel arg `unsigned pixels` | 132,710,400 | $2^{30}$ = 1,073,741,824 | SAFE ($2^{30}$ < $2^{32}$; also < INT32_MAX) |
| CUDA ssimulacra2_cuda.c:471-472 | xyb grid `gx = (pixels + 255) / 256`, gy = 2 | `unsigned` | gridDim.x limit $2^{31} - 1$ | 518,400 | 4,194,304 = $2^{22}$ | SAFE |
| CUDA ssimulacra2_cuda.c:498-503 | blur grids `gh = ceil(H/32)` (x), `gv = ceil(W/64)` (x); y = 5 jobs, z = 3 | `unsigned` | — | gh 270, gv 240 | gh 1,024, gv 512 | SAFE |
| CUDA ssimulacra2_cuda.c:524-526 | `totals + (size_t)scale * 18`; `a.pixels = (size_t)W * H`; `a.chunks` | `size_t`, `unsigned` | — | pixels 132.7 M, chunks 129,600 | $2^{30}$, $2^{20}$ | SAFE |
| CUDA ssimulacra2_cuda.c:528-535 | combine grids `gx = a.chunks` (x), y = 3; `gx = 6` sums | `unsigned` | gridDim.x <= $2^{31} - 1$ | 129,600 | $2^{20}$ | SAFE |
| CUDA ssimulacra2_cuda.c:554-557 | downsample grid `ceil(ow/16)`, `ceil(oh/8)` | `unsigned` | gridDim.y <= 65535 | 480 / 540 | 1,024 / 2,048 | SAFE |
| CUDA ssimulacra2_cuda.c:614 | readback `bytes = num_scales * 18 * sizeof(double)` | `size_t` | <= 6 scales | 864 B | 864 B | SAFE |
| CUDA ssimulacra2_cuda.c:740 | `full = (size_t)3 * width * height * sizeof(float)` | `size_t` (cast on the first operand, left to right) | 3 planes x N x 4 B; 16 buffers (lin x2, xyb x2, pass x5, blurred x5) | 1,592,524,800 B | 12,884,901,888 B = 3 x $2^{32}$ (> UINT32_MAX; no 32-bit intermediate) | SAFE ($2^{33.6}$ < $2^{64}$) |
| CUDA ssimulacra2_cuda.c:741 | `half = (size_t)3 * scale_w[1] * scale_h[1] * 4` | `size_t` | 3 x N/4 x 4 B | 398,131,200 B | 3,221,225,472 B | SAFE |
| CUDA ssimulacra2_cuda.c:742 | `totals = 6 * 18 * sizeof(double)` | `size_t` | — | 864 B | 864 B | SAFE |
| CUDA ssimulacra2_cuda.c:745 | `slots = 18 * (size_t)ss2c_chunks(W*H)` | `size_t` | 3 channels x 6 sums x chunks(scale 0) | 2,332,800 | 18,874,368 | SAFE |
| CUDA ssimulacra2_cuda.c:766-768 | `slots * sizeof(double)`, `slots * sizeof(int16_t)`, `slots * 2u * sizeof(int64_t)` | `size_t` (`vmaf_cuda_buffer_alloc(..., size_t)`, `cuda/common.h:148`) | per-chunk arrays | 18,662,400 / 4,665,600 / 37,324,800 B | 150,994,944 / 37,748,736 / 301,989,888 B | SAFE (< $2^{29}$) |
| CUDA ssimulacra2_device.cu:68-72 | `ss2c_map`: `plane_dim * 2u == luma_dim`; `(uint64_t)v * plane_dim / luma_dim` | `unsigned`; `uint64_t` | chroma coordinate map | <= $2^{15}$ x $2^{15}$ | <= $2^{30}$ | SAFE |
| CUDA ssimulacra2_device.cu:82 | `(size_t)sy * a.pitch[img][p]` (byte offset into a pitched plane) | `size_t` | row x pitch; 16-bit pitch >= 2W | 8,639 x 30,720 ~ 265 MB | 32,767 x 65,536 ~ $2^{31}$ B (> INT32_MAX; `size_t`) | SAFE |
| CUDA ssimulacra2_device.cu:277-278, :508-509 | `x`, `y`, `ox`, `oy = blockIdx * blockDim + threadIdx` | `unsigned` | grid x block | <= 15,375 | <= 32,783 | SAFE |
| CUDA ssimulacra2_device.cu:294-299 | `plane = (size_t)W * H`; `idx = (size_t)y * W + x`; `2u * plane + idx` | `size_t` | element offsets into a 3-plane buffer | < 3 x 132.7 M | < 3 x $2^{30}$ | SAFE |
| CUDA ssimulacra2_device.cu:308 | `i = blockIdx.x * blockDim.x + threadIdx.x` | `unsigned` | gx x 256 | <= 132,710,655 | <= $2^{30}$ + 255 | SAFE |
| CUDA ssimulacra2_device.cu:313-315, :347-349 | `lin[pixels + i]`, `lin[2u * pixels + i]`, `xyb[2u * pixels + i]` | `unsigned` (32-bit element index, widened only at the pointer add) | 3-plane compact buffer, i < pixels | max 398,131,199 | max 3 x $2^{30} - 1$ = 3,221,225,471 < 4,294,967,295 | SAFE (< $2^{32}$, margin 1.33x; would exceed INT32_MAX at cap if it were `int`) |
| CUDA ssimulacra2_device.cu:146-148 | `i = (size_t)chunk * 1024 + (size_t)lane * 4 + j`; `(size_t)c * a.pixels + i` | `size_t` | pixel index in channel c | < 3 x 132.7 M | < 3 x $2^{30}$ | SAFE |
| CUDA ssimulacra2_device.cu:161-166, :175-190 | shared-memory indices `lane * 6 + k`, `(lane * 6 + k) * 2u` | `unsigned` | lane < 256, k < 6 | <= 3,071 | <= 3,071 | SAFE |
| CUDA ssimulacra2_device.cu:199-209 | `ss2c_ordered_tree`: 8 levels of `vmaf_ordsum_then` on `long long shared[]` | `long long` / `int64_t` | 256 lane increments, each already <= $2^{54}$ (cap) | <= $2^{55}$ pre-cap | same | SAFE (see ordered_sum :259) |
| CUDA ssimulacra2_device.cu:420-426 | per-lane `units[k] = vmaf_ordsum_then(units[k], planned_term(...))` | `VmafOrdsumUnits` (`int64_t` x 2) | 4 terms per lane | <= $2^{55}$ pre-cap | same | SAFE |
| CUDA ssimulacra2_device.cu:225-226, :238-243 | `chunk = (size_t)batch * 1024 + slot`; `sums[chunk * 6]`; `units[chunk * 2u (+1u)]` | `size_t` | chunk < chunks | < 129,600 x 6 | < $2^{20}$ x 6 | SAFE |
| CUDA ssimulacra2_device.cu:254-265 | walk counter `*chunk` (`(*chunk)++`), `*chunk / 1024`, `*chunk % 1024` | `unsigned` | 0 .. chunks | <= 129,600 | <= $2^{20}$ | SAFE |
| CUDA ssimulacra2_device.cu:369 | `((size_t)c * a.chunks + chunk) * 6` | `size_t` | chunk_sums offset | < 2.33 M | < 18.9 M | SAFE |
| CUDA ssimulacra2_device.cu:385-387 | `(size_t)c * a.chunks * 6 + k`; `((size_t)c * 6 + k) * a.chunks`; `batches = (a.chunks + 1023) / 1024` | `size_t`; `unsigned` | — | batches 127 | batches 1,024 | SAFE |
| CUDA ssimulacra2_device.cu:416, :424, :434 | `first = ((size_t)c * 6) * a.chunks + chunk`; `first + (size_t)k * a.chunks`; `(...) * 2u` | `size_t` | plan/units offsets | < 2 x 2.33 M | < 2 x 18.9 M | SAFE |
| CUDA ssimulacra2_device.cu:457-458 | `first = ((size_t)c * 6 + k) * a.chunks`; `rounds = a.chunks + a.chunks / 1024 + 2u` | `size_t`; `unsigned` | walk round bound. Each round is one batch load (ceil(chunks/1024)) or one term-wise chunk (<= chunks), plus DONE, so the bound suffices | rounds 129,728 | rounds 1,049,602 | SAFE |
| CUDA ssimulacra2_device.cu:487 | `chunk = what + 1u` | `unsigned` | — | <= 129,600 | <= $2^{20}$ | SAFE |
| CUDA ssimulacra2_device.cu:514-530 | `in_plane = (size_t)iw * ih`; `ox * 2u + dx`; `(size_t)iy * iw + ix`; `(size_t)c * out_plane + (size_t)oy * ow + ox` | `size_t`, `unsigned` | downsample offsets | < 3 x 132.7 M | < 3 x $2^{30}$ | SAFE |
| CUDA ssimulacra2_blur.cu:96-97 | `base + (size_t)row * a.width + col` | `size_t` | element offset | < 3 x 132.7 M | < 3 x $2^{30}$ | SAFE |
| CUDA ssimulacra2_blur.cu:116-119 | `row0 = blockIdx.x * 32`; `plane = (size_t)W * H`; `base = (size_t)blockIdx.z * plane` | `unsigned`; `size_t` | — | row0 <= 8,640; base < 2 x 132.7 M | row0 <= 32,768; base < $2^{31}$ | SAFE |
| CUDA ssimulacra2_blur.cu:120, :134-139, :147 | `w = (int)a.width`; `(int)(chunk * 32) - lead`; `n`, `left`, `col` | `int` (from `unsigned`) | chunk <= ceil((W + radius - 1) / 32) | <= ~15,400 | <= ~32,800 | SAFE (<< $2^{31}$) |
| CUDA ssimulacra2_blur.cu:136 | `(chunk + 1u) * 32 + lane` | `unsigned` | tile column | <= ~15,450 | <= ~32,850 | SAFE |
| CUDA ssimulacra2_blur.cu:150 | `(size_t)(row0 + r) * a.width + (unsigned)col` | `size_t` | pass-buffer offset | < 132.7 M | < $2^{30}$ | SAFE |
| CUDA ssimulacra2_blur.cu:163, :167 | `col = blockIdx.x * 64 + threadIdx.x`; `(size_t)blockIdx.z * a.width * a.height + col` | `unsigned`; `size_t` | — | < 3 x 132.7 M | < 3 x $2^{30}$ | SAFE |
| CUDA ssimulacra2_blur.cu:170-182 | `h = (int)a.height`; `n`, `left`, `right`; `(size_t)left * w`, `(size_t)n * w` | `int`; `size_t` | — | n <= 8,644; offsets < 132.7 M | n <= 32,772; offsets < $2^{30}$ | SAFE |

### SSIMULACRA 2: HIP twin (`hip/ssimulacra2_hip.c`, `hip/ssimulacra2_hip.h`, `hip/ssimulacra2/ssimulacra2_device.hip`)

The HIP twin does not read the picture's device pitch. `submit()` packs every plane into pinned staging with `row_bytes = plane_w x bytes_per_sample` (`ss2h_stage_plane`, :895-907) and copies it into a flat `hipMalloc` per plane. The kernel therefore indexes `sy * plane_w + sx` in `size_t`.

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| HIP ssimulacra2_hip.c:397 | `peak = (float)((1u << bpc) - 1u)` | `unsigned` | bpc in [8,16] | 65,535 | 65,535 | SAFE |
| HIP ssimulacra2_hip.c:421-423 | plane dims; `row_bytes[p] = (size_t)plane_w[p] * bytes_per_sample` | `unsigned`; `size_t` | — | 30,720 B | 65,536 B | SAFE |
| HIP ssimulacra2_hip.c:432-433 | `(scale_w[i-1] + 1u) / 2u` | `unsigned` | — | <= 7,680 | <= 16,384 | SAFE |
| HIP ssimulacra2_hip.c:445 | `(size_t)6 * 18 * sizeof(double)` | `size_t` | — | 864 B | 864 B | SAFE |
| HIP ssimulacra2_hip.c:449-453 | `ss2h_chunks`: `(unsigned)((pixels + 1023) / 1024)` | `size_t` -> `unsigned` | — | 129,600 | $2^{20}$ | SAFE |
| HIP ssimulacra2_hip.c:521-526 | `slots = 18 * (size_t)ss2h_chunks(...)`; `slots * 8`, `slots * 2`, `slots * 2u * 8` | `size_t` (`hipMalloc(void**, size_t)`) | per-chunk arrays | 2,332,800 slots; 18.7 M / 4.7 M / 37.3 M B | 18,874,368 slots; 151.0 M / 37.7 M / 302.0 M B | SAFE |
| HIP ssimulacra2_hip.c:535-536 | `full = (size_t)3 * W * H * 4`; `half = (size_t)3 * w1 * h1 * 4` | `size_t` | 3-plane fp32 buffers (lin x4, xyb x2, scratch, mu1, mu2, s11, s22, s12) | 1,592,524,800 / 398,131,200 B | 12,884,901,888 / 3,221,225,472 B | SAFE (`size_t` from the first operand) |
| HIP ssimulacra2_hip.c:540, :573, :804 | `row_bytes[p] * plane_h[p]` (`hipMalloc`, `hipHostMalloc`, `hipMemcpyAsync` size) | `size_t` x `unsigned` -> `size_t` | packed raw plane; 16-bit luma | 265,420,800 B | $2^{31}$ = 2,147,483,648 B (> INT32_MAX; `size_t`) | SAFE |
| HIP ssimulacra2_hip.c:658-661 | `ss2h_blocks`: `(unsigned)((items + block - 1u) / block)` | `size_t` -> `unsigned` | largest: xyb, items = W x H, block 256 | 518,400 | $2^{22}$ | SAFE |
| HIP ssimulacra2_hip.c:690-691 | yuv grid `ceil(W/16)` x `ceil(H/8)` | `unsigned` | per-dim work-items <= UINT32_MAX | 960 x 1,080 | 2,048 x 4,096 | SAFE |
| HIP ssimulacra2_hip.c:709, :717 | rows grid `ceil(H/32)` x 3; cols grid `ceil((size_t)W * 3 / 64)` | `unsigned` | — | 270; 720 | 1,024; 1,536 | SAFE |
| HIP ssimulacra2_hip.c:743-756 | `pixels = (size_t)w * h`; `totals + (size_t)scale * 18`; `chunks` | `size_t`; `unsigned` | — | 132.7 M; 129,600 | $2^{30}$; $2^{20}$ | SAFE |
| HIP ssimulacra2_hip.c:758-764 | chunk-kernel grids `gx = a.chunks` (x 256 lanes) | `unsigned` | work-items = chunks x 256 | 33,177,600 | $2^{28}$ | SAFE (< $2^{32}$ per-dim limit) |
| HIP ssimulacra2_hip.c:779 | xyb grid `ss2h_blocks((size_t)cw * ch, 256)` | `unsigned` | work-items ~ N | 518,400 blocks | $2^{22}$ blocks ($2^{30}$ work-items) | SAFE |
| HIP ssimulacra2_hip.c:793-794 | downsample grid | `unsigned` | — | 480 x 540 | 1,024 x 2,048 | SAFE |
| HIP ssimulacra2_hip.c:828 | `(size_t)cw * (size_t)ch` | `size_t` | — | 132.7 M | $2^{30}$ | SAFE |
| HIP ssimulacra2_hip.c:900-906 | `stride = (size_t)pic->stride[p]`; `row_bytes * rows`; `(size_t)i * row_bytes`; `(size_t)i * stride` | `size_t` | host staging copy | <= 265 MB | <= $2^{31}$ B | SAFE |
| HIP ssimulacra2_device.hip:79-83 | `ss2h_map`: `plane_dim * 2u`; `(uint64_t)v * plane_dim / luma_dim` | `unsigned`; `uint64_t` | — | <= $2^{28}$ | <= $2^{30}$ | SAFE |
| HIP ssimulacra2_device.hip:91 | `idx = (size_t)sy * a.plane_w[p] + sx` (packed plane) | `size_t` | element index | < 132.7 M | < $2^{30}$ | SAFE |
| HIP ssimulacra2_device.hip:124-129 | `plane = (size_t)W * H`; `idx = (size_t)y * W + x`; `2u * plane + idx` | `size_t` | — | < 3 x 132.7 M | < 3 x $2^{30}$ | SAFE |
| HIP ssimulacra2_device.hip:147-149, :169-171, :563-564 | xyb `i = (size_t)blockIdx.x * blockDim.x + threadIdx.x`; `plane + i`, `2u * plane + i` | `size_t` | (`size_t` here where CUDA uses `unsigned`) | < 3 x 132.7 M | < 3 x $2^{30}$ | SAFE |
| HIP ssimulacra2_device.hip:178-190 | `in_plane`, `out_plane` (`size_t`); `c * in_plane`; `min(ox * 2u + dx, W - 1u)`; `(size_t)iy * W + ix`; `c * out_plane + (size_t)oy * ow + ox` | `size_t`, `unsigned` | downsample offsets | < 3 x 132.7 M | < 3 x $2^{30}$ | SAFE |
| HIP ssimulacra2_device.hip:222-233 | `ss2h_iir_line`: `n`, `left`, `right` (`int`); `base + (size_t)left * step` | `int`; `size_t` | step = 1 or W; xsize <= 32768 | < 3 x 132.7 M | < 3 x $2^{30}$ | SAFE |
| HIP ssimulacra2_device.hip:290-292 | lane-term index, `(size_t)c * a.pixels + i` | `size_t` | — | < 3 x 132.7 M | < 3 x $2^{30}$ | SAFE |
| HIP ssimulacra2_device.hip:304-357 | block tree (double, advice only, not an integer); `ss2h_units_slot`; `ss2h_ordered_tree` on `long long shared[]` via `vmaf_ordsum_then` | `unsigned`; `long long` | 256 lane increments, each <= $2^{54}$ | slot <= 3,071; units <= $2^{55}$ pre-cap | same | SAFE |
| HIP ssimulacra2_device.hip:368-415 | staging `(size_t)batch * 1024 + slot`, `sums[chunk * 6]`, `units[chunk * 2u]`; walk `*chunk` counter | `size_t`; `unsigned` | — | <= 129,600 | <= $2^{20}$ | SAFE |
| HIP ssimulacra2_device.hip:439 | `plane = (size_t)c * args.width * args.height` | `size_t` | — | < 3 x 132.7 M | < 3 x $2^{30}$ | SAFE |
| HIP ssimulacra2_device.hip:466-473 | `col = (unsigned)chunk * 16 + lane % 16`; `row = row0 + j * 2 + sub`; `(size_t)row * W + col` | `unsigned`; `size_t` | — | col <= ~15,380 | col <= ~32,790 | SAFE |
| HIP ssimulacra2_device.hip:494-529 | `row0 = blockIdx.x * 32`; `width = (int)W`; `(chunk * kCols) - lead`; `n`, `left`, `left / 16`, `left % 16` (left >= 0); `(size_t)(row0 + r) * W + (size_t)col` | `unsigned`; `int`; `size_t` | — | int values <= ~15,380 | int values <= ~32,790 | SAFE |
| HIP ssimulacra2_device.hip:550-551, :770-771 | `x`, `y`, `ox`, `oy` from blockIdx | `unsigned` | — | <= 15,375 | <= 32,783 | SAFE |
| HIP ssimulacra2_device.hip:599-606 | `line = blockIdx.x * 64 + threadIdx.x`; `lines = W * 3`; `l = line - c * W`; `base = (size_t)c * W * H + l` | `unsigned`; `size_t` | — | lines 46,080 | lines 98,304 | SAFE |
| HIP ssimulacra2_device.hip:628, :645-647, :677, :686, :697, :719-720 | chunk_sums/plan/units offsets (`size_t`); `batches`, `rounds = chunks + chunks / 1024 + 2u` (`unsigned`) | `size_t`; `unsigned` | same as CUDA | rounds 129,728 | rounds 1,049,602 | SAFE |
| HIP ssimulacra2_device.hip:682-688 | per-lane `vmaf_ordsum_then` over 4 terms | `int64_t` x 2 | 4 terms | <= $2^{55}$ pre-cap | same | SAFE |
| HIP ssimulacra2_device.hip:751, :762 | `chunk = what + 1u`; `totals[(size_t)c * 6 + k]` | `unsigned`; `size_t` | — | <= 129,600 | <= $2^{20}$ | SAFE |

### SSIMULACRA 2: shared headers, integer parts (`ssimulacra2_math.h`, `ssimulacra2_eotf_lut.h`, `ssimulacra2_score.h`, `ssimulacra2_pixel_format.h`)

These are compiled into both device modules (CUDA: device.cu:39-43; HIP: device.hip:54-58).

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| ssimulacra2_math.h:77 | `u.i = u.i / 3u + 0x2a5137a0u` (cube-root seed) | `uint32_t` | x > 0 (:66). Largest bit pattern: +NaN 0x7fffffff; -NaN passes `x <= 0` as false, up to 0xffffffff | 0xffffffff / 3 + 0x2a5137a0 = 0x7fa68cf5 | same (input-value bound, not N) | SAFE (< $2^{32}$, unsigned; no wrap) |
| ssimulacra2_math.h:107-111 | `idx = x * 1023.0f`; `i = (int)idx`; `lut[i]`, `lut[i + 1]` | `float` -> `int` | x in (0, 1) after the guards at :101, :104. Largest x = 1 - $2^{-24}$ gives `fl(x * 1023)` = 1022.99994 (checked with numpy float32), so i <= 1022 and i + 1 <= 1023 = size - 1 | i in [0, 1022] | same | SAFE (index in bounds). Aside: NaN passes both guards, and `(int)NaN` is UB on the host. It is not reachable: the inputs are integer samples times finite coefficients, clamped by `ss2c_clampf` / `ss2h_clampf` |
| ssimulacra2_eotf_lut.h:16, :33 | `SS2_EOTF_LUT_SIZE 1024`; `float vmaf_ss2_eotf_lut[1024]` | `float[]` | data only | — | — | none (no integer math) |
| ssimulacra2_score.h | `vmaf_ss2_split_edge_difference`, `vmaf_ss2_finalize_score` | `double` | fp only | — | — | none |
| ssimulacra2_pixel_format.h | `vmaf_ss2_has_chroma`, `vmaf_ss2_check_pixel_format` | enum compare | no arithmetic | — | — | none |

### Coverage

All files are under `core/src/feature/`.

- `ordered_sum.h` (full, 344 lines): 10 rows. Covers the binade increments, `round_shifted`, `then` composition and cap, `add_chunk` total, `from_units_bits` exponent and the int16 plan narrowing.
- `cuda/ssimulacra2_cuda.c` (850 lines): 17 rows. Covers host size products, chunk count, grids, readback and the `unsigned pixels` kernel arg.
- `cuda/ssimulacra2_cuda.h` (120 lines): none of its own. It holds the types used by the rows above: `pitch` is `size_t`, `pixels` is `size_t`, `chunks` is `unsigned`, `units` is `int64_t`, `plan` is `int16_t`.
- `cuda/ssimulacra2/ssimulacra2_device.cu` (534 lines): 18 rows.
- `cuda/ssimulacra2/ssimulacra2_blur.cu` (186 lines): 7 rows.
- `hip/ssimulacra2_hip.c` (1075 lines): 17 rows.
- `hip/ssimulacra2_hip.h` (130 lines): none of its own; it holds the types (`pixels` `size_t`, `chunks` `unsigned`, `units` `int64_t`, `plan` `int16_t`).
- `hip/ssimulacra2/ssimulacra2_device.hip` (780 lines): 17 rows.
- `ssimulacra2_math.h` (115 lines): 2 rows (cube-root seed, EOTF LUT index).
- `ssimulacra2_eotf_lut.h` (1061 lines; integer parts only): none.
- `ssimulacra2_score.h` (55 lines): none.
- `ssimulacra2_pixel_format.h` (51 lines): none.
- Cross-checked, not in the group: `core/src/picture.c:37-38, :46, :181-185` (bpc 8-16, dims <= 32768); `core/src/cuda/common.h:148, :273` (`vmaf_cuda_buffer_alloc` and `vmaf_cuda_buffer_host_alloc` take `size_t`).

Verdict totals: 88 rows (shared ordered_sum 10, CUDA 42, HIP 34, math.h 2). SAFE 88, OVERFLOW@16K 0, OVERFLOW@CAP-ONLY 0, DEPENDS 0. Five more entries are listed as "none" (lut decl, score.h, pixel_format.h, and the two struct headers).

Tightest margin in the group: CUDA `ssimulacra2_xyb`, which indexes with the 32-bit `unsigned` `2u * pixels + i` (device.cu:315, :349). At cap the index reaches 3,221,225,471, which is below $2^{32}$ = 4,294,967,296 (headroom $2^{0.42}$). It is safe only because N <= $2^{30}$ and the type is unsigned: as `int` it would overflow at cap, and with a fourth plane it would wrap. The HIP twin uses `size_t` for the same index.

## G5: CAMBI and PSNR-HVS, CUDA and HIP twins: integer-overflow audit

Repo: master at 571565a47. Read-only; nothing built or run on a device.
Paths are relative to `core/src/feature/`.

Envelope: 16K = 15360x8640, N = 132,710,400 ($2^{26.98}$). Cap = W,H <= 32768, N <= $2^{30}$.

### CAMBI

#### Bounds that apply to every CAMBI row

- **Window guard.** `vmaf_cambi_check_window_fits_lut()` (cambi.c:1833) runs in the CUDA init (integer_cambi_cuda.c:400) and the HIP init (integer_cambi_hip.c:393). It rejects any window w with $w^{2}$ >= 4226, for both the encode window and the source window. So w <= 65, pad <= 32, and no window holds more than 65*65 = 4225 pixels, at any resolution.
- **What the guard means for resolution.** At window_size >= 15 (the option minimum), the guard rejects W+H >= 26,400, or W+H >= 52,400 with the high-res speed-up. So W=H=32768 never runs CAMBI.
  - Largest reachable per-scale n is about 1.742e8 ($2^{27.38}$).
  - Largest reachable preprocess/mask plane is about 6.86e8 ($2^{29.35}$), with the speed-up.
  - 16K runs only with window_size 15 or 16, or window_size <= 32 with the speed-up.
  - The "cap" column still uses n = $2^{30}$, the conservative value. No verdict changes because of this.
- **Levels.** levels = v_band_size <= tvi_max + 1 <= 1023 + 32 + 1 = 1056 (cambi.c:1951-1965). Preprocessed samples are <= 1024: for 16 bpc, (65535 + 32) >> 6 = 1024.
- **c-value maximum.** c = w \* p0 \* pm / (p0 + pm). Weight w <= 9 (g_contrast_weights, cambi.c:97). p0 and pm count pixels of two different levels in one window, so p0 + pm <= 4225. That gives c <= 9 \* 2112 \* 2113 / 4225 = 9506.25 ($2^{13.22}$). The fixed point is c \* $2^{24}$ < $2^{37.22}$. Call this F.
- **No clip-level accumulators.** Every integer sum is reset each frame (memset of the select/results/frame state), and the score leaves as a per-frame double. Frames-to-overflow does not apply.

#### CUDA (cuda/integer_cambi/cambi_score.cu, cuda/integer_cambi_cuda.c, cuda/integer_cambi_cuda.h)

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| CUDA cuda/integer_cambi/cambi_score.cu:329 | `*cell = (uint16_t)((int)*cell + count)` (hist_apply, sliding column histogram cell) | `uint16_t` cell, `int` intermediate | Window count of one level for one column. Runs are added and removed one thread at a time, in sequence: remove row y-pad-1, then add row y+pad. The true count stays in 0..4225. | <= 4225 | <= 4225 (guard is independent of resolution) | SAFE (4225 < 65535, no transient wrap) |
| CUDA cuda/integer_cambi/cambi_score.cu:347,352 | `sign * (int)(x - start)`, `sign * (int)(hi + 1u - start)` | `int` | Run length inside the window columns [lo, hi] | <= 2*pad+1 = 65 | 65 | SAFE |
| CUDA cuda/integer_cambi/cambi_score.cu:398 | `__ldg(c.weights + d) * p0 * pm` | `int * int * int` | Weight <= 9, p0 + pm <= 4225 | <= 9*2112*2113 = 40,163,904 ($2^{25.3}$); loose bound 9*$4225^{2}$ = 160,655,625 ($2^{27.3}$) | same | SAFE ($2^{27.3}$ < $2^{31}$) |
| CUDA cuda/integer_cambi/cambi_score.cu:398 | `c.lut + pm + p0` (reciprocal LUT index) | `int` | p0 + pm | <= 4225 < CAMBI_RECIPROCAL_LUT_SIZE 4226 | same | SAFE |
| CUDA cuda/integer_cambi/cambi_score.cu:95 | `cambi_fixed()`: `__float2ull_rz(__fmul_rn(value, 2^24))` | `uint64_t` | One c-value < $2^{13.22}$, scaled by $2^{24}$ | < $2^{37.22}$ | same | SAFE |
| CUDA cuda/integer_cambi/cambi_score.cu:445 | `tally.sum += cambi_fixed(value)` | `uint64_t` | One term per row of the thread's chunk. chunk_rows <= H; term < F. | 8640 \* F = $2^{50.29}$ | 32768 \* F = $2^{52.21}$ | SAFE (< $2^{63}$) |
| CUDA cuda/integer_cambi/cambi_score.cu:444,432 | `++tally.count`; `atomicAdd(&select->hist[bin], tally.count)` | `uint32_t` | Radix pass-0 bin counts. Every c-value of the scale is counted once. | <= n = 132,710,400 ($2^{26.98}$) | <= $2^{30}$ | SAFE (< $2^{32}$) |
| CUDA cuda/integer_cambi/cambi_score.cu:104-115 (c-values kernel, 64 threads, :737) | `warp_reduce((int64_t)value)` (cuda/cuda_helper.cuh:125, rebuilt from two 32-bit-half shuffles; exact while < $2^{63}$); `total += warp_sums[w]` | `int64_t` / `uint64_t` | 64 per-thread tallies | 64 \* $2^{50.29}$ = $2^{56.29}$ | $2^{58.21}$ | SAFE (< $2^{63}$, the signed warp_reduce) |
| CUDA cuda/integer_cambi/cambi_score.cu:740 | `partials[(size_t)blockIdx.y * gridDim.x + blockIdx.x]` | `size_t` | Partial-sum index (cvals_groups) | <= 64,800 | <= $2^{19}$ | SAFE |
| CUDA cuda/integer_cambi/cambi_score.cu:487-489 (pool_block), :505, :826 | `(a.n + a.groups - 1u) / a.groups`; `group * per_group`; `begin + per_group`; loop `i += 256` | `unsigned` | n <= $2^{30}$; groups = min(512, ceil(n/4096)); per_group = ceil(n/groups) | n + 511; begin + per_group <= n + 259,200 | <= $2^{30}$ + $2^{21}$ | SAFE (< $2^{32}$) |
| CUDA cuda/integer_cambi/cambi_score.cu:504-519 | `cur_count`; `atomicAdd(&local_hist[cur_bin], cur_count)` | `uint32_t` (shared) | Elements of one pool group | <= 259,200 | <= $2^{21}$ | SAFE |
| CUDA cuda/integer_cambi/cambi_score.cu:767 | `atomicAdd(&select->hist[b], local_hist[b])` | `uint32_t` | Pass 1/2 bin counts, <= n in total | <= $2^{26.98}$ | <= $2^{30}$ | SAFE |
| CUDA cuda/integer_cambi/cambi_score.cu:128,137,139,788,791,795,801 | Exclusive scan `inclusive += below`, `base += warp_totals[w]`; `mine +=`; `before + mine`; `cum + count`; `k - cum` | `uint32_t` | Sums of radix bins; each pass counts <= n elements | <= $2^{26.98}$ | <= $2^{30}$ | SAFE |
| CUDA cuda/integer_cambi/cambi_score.cu:825-830 | `sum += ... cambi_fixed(value)`, then cambi_block_sum over 256 threads | `uint64_t` / `int64_t` warp_reduce | Fixed c-values above the threshold in one pool group; per_group = ceil(n/512) | Thread: 1013 terms = $2^{47.2}$. Block: 259,200 \* F = $2^{55.20}$. | Block: $2^{21}$ \* F = $2^{58.21}$ | SAFE (< $2^{63}$) |
| CUDA cuda/integer_cambi/cambi_score.cu:850-856 | `lo32 += partial & 0xFFFFFFFF`; `hi32 += partial >> 32`; two cambi_block_sum calls | `uint64_t` / `int64_t` | Summed over `count` = cvals_groups (resolved) or <= 512 partials. Each low half < $2^{32}$. Each high half < $2^{26.3}$. | cvals_groups <= 64,800, so lo <= $2^{47.98}$ and hi <= $2^{32.3}$ | cvals_groups <= $2^{19}$, so lo <= $2^{51}$ and hi <= $2^{45.3}$ | SAFE |
| CUDA cuda/integer_cambi/cambi_score.cu:862 | `(t_fixed >> 32u) * k_rem`; `(t_fixed & 0xFFFFFFFFu) * k_rem` | `uint64_t` | t_fixed < $2^{37.22}$; k_rem <= topk <= n | Low part < $2^{58.98}$; high part < $2^{32.2}$ | Low < $2^{62}$; high < $2^{35.22}$ | SAFE (< $2^{64}$) |
| CUDA cuda/integer_cambi/cambi_score.cu:530-543,863-866 | U128 `u128_from_halves` / `u128_add` -> `results->sum_lo/sum_hi` | 2x `uint64_t` (128-bit) | Exact top-K sum: k terms, each < F | <= n \* F = $2^{64.20}$. A single uint64 would wrap at 16K; the 128-bit pair is needed. | $2^{67.21}$ (hi word < $2^{4}$) | SAFE (128-bit; carries checked) |
| CUDA cuda/integer_cambi/cambi_score.cu:155 + cuda/integer_cambi_cuda.c:709 | `(size_t)row * a.src_pitch`, where `src_pitch = (uint32_t)dist->stride[0]` | `size_t` product; `uint32_t` pitch (narrowed from ptrdiff_t) | Byte offset in the cuMemAllocPitch luma plane | Pitch >= 30,720 B; offset ~2.65e8 | Pitch ~65,536 B (fits uint32); offset ~$2^{31}$ in size_t | SAFE |
| CUDA cuda/integer_cambi/cambi_score.cu:246,249,250,328,338,339,361,386,393,394,412,462,468,580,621,641,654,658,676,709-714,721,722 | Every image, level-map, histogram and mask index: `(size_t)y * width + x`, `(size_t)chunk * levels * width + col`, `(size_t)level * width`; at :641 `(size_t)(y * 2u) * src_stride + x * 2u` (y*2u is 32-bit, y < 16384) | `size_t` (first operand widened) | Element offsets < N; histogram <= chunks \* levels \* W | < $2^{27}$ | < $2^{30}$ | SAFE |
| CUDA cuda/integer_cambi/cambi_score.cu:612-617 | `box_sum += zd_tile[..]` (7x7 spatial-mask tile; no summed-area table on the device) | `unsigned` | 49 flags of 0/1 | <= 49 | 49 | SAFE |
| CUDA cuda/integer_cambi/cambi_score.cu:600-603,239-242 | `tid`, `k`, `k / 22`, `gy`, `gx` | `int` | Tile index <= 484; coordinates <= 32768+3 | small | small | SAFE |
| CUDA cuda/integer_cambi/cambi_score.cu:179,202-204 | `(v + rounding) >> shift`; 2x2 anti-dither `here + ... >> 2u` | `unsigned` | 4 samples, each <= 1024 | <= 4096 | same | SAFE |
| CUDA cuda/integer_cambi_cuda.c:370 | `s->enc_width * s->enc_height` (speed-up threshold) | `int * int` | Encode pixel count (enc dims = source dims when unset) | 132,710,400 | $2^{30}$ = 1,073,741,824 | SAFE (< INT_MAX) |
| CUDA cuda/integer_cambi_cuda.c:449-451 | `n = width * height`; `(int)(topk * (int)n)` | `unsigned`, `int` | Per-scale pixels | $2^{26.98}$ | $2^{30}$ | SAFE (< $2^{31}$) |
| CUDA cuda/integer_cambi_cuda.c:409-419,445-448 | `(unsigned)sms * 512u`; `(target_items + width - 1u)`; `(size_t)width * levels * sizeof(uint16_t)`; `chunk_rows`; `cvals_groups = chunks * ceil(width/64)` | `unsigned` / `size_t` | Chunking. chunks <= max(1, min(H/32, 64 MiB / chunk_bytes)) | cvals_groups <= 64,800 | <= $2^{19}$ | SAFE |
| CUDA cuda/integer_cambi_cuda.c:455,474-504,596-597,811,836 | `(size_t)g->chunks * width * s->levels`; `pixels = (size_t)proc_width * proc_height`; `pixels * sizeof(float)`; `words * 4`; `malloc(proc_width * sizeof(uint32_t))`; `(CUdeviceptr)(scale * sizeof(CambiCudaSelect))` | `size_t` | Buffer bytes | cvals 531 MB | cvals $2^{32}$ B | SAFE (size_t; a device-capacity limit, not a wrap) |
| CUDA cuda/integer_cambi_cuda.c:664-665,868-878 (aside, grid dims) | grid_y = ceil(H/16), ceil(H/8), chunks | `unsigned` | CUDA gridDim.y limit is 65,535 | 540 / 1080 / <= 270 | 2048 / 4096 / <= 1024 | SAFE |

#### HIP (hip/integer_cambi/cambi_hip_device.h, hip/integer_cambi/cambi_score.hip, hip/integer_cambi_hip.c, hip/integer_cambi_hip.h)

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| HIP hip/integer_cambi/cambi_hip_device.h:533 | `*cell = (uint16_t)((int)*cell + count)` (cambi_hd_hist_apply) | `uint16_t` / `int` | Same as CUDA: window count of one level, <= 4225 | <= 4225 | <= 4225 | SAFE |
| HIP hip/integer_cambi/cambi_hip_device.h:549,553 | `sign * (int)(x - start)`; `sign * (int)(hi + 1u - start)` | `int` | Run length | <= 65 | 65 | SAFE |
| HIP hip/integer_cambi/cambi_hip_device.h:586 | `a->weights[d] * p0 * pm`; `a->lut[pm + p0]` | `int32_t * int * int`; `int` index | Weight <= 9, p0 + pm <= 4225 | <= $2^{27.3}$; index <= 4225 | same | SAFE |
| HIP hip/integer_cambi/cambi_hip_device.h:252 | `cambi_hd_fixed()`: `(uint64_t)(value * 16777216.0f)` | `uint64_t` | c < $2^{13.22}$ | < $2^{37.22}$ | same | SAFE |
| HIP hip/integer_cambi/cambi_hip_device.h:630 | `tally->sum += cambi_hd_fixed(value)` | `uint64_t` | chunk_rows <= H terms, each < F | $2^{50.29}$ | $2^{52.21}$ | SAFE |
| HIP hip/integer_cambi/cambi_hip_device.h:629,616,223 | `++tally->count`; `cambi_hd_add_u32` -> `atomicAdd(radix_hist)` | `uint32_t` | Pass-0 bin counts, <= n | <= $2^{26.98}$ | <= $2^{30}$ | SAFE |
| HIP hip/integer_cambi/cambi_score.hip:43-53 (used at :183, :270) | cambi_group_sum `shared[lid] += shared[lid + span]` (tree reduction) | `uint64_t` (shared) | c-values group: 64 tallies. Pool group: <= per_group fixed terms. | $2^{56.29}$ / $2^{55.20}$ | $2^{58.21}$ / $2^{58.21}$ | SAFE (< $2^{64}$, unsigned) |
| HIP hip/integer_cambi/cambi_score.hip:186 + cambi_hip_device.h:240-246,232 | `cambi_hd_add_split(&sel->all_lo, &sel->all_hi, sum)`: `atomicAdd((unsigned long long *)...)` of `sum & 0xFFFFFFFF` and `sum >> 32` | `uint64_t` | One add per c-values work-group. Low halves < $2^{32}$. The high halves sum to <= total / $2^{32}$. | Groups <= 64,800, so all_lo <= $2^{47.98}$; all_hi <= $2^{32.2}$ | Groups <= $2^{19}$, so all_lo <= $2^{51}$; all_hi <= $2^{35.21}$ | SAFE |
| HIP hip/integer_cambi/cambi_score.hip:265-272 | `sum += ... cambi_hd_fixed(value)`, then group sum, then `add_split(gt_lo, gt_hi)` | `uint64_t` | Pool groups <= 512; each group's sum <= per_group \* F | Group $2^{55.20}$; gt_lo <= 512 \* $2^{32}$ = $2^{41}$; gt_hi <= $2^{32.2}$ | Group $2^{58.21}$; gt_lo $2^{41}$; gt_hi $2^{35.21}$ | SAFE |
| HIP hip/integer_cambi/cambi_score.hip:205,263 + cambi_hip_device.h:772-774 | `sc->width * sc->height`; `(n + groups - 1u) / groups`; `group * per_group`; `*begin + per_group` | `uint32_t` | n <= $2^{30}$ | <= n + 259,200 | <= $2^{30}$ + $2^{21}$ | SAFE |
| HIP hip/integer_cambi/cambi_score.hip:209,214 | `atomicAdd(&local_hist[bin], 1u)`; `atomicAdd(&sel->hist[b], local_hist[b])` | `uint32_t` | <= per_group; <= n | <= $2^{26.98}$ | <= $2^{30}$ | SAFE |
| HIP hip/integer_cambi/cambi_score.hip:232,236-241,245 + cambi_hip_device.h:717-727 | `mine +=`; lane-0 serial `running += count`; `before + mine`; `cum + count`; `k - cum` | `uint32_t` | Radix bin sums, <= n | <= $2^{26.98}$ | <= $2^{30}$ | SAFE |
| HIP hip/integer_cambi/cambi_hip_device.h:737-766 | `cambi_hd_topk_total`: `(t_fixed >> 32u) * k_rem`, `(t_fixed & 0xFFFFFFFFu) * k_rem`, `cambi_hd_u128_from_halves` / `_add` | `uint64_t` / 2x `uint64_t` | Same as CUDA: exact top-K sum | Ties < $2^{58.98}$; total $2^{64.20}$ (needs the 128-bit pair) | Ties < $2^{62}$; total $2^{67.21}$ | SAFE (128-bit) |
| HIP hip/integer_cambi/cambi_hip_device.h:300,312,359,408,417,426,433,451,453,671; hip/integer_cambi/cambi_score.hip:79,106,169,170 | \*\*32-bit element indices\*\*: `y * p->src_width + x`; `row * src_width + col`; `(uint32_t)y * proc_width + x`; `(y * 2u) * decimate_src_width + x * 2u`; `y * sc->width + x`; `idx = y * w + x`; `(y << mask_shift) * proc_width + (x << mask_shift)`; `(y - pad - 1u) * width + x`; `(y + pad) * width + x`; `y * a->width + col`; `y * sc->words + blockIdx.x` | `uint32_t` | Element offset < plane N. The mask offset is <= (proc_h - 1) \* proc_w + proc_w - 1. Byte addresses come from typed-pointer adds in 64-bit. | <= 132,710,399 ($2^{26.98}$) | <= $2^{30} - 1$ | SAFE (< $2^{32}$; would wrap only above $2^{32}$ elements, and VMAF_PIC_DIM_MAX rules that out) |
| HIP hip/integer_cambi/cambi_hip_device.h:414,448,532,541,542,561,577,583,584,598,659 | `(size_t)y * width`, `(size_t)level * width`, `(size_t)chunk * levels * width + col` | `size_t` | Indices | < $2^{27}$ | < $2^{31}$ | SAFE |
| HIP hip/integer_cambi/cambi_hip_device.h:320,339-342 | `(v + rounding) >> shift`; `sum = here + 3 samples` | `uint32_t` | 4 samples, each <= 1024 | <= 4096 | same | SAFE |
| HIP hip/integer_cambi/cambi_hip_device.h:383-386,393-396 | Mask tile `sum += flags[..]` (stored as `uint8_t`); `sum += row_sums[..]` | `uint32_t` -> `uint8_t` | 7 flags of 0/1; then 7 row sums of <= 7 | 7 -> uint8; 49 | same | SAFE |
| HIP hip/integer_cambi_hip.c:344 | `s->enc_width * s->enc_height` | `int * int` | Encode pixel count | 132,710,400 | $2^{30}$ | SAFE (< INT_MAX) |
| HIP hip/integer_cambi_hip.c:484-489 | `n = width * height`; `(int)(in->topk * (double)(int)n)`; `(n + 4095u) / 4096u` | `unsigned`, `int` | Per-scale pixels | $2^{26.98}$ | $2^{30}$ | SAFE |
| HIP hip/integer_cambi_hip.c:420-433,480-483 | `compute_units * 512u`; `(target_items + width - 1u) / width`; `(size_t)width * levels * sizeof(uint16_t)`; chunk_rows / chunks | `unsigned` / `size_t` | Chunking | small | small | SAFE |
| HIP hip/integer_cambi_hip.c:513-516,547-582,650-661,692,837 | hist cells `(size_t)chunks * width` then `* levels`; arena_take cursor; `pixels`/`half`/`mask_words` (size_t); `src_bytes = (size_t)w * h * (1 or 2)`; `row_bytes`; malloc/hipMemcpy sizes | `size_t` | Arena bytes | ~2.1 GB | ~$2^{34.4}$ B | SAFE (size_t; capacity only) |
| HIP hip/integer_cambi_hip.c:774-775,814,819 (aside, grid dims) | `gy = ceil(h/16)`, `rows = ceil(H/8)`, `g->chunks` | `unsigned` | Grid y | <= 1080 | <= 4096 | SAFE |

#### Shared helpers and CPU reference (cambi.c, cambi.h, cambi_c_values_frame.h, cambi_internal.h)

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| shared cambi.c:373 (vmaf_cambi_adjust_window, both twins) | `(*window_size) * (input_width + input_height)` | `uint16_t` promoted, times `unsigned` -> `unsigned` | window_size <= 127 (option max) \* (W+H) | 127 \* 24000 = 3,048,000 -> window 509 | 127 \* 65536 = 8,323,072 -> 1387 (fits the uint16 store) | SAFE |
| shared cambi.c:1836 (vmaf_cambi_check_window_fits_lut, both twins) | `max_window * max_window` | `int` | max window <= 1387 | $509^{2}$ = 259,081 | $1387^{2}$ = 1,923,769 | SAFE (< $2^{31}$). This guard bounds every histogram count by 4225. |
| shared cambi.c:1135 (vmaf_cambi_mask_index) | `(input_width >> 6) * (input_height >> 6)` | `uint32_t` | Block count | 240 \* 135 = 32,400 | 512 \* 512 = 262,144 | SAFE |
| shared cambi.c:1521 (vmaf_cambi_get_pixels_in_window, both collect()) | `odd_length * odd_length` -> `uint16_t` return | `int` -> `uint16_t` | Window <= 65 (guard) | 4225 | 4225 | SAFE (guarded; an unguarded $1387^{2}$ would truncate in uint16, but cannot be reached) |
| CPU cambi.c:1163-1168 (compute_dp_row, summed-area table) | `prefix += deriv[j]`; `dp_curr[..] = dp_prev[..] + prefix` | `uint32_t` | Running count of 0/1 zero-derivative flags over all rows and columns so far (the DP rows are cyclic, but their values accumulate) | <= N = 132,710,400 ($2^{26.98}$) | <= $2^{30}$ | SAFE (< $2^{32}$; the mask uses modular differences anyway) |
| CPU cambi.c:1183 (compute_mask_row) | `dp_bottom[j+delta] + dp_top[j] - dp_bottom[j] - dp_top[j+delta]` | `uint32_t` (modular) | 7x7 box sum | <= 49 | 49 | SAFE (modular, and the true value is < $2^{32}$) |
| CPU cambi.c:418,425 | `arr[i]++` / `arr[i]--` (increment_range / decrement_range histograms) | `uint16_t` | Window count | <= 4225 | 4225 | SAFE |
| CPU cambi.c:1275,1277 | `diff_weights[d] * p_0 * p_1` (p are `uint16_t`, promoted) | `int` | Weight <= 9, p0 + p1 <= 4225 | <= $2^{27.3}$ | same | SAFE |
| CPU cambi.c:1512 | `int num_elements = height * width` | `unsigned` -> `int` | Pixels | $2^{26.98}$ | $2^{30}$ | SAFE (< INT_MAX) |
| CPU cambi.c:433-436 | `row * stride + col`, `(row + 1) * stride + col` (derivative row) | `int * int` (stride in samples) | Sample offset | 1.33e8 | 32767 \* 32768 + 32767 = $2^{30} - 1$ | SAFE |
| CPU cambi.c:967-984 | `int stride`; `(i + 1) * stride + j + 1` (anti_dithering_filter) | `int` | Sample offset | 1.33e8 | <= $2^{30}$ | SAFE |
| shared cambi.h:4284-4402 | `(i +/- pad_size [- 1]) * stride + j` with `ptrdiff_t stride`; `(ptrdiff_t)row * width` | `ptrdiff_t` | Indices | < $2^{27}$ | < $2^{31}$ | SAFE |
| shared cambi_c_values_frame.h:192,206-207,226-231 | `j0 + 32 * m + ctz`; `i + pad_size`; `(int)vlt_luma - 3 * num_diffs + 1`; memset `(size_t)w * (size_t)h`, `(size_t)w * v_band_size` | `int` / `size_t` | Indices and sizes | small / size_t | small / size_t | SAFE |

### PSNR-HVS

#### Bounds that apply to every PSNR-HVS row

- **Bit-depth guard.** Every implementation rejects bpc > 12: CUDA cuda/integer_psnr_hvs_cuda.c:107, HIP hip/integer_psnr_hvs_hip.c:401, CPU third_party/xiph/psnr_hvs.c:440. Samples are therefore <= 4095, and 16-bit input cannot reach the kernels.
- **DCT intermediates.** od_bin_fdct8 is applied to columns, then to rows. It is bounded with affine arithmetic over the 64 samples in [0, M]: the exact linear part, plus +/-0.5 for every rounding shift.
  - **12-bit:**
    - max |intermediate| = 32,795
    - max |t*K + r| = 379,769,216 ($2^{28.50}$; row pass, K = 11585)
    - max |AC coefficient| = 16,414, so $\mathrm{AC}^{2}$ = 2.69e8 ($2^{28.0}$)
    - max |coefficient| = 32,795
  - **16-bit (rejected input), for reference:** max |t*K + r| = 6.07e9 ($2^{32.5}$) and $\mathrm{AC}^{2}$ = 6.87e10 ($2^{36}$), so both would overflow int32. The products first exceed $2^{31}$ at 15-bit samples, which leaves 2.5 bits of margin above the guard.
- **Block counts.** Step 7, 8x8 blocks, so blocks per plane = ((W-8)/7+1) \* ((H-8)/7+1).
  - 16K: 2,707,396 per plane; 4:4:4 total over 3 planes 8,122,188.
  - Cap: 21,911,761 per plane; 4:4:4 total 65,735,283 ($2^{25.97}$).
- **No clip-level accumulators.** Per-frame float sums are converted to double scores.

#### CUDA (cuda/integer_psnr_hvs/psnr_hvs_score.cu, cuda/integer_psnr_hvs_cuda.c, cuda/integer_psnr_hvs_cuda.h)

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| CUDA cuda/integer_psnr_hvs/psnr_hvs_score.cu:144-162 | od_bin_fdct8 lifting products `(t * K + r) >> s`, K <= 21895 | `int` | One product per lifting step, column and row pass; 12-bit samples | <= 379,769,216 ($2^{28.50}$); per block, so independent of resolution | same | SAFE ($2^{28.5}$ < $2^{31}$; bpc <= 12 guard. 16-bit would reach $2^{32.5}$, but init rejects it.) |
| CUDA cuda/integer_psnr_hvs/psnr_hvs_score.cu:129-143,153-156 | Butterflies `t0 - t1`, `t4 + t5`, `t6 + t7`, `t0 += t6h` | `int` | Intermediates | <= 32,795 | same | SAFE |
| CUDA cuda/integer_psnr_hvs/psnr_hvs_score.cu:110-113 | od_dct_rshift `((unsigned)a >> (32 - b)) + (unsigned)a`, then `(int)` and `>> b` | `unsigned int` (deliberate modular wrap that adds the sign bit) | abs(a) <= 32,795 | Exact truncation toward zero (equals OD_UNBIASED_RSHIFT32) | same | SAFE (intentional wrap; the unsigned-to-int conversion is two's complement on nvcc) |
| CUDA cuda/integer_psnr_hvs/psnr_hvs_score.cu:311-312 | `coefficient * coefficient` (mask energy; DC skipped) | `int` | AC coefficient squared | <= 16,$414^{2}$ = 269,419,396 ($2^{28.0}$) | same | SAFE (12-bit guard; 16-bit would reach $2^{36}$, rejected) |
| CUDA cuda/integer_psnr_hvs/psnr_hvs_score.cu:339 | `abs(block[ref] - block[dist])` | `int` | Coefficient difference | <= 65,590 | same | SAFE |
| CUDA cuda/integer_psnr_hvs/psnr_hvs_score.cu:357-360 | `id = (size_t)blockIdx.x * blockDim.x + threadIdx.x`; `2U * (size_t)args.total_blocks`; `(unsigned)(id >> 1)` | `size_t` / `unsigned` | Work items = 2 \* total_blocks | 16,244,376 | 131,470,566 | SAFE |
| CUDA cuda/integer_psnr_hvs/psnr_hvs_score.cu:237-239,251-258 | `block - first_block`; `(size_t)(in_plane % blocks_x) * 7`; `(const char *)lane.src + y * lane.stride`; `[x]` | `unsigned` / `size_t` | Pitched byte offset, pitch from `(size_t)stride` (integer_psnr_hvs_cuda.c:324-325) | < $2^{29}$ | ~$2^{31}$ (size_t) | SAFE |
| CUDA cuda/integer_psnr_hvs/psnr_hvs_score.cu:361,381,498 | `(size_t)threadIdx.x * 65`; `args.terms + (size_t)lane.block * 64`; `raw_terms + (size_t)b * 64` | `size_t` | Term index <= 64 \* total_blocks | 5.2e8 | 4.2e9 | SAFE |
| CUDA cuda/integer_psnr_hvs/psnr_hvs_score.cu:387 | `(uint32_t)__popcll(mask)` -> block_counts | `uint32_t` | Non-zero terms in one block | <= 64 | 64 | SAFE |
| CUDA cuda/integer_psnr_hvs/psnr_hvs_score.cu:392-403,417-429,467-483 | warp_scan_inclusive `val += n`; block totals; `intra_offset = warp_prefix + warp_sum - val` | `uint32_t` | 256 block counts, each <= 64 | <= 16,384 | 16,384 | SAFE |
| CUDA cuda/integer_psnr_hvs/psnr_hvs_score.cu:411,461 | `b = (unsigned)blockIdx.x * 256u + tid` | `unsigned` | <= num_chunks \* 256 <= total_blocks + 255 | 8.1e6 | 6.6e7 | SAFE |
| CUDA cuda/integer_psnr_hvs/psnr_hvs_score.cu:442-450 | hvs_scan_prefix `running += chunk_totals[c]` -> `chunk_offsets[c]`, `header->total_terms` | `uint32_t` | Non-zero terms in the frame, <= 64 per block over every plane | 4:4:4: 8,122,188 \* 64 = 519,820,032 ($2^{28.95}$) | 65,735,283 \* 64 = 4,207,058,112 ($2^{31.97}$) | SAFE (< $2^{32}$ = 4,294,967,296; margin 2.0% at W=H=32768 4:4:4) |
| CUDA cuda/integer_psnr_hvs/psnr_hvs_score.cu:486,491,500-503 | `global_base = chunk_offsets[blockIdx.x] + intra_offset`; `header->plane_offsets[p]`; `dst[out_idx++]` | `uint32_t` | <= total_terms; out_idx <= 64 | $2^{28.95}$ | $2^{31.97}$ | SAFE |
| CUDA cuda/integer_psnr_hvs_cuda.c:163-167 | `num_blocks_x * num_blocks_y`; `total_blocks += num_blocks[plane]`; `first_block` | `unsigned` | Blocks | 2,707,396 / 8,122,188 | 21,911,761 / 65,735,283 | SAFE |
| CUDA cuda/integer_psnr_hvs_cuda.c:181-202,209 | `num_chunks = (total_blocks + 255u) / 256u`; `(size_t)total_blocks * 64 * sizeof(float)`; masks and counts sizes; hvs_terms_bytes | `unsigned` / `size_t` | Scratch and readback bytes | Raw terms 2.08 GB | 16.8 GB | SAFE (size_t; capacity only) |
| CUDA cuda/integer_psnr_hvs_cuda.c:369-370 | `items = 2U * (size_t)s->total_blocks`; `grid_x = (unsigned)((items + 63) / 64)` | `size_t` -> `unsigned` | Grid x (CUDA limit $2^{31} - 1$) | 253,819 | 2,054,228 | SAFE |
| CUDA cuda/integer_psnr_hvs_cuda.c:415,423-427,462 | `(size_t)first_block[p] * 64`; `end - start`; `compact_terms + start`; `(size_t)total_terms * sizeof(float)` | `size_t` / `uint32_t` | Host plane ranges | <= $2^{28.95}$ | <= $2^{31.97}$ | SAFE |

#### HIP (hip/integer_psnr_hvs/psnr_hvs_score.hip, hip/integer_psnr_hvs_hip.c, hip/integer_psnr_hvs_hip.h)

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| HIP hip/integer_psnr_hvs/psnr_hvs_score.hip:167-185 | od_bin_fdct8 `(t * K + r) >> s` | `int` | Same as CUDA | <= $2^{28.50}$ | same | SAFE (bpc <= 12 guard) |
| HIP hip/integer_psnr_hvs/psnr_hvs_score.hip:152-166 | Butterflies | `int` | Intermediates | <= 32,795 | same | SAFE |
| HIP hip/integer_psnr_hvs/psnr_hvs_score.hip:130-133 | od_dct_rshift unsigned add | `unsigned int` (intentional wrap) | abs(a) <= 32,795 | exact | same | SAFE |
| HIP hip/integer_psnr_hvs/psnr_hvs_score.hip:345 | `sq = blk[i*8+j] * blk[i*8+j]` | `int` | AC coefficient squared | <= $2^{28.0}$ | same | SAFE |
| HIP hip/integer_psnr_hvs/psnr_hvs_score.hip:372 | `abs(ref[index] - dist[index])` | `int` | Coefficient difference | <= 65,590 | same | SAFE |
| HIP hip/integer_psnr_hvs/psnr_hvs_score.hip:399-402,403,420,514 | `id = (size_t)blockIdx.x * 64 + local`; `2u * (size_t)total_blocks`; `(size_t)local * 65`; `(size_t)lane.block * 64`; `(size_t)b * 64` | `size_t` | Indices | 1.6e7 / 5.2e8 | 1.3e8 / 4.2e9 | SAFE |
| HIP hip/integer_psnr_hvs/psnr_hvs_score.hip:278-283,294 | `origin = origin_y * width + origin_x`; `lane.origin + (size_t)row * lane.width` | `size_t` | Element offset in the packed plane | < $2^{27}$ | < $2^{30}$ | SAFE |
| HIP hip/integer_psnr_hvs/psnr_hvs_score.hip:426 | `(uint32_t)__builtin_popcountll(mask)` | `uint32_t` | <= 64 | 64 | 64 | SAFE |
| HIP hip/integer_psnr_hvs/psnr_hvs_score.hip:437-450,487-497 | Hillis-Steele `s_data[tid] += n`; `intra_offset = s_data[tid] - count` | `uint32_t` (shared) | 256 counts, each <= 64 | <= 16,384 | 16,384 | SAFE |
| HIP hip/integer_psnr_hvs/psnr_hvs_score.hip:437,482 | `b = chunk * 256u + tid` | `unsigned` | <= total_blocks + 255 | 8.1e6 | 6.6e7 | SAFE |
| HIP hip/integer_psnr_hvs/psnr_hvs_score.hip:462-471 | hvs_scan_prefix_hip `running += chunk_totals[c]` for `c < limit`, where `limit = num_chunks < 32768u ? num_chunks : 32768u` -> `chunk_offsets[c]`, `header->total_terms` | `uint32_t` sum; `unsigned` loop cap 32,768 chunks | \*\*The sum is safe (<= $2^{31.97}$, as CUDA). The cap is not.\*\* num_chunks = ceil(total_blocks / 256), and only the first 32,768 chunks (8,388,608 blocks) get an offset. Chunks at or past 32,768 read whatever d_scratch held: hipMalloc garbage, or the previous frame's value. total_terms counts only the first 32,768 chunks. | 4:4:4: 31,728 chunks <= 32,768, every chunk visited (3.2% margin). 4:2:0: 15,864. | 4:4:4: 256,779 chunks; luma only: 85,593. Both > 32,768. | \*\*OVERFLOW@CAP-ONLY\*\* (a count cap rather than an arithmetic wrap). It fails once total_blocks > 8,388,608, e.g. 16384x8640 4:4:4 (8,662,680 blocks) or luma/4:0:0 above about 20.3K x 20.3K. hvs_compact_hip then writes `packed_terms + chunk_offsets[chunk] + intra` from unset offsets: out-of-bounds device writes, a wrong total_terms readback, and NaN or wrong plane scores. The CUDA twin (psnr_hvs_score.cu:443) loops over every chunk and is not affected. |
| HIP hip/integer_psnr_hvs/psnr_hvs_score.hip:498-519 | `global_base = chunk_offsets[chunk] + intra_offset`; `plane_offsets[p] = global_base`; `dst[out_idx++]` | `uint32_t` | <= total_terms while the row above holds | $2^{28.95}$ | $2^{31.97}$ (but see the row above) | SAFE (inside the 32,768-chunk cap) |
| HIP hip/integer_psnr_hvs_hip.c:389-394 | `num_blocks_x * num_blocks_y`; `total_blocks += num_blocks[p]`; `row_bytes = (size_t)width * bps` | `unsigned` / `size_t` | Blocks | 8,122,188 | 65,735,283 | SAFE |
| HIP hip/integer_psnr_hvs_hip.c:173-194,256,278,492 | `num_chunks`; `(size_t)total_blocks * 64 * sizeof(float)`; `row_bytes * height` | `unsigned` / `size_t` | Buffer bytes | 2.08 GB | 16.8 GB | SAFE (capacity only) |
| HIP hip/integer_psnr_hvs_hip.c:482,486 | `memcpy(out, src, row_bytes * height)`; `(size_t)row * row_bytes`, `(size_t)row * stride` | `size_t` | Staging offsets | < $2^{28}$ | ~$2^{31}$ | SAFE |
| HIP hip/integer_psnr_hvs_hip.c:566-567 | `items = 2u * (size_t)total_blocks`; `groups = (unsigned)((items + 63) / 64)` | `size_t` -> `unsigned` | HIP grid (gx \* bx < $2^{32}$) | 253,819 | 2,054,228 (1.3e8 threads) | SAFE |
| HIP hip/integer_psnr_hvs_hip.c:636,643-647,684 | `(size_t)first_block * 64`; `end - start` (`uint32_t`); `(size_t)total_terms * sizeof(float)` | `size_t` / `uint32_t` | Host plane ranges. If the scan-prefix cap row has fired, `end - start` can underflow; vmaf_psnr_hvs_plane_score_compacted then returns NaN because n_compact > n_terms. | <= $2^{28.95}$ | <= $2^{31.97}$ | SAFE (inside the cap) |

#### Shared host tail (psnr_hvs_score.c, the implementation behind psnr_hvs_score.h; both twins call it)

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| shared psnr_hvs_score.c:34-37,51 and :64-67,78 | Guard `n_blocks > INT_MAX / 64`; `n_terms = n_blocks * 64`; `int pixels = (int)n_terms` | `size_t` -> `int` | Terms per plane | 173,273,344 | 1,402,352,704 (luma $32768^{2}$) | SAFE (< INT_MAX; the guard returns NaN above 33,554,431 blocks, which cannot be reached: max 21,911,761) |
| shared psnr_hvs_score.c:53-54 and :80-81 | `samplemax * samplemax` | `int32_t` | bpc <= 12 | $4095^{2}$ = 16,769,025 | same | SAFE (16-bit would be $65535^{2}$, an int32 overflow, but bpc > 12 returns NaN first) |

#### Aside: CPU reference outside the G5 file list; the owning group should confirm

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| CPU third_party/xiph/psnr_hvs.c:311-317 | `(y + i) * _systride + (j + x) * 2 + 1` | `int * int` (byte stride) | Byte offset in a > 8-bit plane. The stride comes from picture.c:157-160 (64-sample alignment, so 65,536 B at W=32768). y+i <= H-1 = 32767. | 8639 \* 30720 + 30719 = 265,420,799 | 32767 \* 65536 + 65535 = 2,147,483,647 = INT_MAX exactly | SAFE (zero margin: a padded stride above 65,536 B at H=32768, e.g. a non-vmaf_picture_alloc buffer, would wrap the int) |

### Coverage

| file | rows |
|---|---|
| cuda/integer_cambi/cambi_score.cu | 22 |
| cuda/integer_cambi_cuda.c | 5 (+ :709 inside one cambi_score.cu row) |
| cuda/integer_cambi_cuda.h | none (struct layout only: uint32 dims, uint64 pointers, 2x uint64 result words; the types are covered by the kernel rows) |
| hip/integer_cambi/cambi_hip_device.h | 11 (+ 2 shared with cambi_score.hip rows) |
| hip/integer_cambi/cambi_score.hip | 6 |
| hip/integer_cambi_hip.c | 5 |
| hip/integer_cambi_hip.h | none (CambiHipPlanInput declarations only) |
| cuda/integer_psnr_hvs/psnr_hvs_score.cu | 13 |
| cuda/integer_psnr_hvs_cuda.c | 4 |
| cuda/integer_psnr_hvs_cuda.h | none (PsnrHvsHeader uint32 offsets and total_terms; covered by the scan rows) |
| hip/integer_psnr_hvs/psnr_hvs_score.hip | 12 |
| hip/integer_psnr_hvs_hip.c | 5 |
| hip/integer_psnr_hvs_hip.h | none (PsnrHvsHipHeader uint32 offsets and total_terms; covered by the scan rows) |
| cambi_internal.h | none (declarations; CAMBI_MIN_WIDTH_HEIGHT / threshold macros are compared, never multiplied in an accumulator) |
| cambi_c_values_frame.h | 1 |
| cambi.h | 1 (index math in the update_histogram_\* helpers; the rest is the 4226-entry reciprocal LUT) |
| psnr_hvs_score.h | none (declarations); its implementation psnr_hvs_score.c: 2 |
| cambi.c (only the parts named in the brief: window, mask index, SAT, histograms, c_value_pixel, pooling, derivative, anti-dither, shared exports) | 11 |
| cuda/cuda_helper.cuh:125-133 (warp_reduce, read for the cambi_block_sum bound) | 0 own rows (folded into the CUDA cambi block-sum rows) |
| third_party/xiph/psnr_hvs.c (lines 290-330 and 440 only, for the bpc guard and the CPU index aside) | 1 (aside) |

Verdict totals (99 rows): SAFE 98, OVERFLOW@16K 0, OVERFLOW@CAP-ONLY 1 (HIP psnr_hvs_score.hip:462-471, the 32,768-chunk scan-prefix cap), DEPENDS 0.

## G6 SpEED (speed_chroma, speed_temporal): CUDA + HIP integer-overflow audit

Repo: master at 571565a47. Read only. Paths are relative to `core/src/feature/`.

### Geometry derivation (applies to every row)

SpEED does not run on the full plane. `speed_internal_init_dimensions()` (`speed_internal.c:65-96`) computes the operating geometry, and `si_gpu_fill_geometry()` (`speed_internal.c:833-858`) narrows it to `uint32_t` in `SpeedGpuGeometry`:

- `scaled = lround(src * speed_prescale)`. The option `speed_prescale` is in [0.1, 4.0] on all four extractors (`cuda/speed_chroma_cuda.c:87-88`, `cuda/speed_temporal_cuda.c:88-89`, `hip/speed_chroma_hip.c:89-90`, `hip/speed_temporal_hip.c:92-93`). \*\*The scaled plane can therefore be up to 16x the picture plane.\*\*
- `down = scaled >> 4`, `trunc = down / 5 * 5`, `blocks = (trunc_w/5)*(trunc_h/5)`, `sub = trunc - 4`.
- chroma 4:4:4 uses the luma size (`speed_chroma_dimensions()`, `speed_internal.h:100`). The temporal extractor uses luma.

Every bound below was derived at two prescales: p=1 (default) and p=4 (option max, the worst case). Values come from the formulas above, computed in python.

| point | scaled WxH | down | trunc | blocks | sub_w*sub_h |
|---|---|---|---|---|---|
| 16K p=1 | 15360x8640 | 960x540 | 960x540 | 20,736 | 956*536 = 512,416 |
| 16K p=4 | 61440x34560 | 3840x2160 | 3840x2160 | 331,776 | 3836*2156 = 8,270,416 |
| cap p=1 | 32768x32768 | 2048x2048 | 2045x2045 | 167,281 | $2041^{2}$ = 4,165,681 |
| cap p=4 | 131072x131072 | 8192x8192 | 8190x8190 | 2,683,044 | $8186^{2}$ = 67,010,596 |
| $32740^{2}$ p=4 (inside cap) | $130960^{2}$ | $8185^{2}$ | $8185^{2}$ | 2,679,769 | $8181^{2}$ = 66,928,761 (odd) |

Channels: chroma 4, temporal 2. Raw plane bytes: 16K, 16-bit: 265,420,800; at the cap, $32768^{2}$*2 = $2^{31}$.

There are no integer pixel accumulators in SpEED. Every sum over pixels (means, covariance, filters, solve, variance) is fp32 or an fp32 pair. There are no atomics, `__shfl`, warp/wave integer reductions or histograms in any G6 file. The integer surface is sizes, indices, grid dims, the uint32 tail layout, int-to-float conversions of counts, float-to-int coordinate conversions and the uint64 singular tally.

### speed_chroma + speed_temporal: shared host geometry / tail (speed_internal.h, speed_internal.c, speed_gpu_common.h)

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| speed_internal.h:65-66 | `(unsigned)lround((double)w * prescale)` | `long` -> `unsigned` | w <= 32768, prescale <= 4 | 61,440 | 131,072 | SAFE ($2^{17}$ < $2^{32}$) |
| speed_internal.c:84 | `num_blocks = num_blocks_horizontal * num_blocks_vertical` | `size_t` | (trunc_w/5)*(trunc_h/5) | 331,776 (p4) | 2,683,044 (p4) | SAFE ($2^{21.4}$ < $2^{64}$) |
| speed_internal.c:837-848 | `g->src_w/.../blocks/sub_h = (uint32_t)dim->...` narrowing | `size_t` -> `uint32_t` | largest field: scaled_w 131,072; blocks 2,683,044 | 61,440 / 331,776 | 131,072 / 2,683,044 | SAFE ($2^{21.4}$ < $2^{32}$) |
| speed_gpu_common.h:123-125 | `speed_gpu_tail_layout()`: `eig = ch*2*4`, `var = eig + ch*25*4`, `bytes = var + channels * blocks * 4` | `uint32_t` | ch=4: 8ch + 100ch + 16*blocks | 5,308,848 (p4) | 42,929,136 (p4) | SAFE ($2^{25.4}$ < $2^{32}$) |
| speed_internal.c:909 | lanczos count `(size_t)9 * ((size_t)scaled_w + (size_t)scaled_h)` | `size_t` | 9 per scaled column + row | 864,000 | 2,359,296 | SAFE |
| speed_internal.c:916-918 | `(int)g->src_w`, `(int)g->scaled_w`; `weights + (size_t)9 * g->scaled_w` (vif_tools.c:681 indexes with `ptrdiff_t`) | `uint32_t` -> `int`; `size_t` | dims <= 131,072 | 61,440 | 131,072 | SAFE ($2^{17}$ < $2^{31}$) |
| speed_internal.c:1058, 1071-1072, 1080-1081 | `num_blocks`, `status[(size_t)ref*2u]`, `var + (size_t)ref * num_blocks` | `size_t` | ch*blocks | 1,327,104 | 10,732,176 | SAFE |
| speed_internal.c:953 | `num_blocks * sizeof(float)` memset | `size_t` | blocks*4 | 1,327,104 B | 10,732,176 B | SAFE |
| speed_internal.c:1038 | `score / num_blocks` (int -> float conversion of the divisor) | `size_t` -> `float` | blocks | 331,776 | 2,683,044 | SAFE (< $2^{24}$, exact; the CPU get_speed_score uses the same form) |
| speed_internal.c:756, 760 | `tally->solves++`, `tally->singular++` (clip-level) | `uint64_t` | +4 solves/frame (chroma), +2 (temporal), independent of resolution | 1080p/8K/16K: $2^{62}$ frames to overflow (chroma) | same | SAFE ($2^{64}$ / 4 frames) |
| speed_matmul.h:123 (+ speed.c:225/229 scalar impl) | `int` strides/rows/cols signature; CPU $Q^{T}$*B passes cols = num_blocks | `int`, widened to `size_t` before products | i < 25, k < 25, j < num_blocks | 331,776 | 2,683,044 | SAFE ($2^{21.4}$ < $2^{31}$; products in size_t) |

### speed_chroma + speed_temporal: CUDA pipeline host (cuda/speed_cuda_pipeline.c)

`ch` is `size_t` (line 138/399/415/435), so every `ch * a * b` below is computed in `size_t` from its first operand.

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| cuda/speed_cuda_pipeline.c:283 | `plane_bytes = (size_t)src_w * src_h * bytes_per_sample` | `size_t` | N*2 | 265,420,800 | $2^{31}$ | SAFE (size_t; travels as `uint64_t` in SpeedCudaFrameArgs) |
| cuda/speed_cuda_pipeline.c:142 | `plane_bytes * raw_planes` | `size_t` | 4 planes | 1,061,683,200 | $2^{33}$ | SAFE |
| cuda/speed_cuda_pipeline.c:148 | `ch * scaled_w * scaled_h * f` (SCALED buffer) | `size_t` | 4 ch \* scaled N \* 4 B | 33,973,862,400 B (p4) | $2^{38}$ B (p4) | SAFE (size_t). Aside: a buffer this large fails cuMemAlloc and returns an errno; no wrap |
| cuda/speed_cuda_pipeline.c:150, 152, 154 | `ch*down_w*down_h*f`, `ch*trunc_w*trunc_h*f`, `ch*25*blocks*f` | `size_t` | down/trunc plane, 25*blocks | INDTERM 132,710,400 B (p4) | 1,073,217,600 B (p4) | SAFE |
| cuda/speed_cuda_pipeline.c:264 | `(size_t)2u * blocks * sizeof(float)` host entropy | `size_t` | 2*blocks*4 | 2,654,208 | 21,464,352 | SAFE |
| cuda/speed_cuda_pipeline.c:284 | `g->sub_w * g->sub_h` -> speed_covariance_threads | `uint32_t` | sub product | 8,270,416 (p4) | 67,010,596 (p4) | SAFE ($2^{26}$ < $2^{32}$) |
| cuda/speed_cuda_pipeline.c:129 | `terms / 8u` loop bound; `threads *= 2` capped at 256 | `uint32_t` | - | 256 | 256 | SAFE |
| cuda/speed_cuda_pipeline.c:331, 338 | `row_bytes = (size_t)src_w * bps`; `dstDevice + (CUdeviceptr)(plane_bytes * slot)` | `size_t` | slot <= 3 | 796,262,400 | 3*$2^{31}$ | SAFE |
| cuda/speed_cuda_pipeline.c:381 | `speed_grid`: `(unsigned)((items + threads - 1u) / threads)` | `size_t` -> `unsigned` (gridDim.x) | largest: scale kernel 4*scaled N / 256 | 33,177,600 (p4) | $2^{28}$ = 268,435,456 (p4) | SAFE (< $2^{31} - 1$ gridDim.x limit; 1-D launches only, no y/z) |
| cuda/speed_cuda_pipeline.c:400, 403, 416, 436 | `ch * down_w * down_h`, `ch * scaled_w * scaled_h`, `ch * trunc_w * trunc_h`, `ch * blocks` | `size_t` | item counts | 8,493,465,600 (scaled, p4) | $2^{36}$ (p4) | SAFE |
| cuda/speed_cuda_pipeline.c:424, 428 | `(unsigned)(ch * 325)`, `(unsigned)ch` | `size_t` -> `unsigned` | 4*325 | 1,300 | 1,300 | SAFE |
| cuda/speed_cuda_pipeline.c:367-369 | `dptr(TAIL) + p->tail.status/eig/var` | `CUdeviceptr` + `uint32_t` | offsets <= tail.bytes | 5,308,848 | 42,929,136 | SAFE |

### speed_chroma + speed_temporal: CUDA kernels (cuda/speed/speed_score.cu)

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| cuda/speed/speed_score.cu:244 | RawSource `offset = (size_t)row * width_ + col` | `size_t` | < N samples | 132,710,399 | $2^{30} - 1$ | SAFE |
| cuda/speed/speed_score.cu:256 | `(size_t)index * plane_bytes_` | `size_t`\*`uint64_t` | index <= 3 | 796,262,400 | 3*$2^{31}$ | SAFE |
| cuda/speed/speed_score.cu:277 | FloatSource `((size_t)channel * height_ + row) * width_ + col` | `size_t` | < ch*scaled N | 8,493,465,599 (p4) | $2^{36} - 1$ (p4) | SAFE |
| cuda/speed/speed_score.cu:193-198 | reflect101 `2 * n - folded - 2`, `n = (int32_t)size` | `int32_t` | n <= 131,072, folded <= n+64 | 2*61,440 | 2*131,072 | SAFE ($2^{18}$ < $2^{31}$) |
| cuda/speed/speed_score.cu:206, 213 | `centre + radius < (int32_t)size`; `centre - radius + (int32_t)k` | `int32_t` | radius <= 64 (taps <= 128) | 61,504 | 131,136 | SAFE |
| cuda/speed/speed_score.cu:455-456 | `(int32_t)(i * kDecimation)`, `(int32_t)(j * kDecimation)` | `uint32_t` product -> `int32_t` | i < down_h, \*16 | 61,424 | 131,056 | SAFE |
| cuda/speed/speed_score.cu:294, 425-426 | `(float)index`, `(float)g.src_w / (float)g.scaled_w` (int -> float) | `uint32_t` -> `float` | dims <= 131,072 | 61,440 | 131,072 | SAFE (< $2^{24}$, exact) |
| cuda/speed/speed_score.cu:428-429 | nearest: `(uint32_t)(y_f * ratio_y)` (float -> int) | `float` -> `uint32_t` | >= 0 and < src dim | <= 15,359 | <= 32,767 | SAFE (in range, no UB) |
| cuda/speed/speed_score.cu:317-320, 356-357, 371-374, 395-396, 405-408 | bilinear/bicubic/lanczos `(int32_t)floorf/ceilf/mirror(...)` coords (float -> int) | `float` -> `int32_t` | coords in [-4.5, src+4], mirrored | abs <= 15,364 | abs <= 32,772 | SAFE (in range) |
| cuda/speed/speed_score.cu:437-438 | `(size_t)9 * x`; `(size_t)9 * (g.scaled_w + y)` (inner sum in uint32) | `uint32_t` sum, then `size_t` | scaled_w + y < 2*scaled | 122,879 | 262,143 | SAFE |
| cuda/speed/speed_score.cu:492 | `(size_t)r * g.down_w + c` | `size_t` | < down N | 8,294,399 | 67,108,863 | SAFE |
| cuda/speed/speed_score.cu:511 | add_row `j + kMeanChunk <= width` | `uint32_t` | width = sub_w | 3,852 | 8,202 | SAFE |
| cuda/speed/speed_score.cu:574 | covariance `total = g.sub_w * g.sub_h` | `uint32_t` | sub product | 8,270,416 (p4) | 67,010,596 (p4) | SAFE ($2^{26}$ < $2^{32}$) |
| cuda/speed/speed_score.cu:576-578 | `pos += threads` (threads <= 256), `pos / sub_w`, `pos % sub_w` | `uint32_t` | < total + 256 | 8,270,672 | 67,010,852 | SAFE |
| cuda/speed/speed_score.cu:579-580 | `(size_t)(xr + row) * trunc_w + xc + col` | `size_t` | < trunc N | 8,294,399 | 67,076,099 | SAFE |
| cuda/speed/speed_score.cu:1079-1086 | scale kernel: `plane = (size_t)scaled_w*scaled_h`; `idx = (size_t)blockIdx.x * blockDim.x + threadIdx.x`; `plane * a.channels`; `(uint32_t)(rest / scaled_w)` | `size_t`; narrowed coords `uint32_t` | idx < ch*scaled N | 8,493,465,600 (p4) | $2^{36}$ (p4) | SAFE (widened before the multiply; coords < 131,072) |
| cuda/speed/speed_score.cu:1101-1108, 1125-1132 | decimate raw/scaled: same `size_t` idx scheme over ch*down N | `size_t` | idx < ch*down N | 33,177,600 | $2^{28}$ | SAFE |
| cuda/speed/speed_score.cu:1145-1152, 1154, 1157 | centre: `size_t` idx over ch*trunc N; `(size_t)ch * down_h * down_w`; `(size_t)i * down_w + j` | `size_t` | ch*trunc N | 33,177,600 | 268,304,400 | SAFE |
| cuda/speed/speed_score.cu:1159-1160 | `element = (i%5)*5 + j%5`; `tile = (i/5) * g.blocks_h + (j/5)` | `uint32_t` | tile < blocks | 331,775 | 2,683,043 | SAFE |
| cuda/speed/speed_score.cu:1161-1163 | `term_size = (size_t)25 * blocks`; `ch * term_size + (size_t)element * blocks + tile` (ch uint32 -> size_t) | `size_t` | < ch*25*blocks | 33,177,600 | 268,304,400 | SAFE |
| cuda/speed/speed_score.cu:1171-1175 | means `idx = blockIdx.x*blockDim.x + threadIdx.x`, `a.channels * kN` | `uint32_t` | 1 block of 128 | 127 | 127 | SAFE |
| cuda/speed/speed_score.cu:1177, 1182 | `(size_t)ch * trunc_h * trunc_w`; `(size_t)(row0 + i) * trunc_w + col0` | `size_t` | < ch*trunc N | 33,177,600 | 268,304,400 | SAFE |
| cuda/speed/speed_score.cu:1183 | means divisor `static_cast<float>(g.sub_w * g.sub_h)` | `uint32_t` product -> `float` | sub product | 8,270,416 (exact) | 67,010,596 (rounded above $2^{24}$) | SAFE (no wrap; the CPU `compute_mean` at speed.c:775 is `float / size_t`, which also converts the count to float, so the rounding is identical) |
| cuda/speed/speed_score.cu:1219-1220 | covariance divisor `static_cast<float>(g.sub_w * g.sub_h)` passed to `ff_div_to_float` | `uint32_t` product -> `float` | sub product, must be exact | p4 max 8,270,416 < $2^{24}$: exact | p > ~2.0 gives count > $2^{24}$. $32740^{2}$ at p4: 66,928,761 (odd) is not an fp32 value | OVERFLOW@CAP-ONLY (fp32 exact-integer range, not UB). The CPU `compute_covariance_row` (speed.c:847) divides the double sum by `size_t -> double` (exact). The twin divides by the rounded float, so the covariance can differ from the CPU and the twin is no longer bit-exact. At default p=1 the cap max is 4,165,681: exact |
| cuda/speed/speed_score.cu:1195-1199, 1222-1223 | covariance `ch = blockIdx.x / 325`, triangle entry, `x * kN + y` | `uint32_t` | < 1,300; < 625 | 1,299 | 1,299 | SAFE |
| cuda/speed/speed_score.cu:1262-1263 | solve `idx = blockIdx.x * blockDim.x + threadIdx.x`; `a.channels * g.blocks` | `uint32_t` | ch*blocks, grid rounded to 128 | 1,327,104 (p4) | 10,732,288 (p4) | SAFE ($2^{23.4}$ < $2^{32}$) |
| cuda/speed/speed_score.cu:1265-1268, 1056, 1273-1274, 1280, 1283 | `(size_t)ch * kN * blocks + block`; `b[(size_t)k * stride]`; `(size_t)e * blocks`; `var[idx]` | `size_t` / `uint32_t` idx | < ch*25*blocks | 33,177,600 | 268,304,400 | SAFE |
| cuda/speed/speed_score.cu:643-1064, 1027-1036, 1237-1246 (25x25 linalg: column_norm, householder, tridiagonal_step `span*span`, block_matmul, qr_minor/reflector, pivot_singular, linalg_store `(size_t)ch*625`, `(size_t)ch*2`) | small-matrix index math | `uint32_t` / `size_t` | indices <= 4*625 | 2,500 | 2,500 | SAFE |
| cuda/speed/speed_score.cu:882-897 | QR iteration counters `guard < 525`, `iter < 500`, `b`, `a`, `n_block` | `uint32_t` | bounded by caps | 525 | 525 | SAFE |

### speed_chroma + speed_temporal: HIP pipeline host (hip/speed_hip_pipeline.c)

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| hip/speed_hip_pipeline.c:49 | `(size_t)src_w * src_h * bytes_per_sample` | `size_t` | N*2 | 265,420,800 | $2^{31}$ | SAFE |
| hip/speed_hip_pipeline.c:64 | `q->plane_bytes = (uint32_t)speed_hip_plane_bytes(g)` (device param `SpeedHipParams.plane_bytes`, speed_hip_device.h:126) | `size_t` -> `uint32_t` | N*2 | 265,420,800 | $2^{31}$ = 2,147,483,648 | SAFE ($2^{31}$ <= UINT32_MAX; 1-bit margin. The device widens before multiplying: `(size_t)b.minuend * p->plane_bytes`, speed_hip_device.h:286/290) |
| hip/speed_hip_pipeline.c:68 | `g->sub_w * g->sub_h` -> covariance_group_size | `uint32_t` | sub product | 8,270,416 | 67,010,596 | SAFE |
| hip/speed_hip_pipeline.c:225 | `speed_hip_take`: `(bytes + 255) / 256 * 256`, `*cursor +=` | `size_t` | arena cursor | ~3.5e10 (p4) | ~$2^{38}$ (p4) | SAFE (size_t). Aside: hipMalloc of that size fails and returns an errno |
| hip/speed_hip_pipeline.c:252, 256, 260-266, 268 | `ch * scaled_w * scaled_h * f` (ch size_t), `plane_bytes * raw_planes`, `ch*down*f`, `ch*trunc*f`, `ch*25*blocks*f`, `a.total` | `size_t` | buffer sizes | 33,973,862,400 B (scaled p4) | $2^{38}$ B (p4) | SAFE |
| hip/speed_hip_pipeline.c:299-301 | `a->tail + p->tail.status/eig/var` | `size_t` + `uint32_t` | offsets | arena offset + 5.3e6 | + 4.3e7 | SAFE |
| hip/speed_hip_pipeline.c:351 | `staging_bytes = plane_bytes * staged` (pinned host) | `size_t` | 4 planes (chroma) | 1,061,683,200 | $2^{33}$ | SAFE |
| hip/speed_hip_pipeline.c:362 | `(size_t)2u * blocks * sizeof(float)` | `size_t` | 2*blocks*4 | 2,654,208 | 21,464,352 | SAFE |
| hip/speed_hip_pipeline.c:443-444 | `first + count > raw_planes` | `uint32_t` | <= 4 + 4 | 8 | 8 | SAFE |
| hip/speed_hip_pipeline.c:447, 454, 459 | `row_bytes = (size_t)src_w * bps`; `d_raw + (size_t)(first + i) * plane_bytes`; `rows = src_h` (VmafHipPlaneUpload `size_t` fields, hip/picture_hip.h:70-77) | `size_t` | slot <= 3 | 796,262,400 | 3*$2^{31}$ | SAFE |
| hip/speed_hip_pipeline.c:479 | grid `(w + t - 1u) / t`, `(h + t - 1u) / t`, `gz = channels` | `unsigned` | w,h <= scaled dims, t = 16 | gx 3,840 / gy 2,160 (p4) | 8,192 (p4) | SAFE (gy 8,192 < 65,535; gz <= 4) |
| hip/speed_hip_pipeline.c:497, 501, 505 | means grid `(ch*25 + 127)/128`, `ch * 325`, `ch` | `unsigned` | small | 1,300 | 1,300 | SAFE |
| hip/speed_hip_pipeline.c:507 | solve grid `(g->blocks + items - 1u) / items`, gy = ch | `uint32_t` | blocks + 127 | 2,592 (p4) | 20,962 (p4) | SAFE |
| hip/speed_hip_pipeline.c:86-87 | temporal bindings `(int32_t)(2u * set)`, `(int32_t)(2u * ((set + 1u) % 2))` | `uint32_t` -> `int32_t` | set <= 1 | 2 | 2 | SAFE |

### speed_chroma + speed_temporal: HIP device math and kernels (hip/speed/speed_hip_device.h, hip/speed/speed_pipeline.hip)

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| hip/speed/speed_hip_device.h:241-249 | reflect101 `2 * n - folded - 2` | `int32_t` | n <= 131,072 | 2*61,440 | 2*131,072 | SAFE |
| hip/speed/speed_hip_device.h:256, 263 | `centre + radius < (int32_t)size`; `centre - radius + (int32_t)k` | `int32_t` | radius <= 64 | 61,504 | 131,136 | SAFE |
| hip/speed/speed_hip_device.h:284 | `offset = (size_t)row * src_w + col` | `size_t` | < N | 132,710,399 | $2^{30} - 1$ | SAFE |
| hip/speed/speed_hip_device.h:286, 290 | `p->raw + (size_t)b.minuend * p->plane_bytes` (uint32 field widened first) | `size_t` | index <= 3 | 796,262,400 | 3*$2^{31}$ | SAFE |
| hip/speed/speed_hip_device.h:302 | `((size_t)ch * scaled_h + row) * scaled_w + col` | `size_t` | < ch*scaled N | 8,493,465,599 | $2^{36} - 1$ | SAFE |
| hip/speed/speed_hip_device.h:313, 444-445 | `(float)index`, `(float)src_w / (float)scaled_w` | `uint32_t` -> `float` | <= 131,072 | 61,440 | 131,072 | SAFE (exact) |
| hip/speed/speed_hip_device.h:447-448 | nearest `(uint32_t)((float)y * ratio_y)` (float -> int) | `float` -> `uint32_t` | < src dim | 15,359 | 32,767 | SAFE |
| hip/speed/speed_hip_device.h:337-340, 381-382, 396-397, 418-419, 428-429 | `(int32_t)` of floorf/ceilf/mirror coords (float -> int) | `float` -> `int32_t` | in [-4.5, src+4] | abs <= 15,364 | abs <= 32,772 | SAFE |
| hip/speed/speed_hip_device.h:456-457 | `(size_t)9 * x`, `(size_t)9 * ((size_t)scaled_w + y)` | `size_t` | < 9*2*scaled | 1,105,911 | 2,359,287 | SAFE |
| hip/speed/speed_hip_device.h:476-477 | `(int32_t)(i * 16)`, `(int32_t)(j * 16)` | `uint32_t` -> `int32_t` | i < down | 61,424 | 131,056 | SAFE |
| hip/speed/speed_hip_device.h:510 | `(size_t)r * down_w + c` | `size_t` | < down N | 8,294,399 | 67,108,863 | SAFE |
| hip/speed/speed_hip_device.h:524-528 | `(size_t)ch * down_h * down_w`; `plane_size = (size_t)trunc_h * trunc_w`; `ch * plane_size + (size_t)i * trunc_w + j` (ch uint32 -> size_t) | `size_t` | < ch*trunc N | 33,177,600 | 268,304,400 | SAFE |
| hip/speed/speed_hip_device.h:529-530 | `element`, `tile = (i/5) * blocks_h + (j/5)` | `uint32_t` | < blocks | 331,775 | 2,683,043 | SAFE |
| hip/speed/speed_hip_device.h:531-532 | `term_size = (size_t)25 * blocks`; `ch * term_size + (size_t)element * blocks + tile` | `size_t` | < ch*25*blocks | 33,177,600 | 268,304,400 | SAFE |
| hip/speed/speed_hip_device.h:544, 549 | `(size_t)ch * trunc_h * trunc_w`; `(size_t)(row0 + i) * trunc_w + col0` | `size_t` | < ch*trunc N | 33,177,600 | 268,304,400 | SAFE |
| hip/speed/speed_hip_device.h:553 | means divisor `(float)(g->sub_w * g->sub_h)` | `uint32_t` product -> `float` | sub product | 8,270,416 | 67,010,596 | SAFE (same float conversion as the CPU, speed.c:775) |
| hip/speed/speed_hip_device.h:601-602 | `ch * 25 + x` | `uint32_t` | < 100 | 99 | 99 | SAFE |
| hip/speed/speed_hip_device.h:607-613 | `total = sub_w * sub_h`; `pos += group`; `(size_t)(xr + row) * trunc_w + xc + col` | `uint32_t` / `size_t` | sub product (+256) | 8,270,672 | 67,010,852 | SAFE ($2^{26}$ < $2^{32}$) |
| hip/speed/speed_hip_device.h:624-625 | covariance divisor `(float)(p->geometry.sub_w * p->geometry.sub_h)` passed to `speed_hd_ff_div_to_float` | `uint32_t` product -> `float` | sub product, must be exact | p4 max 8,270,416 < $2^{24}$: exact | $32740^{2}$ at p4: 66,928,761 is not an fp32 value | OVERFLOW@CAP-ONLY (fp32 exact-integer range, not UB). Same defect as CUDA :1219: the CPU speed.c:847 divides by the exact double count, so bit parity breaks for p > ~2.0 beyond 16K |
| hip/speed/speed_hip_device.h:632-637 | `group < terms / 8u`; `group *= 2u` (<= 256) | `uint32_t` | - | 256 | 256 | SAFE |
| hip/speed/speed_hip_device.h:666-1171 (25x25 linalg: slm_layout `(size_t)k*640`, column_norm, householder, tridiagonal `span*span`, group_matmul, qr_minor/reflector, pivot, linalg_store `ch*625 + idx`, `(size_t)ch*2`, `(size_t)ch*25`) | small-matrix index math | `uint32_t` / `size_t` | <= 4*625 | 2,500 | 2,500 | SAFE |
| hip/speed/speed_hip_device.h:982-993 | QR iteration counters (cap 500) | `uint32_t` | bounded | 525 | 525 | SAFE |
| hip/speed/speed_hip_device.h:1185, 1204-1220 | `b[(size_t)k * stride]`; `(size_t)ch * 25 * blocks + block`; `(size_t)e * blocks`; `var[(size_t)ch * blocks + block]` | `size_t` | < ch*25*blocks | 33,177,600 | 268,304,400 | SAFE |
| hip/speed/speed_pipeline.hip:34, 39 | `speed_gx/gy = blockIdx * blockDim + threadIdx` | `uint32_t` | < ceil(scaled/16)*16 | 61,440 | 131,072 | SAFE |
| hip/speed/speed_pipeline.hip:52, 64 | `((size_t)ch * h + y) * w + x` (scaled, down) | `size_t` | < ch*scaled N | 8,493,465,599 | $2^{36} - 1$ | SAFE |
| hip/speed/speed_pipeline.hip:83-88 | means `item`, `p->channels * 25`, `ch * 25 + element` | `uint32_t` | < 128 | 127 | 127 | SAFE |
| hip/speed/speed_pipeline.hip:100-103 | `blockIdx.x / 325`, `% 325` | `uint32_t` | < 1,300 | 1,299 | 1,299 | SAFE |
| hip/speed/speed_pipeline.hip:138 | solve `block = blockIdx.x * blockDim.x + threadIdx.x` (ch = blockIdx.y) | `uint32_t` | < blocks + 128 | 331,903 | 2,683,171 | SAFE |

### speed_chroma: extractor files

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| cuda/speed_chroma_cuda.c:179 (-> speed_cuda_pipeline.c:275 `(int)width`) | chroma_w/h `unsigned` -> `int` | `unsigned` -> `int` | <= 32,768 | 15,360 | 32,768 | SAFE |
| cuda/speed_chroma_cuda.c:242-244 (CUDA) | `speed_internal_tally_solve` x4 per frame | `uint64_t` (speed_internal.h:325-326) | +4/frame | $2^{62}$ frames (1080p/8K/16K alike) | same | SAFE |
| hip/speed_chroma_hip.c:174 (HIP) | `(int)cw`, `(int)ch` | `unsigned` -> `int` | <= 32,768 | 15,360 | 32,768 | SAFE |
| hip/speed_chroma_hip.c:253-255 (HIP) | tally x4 per frame | `uint64_t` | +4/frame | $2^{62}$ frames | same | SAFE |

### speed_temporal: extractor files

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| cuda/speed_temporal_cuda.c:194-195 (CUDA) | `current = 2u * (index % 2u)`, `previous = 2u * ((index + 1u) % 2u)` | `unsigned` / `uint32_t` | index wraps at $2^{32}$ | 0..2 | 0..2 | SAFE (`index + 1u` wraps to 0 only at index = UINT32_MAX, which is odd, so `previous` is still the correct slot 0; defined unsigned wrap) |
| cuda/speed_temporal_cuda.c:203-207 (CUDA) | `(int32_t)previous + 1`, `(int32_t)(current + 1u)` | `uint32_t` -> `int32_t` | <= 3 | 3 | 3 | SAFE |
| cuda/speed_temporal_cuda.c:228-230 (CUDA) | tally x2 per frame | `uint64_t` | +2/frame | $2^{63}$ frames | same | SAFE |
| hip/speed_temporal_hip.c:170 (HIP) | `(int)w`, `(int)h` | `unsigned` -> `int` | <= 32,768 | 15,360 | 32,768 | SAFE |
| hip/speed_temporal_hip.c:222-223 (HIP) | `set = index % 2u`; `first = 2u * set` | `uint32_t` | <= 2 | 2 | 2 | SAFE |
| hip/speed_temporal_hip.c:232-234 (HIP) | tally x2 per frame | `uint64_t` | +2/frame | $2^{63}$ frames | same | SAFE |

### Asides (not verdict rows)

- Device memory, not an integer issue. The `speed_prescale` maximum of 4 makes the SCALED buffer 4 ch \* 16 \* N \* 4 B. That is about 34 GB at 16K and 256 GiB at the cap. The allocation fails cleanly (`vmaf_cuda_buffer_alloc` / `hipMalloc` return an errno). All size math is `size_t`.
- The same covariance divisor form exists outside G6 in `sycl/speed_sycl_pipeline.cpp:808` (`static_cast<float>(a.sub_w * a.sub_h)`). Not audited here; it most likely shares the OVERFLOW@CAP-ONLY parity issue.
- The CPU `compute_mean` (speed.c:775) also rounds its count to float. The means rows are therefore SAFE for parity even above $2^{24}$. Only the covariance divisor differs from the CPU.

### Coverage

| file | rows |
|---|---|
| cuda/speed_chroma_cuda.c | 2 |
| cuda/speed_chroma_cuda.h | none |
| cuda/speed_cuda_pipeline.c | 12 |
| cuda/speed_cuda_pipeline.h | none |
| cuda/speed/speed_cuda_params.h | none (uint64 pointer/size fields only; `plane_bytes` is `uint64_t`, covered by the speed_score.cu:256 row) |
| cuda/speed/speed_score.cu | 29 |
| cuda/speed_temporal_cuda.c | 3 |
| cuda/speed_temporal_cuda.h | none |
| hip/speed_chroma_hip.c | 2 |
| hip/speed_chroma_hip.h | none |
| hip/speed_hip_pipeline.c | 14 |
| hip/speed_hip_pipeline.h | none |
| hip/speed/speed_hip_device.h | 23 |
| hip/speed/speed_pipeline.hip | 5 |
| hip/speed_temporal_hip.c | 3 |
| hip/speed_temporal_hip.h | none |
| speed_gpu_common.h | 1 |
| speed_internal.h | 1 (plus tally types, counted in the speed_internal.c rows) |
| speed_cov.h | none (`size_t` parameters only) |
| speed_matmul.h | 1 |
| speed_givens.h | none (float only) |
| speed_internal.c (geometry source, read for derivation) | 8 |
| speed.c (read only for divisor/mean forms: :225-229, :775, :847) | 0 (cited as the CPU reference) |
| vif_tools.c:673-683 (lanczos weights indexing, read for derivation) | 0 (`ptrdiff_t`, cited in the speed_internal.c:916 row) |

Verdict totals: SAFE 102, OVERFLOW@16K 0, OVERFLOW@CAP-ONLY 2, DEPENDS 0.

## G7: runtime picture and buffer sizes, CUDA reduction helper (integrator)

### Runtime: picture / buffer size products (core/src/cuda, core/src/hip)

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| core/src/cuda/picture_cuda.c:188-189 | `aligned_y = (pic->w[0] + 31u) & ~31u`, `aligned_c` | unsigned | w rounded up to 32 | 15360 | 32768 | SAFE ($2^{15}$ << $2^{32}$; w capped at 32768 by pinned_alloc_check_args:151) |
| core/src/cuda/picture_cuda.c:191-192 | `pic->stride[i] = aligned << hbd` | unsigned -> ptrdiff_t | row bytes, hbd=1 at 16 bit | 30720 | 65536 | SAFE ($2^{16}$) |
| core/src/cuda/picture_cuda.c:193-194 | `*y_sz = pic->stride[0] * pic->h[0]`, `*uv_sz` | ptrdiff_t(long) \* unsigned -> long (LP64) -> size_t | pinned plane bytes | 265,420,800 | $2^{31}$ | SAFE ($2^{31}$ < $2^{63}$; LLP64/Windows: ptrdiff_t is long long, also 64-bit) |
| core/src/cuda/picture_cuda.c:195 | `*y_sz + 2 * *uv_sz` | size_t | 3 planes 4:4:4 | 796,262,400 | 3*$2^{31}$ = 6.4e9 | SAFE (< $2^{64}$) |
| core/src/cuda/picture_cuda.c:55, :80 | `m.WidthInBytes = (size_t)cuda_pic->w[i] * ((bpc + 7) / 8)` | size_t | row bytes | 30720 | 65536 | SAFE |
| core/src/cuda/picture_cuda.c:357-358 | `cuMemAllocPitch(..., (size_t)pic->w[i] * ((bpc+7)/8), pic->h[i], 8 << hbd)`; pitch out -> `(size_t *)&pic->stride[i]` | size_t | device pitch, CUDA rounds widthInBytes up to the device pitch alignment | pitch >= 30720 B | pitch >= 65536 B; plane = $2^{31}$ B | SAFE in host (size_t). Device-side consumers that take this pitch as `int` and form `y * pitch` reach $2^{31}$ at cap -> see per-feature rows. w/h of the cookie are capped at 32768 by libvmaf.c:432 (device_alloc_check_args itself checks only bpc). |
| core/src/cuda/common.c:507-523 | `vmaf_cuda_buffer_alloc(..., size_t size)` -> `cuMemAlloc(&buf->data, buf->size)` | size_t | caller-supplied byte count | n/a (callers' products audited per feature) | n/a | SAFE (in this TU; overflow, if any, is in the caller's size expression) |
| core/src/cuda/common.c:661-672 | `vmaf_cuda_buffer_host_alloc(..., size_t size)` | size_t | caller-supplied | n/a | n/a | SAFE (same note) |
| core/src/cuda/common.c:715-770 | `cuMemcpyHtoDAsync/DtoHAsync(..., buf->size, ...)` | size_t | whole buffer | n/a | n/a | SAFE |
| core/src/hip/picture_hip.c:154-157 | `hip_pic_staged_bytes`: `p->rows * p->row_bytes`, checked `p->rows > SIZE_MAX / p->row_bytes`, `bytes > SIZE_MAX - total` | size_t | plane bytes, <=3 planes | 265,420,800 per plane | $2^{31}$ per plane | SAFE (explicit overflow checks; size_t) |
| core/src/hip/picture_hip.c:171 | `dst + row * p->row_bytes`, `src + row * stride` | size_t | host row offsets | 265,420,800 | $2^{31}$ | SAFE (size_t) |
| core/src/hip/picture_hip.c:207 | `at += p->rows * p->row_bytes` | size_t | staging cursor | 796,262,400 | 6.4e9 | SAFE |
| core/src/hip/picture_hip.c:243 | `vmaf_hip_picture_alloc(ctx, out, size_t size)` -> `hipMalloc(&device, size)` | size_t | caller-supplied | n/a | n/a | SAFE (caller products audited per feature) |
| core/src/hip/shared_frame.c:78-80 | `plane_bytes`: `p->rows * p->row_bytes` with `SIZE_MAX / row_bytes` check | size_t | one packed plane | 265,420,800 | $2^{31}$ | SAFE |
| core/src/hip/shared_frame.c:103, 202, 379 | `(size_t)pic->w[p] * (bpc > 8 ? 2 : 1)` | size_t | row bytes | 30720 | 65536 | SAFE |
| core/src/hip/shared_frame.c:240 (`f->uploads += batch.count`) | `uploads` | uint64_t | upload counter, <= 6 per frame | 6 \* frames | same | SAFE ($2^{64}$ frames-scale) |
| core/src/hip/kernel_template.c:122, :128 | `vmaf_hip_kernel_readback_alloc(rb, ctx, size_t bytes)` -> `hipMalloc`, `hipHostMalloc` | size_t | caller-supplied | n/a | n/a | SAFE (caller products audited per feature) |

Coverage (runtime): core/src/cuda/picture_cuda.c (rows above), core/src/cuda/common.c (rows above), core/src/cuda/drain_batch.c (none: `unsigned n` counts registered events, bounded by VMAF_CUDA_DRAIN_BATCH_MAX), core/src/cuda/dispatch_strategy.c (none), core/src/hip/picture_hip.c (rows above), core/src/hip/shared_frame.c (rows above), core/src/hip/kernel_template.c (rows above), core/src/hip/common.c (none), core/src/hip/stubs.c (none), core/src/feature/hip/hip_hsaco_stubs.c (none: weak zero-length symbols), core/src/feature/cuda/cuda_tile_index.h (`2 * extent - idx - 2` in int, extent <= 32768, |idx| <= extent + radius: max ~$2^{16}$, SAFE).

### CUDA reduction helper used by the int64 accumulators (core/src/cuda/cuda_helper.cuh)

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| core/src/cuda/cuda_helper.cuh:125-133 (`warp_reduce`), :136-139 (`atomicAdd_int64`) | `x += int64_t(__shfl_down_sync(.., x & 0xffffffff, i)) \| int64_t(__shfl_down_sync(.., x >> 32, i) << 32)`; `atomicAdd((uint64_cu *)address, (uint64_cu)val)` | `int64_t` (shuffled as `long long` halves); atomic on `unsigned long long` | 32 lanes of the caller's int64 partials (the bound of each sum is the caller's: ADM rows, VIF frame sums, SSIM weights, rows in the per-feature sections) | caller-defined | caller-defined | SAFE as a helper: the halves reconstruct the neighbour's value exactly, and the atomic adds two's-complement bits, so the int64 result is the caller's exact sum while it fits. Aside: `(x >> 32) << 32` left-shifts a negative `long long` when a partial is negative, which is UB before C++20 (nvcc defaults to C++17); the ADM ADR-0155 negative i4 term is the only negative caller. |

Coverage addendum (integrator): core/src/cuda/cuda_helper.cuh (1 row above). Shared headers included by the twins and not named by a reader, checked by the integrator: `ff_pair.h` (none: fp32 pair arithmetic, no integer type), `motion_window.h` (none: declarations), `vif_tools.h` (none: float filters and declarations), `picture_copy.h` (none: declaration; `ptrdiff_t dst_stride`).

## Coverage (every file read, consolidated)

Per-file row counts and "none" verdicts are in each group's own Coverage subsection above. This list maps every in-scope file to the group that read it in full (G = section above).

### core/src/feature/cuda and core/src/feature/hip (130 files)

| file | lines | read by |
|---|---|---|
| `cuda/cuda_tile_index.h` | 63 | G2, G3a, G3b |
| `cuda/float_adm_cuda.c` | 987 | G2 |
| `cuda/float_adm_cuda.h` | 16 | G2 |
| `cuda/float_adm/float_adm_device.h` | 46 | G2 |
| `cuda/float_adm/float_adm_score.cu` | 301 | G2 |
| `cuda/float_motion_cuda.c` | 614 | G3a |
| `cuda/float_motion_cuda.h` | 16 | G3a |
| `cuda/float_motion/float_motion_score.cu` | 172 | G3a |
| `cuda/float_psnr_cuda.c` | 387 | G3b |
| `cuda/float_psnr_cuda.h` | 22 | G3b |
| `cuda/float_psnr/float_psnr_score.cu` | 122 | G3b |
| `cuda/float_ssim_cuda.h` | 20 | G4a |
| `cuda/float_vif_cuda.c` | 615 | G2 |
| `cuda/float_vif_cuda.h` | 16 | G2 |
| `cuda/float_vif/float_vif_device.h` | 46 | G2 |
| `cuda/float_vif/float_vif_score.cu` | 258 | G2 |
| `cuda/integer_adm/adm_cm.cu` | 943 | G1 |
| `cuda/integer_adm/adm_csf.cu` | 239 | G1 |
| `cuda/integer_adm/adm_csf_den.cu` | 125 | G1 |
| `cuda/integer_adm/adm_decouple_inline.cuh` | 184 | G1 |
| `cuda/integer_adm/adm_dwt2.cu` | 366 | G1 |
| `cuda/integer_adm/adm_dwt2_rows.h` | 88 | G1 |
| `cuda/integer_adm_cuda.c` | 1816 | G1 |
| `cuda/integer_adm_cuda.h` | 101 | G1 |
| `cuda/integer_cambi/cambi_score.cu` | 869 | G5 |
| `cuda/integer_cambi_cuda.c` | 994 | G5 |
| `cuda/integer_cambi_cuda.h` | 172 | G5 |
| `cuda/integer_ciede/ciede_device.h` | 270 | G3b |
| `cuda/integer_ciede/ciede_score.cu` | 87 | G3b |
| `cuda/integer_ciede_cuda.c` | 264 | G3b |
| `cuda/integer_ciede_cuda.h` | 19 | G3b |
| `cuda/integer_moment_cuda.c` | 403 | G3b |
| `cuda/integer_moment_cuda.h` | 31 | G3b |
| `cuda/integer_moment/moment_score.cu` | 212 | G3b |
| `cuda/integer_motion_cuda.c` | 903 | G3a |
| `cuda/integer_motion_cuda.h` | 30 | G3a |
| `cuda/integer_motion_sad_cuda.c` | 136 | G3a |
| `cuda/integer_motion_sad_cuda.h` | 79 | G3a |
| `cuda/integer_motion_v2_cuda.c` | 415 | G3a |
| `cuda/integer_motion_v2_cuda.h` | 16 | G3a |
| `cuda/integer_motion_v2/motion_v2_score.cu` | 216 | G3a |
| `cuda/integer_ms_ssim_cuda.c` | 893 | G4a |
| `cuda/integer_ms_ssim_cuda.h` | 19 | G4a |
| `cuda/integer_ms_ssim/ms_ssim_score.cu` | 320 | G4a |
| `cuda/integer_psnr_cuda.c` | 487 | G3b |
| `cuda/integer_psnr_cuda.h` | 28 | G3b |
| `cuda/integer_psnr_hvs_cuda.c` | 500 | G5 |
| `cuda/integer_psnr_hvs_cuda.h` | 55 | G5 |
| `cuda/integer_psnr_hvs/psnr_hvs_score.cu` | 510 | G5 |
| `cuda/integer_psnr/psnr_score.cu` | 130 | G3b |
| `cuda/integer_ssim_cuda.c` | 782 | G4a |
| `cuda/integer_ssim_cuda.h` | 20 | G4a |
| `cuda/integer_ssim/integer_ssim_score.cu` | 304 | G4a |
| `cuda/integer_ssim/ssim_score.cu` | 518 | G4a |
| `cuda/integer_vif_cuda.c` | 916 | G2 |
| `cuda/integer_vif_cuda.h` | 116 | G2 |
| `cuda/integer_vif/filter1d.cu` | 737 | G2 |
| `cuda/integer_vif/vif_statistics.cuh` | 210 | G2 |
| `cuda/speed_chroma_cuda.c` | 324 | G6 |
| `cuda/speed_chroma_cuda.h` | 24 | G6 |
| `cuda/speed_cuda_pipeline.c` | 497 | G6 |
| `cuda/speed_cuda_pipeline.h` | 78 | G6 |
| `cuda/speed/speed_cuda_params.h` | 71 | G6 |
| `cuda/speed/speed_score.cu` | 1286 | G6 |
| `cuda/speed_temporal_cuda.c` | 291 | G6 |
| `cuda/speed_temporal_cuda.h` | 24 | G6 |
| `cuda/ssim_cuda.c` | 449 | G4a |
| `cuda/ssim_cuda.h` | 20 | G4a |
| `cuda/ssimulacra2_cuda.c` | 850 | G4b |
| `cuda/ssimulacra2_cuda.h` | 120 | G4b |
| `cuda/ssimulacra2/ssimulacra2_blur.cu` | 186 | G4b |
| `cuda/ssimulacra2/ssimulacra2_device.cu` | 534 | G4b |
| `hip/ciede_hip.c` | 438 | G3b |
| `hip/ciede_hip.h` | 35 | G3b |
| `hip/float_adm/float_adm_hip_math.h` | 38 | G2 |
| `hip/float_adm/float_adm_score.hip` | 316 | G2 |
| `hip/float_adm_hip.c` | 839 | G2 |
| `hip/float_adm_hip.h` | 29 | G2 |
| `hip/float_moment_hip.c` | 532 | G3b |
| `hip/float_moment_hip.h` | 32 | G3b |
| `hip/float_moment/moment_score.hip` | 210 | G3b |
| `hip/float_motion/float_motion_rows.h` | 171 | G3a |
| `hip/float_motion/float_motion_score.hip` | 265 | G3a |
| `hip/float_motion_hip.c` | 934 | G3a |
| `hip/float_motion_hip.h` | 23 | G3a |
| `hip/float_psnr/float_psnr_score.hip` | 181 | G3b |
| `hip/float_psnr_hip.c` | 453 | G3b |
| `hip/float_psnr_hip.h` | 33 | G3b |
| `hip/float_ssim_hip.c` | 816 | G4a |
| `hip/float_ssim_hip.h` | 34 | G4a |
| `hip/float_ssim/ssim_decimate.h` | 116 | G4a |
| `hip/float_ssim/ssim_score.hip` | 305 | G4a |
| `hip/float_vif/float_vif_score.hip` | 268 | G2 |
| `hip/float_vif_hip.c` | 686 | G2 |
| `hip/float_vif_hip.h` | 23 | G2 |
| `hip/hip_hsaco_stubs.c` | 37 | G7 |
| `hip/hip_tile_index.h` | 57 | G3a, G3b |
| `hip/integer_adm/adm_cm.hip` | 848 | G1 |
| `hip/integer_adm/adm_csf_den.hip` | 125 | G1 |
| `hip/integer_adm/adm_csf.hip` | 223 | G1 |
| `hip/integer_adm/adm_decouple_inline.hip` | 187 | G1 |
| `hip/integer_adm/adm_dwt2.hip` | 381 | G1 |
| `hip/integer_adm/adm_dwt2_rows.h` | 71 | G1 |
| `hip/integer_adm_hip.c` | 1782 | G1 |
| `hip/integer_adm_hip.h` | 140 | G1 |
| `hip/integer_cambi/cambi_hip_device.h` | 779 | G5 |
| `hip/integer_cambi/cambi_score.hip` | 286 | G5 |
| `hip/integer_cambi_hip.c` | 1004 | G5 |
| `hip/integer_cambi_hip.h` | 77 | G5 |
| `hip/integer_ciede/ciede_hip_math.h` | 62 | G3b |
| `hip/integer_ciede/ciede_score.hip` | 115 | G3b |
| `hip/integer_motion_hip.c` | 708 | G3a |
| `hip/integer_motion_sad_hip.c` | 134 | G3a |
| `hip/integer_motion_sad_hip.h` | 95 | G3a |
| `hip/integer_motion_v2_hip.c` | 462 | G3a |
| `hip/integer_motion_v2_hip.h` | 34 | G3a |
| `hip/integer_motion_v2/motion_v2_score.hip` | 190 | G3a |
| `hip/integer_ms_ssim_hip.c` | 957 | G4a |
| `hip/integer_ms_ssim_hip.h` | 33 | G4a |
| `hip/integer_ms_ssim/ms_ssim_arith.h` | 298 | G4a |
| `hip/integer_ms_ssim/ms_ssim_score.hip` | 115 | G4a |
| `hip/integer_psnr_hip.c` | 581 | G3b |
| `hip/integer_psnr_hip.h` | 27 | G3b |
| `hip/integer_psnr_hvs_hip.c` | 755 | G5 |
| `hip/integer_psnr_hvs_hip.h` | 74 | G5 |
| `hip/integer_psnr_hvs/psnr_hvs_score.hip` | 526 | G5 |
| `hip/integer_psnr/psnr_score.hip` | 132 | G3b |
| `hip/integer_ssim_hip.c` | 434 | G4a |
| `hip/integer_ssim_hip.h` | 35 | G4a |
| `hip/integer_ssim/integer_ssim_score.hip` | 290 | G4a |
| `hip/integer_vif_hip.c` | 790 | G2 |
| `hip/integer_vif_hip.h` | 102 | G2 |
| `hip/integer_vif/vif_statistics.hip` | 646 | G2 |
| `hip/speed_chroma_hip.c` | 345 | G6 |
| `hip/speed_chroma_hip.h` | 24 | G6 |
| `hip/speed_hip_pipeline.c` | 551 | G6 |
| `hip/speed_hip_pipeline.h` | 116 | G6 |
| `hip/speed/speed_hip_device.h` | 1225 | G6 |
| `hip/speed/speed_pipeline.hip` | 143 | G6 |
| `hip/speed_temporal_hip.c` | 304 | G6 |
| `hip/speed_temporal_hip.h` | 24 | G6 |
| `hip/ssimulacra2_hip.c` | 1075 | G4b |
| `hip/ssimulacra2_hip.h` | 130 | G4b |
| `hip/ssimulacra2/ssimulacra2_device.hip` | 780 | G4b |

### Shared core/src/feature headers (integer parts)

| file | read by |
|---|---|
| `core/src/feature/ordered_sum.h` | G3b, G4b |
| `core/src/feature/float_moment_sum_gpu.h` | G3b |
| `core/src/feature/float_moment_sum.h` | G3b |
| `core/src/feature/ciede_frame_sum.h` | G3b |
| `core/src/feature/ciede_ff_math.h` | G3b |
| `core/src/feature/float_psnr_rows.h` | G3b |
| `core/src/feature/psnr_score.h` | G3b |
| `core/src/feature/adm_cm_accumulator.h` | G1 |
| `core/src/feature/integer_adm_kernels.h` | G1 |
| `core/src/feature/adm_csf_fixed_point.h` | G1 |
| `core/src/feature/adm_score.h` | G1, G2 |
| `core/src/feature/adm_angle_flag.h` | G1 |
| `core/src/feature/adm_gain_limit.h` | G1 |
| `core/src/feature/adm_float_reference.h` | G2 |
| `core/src/feature/float_adm_gpu_common.h` | G2 |
| `core/src/feature/float_vif_gpu_common.h` | G2 |
| `core/src/feature/integer_vif_sv_sq.h` | G2 |
| `core/src/feature/vif_log2_table.h` | G2 |
| `core/src/feature/float_motion_sad.h` | G3a |
| `core/src/feature/motion_blend_tools.h` | G3a |
| `core/src/feature/motion_tools.h` | G3a |
| `core/src/feature/integer_motion.h` | G3a |
| `core/src/feature/speed_gpu_common.h` | G6 |
| `core/src/feature/speed_internal.h` | G6 |
| `core/src/feature/speed_cov.h` | G6 |
| `core/src/feature/speed_matmul.h` | G6 |
| `core/src/feature/speed_givens.h` | G6 |
| `core/src/feature/cambi_internal.h` | G5 |
| `core/src/feature/cambi_c_values_frame.h` | G5 |
| `core/src/feature/cambi.h` | G5 |
| `core/src/feature/psnr_hvs_score.h` | G5 |
| `core/src/feature/ssimulacra2_math.h` | G4b |
| `core/src/feature/ssimulacra2_score.h` | G4b |
| `core/src/feature/ssimulacra2_eotf_lut.h` | G4b |
| `core/src/feature/ssimulacra2_pixel_format.h` | G4b |
| `core/src/feature/integer_ssim.h` | G4a |
| `core/src/feature/iqa/decimate_dim.h` | G4a |
| `core/src/feature/ff_math.h` | G3b |
| `core/src/feature/ff_pair.h` | G7 |
| `core/src/feature/motion_window.h` | G7 |
| `core/src/feature/vif_tools.h` | G7 |
| `core/src/feature/picture_copy.h` | G7 |
| `core/src/feature/nonfinite_score.h` | G4a |
| `core/src/feature/barten_csf_tools.h` | G1 |

### Runtime (size products only)

| file | read by |
|---|---|
| `core/src/cuda/picture_cuda.c` | G7 |
| `core/src/cuda/common.c` | G7 |
| `core/src/cuda/drain_batch.c` | G7 |
| `core/src/cuda/dispatch_strategy.c` | G7 |
| `core/src/cuda/cuda_helper.cuh` | G7 |
| `core/src/hip/picture_hip.c` | G7 |
| `core/src/hip/shared_frame.c` | G7 |
| `core/src/hip/kernel_template.c` | G7 |
| `core/src/hip/common.c` | G7 |
| `core/src/hip/stubs.c` | G7 |

CPU references the groups read to derive term maxima or compare arithmetic (no rows of their own unless a group states one): `integer_adm.c`, `integer_vif.c`, `integer_motion.c`, `integer_psnr.c`, `ciede.c`, `integer_ssim.c`, `iqa/decimate.c`, `cambi.c`, `speed.c`, `speed_internal.c`, `third_party/xiph/psnr_hvs.c`, `psnr_hvs_score.c`, `core/src/picture.c`, `core/src/libvmaf.c` (dimension guards).
