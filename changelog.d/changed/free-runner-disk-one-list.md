- **Every hosted-runner disk cleanup calls `scripts/ci/free-runner-disk.sh`.**
  The clang-tidy lane action, the three production GPU image jobs, the tester
  image job and the published licence companions no longer carry their own
  copy of the directory list; `scripts/ci/tests/test_free_runner_disk.py`
  fails when a workflow or action under `.github/` removes one of the listed
  directories itself.
