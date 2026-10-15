- **The OpenSSF Scorecard gate now trusts Scorecard v5.6.0 (`61e84743`) run by
  `ossf/scorecard-action` v2.5.0 (`fa7117db`)**, which pulls its image from
  `ghcr.io`; the earlier action pulled from the retired `gcr.io` registry.
  Reports from the previously reviewed v5.5.0 (`c395761d`) are refused, and a
  new test fails when a workflow's action pin and the gate's pin differ. The
  check weights and risk levels are unchanged between the two versions.
