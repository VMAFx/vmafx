<!-- markdownlint-disable MD013 MD060 -->
# Self-hosted GPU runner — enrollment guide

Follow this runbook to enroll a self-hosted runner for the `coverage-gpu` CI
job (in
[`tests-and-quality-gates.yml`](../../.github/workflows/tests-and-quality-gates.yml)).
The job needs a runner that exposes both NVIDIA and Intel GPUs alongside an
AVX-512-capable CPU; hosted GitHub runners cannot reach those code paths. The
Arc-only SYCL parity lane is a separate capability with its own
[operator guide](ci-self-hosted-sycl.md).

This page is a provisioning runbook, not evidence that the hardware lane has
run.

## How the job is admitted

The `coverage-gpu` job runs only after a hosted admission job sees both
`GPU_COVERAGE_ENABLED=true` and an online runner carrying every label in its
`runs-on` set.

- A missing, partial or offline match fails the hosted probe and skips
  self-hosted dispatch, so the hardware job cannot queue forever.
- While the switch is enabled, the required-check aggregator treats that skip
  as a failure.

This is the
[ADR-1319](../adr/1319-fail-closed-self-hosted-gpu-admission.md) contract.

!!! note
    State on 2026-09-25 (dated snapshot): the repository and organisation
    runner APIs both returned zero runners, `GPU_COVERAGE_ENABLED` was absent,
    and the documented local runner service, environment and state paths were
    absent. Re-check with
    `gh api /repos/VMAFx/vmafx/actions/runners --jq '.total_count'` and
    `gh variable list -R VMAFx/vmafx`.

Historical backlog reference: T7-3. The local `.workingdir/BACKLOG.md`
notebook is not part of the published documentation; the linked workflow is
the current configuration source.

## Required labels

The workflows match on a label triple. Match these exactly:

| Label | Meaning |
|---|---|
| `self-hosted` | GitHub default for any non-hosted runner. |
| `linux` | OS family. Bare-metal Linux only — WSL2 and containers can't pass `cudaSetDevice` to the host driver reliably. |
| `gpu-full` | The fork's combined CUDA + SYCL + AVX-512 tag — at minimum NVIDIA + Intel + AVX-512. It does not claim HIP coverage. Add fine-grained labels (`gpu-cuda`, `gpu-intel`, `avx512`) so future jobs can target a subset without re-tagging. |

## Hardware expectations

The runner needs to satisfy the union of all jobs that target it. Toolkit
versions are the ones pinned in `build-config.env` (`CUDA_VERSION`,
`ONEAPI_VERSION`); an older toolkit cannot build the current tree
([ADR-1223](../adr/1223-cuda-ampere-architecture-floor.md) raises the CUDA floor
to Ampere,
compute capability 8.0).

- **NVIDIA GPU + driver + the pinned CUDA toolkit** — drives the
  `coverage-gpu` CUDA build (`-Denable_cuda=true`) and the CUDA test
  suite. `nvidia-smi` must succeed without `sudo`.
- **Intel GPU + Level Zero + the pinned oneAPI toolkit** — drives
  the SYCL build (`-Denable_sycl=true`). `sycl-ls` must list at least
  one Intel GPU device.
- **Intel `ocloc`** — the SYCL build compiles native GPU code ahead of time
  and `meson setup` refuses without `ocloc` on `PATH`
  ([ADR-1360](../adr/1360-sycl-aot-compile-time-device-codegen.md)). Run
  `bash scripts/ci/install-intel-ocloc.sh` once on the host; it installs the
  release pinned as `INTEL_NEO_VERSION` in `build-config.env`.
- **AVX-512-capable CPU** (Ice Lake or newer / Zen 4 or newer) — the
  AVX-512 SIMD code paths. `lscpu | grep avx512f` should return a hit.
- **`zstd` on `PATH`** — the PQ fixture bundles are `.tar.zst` archives that
  `scripts/test/fetch-test-yuvs.sh --bundle` unpacks with `tar`, which calls
  `zstd`; the script stops with `zstd is not on PATH` before any download
  without it. Install it with the distribution's `zstd` package.
- **≥ 16 GB RAM** — coverage + multi-backend builds peak around 8 GB
  resident; doubling that gives headroom for parallel CPU test processes.
  Tests tagged `gpu` run exclusively because they share one accelerator.
- **≥ 60 GB free disk** — coverage builds + nightly artifact retention
  rotate through ~30 GB.
- **HIP is not part of this job.** An AMD device on the host does not make
  `Coverage GPU` a HIP gate; HIP needs its own executable job and capability
  label before CI can claim hardware coverage.

A typical workstation that runs the fork's local dev loop already
satisfies all of the above. Per maintainer direction (2026-04-25), the
primary dev workstation is the first runner.

## Enrollment steps

These are the canonical GitHub Actions runner setup steps, lightly
adapted for this fork's labels.

### 1. Generate a registration token

The token is short-lived (1 hour) and single-use. Generate one in
the GitHub UI:

`https://github.com/VMAFx/vmafx/settings/actions/runners/new`

Or via `gh`:

```bash
gh api -X POST \
  /repos/VMAFx/vmafx/actions/runners/registration-token \
  --jq .token
```

### 2. Install the runner agent

