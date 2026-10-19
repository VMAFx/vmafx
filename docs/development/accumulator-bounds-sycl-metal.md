<!-- markdownlint-disable MD001 MD004 MD013 MD024 MD029 MD032 MD036 MD060 -->

# Accumulator bounds: SYCL and Metal

Appendix of [integer accumulator bounds](accumulator-bounds.md): every integer accumulator, size product and offset of the SYCL and Metal feature twins and their runtimes, read on master `571565a47` (before the fixes the main page lists). Rows marked OVERFLOW or DEPENDS name their state row on the main page.

- Source: master `571565a47`. Read only: nothing edited, built or run on a device. Commands were `grep`, `sed`, `cat` and `python3` bound arithmetic; the scale-0 ADM row replay `scripts/dev/adm_cm_row_bound.py` was re-run.
- Scope (166 files): every file under `core/src/feature/sycl/`, `core/src/sycl/`, `core/src/feature/metal/` and `core/src/metal/`, plus the shared `core/src/feature/*.h` helpers those twins include for integer math (`adm_gain_limit.h`, `adm_angle_flag.h`, `adm_cm_accumulator.h`, `float_psnr_rows.h`, `ordered_sum.h`, `float_moment_sum*.h`, `ff_math.h`), where the [CUDA and HIP appendix](accumulator-bounds-cuda-hip.md) is cited instead of re-derived.
- Envelope: 16K = 15360 x 8640, N = 132,710,400 (< $2^{27}$); 8K DCI 8192 x 4320; 1080p N = 2,073,600. Cap: W, H <= 32768, N <= $2^{30}$. Samples up to 16 bit (65535; $65535^{2}$ = 4,294,836,225), 4:4:4, worst-case content. "Out-of-range codewords" means uint16 samples above $2^{\mathrm{bpc}} - 1$ at bpc 9-15, which no libvmaf entry point rejects (prior audit, common root of rows 8-10).
- Paths: feature rows name the file under `core/src/feature/sycl/` or `core/src/feature/metal/` by its basename (the summary below writes them as `sycl/<file>` / `metal/<file>`); runtime rows carry `sycl/` or `metal/` relative to `core/src/`; shared headers are under `core/src/feature/`. Every row is labelled SYCL or Metal.

## Verdict totals (340 rows)

| verdict | SYCL | Metal | total |
|---|---|---|---|
| SAFE | 203 | 114 | 317 |
| OVERFLOW@16K | 1 | 1 | 2 |
| OVERFLOW@CAP-ONLY | 2 | 5 | 7 |
| DEPENDS | 8 | 6 | 14 |

## Every row that is not SAFE

### OVERFLOW@16K

1. \*\*SYCL integer ADM scale-0 contrast-masking row total, `int64_t`\*\* (`sycl/integer_adm_sycl.cpp:1127-1141` per-item `cm[b] +=` for DLM and AIM, `:1146-1158` sub-group `reduce_over_group`, `:1184-1189` `row_total += lmem`, `:1164-1174` fold).
   - Term per column (`adm_dev_cm_cube`, `:1085-1098`, shifts from `adm_cm_shifts`, `:1264-1276`): `x_sq = (x^2 + 2^28) >> 29`, then `(x_sq * x + rnd) >> (ceil(log2 Wb) - 4)` for h/v (`>> 30`, `- 3` for d); x is the scale-0 excess `|rfactor * v| - thr * 2^shift`, the CPU's formula exactly.
   - Worst case: thr = 0 (ref == dis for DLM; a flat reference for AIM), x = |band| \* weight. `scripts/dev/adm_cm_row_bound.py` (re-run) gives the exact row maximum over column sign patterns: 1.0207 \* INT64_MAX = 9.414e18 at W = 64 (cols 28), 0.973 at W = 128, 0.855 at 15360, 0.912 at 8K DCI / 16384 / 32768, at the default weights 36,453 (h/v) and 49,417 (d).
   - W = 63-64 is inside the envelope (`adm_frame_size_check` admits W >= 17), so the signed sub-group / work-group int64 sums wrap at default options. Non-default CSF weights (h/v >= 38,406 or d >= 60,965 at 16K, below the budget 46,603 / 65,536) wrap at every size.
   - Known item 2, confirmed for SYCL. Same defect as the CPU `integer_adm_kernels.h:1017-1019, 1079-1099` and CUDA/HIP prior row 2. The frame accumulator (`fetch_add` of rows `>> ceil(log2 Hb)`) cannot wrap (< 0.875 \* $2^{63}$ for any int64 row), so the visible effect is a negative or wrong band numerator, then NaN or a wrong score.
2. \*\*Metal float VIF moment-plane index, `uint`\*\* (`metal/float_vif.metal:117-122` store, `:160-163` read; `plane = scale_w[0] * scale_h[0]` from `float_vif_metal.mm:563-565, 586-588`).
   - `moments[k * plane + at]`, k = 0..4: the largest index is 5 \* plane - 1, which wraps once plane >= 858,993,460.
   - `vif_prescale` (option range 0.1 - 4.0, `float_vif_metal.mm:190-198`, same as the CPU) scales the scale-0 plane: at 16K, prescale > 2.545 wraps (prescale 4: plane 2,123,366,400, largest index 1.06e10). At the default prescale 1.0 it wraps above N = 858,993,459 (e.g. 29,310 x 29,310), inside the cap envelope.
   - The k >= 2 moment planes then alias the start of the buffer. The host sizes the buffer in size_t (`float_vif_metal.mm:421-423`, 42 GB at 16K prescale 4), so on most devices allocation fails first; where `maxBufferLength` admits it, the wrap is silent. SYCL float_vif indexes in size_t (SAFE).

### OVERFLOW@CAP-ONLY

3. \*\*SYCL PSNR-HVS prefix scan stops at 32,768 chunks\*\* (`sycl/integer_psnr_hvs_sycl.cpp:787-795`, compaction `:822-833`).
   - `limit = num_chunks < 32768u ? num_chunks : 32768u`; num_chunks = ceil(total_blocks / 256); 16K 4:4:4 has 8,122,188 blocks = 31,728 chunks (3.2 % margin). Above 8,388,608 blocks (16384 x 8640 4:4:4: 8,662,680; luma-only above ~20.3K x 20.3K; cap 4:4:4: 256,779 chunks) `chunk_offsets[c >= 32768]` are never written, `d_scratch` (`malloc_device`, `:956-957`) is not cleared, and the compaction writes `packed_terms + chunk_offsets[chunk] + intra` from stale offsets: out-of-bounds device writes, short `total_terms`, wrong or NaN scores.
   - Known item 5, confirmed for SYCL (identical to HIP prior row 3). The `uint32_t` running sum is safe (<= 4,207,058,112 at the cap). Metal has no compaction (terms at `(ulong)slot * 64`), so the item is refuted there.
4. \*\*SYCL SpEED covariance divisor rounded to fp32\*\* (`sycl/speed_sycl_pipeline.cpp:808-809`): `static_cast<float>(a.sub_w * a.sub_h)`.
   - Not a wrap (uint32 product <= 67,010,596). The count is exact at 16K for every prescale (largest 3836 \* 2156 = 8,270,416 < $2^{24}$). Above 16K with prescale > ~2 it can be inexact (W = H = 32740 at prescale 4: $8181^{2}$ = 66,928,761, odd), while the CPU `speed.c:847` divides by the exact double. The mean divisor (`:687`) rounds the same way the CPU `compute_mean` does (SAFE).
   - Known item 4, confirmed (already being fixed). Metal has no SpEED twin.
5. \*\*Metal integer SSIM moment-plane index, `uint`\*\* (`metal/integer_ssim.metal:74-80`, `:97`, `:115`, `:131-138`): `hbuf[k * plane + index]`, `plane = W * H`, k = 0..4 (five int64 planes). Largest index 5N - 1 wraps once N >= 858,993,460 (e.g. 32768 x 26,215): 16K is 0.15 \* $2^{32}$, the cap 1.25 \* $2^{32}$. The y2 plane then aliases the mux plane. Host buffer is size_t, 34 GB there (`integer_ssim_metal.mm:223`); init checks only W, H >= 1. SYCL integer SSIM indexes in size_t (SAFE).
6. \*\*Metal float_ms_ssim moment-plane index, `uint`\*\* (`metal/float_ms_ssim.metal:115-126`, `:142-148`): same shape, `plane = (W - 10) * H` at scale 0, which MS-SSIM always runs at full resolution. Wraps at default options once (W - 10) \* H >= 858,993,460 (e.g. 32768 x 26,223); 16K: 663,119,999 (SAFE).
7. \*\*Metal float_ssim moment-plane index, `uint`\*\* (`metal/float_ssim.metal:80-86`, `:94-100`): same as row 6, reachable only with the `scale=1` option pinned (the auto scale is >= 2 for min(W, H) >= 384 and init refuses, `float_ssim_metal.mm:203-209`).
8. \*\*Metal float VIF host plane product, `unsigned`\*\* (`metal/float_vif_metal.mm:563-565, 586-588`): `scale_w * scale_h` passed as `uint32_t plane`. Fits at 16K for every prescale (2,123,366,400 at prescale 4); above 16K with prescale >= 2.0 it wraps ($65536^{2}$ = $2^{32}$).
9. \*\*Metal float VIF term index, `uint`\*\* (`metal/metal_float_vif_math.h:171-175`, used at `float_vif.metal:169, 199`): `(x * height + y) * 2`, largest 2 \* plane - 1. 16K prescale 4: 4,246,732,799 (1.1 % below $2^{32}$, SAFE); wraps for plane >= $2^{31}$ (prescale > 1.414 at the cap).

### DEPENDS

