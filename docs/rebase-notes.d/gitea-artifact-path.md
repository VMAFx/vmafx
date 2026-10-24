## Artifact steps carry a Gitea twin (2026-10-24)

- Every `actions/upload-artifact` / `actions/download-artifact` step under
  `.github/` has `if: ... env.GITEA_ACTIONS != 'true'` and is followed by its
  Gitea twin (v3 pin, same inputs, or a `::warning::` skip). Netflix's
  workflows have no twin. **On sync**: a workflow taken from upstream runs
  `python3 scripts/ci/check_gitea_artifact_steps.py --write` before commit;
  never delete a twin or drop the guard (ADR-3368).
- `renovate.json` holds the `v3` pins below v4; keep the rule when merging
  Renovate config.
