<!-- markdownlint-disable MD060 -->
<!--
SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
Copyright 2026 Lusoris
-->
# Research-0734: CUDA `__mul24` silent-corruption sweep (CUDA 13.3 fix surface)

**Status:** Complete
**Date:** 2026-05-28
**Author:** cuda-reviewer agent

---

## Summary

**Zero `__mul24` (or `__umul24` / `__mul24hi`) calls exist anywhere in
`core/src/feature/cuda/` or `core/src/cuda/`.** The CUDA 13.3 silent-corruption
bug (present since CUDA 11.1) has no exposure in this codebase.

---

## Scope

All `.cu`, `.cuh`, `.c`, `.h` files under:

- `/core/src/feature/cuda/` — 68 files (kernels for ADM, VIF, motion, SSIM, MS-SSIM,
  PSNR, PSNR-HVS, CAMBI, CIEDE, moment, SSIMULACRA2, SpEED, float-twin extractors)
- `/core/src/cuda/` — 10 files (runtime: common, picture, dispatch, kernel template)

---

## Inventory

```text
grep -rn '__mul24\|__umul24\|__mul24hi' \
    core/src/feature/cuda/ core/src/cuda/
# → no output; exit 1 (grep found nothing)
```

**Total `__mul24` sites: 0**

---

## Classification breakdown

| Classification | Count |
|---|---|
| (a) Both args runtime-variable — SAFE | 0 |
| (b) One arg compile-time constant — BUG IMPACTED | 0 |
| (c) Both args compile-time constant — constexpr-foldable | 0 |

---

## CPU-reference cross-check

The kernels that were most likely to have used `__mul24` as a "performance
optimisation for 24-bit-range data" are:

- `integer_vif/filter1d.cu` — uses standard C integer multiply for accumulation;
  no `__mul24`.
- `integer_adm/adm_dwt2.cu`, `adm_decouple.cu`, `adm_csf.cu` — uses standard `*`
  throughout.
- `integer_motion/motion_score.cu` — uses standard `*` and `>>` for SAD/mean.
- `integer_ssim/ssim_score.cu`, `integer_ms_ssim/ms_ssim_score.cu` — uses
  standard `*` for cross-product terms.

In each case the CPU reference path (`core/src/feature/*.c`) also uses plain C
multiply operators. No divergence between CPU and CUDA multiply semantics exists.

---

## Evidence

The bug surface condition is:

1. `__mul24(val, CONSTANT)` where `CONSTANT` is a compile-time-constant integer.
2. Either operand may silently produce wrong results on CUDA 11.1–13.2 JIT.
3. Fixed in CUDA 13.3 per NVIDIA release notes (surfaced in PR #64 digest).

Because the codebase contains zero calls to any `__mul24` variant, condition (1)
is never met. No divergence evidence can be produced because there is nothing to
test.

---

## Score-correctness conclusion

**No score impact.** The CUDA 13.3 fix has no numerical effect on this fork
because the affected intrinsic is absent from the entire CUDA kernel tree.

---

## Recommendation

No in-tree code change is required. The audit finding is documented here and
in `docs/backends/cuda/overview.md` (§ Known gaps / CUDA version notes) so
future contributors know the search was performed.

The `AGENTS.md` invariant note for `core/src/feature/cuda/` is updated to
record the prohibition on introducing `__mul24` — any future use must cite a
CODEOWNERS-approved rationale noting CUDA version constraints.

---

## Reproducer

```bash
# From the worktree root:
grep -rn '__mul24\|__umul24\|__mul24hi' \
    core/src/feature/cuda/ core/src/cuda/
# Expected: no output (exit 1)
```