10. \*\*SYCL integer PSNR clip `apsnr_sse[p] += sse`, `uint64_t`\*\* (`sycl/integer_psnr_sycl.cpp:550`) and \*\*11. Metal\*\* (`metal/integer_psnr_metal.mm:386`). One frame adds up to N \* $65535^{2}$. Frames to wrap at 16-bit max-diff: 1080p 2,072; 8K DCI 122; 16K 33; cap 5 ($2^{64}$ / (N \* 4,294,836,225) = 2071.3, 121.4, 32.4, 4.0). 12 bit: 530,502 / 31,085 / 8,290 / 1,025. 10 bit: 8.50M / 498,076 / 132,821 / 16,417. 8 bit: 136.8M / 8.0M / 2.14M / 264,205. Silent wrap, APSNR too high. Known item 1, confirmed for both; same type as the CPU `integer_psnr.c:211, 249` and CUDA/HIP prior rows 6-7.
12. \*\*SYCL integer PSNR per-item SSE, `uint32_t`\*\* (`sycl/integer_psnr_sycl.cpp:168-172`, instantiated for bpc <= 12 at `:198`). 16 squares per work-item: in range <= 16 \* $4095^{2}$ = 268,304,400; with out-of-range codewords at bpc 9-12 two squares of $65535^{2}$ already exceed $2^{32}$. The CPU sums 16-bit rows in uint64 (`integer_psnr.c:129-135`), so SYCL then differs from the CPU. Not frame-size dependent.
13. \*\*SYCL motion vertical sum, `int32_t`\*\* (`sycl/integer_motion_pipeline_sycl.cpp:99-103`, chosen for bpc <= 15 at `:46, 224-225`). In range (bpc 15) the sum is at most 65536 \* 32767 + 16384 = 2,147,434,496, 49,151 below INT32_MAX. With out-of-range codewords at bpc 9-15 it reaches 65536 \* 65535 = 4,294,901,760 (the first two products alone 2.57e9): signed overflow (UB). The CPU (`integer_motion.c:226-231`) and CUDA's 16 bpc kernel use int64; Metal uses `long` (SAFE).
14. \*\*SYCL float_ssim decimation fixed point, `int64_t`\*\* (`sycl/integer_ssim_sycl.cpp:269-273`, `:290-302`, sample scale `:625-641`). Each product `(raw * sample_scale) * (1 / scale^2)` is converted to units of $2^{-52}$ and the scale x scale window is summed. In range the window mean is <= 255.996, so the sum is < $2^{60}$. With out-of-range codewords the mean reaches raw / 4 = 16,383.75 at bpc 10 or raw / 16 = 4,095.94 at bpc 12, i.e. $2^{66}$ or $2^{64}$ in those units: the sum overflows once the window mean reaches 2048 (raw >= 8192 at bpc 10, raw >= 32768 at bpc 12), and at bpc 10, scale 2 a single product overflows the float-to-int64 conversion. The CPU forms the sum in double. The same exposure exists in the CUDA (`cuda/integer_ssim/ssim_score.cu:317`) and HIP (`hip/float_ssim/ssim_decimate.h:89`) twins, which the prior G4a rows marked SAFE assuming in-range samples (aside B1).
15. \*\*SYCL integer VIF horizontal mean sum, `uint32_t`\*\* (store `sycl/integer_vif_sycl.cpp:490-491` and fused `:1201-1204`, both without the `(uint16_t)` the CPU `integer_vif.c:450-451` applies; sums `:777-778, 793-794, 828-829`, fused `:1239-1240, 1255-1256`). In range tmp <= 65535 and the 17-tap sum <= 4,294,901,760; for bpc 9-15 with out-of-range codewords tmp reaches 8,388,480 (bpc 9) and the sum 5.5e11, which wraps. Known item 7, confirmed for SYCL (same as HIP prior row 9).
16. \*\*SYCL integer VIF rd (next-scale) sum, `uint32_t`\*\* (store `:504-505`, fused `:1210-1214`, sums `:784-785, 802-803, 837-838`, fused `:1246-1247, 1264-1265`, then `dev_downsample_rd` `:748-753`). Same cause for `ref_convol` (CPU `integer_vif.c:205-206` truncates); the later `& 0xFFFF` cannot undo a wrapped 32-bit sum. Same as HIP prior row 10.
17. \*\*Metal integer VIF `decimate_16` rd sum, `uint`\*\* (`metal/integer_vif.metal:665-677`, `filt_scale == 1`): `v_ref_r = (v_ref + round) >> bpc` is not narrowed to `ushort`, so `acc_ref += cj * v_ref_r` (9 taps) reaches 5.5e11 at bpc 9 with out-of-range codewords and wraps. The Metal mean path does truncate (`(ushort)`, `:343, 486`), so known item 7 holds for Metal only on this rd path.
18. \*\*SYCL integer ADM scale-0 flt narrowed to int16\*\* (`sycl/integer_adm_sycl.cpp:856-861`, `adm_i16((4369 * |csf| + 2048) >> 12)`) and \*\*19. Metal\*\* (`metal/integer_adm.metal:672-677`, stored with `iadm_write16`). Wraps negative once |csf| >= 30,720, i.e. an h/v CSF weight >= 43,900 (budget 46,603; default 36,453 gives flt <= 27,212). Option-dependent, bit-identical with the CPU (`integer_adm_kernels.h:488-489`) and CUDA/HIP prior row 12. The Metal AIM pass (`:1083-1100`) forms the same flt inline in `int` without narrowing, so with such weights its AIM threshold differs from the CPU's.
20. \*\*SYCL integer ADM scale-0 `x_sq` narrowed to int32\*\* (`sycl/integer_adm_sycl.cpp:1094-1095`). Fits while thr >= 0 (h/v x <= 1.0686e9 at the budget: x_sq <= 2.127e9; d: 2.103e9). A negative thr from row 18 lets the excess reach the INT32_MAX clamp, x_sq = $2^{33}$, which narrows modulo $2^{32}$. As CUDA/HIP prior row 13.
21. \*\*Metal integer ADM scale-0 cube in `long`\*\* (`metal/integer_adm.metal:759-765`, called at `:883-884, 1107-1108`). Metal keeps x_sq in 64 bits (the CPU narrows to int32); with thr >= 0 `x_sq * x` <= 3.16e18, but with x = INT32_MAX (row 19's negative thr) `x_sq * x = 2^33 * 2^31 = 2^64` overflows `long`.
22. \*\*Metal integer ADM scale-0 contrast-masking row, `ulong`\*\* (`metal/integer_adm.metal:836-896, 1066-1115`; carry-correct `atomic_uint` pair `:728-750`). The row that wraps int64 elsewhere (row 1: 9.414e18 at W = 64) is 0.51 \* $2^{64}$ here, so at default weights Metal does not wrap and returns the arithmetically correct value where the CPU, SYCL, CUDA and HIP overflow (a cross-backend mismatch, not a Metal defect). The term grows with $\mathrm{weight}^{3}$: an h/v weight >= ~45,600 (budget 46,603) wraps the ulong row too. Known item 2, refuted for Metal at default options.
23. \*\*Metal integer ADM host frame sum, `int64_t`\*\* (`metal/integer_adm_metal_host.c:305-315`, `t.cm[band] += (int64_t)iadm_slot(...)`, same for AIM). Rows are folded by `>> ceil(log2 Hb)` and rows / $2^{s}$ <= 0.875, so at default weights the sum is <= 0.875 \* 9.414e18 = 0.89 \* INT64_MAX (SAFE); with an h/v weight >= ~37,850 (W = 63-64) the signed host sum can wrap. The SYCL/CUDA/HIP frame accumulators avoid this only because their rows have already wrapped.

## Known items 1-8

| # | item | SYCL | Metal |
|---|---|---|---|
| 1 | apsnr clip uint64 sum | confirmed, DEPENDS (row 10): `integer_psnr_sycl.cpp:550`; 16-bit max-diff wraps after 2,072 / 122 / 33 / 5 frames (1080p / 8K DCI / 16K / cap) | confirmed, DEPENDS (row 11): `integer_psnr_metal.mm:386`, same counts |
| 2 | integer ADM scale-0 CM row total int64 at W ~ 64 | confirmed, OVERFLOW@16K (row 1): 1.0207 \* INT64_MAX at W = 64, default weights | refuted at default options: the row is `ulong`, 0.51 \* $2^{64}$ (row 22); DEPENDS on CSF weight (ulong row >= ~45,600 h/v; signed host frame sum >= ~37,850, row 23) |
| 3 | scales 1-3 decouple double -> int32 before min | refuted: `adm_gain_limit_product()` (exact truncated product in 64-bit integers, `adm_gain_limit.h:59-74`) then min / max with t in int64 (`integer_adm_sycl.cpp:808-827`); the result lies between 0 and t | refuted: same helper and int64 min / max (`metal_integer_adm_math.h:102-115`) |
| 4 | SpEED covariance divisor `(float)(sub_w * sub_h)` | confirmed at `speed_sycl_pipeline.cpp:808`, OVERFLOW@CAP-ONLY (row 4; exactness, not a wrap; the mean at `:687` is SAFE) | n/a (no Metal SpEED) |
| 5 | psnr_hvs chunk / scan limits | confirmed, OVERFLOW@CAP-ONLY (row 3): `integer_psnr_hvs_sycl.cpp:787` caps the scan at 32,768 chunks; 16K 4:4:4 uses 31,728 | refuted: no compaction; terms at `(ulong)slot * 64` (`integer_psnr_hvs.metal:158`) |
| 6 | float_psnr block sums at 10/12 bit with out-of-range codewords | refuted: per-term `uint64_t` (`float_psnr_sycl.cpp:142`), sub-group and work-group sums in `uint64_t` | refuted: term `uint` <= 4,294,836,224 (= fl32($65535^{2}$), fits), group sum `ulong` |
| 7 | VIF missing uint16 truncation vs CPU `integer_vif.c:450` | confirmed, DEPENDS (rows 15-16): mean and rd planes, separate and fused paths | partly: the mean path truncates (`(ushort)`, SAFE); the `decimate_16` rd path does not (row 17, DEPENDS) |
| 8 | float_motion mirror for W, H in 3..17 | refuted: `dev_mirror_fm()` result is clamped by `vmaf_sycl_tile_index()` (`float_motion_sycl.cpp:190-194`), W, H >= 3 enforced (`:432`) | refuted: `vmaf_mtl_fm_reflect101()` is periodic (`\|idx\| % (2 (size - 1))`, `metal_float_motion_math.h:181-189`), always in range |

## Asides (not a verdict change; worth a ticket)

- **B1 (CUDA/HIP, outside this scope): float_ssim decimation with out-of-range codewords.** Row 14's int64 fixed-point window sum is shared by `cuda/integer_ssim/ssim_score.cu:317` (`__float2ll_rz` saturates each product, the int64 sum then wraps) and `hip/float_ssim/ssim_decimate.h:89` (`(int64_t)` of an out-of-range float is UB). The prior G4a bound (`<= 255.996 * 2^52 < 2^60`) holds only for samples <= $2^{\mathrm{bpc}} - 1$.
- **B2 (common root).** Rows 12-17 all need uint16 codewords above $2^{\mathrm{bpc}} - 1$. Only CAMBI validates samples against bpc (SYCL `launch_validate`, `integer_cambi_sycl.cpp:355-375`, as `cambi.c` does). A single check at picture import would retire every out-of-range DEPENDS row in all backends.
- **B3 (Metal 32-bit indices, zero headroom).** `float_moment.metal:120-121, 154-155` and `integer_motion_v2.metal:132, 224` form byte offsets in `int`: 32767 \* 65536 = 2,147,418,112 at the cap, 65,535 below INT32_MAX. Safe only because the host always packs rows (`row_bytes = W * bpp`); any padded stride at H = 32768 would wrap. `float_ms_ssim_metal.mm:371` (`window_offset`, `unsigned`) reaches 4,282,811,768 = 0.997 \* $2^{32}$ at the cap with 4:4:4 and `enable_chroma`.
- **B4 (SYCL id queries).** icpx compiles with `-fsycl-id-queries-fit-in-int` by default (Intel DPC++ developer guide), so global ids and linear ids must fit `int`. Every SYCL range in scope stays below INT_MAX at 16K, including float_vif's `range<1>(pixels)` at prescale 4 (2,123,366,400, 1.1 % below). Above 16K that range can exceed INT_MAX; the DPC++ runtime then rejects the launch rather than wrapping.
- **B5 (Metal buffer sizes).** The CAP-ONLY Metal rows (5-9) need buffers of 21-42 GB; on a device whose `maxBufferLength` is smaller the allocation fails cleanly (-ENOMEM) before the wrap. The defect is the 32-bit index, not the allocation.
- **B6 (Metal vs CPU ADM).** Row 22: at W = 63-64 and default options Metal returns a correct scale-0 ADM numerator where every other backend (CPU included) wraps int64. Fixing row 1 in the CPU/SYCL/CUDA/HIP sum (e.g. an unsigned 64-bit row as Metal does, or a 128-bit row) would restore cross-backend agreement.

## Verification log

- Row 1: read `integer_adm_sycl.cpp:1000-1276` (cube, shifts, row reductions, fold) and `:405-452`, `:719-975`; re-ran `scripts/dev/adm_cm_row_bound.py` (output in the table above); confirmed `adm_frame_size_check` admits W >= 17.
- Row 2 and rows 8-9: read `float_vif.metal` in full, `metal_float_vif_math.h:68-176`, `float_vif_metal.mm:350-440, 555-600`; option range at `float_vif_metal.mm:190-198`, default `vif_options.h:43` (1.0).
- Row 3: read `integer_psnr_hvs_sycl.cpp:676-860, 940-1220`.
- Row 4: read `speed_sycl_pipeline.cpp:640-850`.
- Rows 5-7: read `integer_ssim.metal`, `float_ssim.metal`, `float_ms_ssim.metal` in full and the host allocation / geometry code of each `.mm`; thresholds from python (5N - 1 vs $2^{32}$).
- Rows 10-12: read `integer_psnr_sycl.cpp:140-300, 540-560`, `integer_psnr_metal.mm:300-430`, CPU `integer_psnr.c:119-250`.
- Row 13: read `integer_motion_pipeline_sycl.cpp` in full and CPU `integer_motion.c:164-257`.
- Row 14: read `integer_ssim_sycl.cpp:86-345, 540-680`; cross-checked the CUDA and HIP decimation lines named in B1.
- Rows 15-17: read `integer_vif_sycl.cpp:330-1320`, `integer_vif.metal` in full, CPU `integer_vif.c:195-215, 400-460`.
- Rows 18-23: read `integer_adm.metal:280-1209`, `metal_integer_adm_math.h`, `integer_adm_metal_host.c:290-320`.

## Runtime: SYCL (`core/src/sycl/`) and Metal (`core/src/metal/`)

No integer accumulators over pixels. Rows are size products, offsets and pitches.

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| SYCL sycl/picture_sycl.cpp:51-53, 80-82 | `row_bytes = pic->w[plane] * bpp`; `total = row_bytes * pic->h[plane]` | `unsigned * size_t` -> `size_t` | packed plane bytes | 265,420,800 | $2^{31}$ | SAFE (size_t; $2^{31}$ < $2^{64}$) |
| SYCL sycl/picture_sycl.cpp:64-65, 97 | `d + y * row_bytes`, `src + y * pic->stride[plane]` (row loop) | `unsigned * size_t`, `unsigned * ptrdiff_t` | row offset | 265,390,080 | 2,147,418,112 | SAFE (64-bit) |
| SYCL sycl/picture_sycl.cpp:117, 136 | `plane_size = (size_t)c->w * c->h * bpp`; `stride[0] = c->w * bpp` | `size_t` | pool plane bytes | 265,420,800 | $2^{31}$ | SAFE |
| SYCL sycl/picture_sycl.cpp:318-325 | aligned widths `(w + 31) & ~31u` (`unsigned`); `stride << hbd`; `(size_t)stride * h`; `y_sz + 2 * uv_sz` | `unsigned`, `ptrdiff_t`, `size_t` | pinned 3-plane buffer | 796,262,400 | 3 x $2^{31}$ | SAFE (aligned width <= 32768 fits unsigned; products in size_t) |
| SYCL sycl/common.cpp:682-683, 940 | `buf_size = (size_t)w * h * bytes_per_pixel`; chroma `plane_bytes = (size_t)cw * ch * bpp` | `size_t` | shared luma / chroma buffers | 265,420,800 | $2^{31}$ | SAFE |
| SYCL sycl/common.cpp:748 | `static_cast<unsigned>(src_stride) == row_bytes` | `ptrdiff_t` -> `unsigned` | caller's row pitch, compared only | <= 30,720 + pad | <= 65,536 + pad | SAFE (stride < $2^{32}$; narrowing cannot alias) |
| SYCL sycl/common.cpp:754-757, 889-897, 998-1003 | `dst += row_bytes`, `src += src_stride`, `row_bytes * h`, `(size_t)y * row_bytes` | pointer / `size_t` | row copies | 265,420,800 | $2^{31}$ | SAFE |
| SYCL sycl/common.cpp:871 (`vmaf_sycl_upload_plane`) | `unsigned pitch` parameter | `unsigned` | D3D11 / caller row pitch | <= $2^{17}$ | <= $2^{17}$ | SAFE |
| SYCL sycl/common.cpp:1619-1623 | FNV-1a `crc *= 16777619u` | `uint32_t` (intentional modular hash) | debug checksum | n/a | n/a | SAFE (defined unsigned wrap, by design) |
| SYCL sycl/common.cpp:1726-1733 | profiling `total_ns += delta_ns`, `count++` | `uint64_t` | per-kernel nanoseconds | $2^{64}$ ns = 584 years | same | SAFE |
| SYCL sycl/d3d11_import.cpp:82-100 | `mapped.RowPitch` handed to `vmaf_sycl_upload_plane(..., unsigned pitch, ...)` | `UINT` -> `unsigned` | D3D11 staging row pitch | <= 30,720 + pad | <= 65,536 + pad | SAFE |
| SYCL sycl/dmabuf_import.cpp:227 | `num_pixels = (size_t)w * h` -> `range<1>(num_pixels)` | `size_t` | P010 normalize items (global id assumed to fit int by icpx default `-fsycl-id-queries-fit-in-int`) | 132,710,400 | $2^{30}$ | SAFE ($2^{30}$ < INT_MAX) |
| SYCL sycl/dmabuf_import.cpp:381-383, 445-447 | `tiles_per_row = y_pitch / 128`; `words_per_row = tiles * 32` | `unsigned` | detile geometry | <= 240 tiles | <= 512 tiles | SAFE |
| SYCL sycl/dmabuf_import.cpp:408, 465-466 | `(size_t)(tr * tiles_per_row + tc) * 4096 + ...` | `unsigned` product then `size_t` | tile byte offset; tr <= H/32 | 270*240 = 64,800 tiles | 1024*512 = $2^{19}$ tiles | SAFE (unsigned part <= $2^{19}$) |
| SYCL sycl/dmabuf_import.cpp:411-412, 468-469 | `(size_t)py * row_bytes + ...`; `(size_t)(py + 1) * row_bytes` | `size_t` | linear destination offset | 265,420,800 | $2^{31}$ | SAFE |
| SYCL sycl/dmabuf_import.cpp:536 | `uint32_t y_size = desc.objects[].size` | `uint32_t` (VA-API field type) | DRM object bytes (P010, 1.5 planes) | 398,131,200 | 3,221,225,472 | SAFE (< $2^{32}$) |
| SYCL sycl/dmabuf_import.cpp:633-634, 660 | `y_offset`, `y_pitch` (`uint32_t`), `row_bytes = (size_t)w * bpp` | `uint32_t`, `size_t` | VA layer offsets | <= $2^{29}$ | <= $2^{32}$ | SAFE |
| SYCL sycl/dispatch_strategy.cpp:96 | `(unsigned long)frame_w * (unsigned long)frame_h` | `unsigned long` (32-bit on Windows LLP64) | frame area for heuristic | 132,710,400 | $2^{30}$ | SAFE (< $2^{32}$ even on LLP64) |
| SYCL sycl/scratch_check.cpp:69-99 | probe sums `s += a[...] * (...)`, `s += v[k] * (k + 1)` | `uint32_t` (modular probe values, compared host vs device) | self-test only | n/a | n/a | SAFE (defined unsigned wrap, intentional) |
| Metal metal/picture_metal.mm:30-45; metal/kernel_template.mm:101-122 | `newBufferWithLength:size` | `size_t` -> `NSUInteger` | caller-sized buffers | n/a | n/a | SAFE (64-bit) |
| Metal metal/iosurface_layout.h:118-152 | `element = (size_t)step * bytes`; `(size_t)dst_w * element`; `(size_t)x * rd->step + offset` | `size_t` | IOSurface plane read plan | <= 30,720 | <= 65,536 | SAFE |
| Metal metal/iosurface_layout.h:157-163; metal/picture_import.mm:155-160 | `dst + (size_t)y * dst_stride`, `src + (size_t)y * src_stride` | `size_t` | row offsets | 265,420,800 | ~$2^{31}$ | SAFE |

## SYCL shared headers (`feature/sycl/sycl_*.h`)

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| SYCL sycl_soft_double.h:65-79 (`u128_mul`) | 32-bit limb products, `middle = (low >> 32) + (cross1 & mask) + (cross2 & mask)` | `uint64_t` | operands < $2^{64}$; middle <= 3*($2^{32} - 1$) | middle < $2^{34}$ | same | SAFE (exact 128-bit product) |
| SYCL sycl_soft_double.h:167-190 (`soft_add`) | `sum = (big_mant << 3) + aligned` | `uint64_t` | two significands < $2^{53}$, 3 guard bits | < $2^{57}$ | same | SAFE |
| SYCL sycl_soft_double.h:194-211 (`soft_div`) | `rem <<= 1` (56 steps), `quot` | `uint64_t` | rem < 2 \* b.mant < $2^{54}$ | < $2^{55}$ | same | SAFE |
| SYCL sycl_soft_double.h:234-241 (`soft_trunc`) | `(int64_t)(a.mant << a.exp)` | `uint64_t` -> `int64_t` | precondition value < $2^{63}$; callers: VIF gg_sigma <= $100^{2}$ \* $2^{31}$ = $2^{44.3}$ | < $2^{45}$ | same | SAFE (every caller far below $2^{63}$) |
| SYCL sycl_soft_double.h:314-332 (`soft_sub_trunc`) | `u128_shl(a, frac_bits)`, `a - (uint32_t)(t.mant << t.exp)` | `uint64_t`/128-bit | a < $2^{31}$, frac_bits <= 84 | < $2^{115}$ (in 128 bits) | same | SAFE |
| SYCL sycl_soft_signed.h:67-87, 90-94 | `signed_from_u64` / `signed_from_exact` shifts | `uint64_t` | normalisation shift <= 63 | n/a | n/a | SAFE |
| SYCL sycl_soft_signed.h:153-188 (`signed_add`) | `big_wide + aligned`, `big_wide - aligned` | `uint64_t` | < $2^{57}$ | < $2^{57}$ | same | SAFE |
| SYCL sycl_soft_signed.h:227-245 (`div_step`) | `(rem << 19) - digit * den` then `(int64_t)` | `uint64_t` modular, then `int64_t` | true value within 3 divisors of 0 (< $2^{55}$); the $2^{72}$ intermediates wrap by design | < $2^{55}$ (N-independent) | same | SAFE (intentional modular arithmetic; result exact in 64 bits) |
| SYCL sycl_exact_fp.h:110-115 (`step_float`) | `bits + 1u` / `bits - 1u` on a finite non-zero float | `uint32_t` | adjacent float | n/a | n/a | SAFE |
| SYCL sycl_ordered_sum.h:74-86, 147 | plan / slot codes `(int16_t)value` | `int` -> `int16_t` | binades in [-900, 900], codes PLAN_TERMS+1 .. +128 | fits int16 | same | SAFE |
| SYCL sycl_ordered_sum.h:142 | `out.slot_chunk[slots] = (int32_t)chunk` | `unsigned` -> `int32_t` | chunk index, 512 px per chunk | 259,200 | $2^{21}$ | SAFE |
| SYCL sycl_ordered_sum.h:158-170, 204-223, 229-247 | `stage_run`, `add_slot_chunk`, `walk_sum`: increments through `vmaf_ordsum_then()` (capped at $2^{54}$, ordered_sum.h; prior audit G4b) | `int64_t` | 512 terms per chunk, 16 per run | pre-cap <= $2^{55}$ | same | SAFE (saturating; N-independent) |
| SYCL sycl_ordered_sum.h:208-209, 242-243 | `w.terms + slot * kChunk`, `(size_t)chunk * kChunk` | `size_t` | term index | <= N | <= $2^{30}$ | SAFE |
| SYCL sycl_tile_index.h:30-36 | clamp of a reflected index | `int` | index | <= 2*W | <= 65,536 | SAFE |

`sycl_compat.h`, `sycl_ff_math.h`, `sycl_ciede_math.h`, `sycl_ssim_terms.h` (beyond the soft-signed calls above) and `sycl_float_adm_math.h` / `sycl_float_vif_math.h` (float arithmetic; their integer parts are rows in the feature sections) hold no integer accumulator.

### Metal shared headers (`metal_soft_double.h`, `metal_soft_signed.h`, `metal_portable.h`)

`metal_soft_double.h` and `metal_soft_signed.h` are line-for-line ports of the SYCL headers above (their own comments map every name); the same rows apply.

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| Metal metal_soft_double.h:124-138 (`vmaf_mtl_u128_mul`) | 32-bit limb products and `middle` | `ulong` | as `u128_mul` | middle < $2^{34}$ | same | SAFE |
| Metal metal_soft_double.h:234-300 (`soft_add`, `soft_div`, `soft_mul`) | `(big_mant << 3) + aligned`; restoring division | `ulong` | significands < $2^{53}$ | < $2^{57}$ | same | SAFE |
| Metal metal_soft_double.h:302-310 (`soft_trunc`) | `(long)(mant << exp)` | `ulong` -> `long` | callers' values < $2^{45}$ (VIF gain) | < $2^{45}$ | same | SAFE |
| Metal metal_soft_signed.h:264-284 (`vmaf_mtl_div_step`) | `(rem << 19) - digit * den` (modular), corrected | `ulong` -> `long` | true value within 3 divisors | < $2^{55}$ | same | SAFE (intentional modular arithmetic) |
| Metal metal_portable.h:87-94 | host `vmaf_mtl_clz64` loop | `uint32_t` / `uint64_t` | bit count | <= 64 | same | SAFE |

## psnr (integer)

Per-pixel term: `|ref - dis|^2`, at most $65535^{2}$ = 4,294,836,225 for any uint16 sample. In range (samples <= $2^{\mathrm{bpc}} - 1$) the term is at most $(2^{\mathrm{bpc}} - 1)^{2}$.

### SYCL (`feature/sycl/integer_psnr_sycl.cpp`)

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| SYCL integer_psnr_sycl.cpp:150-158 | `diff = (int32_t)ref - (int32_t)dis`; `magnitude * magnitude` | `int32_t`; `uint32_t` | one term | 4,294,836,225 | same | SAFE (< $2^{32}$) |
| \*\*SYCL integer_psnr_sycl.cpp:168-172, 198 (`psnr_item_sse<uint32_t>`, used for bpc <= 12)\*\* | \*\*`se += psnr_pixel_se(args, off)`\*\* | \*\*`uint32_t`\*\* | \*\*16 terms per work-item. In range: 16 \* $4095^{2}$ = 268,304,400. bpc 9-12 with uint16 codewords above $2^{\mathrm{bpc}} - 1$: up to 16 \* 4,294,836,225 = 6.87e10\*\* | in range 268,304,400; out of range wraps at the 2nd max term | same (frame-size independent) | \*\*DEPENDS\*\* (out-of-range codewords at bpc 9-12: the 32-bit item sum wraps as soon as two of its 16 terms exceed $2^{31}$; CPU `integer_psnr.c:129-135` sums 16-bit rows in uint64, so SYCL then differs from the CPU. 8-bit and 13-16-bit inputs take uint8 terms or the uint64 instantiation and are SAFE) |
| SYCL integer_psnr_sycl.cpp:168-172, 199 (`psnr_item_sse<uint64_t>`, bpc 13-16) | `se += ...` | `uint64_t` | 16 terms | 6.87e10 | same | SAFE |
| SYCL integer_psnr_sycl.cpp:200 | `reduce_over_group(group, se)` | `uint64_t` | 256 items x 16 px | 1.76e13 | same | SAFE |
| SYCL integer_psnr_sycl.cpp:202-204 | `atomic_ref<int64_t>::fetch_add((int64_t)group_se)` (one per work-group) | `int64_t` | frame SSE, N terms | 5.70e17 ($2^{58.98}$) | 4.61e18 (0.50 \* $2^{63}$) | SAFE (zeroed per frame, :221) |
| SYCL integer_psnr_sycl.cpp:170, 189-191 | `off = first + k * stride`; `pixels`, `items`, `global` | `size_t` | item index | <= N | <= $2^{30}$ | SAFE |
| \*\*SYCL integer_psnr_sycl.cpp:550 (`enable_apsnr`)\*\* | \*\*`s->apsnr_sse[p] += sse`\*\* | \*\*`uint64_t`\*\* | \*\*clip SSE: one frame SSE <= N \* $65535^{2}$ per frame\*\* | $2^{64}$ / (N \* 4,294,836,225) = 32.4 frames | 4.0 frames | \*\*DEPENDS\*\* (frames to wrap at 16-bit max-diff: 1080p 2,072, 8K DCI 122, 16K 33, cap 5; at 12 bit 530,502 / 31,085 / 8,290 / 1,025; at 10 bit 8.50M / 498,076 / 132,821 / 16,417; at 8 bit 136.8M / 8.0M / 2.14M / 264,205. Silent wrap, APSNR too high. Same type as CPU `integer_psnr.c:211,249` and CUDA/HIP) |
| SYCL integer_psnr_sycl.cpp:551 | `apsnr_n_pixels[p] += (uint64_t)h * w` | `uint64_t` | clip pixel count | $2^{64}$ / N = 1.4e11 frames | 1.7e10 frames | SAFE |
| SYCL integer_psnr_sycl.cpp:553 | `(double)s->width[p] * (double)s->height[p]` | `double` | pixel count | exact | exact | SAFE |

### Metal (`feature/metal/integer_psnr.metal`, `integer_psnr_metal.mm`)

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| Metal integer_psnr.metal:65-68, 84-89 | `e = r - d` (`long`); `my_se = (ulong)(e * e)` | `long` -> `ulong` | one term | 4,294,836,225 | same | SAFE |
| Metal integer_psnr.metal:46-49 | `group_se += tg_se[i]` (256 threads, thread 0 serial) | `ulong` | 256 terms | 1.10e12 | same | SAFE |
| Metal integer_psnr.metal:65, 84 | `gid.y * strides.x + gid.x` (byte offset; strides = packed `row_bytes`, .mm:314) | `uint` | row byte offset | 265,420,800 | 32767 \* 65536 + 32767 = 2,147,450,879 | SAFE (< $2^{32}$; 2x margin) |
| Metal integer_psnr.metal:70, 91 | slot `bid.y * grid_groups.x + bid.x` | `uint` | threadgroup index | 518,400 | 4,194,304 | SAFE |
| Metal integer_psnr_metal.mm:314 | `(uint32_t)(width * bpp)` | `unsigned` -> `uint32_t` | row bytes | 30,720 | 65,536 | SAFE |
| Metal integer_psnr_metal.mm:368-371 | `sum += parts[i]` | `uint64_t` | frame SSE | 5.70e17 | 4.61e18 | SAFE |
| \*\*Metal integer_psnr_metal.mm:386 (`enable_apsnr`)\*\* | \*\*`s->apsnr_sse[p] += sse`\*\* | \*\*`uint64_t`\*\* | \*\*clip SSE\*\* | 32.4 frames | 4.0 frames | \*\*DEPENDS\*\* (same frames-to-wrap table as the SYCL row above) |
| Metal integer_psnr_metal.mm:387, 389 | `apsnr_n_pixels += (uint64_t)h * w`; `mse = sse / (w * h)` (`unsigned` product) | `uint64_t`; `unsigned` | count | N | $2^{30}$ | SAFE ($2^{30}$ < $2^{32}$) |

## float_psnr

Term (CPU `float_psnr.c`): `diff * diff` in fp32, in units of 1 / $\mathrm{scaler}^{2}$; at most fl32($65535^{2}$) = 4,294,836,224 for any uint16 sample.

### SYCL (`feature/sycl/float_psnr_sycl.cpp`; host tail `feature/float_psnr_rows.h`)

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| SYCL float_psnr_sycl.cpp:139-142 | `(float)(r - d)` squared, `(uint64_t)square` | `float` -> `uint64_t` | one term | 4,294,836,224 | same | SAFE (< $2^{32}$, any codeword) |
| SYCL float_psnr_sycl.cpp:157 | `reduce_over_group(subgroup, noise)` | `uint64_t` | 32 lanes | 1.37e11 | same | SAFE |
| SYCL float_psnr_sycl.cpp:168-172 | `total += scratch[subgroup_index]`; `partials[workgroup_index] = total` | `uint64_t` | 256 px of one row | 1.10e12 ($2^{40}$) | same | SAFE (no uint32 group sum: the HIP bpc 10/12 defect is absent) |
| SYCL float_psnr_sycl.cpp:171 | `workgroup_index = group(0) * workgroups_x + group(1)` | `size_t` | partial index | 518,400 | 4,194,304 | SAFE |
| SYCL float_psnr_sycl.cpp:197-198 | `(int)item.get_global_id(1/0)` | `int` | pixel coordinates | <= 15,615 | <= 33,023 | SAFE |
| SYCL float_psnr_sycl.cpp:302-304 | `wg_count = wg_count_x * wg_count_y` | `unsigned` | partial count | 518,400 | 4,194,304 | SAFE |
| SYCL float_psnr_rows.h:39 | `row += segments[...]` | `uint64_t` | one row, W terms | 6.60e13 ($2^{45.9}$) | 1.41e14 ($2^{47}$) | SAFE |

### Metal (`feature/metal/float_psnr.metal`, `metal_float_psnr_math.h`, `float_psnr_metal.mm`)

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| Metal metal_float_psnr_math.h:36-38 | `(vmaf_mtl_u32)(diff * diff)` | `float` -> `uint` | one term | 4,294,836,224 = 0xFFFE0000 | same | SAFE (fits uint exactly) |
| Metal float_psnr.metal:54-58 | `total += scratch[i]` (256 threads, one row segment) | `ulong` | 256 terms | 1.10e12 | same | SAFE |
| Metal float_psnr.metal:75-76, 97-98 | `gid.y * strides.x + gid.x` (packed row bytes, .mm:227) | `uint` | byte offset | 265,420,800 | 2,147,450,879 | SAFE (< $2^{32}$) |
| Metal float_psnr_metal.mm:148, 258 | `partials_count = per_row * h`; `vmaf_float_psnr_row_noise()` | `size_t`; `uint64_t` row sums | as SYCL | 6.60e13 per row | 1.41e14 | SAFE |

## float_moment

Terms: first moment <= 65535; second moment `moment_float_square(v)` = fl32(v*v) <= 4,294,836,224. The past-$2^{53}$ ordered-sum machinery is the shared `float_moment_sum.h` (prior audit G3b, SAFE) or its Metal port.

### SYCL (`feature/sycl/integer_moment_sycl.cpp`)

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| SYCL integer_moment_sycl.cpp:98-103 | `moment_float_square(v)`: `(int64_t)(sample * sample)` | `float` -> `int64_t` | one term | 4,294,836,224 | same | SAFE |
| SYCL integer_moment_sycl.cpp:149-150 | `atomic_ref<int64_t>(e_sums[0..1]).fetch_add(r / d)` (one atomic per pixel) | `int64_t` | N terms <= 65535 | 8.70e12 ($2^{42.98}$) | 7.04e13 ($2^{46}$) | SAFE |
| SYCL integer_moment_sycl.cpp:151-152 | `fetch_add(moment_float_square(r / d))` | `int64_t` | N terms <= 4,294,836,224 | 5.70e17 ($2^{58.98}$) | 4.61e18 (0.50 \* $2^{63}$) | SAFE (zeroed per frame, :411) |
| SYCL integer_moment_sycl.cpp:136 | `off = y * (size_t)e_w + x` | `size_t` | pixel index | N | $2^{30}$ | SAFE |
| SYCL integer_moment_sycl.cpp:181-195, 203-232, 237-276, 336-389 | row totals / plans / units / ordered walk via `float_moment_sum.h` + `float_moment_sum_gpu.h` (`vmaf_moment_sum_*`, units capped at $2^{54}$) | `uint64_t` / `int64_t` | as prior G3b shared rows | row < $2^{46}$; frame < $2^{59}$ | row < $2^{47}$; frame < $2^{62}$ | SAFE |
| SYCL integer_moment_sycl.cpp:194, 215, 245 | `(size_t)plane * a.height + row`, `(size_t)2u * at` | `size_t` | row buffers | < $2^{16}$ | < $2^{18}$ | SAFE |
| SYCL integer_moment_sycl.cpp:365 | `rounds = h + h / 256 + 2` | `unsigned` | loop bound | 8,675 | 32,898 | SAFE |
| SYCL integer_moment_sycl.cpp:461-466 | `rows = 2 * h`; `rows * 2 * sizeof(int64_t)` | `size_t` | buffers | 276 KB | 1 MiB | SAFE |

### Metal (`feature/metal/float_moment.metal`, `metal_float_moment_math.h`, `metal_float_moment_sum.h`, `float_moment_metal.mm`)

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| Metal metal_float_moment_math.h:31-35 | `(vmaf_mtl_u32)(sample * sample)` | `float` -> `uint` | one term | 4,294,836,224 | same | SAFE |
| Metal float_moment.metal:120-125 (8 bpc) | `rv * rv` | `ulong` | one term <= 65,025 | 65,025 | same | SAFE |
| Metal float_moment.metal:120-121, 154-155 | `(int)gid.y * (int)strides.x + (int)gid.x`; `ref + (int)gid.y * (int)strides.x` (packed row bytes, .mm:408-414) | `int` | byte offset; 16 bpc row bytes = 2W | 265,390,080 | 32767 \* 65536 = 2,147,418,112 | SAFE (< INT32_MAX by 65,535: no headroom for a padded stride, but the host always packs) |
| Metal float_moment.metal:86-99 | `wg_r1/d1/r2/d2 += tg_*[i]`, written as lo/hi `uint` pairs | `ulong` | 256 terms | r2: 1.10e12 | same | SAFE |
| Metal float_moment.metal:186-211 | `a0..a3 += (hi << 32) \| lo`; tree `tg[] += tg[]` | `ulong` | N terms | 5.70e17 | 4.61e18 | SAFE |
| Metal float_moment.metal:132, 168 | `idx = bid.y * grid_groups.x + bid.x` | `uint` | group index | 518,400 | 4,194,304 | SAFE |
| Metal float_moment.metal:218-380; metal_float_moment_sum.h:96-130, 276-300, 376-400 | row totals, plans, units (`vmaf_mtl_os_then`, capped at $2^{54}$), ordered walk: the Metal port of `float_moment_sum.h` | `ulong` / `long` | as the shared header | row < $2^{46}$ | row < $2^{47}$ | SAFE |
| Metal metal_float_moment_sum.h:149-153 | `(uint64_t)w * (uint64_t)h > (2^53 >> 2bpc)` | `uint64_t` | N | N | $2^{30}$ | SAFE |
| Metal float_moment_metal.mm:213, 451-455 | `partials_count = grid_w * grid_h`; host `sum[k] += reconstruct_partial()` | `size_t`; `uint64_t` | frame sums | 5.70e17 | 4.61e18 | SAFE |

## ciede

Per-pixel float term, no integer accumulator; the frame sum is `ciede_frame_sum()` in double. Rows are index math.

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| SYCL integer_ciede_sycl.cpp:172-175 | `off_y = y * (size_t)width + x`; `off_c = cy * (size_t)chroma_w + cx` | `size_t` | sample index | N | $2^{30}$ | SAFE |
| SYCL integer_ciede_sycl.cpp:197 | `terms[id[0] * (size_t)width + id[1]]` | `size_t` | term index | N | $2^{30}$ | SAFE |
| SYCL integer_ciede_sycl.cpp:227, 239, 247 | `row_bytes = plane_w * bpp`; `row_bytes * plane_h`; `(size_t)w * h * sizeof(float)` | `size_t` | buffers | 530,841,600 B | $2^{32}$ B | SAFE |
| SYCL integer_ciede_sycl.cpp:381 | `de00_sum / (s->width * s->height)` | `unsigned` product -> `double` | pixel count | 132,710,400 | $2^{30}$ | SAFE |
| SYCL and Metal ff_math.h:385/387, 423/432, 465 (via sycl_ff_math.h / sycl_ciede_math.h; Metal via metal_ciede_math.h, MSL subset) | `(int)k` of `rint(x / ln2)`; `(int)k & 31`; `(size_t)(int)j * 2` | `float` -> `int` | exp exponent of bounded CIEDE arguments; sin/cos for \|x\| < 400 (k <= 2037); atan j in [0,16] | small | small | SAFE |
| Metal integer_ciede.metal:74 | `i = gid.y * dim.x + gid.x` | `uint` | pixel index | N | $2^{30}$ | SAFE |
| Metal integer_ciede_metal.mm:179-187 | `in_buf += in_row_step * in_stride_t` | `unsigned * ptrdiff_t` | chroma upscale rows | ~$2^{28}$ | ~$2^{31}$ | SAFE |
| Metal integer_ciede_metal.mm:278-279 | `(size_t)w * h`; `de00_sum / (w * h)` (`unsigned`) | `size_t`; `unsigned` | count | N | $2^{30}$ | SAFE |

## motion, motion_v2 (integer SAD) and float_motion

Derivation (same as prior G3a): taps {3571, 16004, 26386, 16004, 3571}, sum $2^{16}$. Vertical `v = (sum f_k d_k + 2^(bpc-1)) >> bpc`; |sum| <= 65536 \* |d|max. In range |d| <= $2^{\mathrm{bpc}} - 1$, so |sum| <= 65536 \* ($2^{\mathrm{bpc}} - 1$); for any uint16 sample |d| <= 65535 and |sum| <= 4,294,901,760. Horizontal `h = (sum f_k v_k + 2^15) >> 16`; |h| <= 65535 in range, <= 8,388,480 for out-of-range bpc 9 input. Frame SAD in range <= N \* 65535: 16K 8.70e12, cap 7.04e13. SAD accumulators are cleared every frame; no clip-level integer sum.

### SYCL (`integer_motion_pipeline_sycl.cpp`, shared by `integer_motion_sycl.cpp` and `integer_motion_v2_sycl.cpp`)

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| SYCL integer_motion_pipeline_sycl.cpp:89-90 | `diff = read_sample(prev) - read_sample(cur)` | `int32_t` | one difference | 65535 | same | SAFE |
| \*\*SYCL integer_motion_pipeline_sycl.cpp:99-103, 224-225 (`vertical_tap<int32_t>`, chosen for bpc <= 15 by `kInt32VerticalMaxBpc`, :46)\*\* | \*\*`sum = (Acc)f0 * (d0 + d4) + (Acc)f1 * (d1 + d3) + (Acc)f2 * d2`; `(sum + round) >> bpc`\*\* | \*\*`int32_t` (Acc)\*\* | \*\*5 taps. In range (bpc 15): 65536 \* 32767 + 16384 = 2,147,434,496, 49,151 below INT32_MAX. bpc 9-15 with uint16 codewords above $2^{\mathrm{bpc}} - 1$: \|sum\| up to 65536 \* 65535 = 4,294,901,760; the first two products alone reach 3571\*131070 + 16004\*131070 = 2.57e9\*\* | in range <= 2,147,434,496; out of range up to 4.29e9 | same (frame-size independent) | \*\*DEPENDS\*\* (out-of-range codewords at bpc 9-15: signed int32 overflow (UB) in the vertical sum. The CPU `integer_motion.c:226-231` uses int64 for every bpc > 8 and CUDA's 16 bpc kernel int64, so SYCL then differs from both. bpc 16 takes `int64_t`, SAFE; bpc 8 reads uint8, SAFE) |
| SYCL integer_motion_pipeline_sycl.cpp:99-103 (`vertical_tap<int64_t>`, bpc 16) | same sum | `int64_t` | 5 taps | 4,294,901,760 | same | SAFE |
| SYCL integer_motion_pipeline_sycl.cpp:125-128 | `sum = (int64_t)f0 * (v0 + v4) + ...`; `h = (sum + 32768) >> 16`; `\|h\|` | `int64_t` (the `v0 + v4` pairs add in `int32_t`) | 5 taps of v <= 65535 (in range) | <= 4,294,901,760 before the shift; \|h\| <= 65535 | same | SAFE (in range; the v pairs reach int32 limits only after the DEPENDS row above has already overflowed) |
| SYCL integer_motion_pipeline_sycl.cpp:135 | `reduce_over_group(subgroup, value)` | `int64_t` | 32 (or 16) lanes | 2,097,120 | same | SAFE |
| SYCL integer_motion_pipeline_sycl.cpp:141-149 | `total += scratch[i]`; `atomic_ref<int64_t>::fetch_add(total)` (one per work-group) | `int64_t` | frame SAD, N terms | 8.70e12 (out-of-range bpc 9: 1.11e15) | 7.04e13 (9.0e15) | SAFE (< $2^{53}$; zeroed per frame, integer_motion_sycl.cpp:526, integer_motion_v2_sycl.cpp:330) |
| SYCL integer_motion_pipeline_sycl.cpp:74-88 | `tile_y = (int)(group(0) * 8) - 2`; `vmaf_sycl_tile_index(reflect_101(y, H), H)`; `offset = (size_t)y * width + x` | `int`; `size_t` | tile index, clamped | <= 8,641 | <= 32,769 | SAFE (reflect then clamp: no out-of-plane read for any W, H >= 3) |
| SYCL integer_motion_pipeline_sycl.cpp:155, 160-161 | `plane_bytes = (size_t)w * h * bpp`; global ranges | `size_t` | buffers | 265,420,800 | $2^{31}$ | SAFE |
| SYCL integer_motion_sycl.cpp:266, integer_motion_v2_sycl.cpp:257 | reject `w < 3 \|\| h < 3` | `unsigned` | guard | n/a | n/a | SAFE |
| SYCL integer_motion_sycl.cpp:725-733 | `(double)sad / 256.0 / ((double)w * h)` | `int64_t` -> `double` | frame SAD | exact (< $2^{53}$) | exact | SAFE |
| SYCL integer_motion_sycl.cpp:477, 810; integer_motion_v2_sycl.cpp:320-321, 343 | `frame_index++`; `index % ring`, `(index + 1u) % ring`; `frame_index = index + 1u` | `unsigned` | frame counter | wraps at $2^{32}$ frames | same | SAFE (the framework's own `unsigned index` limit; no `(int)index` narrowing as in CUDA row 11) |

### Metal (`integer_motion.metal`, `metal_integer_motion_math.h`, `integer_motion_metal.mm`; `integer_motion_v2.metal`, `integer_motion_v2_metal.mm`)

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| Metal metal_integer_motion_math.h:56-61 | vertical sum, `(int32)((sum + 2^(bpc-1)) >> bpc)` | `long` -> `int` | 5 taps | sum <= 4,294,901,760; result <= 65535 (in range), <= 8,388,480 (bpc 9 out of range) | same | SAFE (int64 for every bpc) |
| Metal metal_integer_motion_math.h:67-75 | horizontal sum; `val = (int32)((sum + 32768) >> 16)`; `\|val\|` | `long` -> `int` -> `uint` | 5 taps | \|h\| <= 65535 (8,388,480 out of range) | same | SAFE |
| Metal integer_motion.metal:88-94 | `total += scratch[i]` (thread 0, 256 terms) | `uint` | 256 px of \|h\| | 16,776,960 (out of range 2,147,450,880) | same | SAFE (< $2^{32}$ even for out-of-range input) |
| Metal integer_motion.metal:60-66 | `vmaf_mtl_motion_mirror` (clamp to [-2, size+1], one reflection); `(uint)(y * width + x)` | `int` | sample index | N | $2^{30} - 1$ | SAFE (W, H >= 3 enforced, integer_motion_metal.mm:260) |
| Metal integer_motion_metal.mm:358-363 | `sad += parts[i]`; `sad / 256. / (w * h)` | `uint64_t` | frame SAD | 8.70e12 | 7.04e13 | SAFE |
| Metal integer_motion_metal.mm:275, 347-349 | `partials_count = ceil(w/16) * ceil(h/16)`; `index % ring`, `(index + 1u) % ring` | `size_t`; `unsigned` | n/a | 518,400 | 4,194,304 | SAFE |
| Metal integer_motion_v2.metal:149-154 (8 bpc) | `blurred_y += MV2_FILTER[yf] * s_diff[...]` | `int` | 5 taps \* 255 | 16,711,680 | same | SAFE |
| Metal integer_motion_v2.metal:240-245 (16 bpc) | `blurred_y += (long)f * (long)d`; `v = (int)((blurred_y + round_y) >> bpc)` | `long` -> `int` | 5 taps | 4.29e9 -> v <= 65535 (8,388,480 out of range) | same | SAFE |
| Metal integer_motion_v2.metal:156-158, 247-249 | `blurred += (long)f * v`; `abs_h = (uint)\|h\|` | `long` -> `uint` | 5 taps | 65535 | same | SAFE |
| Metal integer_motion_v2.metal:164, 172, 253, 261 | `simd_sum(abs_h)`; `group_sum += simd_partials[i]` | `uint` | 32 lanes; 256 px | 16,776,960 (out of range 2,147,450,880) | same | SAFE |
| Metal integer_motion_v2.metal:132, 224 | `gy * prev_stride` (packed row bytes, 2W at 16 bpc) | `int` | byte offset | 265,390,080 | 2,147,418,112 | SAFE (< INT32_MAX by 65,535) |
| Metal integer_motion_v2.metal:76-83 | `mv2_mirror` (loop until inside) | `int` | index | in [0, sup) | same | SAFE |
| Metal integer_motion_v2_metal.mm:286, 405-409, 434 | `partials_count`; `sum += partials[i]`; `sad / 256. / (w * h)` | `size_t`; `uint64_t`; `unsigned` | frame SAD | 8.70e12 | 7.04e13 | SAFE |

### float_motion: SYCL (`float_motion_sycl.cpp`) and Metal (`float_motion.metal`, `metal_float_motion_math.h`, `float_motion_metal.mm`)

fp32 blur and fp32 row sums (never converted to an integer); rows are index math and the small-frame mirror (known item 8).

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| SYCL float_motion_sycl.cpp:130-137, 190-194 | `dev_mirror_fm()` (one reflection, may leave the plane on a small frame) then `vmaf_sycl_tile_index()` clamp to [0, extent-1] | `int` | tile index for W, H in 3..17 | in range | in range | SAFE (clamped: the CUDA `fm_mirror` out-of-bounds read, prior A1, is absent; W, H >= 3 enforced at :432) |
| SYCL float_motion_sycl.cpp:161, 242 | `(size_t)y * width + x` | `size_t` | pixel index | N | $2^{30}$ | SAFE |
| SYCL float_motion_sycl.cpp:256-257 | `args.cur_blur + y * args.width` (`size_t y`) | `size_t` | row base | N | $2^{30}$ | SAFE |
| SYCL float_motion_sycl.cpp:398-404 | `plane_bytes`, `blur_bytes = (size_t)w * h * 4`, `sad_bytes = (size_t)h * 4` | `size_t` | buffers | 530,841,600 | $2^{32}$ | SAFE |
| Metal metal_float_motion_math.h:181-189 | `vmaf_mtl_fm_reflect101`: `\|idx\| % (2 * (size - 1))`, then fold | `int` | any index -> [0, size-1] | in range | in range | SAFE (periodic reflection: in range for every size >= 2; no out-of-plane read) |
| Metal float_motion.metal:109 | `(uint)(sy * width + sx)` | `int` | sample index | N | $2^{30} - 1$ | SAFE |
| Metal float_motion.metal:124; metal_float_motion_math.h:219-225 | `off = gid.y * width + gid.x`; `vmaf_mtl_fm_diff_index`: `((y/64 * width) + x) * 64 + y%64` | `uint` | blur / diff index (rows padded to 64) | 132,710,400 | $2^{30}$ | SAFE (< $2^{32}$; the host also rejects diff_count > INT32_MAX, float_motion_metal.mm:283) |
| Metal float_motion_metal.mm:283, 395-408, 597-599 | guard `vmaf_mtl_fm_diff_count(w, h) > INT32_MAX`; buffers `(size_t)w * h`; staging `(size_t)y * row_bytes` | `uint64_t`; `size_t` | diff plane (rows padded to 64) | 132,710,400 | $2^{30}$ | SAFE (guard keeps every uint diff index below $2^{31}$) |
| Metal float_motion.metal:76-82 | `(uint)at.y1 * width + (uint)at.x1` (scale-1 bilinear corners) | `uint` | index | N | $2^{30}$ | SAFE |
| Metal float_motion.metal:165-169 | row walk `base + j * 64` | `uint` | diff index | N | $2^{30}$ | SAFE |

## ssim (integer), float_ssim, float_ms_ssim

Term maxima for integer SSIM (prior G4a): 9 taps {2,9,28,55,68,55,28,9,2}, sum 256. Horizontal mux/muy <= 256 \* 65535 = 16,776,960; x2/xy/y2 <= 256 \* $65535^{2}$ = 1.10e12. Vertical mux <= 65536 \* 65535 = 4,294,901,760 = $2^{32}$ - $2^{16}$; x2 <= 65536 \* $65535^{2}$ = 2.81e14; w <= 65536. These hold for any uint16 sample. Products: $\mathrm{mux}^{2}$ <= ($2^{32}$ - $2^{16}$)^2 = $2^{64}$ - $2^{49}$ + $2^{32}$; x2 \* w <= $2^{32}$ \* $65535^{2}$ = the same value. Both are below $2^{64}$ by $2^{49}$ - $2^{32}$.

### SYCL integer SSIM (`integer_ssim_sycl.cpp` second half, `sycl_integer_ssim_math.h`)

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| SYCL integer_ssim_sycl.cpp:1242-1249, 1294-1301 | `mux += wk * s`; `x2 += wk * s * s` (all int64 operands) | `int64_t` | 9 taps | mux 16,776,960; x2 1.10e12 | same | SAFE |
| SYCL integer_ssim_sycl.cpp:1242-1243, 1251 | `d_ref[(size_t)y * e_width + (unsigned)src_x]`; `idx = (size_t)y * e_width + x` | `size_t` | pixel index | N | $2^{30}$ | SAFE |
| SYCL integer_ssim_sycl.cpp:1385-1396 | vertical `reference_mean += coefficient * args.reference_mean[index]`, ... | `int64_t` | 9 taps over horizontal moments | mux 4,294,901,760; x2 2.81e14 | same | SAFE |
| SYCL integer_ssim_sycl.cpp:1385-1386 | `source_y = (unsigned)((int)y - 4 + tap)`; `index = (size_t)source_y * width + x` | `unsigned`, `size_t` | taps start at `first` so source_y >= 0 | N | $2^{30}$ | SAFE |
| SYCL integer_ssim_sycl.cpp:1403 | `w = (uint64_t)(row_weight * tap_weight(...))` | `int64_t` | 256 \* 256 | 65,536 | same | SAFE |
| SYCL sycl_integer_ssim_math.h:121-126 | `mux * mux`, `mux * muy`, `x2 * w`, `xy * w`, `y2 * w` | `uint64_t` | moment products | <= $2^{64}$ - $2^{49}$ + $2^{32}$ | same | SAFE (no wrap; $2^{49}$ margin) |
| SYCL sycl_integer_ssim_math.h:176-183 | `2u * covariance`, `p.mx2 + p.my2`, `p.x2w - p.mx2 + p.y2w - p.my2` | `uint64_t` | exact path: the result is used only when every product < $2^{52}$, then all < $2^{53}$; otherwise the computed value is discarded (line 192-195) | < $2^{53}$ when used | same | SAFE (unsigned wrap of the discarded value is defined) |
| SYCL integer_ssim_sycl.cpp:1432 | `terms[id[0] * (size_t)width + id[1]]` | `size_t` | term index | N | $2^{30}$ | SAFE |
| SYCL integer_ssim_sycl.cpp:1449-1454, 1482 | `line_weight()` = sum of tap weights; `total_weight = line_weight(W) * line_weight(H)` | `int64_t` | <= 256 \* extent each; frame weight sum | 8.70e12 | 7.04e13 | SAFE |

### SYCL float_ssim (`integer_ssim_sycl.cpp` first half) and float_ms_ssim (`integer_ms_ssim_sycl.cpp`)

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| \*\*SYCL integer_ssim_sycl.cpp:269-273, 290-302, 625-641 (`decimate_fixed`, `decimate_sample`)\*\* | \*\*`static_cast<int64_t>(product * 0x1p52f)`; `sum += decimate_fixed(product)` over the scale x scale window\*\* | \*\*`float` -> `int64_t`; `int64_t` sum\*\* | \*\*product = (raw \* sample_scale) \* (1 / $\mathrm{scale}^{2}$), scale >= 2 when decimating. In range the window sum is the mean of samples <= 255.996, so <= $2^{60}$ (prior G4a). Out-of-range codewords: sample_scale is 1/4 (bpc 10) or 1/16 (bpc 12) and raw can be 65535, so the mean reaches 16,383.75 (bpc 10) or 4,095.94 (bpc 12)\*\* | in range < $2^{60}$; out of range up to 16,383.75 \* $2^{52}$ = $2^{66}$ (bpc 10) or $2^{64}$ (bpc 12) | same (frame-size independent) | \*\*DEPENDS\*\* (out-of-range codewords: the int64 window sum overflows once the window mean of sample/scaler reaches 2048, i.e. raw >= 8192 at bpc 10 or raw >= 32768 at bpc 12; at bpc 10 and scale 2 a single product (>= 2048 \* $2^{52}$) already makes the float-to-int64 conversion undefined. The CPU forms this sum in double and does not overflow. The CUDA (`ssim_score.cu:317`, `__float2ll_rz`, saturating, then a wrapping int64 sum) and HIP (`ssim_decimate.h:89`, `(int64_t)` cast) twins have the same exposure; prior G4a assumed in-range samples. bpc 8 and 16 are SAFE: their samples cannot exceed 255.996) |
| SYCL integer_ssim_sycl.cpp:258-266 | `symmetric_index`: `position % (2 * extent)` | `int` | extent <= 32768 | in range | in range | SAFE |
| SYCL integer_ssim_sycl.cpp:329-331 | `centre_x = (int)x * scale`; `index = y * output_width + x` | `int`; `size_t` | decimated coordinates times scale <= W | <= 15,360 | <= 32,768 | SAFE |
| SYCL integer_ssim_sycl.cpp:359-367, 388, 410, 451, 495, 515-516 | tile and plane indices | `size_t` | n/a | <= N | <= $2^{30}$ | SAFE |
| SYCL integer_ssim_sycl.cpp:546, 557 | `round_to_int((float)min(w, h) / 256.0f)` | `float` -> `int` | scale <= 128 (geometry check :567-571) | 34 | 128 | SAFE |
| SYCL integer_ssim_sycl.cpp:694-711 | buffer sizes `(size_t)w * h * bytes`, `(enable_lcs ? 2 : 1) * windows * 8` | `size_t` | n/a | 1.06e9 B | $2^{34}$ B | SAFE |
| SYCL sycl_ssim_terms.h:196-204 | `signed_mul` / `signed_from_float` (lv \* cv \* sv as fp64 bits) | soft fp64 in `uint64_t` | per window | n/a | n/a | SAFE (see shared soft-double rows) |
| SYCL integer_ms_ssim_sycl.cpp:222-230 | `source[y * (int)width + x]` (scale-0 float plane, full resolution) | `int` | element index | 132,710,399 | 32767 \* 32768 + 32767 = $2^{30} - 1$ | SAFE (< INT32_MAX, 2x margin) |
| SYCL integer_ms_ssim_sycl.cpp:252, 291, 305, 358, 383 | other plane and window indices | `size_t` | n/a | <= N | <= $2^{30}$ | SAFE |
| SYCL integer_ms_ssim_sycl.cpp:446, 480 | `min_dimension = 11 << 4`; `peak = (1u << bpc) - 1u` | `unsigned` | n/a | 176; 65535 | same | SAFE |
| SYCL integer_ms_ssim_sycl.cpp:533-547 | `window_count += (size_t)w_f * h_f` over planes and scales; `2 * window_count * 8` | `size_t` | windows of every (plane, scale) | 528,929,700 (4:4:4) | 4,286,965,212 | SAFE (size_t) |

### Metal integer SSIM (`integer_ssim.metal`, `metal_integer_ssim_math.h`, `integer_ssim_metal.mm`)

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| Metal metal_integer_ssim_math.h:137-155 | horizontal / vertical taps `acc + coefficient * s * s`, `acc + coefficient * row.x2` | `long` | 9 + 9 taps | mux 4,294,901,760; x2 2.81e14 | same | SAFE |
| Metal metal_integer_ssim_math.h:220-224 | `m.mux * m.mux`, `m.x2 * m.w`, ... | `ulong` | moment products | <= $2^{64}$ - $2^{49}$ + $2^{32}$ | same | SAFE ($2^{49}$ margin) |
| Metal metal_integer_ssim_math.h:273-284 | exact path, evaluated only when every product < $2^{52}$ | `ulong` | n/a | < $2^{53}$ | same | SAFE |
| \*\*Metal integer_ssim.metal:74-80, 97, 115, 131-138\*\* | \*\*`hbuf[k * plane + index]` for k = 0..4, `plane = params.x * params.y` (W \* H)\*\* | \*\*`uint` (32-bit) index arithmetic\*\* | \*\*five int64 moment planes of N each; largest index 4N + N - 1 = 5N - 1\*\* | 663,551,999 (0.15 \* $2^{32}$) | 5 \* $2^{30} - 1$ = 5.37e9 | \*\*OVERFLOW@CAP-ONLY\*\* (the index wraps once 5N > $2^{32}$, i.e. N >= 858,993,460, for example 29,310 x 29,310 or 32768 x 26,215; the y2 plane (k = 4) then aliases the start of the buffer and reads and writes the wrong moments. The host sizes the buffer in size_t (`integer_ssim_metal.mm:223`, 34 GB at that N) and checks only `w, h >= 1`, so on a device whose maxBufferLength admits it the wrap is silent) |
| Metal integer_ssim.metal:96, 112 | `ref + gid.y * params.z` (packed row bytes) | `uint` | byte offset | 265,390,080 | 2,147,418,112 | SAFE (< $2^{32}$) |
| Metal integer_ssim.metal:97, 115, 135, 144 | `gid.y * params.x + gid.x`, `(uint)((int)gid.y - 4 + tap) * width + gid.x` | `uint` | plane index | N | $2^{30}$ | SAFE |
| Metal integer_ssim_metal.mm:135-140, 204 | `issim_line_weight()`; `total_weight = lw(W) * lw(H)` | `int64_t` | frame weight | 8.70e12 | 7.04e13 | SAFE |
| Metal integer_ssim_metal.mm:146-152, 215-223 | double term sum; `pixels * sizeof(uint64_t)`, `5 * pixels * 8` | `double`; `size_t` | n/a | 5.3e9 B | 4.3e10 B | SAFE (size_t) |

### Metal float_ssim (`float_ssim.metal`, `metal_ssim_terms.h`, `float_ssim_metal.mm`) and float_ms_ssim (`float_ms_ssim.metal`, `metal_ms_ssim_math.h`, `float_ms_ssim_metal.mm`, `float_ms_ssim_option_semantics.h`)

Both twins take float planes from `picture_copy()` on the host (no fixed-point decimation), so the SYCL decimation row above has no Metal counterpart.

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| \*\*Metal float_ssim.metal:80-86, 94-100\*\* | \*\*`hbuf[k * plane + out]`, `plane = w_h * height` with w_h = W - 10 (scale 1 only)\*\* | \*\*`uint` index\*\* | \*\*five float moment planes; largest index 5 \* plane - 1\*\* | 5 \* 15350 \* 8640 - 1 = 663,119,999 | 5 \* 32758 \* 32768 - 1 = 5.37e9 | \*\*OVERFLOW@CAP-ONLY\*\* (wraps once (W - 10) \* H >= 858,993,460. Reachable only with the `scale=1` option pinned: the auto scale is >= 2 for min(W, H) >= 384 and init then refuses with -ENOTSUP, `float_ssim_metal.mm:203-209`) |
| Metal float_ssim_metal.mm:145-159, 203-209, 238-256 | `ssim_metal_compute_scale()` (`(int)(min / 256 + 0.5)`); scale must be 1; buffers `5u * (size_t)w_h * frame_h * 4`, `windows * 8` | `int`; `size_t` | scale and buffer sizes | 34 (auto, refused) / 1 (pinned) | 128 / 1 | SAFE (sizes in size_t; the uint index in the kernel is the CAP-ONLY row above) |
| Metal float_ssim.metal:75, 98, 118, 134 | `gid.y * width + gid.x + tap`; `(y + tap) * w_h + x`; `params.offset + gid.y * final_width + gid.x` | `uint` | plane index | N | $2^{30}$ | SAFE |
| \*\*Metal float_ms_ssim.metal:115-126, 142-148\*\* | \*\*`hbuf[k * plane + out]`, `plane = w_h * height` at scale 0 (w_h = W - 10)\*\* | \*\*`uint` index\*\* | \*\*five float moment planes of the full-resolution scale; largest index 5 \* plane - 1\*\* | 663,119,999 | 5.37e9 | \*\*OVERFLOW@CAP-ONLY\*\* (default options: MS-SSIM always runs scale 0 at full resolution, so the index wraps once (W - 10) \* H >= 858,993,460, e.g. 32768 x 26,223; the host buffer is size_t, `float_ms_ssim_metal.mm:276-278`, 21.5 GB there) |
| Metal float_ms_ssim.metal:76, 93 | decimation `src[gid.y * width + (uint)xi]`, `tmp[(uint)yi * output_width + gid.x]` | `uint` | element index | N | $2^{30}$ | SAFE |
| Metal float_ms_ssim.metal:154 | `index = params.offset + gid.y * final_width + gid.x` | `uint` | window index over every (plane, scale) region | 528,929,700 (4:4:4 with enable_chroma) | 4,286,965,212 (0.998 \* $2^{32}$) | SAFE (below $2^{32}$ by 8.0e6 at the cap; luma only 1.43e9) |
| Metal float_ms_ssim_metal.mm:371-372 | `window_offset[i] = (unsigned)s->window_count` | `size_t` -> `unsigned` | first window of each (plane, scale) | 528,426,200 | 4,282,811,768 | SAFE (< $2^{32}$; zero headroom beyond the cap) |
| Metal metal_ms_ssim_math.h:58-62, 78-81 | `msdec` mirror `% (2 * n)`; extent `n/2 + (n & 1)` | `int`; `uint` | index / extent | <= 2W | <= 65,536 | SAFE |
| Metal float_ms_ssim_option_semantics.h:55 | `peak = (1u << bpc) - 1u` | `unsigned` | n/a | 65535 | same | SAFE |
| Metal metal_ssim_terms.h:320-375 | host double sums of the terms | `double` | window terms | n/a (floating point) | n/a | SAFE (no integer) |

## ssimulacra2

The six per-pixel sums are fp64 values formed in integers (SYCL) or summed in double on the host (Metal). The SYCL chunk increments go through the shared `ordered_sum.h` composition, capped at $2^{54}$ (prior G4b, N-independent). Chunk = 512 pixels (SYCL).

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| SYCL ssimulacra2_sycl.cpp:393 | `peak = (float)((1u << bpc) - 1u)` | `unsigned` | n/a | 65535 | same | SAFE |
| SYCL ssimulacra2_sycl.cpp:450-453, 1484 | `ss2s_chunks`: `(unsigned)((pixels + 511) / 512)` | `size_t` -> `unsigned` | chunks per plane | 259,200 | $2^{21}$ | SAFE |
| SYCL ssimulacra2_sycl.cpp:476-482 | `ss2s_map`: `(uint64_t)v * plane_dim / luma_dim`; `plane_dim * 2u == luma_dim` | `uint64_t`; `unsigned` | chroma coordinate mapping | < $2^{30}$ | < $2^{30}$ | SAFE |
| SYCL ssimulacra2_sycl.cpp:486-488, 518-523, 610-622, 727-730 | `(size_t)sy * plane_w + sx`; `plane`, `2u * plane + idx`; `c * in_plane`; `(size_t)c * w * h` | `size_t` | plane / channel indices, 3 channels | 3N | 3 \* $2^{30}$ | SAFE |
| SYCL ssimulacra2_sycl.cpp:718-722 | `lines = per_plane * 3`; `(unsigned)get_global_id(0)` | `unsigned` | blur lines | 25,920 | 98,304 | SAFE |
| SYCL ssimulacra2_sycl.cpp:696-707 | IIR `left`, `right` (`int`), `base + (size_t)left * step` | `int`; `size_t` | n/a | <= W + radius | <= 32,768 + radius | SAFE |
| SYCL ssimulacra2_sycl.cpp:909-919 | advice `own.v[f] += pixel.v[f]` | `float` | advice only, never an integer | n/a | n/a | SAFE (no integer) |
| SYCL ssimulacra2_sycl.cpp:932-941 (`plan_chunks`), sycl_ordered_sum.h:132-152 | per-(channel, sum) plan; `(int16_t)value`, `(int32_t)chunk` | `int16_t`, `int32_t` | chunk index | 259,200 | $2^{21}$ | SAFE |
| SYCL ssimulacra2_sycl.cpp:969-985 (`ss2s_ordered_tree`), 1006-1016 (`ss2s_stage_units`), 1135 (`stage_run`) | `vmaf_ordsum_then(...)` composition of int64 increments | `int64_t` | <= 512 terms per chunk, 16 per run; each operand <= $2^{54}$ (capped) | pre-cap <= $2^{55}$ | same | SAFE (saturating at $2^{54}$) |
| SYCL ssimulacra2_sycl.cpp:1010, 1029 | `((size_t)c * SUMS + k) * chunks + chunk`, `* 2u` | `size_t` | plan / units index | < $2^{23}$ | < $2^{25}$ | SAFE |
| SYCL ssimulacra2_sycl.cpp:1155-1166 | `slot = (size_t)which * 128 + group`; `(size_t)chunk * 512 + j` | `size_t` | kept-chunk terms | < N | < $2^{30}$ | SAFE |
| SYCL ssimulacra2_sycl.cpp:1225-1240 (`walk_sum`) | the exact fp64 sum carried as a bit pattern | `uint64_t` (fp64 bits) | per (scale, channel, sum) | n/a (floating point in integers) | n/a | SAFE |
| SYCL ssimulacra2_sycl.cpp:1484-1496 | `sums = 3 * 6 * chunks`; `slots * 512 * 8`; `slots * 32 * 4 * 8` | `size_t` | buffers | 4.67e6 entries | 3.8e7 entries | SAFE |
| SYCL sycl_ssimulacra2_math.h:86-95, 102-153 | soft fp64 `quartic`, products, quotients; exponent `int32_t` with the $2^{250}$ guard | `uint64_t` / `int32_t` | per pixel | n/a | n/a | SAFE (exponents stay far inside int32; fourth power guarded below $2^{250}$) |
| Metal ssimulacra2.metal:105 | `idx = base + c * plane_stride` | `uint` | 3 channels, `plane_stride = W * H` (ssimulacra2_metal.mm:786) | 3N - 1 = 398,131,199 | 3 \* $2^{30} - 1$ = 3,221,225,471 | SAFE (< $2^{32}$; 25 % headroom) |
| Metal ssimulacra2.metal:152, 220 | `in_base = c * plane_stride + row * width`; `base + (uint)n * width + col` | `uint` | blur line bases | 3N - 1 | 3,221,225,471 | SAFE |
| Metal ssimulacra2_metal.mm:786, 809 | `plane_stride = s->width * s->height` | `unsigned` -> `uint32_t` | W \* H | 132,710,400 | $2^{30}$ | SAFE |
| Metal ssimulacra2_metal.mm:551-591 | host sums `sum_l1 += d`, `e_art4 += ...` over `scale_pixels` | `double` | n/a | n/a | n/a | SAFE (no integer) |
| Metal ssimulacra2_metal.mm:368-375, 386, 451, 515-529 | host indices `(size_t)sy * stride`, `(size_t)y * w + x`, `(size_t)c * plane_stride` | `size_t` | n/a | <= 3N | <= 3 \* $2^{30}$ | SAFE |

## vif (integer)

Basis (prior G2): every filter row sums to 65536; widths {17, 9, 5, 3}; largest tap 43,728 (scale 3). Scale-0 vertical shift is bpc, square shift 2(bpc - 8); scales 1-3 shift 16. The CPU (`integer_vif.c:205-206, 450-451`) truncates the vertical `mu1`, `mu2` and `ref_convol`/`dis_convol` to `uint16_t` before the horizontal pass; for in-range samples the truncation is a no-op. For bpc 9-15 with codewords above $2^{\mathrm{bpc}} - 1$ the untruncated vertical value reaches (65536 \* 65535 + $2^{\mathrm{bpc}-1}$) >> bpc, i.e. 8,388,480 at bpc 9 (prior rows 9-10). Accumulators are per frame and per scale (memset each frame).

### SYCL (`integer_vif_sycl.cpp`, `sycl_integer_vif_math.h`)

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| SYCL integer_vif_sycl.cpp:452-462, 1145-1160 | `icr = fc * rv`; `acc.mu1 += icr` (separate and fused vertical passes) | `uint32_t` | fw taps, sum of fc = 65536, rv <= 65535 for any uint16 (scales 1-3 read `& 0xFFFF`, :346) | 4,294,901,760 | same | SAFE ($2^{32}$ - 65,536; no wrap even out of range) |
| SYCL integer_vif_sycl.cpp:460-462, 1157-1159 | `acc.ref += (uint64_t)icr * rv` | `uint64_t` | sum of fc \* $v^{2}$ | 2.81e14 | same | SAFE |
| SYCL integer_vif_sycl.cpp:353-358, 492-494, 1205-1209 | `dev_quantize_sq`: `(uint32_t)((acc + round) >> shift)` | `uint64_t` -> `uint32_t` | in range <= 4,294,836,225 | 4,294,836,225 | same | SAFE (out-of-range values wrap mod $2^{32}$ with the same cast as CPU `integer_vif.c:452-455`, so CPU and SYCL agree) |
| SYCL integer_vif_sycl.cpp:497-505, 1162-1166 | `acc_ref_rd += fc_rd * s_ref[...]` | `uint32_t` | filter[scale+1] taps (sum 65536) \* v <= 65535 | 4,294,901,760 | same | SAFE |
| \*\*SYCL integer_vif_sycl.cpp:490-491 (store), 777-778, 793-794, 828-829 (horizontal); fused 1201-1204 (store), 1239-1240, 1255-1256\*\* | \*\*`tmp_mu1[idx] = (uint32_t)((acc.mu1 + round) >> shift_vp)` with no `(uint16_t)`; then `h_mu1 += fcc * tmp_mu1[ci]`, `h_mu1 += fc * (tmp_mu1[lo] + tmp_mu1[hi])`\*\* | \*\*`uint32_t`\*\* | \*\*horizontal fw taps (sum 65536). In range tmp <= 65535, h_mu1 <= 4,294,901,760. bpc 9-15 with out-of-range codewords: tmp up to 8,388,480 (bpc 9), so h_mu1 up to 65536 \* 8,388,480 = 5.5e11\*\* | in range 4,294,901,760; out of range wraps | same (frame-size independent) | \*\*DEPENDS\*\* (known item 7, confirmed for SYCL: the uint16 truncation CPU `integer_vif.c:450-451` applies is missing in both the separate and the fused path, so the 32-bit horizontal mean sum wraps and the scores differ from the CPU for out-of-range input. Same defect as HIP prior row 9) |
| \*\*SYCL integer_vif_sycl.cpp:504-505 (store), 784-785, 802-803, 837-838; fused 1210-1214, 1246-1247, 1264-1265; 748-753 (`dev_downsample_rd`)\*\* | \*\*`tmp_ref_convol[idx] = (uint32_t)((acc_ref_rd + round) >> shift_vp)` untruncated; `h_ref_rd += fc_rd * tmp_ref_convol[...]`; `(h_ref_rd + 32768) >> 16` then `& 0xFFFF`\*\* | \*\*`uint32_t`\*\* | \*\*as the row above, for the next scale's decimated plane\*\* | in range 4,294,934,528 after the rounding add; out of range wraps | same | \*\*DEPENDS\*\* (out-of-range codewords at bpc 9-15 only: CPU `integer_vif.c:205-206` truncates `ref_convol` to uint16 first; the later `& 0xFFFF` does not restore the CPU value because the 32-bit sum already wrapped. Same as HIP prior row 10) |
| SYCL integer_vif_sycl.cpp:779-781, 795-797, 830-832, 1241-1243, 1257-1259 | `h_ref += (uint64_t)fc * tmp_ref[...]` | `uint64_t` | 65536 \* 4,294,836,225 | 2.81e14 | same | SAFE |
| SYCL integer_vif_sycl.cpp:654-656 | `xx_filt = (uint32_t)((h_ref + 32768) >> 16)` | `uint64_t` -> `uint32_t` | n/a | 4,294,836,225 | same | SAFE |
| SYCL integer_vif_sycl.cpp:658-663 | `((uint64_t)mu1 * mu1 + 2^31) >> 32` | `uint64_t` | mu <= 4,294,901,760 | $2^{64}$ - $2^{49}$ + $2^{32}$ + $2^{31}$ | same | SAFE ($2^{49}$ margin) |
| SYCL integer_vif_sycl.cpp:665-667 | `(int32_t)(xx_filt - mu1_sq)` | `uint32_t` -> `int32_t` | true value is a (co)variance in [-$2^{30}$, $2^{30}$] for in-range input | fits int32 | same | SAFE (modular conversion, two's complement on every SYCL target) |
| SYCL integer_vif_sycl.cpp:614, 624-626 | `(uint32_t)(SIGMA_NSQ + sigma1_sq)`; `gain.sv_sq + SIGMA_NSQ`; `(uint64_t)gg_sigma + numer1` | `uint32_t`; `uint64_t` | sigma1_sq < $2^{31}$; gg_sigma <= $100^{2}$ \* $2^{31}$ | $2^{31}$ + $2^{17}$; < $2^{44.3}$ | same | SAFE |
| SYCL integer_vif_sycl.cpp:616, 621, 630-636 | log2 LUT indices `log_den1 - 32768` etc. | `uint32_t` | values in [$2^{15}$, $2^{16}$) | <= 32,767 | same | SAFE |
| SYCL sycl_integer_vif_math.h:194, 139-156 | `product = (uint64_t)sigma12 * sigma12`; `divide()`: `first * d`, `(int64_t)p - (int64_t)(first * d)` | `uint64_t`, `int64_t` | sigma12 < $2^{31}$, product < $2^{62}$; quotient < $2^{45}$ | < $2^{62}$ | same | SAFE |
| SYCL sycl_integer_vif_math.h:196, 203-231 | `(int64_t)sigma1_sq << 8`; `shift = (int64_t)shift_f`; `(ra << 8) + shift`; `limited_gg = (L * L) * sigma1_sq` (L integer < 2048) | `int64_t` | n/a | < $2^{53}$ | same | SAFE |
| SYCL sycl_integer_vif_math.h:111-125 (`gain_terms_replayed`) | soft fp64; `soft_trunc(g^2 * sigma1_sq)` | `int64_t` | g <= 100 (option max) | < $2^{44.3}$ | same | SAFE |
| SYCL integer_vif_sycl.cpp:145-151, 695-706 | per-item `vif_terms` fields (`int32_t`): x in [-16, -3], x2 >= -27, num_log in [-2048, 2048], den_log <= 32,768, num_non_log <= $2^{31} - 1$, den 1 | `int32_t` then `(int64_t)` before `reduce_over_group` | one pixel per work-item | per item fits int32 | same | SAFE |
| SYCL integer_vif_sycl.cpp:722-736 | `final_vals[f] += lmem[...]`; `atomic_ref<int64_t>::fetch_add` (7 per work-group) | `int64_t` | frame sums, N terms per scale; num_non_log <= ($2^{31} - 1$) \* N | $2^{58.0}$ | $2^{61.0}$ | SAFE (< $2^{63}$) |
| SYCL integer_vif_sycl.cpp:1819 | host `x + num_x * 17` | `int64_t` | <= 33N | 4.4e9 | $2^{35}$ | SAFE |
| SYCL integer_vif_sycl.cpp:339-346, 489, 857 | `y * stride + x` (`int` y times `unsigned` stride); `idx = gy * width + gx`; `buf_row = gy * width` | `unsigned` | scale-0 byte offset (stride = W or 2W) | 265,390,080 | 2,147,418,112 | SAFE (< $2^{32}$) |
| SYCL integer_vif_sycl.cpp:752-753 | `rd_y * rd_stride + rd_x` | `unsigned` | half-resolution index | N/4 | $2^{28}$ | SAFE |
| SYCL integer_vif_sycl.cpp:1490, 1506 | `tmp_size = (size_t)w * h * 4`; `rd_size` | `size_t` | buffers | 530,841,600 | $2^{32}$ | SAFE |

### Metal (`integer_vif.metal`, `metal_integer_vif_math.h`, `metal_integer_vif_gain.h`, `integer_vif_metal.mm`)

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| Metal integer_vif.metal:330-340 (8 bpc vertical) | `a_mu1 += icr`; `a_xx += icr * rv` | `uint` | 17 taps, v <= 255: sum fc \* $v^{2}$ <= 65536 \* 65025 | mu 16,711,680; xx 4,261,478,400 | same | SAFE ($2^{32}$ - 33,488,896) |
| Metal integer_vif.metal:343 | `v_mu1 = (ushort)((a_mu1 + 128) >> 8)` | `uint` -> `ushort` | n/a | <= 65,280 | same | SAFE |
| Metal integer_vif.metal:474-490 (16 bpc vertical) | `a_mu1 += icr` (`uint`); `a_xx += (ulong)icr * rv`; `v_mu1 = (ushort)(...)`; `v_xx = (uint)((a_xx + round) >> shift)` | `uint`; `ulong`; narrowing to `ushort` / `uint` | n/a | 4,294,901,760; 2.81e14 | same | SAFE (the `ushort` truncation equals CPU `integer_vif.c:450-451`: known item 7 is refuted for the Metal mean path) |
| Metal integer_vif.metal:356-363, 498-505 | `accum_mu1 += fc * (uint)v_mu1[...]` (`uint`); `accum_ref += (ulong)fc * v_xx` | `uint`; `ulong` | 17 taps of ushort; of uint | 4,294,901,760; 2.81e14 | same | SAFE |
| Metal integer_vif.metal:575-589 (`integer_vif_decimate_8`) | `v_ref += ci * raw8` (`uint`); `acc_ref += cj * ((v_ref + 128) >> 8)` | `uint` | 9 x 9 taps of 8-bit samples | v <= 16,711,680; acc <= 4,278,190,080 + 32,768 | same | SAFE |
| \*\*Metal integer_vif.metal:665-677 (`integer_vif_decimate_16`, `filt_scale == 1`)\*\* | \*\*`v_ref += ci * ref_in[...]`; `v_ref_r = (v_ref + round_vp) >> bpc` (no `(ushort)`); `acc_ref += cj * v_ref_r`\*\* | \*\*`uint`\*\* | \*\*9 x 9 taps. In range v_ref_r <= 65535 and acc_ref <= 4,294,901,760 + 32,768. bpc 9-15 with out-of-range codewords: v_ref_r up to 8,388,544 (bpc 9), acc_ref up to 5.5e11\*\* | in range 4,294,934,528; out of range wraps | same | \*\*DEPENDS\*\* (known item 7 for the Metal rd path: the CPU `ref_convol` is `uint16_t`; out-of-range input makes the 32-bit horizontal sum wrap and the scale-1 plane differ. For filt_scale 2-3 the input is the uint16 decimated plane, SAFE) |
| Metal integer_vif.metal:185-221 (`ivif_pixel_stat`) | `mu1_sq`, `(int)(xx_filt - mu1_sq)`, log2 terms, `acc.num_non_log += sigma2_sq` | `ulong` / `int` / `long` | as the SYCL rows | as SYCL | as SYCL | SAFE |
| Metal integer_vif.metal:135-151 | `vif_log2_64`: `(uint)temp & 32767` | `ulong` -> `uint` | normalised to [$2^{15}$, $2^{16}$) | < $2^{16}$ | same | SAFE |
| Metal integer_vif.metal:236-252 | `s_nl/s_dl/s_nn/s_dn += t_*[i]` (256 threads) | `long` | per group | num_non_log <= $2^{39}$ | same | SAFE |
| Metal integer_vif_metal.mm:620-630 | host `num_non_log += p[j].num_non_log` etc. | `int64_t` | frame | $2^{58.0}$ | $2^{61.0}$ | SAFE |
| Metal integer_vif.metal:158-160, 461, 668, 677 | `(uint)y * stride_bytes + (uint)x`; `(uint)py * f_stride + px`; `(uint)gy * out_stride + gx` | `uint` | byte / element offsets | 265,390,080 | 2,147,418,112 | SAFE (< $2^{32}$) |
| Metal metal_integer_vif_math.h:29-40 | `vmaf_mtl_vif_mirror`: `idx % (2 * (sup - 1))` | `int` | n/a | in range | in range | SAFE |
| Metal metal_integer_vif_gain.h:168-273 | port of `sycl_integer_vif_math.h` (`divide`, `limited_gg`, `soft_trunc`) | `ulong` / `long` | as the SYCL rows | < $2^{62}$ | same | SAFE |
| Metal integer_vif.metal:374, 516 | `wg_idx = bid.y * grid_x + bid.x` | `uint` | group index | 518,400 | 4,194,304 | SAFE |

## float_vif

Float arithmetic; integer content is index math, plus the soft-fp64 helpers of `sycl_float_vif_math.h` / `metal_float_vif_math.h`. The `vif_prescale` option (0.1 - 4.0, `float_vif_metal.mm:190-198`, `float_vif.c`) scales the scale-0 plane: at 16K and prescale 4 the plane is 61440 x 34560 = 2,123,366,400 samples.

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| SYCL float_vif_sycl.cpp:262-288, 315, 339, 369, 390-396, 422, 514, 584, 630 | every plane, tile and term index | `size_t` | including prescale 4 | <= 2.12e9 | <= $2^{34}$ | SAFE (64-bit) |
| SYCL float_vif_sycl.cpp:305-314 | tile origin `(int)(group * 16) - hfw`, mirror then `vmaf_sycl_tile_index` clamp | `int` | n/a | <= 61,456 (prescale 4) | <= 131,088 | SAFE |
| SYCL float_vif_sycl.cpp:598-604 | `vif_mirror(2 * x - hw + tap, in_width)` | `int` | decimation coordinates | <= 2 \* 30720 | <= 2 \* 65536 | SAFE |
| SYCL float_vif_sycl.cpp:790-825 | buffer sizes `(size_t)w * h * ...`, `row_floats += 2u * (size_t)scale_h` | `size_t` | buffer sizes | n/a | n/a | SAFE |
| SYCL sycl_float_vif_math.h:72-75, 114-116, 234 | float exponent bits `(bits & 0x7F800000) >> 23`, `exponent - (23 << 23)` for sums >= 1 | `uint32_t` | n/a | n/a | n/a | SAFE (no underflow: biased exponent >= 127) |
| SYCL float_vif_sycl.cpp:529-533 (`range<1>(pixels)`) | 1-D global range over the scale-0 plane, ids assumed to fit `int` (icpx default `-fsycl-id-queries-fit-in-int`) | `size_t` range, `int` id | pixels | 132.7M; 2,123,366,400 at prescale 4 (< INT_MAX by 1.1 %) | $2^{30}$; above INT_MAX for prescale > 1.414 | SAFE (at 16K for every prescale; above that the DPC++ runtime rejects the range instead of wrapping) |
| \*\*Metal float_vif.metal:117-122 (store), 160-163 (read)\*\* | \*\*`moments[k * geometry.plane + at]` for k = 0..4, `plane = scale_w[0] * scale_h[0]`\*\* | \*\*`uint` index\*\* | \*\*five float moment planes of the (prescaled) scale-0 plane; largest index 5 \* plane - 1\*\* | default prescale: 663,551,999; \*\*prescale 4: 1.06e10 (wraps)\*\* | default prescale: 5 \* $2^{30} - 1$ (wraps) | \*\*OVERFLOW@16K\*\* (reachable at 16K only through the `vif_prescale` option: wraps once plane >= 858,993,460, i.e. prescale > 2.545 at 15360 x 8640. At the default prescale 1.0 it wraps above N = 858,993,459, inside the cap envelope. The wrapped index makes the k >= 2 moment planes alias the start of the buffer. The host buffer is size_t, `float_vif_metal.mm:421-423`, 42 GB at 16K prescale 4, so an allocation failure is the more likely outcome on small devices) |
| Metal float_vif_metal.mm:563-565, 586-588 | `s->scale_w[scale] * s->scale_h[scale]` passed as `uint32_t plane` | `unsigned` | prescaled plane size | 2,123,366,400 at prescale 4 | wraps at prescale >= 2.0 ($65536^{2}$ = $2^{32}$) | OVERFLOW@CAP-ONLY (fits uint32 at 16K for every prescale; above 16K with prescale >= 2 the host product itself wraps) |
| Metal metal_float_vif_math.h:171-175 (`vmaf_mtl_fvif_term_index`), float_vif.metal:169, 199 | `(x * height + y) * 2` | `uint` | two floats per sample; largest 2 \* plane - 1 | prescale 4: 4,246,732,799 (1.1 % below $2^{32}$) | wraps for plane >= $2^{31}$ (prescale > 1.414 at the cap) | OVERFLOW@CAP-ONLY (SAFE at 16K for every prescale) |
| Metal float_vif.metal:53-69, 117, 150, 157 | `(uint)y * stride_bytes + x`; `(uint)y * width + x`; `row_at + mirror(...)` | `uint` | single-plane indices | <= 2.12e9 | <= $2^{34}$ at prescale 4 (wraps) | SAFE (at 16K; at the cap these wrap only where the moments row above already does) |
| Metal float_vif_metal.mm:365-379 | `scaled_w = (size_t)lround(w * prescale)`; `scale_w[0] = (unsigned)scaled_w` | `size_t` -> `unsigned` | n/a | 61,440 | 131,072 | SAFE |
| Metal metal_float_vif_math.h:211-213, 372-374, 351 | float exponent bits; `(mant_hi << 32) \| mant_lo` | `uint` / `ulong` | n/a | n/a | n/a | SAFE |

## adm (integer)

Constants (integer ADM group): DWT taps lo {15826, 27411, 7345, -4240}, hi {-4240, -7345, 27411, -15826}, abs sum 54822. Band maxima: scale 0 <= 22,930; scale 1 <= 1,491,390,732 (band_a), h/v/d <= 1,448,979,042; scales 2-3 <= 751,510,749. CSF weight budgets: scale-0 h/v < 46,603.4, d < 65,536; defaults 36,453 (h/v), 49,417 (d). Region: `cols = Wb - 2 * int(0.1 * Wb - 0.5)`. The scale-0 contrast-masking row term is `((x^2 + 2^28) >> 29) * x >> (ceil(log2 Wb) - 4)` (h/v; d: shifts 30 and -3); its exact row maximum over column sign patterns is the `scripts/dev/adm_cm_row_bound.py` replay, re-run for this audit: 0.855 \* INT64_MAX at W = 15360, 0.912 at 8K DCI / 16384 / 32768, 0.973 at W = 128 and \*\*1.0207 at W = 64 (9.414e18)\*\*, all at default weights.

### SYCL (`integer_adm_sycl.cpp`; shared `adm_gain_limit.h`, `adm_angle_flag.h`, `adm_cm_accumulator.h`)

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| SYCL integer_adm_sycl.cpp:552-569 | vertical DWT `lo_val`, `hi_val` (int64 products); `(int32_t)((v + 2^(s-1)) >> s)` | `int64_t` -> `int32_t` | 4 taps; scale 0: samples <= 65535, minus 46342 \* $2^{\mathrm{bpc}-1}$; scales 1-3: band_a | scale 0 <= 3.31e9 before the shift, <= 27,411 after; scale 1 (no shift) <= 1,058,572,481; scales 2-3 <= 1,058,676,611 | same | SAFE |
| SYCL integer_adm_sycl.cpp:639-660 | horizontal DWT `lo`, `hi` int64; band store `(int32_t)((v + 2^(s-1)) >> s)` | `int64_t` -> `int32_t` | 4 taps of the row buffer | <= 5.8e13; band <= 1,491,390,732 | same | SAFE (0.69 \* INT32_MAX) |
| SYCL integer_adm_sycl.cpp:481-492, 573-574, 643-646, 678 | `y * band_stride + x`, `y * in_stride + x`, `gy * out_stride + gx` (out_stride = 2w), `row_off + mirror`, `gy * buf_stride + gx` | `unsigned` | scale-0 row buffer index ~ (H/2) \* 2W | 132,710,399 | (16384 - 1) \* 65536 + 65535 = $2^{30} - 1$ | SAFE (< $2^{32}$) |
| SYCL integer_adm_sycl.cpp:503-508 | `vmaf_sycl_tile_index(dev_mirror_adm(y, h), h)`, column past the plane reads 0 | `int` | tile rows | in range | in range | SAFE |
| SYCL integer_adm_sycl.cpp:910-912; adm_angle_flag.h (`adm_angle_flag_i64`) | `ot_dp`, `o_mag_sq`, `t_mag_sq` = sums of two int32 products; flag via 24-bit mantissas, `mp * mp << (sp + p)` | `int64_t`; `uint64_t` | scales 1-3: 2 \* 1.449e9^2 | 4.2e18 | same | SAFE (< $2^{63}$; flag products < $2^{58}$) |
| SYCL integer_adm_sycl.cpp:756-781 (`adm_dev_best15`) | `(tmp + (1u << (ks - 1))) >> ks`, ks in [1, 17] | `uint32_t` | \|o\| < $2^{31}$ | < $2^{31}$ + $2^{16}$ | same | SAFE |
| SYCL integer_adm_sycl.cpp:792-802 | `div_val = lut[32768 + v] * sign`; `k = ((int64_t)div_val * th + 2^(shift-1)) >> shift`, clamped to [0, 32768] in int64 | `int64_t` | div_val <= $2^{30}$, \|t\| <= 1.449e9 | 1.56e18 | same | SAFE |
| SYCL integer_adm_sycl.cpp:840 | `r = (int32_t)(((int64_t)k * oh + 16384) >> 15)` | `int64_t` -> `int32_t` | k <= 32768 | \|r\| <= \|o\| | same | SAFE |
| SYCL integer_adm_sycl.cpp:808-827; adm_gain_limit.h:59-74 | `gained = adm_gain_limit_product(r_val, g)` (exact truncated double product in 64-bit integers); `(int32_t)min(gained, th)` / `max` in int64 | `int64_t` -> `int32_t` | gain <= 100: \|gained\| <= 1.449e11; the selected value lies between 0 and t | result \|.\| <= \|t\| | same | SAFE (known item 3 refuted for SYCL: no double-to-int32 conversion before the min/max; the CPU's MIN-in-double result is reproduced in integers) |
| SYCL integer_adm_sycl.cpp:919 | `s.d[b] = t[b] - s.r[b]` | `int32_t` | r lies between 0 and t | \|d\| <= \|t\| | same | SAFE |
| SYCL integer_adm_sycl.cpp:447-452 | scale-0 csf: `((int64_t)i_rfactor * a + rnd) >> 15 / 17`, then `adm_i16()` | `int64_t` -> `int16_t` | weight < $2^{16}$ (adm_csf_config_check) times \|a\| <= 22,930 | <= 32,611 (h/v within budget) | same | SAFE (within the ADR-1472 budget) |
| \*\*SYCL integer_adm_sycl.cpp:856-861\*\* | \*\*scale-0 flt `adm_i16((4369 * abs_csf + 2048) >> 12)`\*\* | \*\*`int` -> `int16_t` (mod $2^{16}$)\*\* | \*\*\|csf\| <= 32,611 gives flt <= 34,786\*\* | default weights: <= 27,212 | same | \*\*DEPENDS\*\* (CSF-weight option, not frame size: flt wraps negative once \|csf\| >= 30,720, i.e. an h/v weight >= 43,900 (budget 46,603). Bit-identical with the CPU `integer_adm_kernels.h:488-489` and CUDA/HIP prior row 12) |
| SYCL integer_adm_sycl.cpp:851, 862, 874 | scales 1-3 csf `((int64_t)rf * v + 2^27) >> 28`; flt / centre `((int64_t)143165577 or 286331153 * \|csf\| - 2^31) >> 32` | `int64_t` -> `int32_t` | rf < 5.47e8, \|v\| <= 1.449e9 | csf <= 1,518,500,221; flt <= 1.01e8 | same | SAFE |
| SYCL integer_adm_sycl.cpp:870-872 | scale-0 centre `(8738 * abs_csf + 2048) >> 12` (int, not narrowed, ADR-1402) | `int` | \|csf\| <= 32,768 | 69,904 | same | SAFE |
| SYCL integer_adm_sycl.cpp:1033-1063 | `thr += neighbours + centre` (27 terms); `(unsigned)rows[i] * in.stride` | `int64_t`; `unsigned` | scale 0 <= 1,048,560; scales 1-3 <= 1.5185e9 | same | same | SAFE |
| SYCL integer_adm_sycl.cpp:1068-1073 | scale-0 excess `\|(int64_t)rf * v\| - thr * 2^shift_sub`, clamped to INT32_MAX | `int64_t` | rf \* \|v\| <= 1.5e9; thr \* $2^{12}$ <= 4.3e9 | <= INT32_MAX | same | SAFE |
| \*\*SYCL integer_adm_sycl.cpp:1094-1095\*\* | \*\*scale-0 `x_sq = (int32_t)((cm * cm + 2^(xsq-1)) >> xsq)`\*\* | \*\*`int64_t` -> `int32_t`\*\* | \*\*cm <= 1.0686e9 (h/v at the budget) or 1.5027e9 (d) while thr >= 0\*\* | <= 2,127,000,000 (h/v), 2,103,000,000 (d) | same | \*\*DEPENDS\*\* (fits int32 while thr >= 0; a negative thr from the flt wrap above lets cm reach the INT32_MAX clamp and x_sq = $2^{33}$ narrows modulo $2^{32}$. Option-dependent, as CUDA/HIP prior row 13) |
| SYCL integer_adm_sycl.cpp:1076-1079, 1094-1097 | scales 1-3 excess and `x_sq`, `(x_sq * cm + rnd) >> xcub` | `int64_t` / `int32_t` | x <= 1,518,500,249 (budget, thr >= -27) | x_sq <= 2,147,483,647; term <= 3.26e18 | same | SAFE |
| \*\*SYCL integer_adm_sycl.cpp:1127-1141 (`cm[b] +=`, DLM and AIM), 1146-1158 (`reduce_over_group`), 1184-1189 (`row_total += lmem`), 1164-1174 (fold)\*\* | \*\*scale-0 contrast-masking row total: per work-item partials over the row's columns, sub-group sum, then the work-group row total, then `adm_cm_round_row_total(row_total, rnd, ceil(log2 Hb))`\*\* | \*\*`int64_t`\*\* | \*\*term `((x^2 + 2^28) >> 29) * x >> (ceil(log2 Wb) - 4)`, x = \|band\| \* weight with thr = 0 (ref == dis for DLM, a flat reference for AIM); cols terms per row (`scripts/dev/adm_cm_row_bound.py`)\*\* | default weights: 0.855 \* INT64_MAX at W = 15360, \*\*1.0207 \* INT64_MAX at W = 63-64\*\*; with h/v weight >= 38,406 or d >= 60,965 it wraps at W = 15360 | default: 0.912 \* INT64_MAX; h/v >= 37,588 or d >= 59,667 wraps | \*\*OVERFLOW@16K\*\* (known item 2, confirmed for SYCL: W = 63-64 is inside the envelope (`adm_frame_size_check` admits W >= 17) and wraps at default options; non-default CSF weights wrap at every size. Signed int64 sub-group / work-group sums wrap, the folded row is negative or wrong, and the band numerator becomes NaN or wrong. Shared with the CPU and CUDA/HIP prior row 2) |
| SYCL integer_adm_sycl.cpp:1164-1174 | scale-0 / scales 1-3 frame accumulator `atomic_ref<int64_t>::fetch_add(shifted row)` | `int64_t` | sum over rows of (row >> ceil(log2 Hb)), rows <= 0.875 \* $2^{s}$ | < 0.875 \* $2^{63}$ for any int64 row value | same | SAFE (cannot wrap even when a row did; atomic add is defined modular) |
| SYCL integer_adm_sycl.cpp:1103-1111, 1115-1120 (scale 0) | `den[b] += abs_o * abs_o * abs_o` | `int64_t` | \|o\| <= 22,930: 1.2056e13 per term, cols per row | 7.41e16 per row | 1.58e17 | SAFE |
| SYCL integer_adm_sycl.cpp:1103-1111 (scales 1-3) | `o_sq = (abs_o^2 + 2^den_sq) >> den_sq`; `(o_sq * abs_o + rnd) >> den_cub` | `int64_t` | \|o\| <= 1.449e9: abs_o^2 2.1e18; term <= 1.42e18 / $2^{\lceil \log_2 \mathrm{cols} \rceil}$ | row <= 1.42e18 | same | SAFE |
| SYCL integer_adm_sycl.cpp:1164-1174, 1348 | denominator frame accumulator `fetch_add` on `int64_t`, read back as `(uint64_t)accum[i]` | `int64_t` atomic (modular), host `uint64_t` | scale 0: area \* 1.2056e13 / 2^ceil(log2 area - 20) <= $2^{20}$ \* 1.2056e13 = 1.264e19 | 8.0e18 | 8.09e18 | SAFE (may exceed INT64_MAX in the signed atomic, but the add is modular and the host reinterprets as uint64 like the CPU's uint64 `adm_csf_den_fold`; < $2^{64}$) |
| SYCL integer_adm_sycl.cpp:1241-1265, 1296-1342 | region, shifts `ceil(log2(active_w * active_h)) - 20`, `area = (bottom - top) * (right - left)` | `int` | region products | 21,252,868 | 171,872,100 | SAFE |
| SYCL integer_adm_sycl.cpp:1395-1427, 1439-1446 | buffers `(size_t)w * 2 * half_h * 4`, `buf_stride * half_h * 4`; LUT `2^30 / i` | `size_t`; `int32_t` | n/a | 1.06e9 B | $2^{33}$ B | SAFE |

### Metal (`integer_adm.metal`, `metal_integer_adm_math.h`, `metal_integer_adm_uniforms.h`, `integer_adm_metal_host.c`, `integer_adm_metal.mm`)

Metal has no 64-bit atomics: each (band, row) threadgroup reduces through a pair of 32-bit atomics with an explicit carry and stores a 64-bit value as two `uint` words; the host adds the words.

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| Metal integer_adm.metal:385-398, 460-472 | vertical DWT `accum_lo += (long)tap * s`; `(int)((acc + add) >> shift)` | `long` -> `int` | as SYCL | <= 1,058,676,611 | same | SAFE |
| Metal integer_adm.metal:548-571 | horizontal DWT `a/v/h/dd` (`long`); scale 0 stored as `short` (`iadm_write16`) | `long` -> `int` -> `short` | scale-0 bands <= 22,930 | fits int16 | same | SAFE |
| Metal integer_adm.metal:297-313, 371-378, 397, 471 | `band_idx * slice + y * buf_stride + x` (slice = buf_stride \* half_h, band_idx <= 3); `yy * raw_stride + gx`; `gy * out_stride + gx` | `int` | <= 4 \* N/4 | 132,710,399 | $2^{30} - 1$ | SAFE (< INT32_MAX) |
| Metal metal_integer_adm_math.h:68-75, 77-99 | `k_s0`: `(long)recip(o) * t`; `k_s123`: `((long)recip * t) * k_sign + (1u << (14 + shift))`, shift in [1, 16] | `long`; `uint` | recip <= $2^{30}$ | 1.56e18; `1u << 30` | same | SAFE |
| Metal metal_integer_adm_math.h:123, 133 | `rst = (k * o + 16384) >> 15` (`int` at scale 0, `long` at 1-3) | `int` / `long` | k <= 32768, \|o\| <= 22,930 (s0) | 7.51e8 | same | SAFE |
| Metal metal_integer_adm_math.h:102-115 | `gained = adm_gain_limit_product(rst, g)`; `(int)min/max(gained, t)` in `long` | `long` -> `int` | \|gained\| <= 1.449e11; result between 0 and t | \|t\| | same | SAFE (known item 3 refuted for Metal) |
| Metal integer_adm.metal:225-253, 286-288 | angle flag: `(long)oh * th + ...`; 24-bit mantissa products | `long` / `ulong` | n/a | 4.2e18 | same | SAFE |
| Metal integer_adm.metal:623-627, 1044-1048 | scale-0 `dst_val = (int)(irf * (uint)src)`; `(dst + add) >> 15 / 17` | `uint` modular -> `int` | true product <= 65535 \* 22,930 = 1.5e9 | < INT32_MAX | same | SAFE (modular uint product equals the true value) |
| \*\*Metal integer_adm.metal:672-677\*\* | \*\*scale-0 `flt = (int)((4369u * (uint)abs(csf) + 2048) >> 12)` stored with `iadm_write16` (`(short)`)\*\* | \*\*`int` -> `short`\*\* | \*\*as SYCL\*\* | default <= 27,212 | same | \*\*DEPENDS\*\* (CSF-weight option: wraps negative for h/v weight >= 43,900, as the CPU. The AIM pass, :1083-1100, forms its neighbour flt inline in `int` and does not narrow, so with such weights the Metal AIM threshold differs from the CPU's) |
| \*\*Metal integer_adm.metal:759-765 (`iadm_cm_cube`), 883-884, 1107-1108\*\* | \*\*scale-0 `x_sq = (x * x + add) >> shift_sq` (kept in `long`, not narrowed to int32 as on the CPU); `cube = (x_sq * x + add) >> shift_cub`\*\* | \*\*`long`\*\* | \*\*x = `adm_cm_excess_s0()` <= INT32_MAX; with thr >= 0 x <= 1.5027e9 so x_sq \* x <= 3.16e18\*\* | default <= 1.09e18 | same | \*\*DEPENDS\*\* (option-dependent: a negative thr from the flt wrap above lets x reach INT32_MAX; then x_sq = $2^{33}$ and x_sq \* x = $2^{64}$ overflows `long`) |
| Metal integer_adm.metal:770-779 | `adm_cm_excess_s0`: `\|x\| - (long)thr << shift`, clamped | `long` -> `int` | n/a | <= INT32_MAX | same | SAFE |
| Metal integer_adm.metal:728-750 (`iadm_tg_reduce_u64`) | lo / hi `atomic_uint` pair with carry `prev_lo + lo < prev_lo` | `uint` pair = `ulong` modular | per-lane partials of one (band, row) | exact modulo $2^{64}$ | same | SAFE (correct carry propagation; the value is the ulong sum mod $2^{64}$) |
| \*\*Metal integer_adm.metal:836-896, 1066-1115 (scale-0 `local_cm`, `local_aim`, `total_cm`, `cm_out`)\*\* | \*\*scale-0 contrast-masking row total, summed as `ulong`, folded `(total + rnd) >> ceil(log2 Hb)` in `ulong`\*\* | \*\*`ulong`\*\* | \*\*the `scripts/dev/adm_cm_row_bound.py` row: max 9.414e18 = 0.51 \* $2^{64}$ at W = 64, default weights\*\* | default: <= 0.51 \* $2^{64}$ (no wrap); h/v weight >= ~45,600 at W = 64: > $2^{64}$ | default: <= 0.46 \* $2^{64}$ | \*\*DEPENDS\*\* (known item 2 refuted for Metal at default options: the row that wraps the CPU's and SYCL's int64 (1.0207 \* INT64_MAX at W = 64) fits the unsigned 64-bit sum, so Metal returns the arithmetically correct value where the CPU has signed overflow. With an h/v CSF weight >= ~45,600 (term ~ $\mathrm{weight}^{3}$; budget 46,603) the ulong row wraps too) |
| \*\*Metal integer_adm_metal_host.c:305-315\*\* | \*\*`t.cm[band] += (int64_t)iadm_slot(...)`, `t.aim[band] += ...` (host frame sum of the folded rows)\*\* | \*\*`int64_t`\*\* | \*\*sum over rows of (row >> ceil(log2 Hb)); rows / $2^{s}$ <= 0.875\*\* | default: <= 0.875 \* 9.414e18 = 8.24e18 = 0.89 \* INT64_MAX | same | \*\*DEPENDS\*\* (SAFE at default weights; with an h/v weight >= ~37,850 (W = 63-64) the folded rows can sum past INT64_MAX in this signed host sum, a wrap the SYCL/CUDA frame accumulators avoid because their rows already wrapped) |
| Metal integer_adm.metal:842-845 (scale 0) | `local_csf += ((ulong)t * t) * t` | `ulong` | t <= 22,930 | 7.41e16 per row | 1.58e17 | SAFE |
| Metal integer_adm.metal:956-961 (scales 1-3) | `((t * t + add) >> den_sq) * t + add >> den_cub` | `ulong` | t <= 1.449e9 | <= 1.42e18 per term / $2^{\lceil \log_2 \mathrm{cols} \rceil}$ | same | SAFE |
| Metal integer_adm.metal:895, 1005; integer_adm_metal_host.c:311 | `csf_out = (total + add) >> den_shift_accum`; host `t.den[band] += iadm_slot()` | `ulong`; `uint64_t` | <= $2^{20}$ \* 1.2056e13 = 1.264e19 | 8.0e18 | 8.09e18 | SAFE (< $2^{64}$; same as the CPU's uint64) |
| Metal integer_adm.metal:982-994, 1180-1190 (scales 1-3) | `thr += sum` (`int`); `x = abs(csf) - thr`; `iadm_cm_cube` | `int`; `long` | thr <= 1.5185e9; x <= 1,518,500,249 | term <= 3.26e18; row < 3.27e18 | same | SAFE |
| Metal metal_integer_adm_uniforms.h:102-106; integer_adm_metal_host.c:61-94 | `accum_word = ((wg * 9 + slot) * 2) + hi`; `wg_count = 3 * rows` | `uint` | n/a | 10,374 groups | 39,330 | SAFE |
| Metal integer_adm_metal.mm:353 | `adm_frame_size_check()` | `unsigned` guard | W, H >= 17 | n/a | n/a | SAFE (same minimum as the CPU) |

## adm (float)

Float arithmetic with soft-fp64 replays; integer content is index math.

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| SYCL float_adm_sycl.cpp:185-204 | `band[y * (int)parent_buf_stride + x]`; `(size_t)y * raw_stride` | `int`; `size_t` | parent band (scale >= 1, <= N/4) | 3.3e7 | 2.7e8 | SAFE |
| SYCL float_adm_sycl.cpp:231-234, 331-332 | `dst[gy * out_stride + cur_w + gx]`, out_stride = 2 \* cur_w | `int` | scale-0 row buffer, ~N | 132,710,399 | $2^{30} - 1$ | SAFE (< INT32_MAX) |
| SYCL float_adm_sycl.cpp:347-351 | `slice = buf_stride * half_h`; `3 * slice + gy * buf_stride + gx` | `int` | 4 sub-bands of N/4 | 132,710,400 | $2^{30}$ | SAFE |
| SYCL sycl_float_adm_math.h:421-424, 494-498, 587-591 | `band_index`, `term_index`, `row_item` | `size_t` | 9 term slots of the region | 1.9e8 | 1.55e9 | SAFE |
| SYCL sycl_float_adm_math.h:128-130, 613 | float exponent bits; `(uint64_t)ldexp(fraction, 53)` | `uint32_t`; `uint64_t` | exponent of a sum >= $2^{-100}$ (biased >= 27 > 23); 53-bit significand | n/a | n/a | SAFE |
| SYCL float_adm_sycl.cpp:705 | `1e-10 * (w * h)` | `int` product | n/a | 132,710,400 | $2^{30}$ | SAFE |
| Metal float_adm.metal:171-174, 188-189 | `plane[yy * raw_stride + xx]`; `dst[gy * out_stride + gx]` | `int` | ~N | 132,710,399 | $2^{30} - 1$ | SAFE |
| Metal float_adm.metal:265, 290-291, 352-363 | `slice = buf_stride * half_h`; `k * slice + at` (k <= 3) | `int` | n/a | 132,710,400 | $2^{30}$ | SAFE |
| Metal metal_float_adm_math.h:571-576 | `term_index = (slot * region_w + x) * region_h + y` | `uint` | 9 slots \* region; region <= 0.64 \* N/4 | 191,275,812 | 9 \* $13110^{2}$ = 1,546,848,900 | SAFE (< $2^{32}$) |
| Metal float_adm_metal.mm:397-404, 538-584 | `term_floats = 9 * region_w * region_h`; buffers | `size_t` | buffer sizes | 191,275,812 floats | 1,546,848,900 floats | SAFE |

## cambi

Bounds (prior G5): `vmaf_cambi_check_window_fits_lut()` limits the window to 65 x 65 (4,225 pixels) at any resolution (SYCL `integer_cambi_sycl.cpp:1719-1734`, Metal `integer_cambi_metal.mm:512-516`); levels <= 1056; c-value < 9506.25, fixed point (x $2^{24}$) < $2^{37.22}$ = F. Out-of-range samples are flagged (`launch_validate`, SYCL :355-375) as on the CPU. Per-frame only.

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| SYCL integer_cambi_sycl.cpp:264-267 (`plane_offset`) and every `y * width + x` / `row * pitch` (:303, 366, 423, 547, 569, 639-641, 663, 700-708, 851-857) | 32-bit element offsets | `unsigned` | < N, or chunks \* levels \* width for the histograms (64 MiB budget / 2 B = $2^{25}$ cells) | < $2^{27}$ | < $2^{30}$ | SAFE (< $2^{32}$) |
| SYCL integer_cambi_sycl.cpp:300-314 | `v << (10 - bpc)`, `(v + rounding) >> shift` | `unsigned` | 10-bit conversion | <= 1024 | same | SAFE |
| SYCL integer_cambi_sycl.cpp:318-338 | anti-dither `(a + b) >> 1`, `sum >> 2` | `unsigned` | 4 samples <= 1024 | <= 4096 | same | SAFE |
| SYCL integer_cambi_sycl.cpp:454-473 | `sum += t.flags[...]` (7 taps) stored as `uint8_t`; then 7 row sums | `unsigned` / `uint8_t` | 7 and 49 flags | 7; 49 | same | SAFE |
| SYCL integer_cambi_sycl.cpp:575-576 | `(uint16_t)(value - v_band_base)` | `uint16_t` (intentional modular, then `< v_band_size` test) | n/a | n/a | n/a | SAFE (defined unsigned narrowing; out-of-band values are rejected by the test) |
| SYCL integer_cambi_sycl.cpp:724-731 (`hist_apply`), 747, 752 | `cell = (uint16_t)((int)cell + count)`; `sign * (int)(x - start)` | `uint16_t`; `int` | window count of one level, 0..4225; runs <= 65 | <= 4225 | same | SAFE |
| SYCL integer_cambi_sycl.cpp:776-791 | `a.weights[d] * p0 * pm` | `int` | weight <= 9, p0 + pm <= 4225 | 40,163,904 (loose 160,655,625) | same | SAFE (< $2^{31}$) |
| SYCL integer_cambi_sycl.cpp:788 | `a.lut[pm + p0]` | `int` index | <= 4225 < LUT size 4226 | 4225 | same | SAFE |
| SYCL integer_cambi_sycl.cpp:180-183 | `cambi_fixed(value) = (uint64_t)(value * 2^24)` | `float` -> `uint64_t` | one c-value | < $2^{37.22}$ | same | SAFE |
| SYCL integer_cambi_sycl.cpp:826-835 | `++tally.count`; `tally.sum += cambi_fixed(value)` | `uint32_t`; `uint64_t` | one column of a chunk: chunk_rows <= H terms | count <= 8640; sum <= $2^{50.29}$ | count <= 32768; sum <= $2^{52.21}$ | SAFE |
| SYCL integer_cambi_sycl.cpp:818-823 | `GlobalCounter(hist[bin]).fetch_add(count)` | `uint32_t` atomic | every c-value of a scale counted once | <= N ($2^{26.98}$) | <= $2^{30}$ | SAFE |
| SYCL integer_cambi_sycl.cpp:878-882 | `reduce_over_group(group, tally.sum)` (64 work-items) | `uint64_t` | 64 tallies | $2^{56.29}$ | $2^{58.21}$ | SAFE |
| SYCL integer_cambi_sycl.cpp:911-916 (`pool_block`) | `per_group = (n + groups - 1) / groups`; `begin = group * per_group`; `begin + per_group` | `unsigned` | n <= $2^{30}$, groups <= 512 | < $2^{27}$ + $2^{18}$ | < $2^{30}$ + $2^{21}$ | SAFE |
| SYCL integer_cambi_sycl.cpp:925-945 | `++cur_count`; `LocalCounter(local_hist[bin]).fetch_add(cur_count)`; global `fetch_add(local_hist[b])` | `uint32_t` | one group's block / every element | <= 259,200; <= N | <= $2^{21}$; <= $2^{30}$ | SAFE |
| SYCL integer_cambi_sycl.cpp:993-1016 | `mine += hist[...]`; `exclusive_scan_over_group`; `cum += count`; `k - cum`; `(top - j) << RADIX_SHIFT` | `uint32_t` | bin counts of one pass, <= n | <= $2^{26.98}$ | <= $2^{30}$ | SAFE |
| SYCL integer_cambi_sycl.cpp:1040-1054 | `sum += cambi_fixed(value)` above the threshold; group reduce | `uint64_t` | per_group <= ceil(n / 512) terms | 259,200 \* F = $2^{55.20}$ | $2^{21}$ \* F = $2^{58.21}$ | SAFE |
| SYCL integer_cambi_sycl.cpp:1086-1094 | `lo32 += partial & 0xFFFFFFFF`; `hi32 += partial >> 32`; group reduce | `uint64_t` | <= cvals_groups partials (<= 64,800 at 16K, <= $2^{19}$ at cap) | lo <= $2^{47.98}$, hi <= $2^{32.3}$ | lo <= $2^{51}$, hi <= $2^{45.3}$ | SAFE |
| SYCL integer_cambi_sycl.cpp:1098-1101 | `(t_fixed >> 32) * k_rem`, `(t_fixed & 0xFFFFFFFF) * k_rem`; `u128_from_halves`, `u128_add` | `uint64_t` / 128-bit pair | t_fixed < $2^{37.22}$, k_rem <= n | low < $2^{58.98}$ | low < $2^{62}$ | SAFE |
| SYCL integer_cambi_sycl.cpp:1061-1078 | U128 top-K sum `{lo, hi}` with carries | 2 x `uint64_t` | k terms < F: n \* F | $2^{64.20}$ (needs the 128-bit pair) | $2^{67.21}$ | SAFE (128-bit; carries checked) |
| SYCL integer_cambi_sycl.cpp:1309, 1330-1338, 1357-1366 | `enc_width * enc_height` (`int`); `compute_units * 512`; `chunk_rows`; `cvals_groups = chunks * ceil(W/64)`; `n = height * width`; `(int)(topk * (int)n)` | `int`; `unsigned` | n/a | N; 64,800 | $2^{30}$; $2^{19}$ | SAFE (< $2^{31}$) |
| SYCL integer_cambi_sycl.cpp:1367-1426 | `hist_elements`, `partial_elements`, buffer sizes | `size_t` | n/a | n/a | n/a | SAFE |
| Metal integer_cambi.metal:110-124 | `box_sum += zero_deriv_at(...)` (7 x 7) | `uint` | 49 flags | 49 | same | SAFE |
| Metal integer_cambi.metal:80-84, 124, 152, 209-219 | `(uint)y * stride_words + (uint)x`; `(y * 2u) * src_stride + x * 2u` | `uint` | sample index | N | $2^{30}$ | SAFE |
| Metal integer_cambi_metal.mm:543-553, 568 | host buffers `sizeof * w * h`, `num_bins = (uint16_t)(1024 + ...)` | `size_t`; `uint16_t` | the c-values and pooling run on the host through the CPU helpers (`vmaf_cambi_calculate_c_values`, `vmaf_cambi_spatial_pooling`, cambi.c: CPU scope) | <= 1056 bins | same | SAFE |

## psnr_hvs

Bounds (prior G5): every implementation rejects bpc > 12 (SYCL `integer_psnr_hvs_sycl.cpp:863`, Metal `integer_psnr_hvs_metal.mm:196`), so samples are <= 4095. DCT intermediates <= 32,795, lifting products <= 379,769,216 ($2^{28.5}$), $\mathrm{AC}^{2}$ <= $2^{28.0}$. Blocks: step 7, 16K 4:4:4 total 8,122,188 (2,707,396 per plane), cap 4:4:4 65,735,283.

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| SYCL integer_psnr_hvs_sycl.cpp:255-296 | `od_bin_fdct8` butterflies and lifting `(t * K + r) >> s` | `int` | 12-bit samples | <= 379,769,216 | same | SAFE |
| SYCL integer_psnr_hvs_sycl.cpp:241-244 | `od_dct_rshift`: `((unsigned)a >> (32 - b)) + (unsigned)a` | `unsigned` (intentional modular) | \|a\| <= 32,795 | exact | same | SAFE |
| SYCL integer_psnr_hvs_sycl.cpp:565-575 | `coefficient * coefficient` | `int` | $\mathrm{AC}^{2}$ | <= $2^{28.0}$ | same | SAFE |
| SYCL integer_psnr_hvs_sycl.cpp:599-612, 653 | `mask \|= 1ULL << index`; `(uint32_t)popcountll(mask)` | `uint64_t`; `uint32_t` | 64 bits | 64 | same | SAFE |
| SYCL integer_psnr_hvs_sycl.cpp:418-441, 630-647 | `(size_t)(in_plane % blocks_x) * 7`; `lane.origin + row * width`; `(size_t)lane.block * 64`; `2U * (size_t)total_blocks` | `size_t` | indices | <= 5.2e8 | <= 4.2e9 | SAFE |
| SYCL integer_psnr_hvs_sycl.cpp:684, 688-703 | `num_chunks = (total_blocks + 255) / 256`; scratch layout `(size_t)total_blocks * 64 * 4` etc. | `unsigned`; `size_t` | n/a | 31,728 chunks | 256,779 | SAFE |
| SYCL integer_psnr_hvs_sycl.cpp:711-723, 765-773, 812-822 | Hillis-Steele `s_data[tid] += n`; `intra_offset = s_data[tid] - count`; `b = chunk * 256 + tid` | `uint32_t` / `unsigned` | 256 counts <= 64 | 16,384 | 16,384 | SAFE |
| \*\*SYCL integer_psnr_hvs_sycl.cpp:787-795 (`launch_scan_prefix`), 822-833\*\* | \*\*`limit = num_chunks < 32768u ? num_chunks : 32768u`; `running += chunk_totals[c]` for `c < limit`; compaction writes `packed_terms + chunk_offsets[chunk] + intra`\*\* | \*\*`unsigned` loop cap; `uint32_t` sum\*\* | \*\*num_chunks = ceil(total_blocks / 256); only the first 32,768 chunks (8,388,608 blocks) get an offset. The sum itself is <= 64 \* 65,735,283 = 4,207,058,112 < $2^{32}$\*\* | 4:4:4: 31,728 chunks, all visited (3.2 % margin) | 4:4:4: 256,779 chunks; luma only: 85,593 | \*\*OVERFLOW@CAP-ONLY\*\* (known item 5, confirmed for SYCL, identical to HIP prior row 3: past 8,388,608 blocks (e.g. 16384 x 8640 4:4:4 = 8,662,680 blocks, or luma-only frames above about 20.3K x 20.3K) `chunk_offsets[c >= 32768]` are never written; `d_scratch` is a `malloc_device` buffer that is not cleared, so the compaction writes from stale offsets: out-of-bounds device writes, a short `total_terms`, wrong or NaN scores. The CUDA twin scans every chunk) |
| SYCL integer_psnr_hvs_sycl.cpp:930-934 | `num_blocks = bx * by`; `total_blocks += num_blocks[p]` | `unsigned` | n/a | 8,122,188 | 65,735,283 | SAFE |
| SYCL integer_psnr_hvs_sycl.cpp:1161-1166 | `end - start` (`uint32_t`) of plane offsets | `uint32_t` | n/a | <= total_terms | same | SAFE (inside the 32,768-chunk cap; can underflow once the cap row above has fired) |
| SYCL integer_psnr_hvs_sycl.cpp:1207-1209 | `(size_t)total_terms * sizeof(float)` | `size_t` | readback | 2.08 GB | 16.8 GB | SAFE |
| Metal metal_psnr_hvs_math.h:182, 222-252 | DCT `od_dct_rshift`, lifting products | `int` / `uint` | as SYCL | <= $2^{28.5}$ | same | SAFE |
| Metal metal_psnr_hvs_math.h:280 | `coefficient * coefficient` | `int` | $\mathrm{AC}^{2}$ | <= $2^{28.0}$ | same | SAFE |
| Metal integer_psnr_hvs.metal:197, 204, 257 | `ref[sy * strides.x + sx]`; `slot = blk_y * bx + blk_x` | `uint` | packed rows; block index | 265,390,080; 2,707,396 | 2,147,418,112; 21,911,761 | SAFE |
| Metal integer_psnr_hvs.metal:158 | `terms[(ulong)slot * 64 + lid]` | `ulong` | per-block 64 terms, no compaction | 1.73e8 | 1.40e9 | SAFE (known item 5 refuted for Metal: no prefix scan, no chunk cap) |
| Metal integer_psnr_hvs_metal.mm:248-262 | `num_blocks = bx * by` (`unsigned`); `(size_t)num_blocks * 64 * 4` | `unsigned`; `size_t` | n/a | 2,707,396 | 21,911,761 | SAFE |

## speed_chroma / speed_temporal (SYCL only; Metal has no SpEED twin)

Geometry (prior G6): the scaled plane is `lround(src * speed_prescale)`, prescale in [0.1, 4.0]; at 16K and prescale 4 it is 61440 x 34560; at the cap and prescale 4, 131072 x 131072. sub_w \* sub_h at 16K p4 = 3836 \* 2156 = 8,270,416; at cap p4 = $8186^{2}$ = 67,010,596. Every pixel sum is fp32 or an fp32 pair; integers are sizes and indices.

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| SYCL speed_sycl_pipeline.cpp:118-130 (`reflect101`) | bounded fold loop | `int32_t` | index in [-2n, 3n] | in range | in range | SAFE |
| SYCL speed_sycl_pipeline.cpp:143-147, 533-534, 592-593 | `tap()`; `row = (int32_t)(i * 16)` | `int32_t` / `uint32_t` | down index \* 16 <= scaled dim | <= 61,440 | <= 131,072 | SAFE |
| SYCL speed_sycl_pipeline.cpp:187, 260-264, 502, 560, 589, 622-632 | `(size_t)row * width + col`; `(size_t)ch * dst_h + y) * dst_w + x`; `ch * plane_size + ...`; `ch * term_size + element * blocks + tile` | `size_t` | up to ch \* scaled N | 8.49e9 (p4) | $2^{36}$ (p4) | SAFE (size_t) |
| SYCL speed_sycl_pipeline.cpp:630 | `tile = (i / 5) * blocks_h + (j / 5)` | `uint32_t` | block index | 331,776 | 2,683,044 | SAFE |
| SYCL speed_sycl_pipeline.cpp:684-688 | `count = static_cast<float>(sub_w * sub_h)` for the mean | `uint32_t` -> `float` | count | 8,270,416 | 67,010,596 | SAFE (the CPU `compute_mean` rounds the count the same way) |
| SYCL speed_sycl_pipeline.cpp:771-779 | `total = sub_w * sub_h`; `pos / sub_w`, `pos % sub_w` | `uint32_t` | n/a | 8,270,416 | 67,010,596 | SAFE |
| \*\*SYCL speed_sycl_pipeline.cpp:808-809\*\* | \*\*covariance divisor `static_cast<float>(a.sub_w * a.sub_h)`, then `ff_div_to_float(sum, count)`\*\* | \*\*`uint32_t` -> `float`\*\* | \*\*count exact in fp32 only below $2^{24}$ (or when its odd part fits 24 bits)\*\* | 8,270,416 at p4: exact for every option value | e.g. W = H = 32740, p4: $8181^{2}$ = 66,928,761 (odd, > $2^{24}$) rounds | \*\*OVERFLOW@CAP-ONLY\*\* (known item 4, confirmed; not a wrap: the CPU `speed.c:847` divides by the exact double count, so above 16K with prescale > ~2 the twin is no longer bit-exact. Same as CUDA/HIP prior rows 4-5. Being fixed separately) |
| SYCL speed_sycl_pipeline.cpp:840-846 (`covariance_group_size`), 1874 | `terms / 8u`; `group *= 2` capped at 256 | `uint32_t` | n/a | 256 | 256 | SAFE |
| SYCL speed_sycl_pipeline.cpp:1484-1527 | `q[i * 25 + k] * b[(size_t)k * stride]`; `(size_t)ch * 25 * blocks + block` | `uint32_t` / `size_t` | n/a | 10,732,176 | 85,857,408 | SAFE |
| SYCL speed_sycl_pipeline.cpp:1604-1624, 2026, 2073-2085, 2154-2159 | `plane_bytes = (size_t)src_w * src_h * bps`; `ch * g.scaled_w * g.scaled_h` (`size_t ch` first); staging offsets | `size_t` | n/a | 3.4e10 B (p4 scaled) | $2^{38}$ B (p4) | SAFE (size_t; device capacity, not a wrap) |
| SYCL speed_sycl_pipeline.cpp:1629-1632 | tail layout `bytes / 4` (`speed_gpu_tail_layout`, uint32, prior G6) | `uint32_t` | n/a | 5,308,848 | 42,929,136 | SAFE |
| SYCL speed_temporal_sycl.cpp:236-245 | `(int32_t)(2 * (index % 2))`, `(index + 1u) % 2` | `unsigned` -> `int32_t` | slot | 0..3 | same | SAFE |
| SYCL speed_chroma_sycl.cpp:182, 272-281; speed_temporal_sycl.cpp:277-284 | `(int)chroma_w`; per-frame tally via `speed_internal` (`uint64_t` solves / singular) | `int`; `uint64_t` | +4 / +2 per frame | $2^{62}$ frames | same | SAFE |

## Coverage

Every file under the four scope directories, with the number of table rows that cite it in the file:line column (a row may cite several files). "none" means the file was read and holds no integer accumulator, size product or index math beyond what the note says.

| file | rows / note |
|---|---|
| `core/src/feature/sycl/AGENTS.d/_index.md` | none (agent documentation, no code) |
| `core/src/feature/sycl/AGENTS.d/adm-aim.md` | none (agent documentation, no code) |
| `core/src/feature/sycl/AGENTS.d/adm.md` | none (agent documentation, no code) |
| `core/src/feature/sycl/AGENTS.d/build.md` | none (agent documentation, no code) |
| `core/src/feature/sycl/AGENTS.d/cambi.md` | none (agent documentation, no code) |
| `core/src/feature/sycl/AGENTS.d/ciede.md` | none (agent documentation, no code) |
| `core/src/feature/sycl/AGENTS.d/clang-tidy.md` | none (agent documentation, no code) |
| `core/src/feature/sycl/AGENTS.d/dmabuf-vaapi.md` | none (agent documentation, no code) |
| `core/src/feature/sycl/AGENTS.d/exact-twins.md` | none (agent documentation, no code) |
| `core/src/feature/sycl/AGENTS.d/float-adm.md` | none (agent documentation, no code) |
| `core/src/feature/sycl/AGENTS.d/float-moment.md` | none (agent documentation, no code) |
| `core/src/feature/sycl/AGENTS.d/float-motion.md` | none (agent documentation, no code) |
| `core/src/feature/sycl/AGENTS.d/float-psnr.md` | none (agent documentation, no code) |
| `core/src/feature/sycl/AGENTS.d/float-ssim.md` | none (agent documentation, no code) |
| `core/src/feature/sycl/AGENTS.d/float-vif.md` | none (agent documentation, no code) |
| `core/src/feature/sycl/AGENTS.d/fp64-fallback.md` | none (agent documentation, no code) |
| `core/src/feature/sycl/AGENTS.d/init-unwind.md` | none (agent documentation, no code) |
| `core/src/feature/sycl/AGENTS.d/integer-psnr.md` | none (agent documentation, no code) |
| `core/src/feature/sycl/AGENTS.d/integer-ssim.md` | none (agent documentation, no code) |
| `core/src/feature/sycl/AGENTS.d/integer-vif.md` | none (agent documentation, no code) |
| `core/src/feature/sycl/AGENTS.d/kernel-identities.md` | none (agent documentation, no code) |
| `core/src/feature/sycl/AGENTS.d/local-accessor.md` | none (agent documentation, no code) |
| `core/src/feature/sycl/AGENTS.d/motion-add-uv.md` | none (agent documentation, no code) |
| `core/src/feature/sycl/AGENTS.d/motion-five-frame-window.md` | none (agent documentation, no code) |
| `core/src/feature/sycl/AGENTS.d/motion-sad-pipeline.md` | none (agent documentation, no code) |
| `core/src/feature/sycl/AGENTS.d/motion-v2.md` | none (agent documentation, no code) |
| `core/src/feature/sycl/AGENTS.d/motion.md` | none (agent documentation, no code) |
| `core/src/feature/sycl/AGENTS.d/ms-ssim.md` | none (agent documentation, no code) |
| `core/src/feature/sycl/AGENTS.d/option-table-sync.md` | none (agent documentation, no code) |
| `core/src/feature/sycl/AGENTS.d/orientation.md` | none (agent documentation, no code) |
| `core/src/feature/sycl/AGENTS.d/parity-tests.md` | none (agent documentation, no code) |
| `core/src/feature/sycl/AGENTS.d/psnr-hvs.md` | none (agent documentation, no code) |
| `core/src/feature/sycl/AGENTS.d/queue-sync.md` | none (agent documentation, no code) |
| `core/src/feature/sycl/AGENTS.d/scratch-memory.md` | none (agent documentation, no code) |
| `core/src/feature/sycl/AGENTS.d/speed.md` | none (agent documentation, no code) |
| `core/src/feature/sycl/AGENTS.d/ssimulacra2.md` | none (agent documentation, no code) |
| `core/src/feature/sycl/AGENTS.d/strict-fp.md` | none (agent documentation, no code) |
| `core/src/feature/sycl/AGENTS.d/sub-group-size.md` | none (agent documentation, no code) |
| `core/src/feature/sycl/AGENTS.d/tile-index.md` | none (agent documentation, no code) |
| `core/src/feature/sycl/AGENTS.d/zero-copy-admission.md` | none (agent documentation, no code) |
| `core/src/feature/sycl/AGENTS.md` | none (agent documentation, no code) |
| `core/src/feature/sycl/float_adm_sycl.cpp` | 4 |
| `core/src/feature/sycl/float_motion_sycl.cpp` | 4 |
| `core/src/feature/sycl/float_psnr_sycl.cpp` | 6 |
| `core/src/feature/sycl/float_vif_sycl.cpp` | 5 |
| `core/src/feature/sycl/integer_adm_sycl.cpp` | 25 |
| `core/src/feature/sycl/integer_cambi_sycl.cpp` | 21 |
| `core/src/feature/sycl/integer_ciede_sycl.cpp` | 4 |
| `core/src/feature/sycl/integer_moment_sycl.cpp` | 8 |
| `core/src/feature/sycl/integer_motion_pipeline_sycl.cpp` | 8 |
| `core/src/feature/sycl/integer_motion_pipeline_sycl.h` | none (declarations, types and constants only) |
| `core/src/feature/sycl/integer_motion_sycl.cpp` | 3 |
| `core/src/feature/sycl/integer_motion_v2_sycl.cpp` | 2 |
| `core/src/feature/sycl/integer_ms_ssim_sycl.cpp` | 4 |
| `core/src/feature/sycl/integer_psnr_hvs_sycl.cpp` | 11 |
| `core/src/feature/sycl/integer_psnr_sycl.cpp` | 9 |
| `core/src/feature/sycl/integer_ssim_sycl.cpp` | 13 |
| `core/src/feature/sycl/integer_vif_sycl.cpp` | 18 |
| `core/src/feature/sycl/speed_chroma_sycl.cpp` | 1 |
| `core/src/feature/sycl/speed_sycl_host.cpp` | none (C entry wrappers into the pipeline) |
| `core/src/feature/sycl/speed_sycl_pipeline.cpp` | 11 |
| `core/src/feature/sycl/speed_sycl_pipeline.h` | none (declarations, types and constants only) |
| `core/src/feature/sycl/speed_temporal_sycl.cpp` | 2 |
| `core/src/feature/sycl/ssimulacra2_sycl.cpp` | 13 |
| `core/src/feature/sycl/sycl_ciede_math.h` | 1 |
| `core/src/feature/sycl/sycl_compat.h` | none (declarations, types and constants only) |
| `core/src/feature/sycl/sycl_exact_fp.h` | 1 |
| `core/src/feature/sycl/sycl_ff_math.h` | 1 |
| `core/src/feature/sycl/sycl_float_adm_math.h` | 2 |
| `core/src/feature/sycl/sycl_float_vif_math.h` | 1 |
| `core/src/feature/sycl/sycl_integer_ssim_math.h` | 2 |
| `core/src/feature/sycl/sycl_integer_vif_math.h` | 3 |
| `core/src/feature/sycl/sycl_ordered_sum.h` | 5 |
| `core/src/feature/sycl/sycl_soft_double.h` | 5 |
| `core/src/feature/sycl/sycl_soft_signed.h` | 3 |
| `core/src/feature/sycl/sycl_ssim_terms.h` | 1 |
| `core/src/feature/sycl/sycl_ssimulacra2_math.h` | 1 |
| `core/src/feature/sycl/sycl_tile_index.h` | 1 |
| `core/src/sycl/AGENTS.md` | none (agent documentation, no code) |
| `core/src/sycl/__pycache__/check_aot_image.cpython-314.pyc` | none (compiled Python cache of check_aot_image.py) |
| `core/src/sycl/check_aot_image.py` | none (build-time Python, unbounded integers) |
| `core/src/sycl/coff_add_anchor.py` | none (build-time Python, unbounded integers) |
| `core/src/sycl/common.cpp` | 6 |
| `core/src/sycl/common.h` | none (declarations, types and constants only) |
| `core/src/sycl/d3d11_import.cpp` | 1 |
| `core/src/sycl/dispatch_strategy.cpp` | 1 |
| `core/src/sycl/dispatch_strategy.h` | none (declarations, types and constants only) |
| `core/src/sycl/dmabuf_import.cpp` | 6 |
| `core/src/sycl/dmabuf_import.h` | none (declarations, types and constants only) |
| `core/src/sycl/picture_sycl.cpp` | 4 |
| `core/src/sycl/picture_sycl.h` | none (declarations, types and constants only) |
| `core/src/sycl/scratch_check.cpp` | 1 |
| `core/src/sycl/scratch_check.h` | none (declarations, types and constants only) |
| `core/src/sycl/scratch_ratchet.txt` | none (extractor list) |
| `core/src/feature/metal/AGENTS.md` | none (agent documentation, no code) |
| `core/src/feature/metal/float_adm.metal` | 2 |
| `core/src/feature/metal/float_adm_metal.mm` | 1 |
| `core/src/feature/metal/float_moment.metal` | 6 |
| `core/src/feature/metal/float_moment_metal.mm` | 1 |
| `core/src/feature/metal/float_motion.metal` | 4 |
| `core/src/feature/metal/float_motion_metal.mm` | 1 |
| `core/src/feature/metal/float_ms_ssim.metal` | 3 |
| `core/src/feature/metal/float_ms_ssim_metal.mm` | 1 |
| `core/src/feature/metal/float_ms_ssim_option_semantics.h` | 1 |
| `core/src/feature/metal/float_psnr.metal` | 2 |
| `core/src/feature/metal/float_psnr_metal.mm` | 1 |
| `core/src/feature/metal/float_ssim.metal` | 2 |
| `core/src/feature/metal/float_ssim_metal.mm` | 1 |
| `core/src/feature/metal/float_vif.metal` | 3 |
| `core/src/feature/metal/float_vif_metal.mm` | 2 |
| `core/src/feature/metal/integer_adm.metal` | 14 |
| `core/src/feature/metal/integer_adm_metal.mm` | 1 |
| `core/src/feature/metal/integer_adm_metal_host.c` | 3 |
| `core/src/feature/metal/integer_adm_metal_host.h` | none (declarations, types and constants only) |
| `core/src/feature/metal/integer_cambi.metal` | 2 |
| `core/src/feature/metal/integer_cambi_metal.mm` | 1 |
| `core/src/feature/metal/integer_ciede.metal` | 1 |
| `core/src/feature/metal/integer_ciede_metal.mm` | 2 |
| `core/src/feature/metal/integer_motion.metal` | 2 |
| `core/src/feature/metal/integer_motion_metal.mm` | 2 |
| `core/src/feature/metal/integer_motion_v2.metal` | 6 |
| `core/src/feature/metal/integer_motion_v2_metal.mm` | 1 |
| `core/src/feature/metal/integer_psnr.metal` | 4 |
| `core/src/feature/metal/integer_psnr_hvs.metal` | 2 |
| `core/src/feature/metal/integer_psnr_hvs_metal.mm` | 1 |
| `core/src/feature/metal/integer_psnr_metal.mm` | 4 |
| `core/src/feature/metal/integer_ssim.metal` | 3 |
| `core/src/feature/metal/integer_ssim_metal.mm` | 2 |
| `core/src/feature/metal/integer_vif.metal` | 11 |
| `core/src/feature/metal/integer_vif_metal.mm` | 1 |
| `core/src/feature/metal/metal_ciede_math.h` | 1 |
| `core/src/feature/metal/metal_float_adm_math.h` | 1 |
| `core/src/feature/metal/metal_float_moment_math.h` | 1 |
| `core/src/feature/metal/metal_float_moment_sum.h` | 2 |
| `core/src/feature/metal/metal_float_motion_math.h` | 2 |
| `core/src/feature/metal/metal_float_psnr_math.h` | 1 |
| `core/src/feature/metal/metal_float_vif_math.h` | 2 |
| `core/src/feature/metal/metal_integer_adm_math.h` | 3 |
| `core/src/feature/metal/metal_integer_adm_uniforms.h` | 1 |
| `core/src/feature/metal/metal_integer_motion_math.h` | 2 |
| `core/src/feature/metal/metal_integer_ssim_math.h` | 3 |
| `core/src/feature/metal/metal_integer_vif_gain.h` | 1 |
| `core/src/feature/metal/metal_integer_vif_math.h` | 1 |
| `core/src/feature/metal/metal_ms_ssim_math.h` | 1 |
| `core/src/feature/metal/metal_portable.h` | 1 |
| `core/src/feature/metal/metal_psnr_hvs_math.h` | 2 |
| `core/src/feature/metal/metal_soft_double.h` | 3 |
| `core/src/feature/metal/metal_soft_signed.h` | 1 |
| `core/src/feature/metal/metal_ssim_terms.h` | 1 |
| `core/src/feature/metal/ssimulacra2.metal` | 2 |
| `core/src/feature/metal/ssimulacra2_metal.mm` | 3 |
| `core/src/metal/AGENTS.md` | none (agent documentation, no code) |
| `core/src/metal/common.h` | none (declarations, types and constants only) |
| `core/src/metal/common.mm` | none (device selection, dispatch table, ENOSYS stubs) |
| `core/src/metal/dispatch_strategy.c` | none (device selection, dispatch table, ENOSYS stubs) |
| `core/src/metal/dispatch_strategy.h` | none (declarations, types and constants only) |
| `core/src/metal/import.h` | none (declarations, types and constants only) |
| `core/src/metal/iosurface_layout.h` | 2 |
| `core/src/metal/kernel_template.h` | none (declarations, types and constants only) |
| `core/src/metal/kernel_template.mm` | 1 |
| `core/src/metal/meson.build` | none (build description) |
| `core/src/metal/picture_import.mm` | 1 |
| `core/src/metal/picture_metal.h` | none (declarations, types and constants only) |
| `core/src/metal/picture_metal.mm` | 1 |
| `core/src/metal/state_priv.h` | none (declarations, types and constants only) |
| `core/src/metal/stubs.c` | none (device selection, dispatch table, ENOSYS stubs) |
