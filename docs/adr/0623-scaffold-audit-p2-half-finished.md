<!-- markdownlint-disable MD013 MD060 -->
# ADR-0623: Scaffold audit P2 — half-finished implementation fixes

- **Status**: Accepted
- **Date**: 2026-05-19
- **Deciders**: lusoris
- **Tags**: `ci`, `build`, `docs`, `hip`, `adm`, `state`

## Context

The scaffold audit conducted on 2026-05-19 (`docs/research/scaffold-audit-2026-05-19.md`)
identified nine P2-severity findings: items where a feature is partially implemented or
tracked, but a gap was left without a state.md row, a build gate, or a clear reactivation
criterion.  P2 items do not cause silent wrong scores (that is the P0/P1 domain) but they
accumulate as invisible technical debt: advisory CI gates that are never promoted, disabled
jobs that have no tracking issue, and doc/registry mismatches that confuse users.

This ADR groups all nine fixes into a single PR because each individual diff is a
one-liner or near-one-liner, and grouping avoids the PR-overhead cost while keeping the
changes reviewable.

The nine items addressed:

1. **integer ADM `adm_p_norm`** — ROI block hardcoded to `3.0f`; float path has a
   configurable option that the integer path ignored.
2. **`float_vif_hip` auto-dispatch flag** — `VMAF_FEATURE_EXTRACTOR_HIP` intentionally
   absent (pending T7-10c); no build gate or Meson option to re-enable it once T7-10c
   lands.
3. **SYCL clang-tidy CI permanently disabled** — `&& (false)` with no tracking issue in
   state.md and no reactivation criterion documented.
4. **Docker image CI smoke step absent** — CI proves only "the Dockerfile parses"; no
   `docker run vmaf --version` step; `T7-DOCKER-SMOKE` referenced in a comment but
   missing from state.md.
5. **Vulkan motion/lavapipe diff advisory** — `T-VULKAN-MOTION-LAVAPIPE-INIT` referenced
   in CI comments but absent from state.md Open table.
6. **GPU coverage gate — no stability start date** — "two stable weeks" criterion had no
   reference start date, making promotion condition unverifiable.
7. **`konvid_mos_head_v1.md` stale path** — card referenced `.workingdir2/` paths after
   data moved to `.corpus/` in PR #983 (ADR-0547).
8. **`u2netp_mirror_card.md`** — forward-declaration card with no prominent banner
   indicating there is no backing ONNX in tree.
9. **`lpips_sq_v1` registry id / card name mismatch** — registry uses `lpips_sq_v1`,
   card file was `lpips_sq.md`.

## Decision

We will implement all nine fixes in one PR:

1. Add `adm_p_norm` field and `options[]` entry to integer ADM's `AdmState`; document in
   code that the SIMD dispatch paths still use the hardcoded default pending a function-
   pointer signature update (tracked as T-INTEGER-ADM-P-NORM-SIMD-GAP in state.md).
2. Add `enable_float_vif_hip_autodispatch` Meson option (default `false`); use a
   preprocessor define to conditionally set `VMAF_FEATURE_EXTRACTOR_HIP` in
   `float_vif_hip.c`.
3. Add a `# Tracked as T-SYCL-CLANG-TIDY-DISABLED in docs/state.md` comment and Open
   row in state.md with a reactivation criterion.
4. Add a `docker run --rm vmaf /usr/local/bin/vmaf --version` smoke step to the Docker
   CI job; add `T-DOCKER-SMOKE` Open row in state.md.
5. Add `T-VULKAN-MOTION-LAVAPIPE-INIT` Open row in state.md with closure criterion;
   update the CI comment to cite the ADR.
6. Add stability start date 2026-05-19 and promotion target 2026-06-02 to the
   `coverage-gpu` job comment; add `T-GPU-COVERAGE-STABLE-WEEKS` Open row.
7. Replace all `.workingdir2/` occurrences in `konvid_mos_head_v1.md` with `.corpus/`.
8. Add a prominent forward-declaration status banner at the top of
   `u2netp_mirror_card.md` citing ADR-0265.
9. Rename `docs/ai/models/lpips_sq.md` → `docs/ai/models/lpips_sq_v1.md`; update the
   cross-reference in `docs/ai/overview.md`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| One PR per finding | Smallest possible diff per PR | Nine CI round-trips; PR overhead dominates for one-liner changes | Rejected per `feedback_pr_size_consolidation` memory rule |
| Full `adm_p_norm` plumbing through SIMD function pointer | Fully consistent option across all paths | Requires signature change in `adm_avx2.c` and `adm_avx512.c`; cascade risk for a P2 fix | Deferred to a follow-up; tracked as T-INTEGER-ADM-P-NORM-SIMD-GAP |
| Delete the disabled SYCL clang-tidy job entirely | Removes dead CI YAML | Harder to re-enable once the Intel toolchain improves | Rejected — the disable itself is fine; the issue was the lack of tracking |
| Promote Docker CI from advisory to required | Stronger guarantee | May break on transient Docker Hub rate limits / registry issues | Deferred; keep advisory until T-DOCKER-SMOKE stable ≥ 2 weeks |

## Consequences

- **Positive**: all nine half-finished gaps have state.md rows or code-level documentation;
  future contributors can find and close them without re-auditing the codebase; the
  `lpips_sq_v1` registry id and card filename now match; the `u2netp_mirror_card.md` banner
  prevents users from assuming the ONNX exists; `konvid_mos_head_v1.md` training commands
  reference the correct `.corpus/` path.
- **Negative**: the `adm_p_norm` gap is now a tracked open item rather than a silent TODO;
  the Docker smoke step will fail if the root `Containerfile` produces an image without
  `vmaf` in the expected path (expected and acceptable — that is the point).
- **Neutral / follow-ups**: T-INTEGER-ADM-P-NORM-SIMD-GAP, T-SYCL-CLANG-TIDY-DISABLED,
  T-DOCKER-SMOKE, T-VULKAN-MOTION-LAVAPIPE-INIT, and T-GPU-COVERAGE-STABLE-WEEKS are all
  now Open rows in state.md with concrete closure criteria.

## References

- `docs/research/scaffold-audit-2026-05-19.md` — source audit document (P2 section).
- [ADR-0108](0108-deep-dive-deliverables-rule.md) — six-deliverables rule.
- [ADR-0165](0165-state-md-bug-tracking.md) — state.md maintenance rule.
- [ADR-0547](0547-corpus-dir-migration.md) — `.workingdir2/` → `.corpus/` migration (P2-7).
- [ADR-0265](0265-u2netp-saliency-replacement-blocked.md) — u2netp forward-declaration decision (P2-8).
- [ADR-0325](0325-konvid-150k-corpus-ingestion.md) / [ADR-0336](0336-konvid-mos-head-v1.md) — konvid MOS head context (P2-7).
- Source: paraphrased from the `scaffold-audit-p2-half-finished` dispatch instructions in the task.
