- **`vmaftune.bisect` — Phase B target-VMAF bisect
  ([ADR-0326](../docs/adr/0326-vmaf-tune-phase-b-bisect.md)).** Ships
  the production wiring the existing `compare`, `recommend-saliency`,
  `predict`, `tune-per-shot`, and `ladder` subcommands have been
  stubbing out via the `NotImplementedError("Phase B pending")`
  placeholder. Integer binary search over CRF assuming
  monotone-decreasing VMAF in CRF; six-to-eight encode round-trips per
  call (`O(log range)`), midpoint rounds toward higher CRF so the
  "best so far" record is always a measured CRF. Aborts with a clear
  error on monotonicity violation rather than falling back to a
  different search strategy. `make_bisect_predicate(target_vmaf, *,
  width=..., height=..., framerate=..., duration_s=...)` adapter
  satisfies `compare.PredicateFn`; `compare._default_predicate` now
  points callers at the entry-point. Subprocess seam mirrors
  `encode.run_encode` / `score.run_score`; tests exercise the full
  bisect (including the predicate adapter and `compare_codecs`
  integration) with synthetic curves — no real ffmpeg / vmaf
  invocation. No new CLI subcommand. Sample-clip mode (ADR-0301) and
  cache integration (ADR-0298) are deliberate out-of-scope follow-ups.
