- **The release-PR exemption test no longer fails on every release pull
  request.** Its "without diff" cases ran the gate inside the CI checkout, and
  without a diff file the gate diffs that checkout against `origin/master`. In
  the release pull request's own run that diff is release-shaped, so the test
  saw an exemption it did not expect. The test now runs the gate outside any
  Git checkout.
