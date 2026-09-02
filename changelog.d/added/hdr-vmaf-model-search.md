- **HDR VMAF model search — discoverable fallback documentation
  ([research-0089](docs/research/0089-hdr-vmaf-model-search.md);
  ADR-0300 status update 2026-05-09).** An autonomous
  source-or-train research pass evaluated three paths to close the
  HDR-VMAF-model gap that PR #477's TransNet+HDR work flagged.
  Path A (source from upstream Netflix / Hugging Face / academic
  releases / GitHub-wide search) returned negative findings — no
  publicly-released, BSD-3-Clause-Plus-Patent-compatible,
  libvmaf-JSON-loadable HDR VMAF model exists as of 2026-05-09.
  Path B (train a fork-owned model) was deferred — all five
  candidate subjective HDR corpora (LIVE-HDR, LIVE-HDRvsSDR,
  LIVE-TMHDR, ESPL-LIVE HDR, ITU-T SDR-vs-HDR) are gated behind
  manual access forms with unclear derived-weight-redistribution
  terms, and a multi-day training run exceeded the research-pass
  budget. Path C (degrade gracefully + document) was chosen: ship
  `model/vmaf_hdr_model_card.md` so the SDR-fallback path is
  discoverable from a `model/` directory listing, with a loud
  warning that VMAF scores over PQ / HLG sources are a **lower
  bound only** and must not be used to pick CRFs at high quality
  targets (≥ 90 VMAF) on HDR encodes. **No fabricated model
  weights are added** — the `vmaf_hdr_*.json` resolver glob is
  unchanged and continues to return `None`. ADR-0300 carries an
  inline `### Status update 2026-05-09: HDR model status` section
  recording the outcome and the surveyed sources with access-date
  citations.
