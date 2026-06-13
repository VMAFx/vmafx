<!-- markdownlint-disable MD013 -->
# Research-0089 — HDR VMAF model: source / train / placeholder

- **Status**: digest closed; Path C selected (no model file shipped)
- **Date**: 2026-05-09
- **Author**: Lusoris / Claude (autonomous research-then-implement task)
- **Companion ADR**: [ADR-0300](../adr/0300-vmaf-tune-hdr-aware.md)
  carries an inline `### Status update 2026-05-09` section recording
  the Path C outcome.
- **Parent**: [Research-0072](0072-vmaf-tune-hdr-aware.md) flagged the
  HDR VMAF model port as deferred backlog from Bucket #9 of the PR
  #354 capability audit.

## TL;DR

PR #477's TransNet+HDR work shipped the registration slot + transfer-aware
resolver for `model/vmaf_hdr_*.json` but **no HDR VMAF model file
exists today** in this fork. This digest evaluated three paths to close
that gap:

- **Path A (source from elsewhere)**: exhausted with negative findings.
  Netflix's HDR VMAF model has never been publicly released; the
  most recent authoritative statement (Netflix collaborator `li-zhi`
  on issue #645, plus CSI Magazine 2023-11-30) is "no timeline."
- **Path B (train a fork-owned model)**: infeasible in the PR window
  — the only relevant subjective HDR corpora (LIVE-HDR, LIVE-HDRvsSDR,
  LIVE-TMHDR) are gated behind manual access forms and have unclear
  redistribution-of-derived-weights terms; training would also require
  multi-day compute that exceeds the autonomous task budget.
- **Path C (degrade gracefully + document)**: chosen. Ship a
  `model/vmaf_hdr_README.md` placeholder that loudly warns the
  HDR-fallback path is in effect and points users at this digest.
  No fabricated weights are added to the diff.

## Path A: verbatim verification trail (access date 2026-05-09)

### Upstream Netflix/vmaf model directory

`gh api repos/Netflix/vmaf/contents/model` (2026-05-09) returns the
following filenames — note **no `vmaf_hdr_*` entry**:

```text
other_models, vmaf_4k_rb_v0.6.2, vmaf_4k_v0.6.1.json,
vmaf_4k_v0.6.1neg.json, vmaf_b_v0.6.3.json,
vmaf_float_4k_v0.6.1.json, vmaf_float_b_v0.6.3.json,
vmaf_float_b_v0.6.3, vmaf_float_v0.6.1.json,
vmaf_float_v0.6.1.pkl, vmaf_float_v0.6.1.pkl.model,
vmaf_float_v0.6.1neg.json, vmaf_float_v0.6.1neg.pkl,
vmaf_float_v0.6.1neg.pkl.model, vmaf_rb_v0.6.2,
vmaf_rb_v0.6.3, vmaf_v0.6.1.json, vmaf_v0.6.1neg.json
```

### Netflix issue #645 — "Did the HDR model ever get released?"

URL: <https://github.com/Netflix/vmaf/issues/645> (access date
2026-05-09). The thread's authoritative reply is from Netflix
collaborator `li-zhi`:

> The HDR model is still under development. We currently do not have
> a timeline for when it is going to be open sourced.

No subsequent Netflix-author comment supersedes this.

### CSI Magazine — "Netflix reveals HDR-VMAF solution" (2023-11-30)

URL: <https://www.csimagazine.com/csi/netflix-reveals-hdrvmaf-solution.php>
(access date 2026-05-09). Quote captured from page body:

> committed to making HDR-VMAF available to the open source
> community … current implementation is "largely tailored to our
> internal pipelines" and "has some algorithmic limitations that we
> are in the process of improving before the official release."

No download URL, no license declaration, no estimated release date.

### Netflix Tech Blog — "All of Netflix's HDR video streaming is now

dynamically optimized" (2023-11-29)

URLs (access date 2026-05-09):
<https://netflixtechblog.com/all-of-netflixs-hdr-video-streaming-is-now-dynamically-optimized-e9e0cb15f2ba>
and
<https://research.netflix.com/publication/all-of-netflixs-hdr-video-streaming-is-now-dynamically-optimized>.
Discusses production deployment of HDR-DO encoding by June 2023; does
not reference an open-source model artifact, license, or download
location.

### Hugging Face Hub

`https://huggingface.co/api/models?search=vmaf` (access date
2026-05-09) returns `[]` — there are no VMAF-related models on
Hugging Face at all, HDR or otherwise.

### GitHub code search

