<!-- markdownlint-disable MD001 MD004 MD013 MD024 MD029 MD032 MD036 MD060 -->

# Accumulator bounds: CPU and SIMD

Appendix of [integer accumulator bounds](accumulator-bounds.md): every integer accumulator, size product and offset of the CPU feature extractors, their x86 (AVX2, AVX-512) and arm64 (NEON, SVE2) SIMD paths and the `core/src/` runtime, read on master `571565a47` (before the fixes the main page lists). Rows marked OVERFLOW or DEPENDS name their state row on the main page.

- Source: master `571565a47`.
- Mode: read only. Nothing was edited, built or run. Commands: `grep`, `sed`, `cat`, and one `python3` call that recomputed the bounds quoted below.
- Paths: relative to `core/src/feature/` unless they start with `core/src/`.
- Envelope: 16K W ≤ 15360, H ≤ 8640, N = 132,710,400; 8K DCI 8192x4320; cap W, H ≤ 32768 (`VMAF_PIC_DIM_MAX`, `core/src/picture.c:46`), N ≤ $2^{30}$; samples up to 16 bit; 4:4:4; worst-case content.
- Group of a row: `x86/...` = x86 SIMD, `arm64/...` = arm64 SIMD (NEON and SVE2), everything else = CPU scalar (feature code plus the runtime size products in `core/src/`).
- Already established (cited, not re-derived): integer_psnr line SSE; psnr apsnr clip sum (DEPENDS; fixed since, `T-PSNR-APSNR-CLIP-SSE-UINT64-WRAP-2026-10-05`); integer_motion / motion_v2 `row_sad`; integer_vif frame accumulators; integer ADM `csf_den` (shift adapts); integer ADM scale-0 CM row int64 (OVERFLOW at W = 31–32 / 63–64, fix to uint64 in flight); cambi histograms uint16 (window ≤ 65, counts ≤ 4225); integer_ssim window moments int64 (≤ $2^{48}$). The SIMD twins of each were checked here.
- "Out-of-range input": libvmaf does not range-check samples (cambi is the exception, `cambi.c:990-1029`), so a 10- or 12-bit picture can carry 16-bit values. Rows where that makes a CPU integer accumulator or narrowing wrap are DEPENDS (out-of-range input), because the GPU twins can differ there.

## Summary

| group | rows | SAFE | OVERFLOW@16K | OVERFLOW@CAP-ONLY | DEPENDS |
|---|---|---|---|---|---|
| CPU scalar (feature code + `core/src/` runtime) | 151 | 137 | 1 | 3 | 10 |
| x86 SIMD (AVX2, AVX-512) | 96 | 81 | 3 | 0 | 12 |
| arm64 SIMD (NEON, SVE2) | 33 | 28 | 0 | 0 | 5 |
| total | 280 | 246 | 4 | 3 | 27 |

Of the 27 DEPENDS rows, 1 depends on clip length (apsnr), 20 on out-of-range input, 6 on the ADM CSF weight option.

New defects (not on the established list):

1. \*\*x86 `sad_avx512` (test-only sub-kernel): int16 lane wrap on 16-bit samples\*\* (OVERFLOW@16K, independent of size).
2. \*\*float_vif / SpEED with `vif_prescale` or `speed_prescale` above √2 at 32768²: `int` pixel index overflows\*\* (OVERFLOW@CAP-ONLY, non-default option; SAFE at 16K for every prescale up to the option maximum of 4.0).
3. \*\*AVX2 / AVX-512 integer motion `x_conv`: int32 lane products wrap on out-of-range samples where the scalar's int64 does not\*\* (DEPENDS: out-of-range input; the twins then differ from the scalar before the shared `row_sad` wrap).
4. \*\*AVX2, AVX-512 and NEON integer VIF 16-bit vertical / subsample passes keep the 32-bit filtered mean where the scalar narrows it to uint16\*\* (DEPENDS: out-of-range input; then 32-bit packed-pair carries / 16-bit-half multiplies give garbage).
5. \*\*AVX2 ADM scale-0 CSF `flt` saturates (`packs_epi32`) where the scalar and AVX-512 wrap the int16\*\* (DEPENDS: CSF weight option, h/v weight ≥ 43,900).
6. \*\*psnr_hvs (scalar, AVX2, NEON) DCT on out-of-range samples overflows int\*\* (DEPENDS: out-of-range input; bpc ≤ 12 is guarded but the sample values are not).
7. \*\*integer_vif scalar and ADM 16-bit DWT narrowings on out-of-range samples\*\* (DEPENDS: out-of-range input).

## Every row that is not SAFE

### OVERFLOW@16K

1. \*\*CPU `integer_adm_kernels.h:1017-1019` (`inner[]`), `:1079-1099` (`adm_cm_rows`), fold `:892` + `adm_cm_accumulator.h:30-34`: scale-0 contrast-masking row total, `int64_t`.\*\* Established (fixed since: summed in `uint64_t`, `T-ADM-CM-SCALE0-ROW-INT64-OVERFLOW-2026-10-05`). Term `((x² + 2^28) >> 29)·x >> (ceil(log2 Wb) − 4)` (h/v; d uses 30 / −3); worst case thr = 0 (ref == dis or flat ref), x = |band|·weight. Row max at default weights: 0.855·INT64_MAX at W = 15360, 0.912 at 8K DCI and at the cap; \*\*overflows at W = 31–32 and 63–64\*\* (1.021·INT64_MAX at 16-bit, W = 63–64), which lies inside the envelope. Non-default CSF weights overflow at every size (G1, `audit-cuda-hip.md`).
2. \*\*x86 `x86/adm_avx2.c:1274-1292` (`cm_accum_avx2`), `:1365-1385` (`cm_row_avx2`): same row, biased uint64 lanes.\*\* Each term is added as `(a + 2^63) >>> n`; the lanes wrap mod $2^{64}$ by design and the row is `hsum − lanes·(2^63 >> n)` mod $2^{64}$, read back as int64 (`cm_as_int64`). That equals the true row sum exactly when the true sum fits int64, so the AVX2 row inherits row 1 bit for bit (same W = 31–32 / 63–64 overflow; SAFE at 16K and cap at default weights). The rows of W = 63–64 take this vector path (cols ≈ 26 ≥ 6).
3. \*\*x86 `x86/adm_avx512.c:1310-1327` (`cm_accum_avx512`), `:1394-1410` (`cm_row_avx512`): same row, int64 lanes with native `srai`, `hsum_epi64`.\*\* Every lane holds a subset of the row's non-negative terms, so no lane wraps before the row does; the final sum inherits row 1 (cols ≈ 26 ≥ 14 at W = 63–64, so the vector path runs).
4. \*\*x86 `x86/motion_avx512.c:374-375` `sad_avx512()`: `_mm512_sub_epi16(va, vb)` then `_mm512_abs_epi16` on uint16 samples.\*\* The difference is formed in signed int16 lanes. For 16-bit content with |a − b| > 32767 it wraps: a = 65535, b = 0 gives −1, |·| = 1 instead of 65535; a = 40000, b = 0 gives 25536. The scalar tail (`:384-386`) uses `int` and is right, so the vector and tail columns disagree. Independent of frame size; samples ≤ 32767 (bpc ≤ 15 in range) are exact. Reached only from `core/test/test_motion_avx512_parity.c` (the header says "sub-kernel functions used by unit tests"); no production caller. Fix: `_mm512_max_epu16(a,b) − _mm512_min_epu16(a,b)` (or widen first). The lane sum (`:376-382`, ≤ 2048·65535 per lane, `(uint32_t)` reduce ≤ 32768·65535 < $2^{32}$) is SAFE.

### OVERFLOW@CAP-ONLY

5. \*\*CPU `vif_tools.c:649` (bicubic), `:733` (lanczos4), `:840` (nearest, the default `vif_prescale_method` / `speed_prescale_method`): `dst[y * dst_stride + x]`, `int`.\*\* Called from `float_vif.c:375-381` with `dst_stride = scaled_float_stride / 4` and from SpEED (`speed.c:1211-1213`, `speed_internal.c:137`) with the alloc-width stride. With prescale p the output plane is (W·p)×(H·p). 16K, p = 4.0 (option max, `float_vif.c:127`, `speed.c:1522`): max index 34,559·61,440 + 61,439 = 2,123,366,399 < INT32_MAX (1.1 % margin): SAFE. Cap: (32768·p)² ≥ $2^{31}$ once p ≥ √2 ≈ 1.4142; at p = 4 the index reaches $2^{34}$. Signed overflow UB, out-of-bounds writes. Default p = 1.0 (`vif_options.h:43`, `speed.c:1489,1706`) only copies (`memcpy`). The bilinear path (`:767-769`) uses `size_t` and is safe. Memory for such a plane is ≥ 8.6 GB per float buffer, so this is reachable only on very large hosts, but nothing rejects it (`vif.c:86-94` bounds only the row width).
6. \*\*CPU `vif_tools.c:193` (`vif_dec2_s`), `:206` (`vif_dec16_s`), `:221` (`vif_sum_s`), `:328-331` (`vif_statistic_s`), `:378`, `:397`, `:416-417` (vertical filter passes): `i * px_stride + j` in `int`.\*\* float_vif runs `compute_vif()` (`vif.c:241-262`) on the prescaled plane; SpEED runs `vif_filter1d_s`, `vif_dec16_s` and `vif_filter1d_dec16_s` (`speed.c:1238-1250`) on it. Same bound as row 5: SAFE at 16K for p ≤ 4 (≤ 2,123,366,399), overflows at the cap for p > √2.
7. \*\*CPU `speed.c:1189-1214` (`speed_prescale_frame`) and `speed_internal.c:131-139`: SpEED's resample and filter call sites.\*\* They hand `(int)` widths and strides to the `vif_tools.c` functions of rows 5 and 6; listed separately because they are the SpEED entry (speed_chroma, speed_temporal). Same verdict and threshold (speed_prescale > √2 at 32768², non-bilinear method, which includes the default "nearest").

### DEPENDS

Clip-level (frame count):

8. \*\*CPU `integer_psnr.c:211`, `:249` `s->apsnr.sse[p] += sse`, `uint64_t`\*\* (established; fixed since, `T-PSNR-APSNR-CLIP-SSE-UINT64-WRAP-2026-10-05`). Frames to overflow at 16-bit max difference (65535² = 4,294,836,225 per pixel): 1080p 2071.3 (wraps in frame 2072), 8K DCI 121.4 (frame 122), 16K 32.4 (frame 33), cap 4.0001 (frame 5). At 10-bit: 132,820 frames at 16K. At 8-bit: 2.14e6 frames at 16K. (`n_pixels` at `:212,250` is SAFE: 1.4e11 frames at 16K.)

Out-of-range input (samples above $2^{\mathrm{bpc}} - 1$):

