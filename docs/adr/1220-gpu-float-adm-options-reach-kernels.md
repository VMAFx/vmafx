<!-- markdownlint-disable MD013 MD041 MD060 -->

# ADR-1220: The GPU float-ADM kernels honour `adm_p_norm`, `adm_bypass_cm` and `adm_skip_scale0`

- **Status**: Proposed
- **Date**: 2026-09-07
- **Deciders**: Lusoris
- **Tags**: `cuda`, `sycl`, `hip`, `metal`, `correctness`, `feature-extractor`, `testing`

## Context

`float_adm` exposes three `VMAF_OPT_FLAG_FEATURE_PARAM` options that the GPU
twins declare with the CPU's names, aliases, defaults and ranges — and then did
not implement.

**`adm_p_norm`** (alias `apn`, default `3.0`, range `1..20`) is the exponent of
the ADM p-norm. The CPU applies it in four places: the DLM numerator sum and the
CSF denominator sum in `adm_tools.c`, each of which special-cases `p == 3` to a
literal cube and otherwise calls `powf(x, p)`; the pooling root
`powf(accum, 1.0f / adm_p_norm)`; and `get_noise_constant(w, h, weight, p)`,
which is `powf(w * h * weight, 1.0f / p)`.

All four twins hardcoded the cube in their kernels and `1.0f / 3.0f` in their
host pooling, and applied `adm_p_norm` to the AIM exponent alone. A non-default
`apn` therefore produced a hybrid: a sum of cubes raised to `1/p`, with the
adm2 and `adm_scaleN` sub-scores left entirely at `p = 3`.

**`adm_bypass_cm`** (alias `bcm`, default `0`) drops the 3x3 contrast-masking
threshold from the numerator — `adm_tools.c::adm_cm_accum_px_s` computes `thr`
only `if (c->adm_bypass_cm == 0)`, and `adm.c` passes it to both the DLM and the
AIM `adm_cm()` call. The CUDA and Metal twins declared the option, stored it in
their state struct, and **read it from nowhere**: `grep` over each twin returns
only the struct field and the option-table entry. SYCL and HIP do not declare it
at all, so they reject it rather than ignoring it.

**`adm_skip_scale0`** (alias `ssz`, default `false`) makes `adm.c` take the
lo-pass-only DWT at scale 0 and leave `num_scale = 0` with `den_scale = 1e-10`,
so scale 0 contributes nothing to the pooled `adm2` / `aim`. The Metal twin —
the only one of the four that declares the option — zeroed only the *reported*
`adm_scale0` sub-score while still folding the full scale-0 numerator and
denominator into the pooled score.

Nothing caught any of this. Every `float_adm` parity test instantiated its
extractors with `NULL` options, where `p = 3` **is** the hardcoded exponent and
`bypass = 0` **is** the hardcoded behaviour — the same default-options blind
spot as ADR-1216 and ADR-1217.

## Decision

We will pass `adm_p_norm` into the CSF/CM and AIM-CM kernels on all four
backends and use `1.0f / adm_p_norm` in the host pooling and the noise
constant; pass `adm_bypass_cm` into both kernels on CUDA and Metal and skip the
masking threshold when it is set; and give the Metal host the CPU's
`num_scale = 0, den_scale = 1e-10` treatment for scale 0 under
`adm_skip_scale0`. Each backend's float-ADM parity test gains a variant per
option it declares, reading the ADR-1183-derived key.

The kernels mirror the CPU's own `p == 3` fast path exactly, so the default
path stays bit-identical — confirmed by the pre-existing parity tests, which
remain green.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Thread the options into the kernels (chosen) | The advertised surface starts working on GPU; default path provably unchanged | Two extra kernel arguments; on Metal, two uniform slots | — |
| Reject non-default values in `init()` with `-EINVAL` | Small, honest, no numerical risk | Turns a silent wrong answer into a hard failure for a documented option, on four backends at once | Removes a working surface instead of implementing it |
| Drop the three options from the GPU option tables | Makes the declared surface match behaviour | The ADR-1183-derived feature name would then differ from the CPU's for the same model, so model lookup would miss; and it deletes documented options | Breaks name derivation and removes surfaces |
| Always call `powf(x, p)` in the kernels, no `p == 3` branch | Simpler kernel | Device `powf(x, 3.0f)` is not guaranteed to equal `x * x * x`, so the default path — every shipped model — could drift | Unacceptable risk to the default |
| Add `adm_bypass_cm` to the SYCL and HIP tables too | Full four-way parity of the option surface | Those twins currently *reject* it, which is a loud failure rather than a wrong answer; adding it is a feature, not a fix | Deferred; tracked in `docs/state.md` |

## Consequences

- **Positive**: `--feature float_adm_<backend>:apn=<x>` and `:bcm=1` now do what
  they say, and Metal's `ssz=1` matches the CPU. Any model that sets these
  scores the same on CPU and GPU.
- **Negative**: any GPU run that set one of these options produced a wrong
  number and must be re-scored. No shipped model sets them — every in-tree model
  leaves all three at their defaults — so nothing in the repo changes.
- **Neutral / follow-ups**: at `p != 3` the twins call the device `powf`, whose
  rounding is not guaranteed identical to the host's. The gate is the places=4
  cross-backend tolerance (ADR-0214), not ULP, and the measured deltas are far
  inside it. SYCL and HIP still do not declare `adm_bypass_cm`; adding it is
  tracked separately.

## References

- Findings 98 / 99 / 100 / 101 (`adm_p_norm`), 102 / 103 (`adm_bypass_cm`) and
  104 (`adm_skip_scale0`) from the twin-drift sweep.
- Measured before the fix, 256x144 8-bpc fixture at `adm_p_norm = 2.0`, against
  the `1e-4` ADR-0214 gate: CUDA `adm2_apn_2` `cpu = 0.43097075` vs
  `gpu = 0.45416959`, delta `2.32e-02`; HIP `cpu = 0.99818595` vs
  `gpu = 0.99833104`, delta `1.45e-04` (the HIP fixture is less discriminating,
  but still over the gate).
- [ADR-0214](0214-gpu-parity-ci-gate.md) — the GPU parity CI gate.
- [ADR-1183](1183-model-options-gate-gpu-twin-selection.md) — the derived
  feature-name keys (`adm2_apn_2`, `adm2_bcm_1`) the parity variants read.
- ADR-1216 (PR #1375) and ADR-1217 (PR #1376) — the same default-options blind
  spot on `motion_fps_weight` and the float-VIF options. Referenced by number
  because this branch does not carry those files.
