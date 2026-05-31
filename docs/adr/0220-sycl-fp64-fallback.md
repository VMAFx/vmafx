<!-- markdownlint-disable MD060 -->
# ADR-0220: SYCL feature kernels are unconditionally fp64-free

- **Status**: Accepted
- **Date**: 2026-04-29
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: sycl, perf, gpu, arc, intel, t7-17

## Context

Backlog item T7-17 reported that `--backend sycl` on Intel Arc A380
emitted

```text
SYCL: device lacks fp64 support — using int64 emulation for gain limiting
```

at init and then ran 5–10× slower than `--backend vulkan` on the same
hardware (roughly 510 fps vs 10650 fps for VIF on a 2400-frame fixture).
The wording suggested that VMAF's SYCL path had two code branches — a
native-fp64 fast path and an int64-emulation slow path — and that
fp64-less devices were stuck on the slow path.

Auditing the kernels showed the warning text was wrong:

- `core/src/feature/sycl/integer_adm_sycl.cpp` already implements gain
  limiting via the int64 Q31 split-multiply (`gain_limit_to_q31` +
  `launch_decouple_csf<false>`); the `<true>` (fp64) instantiation is
  never compiled. The comment block in that file explicitly cites the
  Intel Arc A-series rationale and warns that even a single `double`
  operand inside a sibling lambda would taint the whole SPIR-V module
  and crash the runtime on fp64-less devices.
- `core/src/feature/sycl/integer_vif_sycl.cpp` runs gain limiting
  entirely in fp32 (`sycl::fmin(g, vif_enhn_gain_limit)` over `float`
  operands). The launcher casts the host's `double
  vif_enhn_gain_limit` to `float` before kernel submission.
- `core/src/feature/sycl/integer_ciede_sycl.cpp` and
  `core/src/feature/sycl/integer_ssim_sycl.cpp` accumulate via
  `sycl::reduction<int64_t>` / `sycl::plus<int64_t>`; neither reaches for
  `sycl::reduction<double>`.
- The float-input extractors (`float_vif_sycl.cpp`,
  `float_adm_sycl.cpp`, `float_motion_sycl.cpp`) keep their kernel-side
  arithmetic in fp32; every `double` they reference lives strictly on
  the host (post-processing — score normalisation, log10, accumulators
  fed by `s->h_*` host buffers).

So the warning text mis-described the runtime behaviour. There is **no**
fp64-emulation fallback; the int64-only path is the only path. The 5–10×
Vulkan-vs-SYCL gap on Arc A380 has a different root cause (kernel
geometry, subgroup size, USM access pattern) and is tracked outside
T7-17.

## Decision

We will (a) reword the init log line to accurately describe the runtime
behaviour ("device lacks native fp64 — kernels already use fp32 + int64
paths, no emulation overhead", `VMAF_LOG_LEVEL_INFO`), (b) document the
fp64-free contract for SYCL feature kernels in
[`docs/backends/sycl/overview.md`](../backends/sycl/overview.md), and
(c) record `has_fp64` on `VmafSyclState` for future fp64-gated
optimisations without instantiating any fp64 kernel today. We will **not**
introduce a runtime device-aspect probe + dual-kernel dispatch for
gain-limiting, because there is no fp64 kernel to dispatch to.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep WARNING-level "int64 emulation" wording, ignore | Zero diff | Future maintainers chase a non-existent fast path; perf bug reports keep citing this line | The wording was actively misleading — it had to go |
| Add a runtime fp64-aspect probe and a `<UseFP64=true>` kernel instantiation for ADM gain limiting | Closes the door on the "is there a fast path we're missing?" question | The ADM kernel comment already explains why this is unsafe: a single `double` lambda capture taints the SPIR-V module for the whole TU and crashes the runtime on fp64-less devices, even when the fp64 kernel is never submitted. Building a parallel `<true>` TU per feature multiplies build time and binary size for a path no production gain value (1.0, 100.0) actually benefits from | Cost > benefit; the int64 Q31 path is exact for production gains and within ±1 LSB for fractional gains |
| Per-feature fp64 fallback (each extractor probes independently) | Granular control | Same SPIR-V-module-taint problem applies per-TU, not per-feature; doesn't actually unlock anything | Wrong axis of granularity |
| Build-time pin (compile two libvmaf flavours: `-fno-fp64-emulation` and a fp64-native build) | Forces the question at integrate time | Doubles release surface area, still doesn't enable any fp64 kernel that exists today | No customer asked for this; would create rebase-time confusion |
| Re-route the actual perf gap (Arc A380 kernel geometry / subgroup size) under T7-17 | Closes the user-visible perf complaint | Out of T7-17's narrowly-scoped fp64-emulation framing; needs its own backlog item with a reproducer + Vulkan-side baseline measurement | Deferred to a follow-up backlog row; T7-17 closes on the wording / contract fix |

## Consequences

- **Positive**: the init log no longer suggests a non-existent fast path.
  Future contributors who add a new SYCL kernel get a clear contract
  (no `double` in lambda captures, no `sycl::reduction<double>`) plus
  the SPIR-V-module-taint rationale for why the rule is hard, not soft.
- **Positive**: `VmafSyclState.has_fp64` remains queryable for any
  future fp64-gated optimisation (e.g. a CIEDE accumulator that prefers
  `double` on Data Center GPU Max).
- **Negative**: the Arc A380 5–10× perf gap vs Vulkan stays open. It
  was misattributed to fp64; the real root cause is a separate
  investigation (kernel geometry / sub-group size / memory pattern)
  outside T7-17's scope.
- **Neutral**: no API or ABI change. No CLI flag added. No new build
  option.

## References

- Source: `req` (T7-17 backlog row in `.workingdir2/BACKLOG.md`).
- ADR-0202 (float ADM CUDA + SYCL) — established the float-side fp32
  kernel pattern.
- ADR-0181 (feature-characteristics registry) — possible host for a
  future `requires_fp64` aspect field if a fp64-only optimisation ever
  lands.
- `core/src/feature/sycl/integer_adm_sycl.cpp` lines 460–520 — the
  gain-limit Q31 design comment.
- `core/src/feature/sycl/integer_ciede_sycl.cpp` lines 60–80 — the
  fp64-free accumulator commentary.
- Related issues / PRs: this ADR's PR.
