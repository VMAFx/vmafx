- macOS clang / Metal / MoltenVK CI Python-test assertions
  recalibrated for the post-`bf9ad333` VIF on-the-fly filter values
  (ADR-0418). PR #758 cherry-picked Netflix's `142c0671` /
  `7209110e` / `d93495f5` / `fe756c9f` recalibration fixtures for
  the `test_run_vmaf_*` assertions but Netflix had not (and as of
  this PR has not) shipped companion fixtures for
  `local_explainer_test::test_explain_vmaf_results`,
  `vmafexec_test::test_run_vmafexec_runner_akiyo_multiply*`, and
  five `vmafexec_feature_extractor::test_run_float_adm_fextractor_adm_*`
  tests. Fork-recalibrated against the actual macOS-libm binary
  output; revert to upstream values when Netflix ships them
  (mechanical via `git grep "post-VIF-sync (#758) recal"`; see
  `docs/rebase-notes.md` entry `fix/macos-test-recal-post-vif-sync`).
