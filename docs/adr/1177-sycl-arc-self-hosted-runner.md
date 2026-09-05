<!-- markdownlint-disable MD013 MD060 -->
# ADR-1177: Containerised self-hosted GitHub Actions runner for Intel Arc SYCL parity CI

- **Status:** Accepted
- **Date:** 2026-09-04
- **Deciders:** Lusoris
- **Supersedes:** none
- **Superseded by:** none
- **Tags:** sycl, gpu, ci, runner, arc-a380, docker

## Context

The VMAFx fork maintains an extensive SYCL GPU backend across all registered feature extractors,
subject to strict numeric parity gates against the CPU reference implementation (ADR-0214,
ADR-0220, ADR-0234). Hosted GitHub Actions infrastructure (e.g. `ubuntu-latest`, `ubuntu-24.04`)
provides CPU and software emulation (Mesa lavapipe) but provides no physical Intel discrete GPU
silicon or Level-Zero userspace compute runtime (`intel-opencl-icd`, `libze-intel-gpu1`).
Consequently, SYCL kernel execution was historically unexercised in hosted pull-request CI,
relying on manual developer verification or post-merge checks.

The workstation environment running CachyOS contains an Intel Arc A380 discrete GPU
(PCI `0000:03:00.0`, vendor `0x8086`, device `0x56a5`, DG2-G10 die class) coexisting with an
NVIDIA GeForce RTX 4090 and an AMD Raphael processor integrated GPU. To establish an automated,
reliable CI gate for SYCL kernel execution on real Intel silicon without compromising host
security, polluting the host OS, or interfering with adjacent accelerators, a containerised
self-hosted GitHub Actions runner architecture is required.

## Decision

1. **Containerised runner isolation**: The runner executes within a dedicated, isolated Docker
   container (`vmaf-sycl-arc-runner:local`, defined in `dev/Containerfile.runner`) built on top
   of the verified oneAPI environment (`vmaf-dev-mcp:local`). The runner never runs as a host
   service or systemd unit.
2. **Strict device node isolation**: Only the Intel Arc A380 DRI render node is passed into the
   container:
   - Host path: `/dev/dri/by-path/pci-0000:03:00.0-render` (vendor `0x8086`, device `0x56a5`),
     `renderD129` at the time of writing. Because render-node numbers and PCI BDFs change after a PCI
     re-enumeration, the compose file takes the node from `ARC_RENDER_NODE`, resolved by
     `dev/scripts/arc-render-node.sh` (exactly one `0x8086` render node, else it refuses).
   - Neither the NVIDIA RTX 4090 render node (`renderD128`) nor the AMD iGPU render node
     (`renderD130`) is mapped.
   - Verified inside the container via `sycl-ls`: Level-Zero and OpenCL platforms see exclusively
     the Intel Arc A380 Graphics adapter.
3. **Non-root execution and permissions**:
   - The runner runs as unprivileged user `runner` (uid 1001, gid 1001), member of host render
     group (gid 988) and video group (gid 984).
   - Container security options specify `seccomp=unconfined` as required by Intel Level-Zero NEO
     runtime 26.x on Linux kernel >= 7.0 (ADR-0541) to avoid `zeInit()` permission denials.
   - The host Docker daemon socket (`/var/run/docker.sock`) is NOT mounted into the container.
4. **Ephemeral runner lifecycle**:
   - Configured with `--ephemeral`: the runner registers, processes exactly one job, cleanly
     unregisters from GitHub Actions, and terminates.
   - Workspace data is stored in a named Docker volume (`runner-scratch`) at
     `/actions-runner/_work`, with no host directory bind mounts. The image creates that directory
     owned by `runner` so the volume is seeded writable (a root-owned mount point made `config.sh`
     fail with `EACCES` in the first build).
   - Container resource limits are bounded to 8 CPUs and 16 GB memory.
5. **Software version pinning**:
   - Pinned to GitHub Actions Runner `v2.337.0` (linux-x64) with SHA256 checksum verification:
     `70920811a4f8ad4328818682bca5c6469c1c942fab52448868071d0063816613`.
6. **Workflow and security gating**:
   - Workflow `.github/workflows/sycl-parity.yml` defines the REQUIRED CI job `SYCL Parity (Arc A380)`
     (name <= 30 characters and carrying `# required-aggregator: SYCL Parity (Arc A380)` for PR #1286
     compliance).
   - Strict security guard: Never runs on untrusted fork pull requests (`head.repo.full_name == github.repository`).
   - Job 1 (`runner-available`) on `ubuntu-24.04` runs `scripts/ci/check-runner-available.sh`. The
     lane is switched by the repository variable `SYCL_ARC_RUNNER_ENABLED`, not by auto-detecting the
     runner: `GET /repos/{owner}/{repo}/actions/runners` needs the *Administration: read* repository
     permission, which the workflow `permissions:` key cannot grant to `GITHUB_TOKEN`. Lane disabled:
     `available=false`, exit 0, Job 2 skips. Lane enabled: the probe queries the runner list with the
     `SYCL_RUNNER_PROBE_TOKEN` secret (fine-grained PAT, Administration: read-only) and requires an
     ONLINE runner labelled `sycl-arc`; a missing/403 token, no registered runner, or an offline runner
     all fail loudly (exit 1, `::error::`) — an API error is never interpreted as "unregistered".
   - Job 2 (`sycl-parity`) runs on `[self-hosted, linux, x64, sycl-arc]`, verifies GPU visibility
     with `sycl-ls`, compiles with `-Denable_sycl=true -Denable_float=true`, runs all 23 SYCL tests
     via `meson test -C core/build --suite sycl`, and executes `scripts/ci/cross_backend_parity_gate.py`
     with `--gpu-id sycl:0x8086:0x56a5`.
   - Generates and uploads `sycl_parity.json` and `sycl_parity.md` artifacts.
