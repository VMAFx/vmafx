### Fixed

- `dev/Containerfile`: derive the matched set of Intel NEO compute-runtime,
  gmmlib, and IGC deb packages dynamically from the pinned `NEO_VER` release
  metadata at build time (`dev/scripts/fetch-intel-neo.py`, ADR-1145),
  verifying each deb against published sha256 checksums before installation.
  Eliminates recurring HTTP 404 container build failures caused by independent
  Renovate bumps (PR #1184, PR #1205).
- `renovate.json`: delete regex `customManagers` for `intel/gmmlib` and
  `intel/intel-graphics-compiler` that produced invalid isolated updates,
  and standardize regex `fileMatch` patterns.
