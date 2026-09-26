- **Release notes**: removed entries for GPU feature options, performance
  changes and tooling that stale-branch merges had dropped and that this
  release does not contain. Scores are unaffected: a model that sets one of
  those options runs the feature on the CPU, and an explicit GPU request with
  the option fails with "unknown option". The missing work is tracked in
  `docs/state.md` under RC2 (GPU option parity, performance) and RC3 (training
  scripts).
