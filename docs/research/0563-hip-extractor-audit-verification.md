<!-- markdownlint-disable MD060 -->
# Research digest — HIP extractor audit (9 remaining scaffold claims) (ADR-0563)

- **Date**: 2026-05-18
- **Author**: lusoris + claude
- **Status**: completed
- **Trigger**: user task — verify the 9 HIP extractors that the HIP-05 audit
  listed as scaffold-ENOSYS but that had not yet been checked

## Question

The HIP-05 audit listed 14 HIP extractors as scaffold-ENOSYS stubs. Five were
already confirmed real in a prior pass. Which of the remaining 9 are actually
real (audit was stale after the kernel-promotion wave), and which still require
porting?

The 9: `ciede_hip`, `float_moment_hip`, `float_ansnr_hip`,
`integer_motion_v2_hip`, `float_motion_hip`, `float_adm_hip`,
`ssimulacra2_hip`, `integer_adm_hip`, `float_vif_hip`.

## Methodology

Static source audit against `core/src/feature/hip/<name>_hip.c` for each:

1. Does `init()` contain a `#ifdef HAVE_HIPCC` branch calling real HIP module
   APIs (`hipModuleLoadData`, `hipModuleGetFunction`)?
2. Does a `.hip` kernel source file exist in the matching subdirectory?
3. Is the kernel registered in `hip_kernel_sources` in `core/src/meson.build`?
4. Is the extractor's `_hsaco` symbol absent from `hip_hsaco_stubs.c`?

Cross-reference against `docs/state.md` recently-closed entries for
runtime-verification evidence.

## Findings

All 9 are already real. The audit was stale — the kernel-promotion wave
(PRs #1303–#1307, ADR-0539, plus earlier ADR-0372/0373/0375/0377/0468)
had promoted every one of these extractors before this verification pass.

### Kernel registration summary

| Extractor | Kernel file(s) | Meson key | ADR |
|---|---|---|---|
| `ciede_hip` | `integer_ciede/ciede_score.hip` | `ciede_score` | ADR-0377 batch-4 |
| `float_moment_hip` | `float_moment/moment_score.hip` | `moment_score` | ADR-0375 batch-3 |
| `float_ansnr_hip` | `float_ansnr/float_ansnr_score.hip` | `float_ansnr_score` | ADR-0372 batch-1 |
| `integer_motion_v2_hip` | `integer_motion_v2/motion_v2_score.hip` | `motion_v2_score` | ADR-0377 batch-4 |
| `float_motion_hip` | `float_motion/float_motion_score.hip` | `float_motion_score` | ADR-0373 batch-2 |
| `float_adm_hip` | `float_adm/float_adm_score.hip` | `float_adm_score` | ADR-0468 |
| `ssimulacra2_hip` | `ssimulacra2/ssimulacra2_blur.hip`, `ssimulacra2_mul.hip` | `ssimulacra2_blur`, `ssimulacra2_mul` | ADR-0537 batch-5 |
| `integer_adm_hip` | `integer_adm/{adm_dwt2,adm_csf,adm_csf_den,adm_cm}.hip` | `adm_dwt2` / `adm_csf` / `adm_csf_den` / `adm_cm` | ADR-0539 (PR #1307) |
| `float_vif_hip` | `float_vif/float_vif_score.hip` | `float_vif_score` | ADR-0379 (stub removed PR #1305) |

### hip_hsaco_stubs.c status

After PR #1307 (ADR-0539) the stubs file is effectively empty — the comment
block explains that the four integer_adm stubs were removed when their real
kernels landed, and that no other extractor currently requires a weak stub.
The macro definition is retained for future use during incremental porting.

### Runtime parity evidence from state.md

- `float_ansnr_hip`: 0/48 mismatches at places=4 vs CPU (ADR-0372)
- `float_moment_hip`: delta=0.000000 on all 4 features vs CPU
  (T-HIP-INTEGER-MOMENT-HSACO-UNRESOLVED-2026-05-18)
- `ssimulacra2_hip`: bit-exact vs CPU on gfx1036 after `-ffp-contract=off`
  (T-HIP-SSIMULACRA2-BLUR-FMAD-2026-05-18, ADR-0539)
- `integer_adm_hip`: diff=0.000000 across all 6 ADM features on Netflix
  golden src01 pair on gfx1036 (T-HIP-INTEGER-ADM-KERNELS-REAL-2026-05-18,
  ADR-0539, PR #1307)
- `float_vif_hip`: build + link verified (no LTO warning). Runtime parity
  not confirmed due to container lacking ROCm device access at the time of
  PR #1305; parity follow-up tracked.

### Key structural finding

`float_moment_hip.c` unconditionally includes `<hip/hip_runtime_api.h>`
(without a `#ifdef HAVE_HIPCC` guard — unlike the other extractors which
use `#ifdef HAVE_HIPCC` or common.h's `__has_include` probe). This is
harmless because the file is only compiled when `is_hip_enabled=true`,
which requires the HIP SDK to be installed. It is flagged here for
awareness in case future lint passes flag it as a style inconsistency.

## Conclusion

No porting work is needed. All 14 originally-scaffolded HIP extractors
now have real kernels registered and built when `enable_hipcc=true`.
The HIP-05 audit row is closed. `docs/state.md` updated; ADR-0563
records the disposition.
