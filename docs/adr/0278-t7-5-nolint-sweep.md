<!-- markdownlint-disable MD060 -->
# ADR-0278: T7-5 NOLINT-sweep closeout — citation normalisation across libvmaf

- **Status**: Accepted
- **Date**: 2026-05-04
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: lint, cleanup, touched-file-rule, t7-5

## Context

[ADR-0141](0141-touched-file-cleanup-rule.md) §Historical debt scoped
**T7-5** as a one-time sweep of the pre-2026-04-21 baseline of ~18
`readability-function-size` NOLINTs plus the upstream `_iqa_*`
reserved-identifier suppressions. Three predecessor PRs progressively
chipped away at that backlog:

- **PR #82 / [ADR-0146](0146-nolint-sweep-function-size.md)** — Sweep A.
  Refactored 17 fork-touched infra files (dict, picture, picture pool,
  predict, read_json_model, feature_extractor, feature_collector,
  libvmaf, output) plus the three IQA / SIMD cores
  (`_iqa_convolve`, `_iqa_ssim`, `vif_statistic_s_avx2`). After this PR
  the `iqa/` directory was fully lint-clean and no
  `bugprone-reserved-identifier` / `cert-dcl37-c` suppressions on
  `_iqa_*` symbols remain in `core/src/`.
- **PR #293** — Sweeps B+C. Cite-only pass that added inline ADR-0138 /
  ADR-0139 / ADR-0141 references to 30 NOLINTs in fork-added SIMD
  paths (`x86/ssimulacra2_avx2.c`, `x86/ssimulacra2_avx512.c`,
  `arm64/ssimulacra2_neon.c`, `arm64/ssimulacra2_sve2.c`,
  `ssimulacra2.c`, the SIMD bit-exactness test).
- **PR #327** — Follow-up. Refactored the last two pre-2026-04-21
  bare NOLINTs in `float_adm.c::extract` and `tools/vmaf.c::main`
  (eight helpers extracted from `main`, debug-feature appends
  extracted from `float_adm.c`).

