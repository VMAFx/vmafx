- `ai/scripts/calibrate_nr_threshold.py`: `_DEFAULT_CORPUS` now reads the
  `VMAF_CORPUS_DIR` environment variable (falling back to `.corpus/netflix`),
  matching the pattern already used by `validate_ensemble_seeds.py`. Added
  missing `import os`.
- `tools/vmaf-tune/tests/test_bbb_e2e_v5_bug_cluster.py`: BBB corpus path
  inside the dev-mcp container now reads `VMAF_CORPUS_DIR` (falling back to
  `/workspace/.corpus/bbb_e2e`) instead of a hardcoded prefix.
- `.gitignore`: added `.corpus/` entry so the exclusion is visible to all
  contributors, not only locally via `.git/info/exclude`.
