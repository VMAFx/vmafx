<!-- markdownlint-disable MD013 MD041 MD060 -->

# ADR-1217: The GPU float-VIF kernels read `vif_sigma_nsq` and `vif_enhn_gain_limit` from their options

- **Status**: Proposed
- **Date**: 2026-09-07
- **Deciders**: Lusoris
- **Tags**: `cuda`, `sycl`, `hip`, `correctness`, `feature-extractor`, `testing`

## Context

`float_vif` exposes `vif_sigma_nsq` (alias `snsq`, neural noise variance,
default `2.0`, range `0.0–5.0`) and `vif_enhn_gain_limit` (alias `egl`,
enhancement-gain clamp, default `100.0`, range `1.0–100.0`) as
`VMAF_OPT_FLAG_FEATURE_PARAM` options. The CPU threads both through
`vif_statistic_s()` into the per-pixel statistic in
[`core/src/feature/vif_tools.c`](../../core/src/feature/vif_tools.c).

The CUDA, SYCL and HIP twins all declare both options in their option tables —
with the same names, aliases, defaults and ranges as the CPU — but their compute
kernels hardcoded the default values as local constants:

```c
const float vif_sigma_nsq = 2.0f;
const float vif_egl       = 100.0f;
const float sigma_max_inv = (2.0f * 2.0f) / (255.0f * 255.0f);
```

The host never forwarded the option values, and `init()` on all three validates
only `vif_kernelscale`. A non-default value is therefore accepted, range-checked,
and folded into the derived feature name (ADR-1183) — and then discarded.

This is reachable from a **shipped model**.
[`model/vmaf_float_v0.6.1neg.json`](../../model/vmaf_float_v0.6.1neg.json) sets
`"vif_enhn_gain_limit": 1.0` on all four VIF-scale features, which is precisely
what makes it the "NEG" (no-enhancement-gain) model. On the CPU that clamps
`g` to `1.0`; on any GPU backend the kernel clamped to `100.0` instead. The
resulting non-NEG scores were published under the NEG feature keys, so the
divergence was invisible in the output schema.

`float_vif_hip` had no parity test at all — `test_hip_vif_parity.c` covers the
*integer* `vif_hip` twin — and the CUDA and SYCL float-VIF parity tests both ran
with `NULL` options, where the hardcoded constants *are* the correct values.

## Decision

We will pass `vif_sigma_nsq`, `vif_enhn_gain_limit` and the derived
`sigma_max_inv` into the CUDA, SYCL and HIP float-VIF compute kernels as
arguments, computed on the host from the extractor state, and delete the
hardcoded constants. `sigma_max_inv` is derived exactly as the CPU derives it in
`vif_statistic_s()` — `powf(nsq, 2.0f)` evaluated in `float`, divided by
`255.0 * 255.0` in `double`, narrowed to `float` — so the default path stays
bit-identical. Each backend's float-VIF parity test gains a variant that pins
the NEG options and asserts parity on the derived key; HIP gets its first
float-VIF parity test.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Pass the values into the kernels (chosen) | The advertised surface starts working on GPU; the NEG model produces NEG scores on every backend; default path unchanged | Three more kernel arguments per launch | — |
| Reject non-default values in `init()` with `-EINVAL` | Small, honest, no numerical risk | Turns a silent wrong answer into a hard failure for the shipped NEG model on every GPU backend — the model would stop loading rather than start being right | Removes a working user surface to avoid implementing it |
| Drop the two options from the GPU option tables | Makes the twins' declared surface match their behaviour | The derived feature name would then differ from the CPU's for the same model, so model lookup would miss; and it deletes a documented option | Breaks ADR-1183 name derivation and removes a surface |
| Recompute `sigma_max_inv` in the kernel from `vif_sigma_nsq` | One fewer argument | `powf` in device code is not guaranteed to round identically to the host `powf`, risking a default-path drift for no benefit | Needless bit-exactness risk |

## Consequences

- **Positive**: `vmaf_float_v0.6.1neg.json` now produces NEG scores on CUDA,
  SYCL and HIP, not just on CPU. `--feature float_vif_<backend>:snsq=<x>` and
  `:egl=<x>` do what they say.
- **Negative**: any GPU run of the NEG model recorded before this change is
  invalid and must be re-scored. No in-tree snapshot or golden fixture covers a
  GPU NEG run, so nothing in the repo needs regenerating.
- **Neutral / follow-ups**: the default path is unchanged — the kernel arguments
  carry exactly the constants that were inlined before, and the CUDA / SYCL
  default-options parity tests stay green. The Metal float-VIF twin was checked
  and already threads both options through; it needs no change.

## References

- Findings 135 / 136 / 137 from the twin-drift sweep: the CUDA, HIP and SYCL
  float-VIF kernels hardcode `vif_sigma_nsq = 2.0f` and
  `vif_enhn_gain_limit = 100.0f` even though both are advertised as settable
  options with non-degenerate ranges; the host never forwards them and `init()`
  never rejects non-default values.
- [`model/vmaf_float_v0.6.1neg.json`](../../model/vmaf_float_v0.6.1neg.json) —
  sets `"vif_enhn_gain_limit": 1.0` on all four VIF-scale features.
- [ADR-0214](0214-gpu-parity-ci-gate.md) — the GPU parity CI gate and its
  places=4 cross-backend tolerance.
- [ADR-1183](1183-model-options-gate-gpu-twin-selection.md) — the derived
  feature-name key (`vif_scale0_egl_1_snsq_1.5`) the parity variants read.
- ADR-1216 and research digest 2033 (both land in PR #1375, one feature
  earlier) — the same default-options blind spot: an option whose default makes
  the CPU and GPU paths coincide is not covered by a default-options parity
  test. Referenced by number rather than by link because this branch does not
  carry those files.
