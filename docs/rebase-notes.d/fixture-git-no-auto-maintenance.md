## Scratch repositories start no git maintenance (2026-10-13)

`scripts/lib/clean-git-env.sh` sets `maintenance.auto=false` and `gc.auto=0`
through `GIT_CONFIG_COUNT` next to `init.defaultBranch`. A sync that touches the
helper keeps the three keys and the count of three;
`test_the_fixture_helper_starts_no_background_maintenance` in
`scripts/ci/test_git_fixture_isolation.py` fails when a commit after the helper
starts `git maintenance run` or `git gc --auto`. No upstream file is involved.
