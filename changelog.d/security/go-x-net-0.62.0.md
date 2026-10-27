- **`golang.org/x/net` moves from v0.61.0 to v0.62.0** for GO-2026-6860 (quadratic time when the
  `html` parser coalesces text nodes) and GO-2026-6866 (`html` rendering of raw text that did not
  check every ancestor namespace transition). `scripts/ci/govulncheck-gate.py` failed on both
  advisories, which have no `not_affected` statement; with v0.62.0 it reports no finding for
  `golang.org/x/net`.
