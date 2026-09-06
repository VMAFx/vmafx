<!-- markdownlint-disable MD013 MD041 MD060 -->

# ADR-1214: The float-ADM GPU twins ignore `adm_csf_scale` in Watson mode and share the CPU's option aliases

- **Status**: Proposed
- **Date**: 2026-09-07
- **Deciders**: Lusoris
- **Tags**: cuda, sycl, hip, metal, correctness, feature-extractor, options

## Context

Two related drifts in the CUDA, SYCL, HIP and Metal `float_adm` twins, both
found by the twin-drift sweep and confirmed against the source before any code
was touched.

**Semantics.** The only CSF mode the twins support is `adm_csf_mode == 0`
(Watson-97); every other mode is rejected at init. In that mode the CPU
reference, `core/src/feature/adm_tools.c::adm_csf_rfactor_s`, sets
`rfactor = 1 / dwt_quant_step(...)` and never reads `adm_csf_scale` or
`adm_csf_diag_scale` — those two options are arguments of the *Barten* branch
(mode 1) only. All four twins multiplied them into every rfactor anyway:

```c
s->rfactor[scale * 3 + 0] = (float)s->adm_csf_scale / f1;   /* twin  */
factor1 = 1.0f / dwt_quant_step(...);                        /* CPU, mode 0 */
```

The CUDA comment beside it claimed this "matches the CPU Watson-mode path
where rfactor = scale * (1/quant_step)", which is the opposite of what
`adm_tools.c` does. Net effect: `--feature float_adm_cuda=adm_csf_scale=2.0`
doubled every h/v CSF coefficient on the GPU while the CPU ignored the option.

**Naming.** The CUDA, SYCL and HIP option tables declared the two options with
aliases `cs` / `cds` and `max = 100`, where the CPU `float_adm` (and Metal)
use `scf` / `scfd` and `max = 50`. ADR-1183 derives a feature's name from its
alias plus every non-default option's *alias* and value, so for one request the
CPU emitted `adm2_scf_2` and the GPU emitted `adm2_cs_2` — two different keys
for the same feature, which also breaks feature-name parity between backends.

## Decision

We will make the four twins compute the Watson-mode rfactor exactly as the CPU
does (`1 / f`, ignoring the two scale options), and align the CUDA/SYCL/HIP
option aliases and ranges with the CPU (`scf` / `scfd`, `max = 50`). The
options stay advertised: the CPU advertises them too and treats them as no-ops
in this mode, so a model that sets them still selects the twin and gets the
CPU's behaviour and the CPU's feature key.

Each backend's `float_adm` parity test gains a variant that sets
`adm_csf_scale=2.0, adm_csf_diag_scale=0.5` and reads the scores back under
the derived key `adm2_scfd_0.5_scf_2` (options sorted by name, `%g` values),
so both the arithmetic and the naming are gated.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Match the CPU: ignore the options in mode 0, align aliases (chosen) | Bit-for-bit the CPU contract; keeps the feature-name derivation identical; four small edits | The options become documented no-ops on the twins, as they already are on the CPU in this mode | — |
| Keep applying the scale on the GPU and change the CPU to match | Arguably a more useful option | Changes the CPU reference and every published float-ADM score for a non-default option; the CPU is the golden side | Rejected |
| Drop the two options from the twins' tables | Cannot be mis-applied | ADR-1183 would then route any model that sets them to the CPU, silently disabling the GPU path for an option the CPU itself ignores | Rejected |
| Implement the Barten branch on the twins so the options mean something | Feature-complete | A separate feature (Barten CSF port), not a parity fix; tracked under the CSF-mode work | Out of scope |

## Consequences

- **Positive**: with `adm_csf_scale=2.0` the CUDA, SYCL and HIP twins now
  report the same value as with the default (0.962085756 / 0.962090577 on the
  Netflix 576x324 pair, unchanged from their default-path values) under the
  same key as the CPU (`adm2_scf_2`). The new parity variants pass on an RTX
  4090, an Arc A380 and a gfx1030.
- **Negative**: anyone who relied on `cs=` / `cds=` in a model file for a GPU
  run gets an unknown-option error now; those aliases never matched the CPU.
- **Neutral / follow-ups**: Metal received the semantic fix but is unverified
  here (no Apple hardware); its aliases were already correct.

## References

- CPU reference: `core/src/feature/adm_tools.c::adm_csf_rfactor_s`,
  `core/src/feature/float_adm.c` option table.
- [ADR-1183](1183-model-options-gate-gpu-twin-selection.md) — option-honouring
  extractor selection and alias-derived feature names.
- Source: `req` — user direction to fix bugs found by the twin-drift sweep.
