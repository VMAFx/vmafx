- **Unify `vmaf` JSON CLI invocation across `ai.data`.** `ai.data.scores._run_vmaf_score`
  now reuses `ai.data.feature_extractor._run_vmaf_json` rather than maintaining a
  duplicate `subprocess.run` execution and JSON output cleanup helper; argv ordering,
  error handling, and return contracts remain unchanged.