9. \*\*CPU `integer_motion.c:251` and 10. `integer_motion_v2.c:255`: 16-bit `row_sad`, `uint32_t`\*\* (established). In range |val| ≤ 65,535 (bpc 16) → row ≤ 32768·65535 = 2,147,450,880 < $2^{32}$. Out of range the y-conv output is (65536·65535) >> bpc: 4,194,240 at bpc 10 (wraps at W ≥ 1025), 1,048,560 at bpc 12 (W ≥ 4097). Wraps mod $2^{32}$; the uint64 frame sum then under-counts.
11. \*\*x86 `x86/motion_avx2.c:91-92` (`x_conv_abs8_avx2`) and 12. `x86/motion_avx512.c:83-84` (`x_conv_abs16_avx512`): `_mm256/512_mullo_epi32(y, g)` and the pair sums `s04`, `s13` in int32 lanes.\*\* In range |y| ≤ 65,535: s13 = 2·16004·65535 = 2,097,644,280 < INT32_MAX (2.3 % margin), y2·g2 = 1,729,206,510: SAFE. Out of range they wrap once |y| > 67,092 (s13) or 81,387 (y2·26386), i.e. for essentially every out-of-range bpc-10 sample, where the scalar forms the same sum in int64 (`integer_motion.c:245-249`). AVX2 / AVX-512 therefore diverge from the scalar before the `row_sad` wrap of rows 9–10.
13. \*\*arm64 `arm64/motion_v2_neon.c:145-149`: `sad_acc` (uint32x4) and `vaddvq_u32`.\*\* The NEON x-conv is int64 (`vmull_s32`, `:96-106`), so lanes see the scalar's values; per lane ≤ (W/4)·4,194,240 out of range, wrapping mod $2^{32}$. The row total mod $2^{32}$ equals the scalar's wrapped `row_sad` bit for bit. In range: lane ≤ 8192·65535, row ≤ 2,147,450,880: SAFE.
14. \*\*CPU `integer_vif.c:205` (`subsample_rd_16`, scale 0) and 15. `integer_vif.c:450-452` (`vif_vertical_line_16`, scale 0): `(uint16_t)((accum + 2^(b−1)) >> b)` and `(uint32_t)((accum_ref + r) >> 2(b−8))`.\*\* The uint32 / uint64 accumulators themselves never wrap (≤ 65536·65535 + 32768 = 4,294,934,528; ≤ 2.81e14). In range the narrowings are exact (mean ≤ 65,535; moment < $2^{32}$). Out of range at bpc 10 the mean reaches 4,194,240 and the moment 65536·65535²/16 = 1.76e13; both are truncated (mod $2^{16}$, mod $2^{32}$).
16. \*\*x86 `x86/vif_avx2.c:659-672` (`vif_vertical16_store_mean`), used by the statistic and by `vif_subsample16_vertical` (`:962-979`).\*\* Stores `(acc + 2^(b−1)) >>> b` as a full 32-bit value; the scalar narrows to uint16 (row 14–15). In range identical (≤ 65,535; the lane sum ≤ 4,294,934,528 < $2^{32}$ even read unsigned). Out of range the stored mean exceeds 65,535, then `vif_mean256` (`:314-337`) multiplies it in 32 bits (43,728·4,194,240 = 1.8e11 wraps) and adds packed 32-bit products with `add_epi64`, carrying into the neighbour dword; and the subsample horizontal pass (`vif_multiply16`, `:618-624`, `:857-863`) multiplies the two 16-bit halves of each 32-bit value separately. Garbage, and different from the scalar.
17. \*\*x86 `x86/vif_avx2.c:999-1030` (`vif_subsample16_horizontal`, `_block`)**: the 16-bit-half multiply of row 16 on the un-narrowed `ref_convol`. Listed with row 16's cause; SAFE in range (value ≤ 65,535, high half 0, sum ≤ 65536·65535 + 32768).
18. \*\*x86 `x86/vif_avx512.c:639-647` (`vif_vertical_store_mean16`)\*\* and 19. \*\*`x86/vif_avx512.c:1191-1215`, `:1270-1311` (subsample16 store and horizontal)**: same as rows 16–17 for AVX-512.
20. \*\*arm64 `arm64/vif_neon.c:874-900` (`vif_stat16_vertical8` → `vif_store_round_shift_u32`, `:199-204`)\*\* and 21. \*\*`arm64/vif_neon.c:284-293`, `:373-390` (`vif_subsample16_vertical16`, `vif_subsample_horizontal16`)**: NEON stores the un-narrowed 32-bit mean too; out of range the u32 `vmlaq_n_u32` sums of the next pass (65536·4.19e6) wrap mod $2^{32}$. In range SAFE (≤ 4,294,934,528).
22. \*\*CPU `integer_adm_kernels.h:1318-1323`: `(int16_t)adm_dwt2_vpass16_tap4(...)`\*\* (the int64 tap itself, `integer_adm.h:199-209`, is SAFE). In range |result| ≤ 27,411. Out of range at bpc 10: (50582·65535 − 46342·512 + 512) >> 10 = 3,214,028 → int16 wrap. The AVX2 / AVX-512 16-bit DWT call this same scalar pass (`x86/adm_avx2.c:729`, `x86/adm_avx512.c:771`), so they inherit it; the CUDA / HIP twins narrow the same way (G1).
23. \*\*CPU `third_party/xiph/psnr_hvs.c:127-179` (`od_bin_fdct8` lifting products `(t·K + r) >> s`, `int`)\*\* and 24. \*\*`psnr_hvs.c:357`, `:362` (`dct·dct`, `int`)**. `init` rejects bpc > 12 (`:440`), which bounds the products at $2^{28.5}$ and the AC squares at $2^{28}$ (the affine bound of the CAMBI and PSNR-HVS group) — but only for in-range samples. A 10- or 12-bit picture with 16-bit values feeds the DCT 16x larger inputs: products reach $2^{32.5}$, squares $2^{36}$. Signed int overflow (UB).
25. \*\*x86 `x86/psnr_hvs_avx2.c:89-100` (`od_mulrshift_avx2`, `_mm256_mullo_epi32`)\*\* and 26. \*\*`x86/psnr_hvs_avx2.c:461-466` (`dct·dct`, `int`)**: same as rows 23–24; the vector product wraps mod $2^{32}$ where the scalar is UB.
27. \*\*arm64 `arm64/psnr_hvs_neon.c:105-112` (`vmulq_s32`)\*\* and 28. \*\*`arm64/psnr_hvs_neon.c:500-505` (`dct·dct`)**: same.

CSF weight option (not frame size; shared with the GPU twins):

29. \*\*CPU `integer_adm_kernels.h:488-489`: `flt = (int16_t)((4369·|i16| + 2048) >> 12)`\*\* and \*\*`integer_adm_kernels.h:872`: `v_sq = (int32_t)((v² + 2^28) >> 29)`\*\* (one row each in the ADM section). Default weights: flt ≤ 27,212, v_sq ≤ 2.127e9: SAFE. An h/v weight in [43,900, 46,603) (`adm_csf_scale` in Barten modes, or viewing geometry) wraps flt negative; a negative threshold then lets the excess reach INT32_MAX and v_sq ($2^{33}$) wraps.
30. \*\*x86 `x86/adm_avx2.c:891-894` (`csf_block_avx2`): `flt` through `_mm256_packs_epi32`\*\*, which \*\*saturates\*\* to 32,767 where the scalar wraps to e.g. 34,952 − 65,536 = −30,584. Under that option AVX2 diverges from the scalar and from AVX-512 (`x86/adm_avx512.c:930-932` truncates with `_mm512_cvtepi32_epi16`, like the scalar; listed as its own DEPENDS row). The AVX2 and AVX-512 squared-excess narrowings (`mul_epi32` reads the low 32 bits signed, `x86/adm_avx2.c:1280-1283`, `x86/adm_avx512.c:1316-1319`) match the scalar's `(int32_t)` and carry the same option dependence (one DEPENDS row each).

Not counted as defects but worth noting:
- `third_party/xiph/psnr_hvs.c:311-317` (and the AVX2 / NEON copies): `(y + i)·_systride + (j + x)·2 + 1` in `int` reaches exactly 32767·65536 + 65535 = 2,147,483,647 = INT32_MAX at the cap (16-bit storage). SAFE with zero margin: a picture with a stride above 2·W (not produced by `vmaf_picture_alloc`) would wrap.
- `integer_adm_kernels.h:519-526` (called at `:567`, `:1153`): `add_bef_shift_flt = (int32_t)(1u << 31)` = INT32_MIN (ADR-0155 upstream rounding quirk). Implementation-defined conversion, not an accumulator; AVX2 / AVX-512 reproduce it.

## PSNR (integer_psnr, psnr_score.h)

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| integer_psnr.c:121-124 | `sse` (`sse_line_8_c`), `e*e` with `int16_t e` | `uint32_t` | one row: W terms ≤ 255² = 65,025 | 15360·65025 = 998,784,000 | 32768·65025 = 2,130,739,200 | SAFE (established; < $2^{32}$, even < $2^{31}$) |
| integer_psnr.c:131-134 | `sse` (`sse_line_16_c`), `(uint64_t)e*e`, `e = abs(int diff)` | `uint64_t` | one row: W terms ≤ 65535² | 6.6e13 | $2^{47}$ | SAFE (established) |
| integer_psnr.c:201-205 | frame `sse += sse_fn(...)` 8-bit | `uint64_t` | H line sums | N·65025 = 8.63e12 | 6.98e13 | SAFE ($2^{46}$ < $2^{64}$) |
| integer_psnr.c:239-243 | frame `sse` 16-bit (also 10/12-bit, any uint16 value) | `uint64_t` | H line sums, ≤ 65535² per pixel | 5.70e17 | 4.61e18 | SAFE ($2^{62}$ < $2^{64}$; out-of-range samples are still ≤ 65535) |
| integer_psnr.c:211, 249 | `s->apsnr.sse[p] += sse` | `uint64_t` | clip: one frame sum per frame | 32.4 frames at 16-bit max diff | 4.0 frames | DEPENDS (established; frames-to-overflow 1080p 2071, 8K DCI 121, 16K 32, cap 4) |
| integer_psnr.c:212, 250 | `apsnr.n_pixels[p] += (uint64_t)h*w` | `uint64_t` | clip: N per frame | 1.4e11 frames to overflow | 1.7e10 frames | SAFE |
| integer_psnr.c:215, 253 | `ref_pic->w[p] * ref_pic->h[p]` | `unsigned` | size product | 1.33e8 | $2^{30}$ | SAFE (< $2^{32}$) |
| psnr_score.h:31-34 | `255u << (bpc − 8)`, `(1u << bpc) − 1u` | `uint32_t` | peak | ≤ 65,535 | same | SAFE |
| x86/psnr_avx2.c:45-53 | `_mm256_madd_epi16(diff, diff)` → `sum` (8 × int32 lanes) | `__m256i` epi32 | 8-bit diff \|d\| ≤ 255, pair ≤ 130,050; W/8 terms per lane | 1920·65025 = 1.25e8 per lane | 4096·65025 = 2.66e8 | SAFE |
| x86/psnr_avx2.c:57-62 | horizontal sum → `uint32_t result` | `uint32_t` | W terms ≤ 65,025 | 998,784,000 | 2,130,739,200 | SAFE (same as scalar line) |
| x86/psnr_avx2.c:90, 96 | `_mm256_mullo_epi32(diff, diff)` 16-bit | epi32 lane | 65535² = 4,294,836,225 > INT32_MAX | lane holds the uint32 bit pattern | same | SAFE (true square < $2^{32}$; read back with `_mm256_cvtepu32_epi64`) |
| x86/psnr_avx2.c:99-113 | `sum0`/`sum1` → `uint64_t result` | epi64 lanes | W/16 squares per lane | 4.1e12 | $2^{43}$ | SAFE |
| x86/psnr_avx512.c:50-62 | `madd_epi16` → 16 × int32 lanes → `_mm512_reduce_add_epi32` → `uint32_t` | epi32 / `uint32_t` | as AVX2 | 998,784,000 | 2,130,739,200 | SAFE |
| x86/psnr_avx512.c:95-115 | `mullo_epi32` squares read via `cvtepu32_epi64`; `_mm512_reduce_add_epi64` | epi64 / `uint64_t` | W squares | 6.6e13 | $2^{47}$ | SAFE |
| arm64/psnr_neon.c:49-63 | `vabdq_u8`, `vmull_u8` (u16 ≤ 65,025), `vaddl_u16` → u32 lanes, `vaddvq_u32` | `uint32x4_t` / `uint32_t` | W/8 pairs per lane | 998,784,000 total | 2,130,739,200 | SAFE |
| arm64/psnr_neon.c:89-102 | `vabdq_u16`, `vmull_u16` (u32 ≤ 4,294,836,225), `vaddl_u32` → u64 | `uint64x2_t` | W squares | 6.6e13 | $2^{47}$ | SAFE |

## float_psnr, psnr.c

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| float_psnr.c:68-75, 175-178 | `noise_` / `accum` | `double` | floating, no integer accumulator | — | — | SAFE |
| float_psnr.c:178 | `noise_ /= (w * h)` | `int` | size product | 1.33e8 | $2^{30}$ | SAFE (< $2^{31}$) |
| float_psnr.c:106-110 | `s->float_stride * h` | `size_t` | buffer bytes | 5.3e8 | $2^{32}$ | SAFE (64-bit size_t) |
| float_psnr_rows.h:37-42 | `row += segments[(size_t)y*per_row + x]` | `uint64_t` | one row of terms in units 1/scaler² (≤ 65535² each) | 6.6e13 | $2^{47}$ | SAFE |
| psnr.c:36-48 | `noise_`, `(ptrdiff_t)i * stride_` | `double` / `ptrdiff_t` | floating, no integer accumulator | — | — | SAFE |
| x86/float_psnr_avx2.c:30-49 | `result` | `double` | floating, no integer accumulator | — | — | SAFE |
| x86/float_psnr_avx512.c:30-49 | `result` | `double` | floating, no integer accumulator | — | — | SAFE |
| arm64/float_psnr_neon.c:31-51 | `dsum0/1` (`float64x2_t`) | `double` | floating, no integer accumulator | — | — | SAFE |

## Motion (integer_motion, integer_motion_v2)

