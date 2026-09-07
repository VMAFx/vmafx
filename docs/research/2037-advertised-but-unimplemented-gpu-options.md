<!-- markdownlint-disable MD013 -->

# 2037 — Advertised-but-unimplemented options on the float-ADM GPU twins

**Date**: 2026-09-07
**Scope**: `adm_p_norm`, `adm_bypass_cm` and `adm_skip_scale0` on the CUDA,
SYCL, HIP and Metal `float_adm` twins.
**Outcome**: all three implemented
([ADR-1220](../adr/1220-gpu-float-adm-options-reach-kernels.md)); per-option
parity variants added to every backend's float-ADM test.

## The pattern, third instance

An option is declared in a twin's `VmafOption options[]` table with the CPU's
name, alias, default and range. The framework accepts it, range-checks it, and
folds it into the derived feature name (ADR-1183). The kernel then ignores it.

The result is worse than an unimplemented feature: the output schema *asserts*
that the setting was applied. A score filed under `adm2_apn_2` says "this is
adm2 with p = 2" and is not.

This is the third instance in a week:

| Digest / ADR | Option | Symptom |
| --- | --- | --- |
| [2033](2033-identity-default-option-blind-spot.md) / ADR-1216 | `motion_fps_weight` | applied twice, so `motion3` carried it squared |
| [2034](2034-kernel-local-constants-shadowing-options.md) / ADR-1217 | `vif_sigma_nsq`, `vif_enhn_gain_limit` | kernel-local constants shadowed the option names |
| This digest / ADR-1220 | `adm_p_norm`, `adm_bypass_cm`, `adm_skip_scale0` | hardcoded cube + `1/3` root; option read by nothing |

## `adm_p_norm` — four application points, one honoured

`adm_tools.c` applies the p-norm in four places:

```c
/* 1. DLM numerator sum, and 2. CSF denominator sum — both branch on p == 3 */
if (c->adm_p_norm == 3.0) { inner[0] += (xh * xh * xh); }
else                      { inner[0] += powf(xh, c->adm_p_norm); }

/* 3. pooling root, and 4. the noise constant */
powf(accum[0], 1.0f / adm_p_norm) + get_noise_constant(w, h, weight, adm_p_norm)
/* get_noise_constant := powf(w * h * weight, 1.0f / adm_p_norm) */
```

All four twins hardcoded the cube in the kernel and `1.0f / 3.0f` in the host
pooling, and used `adm_p_norm` for the AIM exponent alone. So a non-default
`apn` produced **a sum of cubes raised to 1/p** — a quantity that is neither
the p=3 result nor the p-norm — with `adm2` and every `adm_scaleN` left at
p = 3.

Measured at `adm_p_norm = 2.0` against the `1e-4` ADR-0214 gate:

| Backend | key | CPU | GPU | delta |
| --- | --- | --- | --- | --- |
| CUDA (RTX 4090) | `adm2_apn_2` | `0.43097075` | `0.45416959` | `2.32e-02` |
| SYCL (Arc A380) | `adm_scale0_apn_2` | `0.90092957` | `0.89784085` | `3.09e-03` |
| HIP (gfx1030) | `adm2_apn_2` | `0.99818595` | `0.99833104` | `1.45e-04` |

## `adm_bypass_cm` — declared, stored, read by nothing

On CUDA and Metal, `grep -n adm_bypass_cm` over the twin returns exactly two
hits: the `FloatAdmState*` field and the option-table entry. The kernel computes
the 3x3 masking threshold unconditionally. `bcm=1` was a no-op.

The mechanical check for this whole class is one grep per option name per twin:
**if the only hits are the struct field and the option entry, the option is
advertised and unimplemented.**

## `adm_skip_scale0` — a pooling rule mistaken for a reporting rule

`adm.c` takes the lo-pass-only DWT at scale 0 and leaves `num_scale = 0` with
`den_scale = 1e-10`, so scale 0 contributes nothing to the pooled `adm2` / `aim`.
The Metal twin zeroed only the *reported* `adm_scale0` sub-score and still
folded the full scale-0 numerator and denominator into the pooled score — a
first-order change on every frame, dressed up as a correct-looking output.

No kernel change was needed to fix it: `adm_dwt2_lo_s` writes only `band_a`,
which `adm_dwt2` computes identically, so scales 1..3 are unaffected.

## Two things this cost that are worth remembering

**Keep the CPU's `p == 3` fast path in the kernel.** The obvious
simplification — always call `powf(x, p)` — would move the *default* path,
because device `powf(x, 3.0f)` is not guaranteed to equal `x * x * x`. Every
shipped model runs at p = 3. The kernels carry
`fadm_pnorm_term(x, p) = (p == 3.0f) ? x*x*x : powf(x, p)`, mirroring the CPU
branch exactly, and the pre-existing default-options parity tests stay green as
the proof.

**Aggregate-only parity tests are not sensitive enough.** The SYCL float-ADM
test compared `adm2` alone. On its fixture the p-norm defect does not move
`adm2` past the gate at all — the test passed against the *unfixed* code. Only
after widening it to all five features (`adm2` plus the four `adm_scaleN`) did
`adm_scale0` show the `3.09e-03` drift. The per-scale sub-scores are where a
kernel-vs-CPU divergence surfaces first; the CUDA test's own comment says so,
and the SYCL one had not followed it.

## Remaining candidates

The grep test above, run across the other feature families, still flags:

- `adm_bypass_cm` is not declared at all by the SYCL and HIP `float_adm` twins,
  so they reject it where CPU, CUDA and Metal accept it. That is a loud failure
  rather than a wrong answer, so it is a feature gap rather than a defect —
  tracked in `docs/state.md`.
- Every `VMAF_OPT_TYPE_BOOL` feature param is half-covered by definition: a
  default-options test only ever exercises one of its two values.
