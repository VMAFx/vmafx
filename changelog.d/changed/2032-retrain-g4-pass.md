- The epic #1246 retrain runbook's gate table records **G4 (K150K re-smoke,
  zero disk leak) as PASS**, measured 2026-09-06 against `master` `e91ab8284`
  after PR #1302 merged: `ok=5 fail=0` in 10.6 s, all seven §4.2 assertions
  green, and an empty scratch directory after the run. G1 drops from 12 open
  epics to 11 now that #1244 is closed. Of the five gates, only G1 (remaining
  epics) and G5 (maintainer authorization) are still outstanding.
