<!-- markdownlint-disable MD013 -->
# Research-0010: Is Netflix about to ship a SpEED-driven VMAF successor?

- **Status**: Active
- **Workstream**: informational audit — no ADR yet
- **Last updated**: 2026-04-21

## Question

A fork-maintainer memory note flagged (2026-04-21) a suspicion that
Netflix has an unpublished VMAF model about to land, likely driven by
the SpEED (Spatial Efficient Entropic Differencing, IEEE 7979533)
feature extractor. Secondary signals: Netflix just opened upstream
PR #1488 "port speed updates"; Netflix is said to reference a
phone-targeted model not in the public tree; upstream hasn't shipped
a new model JSON in ~5 years.

Should fork planning treat "SpEED-driven Netflix VMAF v3" as a
realistic near-term landing, or is the signal weaker than the memory
note assumed? What port work — if any — should the fork queue now so
it isn't caught flat-footed when a successor lands?

## Sources

### Upstream code / PRs

- Netflix PR [#1488](https://github.com/Netflix/vmaf/pull/1488) "port speed updates", opened 2026-04-20 (Kyle Swanson / @kylophone). 7 files, +570 / -222. Commits `18c07b22`, `cc615237`.
- Netflix branch `upstream/speed_ported` — 4 commits, tip `86d929fc`. Carries `core/src/feature/speed.c` (1,566 LoC, commit `1e67d38d`, Swanson + Miret 2025-05-02). PR #1488 ships only the **first** of those four commits.
- Netflix PR [#1391](https://github.com/Netflix/vmaf/pull/1391) "Add vmaf_v0.6.1still.json", opened 2024-10-11 by @cosmin (Meta). The only genuinely-pending model JSON anywhere in Netflix-upstream review.
- [`resource/doc/models.md`](https://github.com/Netflix/vmaf/blob/master/resource/doc/models.md) — authoritative upstream model catalog; describes `--phone-model` as a score-transform applied to `vmaf_v0.6.1.json`, not a separate JSON.
- Netflix issue [#645](https://github.com/Netflix/vmaf/issues/645) "Did the HDR model ever get released?" — CLOSED 2025-07-25.

### Literature — efficient-VMAF / SpEED lineage

- Bampis et al., "SpEED-QA: Spatial Efficient Entropic Differencing for Image and Video Quality", [IEEE 7979533](https://ieeexplore.ieee.org/document/7979533/).
- Venkataramanan / Stejerean / Katsavounidis / Bovik, "One Transform To Compute Them All: Efficient Fusion-Based Full-Reference Video Quality Assessment", IEEE TIP 33:509, 2024, [arXiv:2304.03412](https://arxiv.org/abs/2304.03412) — FUNQUE+. Claims 4.2–5.3 % accuracy and 3.8–11× speed vs VMAF. Open-source at [funque_plus](https://github.com/abhinaukumar/funque_plus).
- Hammou / Cai / Madhusudanarao / Bampis / Mantiuk, "Do image and video quality metrics model low-level human vision?", [arXiv:2503.16264](https://arxiv.org/abs/2503.16264) (Mar 2025). Benchmarks 33 metrics including VMAF against contrast sensitivity / masking / matching; flags VMAF as poor at contrast masking.
- Hammou / Krasula / Bampis / Li / Mantiuk, "HDR-VDC dataset", QoMEX 2024, [DOI 10.1109/QoMEX61742.2024.10598289](https://www.repository.cam.ac.uk/items/7ad9e48f-31bd-4b49-8a51-aee7691154bf).
- Sivashanmugam et al., "A Low-Complexity Perceptual Video Quality Metric for SVT-AV1" (pVMAF), Mile-High Video 2025, [DOI 10.1145/3715675.3715850](https://dl.acm.org/doi/10.1145/3715675.3715850).

### Netflix engineering outputs

- [VMAF: The Journey Continues](https://netflixtechblog.com/vmaf-the-journey-continues-44b51ee9ed12) — no "v3" or "next-generation" wording.
- [All of Netflix's HDR video streaming is now dynamically optimized](https://netflixtechblog.com/all-of-netflixs-hdr-video-streaming-is-now-dynamically-optimized-e9e0cb15f2ba) (Nov 2023, Mavlankar / Li / Krasula / Bampis) — HDR-VMAF rollout. HDR-VMAF itself described as "tailored to our internal pipelines… in the process of improving before the official release" — confirms an unreleased successor exists but it is HDR-VMAF, not a SpEED-driven VMAF v3.
- [Measuring and Predicting Perceptions of Video Quality across Screen Sizes](https://research.netflix.com/publication/measuring-and-predicting-perceptions-of-video-quality-across-screen-sizes) (Li / Krasula) — phone-adjacent crowdsourced screen-size study; no model release attached.

## Findings

### 1. SpEED source is dead code even upstream

The SpEED C implementation does exist in the Netflix tree, but only on
branch `upstream/speed_ported` — not on upstream `master`, and not in
PR #1488. The source (`core/src/feature/speed.c`, 1,566 LoC)
defines two extractors that emit four feature names:

- `Speed_chroma_feature_speed_chroma_u_score`
- `Speed_chroma_feature_speed_chroma_v_score`
- `Speed_chroma_feature_speed_chroma_uv_score`
- `Speed_temporal_feature_speed_temporal_score`

Two hard-blocker gaps exist on the `speed_ported` branch itself:

1. **No model consumes the features.** `git grep Speed_chroma_feature
   model/` and `git grep Speed_temporal_feature model/` both return
   zero matches in every `.json` in the Netflix tree.
2. **The extractors are not registered.** `core/src/feature/feature_extractor.c`
   does not extern-declare `vmaf_fex_speed_chroma` or
   `vmaf_fex_speed_temporal`, so they are not added to the global
   feature list. Even if the file were compiled into the shared
   library, the runtime could not instantiate the extractor.

The implementation is scalar-only (no AVX2 / AVX-512 / NEON / CUDA /
SYCL), including the eigenvalue / QR paths that are natural SIMD
targets.

### 2. PR #1488 ships the VIF prelude, not SpEED itself

The PR is the **first** of a four-commit chain on `speed_ported`. It
changes VIF helper infrastructure only:

- `convolution_internal.h` / `vif_tools.c` — edge-mirror bugfix
  (`- 1` → `- 2`, 9 call sites).
- `vif_tools.h` — new `enum vif_scaling_method` (nearest / bilinear /
  bicubic / lanczos4), 21-entry `valid_kernelscales[]` lookup.
- New symbols: `vif_validate_kernelscale`, `vif_get_filter_size`,
  `vif_get_filter`, `vif_dec16_s`, `vif_get_scaling_method`,
  `vif_scale_frame_s`, **and** `speed_get_antialias_filter` (only
  `speed_*`-named symbol in the PR; confirms SpEED will consume the
  antialias path with a `sqrt(scale)` sigma trick).
- Four `python/test/*.py` — golden-score **tolerance loosening** on
  existing assertions. No new golden values. No new fixtures.
- Zero model JSONs touched.

Interpretation: PR #1488 prepares the runway for the SpEED extractor
that waits on the remaining three commits of `speed_ported`, and it
also hints at FUNQUE-family integration (antialias + lanczos4 + kernel
scaling is the FUNQUE / SAST resizer family — cf. upstream PR #1361
"Implement SAST resizer from FUNQUE" by @cosmin, 2024-04-23, still
OPEN).

### 3. Model-hunt: no hidden successor

Upstream `master`'s `model/` tree holds 9 JSONs (plus the legacy
`other_models/`, `vmaf_rb_v0.6.2`, `vmaf_rb_v0.6.3`,
`vmaf_4k_rb_v0.6.2` subdirs) — identical to the fork. **No new model
JSON has landed on upstream `master` since `af5f7aa6 Add
vmaf_4k_v0.6.1neg model`** (predates 2024).

The **only** genuinely-pending model artefact in Netflix-upstream
review is `vmaf_v0.6.1still.json` (PR #1391). It is a 77-line "force
motion to zero" model by @cosmin (Meta, not Netflix), reviewed but
unmerged for 18 months. Neither Netflix nor the fork has it.

The **"phone model gap" memory signal is wrong.** Per upstream
`resource/doc/models.md`: *"The default VMAF model
(`model/vmaf_v0.6.1.json`) also offers a custom model for cellular
phone screen viewing. This model can be invoked by adding
`--phone-model` option."* The phone model *is* shipped — as a
score-transform applied to `vmaf_v0.6.1.json`, not as a separate
JSON. No corroborating internal artefact exists.

`core/src/model.c` does not reference any JSON that is missing
from disk; grep for `phone|mobile` in `resource/doc/*.md` matched
only the existing phone-transform prose. No draft PR or branch
stages a new 4K / HDR / mobile / internal model.

### 4. External signals point away from SpEED, not toward it

- **Bampis (Netflix)**: recent work is HDR + psychophysics
  (arXiv 2503.16264, HDR-VDC QoMEX 2024, Learned Fractional
  Downsampling SPIC 2024). Not SpEED. The Mantiuk collab
  (arXiv 2503.16264) explicitly benchmarks VMAF as weak on
  contrast masking — reads as *motivation* for a successor, not a
  drop-in port of SpEED.
- **Bovik (UT Austin)**: the efficient-VMAF line of descent in his
  group is **FUNQUE / FUNQUE+ / Cut-FUNQUE** — a transform-domain
  single-pass fusion metric that pitches itself explicitly as a
  VMAF-computational replacement. Open-source. This is the live
  thread.
- **Soundararajan (IISc)**: moved toward no-reference / blind IQA.
- **Gupta**: left the VMAF track; now at Lab126 / Google on
  facial-expression datasets.
- **Netflix Tech Blog 2022–2025**: no "VMAF v3" / "next-generation
  VMAF" / SpEED-in-production language. The productionisation signal
  is **HDR-VMAF**, referenced in Nov 2023 as an unreleased internal
  tool. No phone-model announcement.
- **Competitive**: Synamedia's pVMAF (MHV 2025) is a bitstream +
  pixel fusion approximator, orthogonal to SpEED. AIM 2024 / NTIRE
  2024 / AIS 2024 / ICME 2025 HDR-SDR challenges are dominated by
  learned / transformer-based VQA (KVQA, TVQA-C). The field has
  moved past SpEED toward learned features.

## Bottom line

No public evidence supports "Netflix has an unpublished SpEED-driven
VMAF v3 about to land." The visible Netflix signal is (a) HDR-VMAF
productionisation and (b) a psychophysics / neural direction. The
strongest efficient-VMAF thread in open literature is FUNQUE+ (Bovik
group, UT Austin) — open-source, a port candidate, not a Netflix
mirror target.

PR #1488 is real and non-trivial, but it ships VIF helper
infrastructure; the SpEED extractor is a follow-up landing that
would need both (i) its own PR to add `speed.c` and wire the
registry, and (ii) a model JSON that consumes the features. Neither
exists today. Treating SpEED-VMAF-v3 as imminent would be forecasting
from absence.

## Alternatives explored

- **Wait-and-mirror.** Sit still until Netflix commits `speed.c` to
  `master` + publishes a consuming model, then port 1:1. Rejected
  as the *only* strategy because (a) if the successor is FUNQUE+-
  flavoured it may never come from Netflix at all, and (b) the
  scalar-only upstream implementation has no SIMD/GPU coverage, so a
  port has to happen before the fork can ship it at performance
  parity with other metrics.
- **Speculative pre-port of `speed.c`.** Cherry-pick `1e67d38d` onto
  fork master today, wire the registry, leave the features dead
  until a model consumes them. Rejected for this session: the source
  isn't on upstream `master` yet, so the port would chase a moving
  branch, and the follow-up three commits on `speed_ported`
  (`3c3e8172`, `86d929fc`, + test fixes) clearly anticipate edits
  before landing.
- **Port FUNQUE+ instead.** The open-source FUNQUE+ implementation
  (UT Austin) is the most credible efficient-VMAF thread in the
  literature. Rejected as immediate work but queued as an open
  question (below).

## Open questions

- **Port FUNQUE+?** FUNQUE+ is open-source and publication-backed;
  if Netflix-SpEED never lands, FUNQUE+ is the realistic efficient-
  VMAF succession path. Decide whether the fork wants to own that
  port proactively (and own the SIMD / GPU coverage gap too), or
  only react when upstream picks it up.
- **PR #1488 port timing.** Port when (a) it merges upstream,
  (b) the SpEED follow-up lands, or (c) now? The VIF-helper code is
  independently useful (kernel scaling, lanczos4 resizer) — there's
  a case for a pre-emptive port via `/port-upstream-commit` even
  before SpEED arrives.
- **HDR-VMAF.** Netflix's Nov 2023 blog post described HDR-VMAF as
  an unreleased internal tool. Separate research digest candidate if
  the fork wants to track the HDR direction.
- **"400 fps anomaly"** (signal #5 in the original memory note).
  Independent of SpEED — stays queued for a separate audit once
  lawrence provides the command-line and repro.

## Corrections to the `project_deep_research_netflix_upstream_models`

## memory note (2026-04-21)

Three of the five signals in the memory file were partially or
fully wrong. Leaving this here so the memory can be updated:

1. **"Phone / mobile model gap"** — incorrect. The phone model *is*
   shipped, as a score-transform on `vmaf_v0.6.1.json` invoked via
   `--phone-model`. No separate JSON was ever hidden.
2. **"Unpublished model suspicion / 4k v0.6.1 is evidence of
   more"** — no corroborating artefact. `model/vmaf_4k_v0.6.1.json`
   predates 2024 and is not evidence of a new landing.
3. **"SpEED-driven model landing" inferred from PR #1488** —
   over-read. PR #1488 ships VIF helpers; the SpEED source waits
   three commits later on branch `speed_ported`; no model consumes
   SpEED features in any committed or draft Netflix tree.

Signals 1 (SpEED source exists) and 5 (400 fps anomaly) remain
valid.

## Related

- ADRs: none yet.
- PRs: fork PR #75 (unrelated — tooling), upstream PR #1488
  (this research), upstream PR #1391 (stale still-model), upstream
  PR #1361 (stale FUNQUE resizer).
- Memory: `~/.claude/projects/-home-kilian-dev-vmaf/memory/project_deep_research_netflix_upstream_models.md`.
