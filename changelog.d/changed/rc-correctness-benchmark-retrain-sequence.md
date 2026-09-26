- The first-release candidate sequence now has explicit responsibilities:
  RC1 finishes release-blocking correctness and ships a reproducible hardware
  report path, RC2 owns benchmarking and performance tuning, and RC3 owns the
  one-shot real model retrain. “Done fixing” means no confirmed RC1 blocker or
  untriaged ledger row remains and exact-head gates pass; it does not mix
  performance or training into RC1. Ordinary Renovate and version PRs may merge
  under their existing required checks, with affected candidate evidence rerun
  after any merge. See
  [ADR-1341](docs/adr/1341-rc-correctness-benchmark-retrain-sequence.md).
