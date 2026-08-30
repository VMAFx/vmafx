<!-- markdownlint-disable MD013 MD060 -->
# ADR-1104: Remove AVX-512 dispatch from float VIF convolution to restore Netflix golden scores

- **Status**: Accepted
- **Date**: 2026-06-13
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: `simd`, `correctness`, `float-vif`, `bug-fix`

## Context

ADR-0504 added AVX-512 dispatch to the three float VIF convolution wrappers
(`vif_filter1d_s`, `vif_filter1d_sq_s`, `vif_filter1d_xy_s`) in
`core/src/feature/vif_tools.c`, routing to
`convolution_f32_avx512_{s,sq_s,xy_s}` when `VMAF_X86_CPU_FLAG_AVX512` is
set at runtime.

ADR-0504 stated: "Netflix golden assertions still pass at their declared
`places` tolerance." This claim was incorrect on AVX-512 hardware.

The AVX-512 float convolution path uses a wider FMA partial-sum tree (16
floats per iteration vs 8 in AVX2). IEEE-754 FMA is not associative; the
wider reduction order yields different rounding, producing a `VMAFEXEC_score`
mean of approximately `76.66729` on the Netflix src01 pair instead of
`76.66740433333332` (the protected golden assertion in
`python/test/vmafexec_test.py` line 156). The absolute difference is
approximately `1.1e-4`, which exceeds the `places=4` threshold of `5e-5`.

The regression was latent from the day ADR-0504 landed: GitHub Actions runners
(Azure VMs) do not expose AVX-512, so CI always took the AVX2 code path and
the test appeared green. Developer machines with AVX-512 (e.g. Ice Lake, Zen 4)
failed locally. The failure surfaced during a full-matrix validation run that
exercised the AVX-512 path explicitly.

A prior fix (changelog entry `avx512-float-conv-width-alignment-emergency`)
guarded the dispatch behind a row-width alignment check (`w % 16 == 0`) to
prevent out-of-bounds reads on narrower rows, but did not address the
numerical divergence.

## Decision

Remove the `#if HAVE_AVX512` dispatch blocks from `vif_filter1d_s`,
`vif_filter1d_sq_s`, and `vif_filter1d_xy_s` in `core/src/feature/vif_tools.c`.
The float VIF path will dispatch to AVX2 (unchanged from upstream Netflix/vmaf)
or scalar.

The `convolution_f32_avx512_s/sq_s/xy_s` implementations in
`convolution_avx512.c` are retained (they are still used by other callers and
may be re-enabled for non-golden paths in the future), but are no longer called
from `vif_tools.c`.

This restores full golden-score parity:

- Float model (`vmaf_float_v0.6.1.json`): `76.66744` — diff `3.6e-5` < `5e-5` (places=4 PASS)
- Integer model (`vmaf_v0.6.1.json`): `76.66783` — diff `5.8e-7` < `5e-5` (places=4 PASS)

Full test run: **271 passed, 12 skipped, 0 failed** (vmafexec_test,
vmafexec_feature_extractor_test, quality_runner_test, feature_extractor_test,
result_test, ssimulacra2_test).

The integer VIF AVX-512 path (`vif_avx512.c`) is unaffected — it is a
separate code path with its own integer arithmetic and is covered by the
explicitly accepted divergence in ADR-0214.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Weaken the golden assertion (increase `places`) | CI would pass | Violates the project's core rule: golden assertions are immutable; "Fix the code, not the assertion" | Not an option |
| Keep AVX-512 dispatch and round-correct the wider accumulator | Preserves AVX-512 performance | Matching exact Netflix AVX2 bit pattern via 512-bit arithmetic is not practically achievable without emulation; no performance benefit for correctness | Not feasible |
| Guard dispatch behind `enable_avx512=false` build flag only | Easy to toggle | Doesn't help on default builds on AVX-512 machines; same correctness problem | Not sufficient |
| Use compensated summation (Kahan) in AVX-512 path | Numerically stable | Adds complexity and latency; correctness target is Netflix golden, not generic precision; AVX2 path already matches | Not needed |

## Consequences

- **Positive**: Netflix golden assertions pass on all hardware (AVX-512 and non-AVX-512) at `places=4`.
- **Neutral**: Float VIF throughput on AVX-512 CPUs reverts to the AVX2 level (the level that existed before ADR-0504). This is the upstream Netflix/vmaf performance baseline.
- **Negative**: The AVX-512 throughput gain from ADR-0504 is lost for the float VIF path (~40-50 % on AVX-512 CPUs per ADR-0504 profile). Re-enabling it would require a different numerical correction strategy.
- **Follow-up**: If AVX-512 float VIF performance is desired in the future, an ADR must explicitly document the accepted deviation from the Netflix golden and add a CI gate (e.g. a separate snapshot JSON for AVX-512) rather than relying on the integer-path ADR-0214 acceptance.

## References

- ADR-0504 (`0504-float-convolution-avx512-port.md`) — the decision this corrects.
- Changelog entry `avx512-float-conv-width-alignment-emergency` — row-alignment guard (unrelated to numerical divergence; the alignment guard remains in convolution_avx512.c for other callers).
- ADR-0214 — precedent for accepted ULP divergence on the *integer* VIF path; does NOT cover float VIF.
- `python/test/vmafexec_test.py` line 156 — protected Netflix golden assertion (`76.66740433333332`, `places=4`).
