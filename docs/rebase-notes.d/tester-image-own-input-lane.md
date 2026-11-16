## Tester Image as an own-input lane and CPython 3.14.10 licence record (2026-11-16)

- `.github/workflows/docker-publish-tester.yml`: `impact` and the `Tester Image`
  gate run on `needs.tier.outputs.light == 'true'`, routed by selector
  `tester_image` on both tiers (ADR-3554). Fork-only workflow, no upstream
  counterpart. **On sync**: keep both `outputs.light` conditions, which
  `scripts/ci/tests/test_ci_routing_contract.py` and
  `scripts/ci/tests/test_required_release_legs.py` guard.
- `.github/ci-tier.json` (`own_input_lanes`),
  `.github/workflows/required-aggregator.yml` (`delayedMissing` waiting on
  active `delayedStrictDependencies` for `notOwed` own-input lanes) and
  `tools/rc1-tester/image/licensing.json` (`cpython_license_rst["3.14.10"]`)
  are fork-only files.
