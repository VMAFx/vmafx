- **VCQ-223**: `VmafQualityRunnerWithLocalExplainer` no longer times out CI.
  The runner's fallback `LocalExplainer` now defaults to `neighbor_samples=100`
  (previously the upstream default of 5 000 produced ~480 000 libsvm
  `svm_predict_values` calls per run, causing a 4-8 min wall-time hang).
  Production callers that need higher fidelity can pass
  `optional_dict={"explainer_neighbor_samples": 5000}` or supply a fully
  constructed `LocalExplainer` via `optional_dict2={"explainer": ...}`.
  `@unittest.skip("[VCQ-223]")` removed from
  `QualityRunnerTest::test_run_vmaf_runner_local_explainer_with_bootstrap_model`.
  ([ADR-0562](docs/adr/0562-local-explainer-hang-fix.md))