7. **Aggregator integration**:
   - `.github/workflows/required-aggregator.yml` includes `'SYCL Parity (Arc A380)'` in `required`.
   - The aggregator reads the same `SYCL_ARC_RUNNER_ENABLED` variable (no runner API call): lane disabled →
     absence or `skipped` is accepted; lane enabled → the job must report `success`, and absence or
     `skipped` (the probe failed) is a hard CI failure. The operator pauses the lane by flipping the
     variable to `false` *before* stopping the container (daily-driver workstation).
8. **Test suite tagging**:
   - All 23 SYCL parity and unit tests in `core/test/meson.build` are registered with `suite : ['fast', 'gpu', 'sycl']`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Host systemd runner service** | Direct hardware and driver access without container overhead | Mutates host system packages; risks container breakout / privilege escalation; exposes RTX 4090 and AMD iGPU; subject to host rolling-release toolchain drift on CachyOS | Violates multi-GPU isolation and host immutability requirements |
| **Privileged Docker with `/dev/dri` pass-all** | Simple Docker flags (`--device /dev/dri`) | Exposes RTX 4090 (`renderD128`) and AMD iGPU (`renderD130`) inside runner container; breaks isolation | Violates strict requirement to isolate Intel Arc A380 |
| **Auto-detect the runner instead of an operator switch** | No variable to flip | `GITHUB_TOKEN` cannot list self-hosted runners (Administration: read is not grantable via `permissions:`), so a probe that treats a 403 as "unregistered" makes the required check silently green forever; an ephemeral runner is also unregistered between jobs, so "registered" is not a stable signal | Explicit `SYCL_ARC_RUNNER_ENABLED` variable + PAT-backed online probe; API errors fail loudly |
| **Hosted oneAPI CPU-device lane** (`opencl:cpu` on `ubuntu-24.04`) | No self-hosted infrastructure | Runs the SYCL kernels on a CPU device: none of the Arc-specific fp32 / fp64-free behaviour that the ADR-0234 calibration exists for is exercised | Rejected by the maintainer — Arc-specific calibration is the point |
| **Local-only gate** (developer runs `--suite sycl` before pushing) | Zero CI cost | Not enforced; exactly how #865 merged | Rejected |
| **Persistent non-ephemeral runner container with Docker socket** | Retains warm build cache across jobs | Runner contamination between untrusted runs; docker socket enables host root escalation; residual files persist | Ephemeral mode with isolated scratch volume is strictly safer |
| **Cloud GPU runner (AWS/Azure/GCP)** | Fully managed, no local workstation reliance | No major hyperscaler provides affordable Intel Arc discrete GPUs with Level-Zero support | Silicon is physically available on local workstation |

## Consequences

- **Positive**:
  - Provides real-hardware Intel Arc A380 Level-Zero kernel execution verification in CI.
  - Guarantees complete isolation from NVIDIA RTX 4090 and AMD iGPU.
  - Ephemeral runner lifecycle prevents workspace pollution and credential retention.
  - Required checks aggregator tolerates absence while the lane is disabled and fails loudly (never silently green) when the lane is enabled and the runner is missing, offline, or the probe token is rejected.
  - Targeted Meson test suite (`--suite sycl`) runs all 23 SYCL tests in ~15 seconds without running CUDA tests.
- **Negative**:
  - Requires workstation operator action to obtain a registration token and launch the container; the ephemeral runner disappears after each job, so a host-side loop (runbook §3) is needed for a review session, and the lane must be disabled before the workstation is used for something else.
  - Requires a fine-grained PAT (`SYCL_RUNNER_PROBE_TOKEN`, Administration: read-only, single repository) as a repository secret.
- **Neutral / follow-ups**:
  - Operator runbook documented in `docs/development/ci-self-hosted-sycl.md`.
  - Future calibration sweeps for remaining SYCL features (`adm`, `vif`, `motion`, `ciede`, `ssimulacra2` Kahan-IIR).

## References

- Maintainer decision (paraphrased, 2026-09-04): register the workstation's Arc A380 as a self-hosted runner so SYCL parity becomes a required CI check; the runner must be containerised, expose only the Arc, and never run fork code.
- [ADR-0214](0214-gpu-parity-ci-gate.md) — GPU-parity CI gate.
- [ADR-0220](0220-sycl-fp64-fallback.md) — SYCL fp64-less device contract.
- [ADR-0234](0234-gpu-gen-ulp-calibration.md) — Per-GPU-generation ULP calibration table.
- [ADR-0313](0313-ci-required-checks-aggregator.md) — Required checks aggregator.
- [ADR-0541](0541-dev-container-sycl-hip-runtime-fix.md) — dev-container SYCL/HIP runtime fix (NEO matched to the host kernel, `seccomp=unconfined`).
- [Research-0985](../research/0985-sycl-parity-divergence-2026-06-03.md) §3 — DG2-G10 float_ssim two-cause divergence; source of the `float_ssim: 5.0e-4` (places=3) tolerance, matching `PARITY_TOL` in `core/test/test_sycl_float_ssim_parity.c`.
- GitHub REST: `GET /repos/{owner}/{repo}/actions/runners` requires *Administration (read)*; the `permissions:` key for `GITHUB_TOKEN` has no `administration` scope (docs, workflow-syntax § permissions, checked 2026-09-05).