Between those landings and 2026-05-04, new NOLINT sites accreted in
files added after the original cutoff: the CUDA / Vulkan SS2
back-ends (`cuda/ssimulacra2_cuda.c`, `vulkan/ssimulacra2_vulkan.c`,
`vulkan/cambi_vulkan.c`), the Vulkan host-path SIMD helpers
(`arm64/ssimulacra2_host_neon.c`, `x86/ssimulacra2_host_avx2.c`),
the ARM64 / x86 `psnr_hvs` SIMD ports (ADR-0159), the
`integer_adm.c` upstream-mirror block from Netflix `966be8d5`, the
`float_motion.c` upstream port (Research-0024 Strategy A from
Netflix `b949cebf`), the SYCL kernel-launch entry pattern across
`sycl/integer_adm_sycl.cpp` / `integer_motion_sycl.cpp` /
`integer_vif_sycl.cpp`, the upstream `xiph/psnr_hvs.c` block, and
three remaining `tools/vmaf.c` driver functions
(`copy_picture_data`, `init_gpu_backends`, `main`). Every one of
those NOLINTs already carried prose justification, but a strict
read of [ADR-0141](0141-touched-file-cleanup-rule.md) §2 ("every
NOLINT must cite, inline, the ADR / research digest / rebase
invariant that forces it") flagged 22 sites whose comments described
the load-bearing invariant in prose without naming the ADR
explicitly.

The audit also confirmed the original T7-5 cluster framing is now
factually closed: there are zero `_iqa_*` reserved-identifier
NOLINTs anywhere in `core/src/`, and there are zero bare /
uncited `readability-function-size` NOLINTs after this PR.

## Decision

Land a cite-only sweep that appends an explicit
`(ADR-0141 §2 ... load-bearing invariant; T7-5 sweep closeout —
ADR-0278)` reference to each of the 22 surviving NOLINT sites that
described the invariant in prose without naming an ADR. No function
bodies are split. No behavioural change. The PR formally closes
backlog item **T7-5**.

The choice of ADR reference per cluster:

| Cluster | Files | Citation appended |
|---|---|---|
| Upstream-mirror parity | `integer_adm.c::adm_decouple_s123`, `cuda/ssimulacra2_cuda.c` (3 sites), `vulkan/ssimulacra2_vulkan.c` (3 sites), `vulkan/cambi_vulkan.c` (1 site) | ADR-0141 §2 upstream-parity load-bearing invariant + T7-5 / ADR-0278 |
| SYCL kernel-launch pattern | `sycl/integer_adm_sycl.cpp` (6), `sycl/integer_motion_sycl.cpp` (2), `sycl/integer_vif_sycl.cpp` (4) | ADR-0141 §2 load-bearing invariant + T7-5 / ADR-0278 |
| `tools/vmaf.c` structural | `copy_picture_data`, `init_gpu_backends`, `main` | ADR-0141 §2 load-bearing invariant + T7-5 / ADR-0278 (and ADR-0146 prior sweep precedent for `main`) |

After this PR a programmatic audit (`scripts` / repo-wide regex)
of every `NOLINT(readability-function-size)` site reports
**75 sites, 0 missing ADR or Research-digest references**.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Cite-only sweep across all 22 sites (chosen)** | Discharges the ADR-0141 §2 letter-of-the-rule audit; no behavioural change; rebase-friendly; closes T7-5 in one mechanical PR | Adds parenthetical text to existing comments; does not reduce function sizes | **Decision** — every site already had prose justification; the gap was citation form, not invariant strength |
| Refactor the surviving SIMD / upstream-mirror sites | Would drop the NOLINTs entirely | ADR-0146's verification matrix (Netflix golden under `VMAF_CPU_MASK=0/255`, `/cross-backend-diff`) ran for fork-local files; refactoring upstream-mirror IDs (Netflix `966be8d5`) would multiply rebase pain and risk SIMD-vs-scalar drift | Rejected — these are exactly the sites the ADR-0141 carve-out exists for (load-bearing-invariant NOLINTs) |
| Defer to a future "T7-5b" PR after introducing a CI lint-rule that enforces ADR refs in NOLINT comments | Would automate enforcement going forward | Extra scope, blocks T7-5 closure on CI-rule design; doesn't move the existing 22 sites | Rejected — landing the citations now is independent of the future automation, and unblocks the ADR-0141 §Historical debt language |
| Strengthen ADR-0141 §Historical debt to formally mark T7-5 closed | Tidies the parent ADR | ADR-0141 is Accepted (per the project's immutable-once-Accepted rule); modifying its body is not allowed | Rejected — closure is recorded here in the Supersedes-style follow-up ADR (ADR-0278) instead |

## Consequences

- **Positive**:
  - Programmatic audit of `NOLINT(readability-function-size)` across
    `core/src/` + `core/tools/` reports **75 sites, 0 missing
    ADR / Research citations**.
  - Backlog item **T7-5** is closed. Future PRs that add new NOLINT
    sites are governed by ADR-0141 §2 directly; there is no
    historical-debt carve-out left to invoke.
  - The cluster taxonomy in this ADR is a template for future cite
    decisions: upstream-parity → cite ADR-0141 §2; SIMD bit-exact →
    cite ADR-0138 / ADR-0139; SYCL kernel-launch → cite ADR-0141 §2
    with the kernel-inlining rationale.
- **Negative**:
  - The added `(ADR-0141 §2 ...; T7-5 sweep closeout — ADR-0278)`
    parentheticals make the existing comment lines longer; the
    SYCL pattern in particular pushes one already-long
    `// NOLINTNEXTLINE(...)` comment past the column budget. Format
    is preserved for grep-ability across the 12 SYCL sites
    (verbatim duplicate string is the load-bearing invariant
    documentation).
  - Citation-only sweeps don't reduce function size; if the
    SYCL kernel-launch budget changes upstream, all 12 of these
    will need a fresh look.
- **Neutral / follow-ups**:
  - Future enforcement: a `scripts/ci/check-nolint-citation.sh`
    would mechanise ADR-0141 §2 — out of scope here, queued as
    backlog item T7-5b if the rule grows teeth.
  - When a future ADR supersedes the cluster (e.g., a new
    upstream-parity policy), the citations should be retargeted
    in-place rather than removed wholesale.

## Verification

- Programmatic check: a Python audit walks every
  `NOLINT(readability-function-size)` site, reads the preceding
  comment block (up to 14 lines, contiguous comment-only),
  and verifies the union contains either `ADR-NNNN` or
  `Research-NNNN`. After this PR: 75 sites, 0 missing.
- `meson test -C build` (CPU-only setup) — pass count to be
  confirmed in PR body.
- `make test-netflix-golden` — Netflix CPU golden gate (3
  reference pairs) green.
- No assertions modified, no `assertAlmostEqual` baselines
  touched.

## References

- [ADR-0141](0141-touched-file-cleanup-rule.md) — touched-file
  lint-clean rule; §Historical debt scoped T7-5.
- [ADR-0146](0146-nolint-sweep-function-size.md) — Sweep A
  (parent function-size sweep PR #82).
- PR #293 — Sweeps B+C (cite-only fork-added SIMD/scalar/test).
- PR #327 — Follow-up (`float_adm.c`, `tools/vmaf.c::main`).
- [ADR-0138](0138-iqa-convolve-avx2-bitexact-double.md) — SIMD
  bit-exactness (cited inline at SIMD NOLINT sites elsewhere
  in tree, not modified by this PR).
- [ADR-0139](0139-ssim-simd-bitexact-double.md) — SSIM SIMD
  per-lane reduction.
- [ADR-0108](0108-deep-dive-deliverables-rule.md) — six
  deep-dive deliverables (this PR ships them).
- Source: `req` (user prompt 2026-05-04 paraphrased: T7-5 NOLINT
  sweep closeout — refactor or cite-only justify the residual
  `readability-function-size` and `_iqa_*` NOLINTs; pin to
  ADR-0278; cite ADR-0146 as parent precedent).
