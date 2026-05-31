<!-- markdownlint-disable MD060 -->
# Research-0061: `vmaf-tune` capability audit — beyond Phases A/B + the fast path

- **Status**: Active
- **Workstream**: ADR-0237 (`vmaf-tune` umbrella), ADR-0235 (codec-aware FR regressor)
- **Last updated**: 2026-05-03
- **Author**: research scoping pass (no code)

## Question

`vmaf-tune` Phase A (corpus tooling, PR #329 merged) plus Phase B
(`fr_regressor_v2` codec-aware proxy, in flight via PR #347) plus the
fast-path proposal (proxy + Bayesian + GPU verify, parallel PR being
scaffolded) cover roughly:

- offline grid sweeps with JSONL emission,
- target-VMAF bisect,
- predicted-CRF jump-start ≈ 100–500× speedup over naive search.

**What else can `vmaf-tune` do** with the primitives the fork already
ships? This digest surveys 18 capability buckets, ranks them by
expected impact divided by effort, and recommends an execution order
for Phases C–H.

This is a pure scoping pass. No implementation, no benchmarks, no
new code. Numbers in this document are back-of-envelope; any speedup
or quality claim that survives into an ADR has to be re-validated
against a real corpus.

## Existing fork primitives (the library this audit shops from)

| Primitive | Path | Audit relevance |
|---|---|---|
| Codec adapter interface | `tools/vmaf-tune/src/vmaftune/codec_adapters/` (x264 only today) | Every multi-codec bucket assumes more adapters land here |
| `vmaf_tiny_v{1..4}` proxy regressors | `model/tiny/vmaf_tiny_v*.onnx` | Cheap "what would VMAF be?" estimator (canonical-6 features) |
| `fr_regressor_v1`, scaffold for `_v2` | `model/tiny/fr_regressor_v1.onnx` (+ ADR-0235) | Codec-aware FR proxy — drives the fast path |
| `nr_metric_v1` (NR proxy) | `model/tiny/nr_metric_v1.onnx` | Source-side complexity / quality without a reference |
| `transnet_v2` shot detector | `model/tiny/transnet_v2.onnx`, ADR-0223 | Per-shot segmentation for Phase D / per-shot tuning |
| `mobilesal` (saliency placeholder) | `model/tiny/mobilesal.onnx` | ROI weighting for saliency-aware tuning (real-weights swap deferred per ADR-0257) |
| Canonical-6 source features | `ai/src/vmaf_train/canonical6.py` | Source descriptor that all per-title predictors consume |
| FFmpeg patches with libvmaf filter | `ffmpeg-patches/0001-…` through `0006-…` | In-process scoring during encode-search |
| `vf_libvmaf_vulkan` zero-copy | `core/src/vulkan/`, ADR-0186 | GPU verify path with no PCIe round-trip |
| Vulkan / CUDA / SYCL VMAF backends | `core/src/{cuda,sycl,vulkan}/` | Verify-side acceleration |
| MCP server | `mcp-server/vmaf-mcp/` | Phase F surface — agent-callable tools |
| `--precision` flag (lossless floats) | `core/tools/cli_parse.c` | Pareto-frontier needs IEEE-round-trip JSONL |

The audit's premise: **most of the 18 buckets are 80% built already
in the metric stack**; what's missing is the orchestration layer in
`vmaf-tune`.

## Per-bucket capability survey

Effort key: **S** = ≤ 1 week, **M** = 1–3 weeks, **L** = ≥ 1 month.
Impact key: **Low**, **Medium**, **High**, **Game-changer** (= reshapes
what a producer can ship).

### Bucket 1 — Per-shot CRF tuning (Phase C as written)

- **Summary**: Use `transnet_v2` to cut the source into shots, run
  Phase B's bisect/proxy per shot, emit `--qpfile` (x264) /
  `--zones` (x265) / SVT-AV1 segment table.
- **Existing primitives**: `transnet_v2.onnx`, codec adapter
  `emit_per_shot_overrides()` hook (already declared in ADR-0237),
  Phase B bisect.
- **Effort**: **M** — shot-aware orchestration + per-codec override
  emission; non-trivial because shot-boundary frames near GOP edges
  need encoder-specific handling.
- **Impact**: **High** — Netflix's per-shot encoding is the canonical
  reference; same-VMAF bitrate savings of 10–30% are the public
  numbers from their 2018 paper.
- **Open**: do we re-train the proxy on per-shot canonical-6, or is
  the per-title proxy "good enough" if features are computed per
  shot? (Hypothesis: per-title proxy generalises if features are
  recomputed; needs a held-out check on BVI-DVC.)
- **Already in roadmap**: yes — Phase D in ADR-0237, gated on T6-3b
  per-shot CRF predictor.

### Bucket 2 — Saliency-aware ROI tuning

- **Summary**: Run MobileSal (or a fork-trained student) per frame,
  weight CRF toward salient regions; emit per-tile or per-MB QP
  offsets.
- **Existing primitives**: `mobilesal.onnx` (placeholder; ADR-0257
  defers real-weights swap), `vmaf_roi` precedent.
- **Effort**: **L** — needs a saliency-to-QP-offset mapping table,
  per-codec MB/CTU/SB QP offset emission, and the saliency model is
  still a placeholder.
- **Impact**: **Medium** — saliency-aware encoding is a well-studied
  but rarely productionised win; gains are real (5–15% bitrate at
  same VMAF) but the encoder integration is finicky.
- **Open**: which codec exposes the cleanest per-region QP API
  (x264 `--zones` is frame-range, not spatial)? Does VMAF *itself*
  reward saliency-weighted distortion? (VMAF-NEG variant exists.)
- **Roadmap status**: not in ADR-0237; would be a new "Phase G:
  saliency-aware".

### Bucket 3 — Two-pass complexity-aware

- **Summary**: Pass 1 = cheap proxy estimates per-frame complexity
  (canonical-6 + `nr_metric_v1`); pass 2 = budget bitrate where the
  proxy says it matters.
- **Existing primitives**: canonical-6 extractor, `nr_metric_v1`,
  FFmpeg's native 2-pass mode (codec-side rate control).
- **Effort**: **M** — the harness has to plug into FFmpeg's
  `-pass 1`/`-pass 2` flow and inject a learned λ per macroblock or
  per-frame.
- **Impact**: **Medium** — overlaps with codec-internal AQ
  (adaptive quantisation); marginal gain over a tuned `--aq-mode`.
- **Open**: is there meaningful headroom over x265's
  `--aq-mode 4`/x264's psy-trellis when both are well-tuned?
  (Suspect: small for x264/x265, larger for SVT-AV1 where the
  default AQ is less aggressive.)
- **Roadmap status**: not in ADR-0237; modest priority.

### Bucket 4 — Bitrate-budget mode

- **Summary**: Given target bitrate `B`, find encoding parameters
  that maximise VMAF subject to bitrate ≤ B (± slack). Inverse of
  Phase B.
- **Existing primitives**: Phase A grid + Phase B bisect +
  `fr_regressor_v2` proxy.
- **Effort**: **S** — the same bisect machinery, just different
  predicate (bisect on CRF where bitrate ≈ B, then read the
  resulting VMAF).
- **Impact**: **High** — every CDN / streaming engineer sizes by
  bitrate budget first, quality second. This is the most-asked-for
  feature in encoding ops.
- **Open**: how to handle bitrate elasticity (a CRF that hits 5 Mbps
  on shot A may overshoot on shot B); same per-shot story as
  Bucket 1.
- **Roadmap status**: not in ADR-0237; **trivially layered onto
  Phase B** if we generalise the predicate from "VMAF ≥ T" to
  "bitrate ≤ B" or "any user predicate".

### Bucket 5 — Quality-floor mode

- **Summary**: Given minimum VMAF target `T`, find smallest bitrate
  hitting it. The "achieve quality with minimum bytes" framing.
- **Existing primitives**: same as Bucket 4 — Phase B's bisect runs
  inverted.
- **Effort**: **S** — already implicit in Phase B; just needs a
  user-facing flag (`--target-vmaf`, exists; `--minimise bitrate`,
  doesn't).
- **Impact**: **High** — symmetrical to Bucket 4; together they
  cover the two operational modes producers actually use.
- **Open**: what's the convergence shape — do we need golden-section
  search, or is plain bisect on CRF enough? (Plain bisect is
  monotone in CRF, so yes.)
- **Roadmap status**: arguably the *default* mode of Phase B; just
  needs explicit framing in docs.

### Bucket 6 — Bitrate-ladder optimisation (Phase E)

- **Summary**: Per-title across resolutions (240p / 480p / 720p /
  1080p / 2160p). Build (bitrate, VMAF) convex hull, pick knees,
  emit DASH/HLS manifest.
- **Existing primitives**: Phase B bisect + scaling pipeline (FFmpeg
  `-vf scale`).
- **Effort**: **L** — convex-hull pruning + Pareto computation +
  manifest emission (DASH MPD / HLS m3u8 templates) + per-resolution
  encoder reconfiguration.
- **Impact**: **Game-changer** — this is *the* Netflix per-title
  feature; turning the fork into a "per-title ladder generator"
  is the single biggest product-level differentiator.
- **Open**: do we ship a manifest generator, or stop at
  `(resolution, bitrate, vmaf)` JSON and let the user wire to
  Bento4/Shaka? (Prefer: stop at JSON; manifest tooling is a
  separate concern.)
- **Roadmap status**: yes — Phase E in ADR-0237.

### Bucket 7 — Codec-comparison mode

- **Summary**: Same source, fixed VMAF target, sweep
  {x264, x265, SVT-AV1, libaom, libvvenc}; report smallest file +
  encode time per codec.
- **Existing primitives**: codec adapter interface (x264 wired;
  rest gated on adapter PRs).
- **Effort**: **M** — the orchestration is trivial once 4–5 adapters
  exist; effort lives in writing each adapter (~3 days each).
- **Impact**: **High** — answers the perennial "should I migrate to
  AV1 yet?" question per-source, not per-marketing-deck.
- **Open**: how to normalise encode-time across codecs (real-time
  factor on what hardware)? Need a fixed reference machine claim
  in the report.
- **Roadmap status**: not explicit in ADR-0237 phases, but falls
  out free once 3+ adapters land.

### Bucket 8 — Resolution-aware tuning

- **Summary**: VMAF's display-size model varies by viewing distance
  (`vmaf_v0.6.1neg` vs `vmaf_4k_v0.6.1`). Pick the right model for
  the rendition's display target.
- **Existing primitives**: existing VMAF model registry
  (`model/vmaf_*.json`).
- **Effort**: **S** — flag to select VMAF model per rendition;
  documentation work.
- **Impact**: **Medium** — invisible until you ship a 4K rendition
  scored against the 1080p model and notice the numbers are
  optimistic.
- **Open**: should we *automatically* pick the model from output
  resolution, or require user to specify? (Auto-pick + override
  flag.)
- **Roadmap status**: not in ADR-0237; falls into Phase E
  preliminaries.

### Bucket 9 — HDR-aware tuning

- **Summary**: HDR content needs HDR-VMAF / SVT-AV1 HDR mode and
  PQ/HLG-aware encoding parameters (`--master-display`,
  `--max-cll`, `--colorprim bt2020`).
- **Existing primitives**: libvmaf supports HDR scoring
  (PQ transfer); the encoders all have HDR flags.
- **Effort**: **M** — HDR metadata propagation through the harness
  HDR-specific VMAF model selection + per-codec HDR flag plumbing.
- **Impact**: **Medium** — narrow audience (HDR producers) but the
  audience is high-value (premium content).
- **Open**: do we re-train the proxy on HDR sources? (Suspect: yes;
  canonical-6 likely doesn't transfer cleanly across PQ/HLG/SDR.)
- **Roadmap status**: not in ADR-0237; defer until corpus contains
  HDR sources.

### Bucket 10 — Encode-time vs quality trade-off curves

- **Summary**: Per source: how much slower is `slow` vs `medium` for
  the same VMAF? If `medium` is within 0.5 VMAF of `slow` at 30%
  the time, recommend `medium`.
- **Existing primitives**: Phase A grid (already sweeps presets).
- **Effort**: **S** — pure post-processing on the existing JSONL
  corpus; emit a "preset recommendation" table.
- **Impact**: **Medium** — useful for ops teams sizing encode farms;
  the analysis is a 200-line Python script.
- **Open**: is the recommendation source-dependent, or are there
  universal preset-vs-quality trade-offs? (Suspect: source-dependent
  but with strong content-class clustering.)
- **Roadmap status**: not in ADR-0237; trivial bolt-on after Phase A
  corpora exist.

### Bucket 11 — Live / low-latency mode

- **Summary**: Real-time CRF adaptation based on a 1-second
  look-back proxy estimate. Live encoding instead of offline.
- **Existing primitives**: `nr_metric_v1` (NR — no reference needed);
  Vulkan zero-copy import (low latency).
- **Effort**: **L** — live mode is a different deployment shape
  (streaming pipeline, not file-by-file); needs a daemon + IPC + GOP-
  aligned re-config.
- **Impact**: **Medium** — interesting for live streaming but
  outside the fork's current "offline analysis" centre of gravity.
- **Open**: is the 1-second look-back actionable for a 2-second
  GOP encoder? (Suspect: marginally; sub-GOP CRF changes are
  typically rejected by encoders.)
- **Roadmap status**: not in ADR-0237; deferred indefinitely.

### Bucket 12 — CMAF segment tuning

- **Summary**: For HLS/DASH streaming, tune each CMAF segment's
  bitrate independently (different from per-shot — segments are
  fixed-duration, e.g., 2 s).
- **Existing primitives**: codec-side `-force_key_frames`, FFmpeg
  segment muxer.
- **Effort**: **M** — overlaps with per-shot tuning but with
  fixed-duration boundaries that may mid-shot.
- **Impact**: **Medium** — useful for ABR streaming but per-shot
  (Bucket 1) typically dominates; segment-aligned tuning is a
  refinement, not a replacement.
- **Open**: does segment-aligned + shot-aligned compose, or do they
  fight (mid-shot keyframe at segment boundary)? (Suspect: the
  encoder forces a keyframe at the segment boundary regardless;
  per-shot CRF still wins for the within-segment frames.)
- **Roadmap status**: not in ADR-0237; Phase E adjacent.

### Bucket 13 — Cross-codec proxy consistency check

- **Summary**: Audit whether `fr_regressor_v2` predicts VMAF equally
  well across codecs. If x264 PLCC is 0.99 but VVC PLCC is 0.85,
  add a per-codec head.
- **Existing primitives**: corpus from Phase A, ADR-0235's six-bucket
  codec one-hot.
- **Effort**: **S** — pure offline analysis once multi-codec
  corpora exist; produce a per-codec PLCC/SROCC table; feeds back
  into ADR-0235's training plan.
- **Impact**: **Medium** — *required* for trust; if the proxy is
  unreliable on AV1 we silently mis-tune AV1 encodes. This is
  governance, not user-facing.
- **Open**: what PLCC threshold triggers a per-codec head? (Suspect:
  per-codec PLCC < 0.97 → head; ≥ 0.97 → shared trunk is fine.)
- **Roadmap status**: implicit in Phase B's validation gate;
  should be promoted to an explicit deliverable.

### Bucket 14 — Bayesian-optimised preset selection

- **Summary**: Beyond CRF, jointly optimise preset (slow / medium
  / fast / ...) against quality + time using
  `scikit-optimize`/`hyperopt`.
- **Existing primitives**: Phase A grid; Bayesian opt is mentioned
  as opt-in in Research-0044.
- **Effort**: **S** — `scikit-optimize` is a single dep; the search
  space is small (5–8 presets × CRF range).
- **Impact**: **Medium** — strict super-set of Bucket 10; the
  Bayesian framing handles non-monotonic preset effects but
  coordinate-descent gets you 90% of the way.
- **Open**: is BO worth a dep over coordinate descent for a
  ≤ 12-point space? (Suspect: marginal; useful if the space grows
  to include `--ref`, `--bf`, `--rc-lookahead`.)
- **Roadmap status**: optional; ADR-0237 already names it as
  Phase B+ opt-in.

### Bucket 15 — Multi-objective Pareto

- **Summary**: Minimise (bitrate, encode_time) subject to
  VMAF ≥ T. Emit Pareto frontier so the user picks their own
  trade-off.
- **Existing primitives**: Phase A grid + standard Pareto pruning.
- **Effort**: **M** — Pareto pruning is 50 lines; UX (how to
  *present* a frontier) is most of the work.
- **Impact**: **High** — directly addresses the "I want the best
  trade-off, not a single answer" producer mindset.
- **Open**: 2-D (bitrate, time) or 3-D (bitrate, time, quality
  variance)? (Start 2-D; add dimensions only if asked.)
- **Roadmap status**: not in ADR-0237; companion to Phase E
  ladder generation.

### Bucket 16 — Adaptive seeding (content-fingerprint → seed CRF)

- **Summary**: Build a database of `(canonical-6 fingerprint, codec)
  → recommended starting CRF`. New source → nearest-neighbour
  lookup → start the bisect from a smart seed instead of
  midpoint.
- **Existing primitives**: canonical-6, the corpus from Phase A.
- **Effort**: **S** — once Phase A corpus has ≥ 1k rows, k-NN +
  median CRF is 30 lines.
- **Impact**: **Medium** — saves 1–2 bisect iterations on average,
  worth ~30% wall time on the hot path. Compounds with the
  fast-path.
- **Open**: do we ship the fingerprint DB in-tree, or as a
  downloadable artefact? (Suspect: artefact, since it grows with
  the corpus.)
- **Roadmap status**: not in ADR-0237; natural Phase C+ refinement.

### Bucket 17 — Online learning / bandit

- **Summary**: When production decisions stream in (CRF → VMAF →
  bitrate), update the proxy. A contextual bandit / Bayesian update.
- **Existing primitives**: `fr_regressor_v2` weights are a thin MLP;
  online retraining is feasible.
- **Effort**: **L** — online retraining infra (incremental fit,
  drift detection, rollback, model-card bookkeeping per ADR-0042).
- **Impact**: **Low–Medium** — the proxy is already 100×+ faster
  than ground-truth VMAF; marginal gain on PLCC won't shift the
  user-facing speedup.
- **Open**: who *owns* the streaming corpus — the user, or the
  fork? (Probably user-owned; the fork ships the training script,
  not the data.)
- **Roadmap status**: not in ADR-0237; deferred indefinitely.

### Bucket 18 — Quality-scoring uncertainty

- **Summary**: Proxy emits `(VMAF_pred, σ)` instead of `VMAF_pred`.
  Producer can ask `P(VMAF ≥ 92) ≥ 0.95` instead of
  `VMAF_pred ≥ 92`.
- **Existing primitives**: any of MC dropout, deep ensembles,
  Laplace approximation, conformal prediction — all bolt onto a
  trained MLP.
- **Effort**: **M** — re-train (or post-hoc calibrate) the proxy
  to emit a calibrated standard deviation; surface a CLI flag
  (`--quality-confidence 0.95`).
- **Impact**: **High** — risk-tolerance framing matches how
  producers actually think (SLA = "VMAF ≥ 90 on 99% of titles");
  point-estimate framing forces them to pad CRFs by hand.
- **Open**: which uncertainty method calibrates well on tiny MLPs?
  (Suspect: deep ensembles of 5 cheap proxies + conformal post-hoc.
  Conformal needs a held-out calibration set.)
- **Roadmap status**: not in ADR-0237; new "Phase H: probabilistic
  proxy".

## Top-5 ranking by impact / effort

Methodology: scored each bucket on a 1–4 impact axis
(Low=1 / Medium=2 / High=3 / Game-changer=4) and a 1–3 effort axis
(S=1 / M=2 / L=3); ranked by impact ÷ effort. Ties broken by
"shipping the underlying primitive is already in flight".

| Rank | Bucket | Impact | Effort | Score | Reasoning |
|---|---|---|---|---|---|
| 1 | **#5** Quality-floor mode | High (3) | S (1) | 3.0 | Already implicit in Phase B; ship the docs + flag, done |
| 1 | **#4** Bitrate-budget mode | High (3) | S (1) | 3.0 | Symmetric to #5; same orchestration, different predicate |
| 3 | **#6** Bitrate-ladder (Phase E) | Game-changer (4) | L (3) | 1.33 | The *single* biggest product differentiator; effort dominated by manifest tooling we can stop short of |
| 4 | **#1** Per-shot CRF (Phase D) | High (3) | M (2) | 1.5 | Already roadmap; gated on T6-3b which is in flight |
| 5 | **#18** Probabilistic proxy | High (3) | M (2) | 1.5 | Risk-tolerance framing; reshapes the producer UX |

(Bucket #16 *adaptive seeding* and #10 *encode-time curves* tie at
2.0 score but with Low/Medium impact respectively — they're
"polish-grade" and listed below the cut-line.)

## Recommended execution order

1. **Phase B'** (already in flight, finish first) — fast-path PR
   lands `fr_regressor_v2` + Bayesian + GPU verify.
2. **Phase B docs follow-up** (≤ 1 week) — ship Buckets #4 + #5
   explicitly as `--target-bitrate` and `--target-vmaf` modes;
   trivial flag work, big perceived feature add. *Highest impact
   ÷ effort in the audit.*
3. **Phase D** (≈ 3 weeks) — per-shot CRF tuning per ADR-0237 D;
   gated on T6-3b. Adds the "Netflix per-shot" table-stakes feature.
4. **Phase E** (≈ 4 weeks) — bitrate ladder + 2-D Pareto frontier
   (Buckets #6 + #15 together, since Pareto pruning *is* how the
   ladder knees are picked).
5. **Phase H** (≈ 2 weeks) — probabilistic proxy (Bucket #18);
   re-train `fr_regressor_v2` with deep ensemble + conformal head;
   surface `--quality-confidence`.
6. **Adapter expansion** runs in parallel as a "one PR per codec"
   stream — x265 → SVT-AV1 → libaom → libvvenc. Unlocks Bucket #7
   (codec-comparison) for free.

Buckets #2 (saliency), #9 (HDR), #11 (live), #17 (online learning)
are explicitly **deferred** — each requires either a model swap, a
deployment-shape change, or a corpus the fork doesn't own.
Bucket #13 (cross-codec consistency) gets folded into Phase B's
validation gate as a CI artefact.

## Game-changer candidate

**Bucket #6 (bitrate-ladder optimisation, Phase E)** is the single
capability that re-frames the fork's product position. Today the
fork is "the best open-source VMAF measurement stack". With Phase E,
the fork becomes "the only open-source per-title-ladder generator
with a measured-PLCC proxy". That's a category move, not a feature
add. Effort is **L** but the work decomposes cleanly: (a) Phase B
already produces (CRF, bitrate, VMAF) tuples; (b) per-resolution
sweep is `(scale → encode → score)` looped 5× over standard
ladders; (c) convex-hull pruning is 50 lines; (d) manifest
generation is the only large new surface and can be deferred to a
follow-up PR.

## Biggest blocker

**Codec adapter coverage.** Buckets #7, #6 (multi-resolution × multi-
codec ladders), #9 (HDR), #15 (Pareto across codecs) all degrade to
"x264 only" until x265 / SVT-AV1 / libaom / libvvenc adapters land.
Each adapter is an S-effort PR but five of them on the critical
path is 5 weeks calendar. Recommendation: open the adapter-stream
*now*, in parallel with Phase D, so the multi-codec capabilities
don't all stack at the end.

## What's explicitly out of scope

- **Netflix-internal data / services** — the user rule. Anything
  needing Netflix's Mosaic corpus, internal models, or the dynamic-
  optimiser λ table is rejected by construction.
- **Promised speedup numbers** — every "X×" claim above is a
  back-of-envelope hypothesis. Real numbers come from the Phase B
  validation corpus once it exists.
- **Implementation work** — this is a scoping pass; nothing here
  becomes code until the corresponding ADR opens.

## References

- [ADR-0237](../adr/0237-quality-aware-encode-automation.md) —
  `vmaf-tune` umbrella + Phase A/B/C/D/E/F roadmap.
- [Research-0044](0044-quality-aware-encode-automation.md) — the
  original option-space digest for `vmaf-tune` (this document
  extends it).
- [ADR-0235](../adr/0235-codec-aware-fr-regressor.md) — codec-aware
  FR regressor v2 (Phase B's proxy).
- [ADR-0223](../adr/0223-transnet-v2-shot-detector.md) — shot
  detector (Phase D dependency).
- [ADR-0257](../adr/0257-mobilesal-real-weights-deferred.md) —
  saliency real-weights deferred (Bucket #2 dependency).
- Source: user request 2026-05-03 — *"audit the vmaf-tune capability
  surface beyond Phase A + Phase B + the fast-path"* (paraphrased
  per CLAUDE.md user-quote rule).
