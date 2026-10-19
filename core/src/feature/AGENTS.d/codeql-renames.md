---
paths:
  - core/src/feature/integer_adm.c
  - core/src/feature/integer_vif.c
  - core/src/feature/cambi.c
invariant: CodeQL declaration-hides-variable renames preserve bit-exact behavior across extractors.
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# CodeQL Declaration Hides Variable Renames Invariant

## CodeQL `cpp/declaration-hides-variable` rename invariants (2026-05-09)

64-alert sweep of 2026-05-09 (see
[`docs/rebase-notes.md`](../../../../docs/rebase-notes.md) entry of
same date) renamed inner-scope shadows in
`x86/adm_avx2.c`, `x86/adm_avx512.c`, `x86/vif_avx2.c`,
`x86/vif_avx512.c`, plus `cambi.c`. **Do not let upstream port
re-introduce unprefixed names** — CodeQL re-flags them and
strict `cpp/declaration-hides-variable` gate trips. rebase-safe
identifier dictionary is:

| Surface | Old (origin/Netflix) | New (fork) |
|---------|----------------------|------------|
| ADM AVX2/AVX-512 horizontal pass | `j == 0` block at top of i-loop using `j0`/`j1`/`j2`/`j3`/`s0`/`s1`/`s2`/`s3` | same names but wrapped in tight `{ ... }` block; per-`j` tail loop owns names afterwards |
| ADM AVX2/AVX-512 horizontal pass | inner `__m256i add_shift_HP_vex = _mm256_set1_epi32(32768)` | removed (function-scope outer is bit-identical) |
| `i4_adm_cm_avx2` / `_avx512` rfactor splat | `__m256i rfactor0/1/2` (or `__m512i`) shadowing `float rfactor1[3]` | `rfactor_v0/_v1/_v2` |
| ADM AVX-512 8-bit angle path | `__m512i o_mag_sq` / `ot_dp` / `t_mag_sq` shadowing function-scope `int64_t` scalars | `o_mag_sq_v` / `ot_dp_v` / `t_mag_sq_v` |
| VIF AVX2 vertical-tap loop (`for tap`) | `__m256i f0` / `r0` / `r1` / `d0` / `d1` shadowing centre-tap broadcasts | `f_tap`, `r_top` / `r_bot`, `d_top` / `d_bot` |
| VIF AVX-512 vertical-tap loop (paired-tap) | `__m512i f0` / `f1` / `r0` / `r16` / `d0` / `d16` / `r1` / `r15` / `d1` / `d15` | `f_tap0` / `f_tap1`, `r_back0` / `r_fwd0`, `d_back0` / `d_fwd0`, `r_back1` / `r_fwd1`, `d_back1` / `d_fwd1` |
| VIF AVX2/AVX-512 horizontal-tap loops (`for fj`) | inner `__m256i fq` / `__m512i fq` re-broadcasting `vif_filt_*[fj]` | `f_tap` |
| VIF AVX2 ref/dis/refdis horizontal stage | inner `__m256i m0` / `m1` reading sliding window | `m_top` / `m_bot` |
| VIF AVX-512 16-bit / subsample tail loops | inner `int ii` / `const ptrdiff_t stride` / `uint16_t *ref` / `uint8_t *ref` / `uint16_t *dis` / `uint8_t *dis` | removed (function-scope outer is identical) |
| VIF AVX-512 tail residual reductions | inner `VifResiduals residuals` shadowing `Residuals512 residuals` | `tail_residuals` |
| VIF AVX-512 subsample horizontal tail | inner `const uint16_t fcoeff` shadowing `__m512i fcoeff` | `fcoeff_scalar` |
| `cambi.c` heatmap init | inner `int err` shadowing init-loop accumulator | `mkdir_err` |
| `cambi.c` full-ref extract path | inner `int err` shadowing dist-side accumulator | `src_err` |

Bit-exactness: sweep is provably no-op (renames + scope-tighten

- identical-typed deletes only). Netflix CPU golden 3 must remain
green across rebases — re-run
`PYTHONPATH=$PWD/python python3 -m pytest python/test/quality_runner_test.py
-k test_run_vmaf python/test/vmafexec_test.py
python/test/vmafexec_feature_extractor_test.py -m "not slow"` block
after port-upstream of any of these files.
