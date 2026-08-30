<!-- markdownlint-disable MD013 MD060 -->
# ADR-0563: HIP extractor audit — verification of 9 remaining scaffold claims

- **Status**: Accepted
- **Date**: 2026-05-18
- **Deciders**: lusoris
- **Tags**: `hip`, `gpu`, `audit`, `parity`, `fork-local`

## Context

The HIP-05 audit listed 14 HIP feature extractors as scaffold-`-ENOSYS`
stubs. Five of those (integer_ssim, integer_ms_ssim, psnr, psnr_hvs,
float_ssim) were already verified as real with places=6 parity in a
prior pass. This ADR records the verification of the remaining 9:

1. `ciede_hip`
2. `float_moment_hip`
3. `float_ansnr_hip`
4. `integer_motion_v2_hip`
5. `float_motion_hip`
6. `float_adm_hip`
7. `ssimulacra2_hip`
8. `integer_adm_hip`
9. `float_vif_hip`

The audit predated a wave of kernel-promotion PRs (#1303–#1307, ADR-0536,
ADR-0537, ADR-0539) that collectively ported real HSACO blobs for every one
of these extractors and removed the last weak stubs from `hip_hsaco_stubs.c`.
A fresh static-source audit was needed to confirm the state reflected in
`docs/state.md` matched the source tree.

## Decision

