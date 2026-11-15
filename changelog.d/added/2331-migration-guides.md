- **A migration guide maps upstream commands to their VMAFx equivalents, and a
  docs check holds every equivalence row to the golden pair (#2331).**
  `docs/usage/migration.md` covers the four starting points users arrive with —
  the upstream `vmaf` CLI and Python harness, the FFmpeg `libvmaf` filter, and
  the `ffmpeg-quality-metrics` and `easyVmaf` wrappers — with command tables,
  what is the same, what differs, and why the one not-equivalent row
  (`easyVmaf`'s automatic preprocessing) has no VMAFx flag. Every equivalent
  row quotes the pooled mean `testdata/scores_cpu_576.json` records for the
  golden pair, and `scripts/docs/check_migration_equivalence.py` (wired into
  `make docs-fragments-check`) fails the docs check on a drifted number, an
  equivalent row without a command block, or a not-equivalent row without a
  reason and an ADR link. With `--execute` the rows run where their toolchain
  exists — `cli` rows nightly in the `netflix-benchmark` job, `python`,
  `ffmpeg` and `wrapper` rows only when their toolchain is present, skipped
  with a printed reason otherwise.
