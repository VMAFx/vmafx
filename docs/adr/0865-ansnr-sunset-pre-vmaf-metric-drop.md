# ADR-0865: Sunset ANSNR — drop `ansnr` / `float_ansnr` feature extractors

- **Status**: Accepted
- **Date**: 2026-05-28
- **Deciders**: lusoris
- **Tags**: `metric`, `feature-extractor`, `breaking-change`, `cleanup`, `fork-local`

## Context

ANSNR (Average Noise-to-Signal Ratio) is a pre-VMAF (circa 2001) reduced-reference
quality metric. Netflix shipped it in the legacy SVM regressor (`model_V8a.model`,
the four-feature `vif + adm + ansnr + motion` linear combiner), but **never adopted
it in any production VMAF model**. Every shipped VMAF release — `vmaf_v0.6.1`,
`vmaf_4k_v0.6.1`, `vmaf_b_v0.6.3`, and the neg/4k variants — drops ansnr from the
feature set. Upstream Netflix/vmaf still carries the `ansnr` / `float_ansnr`
extractors in their C tree as historical scaffolding for the legacy SVM runner;
they are otherwise dead weight.

On this fork:

- PR #38 (merged 2026-05-28) removed `float_ansnr` from the C backend across
  all enabled paths (CPU scalar, AVX2, AVX-512, NEON, CUDA, HIP). PR #295 and
  PR #324 followed up with downstream cleanup (Python `ATOM_FEATURES` hygiene,
  test pruning, FFmpeg-patches stack alignment). PR #38's body cited
  `Parent ADR-0709` as the authorising decision record, but ADR-0709 is the
  **Phase 4b distributed-platform umbrella** — it contains zero ANSNR content.
  No dedicated ADR existed for the ANSNR-drop decision until this one.
- ADR-0749 ("Sunset VmafLegacyQualityRunner") covers the *Python* side: once
  PR #38 removed `float_ansnr` from the C output, the legacy float-path quality
  runner became unusable (the SVM scoring step `KeyError`s on the missing
  `float_ansnr` key). ADR-0749 sunsets the runner but explicitly does not
  re-litigate the C-side feature drop — it points at PR #38 as the upstream
  cause. That cite is correct; ADR-0749 is the *consequence* ADR. The *cause*
  ADR is this one.
- `docs/research/0733-feature-importance-audit-2026-05-28.md` (Research-0733)
  is the empirical justification: the audit ranked feature importance across
  shipped models and confirmed ANSNR's zero contribution to any non-legacy
  model's score. The user authorised the drop on 2026-05-28 after reviewing
  the digest ("drop legacy only" — distinct from the broader proposal to
  prune additional underweighted features).

Per CLAUDE.md §12 r8 + ADR-0108, every fork-local non-trivial architectural
decision gets its own ADR file. The ANSNR drop is fork-local (diverges from
upstream Netflix), affects a public-facing feature surface (CLI flag
parsing, JSON output schema, FFmpeg filter probe), and is a breaking change
for any caller that requested the `ansnr` / `float_ansnr` features by name.
The decision was therefore ADR-required and the missing ADR is a Rule-8
compliance gap that this ADR closes.

## Decision

We sunset ANSNR as a first-class VMAF feature on this fork. Concretely:

1. `ansnr` and `float_ansnr` feature extractors are removed from every enabled
   backend (CPU scalar, AVX2, AVX-512, NEON, CUDA, HIP) — done by PR #38.
2. The legacy four-feature SVM quality runner that depended on `float_ansnr`
   (`VmafLegacyQualityRunner`) is sunset per ADR-0749.
3. The FFmpeg-patches stack drops the `ansnr` feature probe in `vf_libvmaf.c`
   so the rebased filter does not attempt to enable a missing extractor.
