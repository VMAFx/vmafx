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
- **The spot check decodes the cached encode and scores both legs at the
  cached entry's geometry.** The cached side of the spot check scored the
  stored encode artifact directly, so a container-format artifact was scored
  as raw YUV (issues #3447, #3448), and the query source was scored at the
  query job's resolution rather than the entry's, so a query at a different
  resolution was compared against pixels of a different size. The cached
  encode is now run through the same decode path as the trial it came from,
  both legs score at the width, height and pixel format recorded in the
  entry's row, a query at another resolution is scaled to that geometry and
  a query with a different pixel format is refused. Decode or geometry
  failures refuse the candidate instead of scoring mismatched inputs.
  Thirteen unit tests cover the decode, scaling, pixel-format and
  fail-closed cases alongside the original window checks.
- **Rebased onto the landed decode fix (#3451).** Upstream's decode
  change decodes the cached encode in a temporary directory that closes
  with the check, guards on the sweep's reference decode result, and
  scores both legs with the cell's score model; this PR keeps all of
  that and restores the same-window comparison on top of it — the
  landed spot check compares the window score with the trial's
  whole-clip aggregate again, which is the defect this PR exists to fix
  (issue #3343) — and adds the entry's geometry to both legs (issue
  #3448). The landed decode tests in
  `tests/test_near_dup_spot_decode.py` are adapted to the two-leg
  behavior: their cleanup, refusal and model assertions stand, their
  single-run assertions now expect the two scoring legs, and the
  real-ffmpeg window test refuses on a different cached source instead
  of an offset the index cannot produce.
