## One MSVC leg on light-tier pull requests that change core C or C++ (2026-10-12)

`ci/msvc-sycl-light-leg`, [ADR-3279](adr/3279-msvc-sycl-leg-on-light-tier.md). A rebase or sync
keeps, in `.github/workflows/libvmaf-build-matrix.yml`, the job `impact`
(`Plan MSVC leg impact`), the `if` of `windows-gpu-build` with all three terms (full tier, a
failed planner, the selector `windows_msvc_sycl`) behind `!cancelled()`, the `tier` key of both
matrix rows (`Windows MSVC+SYCL` is `light`, `Windows MSVC+CUDA` is `full`) and the first step
`leg`, whose output `run` gates every step after the checkout. The checkout itself stays
unconditional and shallow (`fetch-depth: 1`): the checkout-ordering gate counts no checkout
that carries an `if`. A step added to that job takes
`if: steps.leg.outputs.run == 'true'`, or it runs on the CUDA row of a light-tier pull request
without a build tree. `.github/ci-tier.json` keeps `Windows MSVC+SYCL` in `full_only` and in
`own_input_lanes`; `.github/ci-impact.json` keeps `windows_msvc_sycl` as `own_paths_only`. In
`scripts/ci/ci_escalate.py`, `cancel()` treats 409 as "already completed" and `rerun_all()`
carries on after a run it could not re-run and returns the count: do not bring back a loop that
ends at the first refused request. `scripts/ci/tests/test_ci_routing_contract.py` (ten planted
defects), `scripts/ci/tests/test_ci_impact.py` (`MsvcLegRouting`) and
`scripts/ci/tests/test_ci_escalate.py` (`RerunAll`) fail when one of these is lost.
