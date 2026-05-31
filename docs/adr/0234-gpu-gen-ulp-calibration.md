<!-- markdownlint-disable MD060 -->
# ADR-0234: GPU-generation-aware ULP calibration head

- **Status**: Accepted (2026-05-03 — calibration-table tier landed; ONNX-head
  tier remains scoped per "Decision" §)
- **Date**: 2026-05-01 (proposed) / 2026-05-03 (accepted)
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: ai, gpu, vulkan, cuda, sycl, cross-backend, fork-local, t7-gpu-ulp-cal

## Context

The fork ships four backends (CPU, CUDA, SYCL, Vulkan) and gates them
through [ADR-0214](0214-gpu-parity-ci-gate.md)'s
[`scripts/ci/cross_backend_parity_gate.py`](../../scripts/ci/cross_backend_parity_gate.py).
The matrix gate confirms that every GPU path stays *within* the per-feature
absolute tolerance (`5e-5` for the integer pipeline, `5e-4` /
`5e-3` for transcendentals / DCT-heavy features) but never claims
bit-exactness. The CPU is the canonical reference; the GPU paths
diverge by ~1e-4 on float metrics — small, deterministic, and a
direct consequence of fp32-only kernel arithmetic on architectures
without native fp64 (see [ADR-0220](0220-sycl-fp64-fallback.md)).

Two facts matter for this ADR:

1. The divergence is *deterministic per (feature, kernel, GPU
   architecture)* — not random noise. Re-running the same
   `(ref, dist)` pair on the same Vulkan device gives the same
   ~1e-4 delta versus the CPU run.
2. The cross-backend parity gate already emits a per-cell JSON
   record (`per_metric_max_abs_diff`, `per_metric_mismatches`,
   `tolerance_abs`, `n_frames`). That record, run over enough
   frames and broken down per-frame, *is* a training corpus
   for a calibration model.

The hypothesis: a tiny per-architecture calibration head
`(arch_id, raw_gpu_score) → cpu_equivalent_score` could close the
gap to bit-exact for the user-visible scores, *deterministically per
arch*, without touching the kernel arithmetic. This would not replace
the parity gate — it would compose with it: the gate keeps watch over
raw GPU scores, the calibration head transforms them into
CPU-equivalent values for callers that want bit-exact reproducibility
across hardware (e.g. Netflix-golden-replay on a non-Netflix GPU
runner).

This is exploratory. The rest of this fork's GPU strategy
(ADR-0220, ADR-0214) is "fix it at the kernel" or "accept the
known floor"; calibration is a third axis worth scoping but not
yet committing to.

## Decision

