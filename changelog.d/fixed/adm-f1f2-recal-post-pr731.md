- Recalibrated the fork-local `adm_f1f2` test assertion in
  `python/test/feature_extractor_test.py::test_run_vmaf_fextractor_adm_f1f2`
  from `0.9539779375` to `0.8872294166666667` (at `places=4`) to reflect
  the corrected upstream-canonical `adm_cm` AIM derivation shipped in PR #731.
  The prior value was calibrated against the buggy single-pass AIM that
  measured reference self-energy rather than distorted-vs-reference energy,
  yielding AIM ~0.52 and adm3 ~0.95; the correct value after the PR #731
  port is AIM ~0.026 and adm3 ~0.887 for the fork-local f1s/f2s noise-weight
  parameters.
