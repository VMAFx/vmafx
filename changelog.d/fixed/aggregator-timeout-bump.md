- Fixed `Required Checks Aggregator` workflow timing out at 30 minutes
  while CI queue depth pushed sibling workflows past 60+ minutes wall-clock.
  Bumped poll deadline from 30 → 90 minutes and job timeout from 35 → 100
  minutes. Symptom: every PR shipped on 2026-05-09 morning sat in
  `BLOCKED` with the only failing check being the Aggregator marking
  required jobs as `queued: queued`; CI eventually completed all the
  underlying jobs, but auto-merge stayed stuck because the aggregator
  result is the required gate per ADR-0313.
