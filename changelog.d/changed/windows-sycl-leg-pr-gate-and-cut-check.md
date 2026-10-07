- **A Windows SYCL zip regression is seen before it merges, and a cut needs every tester leg green.**
  A pull request now builds the x64 SYCL tester zip when it changes anything that leg reads
  (selector `windows_tester_zip_sycl`), in the light CI tier too (`own_input_lanes` of
  `.github/ci-tier.json`). `scripts/release/check-candidate-legs.py` requires every Windows
  zip, macOS bundle and tester-image leg to be green on the exact commit, and the Release
  Script Contract runs it on the cut pull request; the release guide lists the dispatches
  to make first. See ADR-2198.
