- Tests: scratch repositories built after `scripts/lib/clean-git-env.sh` no longer
  start git's automatic maintenance. A commit in such a repository started a
  detached `git maintenance run --auto`, whose repack deleted loose objects while
  `scripts/ci/tests/test-default-model-single-source.sh` copied the repository, and
  `Tooling Tests` failed intermittently with `cp: cannot stat .git/objects/..`. The
  helper now sets `maintenance.auto=false` and `gc.auto=0`, and
  `scripts/ci/test_git_fixture_isolation.py` fails when it stops doing so.
