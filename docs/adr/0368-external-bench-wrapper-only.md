<!-- markdownlint-disable MD060 -->
# ADR-0368: External-competitor benchmark harness — wrapper-only architecture

- **Status**: Accepted
- **Status update 2026-05-15**: implemented; `tools/external-bench/`
  directory present with `run.sh` wrappers and `compare.py`
  orchestrator; wrapper-only architecture active.
- **Date**: 2026-05-08
- **Deciders**: Lusoris, Claude
- **Tags**: ai, testing, license, tooling, fork-local

## Context

The fork ships two perceptual-quality predictors that warrant
side-by-side comparison against external open-source competitors:

- `fr_regressor_v2_ensemble_v1` — a full-reference VMAF regressor
  ensemble (5 seeds; ADR-0319 / ADR-0321).
- `nr_metric_v1` — a no-reference MOS predictor.

Two competitors are publicly available and worth comparing against
on the same corpus:

1. **Synamedia/Quortex `x264-pVMAF`** (`github.com/quortex/x264-pVMAF`,
   November 2024) — predicted-VMAF estimator integrated into a forked
   `x264` encoder. Upstream licence: **GPL-2.0**.
2. **DOVER-Mobile** — no-reference video quality predictor distributed
   as a Python package. Upstream licence: Apache-2.0 (code) plus
   CC-BY-NC-SA 4.0 (weights).

The fork is BSD-3-Clause-Plus-Patent. **GPL-2.0 cannot be combined
with permissive-licensed redistributable code without relicensing
the combined work.** Vendoring `x264-pVMAF` source into the fork
would force the entire fork to GPL-2.0 — a non-starter given the
upstream Netflix/vmaf licence and every downstream consumer (FFmpeg
filters, third-party tools, MCP server) the fork ships for.

Yet running a side-by-side benchmark against `x264-pVMAF` is the
only way to substantiate claims of relative accuracy / runtime —
and the user explicitly asked for that comparison.

## Decision

We will land a benchmark harness at `tools/external-bench/` under a
**wrapper-only architecture**:

- Each external competitor lives in
  `tools/external-bench/<competitor>/run.sh` — a thin bash wrapper
  that invokes a **user-installed** external binary (path via env
  var) and re-shapes its output into a normalised JSON schema.
- The fork-side predictors get the same wrapper shape
  (`fork-fr-regressor`, `fork-nr-metric`) so `compare.py` can
  aggregate all four into a single comparison table.
- `compare.py` is the orchestrator: it discovers a corpus
  (BVI-DVC test fold + Netflix Public Drop by default), runs each
  wrapper across every (ref, dis) pair, aggregates PLCC / SROCC /
  RMSE / runtime, and renders a fixed-width comparison table.
- Tests under `tools/external-bench/tests/test_compare.py` stub
  `subprocess.run` so the suite never depends on external binaries
  being installed.

The fork redistributes only the wrapper scripts + comparison logic

- documentation. **No GPL'd code is vendored, linked, or copied
into this fork.** Side-by-side benchmarking is permissible because
the harness invokes the external binary as a subprocess and reads
its (factual) numerical output — same posture as running
`/usr/bin/ffmpeg` from a BSD-licensed test harness.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Vendor `x264-pVMAF` source** | Reproducible build; no operator install step | Relicenses the entire fork to GPL-2.0; breaks every downstream consumer (FFmpeg filter integration, MCP server, BSD-licensed tiny-AI surfaces); upstream Netflix/vmaf licence terms forbid it | Existential licence break — fork loses its permissive posture and every downstream relicenses by association |
| **Skip `x264-pVMAF`, compare only against DOVER-Mobile** | No GPL question | User explicitly asked for the Synamedia comparison; benchmarking against DOVER-Mobile alone leaves the most directly competitive predictor unmeasured | Drops the most informative comparison; the GPL boundary is solvable without dropping the comparison |
| **Wrapper-only architecture (this ADR)** | Zero GPL'd code in the fork; operator installs external binary themselves; same wrapper shape works for any future competitor (copyleft or not); tests stub the subprocess so CI never depends on external installs | Operator must install binaries themselves; CLI shapes drift across upstream versions and the wrapper's schema-shim has to track them | **Chosen.** The boundary cost (a documented env-var per competitor) is small; the licence safety is total |
| **Build a separate GPL-licensed sibling repo** | Vendoring is then legal in *that* repo | Doubles the maintenance surface for one comparison; bench harness has to live somewhere and "out-of-tree" means it rots; reviewers cannot easily inspect the apples-to-apples invocation | Operational cost outweighs any advantage over wrapper-only |

## Consequences

### Positive

- The fork stays BSD-3-Clause-Plus-Patent. Every downstream
  consumer (FFmpeg filter, MCP server, tiny-AI surfaces) is
  unaffected.
- Adding a new external competitor (Netflix VMAF NEG, GMSD,
  ITU-R BT.500-style models, future GPL'd predictors) follows the
  same recipe: drop in `run.sh`, register in `WRAPPERS`, add a
  stubbed test.
- The harness ships with deterministic stubbed tests
  (`tools/external-bench/tests/test_compare.py`, 7 passing) so CI
  can verify schema + aggregation regressions without external
  installs.

### Negative

- Operators have to install the external binaries themselves
  (`pipx install dover-mobile`; `git clone … && make` for
  `x264-pVMAF`). Documented in `tools/external-bench/README.md`.
- Wrapper schema-shims may drift across upstream versions of the
  external binaries. Mitigation: each `run.sh` has a single
  Python heredoc that does the JSON re-shape, so an upstream CLI
  break needs at most a one-file fix.

### Neutral / follow-ups

- The harness's BVI-DVC corpus default assumes the operator has the
  archive locally per ADR-0310. Failure mode is a clear stderr
  message naming the expected paths, not a silent zero-row run.
- A future ADR may layer a GPU-runtime gate on the comparison
  (e.g. require the runtime metric to come from the same backend
  as `/cross-backend-diff`) but that is out of scope for this PR.

## References

- `req`: user direction to "build a side-by-side benchmark harness
  comparing the fork's `fr_regressor_v2_ensemble` and `nr_metric_v1`
  against two external open-source competitors: Synamedia/Quortex
  x264-pVMAF (GPL-2.0 OSS, github.com/quortex/x264-pVMAF, Nov 2024)
  and DOVER-Mobile" with the explicit constraint "x264-pVMAF is
  GPL-2.0. The fork is BSD-3-Clause-Plus-Patent. The harness MUST
  NOT vendor, link, or copy any code from x264-pVMAF."
- [ADR-0024](0024-netflix-golden-preserved.md) — the Netflix golden
  numerical-correctness gate against which the fork's predictors are
  ultimately calibrated.
- [ADR-0108](0108-deep-dive-deliverables-rule.md) — deep-dive
  deliverables rule covering this PR.
- [ADR-0310](0310-bvi-dvc-corpus-ingestion.md) — BVI-DVC corpus
  ingestion that this harness defaults to.
- [ADR-0319](0319-ensemble-loso-trainer-real-impl.md) /
  [ADR-0321](0321-fr-regressor-v2-ensemble-full-prod-flip.md) —
  fork-side `fr_regressor_v2_ensemble_v1` lineage.
- [Synamedia/Quortex x264-pVMAF](https://github.com/quortex/x264-pVMAF)
- [DOVER](https://github.com/QualityAssessment/DOVER) — DOVER /
  DOVER-Mobile upstream.
