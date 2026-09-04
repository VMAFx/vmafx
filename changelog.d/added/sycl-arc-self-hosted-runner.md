### CI: Containerised self-hosted GitHub Actions runner for Intel Arc A380 SYCL parity (ADR-1177)

Added an isolated containerised GitHub Actions runner infrastructure and required CI workflow
for Intel Arc A380 SYCL kernel verification on real discrete GPU silicon.

- **Containerised Runner**: Built via `dev/Containerfile.runner` on top of `vmaf-dev-mcp:local`
  with pinned GitHub Actions runner v2.337.0. Managed through `dev/docker-compose.runner.yml`
  in ephemeral mode (`--ephemeral`), non-root execution (`runner`, UID 1001), no Docker socket,
  and isolated scratch volume.
- **Hardware Isolation**: Passthrough restricted strictly to `/dev/dri/renderD129` (Intel Arc A380,
  PCI `0000:03:00.0`, vendor `0x8086`, device `0x56a5`). The workstation's NVIDIA RTX 4090 and AMD
  processor iGPU are completely excluded.
- **CI Workflow**: `.github/workflows/sycl-parity.yml` runs required job `SYCL Parity (Arc A380)`
  gated by a runner availability probe (`scripts/ci/check-runner-available.sh`). Runs all 23 SYCL
  tests (`meson test -C core/build --suite sycl`) and executes `cross_backend_parity_gate.py` with
  `--gpu-id sycl:0x8086:0x56a5`.
- **Aggregator Integration**: `.github/workflows/required-aggregator.yml` verifies `SYCL Parity (Arc A380)`.
  Accepts absence/skip when unregistered; fails loudly if registered and offline.
- **Operator Runbook**: Documented in `docs/development/ci-self-hosted-sycl.md`.

References: ADR-1177, ADR-0214, ADR-0220, ADR-0234, ADR-0541.
