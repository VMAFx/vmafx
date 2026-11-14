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
