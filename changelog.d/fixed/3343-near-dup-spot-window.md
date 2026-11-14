- **`vmaf-tune --near-dup-verify` compares the same one-second window on both sides.**
  The spot check compared the query source's one-second score with the cached
  trial's whole-clip aggregate, so an exact duplicate whose first second is
  easier or harder than its average could be refused, and a different clip
  could be accepted, on the wrong difference (issue #3343). Both sides now
  score the cached encode over the same one-second window — the query source
  and the cached source read from the cached row's `src` column, at the
  entry's own sample offset for a sample-mode entry — and a cached entry
  without a readable source path, without a stored `src_sha256`, or whose
  file no longer hashes to that stored hash fails closed like a missing
  artifact, so a replaced file is never scored as the baseline. Nine unit
  tests cover the positive, negative, exactly-1.5 boundary, the offset
  window and the four fail-closed cases; the option stays off by default
  (ADR-3372 decision 5).
- **The spot check decodes the cached encode before scoring.** The cached
  side of the spot check scored the stored encode artifact directly, so a
  container-format artifact was scored as raw YUV (issue #3447). The
  decode path is shared with the landed fix for that issue: the cached
  encode decodes in a temporary directory that closes with the check, a
  failed decode refuses the candidate, and the cached source normalizes
  into the same directory through the same reference-decode path the
  sweep uses for the query leg — a container decodes there, and a raw
  source whose native geometry differs from the rung target scales there
  — so no stale sidecar named for another source can zero the delta and
  no raw source is misread at the rung geometry. The near-duplicate
  index still matches on the cell params, which carry width, height,
  pixel format and frame rate, so a cached entry at another geometry
  never reaches the spot check (issue #3448, closed as not planned).
- **Rebased onto the landed decode fix (#3451).** Upstream's decode
  change decodes the cached encode in a temporary directory that closes
  with the check, guards on the sweep's reference decode result, and
  scores with the cell's score model; this PR keeps all of that and
  restores the same-window comparison on top of it — the landed spot
  check compares the window score with the trial's whole-clip aggregate
  again, which is the defect this PR exists to fix (issue #3343). The
  cached source normalizes into the same temporary directory through
  the sweep's own reference-decode path: the shared decode path leaves
  no files behind, never reads a stale sidecar named for another source,
  and parses a raw cached source at its native geometry before scaling
  to the rung target like the query leg, and the cached-source leg
  lives in its own helper to keep both functions inside the 60-line
  limit. The landed decode tests in `tests/test_near_dup_spot_decode.py`
  are adapted to the two-leg behavior: their cleanup, refusal and model
  assertions stand, their single-run assertions now expect the two
  scoring legs, and the real-ffmpeg window test refuses on a different
  cached source instead of an offset the index cannot produce.
