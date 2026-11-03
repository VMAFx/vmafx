## Upstream PR sweep tool and hook GIT_DIR isolation (#3266) (2026-11-03)

- `scripts/dev/upstream_pr_sweep.py` and `scripts/dev/tests/test_upstream_pr_sweep.py`
  are fork-only developer tooling; no upstream Netflix/vmaf file is touched.
- `scripts/git-hooks/framework-hooks.sh` unsets repository-locating `GIT_*`
  variables (`GIT_DIR`, `GIT_WORK_TREE`, `GIT_COMMON_DIR`, `GIT_OBJECT_DIRECTORY`,
  `GIT_ALTERNATE_OBJECT_DIRECTORIES`) while keeping `GIT_INDEX_FILE` for `run`
  and preserving `env -u GIT_INDEX_FILE -u GIT_DIR -u GIT_WORK_TREE -u GIT_OBJECT_DIRECTORY`
  around `install-hooks`. Fork-only; no rebase impact on upstream syncs.