Filter taps {3571, 16004, 26386, 16004, 3571}, sum 65,536 (`integer_motion.h:26`). 8-bit y-conv output ≤ 65,280; 16-bit in range ≤ 65,535; out of range (bpc 10 / 12, samples 65535) ≤ 4,194,240 / 1,048,560.

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| integer_motion.c:179-185 | 8-bit y-conv `accum += (int32_t)filter[k]*diff` | `int32_t` | 5 taps, \|diff\| ≤ 255 | 16,711,680 | same | SAFE |
| integer_motion.c:193-203 | 8-bit `row_sad += abs(val)` | `uint32_t` | W terms ≤ 65,280 | 1,002,700,800 | 2,139,095,040 | SAFE (established; < $2^{32}$) |
| integer_motion.c:229-235 | 16-bit y-conv `accum`, then `(int32_t)` y_row | `int64_t` → `int32_t` | 5 taps × 65535 | ≤ 4.29e9 accum; y ≤ 65,535 (≤ 4,194,240 out of range) | same | SAFE (fits int32 even out of range) |
| integer_motion.c:243-253 | 16-bit `row_sad` | `uint32_t` | W terms ≤ 65,535 in range | 1,006,617,600 | 2,147,450,880 | DEPENDS (established; out-of-range input wraps at W ≥ 1025 (bpc 10) / 4097 (bpc 12)) |
| integer_motion.c:173-203, 223-253 | frame `sad += row_sad` | `uint64_t` | H rows < $2^{32}$ | $2^{45}$ | $2^{47}$ | SAFE |
| integer_motion.c:374 | `(w * h)` | `unsigned` | size product | 1.33e8 | $2^{30}$ | SAFE |
| integer_motion.h:29-53 | `edge_16` `accum += filter[k]*src[...]` (uint16·uint16 → `int`) | `uint32_t` (product `int`) | 5 taps; product ≤ 26386·65535 = 1,729,206,510 | 4,294,901,760; callers add ≤ 32,768 → 4,294,934,528 | same | SAFE (product < INT32_MAX; sum < $2^{32}$, margin 32,768) |
| integer_motion.h:52 | `src[i_tap * stride + j_tap]` | `int` | index in samples | 1.33e8 | 32767·32768 + 32767 = $2^{30} - 1$ | SAFE |
| integer_motion_v2.c:183-189 | 8-bit y-conv `accum` | `int32_t` | as motion | 16,711,680 | same | SAFE |
| integer_motion_v2.c:197-207 | 8-bit `row_sad` | `uint32_t` | as motion | 1,002,700,800 | 2,139,095,040 | SAFE (established) |
| integer_motion_v2.c:247-257 | 16-bit `row_sad` | `uint32_t` | as motion | 1,006,617,600 | 2,147,450,880 | DEPENDS (established; out-of-range input, as integer_motion) |
| integer_motion_v2.c:177-257, 379 | frame `sad` (`uint64_t`); `(w * h)` (`unsigned`) | `uint64_t` / `unsigned` | as motion | $2^{45}$ / 1.33e8 | $2^{47}$ / $2^{30}$ | SAFE |
| x86/motion_avx2.c:236-262 | 8-bit y-conv `mullo_epi16`/`mulhi_epi16` → int32 lanes | epi32 | \|diff\| ≤ 255 × f ≤ 26386 (< 32768) exact 32-bit products; 5 taps | 16,711,680 | same | SAFE |
| x86/motion_avx2.c:163-170 | 16-bit y-conv `mullo_epi32(diff, g)` → int64 lanes; `srlv_epi64` (logical) then low dword | epi32 / epi64 | product ≤ 1,729,206,510; 5 taps | low dword = arithmetic result (shift ≤ 16, \|y\| < $2^{31}$) | same | SAFE |
| x86/motion_avx2.c:91-92 | x-conv `mullo_epi32(y, g)`, pair sums `s04`, `s13` | epi32 lanes | in range \|y\| ≤ 65,535: s13 ≤ 2,097,644,280 | same (per pixel) | same | DEPENDS (out-of-range input: wraps for \|y\| > 67,092; the scalar uses int64) |
| x86/motion_avx2.c:130-135 | `sad_acc` (8 × int32) → `hsum_epi32_avx2` → `uint32_t row_sad` | epi32 / `uint32_t` | (W−4)/8 terms ≤ 65,535 per lane | row ≤ 1,006,617,600 | ≤ 2,147,450,880 | SAFE (in range; out of range covered by the scalar DEPENDS row) |
| x86/motion_avx2.c:209-219, 314-325 | `prev + r * prev_stride` | `int` × `ptrdiff_t` | row pointer | — | — | SAFE |
| x86/motion_avx512.c:83-84 | x-conv `mullo_epi32(y, g)`, `s04`, `s13` | epi32 lanes | as AVX2 | as AVX2 | as AVX2 | DEPENDS (out-of-range input, as AVX2) |
| x86/motion_avx512.c:112-118 | `sad_acc` (16 × int32) → `(uint32_t)_mm512_reduce_add_epi32` | epi32 / `uint32_t` | (W−4)/16 terms per lane | row ≤ 1,006,617,600 | ≤ 2,147,450,880 | SAFE |
| x86/motion_avx512.c:150-159 | 16-bit y-conv `mullo_epi32`, int64 lanes, `srav_epi64`, `cvtsepi64_epi32` (saturating) | epi64 | as AVX2 | in range exact | same | SAFE |
| x86/motion_avx512.c:229-268 | 8-bit y-conv `mullo_epi16`/`mulhi_epi16` → int32 | epi32 | as AVX2 | 16,711,680 | same | SAFE |
| x86/motion_avx512.c:374-375 | `sad_avx512`: `_mm512_sub_epi16(va, vb)`, `_mm512_abs_epi16` on uint16 samples | epi16 lanes | per-sample difference of 16-bit samples | wraps for \|a−b\| > 32767 | same | OVERFLOW@16K (test-only sub-kernel; size-independent; scalar tail correct) |
| x86/motion_avx512.c:376-382 | `sad_avx512` `acc` (16 × int32) → `(uint32_t)reduce` → `uint64_t sad` | epi32 / `uint64_t` | 2·W/32 terms ≤ 65,535 per lane; row ≤ W·65535 | 1,006,617,600 | 2,147,450,880 | SAFE |
| x86/motion_avx512.c:397-400 | `filter5_scalar` (uint32 operands) | `uint32_t` | 5 taps × 65535 | 4,294,901,760 (+ round ≤ 32,768) | same | SAFE (margin 32,768) |
| x86/motion_avx512.c:405-413 | `filter5_epu32_avx512` `mullo_epi32` + `add_epi32` + round, `srli` | epi32 (uint32 bit pattern) | as filter5_scalar | 4,294,934,528 | same | SAFE (< $2^{32}$; logical shift) |
| x86/motion_avx512.c:420-435 | `y_conv_edge_row_8` `accum` | `uint32_t` | 5 taps × 255 | 16,711,680 | same | SAFE |
| x86/motion_avx512.c:520-533, 595-607 | edge rows / columns through `edge_16` | `uint32_t` | as integer_motion.h | 4,294,934,528 | same | SAFE |
| arm64/motion_neon.c:31-46 | `filter5_u16x4_neon` `vmull_u16`/`vmlal_u16` + 32768, `vshrq_n_u32` | `uint32x4_t` | 5 taps × 65535 | 4,294,934,528 | same | SAFE |
| arm64/motion_neon.c:90-99 | scalar tail `accum += filter[k]*sp[k]` | `uint32_t` (product `int`) | 5 taps | 4,294,901,760 | same | SAFE |
| arm64/motion_v2_neon.c:84-108 | x-conv `vmull_s32` → int64, `vmovn_s64` of (sum>>16) | `int64x2_t` | 5 taps | in range and out of range exact (\|y\| ≤ 4,194,240) | same | SAFE |
| arm64/motion_v2_neon.c:145-149 | `sad_acc` (u32 lanes) + `vaddvq_u32` → `row_sad` | `uint32x4_t` / `uint32_t` | W/4 terms per lane | row ≤ 1,006,617,600 | ≤ 2,147,450,880 | DEPENDS (out-of-range input; wraps mod $2^{32}$ exactly like the scalar `row_sad`) |
| arm64/motion_v2_neon.c:160-191 | 16-bit y-conv `vmull_s32` → int64, `vshlq_s64` (arithmetic) | `int64x2_t` | 5 taps | exact | same | SAFE |
| arm64/motion_v2_neon.c:265-292 | 8-bit y-conv `vmulq_s32` | `int32x4_t` | \|diff\| ≤ 255 × 26386, 5 taps | 16,711,680 | same | SAFE |

## float_motion, motion.c

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| float_motion.c:66-74, 201-212 | `accum` (row, frame) | `float` | floating, no integer accumulator | — | — | SAFE |
| float_motion.c:212 | `(w * h)` | `int` | size product | 1.33e8 | $2^{30}$ | SAFE |
| float_motion_sad.h:31-39 | `(float)(int)(w * h)` | `unsigned` → `int` | size product | 1.33e8 | $2^{30}$ | SAFE (< INT32_MAX) |
| motion.c:95-109 | `img1[i * img1_stride + j]`, `(width * height)` | `int` | float-element index; size product | 1.33e8 | $2^{30}$ | SAFE |
| x86/float_motion_avx2.c, x86/float_motion_avx512.c (whole files) | row SAD | `float` | floating, no integer accumulator | — | — | SAFE |
| arm64/float_motion_neon.c (whole file) | row SAD | `float` | floating, no integer accumulator | — | — | SAFE |

## Integer VIF

