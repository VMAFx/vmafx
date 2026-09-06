### CI: Containerised self-hosted GitHub Actions runner for Intel Arc A380 SYCL parity (ADR-1177)

Added an isolated containerised GitHub Actions runner infrastructure and required CI workflow
for Intel Arc A380 SYCL kernel verification on real discrete GPU silicon.

- **Containerised Runner**: Built via `dev/Containerfile.runner` on top of `vmaf-dev-mcp:local`
  with pinned GitHub Actions runner v2.337.0. Managed through `dev/docker-compose.runner.yml`
  in ephemeral mode (`--ephemeral`), non-root execution (`runner`, UID 1001), no Docker socket,
  and isolated scratch volume.
- **Hardware Isolation**: Passthrough restricted to the Intel Arc A380 render node
  (`/dev/dri/by-path/pci-0000:03:00.0-render`, vendor `0x8086`, device `0x56a5`; `renderD129` today),
  resolved per launch by `dev/scripts/arc-render-node.sh` into `ARC_RENDER_NODE`. The workstation's
  NVIDIA RTX 4090 and AMD processor iGPU are not mapped.
- **CI Workflow**: `.github/workflows/sycl-parity.yml` runs the required job `SYCL Parity (Arc A380)`
  behind a hosted probe (`scripts/ci/check-runner-available.sh`) switched by the repository variable
  `SYCL_ARC_RUNNER_ENABLED` and backed by the `SYCL_RUNNER_PROBE_TOKEN` secret (`GITHUB_TOKEN` cannot
  list self-hosted runners). Runs all 23 SYCL tests (`meson test -C core/build --suite sycl`) and
  `cross_backend_parity_gate.py --gpu-id sycl:0x8086:0x56a5` against the ADR-0234 table
  (`float_ssim: 5.0e-4`, Research-0985 §3).
- **Aggregator Integration**: `.github/workflows/required-aggregator.yml` requires `SYCL Parity (Arc A380)`
  while `SYCL_ARC_RUNNER_ENABLED=true` (a skip = loud failure: runner unregistered, offline, or probe
  token rejected) and accepts absence/skip while the lane is disabled.
- **Host Supervisor & User Service**: Added `dev/scripts/runner-supervisor.sh` and
  `dev/systemd/vmafx-sycl-arc-runner.service` (`systemd --user` unit) to automate
  ephemeral runner re-registration between CI jobs, blocking via `docker wait`, clean
  SIGTERM teardown, exponential backoff on token/docker failures, and daily-driver pause
  support. Validated with hermetic unit tests in `scripts/ci/tests/test-runner-supervisor.sh`
  wired into `.github/workflows/rule-enforcement.yml`.
- **Operator Runbook**: `docs/development/ci-self-hosted-sycl.md` — repository settings (`gh api` calls), image build, isolation proof, supervisor / systemd setup, daily-driver pause flow, re-registration verification, and rotation/removal.

References: ADR-1177, ADR-0214, ADR-0220, ADR-0234, ADR-0541.
