- CI: the required-checks aggregator of a master push no longer reads the check runs of a scheduled
  workflow run on the same commit. A skipped scheduled `FFmpeg Patch Stack` run had failed master
  `fdf711ba6` although the push's own run succeeded (`T-CI-AGGREGATOR-READS-SCHEDULE-RUNS-2026-10-15`).