We will scope a per-architecture ONNX calibration head
`(arch_id_one_hot, raw_gpu_score) → cpu_score` as a *proposal* and
ship the data-collection plumbing first. The head will live behind a
new CLI flag `--gpu-calibrated` (default **off**) and will be wired
into the existing tiny-AI surface (loadable via the
[`--tiny-model`](../usage/cli.md#tiny-ai-flags-fork-added) machinery
or a dedicated registry entry, decision deferred to the
implementation ADR). The flag will not flip default-on until a
measurement-driven follow-up ADR cites:

- per-arch held-out PLCC ≥ 0.9999 against the CPU score on the
  Netflix golden fixtures, **and**
- a worst-case per-frame absolute residual of ≤ 1e-6 on the
  calibrated path (i.e. tighter than the current `5e-5` parity
  tolerance, otherwise calibration buys nothing).

The proposal-stage scope is exactly: ADR-0234 (this file),
[Research-0041](../research/0041-gpu-gen-ulp-calibration.md)
(corpus design), and
[`ai/scripts/collect_gpu_calibration_data.py`](../../ai/scripts/collect_gpu_calibration_data.py)
(data-collection script with a `--smoke` mode wired to lavapipe so
hosted CI can verify the pipeline end-to-end). Training, the ONNX
artefact, and the `--gpu-calibrated` CLI surface are deferred to a
follow-up PR (`feat(ai): T7-GPU-ULP-CAL — calibration-head v0`)
gated by the data-collection script producing a real corpus on at
least one real GPU.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **(a) Per-arch calibration head (this proposal)** | Deterministic per-arch correction; doesn't touch kernel arithmetic; composes with the parity gate; small ONNX (probably < 50 KiB per arch); user opts in via `--gpu-calibrated`; reuses the tiny-AI runtime already on disk; fits the ADR-0214 "verify, don't modify" philosophy | Adds a new CLI surface, a per-arch training corpus to maintain, and a registry entry with `smoke: true` until measured; requires a per-arch detection mechanism that works for Vulkan/CUDA/SYCL/HIP; calibration only helps callers that opt in | Picked because it's the only option that closes the residual without re-litigating ADR-0220's fp64-free contract |
| (b) Fix the divergence at the kernel level (matches ADR-0220's approach for SYCL fp64) | Truly closes the gap at the source; no calibration corpus, no new CLI surface, no model registry entry; per ADR-0220 the int64 Q31 path is already the right answer for ADM gain limiting | Requires bit-exact fp32 reductions across every feature × backend cell — a multi-quarter project; some GPUs (Arc A380 et al.) lack the fp64 fallback that would make this trivial; the residual is *already* below the production tolerance so the ROI on a kernel-level fix is hard to justify against the rebase/maintenance cost; doesn't generalise to future ULP-sensitive consumers (HIP, Metal) | Not chosen *yet* — kernel-level fixes happen on a per-feature, per-backend basis as their own follow-ups (ADR-0220 was one such); the calibration-head approach is orthogonal and ships in weeks rather than quarters |
| (c) Accept the divergence and do nothing | Zero diff; the parity gate already proves we're within tolerance | Some downstream callers (golden-replay validators, regulatory comparisons) want bit-exactness, not "within tolerance"; the ADR-0220 / ADR-0214 audit explicitly named ULP divergence as a known floor — leaving it untreated forecloses future use cases that need bit-exact CPU equivalence on a GPU runner | Default on the table but rejected because the proposal in (a) is cheap to scope and the corpus collection is a prerequisite for *any* future treatment, including (b) |
| (d) Fixed per-feature scalar offset (no model, just a constant per (feature, arch) cell) | Tiny — a JSON table; no ONNX runtime dependency; explicit; auditable | The residual is not a constant — it varies per (frame, score-magnitude); a scalar offset would over-correct in some regimes and under-correct in others; would not pass the `1e-6` worst-case bar | Considered as a fallback if the corpus shows the residual really is constant per cell — would obviate the ONNX head entirely. Documented here so future work knows to check |

## Consequences

- **Positive**: opens an option for bit-exact GPU↔CPU reproducibility
  without touching kernel arithmetic; gives the fork a per-arch
  treatment of the residual that ADR-0214 currently only *reports*;
  reuses the tiny-AI runtime; the data-collection script is
  immediately useful as a richer instrumentation of the parity gate
  even if calibration never ships (it produces per-frame ULP
  histograms that the gate's max-abs-diff alone hides).
- **Negative**: a new CLI flag (`--gpu-calibrated`); a per-arch
  training corpus to keep current as kernels evolve (every kernel
  change invalidates the corresponding calibration head); a model
  registry entry that ships as `smoke: true` until measured;
  arch-detection plumbing that has to work across CUDA, SYCL,
  Vulkan, and HIP (the latter not yet shipped — see ADR for HIP
  scaffold pending). Per CLAUDE.md §12 r1 the Netflix golden
  assertions remain untouchable; calibration can only run on the
  GPU side of the pair, never on the CPU reference.
- **Neutral / follow-ups**:
  - Research-0041 must converge on a corpus design before any
    training PR.
  - The implementation PR will need its own ADR ("ADR-NNNN: GPU
    calibration head v0 implementation") with the held-out PLCC /
    residual measurements and the `--gpu-calibrated` CLI contract.
  - HIP is currently un-scaffolded (T7-10); this proposal does
    *not* block on HIP — Vulkan + lavapipe is enough to prove the
    pipeline.
  - If the data-collection script ships and the corpus shows the
    residual is effectively constant per cell, alternative (d)
    supersedes this proposal and we drop the ONNX in favour of a
    JSON offset table. That would be a *good* outcome.

## References

- Source: `req` — user-direction: scaffold a GPU-generation-aware
  ULP calibration head as a proposal-stage PR (paraphrased).
- [ADR-0214](0214-gpu-parity-ci-gate.md) — GPU-parity CI gate.
  Source of training data; shape of the per-cell JSON record.
- [ADR-0220](0220-sycl-fp64-fallback.md) — SYCL fp64-free contract;
  documents one cause of the divergence and the alternative-(b)
  approach this proposal is orthogonal to.
- [ADR-0042](0042-tinyai-docs-required-per-pr.md) — tiny-AI
  per-PR doc bar; calibration head will inherit it on the
  implementation PR.
- [Research-0041](../research/0041-gpu-gen-ulp-calibration.md) —
  corpus design, arch-detection mechanisms, training matrix size
  estimate, smallest viable training set.
- Companion code: [`ai/scripts/collect_gpu_calibration_data.py`](../../ai/scripts/collect_gpu_calibration_data.py).
- Related PRs: this ADR's PR.

## Implementation status (2026-05-03)

The calibration-table tier of this ADR shipped under the PR
"feat(ci): T-GPU-ULP — per-GPU-gen calibration table". That PR adds
[`scripts/ci/gpu_ulp_calibration.yaml`](../../scripts/ci/gpu_ulp_calibration.yaml)
and wires both
[`scripts/ci/cross_backend_vif_diff.py`](../../scripts/ci/cross_backend_vif_diff.py)
and
[`scripts/ci/cross_backend_parity_gate.py`](../../scripts/ci/cross_backend_parity_gate.py)
to a per-GPU-id tolerance lookup. Coverage on land:

- 1 calibrated row (Mesa lavapipe — hosted-CI baseline; tolerances
  match the gate's pre-existing `FEATURE_TOLERANCE` defaults so
  behaviour is unchanged).
- 11 placeholder rows (NVIDIA Ampere/Turing/Ada/Hopper, AMD
  RDNA2/RDNA3, Intel Arc Alchemist/Battlemage, generic Intel SYCL).
  Placeholder rows fall back to the per-feature default until a
  real-hardware corpus replaces their `features:` block.

The `--gpu-calibrated` CLI flag and the ONNX calibration head
described in §Decision remain deferred to the follow-up PR
`feat(ai): T7-GPU-ULP-CAL — calibration-head v0`, gated on the
data-collection script producing a real corpus on at least one
real GPU. The table tier is the prerequisite plumbing — it
establishes the per-arch identification surface and the
threshold-lookup contract that the head will reuse.
