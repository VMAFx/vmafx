- **Tutorial series and first cookbook slice (#2330).**
  Five ten-minute tutorials (score a pair, compare encodes, choose a target,
  gate a build, score live) and a cookbook slice (10-bit, frame offset,
  non-420 subsampling, batch, two GPUs) now run against the fixture pair.
  Every command block is tagged `run-check` and held by
  `scripts/docs/check_docs_commands.py`, wired into
  `make docs-fragments-check`; the check fails on a duplicate block id, a
  page without a block, or a score-like number no recorded source pins.
