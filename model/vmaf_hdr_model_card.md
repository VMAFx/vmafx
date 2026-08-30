# HDR VMAF model — fork status as of 2026-05-09

This file is **discoverable documentation, not a model**. The fork's
HDR VMAF resolver
([`select_hdr_vmaf_model`](../tools/vmaf-tune/src/vmaftune/hdr.py))
globs `vmaf_hdr_*.json` under `model/`. This file deliberately uses a
`.md` extension so the resolver continues to return `None`, and the
harness keeps falling back to the SDR model. See
[ADR-0300](../docs/adr/0300-vmaf-tune-hdr-aware.md) and
[research-0089](../docs/research/0089-hdr-vmaf-model-search.md).

## WARNING — HDR fallback is the SDR model

This fork does **not** ship a fork-owned HDR-trained VMAF model. When
`vmaftune.hdr.select_hdr_vmaf_model()` is called against `model/`, it
returns `None`, and the harness falls back to scoring HDR sources with
the SDR-trained `vmaf_v0.6.1.json` weights. This produces VMAF scores
that **trend artificially low** for high-luminance regions of PQ / HLG
content — the SDR model's RBF SVR was never exposed to the PQ EOTF or
BT.2020 primaries during training.

Quantitative impact: empirical SDR-vs-HDR-VMAF deltas have **not been
characterised on this fork**. Treat any VMAF score over a PQ / HLG
source as a **lower bound only** until either a fork-owned HDR model
lands or Netflix releases their HDR artifact. Do **not** use the score
to pick CRFs at high quality targets (≥ 90 VMAF) on HDR encodes — the
SDR-on-HDR bias dominates in that regime.

## Why no model file is shipped

A 2026-05-09 source-or-train autonomous research pass evaluated three
paths and selected Path C (degrade gracefully + document):

- **Path A — source from elsewhere.** Exhausted with negative
  findings. Netflix's HDR VMAF model has never been publicly
  released; Netflix collaborator `li-zhi` on issue #645 confirmed
  "no timeline" and the most recent public statement (CSI Magazine
  2023-11-30) said "before the official release." No HDR VMAF model
  exists on Hugging Face, in any GitHub fork, or in any academic
  release. UT Austin's HDRMAX is a different algorithm (sklearn
  SVRs over local-expansive-nonlinearity features, not libvmaf
  adm/vif/motion), incompatible with the libvmaf JSON loader.
- **Path B — train a fork-owned model.** Infeasible in the
  research-pass window. Subjective HDR corpora (LIVE-HDR,
  LIVE-HDRvsSDR, LIVE-TMHDR) are gated behind manual access forms
  with unclear redistribution-of-derived-weights terms for
  BSD-3-Clause-Plus-Patent shipping, and a multi-day grid-search
  training run exceeded the autonomous task budget. Filed as a
  follow-up backlog row in [`docs/state.md`](../docs/state.md).
- **Path C — degrade gracefully + document** (chosen). Ship this
  model card so the fallback is discoverable; do not fabricate
  weights.

Full verbatim verification trail (URLs + access dates) is in
[research-0089](../docs/research/0089-hdr-vmaf-model-search.md).

## How to verify which model the harness used

`vmaf-tune`'s corpus driver records the resolved model path in each
output row (the schema-v2 `model_path` field). To confirm an HDR run
used the SDR fallback:

```bash
jq -r '.model_path' < corpus.jsonl | sort -u
# /…/model/vmaf_v0.6.1.json   ← SDR fallback in effect
```

If the output later contains `…/model/vmaf_hdr_v0.6.1.json` (or any
other `vmaf_hdr_*.json`), the fork has gained a real HDR model and
this card is stale — update it via the same workflow that lands the
weights.

## What would unblock shipping a real HDR model

Either of:

1. **Netflix releases `vmaf_hdr_v0.6.1.json` upstream.** Watch
   <https://github.com/Netflix/vmaf/tree/master/model> and
   issue #645. Port via `/port-upstream-commit` once it lands; the
   resolver picks it up with no further `vmaftune` change.
2. **The fork acquires a permissively-licensed HDR-MOS-labelled
   training corpus** AND a deliberate multi-day training slot. See
   the Path B follow-up in
   [research-0089](../docs/research/0089-hdr-vmaf-model-search.md)
   and the corpus-ingestion ADRs
   ([ADR-0310](../docs/adr/0310-bvi-dvc-corpus-ingestion.md)).

## License

This documentation file is BSD-3-Clause-Plus-Patent, matching the
fork's license. It contains no model weights.
