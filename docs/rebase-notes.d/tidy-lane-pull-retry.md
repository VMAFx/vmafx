## Tidy lane dev container pull retries (2026-10-16)

- `.github/actions/tidy-lane/action.yml` pulls the dev container through
  `scripts/ci/pull-dev-image.sh` (digest check, four bounded attempts, error
  naming the image). Fork-only CI; no upstream counterpart. Keep the action on
  the script rather than an inline `docker pull`.
  `scripts/ci/tests/test_pull_dev_image.py` guards it.
