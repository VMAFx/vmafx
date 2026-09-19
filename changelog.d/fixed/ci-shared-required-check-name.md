- **CI: a required check can no longer be masked by a second job with the
  same name.** Two workflows reported `Windows MSVC+CUDA`, and the required
  checks aggregator keeps only the newest run per name. The `build.yml` job is
  now `Windows MSVC+CUDA (full)`, and `scripts/ci/check-aggregator-names.sh`
  fails when more than one job reports a required name.
