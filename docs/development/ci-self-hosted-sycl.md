<!-- markdownlint-disable MD013 -->
# Intel Arc A380 Containerised Self-Hosted Runner (SYCL CI)

This document provides the architecture, security model, and operator runbook for the
containerised self-hosted GitHub Actions runner used for Intel Arc A380 SYCL parity CI.

Governing ADR: [ADR-1177](../adr/1177-sycl-arc-self-hosted-runner.md).

---

## 1. Architecture and Security Model

The runner executes real Intel Arc A380 GPU kernels in CI while preserving strict host isolation
on the operator's workstation (CachyOS Linux):

- **Containerised Isolation**: Built on top of `vmaf-dev-mcp:local` via
  [`dev/Containerfile.runner`](../../dev/Containerfile.runner). The runner executes inside a
  container; it never runs as a host service or systemd daemon.
- **Hardware Isolation (Multi-GPU System)**:
  - The host workstation houses an Intel Arc A380 (PCI `0000:03:00.0`), an NVIDIA GeForce RTX 4090,
    and an AMD Raphael processor iGPU.
  - ONLY the Intel Arc A380 DRI node is exposed:
    `/dev/dri/renderD129` (pointing to `/dev/dri/by-path/pci-0000:03:00.0-render`, vendor `0x8086`, device `0x56a5`).
  - Neither NVIDIA (`renderD128`) nor AMD (`renderD130`) device nodes are mounted.
- **Unprivileged Execution**:
  - Executes as non-root user `runner` (UID 1001, GID 1001).
  - Member of host `render` (`988`) and `video` (`984`) groups.
  - No host Docker socket (`/var/run/docker.sock`) is mounted.
  - Ephemeral scratch volume (`runner-scratch`) at `/actions-runner/_work`; no host filesystem bind mounts.
- **Seccomp Profile**:
  - `seccomp=unconfined` is applied per [ADR-0541](../adr/0541-level-zero-seccomp-unconfined.md).
    Intel Level-Zero NEO runtime 26.x requires unconfined seccomp on Linux kernel >= 7.0 for ioctl/syscall compatibility.
- **Resource Limits**:
  - Bounded to 8 CPUs and 16 GB memory in [`dev/docker-compose.runner.yml`](../../dev/docker-compose.runner.yml).
- **Ephemeral Mode**:
  - Configured with `--ephemeral`. The runner processes exactly one CI job, unregisters from GitHub Actions,
    and stops.

---

## 2. CI Workflow and Aggregator Integration

- **Workflow**: [`.github/workflows/sycl-parity.yml`](../../.github/workflows/sycl-parity.yml)
  - Job: `SYCL Parity (Arc A380)` (name <= 30 characters; `# required-aggregator: SYCL Parity (Arc A380)`).
  - **Fork Safety**: Restricted to internal pushes and same-repo PRs. Untrusted fork PRs never execute on self-hosted hardware.
  - **Graceful Degradation vs Loud Failure**:
    - Job 1 (`runner-available`) probes runner registration via [`scripts/ci/check-runner-available.sh`](../../scripts/ci/check-runner-available.sh).
    - If NO runner with label `sycl-arc` is registered: Job 2 skips cleanly; [`.github/workflows/required-aggregator.yml`](../../.github/workflows/required-aggregator.yml) permits absence/skip.
    - If a runner IS registered but offline: the probe fails loudly (exit code 1) and the aggregator rejects the check.
- **Verification Gates**:
  - Compiles with `-Denable_sycl=true -Denable_float=true`.
  - Runs all 23 SYCL tests: `meson test -C core/build --suite sycl`.
  - Runs cross-backend parity gate: `python3 scripts/ci/cross_backend_parity_gate.py --gpu-id sycl:0x8086:0x56a5 --features float_ssim`.
  - Uploads `sycl_parity.json` and `sycl_parity.md` artifacts.

---

## 3. Operator Runbook

All commands are run from the repository root on the host workstation.

### Step 1: Build the Runner Image

Ensure the base dev container image (`vmaf-dev-mcp:local`) is current, then build the runner image:

```bash
docker build -t vmaf-sycl-arc-runner:local -f dev/Containerfile.runner .
```

### Step 2: Smoke-Test Device Visibility Inside Image

Verify that only the Intel Arc A380 is visible before registering with GitHub:

```bash
docker run --rm \
  --device /dev/dri/renderD129 \
  --security-opt seccomp=unconfined \
  --group-add 988 \
  vmaf-sycl-arc-runner:local sycl-ls
```

Expected output:

- Level-Zero: `[level_zero:gpu] ... Intel(R) Arc(TM) A380 Graphics ...`
- OpenCL: `[opencl:gpu] ... Intel(R) Arc(TM) A380 Graphics ...`
- Zero NVIDIA devices (RTX 4090 hidden).
- Zero AMD GPUs (iGPU hidden).

### Step 3: Fetch Registration Token

Generate an ephemeral registration token via the GitHub CLI:

```bash
export RUNNER_TOKEN="$(gh api -X POST repos/VMAFx/vmafx/actions/runners/registration-token --jq .token)"
```

### Step 4: Start the Ephemeral Runner

Launch the container in the background:

```bash
docker compose -f dev/docker-compose.runner.yml up -d
```

Follow runner logs:

```bash
docker compose -f dev/docker-compose.runner.yml logs -f
```

The runner will configure itself, listen for a matching job, execute the `SYCL Parity (Arc A380)` job,
unregister itself, and exit.

### Step 5: Stop or Clean Up

To manually stop or pause the runner:

```bash
docker compose -f dev/docker-compose.runner.yml down
```

To clean scratch volumes:

```bash
docker compose -f dev/docker-compose.runner.yml down -v
```

---

## 4. GitHub Repository Security Configuration

In GitHub Repository Settings (`Settings` -> `Actions` -> `General`):

1. **Fork Pull Request Workflows**: Set to **"Require approval for all outside collaborators"** or **"Require approval for first-time contributors"**.
2. **Workflow Permissions**: Read repository contents and packages permissions.
3. **Runner Groups**: Use default runner group with repository-scoped access.