Pick a working directory the agent will own (e.g. `~/actions-runner`):

```bash
mkdir -p ~/actions-runner && cd ~/actions-runner

# Pin to a known release; bump deliberately. 2.319.1 is an example:
# take the current release from https://github.com/actions/runner/releases.
RUNNER_VERSION=2.319.1
curl -O -L \
  "https://github.com/actions/runner/releases/download/v${RUNNER_VERSION}/actions-runner-linux-x64-${RUNNER_VERSION}.tar.gz"
echo "<sha256-from-release-page>  actions-runner-linux-x64-${RUNNER_VERSION}.tar.gz" | sha256sum -c -
tar xzf "actions-runner-linux-x64-${RUNNER_VERSION}.tar.gz"
```

### 3. Configure with the fork's labels

```bash
./config.sh \
  --url https://github.com/VMAFx/vmafx \
  --token "<token from step 1>" \
  --labels self-hosted,linux,gpu-full,gpu-cuda,gpu-intel,avx512 \
  --name "$(hostname)-gpu-full" \
  --work _work \
  --replace
```

The fine-grained labels (`gpu-cuda`, `gpu-intel`, `avx512`) are not
required by any current workflow but are reserved so future jobs can
match a subset of capabilities without forcing a label change.

### 4. Run as a systemd service (recommended)

```bash
sudo ./svc.sh install "$USER"
sudo ./svc.sh start
sudo ./svc.sh status
```

The service auto-starts on boot and respawns on crash. Logs land in
`~/actions-runner/_diag/` and `journalctl -u actions.runner.VMAFx-vmafx.*`.

### 5. Verify it's online

```bash
gh api /repos/VMAFx/vmafx/actions/runners --jq '.runners[] | {name, status, labels: [.labels[].name]}'
```

The runner should appear with `"status": "online"` and the full label
set. Trigger the `coverage-gpu` job once with `gh workflow run` to
confirm end-to-end:

```bash
gh variable set GPU_COVERAGE_ENABLED -b true -R VMAFx/vmafx
gh workflow run tests-and-quality-gates.yml
```

Enable the variable **last**. The hosted probe must first report a complete
online `self-hosted,linux,gpu-full` match. The `Coverage GPU` check is already
required while enabled; a failed probe produces a skipped hardware check that
the aggregator rejects.

## Operational notes

- **GPU driver upgrades**: stop the agent (`sudo ./svc.sh stop`),
  drain in-flight jobs (the runner finishes the current job before
  exiting), upgrade, restart. The runner deregisters automatically
  on prolonged offline.
- **Disk hygiene**: GitHub Actions does **not** clean `_work/` on its
  own. A nightly `find _work -mtime +14 -delete` is standard. The
  fork's coverage artifacts retain for 14 days on the GitHub side, so
  local rotation at the same cadence is safe.
- **Secrets**: this runner has access to repo + organisation secrets
  scoped to the `gpu-coverage` environment if and when one is added.
  Treat the host as security-sensitive — no third-party SSH keys, no
  shared user accounts, audit `~/.ssh/authorized_keys` quarterly.
- **Fork pull requests**: the hosted probe excludes fork heads, so untrusted
  fork code never reaches this self-hosted runner.
- **Concurrency**: see [GPU test serialisation](#gpu-test-serialisation) below.
- **Second runner**: adding a runner with the same label set (for example a
  remote Intel-only or NVIDIA-only host) lets `coverage-gpu` parallelise with
  whatever fine-grained-label job comes next without label collisions.

### GPU test serialisation

A single runner serialises GPU jobs.

- Inside a Meson test job, every test tagged `gpu` sets `is_parallel : false`.
  Meson drains running tests before each one and does not start another until
  it finishes.
- Keep the normal parallel Meson invocation. `-j1` and inflated timeouts hide
  accelerator contention instead of enforcing the shared-device contract.
- The source guard covers dormant backends that cannot be configured on the
  current host; the metadata guard checks Meson's effective scheduling flags.

Verify both the source registry and the configured metadata:

```bash
python3 "$(git rev-parse --show-toplevel)/scripts/ci/run_meson_test.py" -- \
  -C build --no-rebuild test_gpu_serialization_contract check_gpu_test_serialization
```

## Decommissioning

```bash
# Disable admission before taking the runner offline.
gh variable set GPU_COVERAGE_ENABLED -b false -R VMAFx/vmafx
cd ~/actions-runner
sudo ./svc.sh stop && sudo ./svc.sh uninstall
./config.sh remove --token "$(gh api -X POST \
  /repos/VMAFx/vmafx/actions/runners/remove-token --jq .token)"
rm -rf ~/actions-runner
```

## References

- Historical T7-3 reference: `.workingdir/BACKLOG.md` (local notebook,
  not shipped with this documentation).
- [`tests-and-quality-gates.yml` §
  coverage-gpu](../../.github/workflows/tests-and-quality-gates.yml) —
  the first consumer of the `gpu-full` label.
- [GitHub Actions: self-hosted
  runners](https://docs.github.com/en/actions/hosting-your-own-runners/managing-self-hosted-runners/about-self-hosted-runners)
- Maintainer direction, 2026-04-25 (paraphrased): use the local workstation GPUs
  for CUDA and Intel testing for now.
