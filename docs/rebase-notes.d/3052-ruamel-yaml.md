## Gitea artifact checker uses ruamel.yaml (2026-11-18)

`fix/issue-3052-ruamel-yaml`, [issue #3052](https://github.com/VMAFx/vmafx/issues/3052). `scripts/ci/check_gitea_artifact_steps.py` now uses `ruamel.yaml` 0.19.1's safe loader and scalar dumper instead of PyYAML, and the pre-commit and tooling-test locks record the dependency. A rebase must keep the ruamel import, the `_load_yaml` / `_yaml_scalar` helpers, the lock entries, the credits row and the checker tests; upstream's PyYAML version has neither the dependency nor the same lock inputs.
