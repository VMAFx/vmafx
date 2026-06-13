# Research-0087 — External-competitor benchmark survey (Synamedia x264-pVMAF, DOVER-Mobile)

- **Status**: Scoping digest for [ADR-0332](../adr/0332-external-bench-wrapper-only.md).
  Captures the licence + integration-cost trade-off space behind the
  wrapper-only architecture decision.
- **Workstream**: external-bench harness, fork-side predictor
  comparison.
- **Last updated**: 2026-05-08.

## Open questions

1. Is `x264-pVMAF` GPL-2.0 redistribution-compatible with the fork's
   BSD-3-Clause-Plus-Patent licence?
2. Is DOVER-Mobile redistribution-compatible?
3. What is the minimum-cost integration that lets us run a
   side-by-side benchmark without relicensing the fork?
4. What corpus split should the harness default to?
5. How do we keep the harness's tests deterministic when the
   competitors are not installed?

## Findings

### Q1 — `x264-pVMAF` licence

- Upstream repo: <https://github.com/quortex/x264-pVMAF>
  (Synamedia/Quortex, November 2024). Forked from `x264` which is
  itself GPL-2.0; predicted-VMAF additions inherit GPL-2.0.
- GPL-2.0 ↔ BSD-3-Clause-Plus-Patent: combined-work redistribution
  forces the entire combined work to GPL-2.0. Vendoring would
  relicense the fork. **Not viable.**
- Subprocess invocation is fine: the fork invokes the binary as a
  separate process, reads its (factual) output, and re-shapes that
  output. No GPL'd code lands in this repo.

### Q2 — DOVER-Mobile licence

- Upstream: <https://github.com/QualityAssessment/DOVER>. Code is
  Apache-2.0; the trained weights are CC-BY-NC-SA 4.0. Same
  subprocess posture applies — the fork invokes the
  user-installed CLI and never re-distributes either the code or
  the weights. NC clause does not bind us because we are not
  distributing the weights, only invoking a tool that happens to
  ship them.

### Q3 — Minimum-cost integration

- Wrapper-only architecture: each competitor is one
  `tools/external-bench/<competitor>/run.sh` script that translates
  upstream-CLI conventions into a normalised JSON shape. Adding a
  fifth competitor later is one new directory + one entry in
  `compare.WRAPPERS` + one stubbed test.
- Cost: operator must install the external binaries themselves
  (documented in the wrapper's header + the harness README).

### Q4 — Corpus default

- BVI-DVC test fold (ADR-0310) — content-diverse, 4-tier resolution,
  10-bit YCbCr; already ingested by the fork's training pipeline.
- Netflix Public Drop — local-only, 9 ref × 70 dis YUVs. Same
  posture as ADR-0310.
- Combination gives ≥200 row corpus across resolutions, which is
  enough for stable PLCC/SROCC means with low variance.

### Q5 — Deterministic tests

- `run_wrapper` accepts a `runner` callable parameter that resolves
  to `subprocess.run` at call time (not at definition time — see
  the AGENTS.md invariant). Tests stub it via either the explicit
  parameter or `monkeypatch.setattr(compare.subprocess, "run", ...)`
  and write canned `output.json` files to the path passed via
  `--out`.
- Result: 7 stubbed tests (`tests/test_compare.py`) cover schema
  parsing, failure propagation, aggregation, table rendering,
  end-to-end main path, error path on missing corpus, and corpus
  discovery. None depend on `x264-pVMAF` or `dover-mobile` being
  installed.

## Decision matrix → see ADR-0332

The runner-up options (vendor `x264-pVMAF`; skip the comparison;
build a separate GPL'd sibling repo) are documented in
[ADR-0332's "Alternatives considered" section](../adr/0332-external-bench-wrapper-only.md).

## References

- Synamedia/Quortex `x264-pVMAF`:
  <https://github.com/quortex/x264-pVMAF> (Nov 2024).
- DOVER / DOVER-Mobile: <https://github.com/QualityAssessment/DOVER>.
- ADR-0310 — BVI-DVC corpus ingestion (corpus default).
- ADR-0319 / ADR-0321 — fork-side `fr_regressor_v2_ensemble_v1`
  lineage.
- ADR-0024 — Netflix golden numerical-correctness gate.
