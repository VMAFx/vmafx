- **`vmaf-tune --near-dup-verify` compares the same one-second window on
  both sides.** The spot check compared the query source's one-second
  score with the cached trial's whole-clip aggregate, so an exact
  duplicate whose first second is easier or harder than its average
  could be refused, and a different clip could be accepted, on the wrong
  difference (issue #3343). Both sides now score the cached encode over
  the same one-second window — the query source and the cached source
  read from the cached row's `src` column, at the entry's own sample
  offset for a sample-mode entry. The cached encode decodes through the
  shared decode path in a temporary directory that closes with the
  check, and the cached source normalizes through the same
  reference-decode path the sweep uses for the query leg, so a container
  decodes there and a raw source at another native geometry scales to
  the rung target instead of being misread. A cached entry without a
  readable source path, without a stored `src_sha256`, whose file no
  longer hashes to that stored hash, or whose decode fails is refused
  and reuse fails closed; the near-duplicate index still keeps entries
  at another geometry away from the check (issue #3448, closed as not
  planned), and the option stays off by default (ADR-3372 decision 5).
  Unit tests cover the two-leg window, the decode path and the
  fail-closed cases in `tests/test_near_dup_verify.py` and
  `tests/test_near_dup_spot_decode.py`.
