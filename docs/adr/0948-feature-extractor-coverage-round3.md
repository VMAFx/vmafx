<!-- markdownlint-disable MD013 MD060 -->
# ADR-0948: Feature-extractor coverage round 3 — targeted unit tests for low-coverage files

- **Status**: Accepted
- **Date**: 2026-05-31
- **Deciders**: @lusoris, Claude Opus 4.7 (agent)
- **Tags**: `test`, `coverage`, `feature`

## Context

The 2026-05-31 gcovr baseline against `core/src/feature/` (CPU-only build,
fast+simd suites) showed a handful of files in the 40 %–85 % band that
neither the existing test suite nor the two prior coverage pushes —
PR #344 (round 1 — `mkdirp.c` 0→80 %, `luminance_tools.c` 78→93 %,
`feature_name.c` 85→92 %, `feature_extractor.c` 73→77 %) and PR #433
(round 2 — `barten_csf_tools.h`, `integer_motion{,_v2,_psnr}.c`,
`integer_vif.h` log2, `iqa/convolve.c` KBND helpers,
`ms_ssim_decimate.c`) — had reached. The Coverage Gate (ADR-0114) holds
the global floor at 37 %; raising the per-file floor on still-low files
is cheap insurance against silent regressions in NULL-guard / duplicate-
write / mirror-extension paths that production callers rarely exercise.

The remaining candidates after subtracting the in-flight rounds were:

| File | Baseline line / branch | Why it stayed low |
|---|---|---|
| `integer_motion.h` | 58 % / 40 % | `edge_16` 5-tap mirror helper — reached only via the SIMD hot path's edge-frame iterations; interior-frame fixtures rarely trigger the four mirror branches simultaneously. |
| `adm_csf_tools.h` | 0 % / 45 % | `adm_native_csf` inline reached only from `adm_tools.c` setup; no targeted unit test pinned the CSF curve values for either the H/V or diagonal branch. |
| `feature_collector.cpp` | 90 % / 62.5 % | Line coverage already high; branch coverage missed the deterministic NULL-input guards, duplicate-key rejects in `aggregate_vector_append`, duplicate-index rejects in `feature_vector_append`, and the unmount-not-found path. |

## Decision

Add three focused test binaries under `core/test/`:

1. `test_integer_motion_edge16_coverage` — drives `edge_16` directly
   for interior, left mirror, right mirror, top mirror, and bottom
   mirror paths with closed-form expected accumulators against the
   `{3571, 16004, 26386, 16004, 3571}` 5-tap filter.
2. `test_adm_csf_tools_coverage` — drives `adm_native_csf` on a
   representative `(lambda, view_dist, display_height)` sweep for
   both `theta=0` (H/V) and `theta=45` (diagonal /= 0.7) branches,
   cross-checking against an inline closed-form reference.
3. `test_feature_collector_coverage` — exercises the deterministic
   guard / reject branches: `aggregate_vector_append` NULL guard +
   duplicate-key reject, `feature_vector_append` NULL + duplicate-
   index reject, `vmaf_feature_collector_init(NULL)`,
   `vmaf_feature_collector_append` NULL guards + duplicate-index
   propagation, `vmaf_feature_collector_unmount_model` NULL inputs
   ot-found path, `set/get_aggregate` round-trip, and `destroy(NULL)`.

All three binaries link against the existing `libvmaf` static target —
no new build-system surface, no symbol changes to production code, no
behavioural deltas.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Push `cambi.c` (48 % line) instead | Larger absolute coverage delta; touches a real production extractor. | Most uncovered functions are file-static helpers reachable only through the full `cambi_score` pipeline — adding a test would duplicate the existing `test_cambi.c` integration scaffold and risk overlap with future cambi PRs. | Deferred. The chosen three files are independent units with no in-flight PR overlap. |
| Push `feature_dists.c` / `feature_lpips.c` (25 % line) | Highest line-coverage delta available. | Both extract paths require an ONNX runtime + a model file; without those, only the registration / NULL-guard surface (already covered by `test_dists.c`, `test_lpips.c`) is reachable from a unit test. | Out of scope; tracked separately under the tiny-AI suite. |
| Push `speed_qa.c` HBD branch (86 % → ≥95 %) | Targeted; would close the 10-bit normalisation gap. | At 86 % the file is just above the 50–85 % round-3 target band, and the existing `test_speed_qa.c` could absorb the additions without a new binary. | Deferred to a follow-up that lives inside `test_speed_qa.c`. |
| Skip round 3 entirely | Lower CI cost. | Coverage Gate floor is at 37 % globally; the still-low files compound across rounds and become harder to raise once feature work lands on top. | Rejected — the per-file pushes are cheap and reduce the bus factor on guard branches. |

## Consequences

- **Positive**: per-file line coverage on the three targets jumps to
  100 % (`integer_motion.h`), 75 % (`adm_csf_tools.h` — one inlined-
  declaration line still flagged due to a gcov + `FORCE_INLINE`
  artifact, function body fully executed), and 91.5 % line /
  71.9 % branch (`feature_collector.cpp`). The Coverage Gate floor
  edges upward.
- **Negative**: three additional CI test binaries; each runs in ≤10 ms.
  Negligible CI cost.
- **Neutral / follow-ups**: future rounds can target `speed_qa.c` HBD,
  `feature_dists.c` / `feature_lpips.c` (gated on ONNX-runtime test
  scaffolding), and `cambi.c` (gated on a static-helper extraction
  refactor). The 1-line `adm_csf_tools.h` gcov artefact is a known
  limitation of gcov-on-inlined-declarations and does not warrant
  a code restructure.

## References

- ADR-0114 — Coverage Gate (37 % floor).
- ADR-0138 / ADR-0140 — SIMD bit-exactness contract (governs the
  existing `test_iqa_convolve.c`; this round does not extend it).
- ADR-0500 — Compact log2 LUT (governs PR #433's
  `test_integer_vif_log2.c`; this round avoids overlap).
- PR #344 — Coverage round 1 (mkdirp / luminance_tools / feature_name
  / feature_extractor).
- PR #433 — Coverage round 2 (barten_csf_tools / integer_motion{,_v2,
  _psnr} / iqa/convolve / ms_ssim_decimate / integer_vif log2).
- Source: `req` — user direction 2026-05-31, "core/src/feature
  coverage round 3 — push the still-low files."
