<!-- markdownlint-disable MD060 -->
# ADR-0214: GPU-parity CI gate (T6-8) — cross-device variance matrix

- **Status**: Accepted
- **Date**: 2026-04-29
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: ci, gpu, vulkan, cuda, sycl, agents, fork-local

## Context

The fork ships four backends today (CPU, CUDA, SYCL, Vulkan) and is
about to add HIP (T7-10). Every feature with a GPU twin is gated
today by `scripts/ci/cross_backend_vif_diff.py`, but the gate runs
*one cell per CLI invocation*: the `tests-and-quality-gates.yml`
`vulkan-vif-cross-backend` job calls the script ~17 times
(once per feature) and each call only diffs CPU↔Vulkan. There is
no gate on CPU↔CUDA, CPU↔SYCL, or pairwise GPU↔GPU consistency, no
machine-readable summary, and the per-feature tolerance is buried
inside a hand-written `--places` flag on each step (which drifts
silently when a feature's empirical floor changes).

Wave-1 roadmap §7 calls for a single matrix gate before Vulkan goes
to production. T6-8 is that gate.

## Decision

We add `scripts/ci/cross_backend_parity_gate.py` and a new CI job
`vulkan-parity-matrix-gate` in `tests-and-quality-gates.yml`. The
script iterates every (feature, backend-pair) cell, runs `vmaf`
once per backend, diffs per-frame metrics with a per-feature
absolute tolerance declared in a single `FEATURE_TOLERANCE` table,
and emits one JSON record + one Markdown row per cell. Default
tolerance is `5e-5` (places=4 — the existing fork contract from
ADR-0125 / ADR-0138 / ADR-0140); transcendental-heavy features
(`ciede`, `psnr_hvs`, `ssimulacra2`) keep their relaxed contracts;
a `--fp16-features` flag forces `1e-2` for the future tiny-AI lane
(T7-39 ONNX Runtime FP16). The hosted CI lane uses Mesa lavapipe
for Vulkan (no GPU runner needed); CUDA / SYCL / hardware-Vulkan
remain advisory on a self-hosted lane until that hardware is
plumbed in. The gate **never** modifies feature implementations —
it only verifies.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Per-feature tolerance table (chosen) | Honest about real precision floors (ciede places=2, psnr_hvs places=3, vif places=4); single source of truth | One Python dict to keep current with kernel changes | Aligns with measure-then-set-the-contract from ADR-0188/9; uniform places=4 would force fake-passing or hide the relaxation in a `--places` flag per CI step |
| Uniform places=4 across all cells | Simplest mental model | Already false (psnr_hvs has been places=3 since ADR-0191; ciede places=2 since ADR-0187); would need per-step overrides anyway | Hides the relaxation in CI YAML rather than in code; reviewers can't see the contract at a glance |
| CUDA / SYCL hard-required on every PR | Catches every regression immediately | Needs self-hosted runner; cost + flake risk on every PR | Lane is `if: false` until self-hosted runner with the right hardware exists; advisory-only until then |
| Lavapipe-only Vulkan in CI (chosen) | Free hosted runners, no GPU dependency, ~deterministic | No hardware drift coverage (Mesa anv, Intel Arc, NVIDIA proprietary) | Self-hosted Arc lane already exists for `vulkan-vif-arc-nightly`; T6-8 piggy-backs on it for hardware-Vulkan once enabled |
| JSON-only output | Less code to maintain | Reviewers reading PR comments can't see a status table | Markdown table is ~30 lines; reviewers comment on it directly. JSON for downstream tooling, MD for humans |

## Consequences

- **Positive**: one place declares the parity contract for every
  feature; CI surfaces a single PASS/FAIL plus a per-cell report
  instead of 17 separate green check-marks; new GPU backends (HIP)
  drop into the matrix by adding one entry to `BACKEND_SUFFIX`;
  CUDA / SYCL parity becomes a measured value the moment a runner
  exists (no code change).
- **Negative**: the per-feature `FEATURE_TOLERANCE` table can drift
  out of agreement with the kernel's empirical floor — bumping
  tolerance must come with a measurement-driven follow-up ADR per
  CLAUDE.md §12 r1.
- **Neutral / follow-ups**: the legacy
  `cross_backend_vif_diff.py` stays — sister PRs in this session
  still wire it as the per-feature smoke. The legacy lane is
  deleted in the T6-8b PR once the matrix gate has soaked one
  release cycle. Future `--fp16-features` consumers (T7-39 ONNX
  tiny-AI) inherit the gate without further changes.

## References

- `req` — backlog row T6-8 ("GPU-parity CI gate (CPU ↔ CUDA ↔ SYCL
  ↔ Vulkan cross-device variance). Wave 1 roadmap §7. Cross-device
  ULP gate with ≤1e-4 FP32 / ≤1e-2 FP16 tolerance. Blocks Vulkan →
  production once T5-1b lands.")
- ADR-0125 / ADR-0138 / ADR-0140 — places=4 cross-backend contract
  (places=4 ↔ 5e-5 absolute).
- ADR-0175 — Vulkan backend scaffold; the audit-first shape this
  PR mirrors.
- ADR-0176 — `cross_backend_vif_diff.py` rationale for places=4 +
  half-ULP threshold.
- ADR-0187 / ADR-0188 — relaxed places=2 contracts for ciede /
  ssimulacra2.
- ADR-0191 — psnr_hvs places=3 contract.
- Companion code: `scripts/ci/cross_backend_parity_gate.py`,
  `.github/workflows/tests-and-quality-gates.yml` lane
  `vulkan-parity-matrix-gate`,
  `docs/development/cross-backend-gate.md`.
