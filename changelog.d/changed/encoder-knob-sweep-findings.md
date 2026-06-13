- **Encoder knob-sweep — populated Pareto hulls + recipe regressions
  (ADR-0308 / Research-0080)** — runs the
  [Research-0077 / ADR-0305](docs/adr/0305-encoder-knob-space-pareto-analysis.md)
  analysis script over the 12,636-cell Phase A sweep
  (`runs/phase_a/full_grid/comprehensive.jsonl`) and records the
  resulting Pareto-hull populations + recipe-regression count per
  codec. Headline numbers in
  [`docs/research/0080-encoder-knob-sweep-findings.md`](docs/research/0080-encoder-knob-sweep-findings.md):
  162 realised slices (every slice has a populated hull), 1,915
  recipe-vs-bare regressions at default tolerances
  (`bitrate_tol_pct=5`, `vmaf_tol=0.1`), CQP regression rate 6.6 %
  vs CBR 20.2 % / VBR 18.7 % — re-confirms
  [Research-0063](docs/research/0063-encoder-knob-space-cq-vs-vbr-stratification.md)
  with hard numbers. Top-15 aggregated bad-recipe cells all reproduce
  on **all 9** corpus sources, clustered around `h264_nvenc + bf3 /
  spatial_aq / full_hq` under CBR/VBR plus a smaller `hevc_nvenc +
  spatial_aq` cluster.
  [ADR-0308](docs/adr/0308-encoder-knob-sweep-recipe-regression-policy.md)
  commits the fork to a 7-of-9 *structural*-vs-*content-dependent*
  threshold: structural regressions are forbidden as
  `tools/vmaf-tune/codec_adapters/*` defaults and `vmaf-tune
  recommend` outputs without explicit override; content-dependent
  ones are filtered at recommend-time only via the per-slice hull
  lookup. The detector remains an **offline** gate — promotion to a
  CI gate is deferred until a smaller stratified sample reproduces
  the structural patterns. Per-codec adapter revisions land as
  follow-up PRs; this PR is documentation + ADR only.
