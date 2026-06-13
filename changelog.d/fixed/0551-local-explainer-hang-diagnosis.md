- **VCQ-223 `LocalExplainer` CI hang root cause identified.**
  `QualityRunnerTest::test_run_vmaf_runner_local_explainer_with_bootstrap_model`
  (`python/test/local_explainer_test.py:252`) was skipping with
  `@unittest.skip("[VCQ-223]")` due to a CI timeout. Investigation confirmed the
  hang is a CPU-bound computation timeout, not a deadlock or pipe stall:
  `VmafQualityRunnerWithLocalExplainer._run_on_asset` constructs `LocalExplainer()`
  with the default `neighbor_samples=5000`, producing 2 assets × 48 frames × 5 001
  libsvm `svm_predict_values` calls per run (≈ 480 000 Python-C boundary crossings;
  wall time 4–8 min). The passing sibling test uses `neighbor_samples=100` (50×
  fewer). The `@unittest.skip` is preserved pending a follow-up fix PR that adds
  `optional_dict2={"explainer": LocalExplainer(neighbor_samples=100)}` and updates
  score assertions. See ADR-0551 and Research-0551.
