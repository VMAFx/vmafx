# ADR-0345: cambi × {CUDA, SYCL, HIP} GPU port strategy

- **Status**: Accepted
- **Status update 2026-05-15**: CUDA + SYCL ports implemented;
  `core/src/feature/cuda/integer_cambi_cuda.c` and
  `core/src/feature/sycl/integer_cambi_sycl.cpp` present on
  master; cross-backend gate at places=4 active. HIP port deferred
  to a separate follow-up task.
- **Date**: 2026-05-09
- **Deciders**: <lusoris@pm.me>, Claude (Anthropic)
- **Tags**: cuda, sycl, hip, gpu, cambi, fork-local, places-4

## Context

The CAMBI banding metric ships on CPU (scalar + AVX2 + AVX-512 + NEON)
and on Vulkan (per [ADR-0210](0210-cambi-vulkan-integration.md), shipped
as PR #196 / T7-36). It does **not** yet ship on CUDA, SYCL, or HIP —
those three backends were explicitly deferred from
[ADR-0192](0192-gpu-long-tail-batch-3.md)'s long-tail batch 3 ("follow
per-backend cadence after the Vulkan terminus lands") and again from
[Research-0090](../research/0090-t3-15-gpu-coverage-long-tail-2026-05-09.md)
§Ordering rationale ("`cambi` × {CUDA, SYCL} last. Highest risk by 5×.
... Defer to a separate planning round once chroma is in").

This ADR is that planning round's decision. It does not implement the
ports. The implementing PRs (one per backend) follow per the
[Research-0091](../research/0091-cambi-gpu-port-planning-2026-05-09.md)
ordered plan and each carry their own narrow per-port ADR if a new
architectural decision arises at port time.

The structural blocker — `calculate_c_values`'s sliding 1024-bin
histogram per output column with sequential row-major update — is
algorithmic, not hardware-specific. It pessimises CUDA / SYCL / HIP
the same way it pessimises Vulkan ([ADR-0205](0205-cambi-gpu-feasibility.md)
§Decision). The host-staged Strategy II Vulkan precedent therefore
generalises directly.

## Decision

We will port cambi to CUDA, SYCL, and HIP using **Strategy II
host-staged hybrid** — the same architecture
[ADR-0205](0205-cambi-gpu-feasibility.md) chose and
[ADR-0210](0210-cambi-vulkan-integration.md) shipped on Vulkan.

For each of the three backends:

1. The GPU services every embarrassingly-parallel pre-pass:
   `cambi_preprocess`, `cambi_derivative`, the spatial-mask SAT
   (row-prefix + col-prefix + threshold compare), `cambi_decimate`,
   and the separable 3-tap mode filter.
2. The `calculate_c_values` sliding-histogram pass and the top-K
   spatial pooling stay on the host, executing the unmodified CPU
   code (`vmaf_cambi_calculate_c_values` / `vmaf_cambi_spatial_pooling`
   from `cambi_internal.h`) against GPU-produced byte-identical
   buffers.
3. The cross-backend gate runs at `places=4` from day one — same
   contract as every other GPU twin in the fork. Per ADR-0205
   §Precision contract this is by-construction (all GPU pre-passes
   are integer-only; readback is byte-identical; host residual runs
   unmodified CPU code).

Implementation ordering: **CUDA first, SYCL second, HIP third**, per
Research-0091 §6.

LOC envelopes (anchored against in-tree comparables — Vulkan cambi
1916 LOC; CUDA `integer_adm` 2700 LOC; SYCL `integer_adm` 1663 LOC;
HIP `integer_psnr` ≈600 LOC):

- CUDA ≈ **1100 LOC**, risk **LOW**.
- SYCL ≈ **1300 LOC**, risk **MEDIUM** (single-TU style + dual
  toolchain matrix per ADR-0335).
- HIP ≈ **1100 LOC**, risk **MEDIUM-LOW** (most-complex HIP consumer
  to date; structural mirror of CUDA via hipify-perl).

Each per-port PR ships its own ADR-0108 six-deliverables set. The
per-port ADR cites this ADR + Research-0091 as design parents; if no
new architectural decision arises at port time, the per-port ADR's
`## Alternatives considered` is "no alternatives — Strategy II
inherited from ADR-0345".

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| **Strategy II hybrid for all three (chosen)** | Inherits ADR-0205 / ADR-0210 precedent verbatim; bit-exact w.r.t. CPU; closes the GPU coverage matrix; modest LOC; reuses `cambi_internal.h` host residual that already shipped with the Vulkan port. | C-values phase doesn't accelerate on any of the three backends; PCIe / HSA shuttle per scale (≤50 MiB at 4K). | **Selected** — same risk/reward calculation as ADR-0205, applied to three more backends with no new structural argument. |
| **Strategy III fully-on-GPU `calculate_c_values` for the new ports only** | Maximal GPU utilisation on the new ports; might justify the LOC investment if profile data on RDNA / Ada / Xe shows good cache behaviour at the per-pixel 65×65 scan. | Speculative — no profile data on per-output-pixel 4225-read cache-hit rate exists yet; would create three different cambi GPU architectures (Vulkan = II, CUDA/SYCL/HIP = III) and complicate the cross-backend gate; per ADR-0205 §Precision investigation Strategy III needs its own per-backend precision audit because the histogram-update order differs. | Premature optimisation. Strategy III remains parked per ADR-0205 §Out of scope as a focused v2 PR with its own ADR + profile data. Mixing strategies across backends would multiply the maintenance surface for marginal gain. |
| **Single ADR-0NNN per backend at decision time, no umbrella ADR** | Fine-grained ADR per port; each port ADR is self-contained. | Creates three near-identical ADRs that all repeat the Strategy II argument from ADR-0205; per ADR-0028 ADRs document non-trivial decisions and three repeats of the same decision are noise; per CLAUDE.md §12 r8 "non-trivial = another engineer could reasonably have chosen differently" — at this point a different engineer cannot reasonably have chosen Strategy III given the Vulkan precedent. | This umbrella ADR locks the strategy once; per-port ADRs become near-mechanical and only fire if a port-time issue forces a new decision. Mirrors the way ADR-0192 acted as the umbrella for the long-tail batch 3 metrics. |
| **Defer all three indefinitely** | Zero implementation risk for this round. | Leaves three permanent gaps in the GPU coverage matrix; CAMBI is a registered feature extractor that real users invoke (not an experimental metric); the Vulkan terminus is closed but the matrix is not. | Rejected — closing the matrix on the remaining three backends is the explicit Research-0090 §Ordering rationale follow-up. |
| **Different strategies per backend (II for CUDA/SYCL, III for HIP)** | Could exploit the fact HIP just landed (PR #499) to start clean with the more ambitious architecture. | No rationale supports the asymmetry; cache-hit-rate data is missing on RDNA the same as on Ada / Xe; HIP is the most-junior backend and adding novel-architecture risk on top of post-bring-up risk concentrates failure modes. | Rejected — uniform Strategy II minimises cross-backend variance and lets the existing cambi cross-backend gate cover all three with one line of `cross_backend_vif_diff.py` config. |

## Consequences

- **Positive**: closes the GPU coverage matrix on CAMBI for the three
  remaining backends. Combined with the Vulkan terminus from
  ADR-0210, every backend in the fork (CPU + Vulkan + CUDA + SYCL +
  HIP, plus the in-flight Metal scaffold from PR #509) has a path
  to a CAMBI twin.
- **Positive**: preserves the canonical `places=4` cross-backend gate
  on cambi for all three backends from day one — no per-backend
  tolerance carve-outs.
- **Positive**: the existing `cambi_internal.h` (shipped with PR #196
  for the Vulkan port) is reused as-is. The host residual surface is
  one shared header; the three new GPU twins each link against it.
- **Positive**: lowers the cost of any future Strategy III v2 PR.
  When profile data eventually lands, Strategy III can be applied
  to one backend at a time (likely starting with whichever vendor
  shows the best cache-hit data) without disturbing the other ports.
- **Negative**: cambi GPU performance in v1 is bottlenecked by the
  CPU c-values phase on all three backends. At 4K this remains
  ~250 ms/frame end-to-end. Real users who care about cambi
  throughput need to wait for v2 (Strategy III) or stick with CPU.
- **Negative**: adds three new maintenance surfaces that mirror the
  CPU `calculate_c_values` host residual. Mitigated by the shared
  `cambi_internal.h` — any CPU-side change to the c-value formula
  flows through to all four GPU twins (Vulkan + CUDA + SYCL + HIP)
  via the header.
- **Neutral / follow-ups**:
  1. Per-backend port PR cadence per Research-0091 §6: CUDA → SYCL
     → HIP, one PR each.
  2. Each port PR registers a `cambi_<backend>` row in
     `scripts/ci/cross_backend_vif_diff.py::FEATURE_METRICS` and
     adds a smoke-fixture lane to the relevant CI workflow.
  3. SYCL port runs through both oneAPI and AdaptiveCpp toolchains
     per ADR-0335 — the SYCL port PR adds smoke fixtures for both,
     not just oneAPI (Research-0091 §9).
  4. HIP port is seeded from the CUDA port via `hipify-perl`; the
     HIP port PR documents the deltas inline, no new architectural
     decision expected.
  5. v2 Strategy III ADR (likely `ADR-0NNN-cambi-strategy-iii`) is a
     focused follow-up with its own profile data and per-backend
     precision audit. Out of scope for this round.

## Precision investigation

Not applicable for this planning round — the precision argument is
inherited verbatim from
[ADR-0205 §Precision contract](0205-cambi-gpu-feasibility.md#precision-contract-places4-tightened-from-adr-0192s-places2)
and [ADR-0210 §Precision investigation](0210-cambi-vulkan-integration.md#precision-investigation):

- All Strategy II GPU pre-passes are integer arithmetic (no float
  rounding).
- Readback into the per-scale `(image, mask)` `VmafPicture` pair is
  byte-identical to what the CPU code path would have written
  in-place.
- The host residual runs the unmodified CPU c-values code on those
  buffers; output is bit-identical.

Therefore each per-port PR is expected to land at ULP=0 /
max_abs_diff=0 against the CPU on the smoke fixture, comfortably
under `places=4`. Per memory `feedback_no_test_weakening`: if a
port misses `places=4` empirically, fix the kernel — never relax
the gate.

The one place where empirical confirmation is genuinely required
is the SYCL port's AdaptiveCpp lane (Research-0091 §9), because
AdaptiveCpp's `sycl::group_reduce` lowering on AMD differs from
oneAPI's. The SYCL port PR carries that empirical step.

## References

- Companion digest:
  [Research-0091](../research/0091-cambi-gpu-port-planning-2026-05-09.md)
  — full per-backend Strategy / LOC / risk / ordering analysis.
- Predecessor ADR:
  [ADR-0205](0205-cambi-gpu-feasibility.md) — feasibility spike,
  Strategy II decision for Vulkan.
- Predecessor ADR:
  [ADR-0210](0210-cambi-vulkan-integration.md) — Vulkan v1
  integration that landed as PR #196 / T7-36.
- Predecessor digest:
  [Research-0020](../research/0020-cambi-gpu-strategies.md) —
  cross-strategy comparison; bandwidth analysis; literature survey.
- Predecessor digest:
  [Research-0032](../research/0032-cambi-vulkan-integration.md) —
  Vulkan integration-time trade-offs.
- Predecessor digest:
  [Research-0090](../research/0090-t3-15-gpu-coverage-long-tail-2026-05-09.md)
  §Ordering rationale — deferred this round.
- Parent ADR:
  [ADR-0192](0192-gpu-long-tail-batch-3.md) — GPU long-tail batch 3
  scope; explicitly conditioned cambi × {CUDA, SYCL, HIP} on a
  separate planning round.
- Sibling ADR:
  [ADR-0212](0212-hip-backend-scaffold.md) — HIP backend scaffold.
- Sibling ADR:
  [ADR-0241](0241-hip-first-consumer-psnr.md) — HIP consumer cadence.
- Sibling ADR:
  [ADR-0335](0335-sycl-adaptivecpp-second-toolchain.md) (in-flight,
  PR #498) — dual SYCL toolchain scope.
- Source: `req` — PR #520 §Ordering rationale, "Defer to a separate
  planning round once chroma is in — the cambi backlog is large
  enough to warrant its own ADR(s)."
- User direction: standing CLAUDE.md §12 r8 (ADRs document non-
  trivial decisions); §12 r10/r11 (every fork-local PR ships the six
  deep-dive deliverables; doc-substance rule applies); §12 r4 (`make
  lint` before push); memory `feedback_no_guessing` (LOC + risk
  estimates cite in-tree comparables); memory
  `feedback_no_test_weakening` (places=4 is non-negotiable).