Filter tables sum to 65,536 per pass (`integer_vif.h:39-44`); the scale-3 centre tap 43,728 exceeds INT16_MAX, so every twin that multiplies it must use unsigned 16-bit multiplies (they do: `mulhi_epu16`, `vmull_n_u16`).

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| integer_vif.c:142-150 | `subsample_rd_8` vertical `accum_ref` | `uint32_t` | 9 taps × 255 | 16,711,680 | same | SAFE |
| integer_vif.c:159-168 | `subsample_rd_8` horizontal `accum_ref` (+32768) | `uint32_t` | 9 taps × 65,280 | 4,278,222,848 | same | SAFE (< $2^{32}$, 0.4 % margin) |
| integer_vif.c:196-205 | `subsample_rd_16` vertical `accum_ref` + $2^{b-1}$, then `(uint16_t)(… >> b)` | `uint32_t` → `uint16_t` | 9 / 5 / 3 taps × 65535 | 4,294,934,528 accum; mean ≤ 65,535 in range | same | DEPENDS (out-of-range input at scale 0: mean up to 4,194,240 truncated mod $2^{16}$) |
| integer_vif.c:214-223 | `subsample_rd_16` horizontal `accum_ref` (+32768) | `uint32_t` | taps × 65535 | 4,294,934,528 | same | SAFE |
| integer_vif.c:259-270 | `vif_horizontal_pixel` `accum_mu1` (`uint32_t`), `accum_ref` (`uint64_t`) | `uint32_t` / `uint64_t` | taps × mean ≤ 65535; taps × moment < $2^{32}$ | 4,294,901,760; 2.8e14 | same | SAFE |
| integer_vif.c:289-291 | `(uint64_t)m.mu1 * m.mu1 + 2^31` | `uint64_t` | ($2^{32}$ − $2^{16}$)² | 1.8446e19 | same | SAFE (< $2^{64}$ by $2^{49}$) |
| integer_vif.c:293-295 | `sigma = (int32_t)(m.xx − mu_sq)` | `uint32_t` → `int32_t` | variance in $2^{32}$ units of (x/$2^{b}$)² | ≤ $2^{30}$ | same | SAFE |
| integer_vif.c:307-308 | `log2_32(..., (uint32_t)sigma_nsq + (uint32_t)sigma1_sq)` | `uint32_t` | 131,072 + σ1 (< $2^{31}$) | < $2^{32}$ | same | SAFE |
| integer_vif.c:322-329 | `numer1 = sv_sq + sigma_nsq` (`uint32_t`); `numer1_tmp = (int64_t)(g²σ1) + numer1` | `uint32_t` / `int64_t` | sv_sq < $2^{31}$; g ≤ 100 | 2.15e13 | same | SAFE |
| integer_vif.c:307, 330, 332-333 | frame `accum_den_log`, `accum_num_log`, `accum_num_non_log`, `accum_den_non_log` | `int64_t` | per pixel ≤ $2^{16}$ (log) / ≤ $2^{31}$ (non-log) / 1 | N·$2^{31}$ = $2^{58}$ | $2^{61}$ | SAFE (established) |
| integer_vif.c:361-372 | `vif_vertical_line_8` `accum_ref` Σ f·x² | `uint32_t` | 17 taps; Σf·x² ≤ 65536·65025 | 4,261,478,400 | same | SAFE (< $2^{32}$, margin 33,488,896) |
| integer_vif.c:440-452 | `vif_vertical_line_16` `accum_mu1` (`uint32_t`), `accum_ref` (`uint64_t`), `(uint16_t)` mean, `(uint32_t)` moment | `uint32_t` / `uint64_t` | taps × 65535 / × 65535² | 4,294,934,528; 2.81e14 | same | DEPENDS (out-of-range input at scale 0: mean and moment truncated; in range exact) |
| integer_vif.c:117-127 | `decimate_and_pad` `ref[i * stride + j]` | `unsigned` × `ptrdiff_t` | index | — | — | SAFE |
| integer_vif.c:543-551 | `frame_size = stride * h`, `data_sz` | `size_t` | buffer bytes | 3.4e9 | 2.6e10 | SAFE |
| integer_vif_sv_sq.h:43-47 | `vif_sv_sq` double → uint32 only inside (0, $2^{31}$) | `uint32_t` | conversion guard | — | — | SAFE |
| integer_vif.h:142-160 | `log2_32`/`log2_64` `table + 2048·k` | `int32_t` | k ≤ 48 | ≤ 1.3e5 | same | SAFE |
| vif_log2_table.h:52-60 | `(uint16_t)roundf(log2f(...)·2048)` | `uint16_t` | ≤ 2048·16 | 32,768 | same | SAFE |
| x86/vif_avx2.c:88-113, 138-189 | 8-bit vertical `multiply2(_and_accumulate)` (`madd_epi16`), `multiply3(_and_accumulate)` (`mullo_epi16`/`mulhi_epu16`) → epi32 | epi32 lanes | Σf·x² ≤ 4,261,478,400 (> INT32_MAX, < $2^{32}$) | stored as uint32 | same | SAFE (lane wrap as signed only; true sum < $2^{32}$, stored unsigned) |
| x86/vif_avx2.c:314-337 | `vif_mean256`: `mullo_epi32` products, `add_epi64` on packed 32-bit pairs | epi32 / epi64 | Σ taps × mean ≤ 4,294,901,760 per dword | no carry between dwords (true dword sum < $2^{32}$) | same | SAFE (margin 65,536) |
| x86/vif_avx2.c:342-353 | `vif_product8` `mul_epu32` + $2^{31}$ | epi64 | ($2^{32}$ − $2^{16}$)² | < $2^{64}$ | same | SAFE |
| x86/vif_avx2.c:355-422, 496-557 | `vif_moment8` / `vif_moment16` 64-bit lanes + 0x8000 | epi64 | taps × moment < $2^{32}$ | 2.8e14 | same | SAFE |
| x86/vif_avx2.c:618-657 | `vif_multiply16` (`mulhi_epu16`/`mullo_epi16`), `vif_accumulate16_moment` (`mul_epu32`) | epi32 / epi64 | pixel·coeff ≤ 2.87e9 (u32), ×pixel ≤ 1.9e14 | 2.81e14 | same | SAFE |
| x86/vif_avx2.c:659-672 | `vif_vertical16_store_mean`: `add_epi32` sum + bias, `srli`, stored as 32-bit | epi32 | 4,294,934,528 | stored without the scalar's uint16 narrowing | same | DEPENDS (out-of-range input; then `vif_mean256` carries; diverges from scalar) |
| x86/vif_avx2.c:799-848 | `vif_subsample8_filter` `madd_epi16` (+128) | epi32 | 9 taps × 255 | 16,711,680 | same | SAFE |
| x86/vif_avx2.c:857-937 | `vif_subsample8_horizontal`: 32-bit `ref_convol` multiplied as 16-bit halves, + 32768, `packus_epi32` | epi32 | 9 taps × 65,280 (high half 0) | 4,278,222,848 | same | SAFE |
| x86/vif_avx2.c:999-1030 | `vif_subsample16_horizontal(_block)` on 32-bit `ref_convol` | epi32 | 9 / 5 / 3 taps × 65,535 | 4,294,934,528 in range | same | DEPENDS (out-of-range input: un-narrowed convol has a nonzero high half) |
| x86/vif_avx2.c:457-486, 756-787 | frame `VifResiduals totals` + tail residuals | `int64_t` | as scalar | $2^{58}$ | $2^{61}$ | SAFE |
| x86/vif_avx512.c:93-160 | log stage: int64 `mnumer1`, `mnumer1_tmp`, `cvttpd_epi64` | epi64 | as scalar | 2.15e13 | same | SAFE |
| x86/vif_avx512.c:170-211, 736-745 | frame `Residuals512` int64 lanes, `_mm512_reduce_add_epi64` | epi64 | N/8 pixels per lane, ≤ $2^{31}$ each | $2^{55}$ per lane | $2^{58}$ per lane, $2^{61}$ total | SAFE |
| x86/vif_avx512.c:226-258 | `vif_horizontal_means512` `mullo_epi32`, packed `add_epi64` | epi32 / epi64 | as AVX2 | ≤ 4,294,901,760 per dword | same | SAFE |
| x86/vif_avx512.c:264-300, 314-345 | mean products `mul_epu32`; energies 64-bit lanes + 0x8000 | epi64 | as AVX2 | 2.8e14 | same | SAFE |
| x86/vif_avx512.c:488-541 | 8-bit vertical `madd_epi16` pairs (x² + x'² ≤ 130,050) × f, `add_epi32` | epi32 | Σ ≤ 4,261,478,400 | stored as uint32 | same | SAFE |
| x86/vif_avx512.c:604-660 | 16-bit vertical weight / energy (unsigned 16-bit multiplies, 64-bit lanes) | epi32 / epi64 | as AVX2 | 2.81e14 | same | SAFE |
| x86/vif_avx512.c:639-647 | `vif_vertical_store_mean16` (no uint16 narrowing) | epi32 | 4,294,934,528 | — | — | DEPENDS (out-of-range input, as AVX2) |
| x86/vif_avx512.c:897-978, 992-1040 | subsample8 `VIF_VERT_MADD5` (paired coefficients), `VIF_HORIZ_TAP8` | epi32 | 9 taps × 255; 9 taps × 65,280 + 32768 | 4,278,222,848 | same | SAFE |
| x86/vif_avx512.c:1191-1311 | subsample16 vertical store (no narrowing) and 16-bit-half horizontal | epi32 | in range ≤ 4,294,934,528 | — | — | DEPENDS (out-of-range input, as AVX2) |
| x86/vif_statistic_avx2.c:35-310 | float VIF statistic (`log2_ps_avx2` exponent bits only) | `float` | floating, no integer accumulator | — | — | SAFE |
| arm64/vif_neon.c:270-282 | `vif_subsample8_vertical16` `vmlal_n_u16` (+128) | `uint32x4_t` | 9 taps × 255 | 16,711,680 | same | SAFE |
| arm64/vif_neon.c:284-293, 373-390 | `vif_subsample16_vertical16` (stored un-narrowed) and `vif_subsample_horizontal16` (`vmlaq_n_u32`) | `uint32x4_t` | ≤ 4,294,934,528 in range | — | — | DEPENDS (out-of-range input: next pass sums wrap mod $2^{32}$) |
| arm64/vif_neon.c:541-600 | horizontal stat: mean `vmlaq_n_u32` (u32), moments `vmlal_n_u32` (u64), `vif_mean_product` `vmlal_u32` (u64) | u32 / u64 lanes | ≤ 4,294,901,760; 2.8e14; ≤ 1.8446e19 | same | same | SAFE |
| arm64/vif_neon.c:661-720 | 8-bit vertical: `vmull_u8` squares (u16), `vmlal_n_u16` (u32), cross `vmulq_u32` | `uint32x4_t` | Σ ≤ 4,261,478,400 | same | same | SAFE |
| arm64/vif_neon.c:823-900 | 16-bit vertical: squares `vmulq_u32` (≤ 4,294,836,225), moments u64, mean u32 + round, stored un-narrowed | u32 / u64 lanes | in range ≤ 4,294,934,528 | — | — | DEPENDS (out-of-range input, as AVX2) |

## float VIF, vif_tools.c (also SpEED's filters and resampler)

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| vif.c, float_vif.c (whole files) | `num`, `den`, `score` | `float` / `double` | floating, no integer accumulator | — | — | SAFE |
| vif_tools.c:649, 733, 840 | `dst[y * dst_stride + x]` (bicubic, lanczos4, nearest) | `int` | prescaled float plane index (W·p)(H·p) | 2,123,366,399 at p = 4 | $2^{31}$ at p ≈ 1.414; $2^{34}$ at p = 4 | OVERFLOW@CAP-ONLY (vif_prescale / speed_prescale > √2; default 1.0 safe) |
| vif_tools.c:193, 206, 221, 328-331, 378, 397, 416-417 | `i * px_stride + j` in dec2/dec16/sum/statistic/vertical filter | `int` | same planes | 2,123,366,399 | > $2^{31}$ for p > √2 | OVERFLOW@CAP-ONLY (same option dependence) |
| vif_tools.c:767-769 | bilinear rows `(size_t)y * src_stride` | `size_t` | row pointer | — | — | SAFE |
| vif.c:80-81 | `apply_frame_differencing` `i * stride + j` | `int` | unscaled plane (vifdiff) | 1.33e8 | $2^{30}$ | SAFE |
| vif.c:86-94 | `vif_plane_size` guarded `stride * h` | `size_t` | bytes | 5.3e8 | $2^{32}$ | SAFE |
| float_vif.c:317-328, 230-254 | `scaled_w`, `scaled_h`, `scaled_float_stride * scaled_h` | `size_t` | bytes | 8.5e9 at p = 4 | 6.9e10 | SAFE (size_t; memory only) |

## Integer ADM

Band maxima, weight budgets and the region come from the integer ADM group of the [CUDA and HIP appendix](accumulator-bounds-cuda-hip.md) (`adm_csf_fixed_point.h:88-127`): scale-0 bands ≤ 22,930; scale 1 ≤ 1.449e9 (band_a ≤ 1.491e9); scale-0 weights ≤ 46,603 (h/v), 65,535 (d); scales 1–3 CSF output ≤ 1,518,500,221.

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| integer_adm_kernels.h:1262-1270, 1277-1300 | `adm_dwt2_tap4` (8-bit vertical, minus `coeffs_sum·128`) | `int32_t` | 4 taps × 255 | 12,898,410 | same | SAFE |
| integer_adm.h:199-209 | `adm_dwt2_vpass16_tap4` | `int64_t` | 4 taps × 65535 | 3.31e9 | same | SAFE |
| integer_adm_kernels.h:1318-1323 | `(int16_t)` narrowing of the 16-bit vertical result | `int32_t` → `int16_t` | in range ≤ 27,411 | — | — | DEPENDS (out-of-range input: 3,214,028 at bpc 10 wraps) |
| integer_adm_kernels.h:1330-1360 | `adm_dwt2_hpass` `accum` (+32768) | `int32_t` | 4 taps × \|tmp\| ≤ 27,412 | 1.5028e9 | same | SAFE (0.70·INT32_MAX) |
| integer_adm_kernels.h:1384-1393, 1398-1458 | `i4_dwt2_tap4` (scales 1–3), narrowed to int32 | `int64_t` → `int32_t` | 4 taps × ≤ 1.491e9 | 8.2e13; band ≤ 1.491e9 | same | SAFE |
| integer_adm_kernels.h:1283-1286, 1311-1314, 1406-1415 | `src[ind_y * src_stride + j]` (8-bit bytes / 16-bit samples, `integer_adm.c:779`) | `int` | index | 1.33e8 | $2^{30} - 1$ | SAFE |
| integer_adm_kernels.h:274-288 | `adm_decouple_band`: `(int64_t)lut·t`, `k * o` (`int32_t`), `rst * gain` (`double`, MIN with t) | `int64_t` / `int32_t` | k ≤ 32768, \|o\|, \|t\| ≤ 22,930 | 7.51e8 | same | SAFE |
| integer_adm_kernels.h:323-326 | `a = th − rst_h` | `int16_t` | \|a\| ≤ \|t\| — | 22,930 | same | SAFE |
| integer_adm_kernels.h:312-313, 407-408 | angle operands `(int64_t)oh*th + ...` | `int64_t` | scale 0 ≤ 1.05e9; s123 ≤ 2·(1.449e9)² | 4.2e18 | same | SAFE (2.2x below $2^{63}$) |
| integer_adm_kernels.h:369-385 | s123 `tmp_k` (lut ≤ $2^{30}$ × t), `1u << (14+k_shift)` (k_shift ≤ 16), `rst` | `int64_t` | — | 1.556e18 | same | SAFE |
| integer_adm_kernels.h:484-486 | `dst_val = i_rfactor * src`, `i16_dst_val` | `int` → `int16_t` | weight ≤ 65,535 × 22,930 | 1.5027e9; ≤ 32,611 after shift | same | SAFE (ADR-1472 budget) |
| integer_adm_kernels.h:488-489 | `flt = (int16_t)((4369·\|i16\| + 2048) >> 12)` | `int` → `int16_t` | default ≤ 27,212 | — | — | DEPENDS (CSF weight option: h/v weight ≥ 43,900 wraps; integer ADM group) |
| integer_adm_kernels.h:584-590 | i4 CSF `i_rfactor * (int64_t)src >> 28`; `flt` | `int64_t` → `int32_t` | < 5.47e8 × 1.449e9 | 7.9e17; ≤ 1,518,500,221 | same | SAFE |
| integer_adm_kernels.h:647 | `area = (bottom−top)*(right−left)` | `int` | scale-0 region | 2.13e7 | 1.72e8 | SAFE |
| integer_adm_kernels.h:656-668 | scale-0 `csf_den` row `inner[]` Σ \|band\|³ | `uint64_t` | cols terms ≤ 1.2056e13 | 6146 → 7.41e16 | 13110 → 1.58e17 | SAFE |
| integer_adm_kernels.h:671-677 + adm_cm_accumulator.h:44-48 | scale-0 `csf_den` frame `accum[]` | `uint64_t` | rows of (row + r) >> ceil(log2 area − 20) | ≤ $2^{20}$·1.2056e13 | same | SAFE (established; shift adapts) |
| integer_adm_kernels.h:693-697, 743-760 | `i4_cube_term`; s123 row `inner[]` | `uint64_t` | x² ≤ 2.1e18 + $2^{31}$; ×x after >>30/31 ≤ 1.417e18; row ≤ cols/$2^{\lceil \log_2 \mathrm{cols} \rceil}$·term | ≤ 1.417e18 | same | SAFE |
| integer_adm_kernels.h:675 (s123 fold) | s123 `csf_den` frame `accum[]` | `uint64_t` | rows/$2^{\lceil \log_2 \mathrm{rows} \rceil}$ × row | ≤ 1.417e18 | same | SAFE |
| integer_adm_kernels.h:796-823 | `adm_cm_thresh` `sum`/`accum` (27 taps) | `int32_t` | flt ≤ 32,767 (even wrapped), centre ≤ 69,904 | ≤ 996,120 | same | SAFE |
| integer_adm_kernels.h:826-856 | `i4_adm_cm_thresh` | `int32_t` | Σ ≤ \|csf\|·(10/30)·3 | ≤ 1,518,500,248 | same | SAFE (0.71·INT32_MAX) |
| integer_adm_kernels.h:1011-1013 | `xh = band * i_rfactor` (int16 × uint16 → `int`) | `int32_t` | 22,930 × 65,535 | 1.5027e9 | same | SAFE |
| adm_cm_accumulator.h:72-82 | `adm_cm_excess_s0` int64 then clamp | `int64_t` → `int32_t` | \|x\| − thr·$2^{\mathrm{shift}}$ | clamped to INT32_MAX | same | SAFE |
| integer_adm_kernels.h:872 | scale-0 `v_sq = (int32_t)((v² + 2^28) >> 29)` | `int64_t` → `int32_t` | thr ≥ 0: ≤ 2.127e9 | — | — | DEPENDS (CSF weight option: negative thr → v up to INT32_MAX → $2^{33}$ wraps; integer ADM group) |
| integer_adm_kernels.h:873, 1017-1019, 1079-1099 | scale-0 CM row `inner[]` | `int64_t` | cols cube terms | default 0.855·INT64_MAX (W = 15360) | 0.912·INT64_MAX | OVERFLOW@16K (established: W = 31–32 / 63–64 at default weights; fix to uint64 in flight) |
| integer_adm_kernels.h:888-895 + adm_cm_accumulator.h:30-34 | scale-0 CM frame `accum[]` = Σ (row + r) >> ceil(log2 h) | `int64_t` | rows/$2^{\lceil \log_2 \mathrm{h} \rceil}$ ≤ 1 × row | ≤ row max | same | SAFE (cannot wrap unless a row already did) |
| integer_adm_kernels.h:878-884, 1172-1188 | s123 `i4_adm_cm_scale` (int64), `v_sq` (≤ $2^{31} - 1$ by budget), `v_sq*v` | `int64_t` | 2.147e9 × 1.5185e9 | 3.26e18 | same | SAFE (2.8x below $2^{63}$) |
| integer_adm_kernels.h:1186-1188, 1235-1256 | s123 CM row `inner[]` and frame `accum[]` | `int64_t` | row ≤ cols/$2^{\lceil \log_2 \mathrm{w} \rceil}$ × term | ≤ 3.27e18 | same | SAFE |
| integer_adm.h:62-72 | `div_lookup` = $2^{30}$ / i | `int32_t` | — | ≤ $2^{30}$ | same | SAFE |
| integer_adm.c:815, 1024-1029 | `numden_limit` `(w * h)` (`int`); buffer sizes | `int` / `size_t` | — | 1.33e8; 2.2e9 B | $2^{30}$; 1.8e10 B | SAFE |
| adm_gain_limit.h:75-89 | `a * g.m_lo`, `a * g.m_hi + (p_lo >> 32)` | `uint64_t` | a < $2^{31}$, m_lo < $2^{32}$, m_hi < $2^{21}$ | < $2^{63}$ | same | SAFE |
| adm_angle_flag.h:172-261 | integer angle test: `mo*mt` < $2^{48}$, `(s_val << p)` < $2^{56}$, `(mp*mp) << (sp+p)` ≤ $2^{58}$ | `uint64_t` | — | < $2^{58}$ | same | SAFE |
| adm_csf_fixed_point.h:302-305 | `adm_half_shift` | `uint32_t` | 1 << (shift − 1) | ≤ $2^{31}$ | same | SAFE |
| x86/adm_avx2.c:163-183 | angle `madd_epi16` (dot read signed / INT32_MIN-fixed, magnitudes unsigned) | epi32 | 2 × 22,930² | 1.05e9 | same | SAFE |
| x86/adm_avx2.c:186-256 | decouple: `mul_epi32` (div·t), `mullo_epi32(k, o)`, `cvttpd_epi32(rst·gain)`, `packs_epi32` | epi64 / epi32 | ≤ $2^{30}$·22,930; 7.51e8; 2.29e6; ≤ 22,930 | same | same | SAFE |
| x86/adm_avx2.c:329-475 | s123 decouple: `mul_epi32` angle sums, `tmp_k`, `(int64_t)(rst·gain)` | epi64 / `int64_t` | 4.2e18; 1.556e18; 1.449e11 | same | same | SAFE |
| x86/adm_avx2.c:574-591 | 8-bit DWT vertical `madd_epi16`, `srli`+`blend`+`packus` (keeps the low 16 bits) | epi32 | 4 taps × 255; result fits int16 | exact mod $2^{16}$ | same | SAFE |
| x86/adm_avx2.c:636-648 | DWT horizontal `madd_epi16` pairs (\|tmp\| ≤ 27,412 × ≤ 43,237) + 32768 | epi32 | pair ≤ 1.185e9; 4 taps ≤ 1.5028e9 | same | same | SAFE |
| x86/adm_avx2.c:717-731 | 16-bit DWT: scalar `adm_dwt2_vpass_16` (int64) + vector hpass | — | inherits the scalar narrowing row | — | — | SAFE (in range; out-of-range counted on the scalar row) |
| x86/adm_avx2.c:768-783, 841-878 | s123 DWT `mul_epi32` taps, `sra_fit_epi64`, narrow | epi64 | as scalar | 8.2e13 | same | SAFE |
| x86/adm_avx2.c:882-890 | scale-0 CSF `mullo_epi32(src, i_rfactor)`, `packs_epi32` dst | epi32 | ≤ 1.5027e9; dst ≤ 32,611 | same | same | SAFE |
| x86/adm_avx2.c:891-894 | scale-0 CSF `flt` via `packs_epi32` (saturating) | epi32 → epi16 | default ≤ 27,212 | — | — | DEPENDS (CSF weight option; saturates where the scalar wraps, so AVX2 diverges) |
| x86/adm_avx2.c:946-970 | s123 CSF `mul_epi32`, INT32_MIN `add_flt` with logical shift (low dword = scalar's arithmetic result) | epi64 | ≤ 7.9e17 | same | same | SAFE |
| x86/adm_avx2.c:1023-1048 | scale-0 `csf_den` cube lanes (`mullo_epi32` square, `mul_epu32` cube) → `hsum_epu64` | epi64 | lane ≤ row | 7.41e16 | 1.58e17 | SAFE |
| x86/adm_avx2.c:1078-1110 | s123 `csf_den` cube lanes | epi64 | lane ≤ row | ≤ 1.417e18 | same | SAFE |
| x86/adm_avx2.c:1148-1180 | `cm_thresh_avx2` int32 lanes | epi32 | as scalar | 996,120 | same | SAFE |
| x86/adm_avx2.c:1250-1271 | `cm_excess_avx2` short form `sll_epi32(thr, shift)` | epi32 | wraps for thr ≥ $2^{31-\mathrm{shift}}$; the row is then redone in the exact form (`rare` test, `:1377-1379`) | — | — | SAFE |
| x86/adm_avx2.c:1280-1283 | squared excess `srl(x·x + add)`, then `mul_epi32(lo, x)` reads the low 32 bits signed | epi64 | = scalar `(int32_t)` narrowing | — | — | DEPENDS (CSF weight option, as the scalar `v_sq` row) |
| x86/adm_avx2.c:1274-1292, 1365-1385 | scale-0 CM row: biased uint64 lanes (wrap mod $2^{64}$ by design), `hsum − lanes·cub_bias`, `cm_as_int64` | epi64 / `int64_t` | = true int64 row sum when it fits | default 0.855·INT64_MAX | 0.912 | OVERFLOW@16K (inherits the established scalar row: W = 31–32 / 63–64) |
| x86/adm_avx2.c:1436-1513 | s123 CM thresh / cube / row in int64 lanes, `hsum_epi64` | epi64 | as scalar | ≤ 3.27e18 | same | SAFE |
| x86/adm_avx512.c:116-160 | angle `madd_epi16` → float/double compare | epi32 | 1.05e9 | same | same | SAFE |
| x86/adm_avx512.c:212-292 | decouple k / gain / rst, `permutexvar_epi16` narrowing (truncation = scalar) | epi64 / epi32 | as AVX2 | same | same | SAFE |
| x86/adm_avx512.c:360-597 | s123 decouple | epi64 | as AVX2 | same | same | SAFE |
| x86/adm_avx512.c:613-692 | DWT 8-bit vertical / horizontal `madd_epi16`, `cvtepi32_epi16` | epi32 | as AVX2 | 1.5028e9 | same | SAFE |
| x86/adm_avx512.c:759-773 | 16-bit DWT through the scalar vertical pass | — | inherits the scalar row | — | — | SAFE (in range) |
| x86/adm_avx512.c:789-921 | s123 DWT `mul_epi32`, `srai_epi64`, `cvtepi64_epi32` | epi64 | as scalar | 8.2e13 | same | SAFE |
| x86/adm_avx512.c:923-929 | scale-0 CSF dst `mullo_epi32`, `cvtepi32_epi16` | epi32 | ≤ 1.5027e9 | same | same | SAFE |
| x86/adm_avx512.c:930-932 | scale-0 CSF `flt` via `cvtepi32_epi16` (truncation, like the scalar) | epi32 → epi16 | default ≤ 27,212 | — | — | DEPENDS (CSF weight option, same wrap as the scalar) |
| x86/adm_avx512.c:984-1059 | s123 CSF | epi64 | as AVX2 | ≤ 7.9e17 | same | SAFE |
| x86/adm_avx512.c:1061-1188 | `csf_den` (scale 0 and s123) uint64 lanes | epi64 | lane ≤ row | ≤ 1.417e18 | same | SAFE |
| x86/adm_avx512.c:1190-1305 | CM thresh, excess (short / exact form) | epi32 | as AVX2 | 996,120 | same | SAFE |
| x86/adm_avx512.c:1316-1319 | squared excess `sra(x·x + add)`, `mul_epi32(lo, x)` | epi64 | = scalar narrowing | — | — | DEPENDS (CSF weight option) |
| x86/adm_avx512.c:1310-1327, 1394-1410 | scale-0 CM row int64 lanes, `hsum_epi64` | epi64 / `int64_t` | lane ≤ row | default 0.855·INT64_MAX | 0.912 | OVERFLOW@16K (inherits the established scalar row) |
| x86/adm_avx512.c:1438-1517 | s123 CM | epi64 | as scalar | ≤ 3.27e18 | same | SAFE |
| arm64/adm_neon.c:36-49, 68-131 | 8-bit DWT vertical `vmlal_lane_s16` (init −46342·128 + 128), scalar tail | `int32x4_t` | 4 taps × 255 | 12,898,410 | same | SAFE |
| arm64/adm_neon.c:136-205 | DWT horizontal (init 32768) + `vuzp1q_s16` low-16 narrowing (= scalar) | `int32x4_t` | 4 taps × 27,412 | 1.5028e9 | same | SAFE |
| arm64/adm_neon.c:237, 371 | `i * dst_stride`, `(i * stride) + j0` | `int` | band index | 3.3e7 | 2.7e8 | SAFE |
| arm64/adm_neon.c:257-264 | `adm_neon_dot_s16` `vmull_s16` + `vaddl_s32` | `int64x2_t` | 2 × 22,930² | 1.05e9 | same | SAFE |
| arm64/adm_neon.c:297-322 | decouple `vmull_s32`/`vrshrn_n_s64` (≤ 7.5e8), `vmulq_s32(k, o)` (≤ 7.51e8), `vmulq_n_s32(rst, gain)` (≤ 2.29e6) | `int32x4_t` / `int64x2_t` | — | same | same | SAFE |

## float ADM (adm.c, adm_tools.c, float_adm.c)

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| adm.c, adm_tools.c, float_adm.c (whole files) | num / den / CM sums | `float` / `double` | floating, no integer accumulator | — | — | SAFE |
| adm_tools.c:68, 227, 256, 957-982, 1059 | `w * h`, `x[i * px_stride + j]`, `dst->band_*[i * dst_px_stride + j]` | `int` | band-plane index (W/2·H/2) | 3.3e7 | 2.7e8 | SAFE |
| adm.c:415 | `(w * h)` | `int` | size product | 1.33e8 | $2^{30}$ | SAFE |
| x86/float_adm_avx2.c, x86/float_adm_avx512.c (whole files) | CSF / CM / DWT | `float` | floating, no integer accumulator | — | — | SAFE |
| arm64/float_adm_dwt2_neon.c (whole file) | CSF / CM / DWT | `float` | floating, no integer accumulator | — | — | SAFE |
| arm64/float_adm_neon.c:45-46 | `src_off = i * src_px_stride` | `int` | band index | 3.3e7 | 2.7e8 | SAFE |

## Integer SSIM

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| integer_ssim.c:72-93 | kernel `sum` | `unsigned` | taps, total 256 | 256 | same | SAFE |
| integer_ssim.c:146-147 | `src[off*2] + (src[off*2+1] << 8)` | `int` | 16-bit sample | 65,535 | same | SAFE |
| integer_ssim.c:153-158 | horizontal moments `m.x2 += (int64_t)window*s*s` etc. | `int64_t` | Σ window (256) × 65535² | 1.1e12 ($2^{40}$) | same | SAFE (also for out-of-range samples) |
| integer_ssim.c:247-252 | vertical moments `window * buf->x2` (`signed` × `int64_t`) | `int64_t` | 256 × $2^{40}$ | 2.8e14 ($2^{48}$) | same | SAFE (established) |
| integer_ssim.c:296-302 | line buffer `(size_t)line_sz * w * sizeof` | `size_t` | bytes | 3.9e6 | 8.4e6 | SAFE |
| x86/integer_ssim_avx2.c:150-186 | 8-bit moments in epi32 lanes (`mullo_epi32`) | epi32 | Σ w·s² ≤ 256·65,025 | 16,646,400 | same | SAFE |
| x86/integer_ssim_avx2.c:268-310 | 16-bit `ws = mul_epi32(w, s)` (< $2^{24}$), `mul_epi32(ws, s)` → epi64 | epi64 | 256 × 65535² | 1.1e12 | same | SAFE |
| x86/integer_ssim_avx2.c:24-72 | scalar boundary pixels | `int64_t` | as scalar | 1.1e12 | same | SAFE |

## float SSIM, MS-SSIM, IQA

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| ssim.c, float_ssim.c, ms_ssim.c, float_ms_ssim.c, ms_ssim_decimate.c, iqa/convolve.c, iqa/decimate.c, iqa/ssim_tools.c, iqa/math_utils.c (whole files) | SSIM / MS-SSIM sums | `float` / `double` | floating, no integer accumulator | — | — | SAFE |
| ssim.c:58-59, ms_ssim.c:201-202, iqa/convolve.c:70-95, 208, 259, iqa/decimate.c:53 | `y * stride` (stride in floats: `ssim.c:130`, `ms_ssim.c:322`), `y * w + x` | `int` | float-element index | 1.33e8 | $2^{30}$ | SAFE |
| iqa/ssim_tools.c:322, iqa/math_utils.c:72, float_ssim.c:137, float_ms_ssim.c:202 | `w * h` | `int` / `unsigned` | size product | 1.33e8 | $2^{30}$ | SAFE |
| x86/ssim_avx2.c, x86/ssim_avx512.c, x86/convolve_avx2.c, x86/convolve_avx512.c, x86/ms_ssim_decimate_avx2.c, x86/ms_ssim_decimate_avx512.c (whole files) | SIMD SSIM / convolution / decimation | `float` | floating, no integer accumulator | — | — | SAFE |
| arm64/ssim_neon.c, arm64/convolve_neon.c, arm64/ms_ssim_decimate_neon.c (whole files) | SIMD SSIM / convolution / decimation | `float` | floating, no integer accumulator | — | — | SAFE |

## Moment (moment.c, float_moment.c, float_moment_sum.h, ordered_sum.h)

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| moment.c:33-70, float_moment.c (whole file) | `cum` | `double` | floating, no integer accumulator | — | — | SAFE |
| float_moment_sum.h:107-110 | `(uint64_t)w*h > 2^53 >> (2·bpc)` | `uint64_t` | — | 1.33e8 | $2^{30}$ | SAFE |
| float_moment_sum.h:143-150 | `exact = sum + (uint64_t)term` | `uint64_t` | term < $2^{32}$ (65535²); N terms | < $2^{57}$ | < $2^{62}$ | SAFE (header bound: sums < $2^{62}$) |
| float_moment_sum.h:168-177 | `plan_batch` prefix `after = before + totals[i]` | `uint64_t` | row totals | < $2^{57}$ | < $2^{62}$ | SAFE |
| float_moment_sum.h:334-355 | `lane_total` / `run` totals | `uint64_t` | ≤ $2^{15}$ terms < $2^{32}$ | $2^{47}$ | $2^{47}$ | SAFE |
| float_moment_sum.h:230-250 | `add_run` `m + units`, `end << shift` | `uint64_t` | m < $2^{53}$, units capped at $2^{54}$ | < $2^{63}$ | same | SAFE |
| ordered_sum.h:178-190, 249-262, 306-327 | `round_shifted`, `vmaf_ordsum_then` (capped at $2^{54}$), `m + units` | `int64_t` | — | ≤ $2^{55}$ | same | SAFE |
| float_moment_sum_gpu.h:53-70, 82-130 | shared `totals[lane] += totals[lane+step]`; `(size_t)plane * height + row` | `uint64_t` / `size_t` | one row (256 lanes) | $2^{47}$ | $2^{47}$ | SAFE |
| x86/moment_avx2.c, x86/moment_avx512.c (whole files) | `cum` | `double` | floating, no integer accumulator (`(size_t)i * stride_f` rows) | — | — | SAFE |
| arm64/moment_neon.c, arm64/moment_sve2.c (whole files; SVE2 included) | `cum` | `double` | floating, no integer accumulator (`(size_t)i * stride_f` rows) | — | — | SAFE |

## CAMBI

`validate_image` (`cambi.c:990-1029`) rejects samples above $2^{\mathrm{bpc}} - 1$, so CAMBI has no out-of-range path. Internal samples are 10-bit.

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| cambi.c:415-427 (and all range updaters) | histogram cells `uint16_t` ++/−− | `uint16_t` | window² ≤ 65² | 4,225 | same | SAFE (established) |
| cambi.c:1155-1176 | `compute_dp_row` `prefix`, `dp_curr = dp_prev + prefix` | `uint32_t` | cumulative 0/1 counts over the plane | ≤ N = 1.33e8 | ≤ $2^{30}$ | SAFE (< $2^{32}$; used only through modular box differences) |
| cambi.c:1178-1186 | `compute_mask_row` box sum | `uint32_t` | inclusion–exclusion, modular | ≤ filter² | same | SAFE |
| cambi.c:1132-1137 | `get_mask_index` `(w>>6)*(h>>6)` | `uint32_t` | — | 3.2e4 | 2.6e5 | SAFE |
| cambi.c:1275-1277 | `diff_weights[d] * p_0 * p_1` | `int` | weight ≤ 9 × 4225² | 160,655,625 | same | SAFE |
| cambi.c:369-379 | `window * (w + h)` | `unsigned` | window ≤ 127 option | 3.0e6 | 8.3e6 | SAFE |
| cambi.c:960-985 | anti-dithering `i * stride + j`, sum of 4 | `unsigned` (i × `int`) / `int` | index; 4 × 1023 | 1.33e8; 4,092 | $2^{30}$ | SAFE |
| cambi.c:1510-1513 | `int num_elements = height * width` | `unsigned` → `int` | — | 1.33e8 | $2^{30}$ | SAFE |
| cambi.c:1545-1556 | heatmap `max_c_value`; file offset `((ptrdiff_t)frame*h + i)*w*2` | `int` / `ptrdiff_t` | per frame 2·N bytes | 3.5e10 frames to overflow | 4.3e9 frames | SAFE |
| cambi.c:1898-1907 | `get_pixels_in_window` odd_length² | `uint16_t` | ≤ 127² | 16,129 | same | SAFE |
| cambi.c:1880-1886 | `vmaf_cambi_fixed_topk_mean` 128-bit → double | `uint64_t` pair | conversion only | — | — | SAFE |
| x86/cambi_avx2.c:66-121 | anti-dithering epi32 sums, `packus_epi32` | epi32 | 4 × 65535 | 262,140 | same | SAFE |
| x86/cambi_avx2.c:195-221 | range inc/dec `epi16` | epi16 | ≤ 4,225 | same | same | SAFE |
| x86/cambi_avx2.c:223-284 | gather index `mullo_epi32(compact, width) + col`; `num = weight·p0·p_max` | epi32 | v_band (≈ 1,100) × W; 9 × 4225² | 1.7e7; 1.6e8 | 3.6e7; 1.6e8 | SAFE |
| x86/cambi_avx2.c:614-690 | dp prefix scan, mask box sum (biased unsigned compare) | epi32 | modular, as scalar | ≤ $2^{30}$ | same | SAFE |
| x86/cambi_avx512.c:40-55 | range updates `epi16` | epi16 | ≤ 4,225 | same | same | SAFE |
| x86/cambi_avx512.c:142-178, 263-275 | gather indices and `num` `mullo_epi32` | epi32 | as AVX2 | 1.6e8 | same | SAFE |
| x86/cambi_avx512.c:321-373 | dp prefix scan, mask box sum | epi32 | modular | ≤ $2^{30}$ | same | SAFE |
| x86/cambi_avx512.c:433-458 | anti-dithering epi16: Σ(x>>2) ≤ 65,532, Σ(x&3) ≤ 12 | epi16 | exact for any uint16 | same | same | SAFE |
| arm64/cambi_neon.c:204-275 | dp prefix / box sum `vaddq_u32`/`vsubq_u32` | `uint32x4_t` | modular | ≤ $2^{30}$ | same | SAFE |
| arm64/cambi_neon.c:315-326 | anti-dithering `vaddl_u16` → u32 | `uint32x4_t` | 4 × 65535 | 262,140 | same | SAFE |
| arm64/cambi_neon.c:443-447 | `lane_bits_neon` `vaddvq_u16` | `uint16_t` | ≤ 255 | same | same | SAFE |

## PSNR-HVS (third_party/xiph/psnr_hvs.c, psnr_hvs_score.c)

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| third_party/xiph/psnr_hvs.c:127-179 | `od_bin_fdct8` lifting `(t*K + r) >> s` | `int` | 12-bit samples (bpc ≤ 12 guard `:440`) ≤ $2^{28.5}$ (CAMBI and PSNR-HVS group) | per block | same | DEPENDS (out-of-range input: 16-bit values reach $2^{32.5}$, int UB) |
| third_party/xiph/psnr_hvs.c:357, 362 | `dct_s * dct_s` (AC), then `* mask` | `int` then `float` | ≤ $2^{28}$ in range | per block | same | DEPENDS (out-of-range input: $2^{36}$) |
| third_party/xiph/psnr_hvs.c:311-317 | `(y+i)*_systride + (j+x)*2 + 1` | `int` | byte index (16-bit storage) | 8639·30720 + 30719 = 2.65e8 | 32767·65536 + 65535 = INT32_MAX | SAFE (zero margin at the cap) |
| third_party/xiph/psnr_hvs.c:381 | `pixels++` | `int` | 64 per 8x8 block, step 7 | 1.73e8 | 4681²·64 = 1.40e9 | SAFE (0.65·INT32_MAX) |
| third_party/xiph/psnr_hvs.c:387-388 | `samplemax * samplemax` | `int32_t` | ≤ 4095² (bpc ≤ 12 guard) | 16,769,025 | same | SAFE |
| psnr_hvs_score.c:31-37, 49-64 | guard `n_blocks ≤ INT_MAX/64`, `int pixels`, `samplemax²` | `size_t` / `int` / `int32_t` | as above | 1.73e8 | 1.40e9 | SAFE |
| x86/psnr_hvs_avx2.c:89-100 | `od_mulrshift_avx2` `_mm256_mullo_epi32` | epi32 | ≤ $2^{28.5}$ in range | per block | same | DEPENDS (out-of-range input; lane wraps where the scalar is UB) |
| x86/psnr_hvs_avx2.c:461-466 | `dct * dct * mask` | `int` | ≤ $2^{28}$ | per block | same | DEPENDS (out-of-range input) |
| x86/psnr_hvs_avx2.c:394-400, 502, 533-534 | byte index, `pixels`, `samplemax²` | `int` | as scalar | 2.65e8 | INT32_MAX (index) | SAFE |
| arm64/psnr_hvs_neon.c:105-112 | `vmulq_s32` lifting products | `int32x4_t` | ≤ $2^{28.5}$ in range | per block | same | DEPENDS (out-of-range input) |
| arm64/psnr_hvs_neon.c:500-505 | `dct * dct * mask` | `int` | ≤ $2^{28}$ | per block | same | DEPENDS (out-of-range input) |
| arm64/psnr_hvs_neon.c:433-438, 543, 574-575 | byte index, `pixels`, `samplemax²` | `int` | as scalar | 2.65e8 | INT32_MAX (index) | SAFE |

## SpEED (speed.c, speed_internal.c, speed_qa.c)

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| speed.c, speed_internal.c (whole files) | covariance, entropy, eigen | `float` / `double` | floating, no integer accumulator; index math `size_t` | — | — | SAFE |
| speed.c:1285-1300 | `scaled_height = (int)lround(h * prescale)` | `int` → `size_t` | prescale ≤ 4 | 61,440 | 131,072 | SAFE |
| speed.c:1189-1214, speed_internal.c:131-139 | resample / filter at the prescaled size through `vif_tools.c` `int` indices | `int` | (W·p)(H·p) | 2,123,366,399 | > $2^{31}$ for p > √2 | OVERFLOW@CAP-ONLY (speed_prescale > √2; see the vif_tools rows) |
| speed.c:1155-1165 | `num_blocks > 2^24` guard (GPU helper) | `size_t` | fail-closed | — | — | SAFE |
| speed_qa.c:132, 144, 178 | `wr * g_gauss_1d[dc]` | `int32_t` | ≤ 23,903² | 571,353,409 | same | SAFE |
| speed_qa.c:248-261 | HBD diff clamped to int16 | `int16_t` | — | ≤ 32,767 | same | SAFE |
| speed_qa.c:229, 241-252, 324 | `(size_t)w*h`; `r * cur_stride` (`unsigned` × `ptrdiff_t`) | `size_t` | — | — | — | SAFE |
| x86/speed_avx2.c, x86/speed_avx512.c, x86/speed_matmul_avx2.c, x86/speed_matmul_avx512.c (whole files) | covariance / matmul | `double` / `float` | floating, no integer accumulator (`size_t` rows) | — | — | SAFE |
| arm64/speed_neon.c (whole file) | covariance / matmul | `double` / `float` | floating, no integer accumulator (`size_t` rows) | — | — | SAFE |

## CIEDE, delta-E ITP

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| ciede.c, ciede_frame_sum.h, ciede_ff_math.h, ff_math.h, ff_pair.h, delta_e_itp.c, delta_e_itp_math.h (whole files) | ΔE sums | `double` / `float` | floating, no integer accumulator; row offsets `ptrdiff_t` / `size_t` | — | — | SAFE |
| ciede.c:603 | `ref_pic->w[0] * ref_pic->h[0]` | `unsigned` | size product | 1.33e8 | $2^{30}$ | SAFE |
| x86/ciede_avx2.c, x86/ciede_avx512.c (whole files) | u8/u16 → i32/f32 widening only | epi32 | no accumulator | — | — | SAFE |
| arm64/ciede_neon.c (whole file) | u8/u16 → i32/f32 widening only | epi32 | no accumulator | — | — | SAFE |

## SSIMULACRA 2

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| ssimulacra2.c, ssimulacra2_math.h, ssimulacra2_score.h, ssimulacra2_pixel_format.h, ssimulacra2_simd_common.h, ssimulacra2_eotf_lut.h (whole files) | error maps, pooling | `float` / `double` | floating, no integer accumulator; `size_t` planes | — | — | SAFE |
| ssimulacra2.c:440-458 | `(int64_t)x * pw / lw` | `int64_t` | 32767 × 32768 | 5.0e8 | 1.07e9 | SAFE |
| x86/ssimulacra2_avx2.c:529-534 | gather index `(int32_t)((y_base + i) * w)` | `unsigned` → `int32_t` | row × width | 1.33e8 | $2^{30}$ − 32768 | SAFE |
| x86/ssimulacra2_avx2.c (rest), x86/ssimulacra2_avx512.c, x86/ssimulacra2_host_avx2.c (whole files) | blur, error maps | `float` / `double` | floating, no integer accumulator (`int64_t` resample like the scalar) | — | — | SAFE |
| arm64/ssimulacra2_neon.c, arm64/ssimulacra2_sve2.c, arm64/ssimulacra2_host_neon.c, arm64/ssimulacra2_arm64_common.h (whole files; SVE2 included) | blur, error maps | `float` / `double` | floating, no integer accumulator (`int64_t` resample like the scalar) | — | — | SAFE |

## Other CPU extractors and helpers

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| picture_copy.cpp:51-104 | row stepping `dst += dst_stride/4`, `src_row += stride` | `ptrdiff_t` | no accumulator | — | — | SAFE |
| offset.c:22-46 | `byte_ptr += stride` | `int` stride | no accumulator | — | — | SAFE |
| common/convolution.c:42-121, common/convolution_internal.h:97-153 | `src[i * src_stride + j]` (float elements) | `int` | index | 1.33e8 | $2^{30}$ | SAFE |
| perceptual_weight.c:124-142, 319 | `sum += image[...]`; `n_cells = cols·rows` | `uint64_t` / `uint32_t` | ≤ 255 × 65535² | 1.1e12 | same | SAFE |
| feature_lpips.c:131-142, feature_dists.c:123-134, feature_mobilesal.c:113-131, 246, 284 | `(size_t)w*h`, `3u * plane * sizeof(float)` | `size_t` | tensor sizes | 1.6e9 B | 1.3e10 B | SAFE |
| fastdvdnet_pre.c:197-230, 263 | `FASTDVDNET_PRE_WINDOW * plane * sizeof(float)`; `n_buffered` | `size_t` / `unsigned` | — | 2.7e9 B | 2.1e10 B | SAFE |
| transnet_v2.c:115-132 | `(i * src_h) / 27`, `(j * src_w) / 48` | `unsigned` | i ≤ 26, j ≤ 47 | 4.0e5 | 1.5e6 | SAFE |
| transnet_v2.c:295, 324 | `next_emit`, `n_read` frame counters | `unsigned` | one per frame | $2^{32}$ frames | same | SAFE |
| niqe.c, brisque.c, y_funque_plus.c, pu21.c, pu21_ssim.c (whole files) | feature statistics | `double` | floating, no integer accumulator; indices `size_t` | — | — | SAFE |
| feature_collector.cpp:246-260, 78-91, 530-546 | capacity doubling (index < `FEATURE_VECTOR_MAX_INDEX` = $2^{28}$), counts | `unsigned` / `size_t` | — | ≤ $2^{28}$ | same | SAFE |

## Runtime size products (core/src)

| file:line | variable / expression | type (exact C type) | what it sums (per-term max, term count) | worst-case bound at 16K | at cap | verdict |
|---|---|---|---|---|---|---|
| core/src/picture.c:157-162 | `(w + 63) & ~63`, `aligned_y << hbd` | `unsigned` → `ptrdiff_t` | stride bytes | 30,720 | 65,536 | SAFE |
| core/src/picture.c:193-195 | `y_sz = stride * h`, `pic_size = y_sz + 2*uv_sz` | `ptrdiff_t` × `unsigned` → `size_t` | bytes | 7.96e8 | 6.4e9 | SAFE (64-bit size_t) |
| core/src/picture.c:217 | `(void *)(uintptr_t)pic_size` | `uintptr_t` | — | 7.96e8 | 6.4e9 | SAFE (64-bit) |
| core/src/picture_pool.cpp:173-180 | `sizeof(*p->pictures) * cfg.pic_cnt` | `size_t` | — | — | — | SAFE |
| core/src/gpu_picture_pool.cpp:120-134, 210, 222 | guarded `slot_bytes * pic_cnt`; `curr_idx` mod; NVTX label counter | `size_t` / `unsigned` | — | — | — | SAFE |
| core/src/mem.cpp:30-60 | `aligned_malloc(size, …)` pass-through | `size_t` | — | — | — | SAFE |
| core/src/libvmaf.c:1470 | DNN `n = (size_t)w * (size_t)h` | `size_t` | — | 1.33e8 | $2^{30}$ | SAFE |
| core/src/libvmaf.c:772 | `pic_cnt = n_threads * 2 + 2` | `unsigned` | — | — | — | SAFE |
| core/src/libvmaf.c:4413-4418 | `pool_samples_push` capacity doubling with wrap check | `unsigned` / `size_t` | — | — | — | SAFE |

## Coverage

Every in-scope file, with the number of table rows whose first cell names it (a row naming several files counts once for each; the non-SAFE narrative cites some of these rows again). "none" = read, no integer accumulator or size product. The GPU subtrees (`cuda/`, `hip/`, `sycl/`, `metal/`, `rust/`) are out of scope (other audits). `x86/AGENTS.d/` and the `AGENTS.md` files are documentation.

### CPU scalar, `core/src/feature/` (top level, `iqa/`, `common/`, `third_party/xiph/`)

- `adm.c`: 2 rows
- `adm_tools.c`: 2 rows
- `alias.c`: none
- `brisque.c`: 1 row
- `cambi.c`: 11 rows
- `ciede.c`: 2 rows
- `delta_e_itp.c`: 1 row
- `fastdvdnet_pre.c`: 1 row
- `feature_dists.c`: 1 row
- `feature_lpips.c`: 1 row
- `feature_mobilesal.c`: 1 row
- `float_adm.c`: 1 row
- `float_moment.c`: 1 row
- `float_motion.c`: 2 rows
- `float_ms_ssim.c`: 2 rows
- `float_psnr.c`: 3 rows
- `float_ssim.c`: 2 rows
- `float_vif.c`: 2 rows
- `integer_adm.c`: 1 row
- `integer_motion.c`: 6 rows
- `integer_motion_v2.c`: 4 rows
- `integer_psnr.c`: 7 rows
- `integer_ssim.c`: 5 rows
- `integer_vif.c`: 14 rows
- `moment.c`: 1 row
- `motion.c`: 1 row
- `ms_ssim.c`: 2 rows
- `ms_ssim_decimate.c`: 1 row
- `niqe.c`: 1 row
- `null.c`: none
- `offset.c`: 1 row
- `perceptual_weight.c`: 1 row
- `psnr.c`: 1 row
- `psnr_hvs_score.c`: 1 row
- `pu21.c`: 1 row
- `pu21_ssim.c`: 1 row
- `speed.c`: 4 rows
- `speed_internal.c`: 2 rows
- `speed_qa.c`: 3 rows
- `ssim.c`: 2 rows
- `ssimulacra2.c`: 2 rows
- `tad_rust.c`: none (Rust FFI shim)
- `transnet_v2.c`: 2 rows
- `vif.c`: 3 rows
- `vif_tools.c`: 3 rows
- `y_funque_plus.c`: 1 row
- `adm.h`: none
- `adm_angle_flag.h`: 1 row
- `adm_cm_accumulator.h`: 3 rows
- `adm_csf_fixed_point.h`: 1 row
- `adm_csf_tools.h`: none
- `adm_float_reference.h`: none
- `adm_gain_limit.h`: 1 row
- `adm_options.h`: none
- `adm_score.h`: none
- `adm_tools.h`: none
- `alias.h`: none
- `barten_csf_tools.h`: none
- `brisque_math.h`: none
- `brisque_model.h`: none (double model constants; grep-checked)
- `cambi.h`: none (prototypes and the float reciprocal LUT; grep-checked)
- `cambi_c_values_frame.h`: none (`size_t` memsets, mask words)
- `cambi_internal.h`: none (prototypes; `uint32_t *mask_dp`)
- `ciede_ff_math.h`: 1 row
- `ciede_frame_sum.h`: 1 row
- `compat_builtin.h`: none
- `delta_e_itp_math.h`: 1 row
- `feature_characteristics.h`: none
- `feature_collector.h`: none
- `feature_collector_internal.h`: none
- `feature_dimensions.h`: none
- `feature_extractor.h`: none
- `feature_name.h`: none
- `ff_math.h`: 1 row
- `ff_pair.h`: 1 row
- `float_adm_gpu_common.h`: none (`size_t` index helpers)
- `float_moment_sum.h`: 5 rows
- `float_moment_sum_gpu.h`: 1 row
- `float_motion_sad.h`: 1 row
- `float_psnr_rows.h`: 1 row
- `float_vif_gpu_common.h`: none (`size_t` index helpers, `fvif_term_index`)
- `integer_adm.h`: 2 rows
- `integer_adm_kernels.h`: 25 rows
- `integer_motion.h`: 2 rows
- `integer_ssim.h`: none (six-int64 layout only)
- `integer_vif.h`: 1 row
- `integer_vif_sv_sq.h`: 1 row
- `luminance_tools.h`: none
- `mkdirp.h`: none
- `moment.h`: none
- `moment_options.h`: none
- `motion.h`: none
- `motion_blend_tools.h`: none
- `motion_options.h`: none
- `motion_tools.h`: none
- `motion_window.h`: none
- `ms_ssim.h`: none
- `ms_ssim_decimate.h`: none
- `niqe_math.h`: none
- `niqe_model.h`: none (double model constants; grep-checked)
- `nonfinite_score.h`: none (`scale * 2u` array index ≤ 7)
- `offset.h`: none
- `ordered_sum.h`: 1 row
- `perceptual_weight.h`: none
- `picture_copy.h`: none
- `psnr.h`: none
- `psnr_hvs_score.h`: none
- `psnr_options.h`: none
- `psnr_score.h`: 1 row
- `psnr_tools.h`: none
- `pu21_math.h`: none
- `pu21_ssim.h`: none
- `simd_dx.h`: none (documentation macros)
- `speed_cov.h`: none
- `speed_givens.h`: none
- `speed_gpu_common.h`: none (uint32 geometry fields, no products)
- `speed_internal.h`: none (`uint64_t` solve counters, one per solve)
- `speed_matmul.h`: none
- `ssim.h`: none
- `ssimulacra2_eotf_lut.h`: 1 row
- `ssimulacra2_math.h`: 1 row
- `ssimulacra2_pixel_format.h`: 1 row
- `ssimulacra2_score.h`: 1 row
- `ssimulacra2_simd_common.h`: 1 row
- `transnet_v2_score.h`: none
- `vif.h`: none
- `vif_log2_table.h`: 1 row
- `vif_options.h`: none
- `vif_tools.h`: none
- `feature_collector.cpp`: 1 row
- `feature_extractor.cpp`: none (registry loops, `malloc(sizeof)`)
- `feature_name.cpp`: none (string sizes; double bit compare)
- `luminance_tools.cpp`: none (double EOTFs)
- `mkdirp.cpp`: none
- `picture_copy.cpp`: 1 row
- `psnr_tools.cpp`: none (lookup table)
- `iqa/convolve.c`: 2 rows
- `iqa/convolve.h`: none
- `iqa/decimate.c`: 2 rows
- `iqa/decimate.h`: none
- `iqa/decimate_dim.h`: none
- `iqa/iqa.h`: none
- `iqa/iqa_options.h`: none
- `iqa/iqa_os.h`: none
- `iqa/math_utils.c`: 2 rows
- `iqa/math_utils.h`: none
- `iqa/ssim_accumulate_lane.h`: none
- `iqa/ssim_simd.h`: none
- `iqa/ssim_tools.c`: 2 rows
- `iqa/ssim_tools.h`: none
- `common/alignment.c`: none (`vmaf_floorn` / `vmaf_ceiln` on small ints)
- `common/alignment.h`: none
- `common/blur_array.c`: none (`size_t` buffer size; no caller outside the file)
- `common/blur_array.h`: none
- `common/convolution.c`: 1 row
- `common/convolution.h`: none
- `common/convolution_avx.c`: none (float; `(ptrdiff_t)i * stride` rows)
- `common/convolution_avx512.c`: none (float; `(ptrdiff_t)i * stride` rows)
- `common/convolution_internal.h`: 1 row
- `common/fmaf_exact.h`: none
- `common/macros.h`: none
- `third_party/xiph/psnr_hvs.c`: 5 rows

### CPU scalar, `core/src/` runtime

- `core/src/picture.c`: 3 rows
- `core/src/picture_pool.cpp`: 1 row
- `core/src/gpu_picture_pool.cpp`: 1 row
- `core/src/mem.cpp`: 1 row
- `core/src/libvmaf.c`: 3 rows (read-path sizes only)

### x86 SIMD, `core/src/feature/x86/`

- `x86/adm_avx2.c`: 17 rows
- `x86/adm_avx2.h`: none (prototypes and comments)
- `x86/adm_avx512.c`: 14 rows
- `x86/adm_avx512.h`: none (prototypes and comments)
- `x86/cambi_avx2.c`: 4 rows
- `x86/cambi_avx2.h`: none (prototypes and comments)
- `x86/cambi_avx512.c`: 4 rows
- `x86/cambi_avx512.h`: none (prototypes and comments)
- `x86/ciede_avx2.c`: 1 row
- `x86/ciede_avx2.h`: none (prototypes and comments)
- `x86/ciede_avx512.c`: 1 row
- `x86/ciede_avx512.h`: none (prototypes and comments)
- `x86/convolve_avx2.c`: 1 row
- `x86/convolve_avx2.h`: none (prototypes and comments)
- `x86/convolve_avx512.c`: 1 row
- `x86/convolve_avx512.h`: none (prototypes and comments)
- `x86/float_adm_avx2.c`: 1 row
- `x86/float_adm_avx2.h`: none (prototypes and comments)
- `x86/float_adm_avx512.c`: 1 row
- `x86/float_adm_avx512.h`: none (prototypes and comments)
- `x86/float_motion_avx2.c`: 1 row
- `x86/float_motion_avx2.h`: none (prototypes and comments)
- `x86/float_motion_avx512.c`: 1 row
- `x86/float_motion_avx512.h`: none (prototypes and comments)
- `x86/float_psnr_avx2.c`: 1 row
- `x86/float_psnr_avx2.h`: none (prototypes and comments)
- `x86/float_psnr_avx512.c`: 1 row
- `x86/float_psnr_avx512.h`: none (prototypes and comments)
- `x86/integer_ssim_avx2.c`: 3 rows
- `x86/integer_ssim_avx2.h`: none (prototypes and comments)
- `x86/moment_avx2.c`: 1 row
- `x86/moment_avx2.h`: none (prototypes and comments)
- `x86/moment_avx512.c`: 1 row
- `x86/moment_avx512.h`: none (prototypes and comments)
- `x86/motion_avx2.c`: 5 rows
- `x86/motion_avx2.h`: none (prototypes and comments)
- `x86/motion_avx512.c`: 10 rows
- `x86/motion_avx512.h`: none (prototypes and comments)
- `x86/ms_ssim_decimate_avx2.c`: 1 row
- `x86/ms_ssim_decimate_avx2.h`: none (prototypes and comments)
- `x86/ms_ssim_decimate_avx512.c`: 1 row
- `x86/ms_ssim_decimate_avx512.h`: none (prototypes and comments)
- `x86/psnr_avx2.c`: 4 rows
- `x86/psnr_avx2.h`: none (prototypes and comments)
- `x86/psnr_avx512.c`: 2 rows
- `x86/psnr_avx512.h`: none (prototypes and comments)
- `x86/psnr_hvs_avx2.c`: 3 rows
- `x86/psnr_hvs_avx2.h`: none (prototypes and comments)
- `x86/speed_avx2.c`: 1 row
- `x86/speed_avx2.h`: none (prototypes and comments)
- `x86/speed_avx512.c`: 1 row
- `x86/speed_avx512.h`: none (prototypes and comments)
- `x86/speed_matmul_avx2.c`: 1 row
- `x86/speed_matmul_avx2.h`: none (prototypes and comments)
- `x86/speed_matmul_avx512.c`: 1 row
- `x86/speed_matmul_avx512.h`: none (prototypes and comments)
- `x86/ssim_avx2.c`: 1 row
- `x86/ssim_avx2.h`: none (prototypes and comments)
- `x86/ssim_avx512.c`: 1 row
- `x86/ssim_avx512.h`: none (prototypes and comments)
- `x86/ssimulacra2_avx2.c`: 2 rows
- `x86/ssimulacra2_avx2.h`: none (prototypes and comments)
- `x86/ssimulacra2_avx512.c`: 1 row
- `x86/ssimulacra2_avx512.h`: none (prototypes and comments)
- `x86/ssimulacra2_host_avx2.c`: 1 row
- `x86/ssimulacra2_host_avx2.h`: none (prototypes and comments)
- `x86/vif_avx2.c`: 10 rows
- `x86/vif_avx2.h`: none (prototypes and comments)
- `x86/vif_avx512.c`: 9 rows
- `x86/vif_avx512.h`: none (prototypes and comments)
- `x86/vif_statistic_avx2.c`: 1 row
- `x86/vif_statistic_avx2.h`: none (prototypes and comments)

### arm64 SIMD, `core/src/feature/arm64/`

- `arm64/adm_neon.c`: 5 rows
- `arm64/adm_neon.h`: none (prototypes and comments)
- `arm64/cambi_neon.c`: 3 rows
- `arm64/cambi_neon.h`: none (prototypes and comments)
- `arm64/ciede_neon.c`: 1 row
- `arm64/ciede_neon.h`: none (prototypes and comments)
- `arm64/convolve_neon.c`: 1 row
- `arm64/convolve_neon.h`: none (prototypes and comments)
- `arm64/float_adm_dwt2_neon.c`: 1 row
- `arm64/float_adm_neon.c`: 1 row
- `arm64/float_adm_neon.h`: none (prototypes and comments)
- `arm64/float_motion_neon.c`: 1 row
- `arm64/float_motion_neon.h`: none (prototypes and comments)
- `arm64/float_psnr_neon.c`: 1 row
- `arm64/float_psnr_neon.h`: none (prototypes and comments)
- `arm64/moment_neon.c`: 1 row
- `arm64/moment_neon.h`: none (prototypes and comments)
- `arm64/moment_sve2.c`: 1 row
- `arm64/moment_sve2.h`: none (prototypes and comments)
- `arm64/motion_neon.c`: 2 rows
- `arm64/motion_neon.h`: none (prototypes and comments)
- `arm64/motion_v2_neon.c`: 4 rows
- `arm64/motion_v2_neon.h`: none (prototypes and comments)
- `arm64/ms_ssim_decimate_neon.c`: 1 row
- `arm64/ms_ssim_decimate_neon.h`: none (prototypes and comments)
- `arm64/psnr_hvs_neon.c`: 3 rows
- `arm64/psnr_hvs_neon.h`: none (prototypes and comments)
- `arm64/psnr_neon.c`: 2 rows
- `arm64/psnr_neon.h`: none (prototypes and comments)
- `arm64/speed_neon.c`: 1 row
- `arm64/speed_neon.h`: none (prototypes and comments)
- `arm64/ssim_neon.c`: 1 row
- `arm64/ssim_neon.h`: none (prototypes and comments)
- `arm64/ssimulacra2_arm64_common.h`: 1 row
- `arm64/ssimulacra2_host_neon.c`: 1 row
- `arm64/ssimulacra2_host_neon.h`: none (prototypes and comments)
- `arm64/ssimulacra2_neon.c`: 1 row
- `arm64/ssimulacra2_neon.h`: none (prototypes and comments)
- `arm64/ssimulacra2_sve2.c`: 1 row
- `arm64/ssimulacra2_sve2.h`: none (prototypes and comments)
- `arm64/vif_neon.c`: 5 rows
- `arm64/vif_neon.h`: none (prototypes and comments)
