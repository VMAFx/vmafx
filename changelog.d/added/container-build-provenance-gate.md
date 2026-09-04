- `scripts/ci/check-container-build.sh` makes the container-only publishing
  policy (ADR-1102) enforceable instead of documentation-only. It has three
  fail-closed modes: assert that the current process runs inside the
  `vmaf-dev-mcp` container, `--stamp` a staged artifact tree with a
  `container-build-provenance.txt` receipt (which a host build cannot produce,
  because the assertion runs first), and `--verify` such a tree from anywhere.
  `dev/Containerfile` bakes the `/etc/vmafx-dev-container` marker the gate
  reads into its first stage, so every downstream stage inherits it. Wired into
  `Dev Container Build (PR gate)` (rejects the bare runner, accepts the built
  image, and round-trips a stamp through it) and into `Release Script Contract
  (ADR-1128)`, which runs the gate's hermetic 23-case unit suite on every PR.
