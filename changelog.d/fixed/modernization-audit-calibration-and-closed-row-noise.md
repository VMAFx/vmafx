- Suppress ~100 false-positive findings from the project-modernization audit
  scanner. Two exclude rules added: (1) `placeholder` matches in
  `scripts/ci/cross_backend_calibration.py`, `cross_backend_parity_gate.py`,
  and `gpu_ulp_calibration.yaml` are no longer reported — those files use
  "placeholder" as a calibration-framework STATUS value, not a code gap.
  (2) `T-*` rows under `## Recently closed`, `## Resolved`, and
  `## Confirmed not-affected` headings in `docs/state.md` and the
  `.workingdir2/` state files are now skipped; rows carrying an inline
  `(YYYY-MM-DD)` date stamp or a `closed: ` prefix are also excluded.
  Eleven new unit tests verify both exclusion paths and the ~100-finding
  drop in the combined scenario.
