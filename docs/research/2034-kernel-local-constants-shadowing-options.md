<!-- markdownlint-disable MD013 -->

# 2034 — Kernel-local constants that shadow an option name defeat both review and the type system

**Date**: 2026-09-07
**Scope**: `vif_sigma_nsq` / `vif_enhn_gain_limit` in the CUDA, SYCL and HIP
`float_vif` compute kernels.
**Outcome**: all three fixed to take the values as kernel arguments
([ADR-1217](../adr/1217-gpu-float-vif-options-reach-kernel.md)); `float_vif_hip`
gained its first parity test.

## The defect

Each of the three GPU float-VIF compute kernels contained, inside the per-pixel
block:

```c
const float eps = 1.0e-10f;
const float vif_sigma_nsq = 2.0f;
const float vif_egl = 100.0f;
const float sigma_max_inv = (2.0f * 2.0f) / (255.0f * 255.0f);
```

Every use below that point — `g = fminf(g, vif_egl)`,
`log2f(1.0f + (g * g * sigma1_sq) / (sv_sq + vif_sigma_nsq))`,
`if (sigma1_sq < vif_sigma_nsq)` — is a *correct* transcription of the CPU
reference in `vif_tools.c`. The arithmetic is right. The inputs are not: both
names are `VMAF_OPT_FLAG_FEATURE_PARAM` options that the extractor declares,
range-checks, and folds into the derived feature name, and neither host ever
forwarded the value.

## Why this is worse than an ordinary hardcoded constant

Three properties compound:

1. **The local shadows the option, and carries its exact name.** Reading the
   kernel body, `vif_sigma_nsq` looks like the option. Nothing at the use site
   distinguishes "the option" from "a local that happens to be named after the
   option". A reviewer checking the *formula* against the CPU finds a perfect
   match.
2. **The type system cannot help.** A missing kernel *argument* is a compile
   error. A kernel-local constant compiles, links, and runs. There is no
   diagnostic anywhere in the chain.
3. **The option still changes the output key.** Because both are feature params,
   setting one changes the derived feature name (ADR-1183) — so the score
   *appears* under `vif_scale0_egl_1`, advertising a setting that was discarded.
   The output schema actively asserts the wrong thing.

## Reachability: a shipped model, not a hypothetical flag

`model/vmaf_float_v0.6.1neg.json` sets `"vif_enhn_gain_limit": 1.0` on all four
VIF-scale features. That setting *is* what makes it the NEG (no-enhancement-gain)
model. On the CPU it clamps the gain ratio to `1.0`; on CUDA, SYCL and HIP the
kernel clamped to `100.0`, i.e. did not clamp at all. Every GPU run of the NEG
model returned ordinary enhancement-gain-enabled VIF, filed under NEG keys.

Measured on the local hardware at `vif_enhn_gain_limit = 1.0`,
`vif_sigma_nsq = 1.5`, against the `1e-4` ADR-0214 gate:

| Backend | CPU | GPU | delta |
| --- | --- | --- | --- |
| CUDA (RTX 4090), scale 0, 256x144 ramp fixture | `0.22693315` | `0.24385797` | `1.69e-02` |
| HIP (gfx1030), scale 0, 256x144 skewed-ramp fixture | `0.56285676` | `0.55776394` | `5.09e-03` |
| SYCL (Arc A380), scale 0, 256x144 fixture | `0.81855110` | `0.81771439` | `8.37e-04` |

The three rows use different fixtures, so the magnitudes are not comparable to
each other — only to the gate, which all three exceed by roughly one to two
orders of magnitude.

## Why no test caught it

- `test_cuda_float_vif_parity.c` and `test_sycl_float_vif_parity.c` both ran
  `vmaf_use_feature(..., NULL)`. With default options the hardcoded constants
  *are* the correct values, so CPU and GPU agreed. Same shape as
  [digest 2033](2033-identity-default-option-blind-spot.md).
- `float_vif_hip` had **no** parity test. `test_hip_vif_parity.c` exists and
  looks like coverage, but targets the *integer* `vif_hip` twin. A name one word
  away from the thing it does not test.

## The generalisable rules

1. **A GPU kernel must not declare a local constant whose name matches an
   option.** Take the value as an argument. The compiler then enforces that every
   launch site supplies it, and a reader sees the plumbing.
2. **Derive option-dependent constants on the host, mirroring the CPU
   expression.** `sigma_max_inv` is
   `powf(vif_sigma_nsq, 2.0f) / (255.0 * 255.0)` — `powf` in `float`, the
   division in `double`, narrowed on assignment. Recomputing it in device code
   would risk a default-path rounding drift for no benefit.
3. **`cuLaunchKernel` / `hipModuleLaunchKernel` fail silently in both
   directions.** Surplus `kernelParams` entries are ignored; missing ones read
   uninitialised memory. Signature and `args[]` must change together, at *every*
   launch site — `float_vif_cuda.c` has two for the same kernel.
4. **Check that the parity test named after a twin actually targets it.**
   `vif_hip` vs `float_vif_hip` cost this defect its detection.

## Remaining candidates

Other GPU twins that declare an option and may not thread it through — each
worth the same one-variant test:

- `adm_p_norm` on the four `float_adm` twins: kernels hard-code the cube sum and
  the `1/3` DLM pooling exponent, honouring the option only for the AIM term.
- `adm_bypass_cm` on the CUDA and Metal `float_adm` twins: declared, stored in
  the state struct, read by nothing.
- `adm_skip_scale0` on the Metal `float_adm` twin: zeroes the reported scale-0
  sub-score but still folds scale 0 into the pooled `adm2`.

The mechanical check is one grep per option name per twin: if the only hits are
the struct field and the option-table entry, the option is advertised and
unimplemented.
