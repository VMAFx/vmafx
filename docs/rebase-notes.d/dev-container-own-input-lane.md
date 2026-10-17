## Dev Container Build as an own-input lane (2026-10-17)

- `.github/workflows/dev-container-build.yml`: `impact` and the `Dev Container Build`
  gate run on `needs.tier.outputs.light`; step `pick` reads `dev_container` on the full
  tier and `dev_container_inputs` otherwise (ADR-3338). Fork-only workflow, no upstream
  counterpart. **On sync**: nothing upstream touches it; keep the three conditions and
  the `pick` step, which `scripts/ci/tests/test_ci_routing_contract.py` guards.
- `.github/ci-impact.json` (`dev_container_inputs`) and `.github/ci-tier.json`
  (`own_input_lanes`) carry the selector and the lane entry.