4. Python `ATOM_FEATURES` is purged of stale `ansnr` mappings (handled in
   PR #295 / `changelog.d/fixed/atom-features-stale-ansnr.md`).
5. Vulkan / SYCL / Metal backends never shipped ANSNR; no action required.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Restore `float_ansnr` C implementation | Legacy SVM runner would work again; preserves upstream parity in feature listing | Reinstates a pre-VMAF metric Netflix never shipped in production; directly contradicts PR #38's rationale; saddles every backend with maintenance burden (CUDA kernel, HIP port, AVX2/AVX-512 SIMD paths) for a feature with zero downstream consumers | Rejected — the empirical Research-0733 importance ranking shows zero contribution to any shipped model |
| Keep extractors, mark deprecated, skip ANSNR in default feature set | Runner callable without crashing; gradual deprecation curve | Ansnr column silently absent or wrong; SVM model expects 4 features and computes garbage when one is missing; deceptive public API; defers the breaking-change cost rather than absorbing it now | Rejected — broken-but-callable API is worse than the explicit removal; per CLAUDE.md §correctness-first, no silent-wrong-result paths |
| Author dedicated ADR per ADR-0108 rule (this ADR) | Closes the Rule-8 compliance gap from PR #38; gives ADR-0749 a parent to cite; makes the cause/consequence chain searchable; documents the bad `Parent ADR-0709` cite for future readers | Late paperwork; the decision was already enacted by PR #38 | **Accepted** — back-dated to PR #38's merge date (2026-05-28) per the standard "Accepted on the date the decision became binding" convention |
| Author the ADR but skip the cross-cite cleanup | Smaller PR | Downstream PRs (#295, #324) inherit the bad `Parent ADR-0709` cite forever | Rejected — the per-tree grep is cheap and the fix-up is the whole point of authoring this ADR |

## Consequences

- **Positive**:
  - CI gate T-LEGACY-RUNNER-ANSNR-BROKEN is fully resolved (ADR-0749 closed
    the Python half; this ADR documents the C half that caused it).
  - Maintenance surface reduced by one extractor across 6 backend
    implementations (CPU scalar, AVX2, AVX-512, NEON, CUDA, HIP) — roughly
    1,200 LOC of fork-original / upstream-mirrored kernel code retired.
  - FFmpeg-patches stack one entry shorter; future ffmpeg rebases skip the
    ansnr-probe conflict.
  - ADR-0108 compliance hole closed: PR #38 / #295 / #324 now have a real
    parent ADR to cite (this one) instead of the wrong ADR-0709.
- **Negative**:
  - **Breaking change** for any caller invoking `vmaf --feature ansnr` /
    `vmaf --feature float_ansnr`. Migration path: omit the flag (no
    shipped model uses ANSNR) or use `VmafQualityRunner` with
    `vmaf_float_v0.6.1.pkl` / a current `.json` model.
  - **One ADR cite cannot be corrected**: PR #38's body on GitHub cites
    `Parent ADR-0709`. PR bodies are part of the immutable merge-commit
    record on the remote; we cannot rewrite history. Future readers
    landing on PR #38 will hit the wrong ADR. This ADR's `## Notes`
    section documents the cite so the trail is recoverable from either
    direction.
- **Neutral / follow-ups**:
  - PR #295 already swept stale `ansnr` mappings out of Python
    `ATOM_FEATURES` (see `changelog.d/fixed/atom-features-stale-ansnr.md`).
  - PR #324 aligned the FFmpeg-patches stack (`vf_libvmaf.c` no longer
    probes the missing extractor).
  - `core/include/libvmaf/feature_extractor.h` no longer exports the
    `ansnr` / `float_ansnr` extractor descriptors; downstream callers
    must adapt or stay on a pre-PR-38 release.
  - No model retraining required — no shipped model uses ANSNR.

## Notes

**Historical citation correction**: PR #38's body cites `Parent ADR-0709`
as the authorising decision record. This is incorrect. ADR-0709 is the
**VMAFX Phase 4b distributed-platform umbrella** and contains no ANSNR
content. The correct parent ADR is **this one (ADR-0865)**, back-dated
to PR #38's merge date so the dependency chain is consistent.

Affected PR bodies (cannot be rewritten because they are merged-commit
history on GitHub):

- PR #38 — original ANSNR-drop C-side change
- PR #295 — Python `ATOM_FEATURES` cleanup (cites PR #38)
- PR #324 — FFmpeg-patches alignment (cites PR #38)

In-tree tree-side citations were audited (see commit message); all
remaining `ADR-0709` references point at the Phase 4b umbrella content
correctly. No in-tree cleanup was required because the bad cite never
landed in `docs/` or `changelog.d/` — it was confined to PR descriptions
on the remote.

## References

- PR #38 (merged 2026-05-28) — drop legacy `ansnr` feature extractor from C backend.
- PR #295 — Python `ATOM_FEATURES` stale `ansnr` mapping cleanup.
- PR #324 — FFmpeg-patches stack alignment (drop ansnr probe in `vf_libvmaf.c`).
- [ADR-0749](0749-sunset-legacy-vmaf-feature-extractor.md) — sunset
  `VmafLegacyQualityRunner` (Python consequence of this decision).
- [Research-0733](../research/0733-feature-importance-audit-2026-05-28.md) —
  empirical feature-importance audit; ANSNR shows zero contribution to any
  shipped VMAF model.
- [ADR-0108](0108-deep-dive-deliverables-rule.md) — six-deliverables rule
  (this ADR closes the Rule-8 ADR-required compliance gap from PR #38).
- Source: `req` (direct user quote, 2026-05-28): "drop legacy only".
- Source: `req` (direct user direction, 2026-05-30): "Author a proper
  ANSNR sunset ADR and fix the broken citations across the tree."
  (paraphrased per CLAUDE.md user-quote-handling rule.)
