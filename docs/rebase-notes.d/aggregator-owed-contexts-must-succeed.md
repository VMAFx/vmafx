## The aggregator fails an owed context that did not succeed (2026-10-13)

- `.github/workflows/required-aggregator.yml` accepts only `success` for an owed
  required name. Absent or skipped passes only for a name the CI tier does not
  owe (ADR-2169), a name in `pullRequestOnly` on a non-`pull_request` event, or
  `Coverage GPU` / `SYCL Parity (Arc A380)` while their repository switch is off.
  `releaseMustReport` is gone (ADR-3293).
- `libvmaf-build-matrix.yml` and `tests-and-quality-gates.yml` have no
  trigger-level `paths-ignore`; ADR-0341's docs-only filter no longer exists.
- **On sync**: fork-only files; no upstream counterpart. Do not bring back an
  absent-means-pass branch, `releaseMustReport` or a `paths-ignore` on a required
  workflow. A workflow that loses its push trigger adds its names to
  `pullRequestOnly` in the same pull request.
  `scripts/ci/tests/test_aggregator_absent_fails.py` fails on the old aggregator
  (9 failed with `AGGREGATOR_UNDER_TEST` pointed at `bfd2074e9`'s copy).
