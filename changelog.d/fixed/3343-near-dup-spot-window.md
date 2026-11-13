- **`vmaf-tune --near-dup-verify` compares the same one-second window on both sides.**
  The spot check compared the query source's one-second score with the cached
  trial's whole-clip aggregate, so an exact duplicate whose first second is
  easier or harder than its average could be refused, and a different clip
  could be accepted, on the wrong difference (issue #3343). Both sides now
  score the cached encode over the first second — the query source and the
  cached source read from the cached row's `src` column — and a cached entry
  without a readable source path fails closed like a missing artifact. Five
  unit tests cover the positive, negative, exactly-1.5 boundary and the two
  fail-closed cases; the option stays off by default (ADR-3372 decision 5).