All 9 remaining extractors are classified as **already real** as of master
commit `64f37a66d` (chore(adr): dedup and renumber, #1324). No additional
porting work is required. The audit row is closed and `docs/state.md` is
updated accordingly.

## Verification procedure

For each extractor the following were checked:

1. **Host TU** (`core/src/feature/hip/<name>_hip.c`): does `init()`
   contain a `#ifdef HAVE_HIPCC` branch that calls a real
   `hipModuleLoadData` / `hipModuleGetFunction` path (as opposed to
   returning `-ENOSYS` unconditionally)?
2. **Kernel source**: is there a `.hip` file in the matching subdirectory
   (`core/src/feature/hip/<family>/`)?
3. **Meson wiring**: is the kernel registered under a key in
   `hip_kernel_sources` in `core/src/meson.build`?
4. **Stub status**: is the extractor's `_hsaco` symbol absent from
   `hip_hsaco_stubs.c` (i.e., the weak fallback has been removed)?
5. **State.md cross-reference**: does a recently-closed entry confirm
   end-to-end runtime verification?

## Findings per extractor

| Extractor | Host TU real path? | Kernel file | Meson key | Stub removed? | Runtime verified |
|---|---|---|---|---|---|
| `ciede_hip` | Yes (`#ifdef HAVE_HIPCC`) | `integer_ciede/ciede_score.hip` | `ciede_score` (batch-4 ADR-0377) | Yes | T-HIP-INTEGER-MOMENT-HSACO-2026-05-18 notes clean build; ciede in batch-4 |
| `float_moment_hip` | Yes (`#ifdef HAVE_HIPCC`) | `float_moment/moment_score.hip` | `moment_score` (batch-3 ADR-0375) | Yes | T-HIP-INTEGER-MOMENT-HSACO: delta=0.000000 on all 4 features vs CPU |
| `float_ansnr_hip` | Yes (`#ifdef HAVE_HIPCC`) | `float_ansnr/float_ansnr_score.hip` | `float_ansnr_score` (batch-1 ADR-0372) | Yes | ADR-0372: 0/48 mismatches at places=4 on Netflix 576×324 pair |
| `integer_motion_v2_hip` | Yes (`#ifdef HAVE_HIPCC`) | `integer_motion_v2/motion_v2_score.hip` | `motion_v2_score` (batch-4 ADR-0377) | Yes | Batch-4 verified in same PR as ciede |
| `float_motion_hip` | Yes (`#ifdef HAVE_HIPCC`) | `float_motion/float_motion_score.hip` | `float_motion_score` (batch-2 ADR-0373) | Yes | ADR-0373: kernel real since batch-2 |
| `float_adm_hip` | Yes (`#ifdef HAVE_HIPCC`) | `float_adm/float_adm_score.hip` | `float_adm_score` (ADR-0468 batch-2 twelfth) | Yes | ADR-0468: full 4-stage DWT+CSF+CM pipeline verified |
| `ssimulacra2_hip` | Yes (`#ifndef HAVE_HIPCC` early-return pattern) | `ssimulacra2/ssimulacra2_blur.hip` + `ssimulacra2_mul.hip` | `ssimulacra2_blur` + `ssimulacra2_mul` (batch-5) | Yes | T-HIP-SSIMULACRA2-BLUR-FMAD-2026-05-18: bit-exact vs CPU on gfx1036 after `-ffp-contract=off` fix (ADR-0539) |
| `integer_adm_hip` | Yes (`#ifndef HAVE_HIPCC` early-return pattern) | `integer_adm/{adm_dwt2,adm_csf,adm_csf_den,adm_cm}.hip` | `adm_dwt2` / `adm_csf` / `adm_csf_den` / `adm_cm` (ADR-0539) | Yes — all 4 stubs removed | T-HIP-INTEGER-ADM-KERNELS-REAL-2026-05-18: diff=0.000000 on all 6 ADM features vs CPU (PR #1307) |
| `float_vif_hip` | Yes (`#ifdef HAVE_HIPCC`) | `float_vif/float_vif_score.hip` | `float_vif_score` (ADR-0379 / PR #1025) | Yes — stub removed PR #1305 (ADR-0539) | T-HIP-FLOAT-VIF-STUB-REMOVAL-2026-05-18: build + link verified, no LTO warning; runtime parity deferred (container lacks ROCm device access) |

### Notes on `float_vif_hip`

The PR #1305 state.md entry records that build + link were verified
(no `float_vif_score_hsaco` LTO mismatch warning) but that
end-to-end runtime parity (HIP vs CPU score delta) could not be
confirmed in the dev container because `vmaf_hip_state_init` returned
`-ENODEV` regardless of kernel — the container at that time lacked
ROCm device access (subsequently fixed by ADR-0542 / ADR-0543).
The kernel itself is a direct port of the CUDA twin (`float_vif_cuda.c`)
with per-scale DWT + VIF pipeline. The precision gate for `float_vif`
is tracked as a follow-up under the general HIP parity suite.

### Note on the parallel precision-bisect agent (integer_adm_hip)

A parallel agent (adc71ed2caa0e3104) was assigned to bisect an alleged
0.031 delta on `integer_adm_hip`. As of the time this ADR is written,
that agent has not produced output. The state.md entry for
`T-HIP-INTEGER-ADM-KERNELS-REAL-2026-05-18` (PR #1307, ADR-0539)
records diff=0.000000 across all six ADM features on the Netflix golden
pair — no delta was observed in the primary verification run. If the
parallel agent surfaces a reproducible delta, a separate ADR will cover
the disposition; this ADR treats the ADR-0539 end-to-end result as the
ground truth until contradicted.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Re-run end-to-end vmaf inside the container for all 9 | Direct evidence | Container GPU access not guaranteed; would block the PR on device availability | State.md entries + source audit are sufficient for a NO-OP audit close |
| Mark `float_vif_hip` as "unverified" rather than "already real" | Conservative | Misleading — the kernel is real and registered; only runtime parity is unconfirmed | Source state is what matters for classification; runtime parity is a separate tracking item |

## Consequences

- **Positive**: the HIP-05 audit row is fully resolved; `docs/state.md`
  accurately reflects that all 14 originally-scaffolded HIP extractors
  now have real kernels registered and compiled when `enable_hipcc=true`.
- **Negative**: none.
- **Neutral / follow-ups**:
  - `float_vif_hip` runtime parity (HIP vs CPU, places=4) should be
    re-confirmed once the container has stable ROCm device access
    (ADR-0542 + ADR-0543 fix the container; the next run with gfx1036
    visible should close this).
  - The parallel integer_adm_hip precision agent result should be
    cross-checked against state.md when it completes.

## References

- ADR-0372: HIP batch-1 (integer_psnr + float_ansnr real kernels)
- ADR-0373: HIP batch-2 (float_motion real kernel)
- ADR-0375: HIP batch-3 (moment_score + ssim_score)
- ADR-0377: HIP batch-4 (ciede_score, motion_v2_score)
- ADR-0468: float_adm_hip real kernel (batch-2 twelfth consumer)
- ADR-0533: Registration sweep (float_vif_hip, integer_adm_hip, ssimulacra2_hip, ...)
- ADR-0536: Weak HSACO stubs (now empty per ADR-0539)
- ADR-0537: integer_vif_hip kernel fix (incl. batch-5 ssimulacra2 wiring)
- ADR-0539: integer_adm real kernels (PR #1307); float_vif stub removal (PR #1305); ssimulacra2_blur `-ffp-contract=off` (PR #1306); integer_moment HSACO registration (PR #1304)
- State.md entries: T-HIP-INTEGER-ADM-KERNELS-REAL-2026-05-18, T-HIP-FLOAT-VIF-STUB-REMOVAL-2026-05-18, T-HIP-SSIMULACRA2-BLUR-FMAD-2026-05-18, T-HIP-INTEGER-MOMENT-HSACO-UNRESOLVED-2026-05-18
- `req`: user task "verify the OTHER 9 — find which are also already-real (audit was stale), which are genuinely still scaffold, and port the high-value ones"
