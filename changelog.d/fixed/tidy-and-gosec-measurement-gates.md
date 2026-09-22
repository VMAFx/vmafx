- **The three measurement gates measure the tree again instead of the
  fixtures.** `scripts/ci/tidy-baseline-cpu.json` is re-recorded from the
  `Tidy Ratchet` job's own `tidy-ratchet-cpu` artifact (314 TUs, 742 warnings,
  0 uncited NOLINTs), so the eight files this train cleaned below their
  allowance no longer fail the ratchet as stale-high debt. The changed-files
  `Tidy Changed` lane and the `gosec` step now skip the HISS rule engine's
  fixture tree under `.config/hiss/testdata/`, whose planted defects are the
  fixtures themselves, and `Tidy Changed` also skips the upstream MATLAB MEX
  sources under `compat/python-vmaf/matlab/`, which cannot be parsed at all
  without the proprietary MATLAB SDK headers. No warning in shipped code was
  suppressed and no allowance was raised.
