<!-- markdownlint-disable MD013 MD060 -->
# Self-hosted GPU runner — enrollment guide

A small set of CI jobs (`coverage-gpu` in
[`tests-and-quality-gates.yml`](../../.github/workflows/tests-and-quality-gates.yml),
plus future SYCL / CUDA-specific suites) require a self-hosted runner
that exposes both NVIDIA and Intel GPUs alongside an AVX-512-capable
CPU. Hosted GitHub runners can't reach those code paths.

Until a runner is enrolled, every `runs-on: [self-hosted, linux, gpu-full]`
job queues forever. This guide pins the enrollment steps so the next
operator can stand a runner up in ~10 minutes.

Tracked as backlog item T7-3.

## Required labels

The workflows match on a label triple. Match these exactly:

| Label | Meaning |
|---|---|
| `self-hosted` | GitHub default for any non-hosted runner. |
| `linux` | OS family. Bare-metal Linux only — WSL2 and containers can't pass `cudaSetDevice` to the host driver reliably. |
| `gpu-full` | The fork's "has all the GPUs we test against" tag — at minimum NVIDIA + Intel + AVX-512. Add additional fine-grained labels (`gpu-cuda`, `gpu-intel`, `avx512`) so future jobs can target a subset without re-tagging. |

## Hardware expectations

The runner needs to satisfy the union of all jobs that target it:

- **NVIDIA GPU + driver + CUDA toolkit ≥ 12.0** — drives the
  `coverage-gpu` CUDA build (`-Denable_cuda=true`) and the CUDA test
  suite. `nvidia-smi` must succeed without `sudo`.
- **Intel GPU + Level Zero + oneAPI Base Toolkit ≥ 2024.2** — drives
  the SYCL build (`-Denable_sycl=true`). `sycl-ls` must list at least
  one Intel GPU device.
- **AVX-512-capable CPU** (Ice Lake or newer / Zen 4 or newer) — the
  AVX-512 SIMD code paths. `lscpu | grep avx512f` should return a hit.
- **≥ 16 GB RAM** — coverage + multi-backend builds peak around 8 GB
  resident; doubling that gives headroom for the parallel meson test
  runs.
- **≥ 60 GB free disk** — coverage builds + nightly artifact retention
  rotate through ~30 GB.

A typical workstation that runs the fork's local dev loop already
satisfies all of the above; the user's primary dev box has been
greenlit (per popup 2026-04-25) as the first runner.

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

# Pin to a known release; bump deliberately.
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
gh variable set GPU_COVERAGE_ENABLED -b true
gh workflow run tests-and-quality-gates.yml
```

Once the job completes green twice in a row, the runner is considered
stable; the workflow's `continue-on-error` flag can be flipped to
required (separate ADR + PR).

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
- **Concurrency**: a single runner serialises GPU jobs. Adding a
  second runner with the same label set (e.g. a remote Intel-only or
  NVIDIA-only host) lets `coverage-gpu` parallelise with whatever
  fine-grained-label job comes next without label collisions.

## Decommissioning

```bash
cd ~/actions-runner
sudo ./svc.sh stop && sudo ./svc.sh uninstall
./config.sh remove --token "$(gh api -X POST \
  /repos/VMAFx/vmafx/actions/runners/remove-token --jq .token)"
rm -rf ~/actions-runner
```

## References

- [BACKLOG T7-3](../../.workingdir2/BACKLOG.md) — backlog row.
- [`tests-and-quality-gates.yml` § coverage-gpu](../../.github/workflows/tests-and-quality-gates.yml) —
  the first consumer of the `gpu-full` label.
- [GitHub Actions: self-hosted runners](https://docs.github.com/en/actions/hosting-your-own-runners/managing-self-hosted-runners/about-self-hosted-runners)
- `req` — user popup choice 2026-04-25: "we can test cuda and intel
  on my pc, so just use my local gpu's for now lol".