`gh search code "vmaf_hdr"` and
`gh search code "vmaf_hdr_v0.6.1"` (2026-05-09) returned exactly
one hit (a third-party docs file referencing Netflix's prototype),
no model-weight files. No fork or mirror of Netflix/vmaf carries
the artifact.

### HDRMAX (UT Austin LIVE Lab)

URL: <https://github.com/utlive/HDRMAX> (access date 2026-05-09).
HDRMAX is a different algorithm — it implements HDR-VMAF /
HDR MS-SSIM / HDR-SSIM as scikit-learn SVR models trained over
features computed via a local-expansive-nonlinearity transform,
**not the libvmaf adm/vif/motion feature stack**. Concrete model
files shipped:

- `models/svr/model_svr_livehdr.pkl` (58 033 bytes)
- `models/svr/model_svr_liveaq.pkl` (92 887 bytes)
- `models/scaler/model_scaler_livehdr.pkl` (1 049 bytes)
- `models/scaler/model_scaler_liveaq.pkl` (1 026 bytes)

License: MIT-style (BSD-3-Clause-Plus-Patent compatible). **But
these are not Netflix-VMAF-JSON-loadable** — libvmaf's JSON loader
expects libsvm-format SV strings inside `model_dict.model`, and
the feature columns in HDRMAX (HDR-luma-statistics features) do not
correspond to the adm2 / vif_scale0..3 / motion2 columns the libvmaf
predict path computes. Adopting HDRMAX would require a separate
metric pipeline, not a JSON drop-in.

### Industry partners

`Synamedia/Quortex`, `MainConcept`, `Dolby` — Dolby co-developed
the first HDR-VMAF iteration with Netflix (2021); no public model
artifact has been released by either party. Synamedia and
MainConcept ship proprietary HDR quality measures behind commercial
licenses (incompatible with BSD-3-Clause-Plus-Patent shipping).
Verified via web search (2026-05-09); negative finding.

### Internet Archive / Wayback

`web.archive.org` is firewalled from the harness's WebFetch tool;
the cached versions of `github.com/Netflix/vmaf/tree/master/model`
across 2020-2025 (sampled previously) all show the same SDR-only
file set as today.

### Path A verdict

No publicly-released, BSD-3-Clause-Plus-Patent-compatible,
libvmaf-JSON-loadable HDR VMAF model exists anywhere on the
public web as of 2026-05-09. Path A is a confirmed dead end.

## Path B: feasibility evaluation

### Subjective HDR corpora surveyed

| Corpus | URL | License | Access | Verdict |
| --- | --- | --- | --- | --- |
| LIVE-HDR (compression) | `live.ece.utexas.edu/research/LIVEHDR/LIVEHDR_index.html` | Custom academic — "permission to use, copy, modify, distribute" + attribution; silent on derived-model redistribution | Google Form gating | Cannot autonomously obtain; redistribution-of-derived-weights status is **unclear** for BSD-3 shipping |
| LIVE-HDRvsSDR | `live.ece.utexas.edu/research/LIVE_HDRvsSDR/index.html` | Same as above | Google Form gating | Same caveats |
| LIVE-TMHDR (tone-mapped) | `live.ece.utexas.edu/research/LIVE_TMHDR/index.html` | Same as above | Google Form gating | Tone-mapping is not the target use case |
| ESPL-LIVE HDR | `live.ece.utexas.edu/research/HDRDB/hdr_index.html` | Same as above | Google Form gating | Image database, not video |
| ITU-T SDR-vs-HDR test sets | (no public download identified) | Unclear | Negative finding | Not a viable autonomous source |
| Disney Research HDR | (no public release identified) | Proprietary | Negative finding | Not a viable autonomous source |

All five candidates either (a) require human-in-the-loop access
forms, (b) have ambiguous derived-weight-redistribution terms for
BSD-3-Clause-Plus-Patent shipping, or (c) are not publicly available
at all. The fork carries **no local HDR-MOS-labelled training corpus**
today; the Konvid/BVI/DVC corpora referenced in
[`docs/state.md`](../state.md) are SDR.

### Compute budget

Even with a corpus in hand, training a libvmaf-format SVR (nu_svr +
RBF, ~211 support vectors per the `vmaf_v0.6.1.json` precedent) over
a multi-thousand-clip HDR corpus is a multi-day grid-search over
`C` / `nu` / `gamma`, plus held-out PLCC/SROCC validation, plus
ONNX-Runtime cross-check if we want a tiny-AI sibling. This exceeds
the single-shot autonomous task budget.

### Path B verdict

Infeasible in this PR window. Filed as a follow-up backlog item
(`docs/state.md` row added in this PR; reopens once the fork has
both a permissively-licensed HDR corpus on disk and a deliberate
multi-day training slot).

## Path C: degrade gracefully + document (chosen)

### Implementation

1. Drop a `model/vmaf_hdr_README.md` file. It is **not** a JSON
   model and does **not** match the `vmaf_hdr_*.json` glob, so
   `select_hdr_vmaf_model` continues to return `None` and the
   harness keeps falling back to the SDR model — exactly as it
   does today. Goal of the file: make the fallback discoverable
   to anyone listing `model/`.
2. Ship a `model/vmaf_hdr_model_card.md` carrying a loud warning
   that **HDR scoring on this fork falls back to the SDR-trained
   `vmaf_v0.6.1.json` weights**, with a citation back to this
   research digest and ADR-0300.
3. Append `### Status update 2026-05-09: HDR model status` to
   ADR-0300, recording that the autonomous source-or-train pass
   resolved to Path C and listing the surveyed sources.
4. Update `docs/state.md` with a new "Open / deferred" row tracking
   the HDR-VMAF-model gap as confirmed infeasible until either
   Netflix open-sources the artifact or a permissively-licensed
   corpus becomes available.
5. Add a `changelog.d/added/hdr-vmaf-model-search.md` fragment
   per ADR-0221 recording the negative-research outcome.

### Crucially: no fabricated weights

This PR ships **zero JSON model files** under `model/`. There is
no synthetic SVR, no copy of an SDR model renamed `vmaf_hdr_*.json`,
no placeholder weights. The fallback that occurs at runtime today
(silent SDR-model selection plus a one-shot warning per
ADR-0300) is unchanged; this PR only adds discoverable
documentation that the fallback is in effect, plus the negative-
research record.

### Fallback warning text shipped

Lifted verbatim from the model card committed in this PR:

> ## WARNING — HDR fallback is the SDR model
>
> This fork does **not** ship a fork-owned HDR-trained VMAF
> model. When `vmaftune.hdr.select_hdr_vmaf_model()` is called
> against `model/`, it returns `None`, and the harness falls back
> to scoring HDR sources with the SDR-trained `vmaf_v0.6.1.json`
> weights. This produces VMAF scores that **trend artificially
> low** for high-luminance regions of PQ / HLG content — the SDR
> model's RBF SVR was never exposed to the PQ EOTF or BT.2020
> primaries during training.
>
> Quantitative impact (paraphrased from
> [research-0072](../docs/research/0072-vmaf-tune-hdr-aware.md)):
> empirical SDR-vs-HDR-VMAF deltas have **not been characterised
> on this fork**. Treat any VMAF score over a PQ / HLG source as
> a **lower bound only** until either a fork-owned HDR model lands
> or Netflix releases their HDR artifact. Do not use the score to
> pick CRFs at high quality targets (≥ 90 VMAF) on HDR encodes.

The model card also carries a "How to verify which model the
harness used" snippet so users can confirm the fallback after
the fact, plus pointers to Path A's verbatim trail above for any
future contributor evaluating whether the upstream situation has
changed.

## Follow-up backlog

1. **Re-run Path A periodically.** Watch `Netflix/vmaf/model/` and
   issue #645; if Netflix ships `vmaf_hdr_*.json` upstream, port it
   via `/port-upstream-commit` (the resolver picks it up
   automatically — no `vmaftune` change needed).
2. **Path B if the fork acquires an HDR corpus.** Once a
   permissively-licensed HDR-MOS-labelled video corpus lands in the
   fork (e.g. via the Konvid / BVI / DVC ingestion paths in
   [ADR-0310](../adr/0310-bvi-dvc-corpus-ingestion.md)), schedule
   a dedicated multi-day training run and lift the
   `model/vmaf_hdr_*.json` slot from a `None` resolver result to a
   concrete artifact.
3. **Empirical SDR-on-HDR delta measurement** as a smaller follow-up
   that does not need HDR MOS labels — score a controlled set of PQ
   / HLG clips with the SDR fallback, compare against the same clips
   tone-mapped to BT.709 + scored on SDR, report mean and worst-case
   VMAF delta. This characterises the magnitude of the bias the
   model card warns about, and is feasible without the gated
   subjective corpora.

## References

- [Issue #645 — "Did the HDR model ever get released?"](https://github.com/Netflix/vmaf/issues/645) — accessed 2026-05-09; collaborator `li-zhi` confirmed "no timeline."
- [CSI Magazine 2023-11-30 article](https://www.csimagazine.com/csi/netflix-reveals-hdrvmaf-solution.php) — accessed 2026-05-09; "before the official release."
- [Netflix research blog post (2023-11-29)](https://research.netflix.com/publication/all-of-netflixs-hdr-video-streaming-is-now-dynamically-optimized) — accessed 2026-05-09; production deployment, no model artifact.
- [HDRMAX (utlive/HDRMAX)](https://github.com/utlive/HDRMAX) — accessed 2026-05-09; different algorithm, sklearn pickles, not libvmaf-JSON-loadable.
- [LIVE-HDR](https://live.ece.utexas.edu/research/LIVEHDR/LIVEHDR_index.html) — accessed 2026-05-09; gated, redistribution-of-derived-weights unclear.
- Parent: [Research-0072 — vmaf-tune HDR-aware](0072-vmaf-tune-hdr-aware.md).
- Companion: [ADR-0300 — vmaf-tune HDR-aware](../adr/0300-vmaf-tune-hdr-aware.md).
- Source: `req` — task brief: "source or train an HDR VMAF model … if shipping a placeholder, the model card MUST loudly warn users."
