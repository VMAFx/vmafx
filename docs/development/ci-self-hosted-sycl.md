<!-- markdownlint-disable MD013 -->
# Intel Arc A380 containerised self-hosted runner (SYCL parity CI)

Architecture, security model and operator runbook for the containerised
self-hosted GitHub Actions runner that executes the `SYCL Parity (Arc A380)`
check on the maintainer's workstation. Governing ADR:
[ADR-1177](../adr/1177-sycl-arc-self-hosted-runner.md). Sibling page for the
older, never-provisioned `gpu-full` runner design:
[self-hosted-runner.md](self-hosted-runner.md).

Why it exists: until this lane runs, no CI job has ever executed a SYCL
kernel. `SYCL float_ssim Parity (Arc DG2-G10)` in
`tests-and-quality-gates.yml` is gated on `vars.GPU_COVERAGE_ENABLED` (unset)
and a `gpu-full` runner that was never registered, so the divergent
ssimulacra2 blur change (#865) merged unnoticed (`docs/state.md` row
`T-SYCL-ARC-SSIMULACRA2-PARITY-2026-06-03`).

---

## 1. Architecture and security model

The runner execution environment is always an isolated Docker container, never
running arbitrary CI jobs directly on the host. Lifecycle management (waiting for
job completion, minting registration tokens, starting fresh containers) is handled
by the host supervisor loop, which runs either as a user script or as a
`systemd --user` service unit. The workstation is a daily driver with three GPUs;
the container sees one.

| Property | Value | Where |
| --- | --- | --- |
| Image | `vmaf-sycl-arc-runner:local`, `FROM vmaf-dev-mcp:local` (oneAPI + NEO already inside) plus the official `actions/runner` tarball | [`dev/Containerfile.runner`](../../dev/Containerfile.runner) |
| Runner version | `v2.337.0`, tarball SHA-256 `70920811a4f8ad4328818682bca5c6469c1c942fab52448868071d0063816613`, verified at build time | `Containerfile.runner` `ARG RUNNER_VERSION` / `RUNNER_SHA256` |
| GPU exposed | the Intel Arc A380 render node only: `/dev/dri/by-path/pci-0000:03:00.0-render` (vendor `0x8086`, device `0x56a5`), currently `renderD129`. The RTX 4090 (`pci-0000:06:00.0`, `renderD128`) and the AMD iGPU (`pci-0000:7d:00.0`, `renderD130`) are not mapped | [`dev/docker-compose.runner.yml`](../../dev/docker-compose.runner.yml) `devices:` |
| User | `runner` (uid 1001, gid 1001) in the host `render` (988) and `video` (984) groups; never root | `Containerfile.runner`, compose `group_add:` |
| Mounts | one named scratch volume at `/actions-runner/_work`; no host bind mounts; no Docker socket | compose `volumes:` |
| Limits | 8 CPUs, 16 GB RAM | compose `deploy.resources.limits` |
| Lifecycle | `--ephemeral`: register, wait, run exactly one job, unregister, exit | [`dev/scripts/runner-entrypoint.sh`](../../dev/scripts/runner-entrypoint.sh) |
| seccomp | `seccomp=unconfined`, required by the Level-Zero NEO runtime on Linux ≥ 7.0 ([ADR-0541](../adr/0541-dev-container-sycl-hip-runtime-fix.md)) | compose `security_opt:` |
| Scope | repository-level runner on `VMAFx/vmafx` (not an org runner group) | registration token endpoint |

What runs on it: the `sycl-parity` job of
[`.github/workflows/sycl-parity.yml`](../../.github/workflows/sycl-parity.yml)
for pushes to `master`, `workflow_dispatch`, and non-draft pull requests
whose head branch lives in `VMAFx/vmafx`.

What never runs on it: fork pull requests (the workflow's `if:` requires
`github.event.pull_request.head.repo.full_name == github.repository`), draft
PRs, any other workflow (nothing else carries `runs-on: [..., sycl-arc]`),
and anything via `pull_request_target` — that trigger must not appear in a
workflow that targets this label.

Blast radius: a job runs as an unprivileged user inside a container that
holds no credentials beyond the job's own `GITHUB_TOKEN` (`contents: read`),
sees one GPU, has no Docker socket, no host filesystem, and is destroyed
(container exit + runner unregistered) after one job. The scratch volume is
reused across jobs on the same host — `down -v` wipes it.

---

## 2. CI wiring

- `runner-available` (hosted, `ubuntu-24.04`) runs
  [`scripts/ci/check-runner-available.sh`](../../scripts/ci/check-runner-available.sh)
  and gates the self-hosted job through `needs:`.
- `SYCL Parity (Arc A380)` (self-hosted) checks that only the Arc is
  visible (`sycl-ls`), builds `-Denable_sycl=true -Denable_cuda=false
  -Denable_float=true`, runs `meson test -C core/build --suite sycl`
  (23 tests), then `scripts/ci/cross_backend_parity_gate.py --backends cpu
  sycl --features float_ssim --gpu-id sycl:0x8086:0x56a5` against the
  [ADR-0234](../adr/0234-gpu-gen-ulp-calibration.md) table
  (`scripts/ci/gpu_ulp_calibration.yaml`, `sycl:0x8086:0x56a*`) and uploads
  `sycl_parity.json` / `sycl_parity.md`.
- [`.github/workflows/required-aggregator.yml`](../../.github/workflows/required-aggregator.yml)
  lists `SYCL Parity (Arc A380)` as required.

### The lane switch and the two failure modes

`GITHUB_TOKEN` cannot list self-hosted runners (that endpoint needs the
*Administration: read* repository permission, which the workflow
`permissions:` key cannot grant), so the lane is switched explicitly by the
repository variable `SYCL_ARC_RUNNER_ENABLED`:

| `SYCL_ARC_RUNNER_ENABLED` | probe result | parity job | aggregator |
| --- | --- | --- | --- |
| unset / not `true` | `available=false`, exit 0 | skipped | accepts absence or skip (lane not provisioned) |
| `true`, runner online | `available=true`, exit 0 | runs | requires `success` |
| `true`, runner not registered or offline | `::error::`, exit 1 | skipped | **fails**: "SYCL_ARC_RUNNER_ENABLED=true but the job was skipped" |
| `true`, probe token missing / 403 | `::error::`, exit 1 | skipped | **fails** (same path) |

Offline is loud, never silently green. While the lane is enabled, the probe
queries `GET /repos/VMAFx/vmafx/actions/runners` with the repository secret
`SYCL_RUNNER_PROBE_TOKEN` (a fine-grained personal access token restricted to
`VMAFx/vmafx` with *Administration: Read-only*; it falls back to
`github.token`, which fails with 403 — deliberately loud).

---

## 3. Operator runbook

All commands run from the repository root on the workstation, with `gh`
authenticated as a repository admin. Nothing here is run by CI.

### Step 0 — one-time repository settings (review, then run)

```bash
# Fork PRs: require approval for every outside collaborator before any
# workflow runs (defence in depth; the SYCL jobs already exclude fork heads).
gh api -X PUT repos/VMAFx/vmafx/actions/permissions/fork-pr-contributor-approval \
  -f approval_policy=all_external_contributors

# Probe token: fine-grained PAT, resource owner VMAFx, repository access
# "Only select repositories" -> VMAFx/vmafx, permission Administration: Read-only.
# Note: during initial lane bootstrap, SYCL_RUNNER_PROBE_TOKEN was seeded with
# the maintainer's personal `gh` OAuth token. It should be rotated to a dedicated
# fine-grained PAT with Administration: Read-only scoped strictly to VMAFx/vmafx.
# Create it at https://github.com/settings/personal-access-tokens/new, then:
gh secret set SYCL_RUNNER_PROBE_TOKEN -R VMAFx/vmafx   # paste the token

# Confirm no workflow targets the label via pull_request_target (must print nothing):
git grep -l 'pull_request_target' -- .github/workflows | xargs -r grep -l 'sycl-arc'
```

Runner groups: registering with the repository token (Step 3) creates a
repository-level runner, which lives outside org runner groups and cannot be
shared with other repositories. If the runner is ever moved to the `VMAFx`
org level, restrict its group first:
`gh api -X PATCH orgs/VMAFx/actions/runner-groups/<id> -f visibility=selected`
plus `selected_repository_ids`.

### Step 1 — build the image

`vmaf-dev-mcp:local` must exist and be current
([dev-mcp.md](dev-mcp.md)); the runner image is a thin layer on top
(≈14.9 GB content, of which the runner adds ≈0.2 GB).

```bash
nohup docker build -t vmaf-sycl-arc-runner:local -f dev/Containerfile.runner . \
  > /tmp/runner-image-build.log 2>&1 &
tail -f /tmp/runner-image-build.log   # "naming to docker.io/library/vmaf-sycl-arc-runner:local"
```

### Step 2 — resolve the Arc node and prove isolation

```bash
export ARC_RENDER_NODE="$(dev/scripts/arc-render-node.sh)"   # exactly one 0x8086 node, else exit 1
echo "$ARC_RENDER_NODE"                                       # /dev/dri/renderD129 today
readlink -f /dev/dri/by-path/pci-0000:03:00.0-render          # must agree

docker run --rm --device "$ARC_RENDER_NODE:/dev/dri/renderD129" \
  --security-opt seccomp=unconfined --group-add 988 --group-add 984 \
  vmaf-sycl-arc-runner:local sycl-ls
```

Expected (2026-09-05, NEO 26.31.39395.13):

```text
[level_zero:gpu][level_zero:0] Intel(R) oneAPI Unified Runtime over Level-Zero, Intel(R) Arc(TM) A380 Graphics 12.56.5 [1.17.39395+13]
[opencl:cpu][opencl:0] Intel(R) OpenCL, AMD Ryzen 9 9950X3D 16-Core Processor OpenCL 3.0 (Build 0) [...]
[opencl:gpu][opencl:1] Intel(R) OpenCL Graphics, Intel(R) Arc(TM) A380 Graphics OpenCL 3.0 NEO  [26.31.39395.13]
```

No NVIDIA and no AMD GPU line may appear (the `opencl:cpu` entry is the
host CPU via the Intel OpenCL CPU runtime, not a GPU). Smoke the runner
binary without registering: `docker run --rm vmaf-sycl-arc-runner:local ./run.sh --help`.

### Step 3 — supervise and serve (systemd --user or background script)

The runner runs in `--ephemeral` mode: it handles exactly one job and terminates.
To keep the runner continuously online and automatically re-register between CI jobs,
the host supervisor script [`dev/scripts/runner-supervisor.sh`](../../dev/scripts/runner-supervisor.sh)
manages the lifecycle:

1. Waits for any active job to finish (`docker wait`) and removes the container (`docker compose down`).
2. Mints a fresh registration token via `gh api`.
3. Resolves the active Arc render node via `dev/scripts/arc-render-node.sh`.
4. Starts the ephemeral container via `docker compose up -d`.
5. Applies exponential backoff on token or compose failures.
6. Respects the pause file to prevent container launches when the workstation is paused.

#### Option A: systemd --user service (recommended)

Install the provided user unit [`dev/systemd/vmafx-sycl-arc-runner.service`](../../dev/systemd/vmafx-sycl-arc-runner.service):

```bash
mkdir -p ~/.config/systemd/user
cp dev/systemd/vmafx-sycl-arc-runner.service ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user enable --now vmafx-sycl-arc-runner.service
```

To allow the supervisor loop to continue running when you log out of desktop or SSH sessions,
enable lingering:

```bash
loginctl enable-linger "$USER"
```

Monitor status and logs:

```bash
systemctl --user status vmafx-sycl-arc-runner.service
journalctl --user -u vmafx-sycl-arc-runner.service -f
# or directly inspect the supervisor log:
tail -f "${XDG_STATE_HOME:-$HOME/.local/state}/vmafx-runner/supervisor.log"
```

#### Option B: foreground or tmux script

Alternatively, run the supervisor directly in an active shell session:

```bash
dev/scripts/runner-supervisor.sh
```

#### Token source & headless host configuration

By default, `runner-supervisor.sh` uses `gh` on the host, running under the maintainer's
authenticated CLI user to mint registration tokens (`gh api -X POST repos/VMAFx/vmafx/actions/runners/registration-token`).

For an unattended or headless server where interactive `gh auth login` is unavailable,
set up an environment file with a fine-grained PAT with *Administration: read-write* permissions
scoped to `VMAFx/vmafx`:

```bash
mkdir -p ~/.config/vmafx-runner
echo "GH_TOKEN=github_pat_..." > ~/.config/vmafx-runner/env
chmod 0600 ~/.config/vmafx-runner/env
```

Then un-comment `EnvironmentFile=-%h/.config/vmafx-runner/env` in `~/.config/systemd/user/vmafx-sycl-arc-runner.service`.

#### Enable the CI lane

Once the runner is running and confirmed online, enable the lane variable in GitHub:

```bash
gh variable set SYCL_ARC_RUNNER_ENABLED -b true -R VMAFx/vmafx   # enable the lane LAST
```

### Step 4 — verify registration and post-job re-registration

Check that the runner is registered and online:

```bash
gh api repos/VMAFx/vmafx/actions/runners --jq '.runners[] | {name,status,labels:[.labels[].name]}'
# -> {"name":"cachyos-arc-a380-ephemeral","status":"online","labels":["self-hosted","linux","x64","sycl-arc"]}

docker ps --filter "name=vmaf-sycl-arc-runner"
```

**Verify re-registration after a job**:
When a CI job finishes, the ephemeral container exits and unregisters itself.
The supervisor detects container termination via `docker wait`, removes the container,
mints a fresh token, and brings up a new container within seconds. Check the supervisor log:

```bash
tail -n 20 "${XDG_STATE_HOME:-$HOME/.local/state}/vmafx-runner/supervisor.log"
```

Expected log sequence across a completed job:

```text
[HH:MM:SS] runner container 'vmaf-sycl-arc-runner' alive; waiting for job completion
[HH:MM:SS] job finished; container removed
[HH:MM:SS] runner re-registered (node /dev/dri/renderD129); listening for jobs
```

### Step 5 — pause for daily-driver use

When using the workstation for gaming or other interactive workloads, pause the runner so it
does not contend for the Arc GPU or CPU resources:

```bash
# 1. Disable the lane variable first so incoming PRs skip cleanly without failing the probe:
gh variable set SYCL_ARC_RUNNER_ENABLED -b false -R VMAFx/vmafx

# 2. Touch the pause file (stops the supervisor from spawning any new containers):
mkdir -p "${XDG_STATE_HOME:-$HOME/.local/state}/vmafx-runner"
touch "${XDG_STATE_HOME:-$HOME/.local/state}/vmafx-runner/pause"

# 3. Stop the active runner container:
docker compose -f dev/docker-compose.runner.yml down
```

Order matters: disable `SYCL_ARC_RUNNER_ENABLED` *before* stopping the container,
or concurrent PRs will encounter a loud probe failure.

To resume runner service:

```bash
# 1. Remove the pause file (supervisor will detect this and launch a container within 60 s):
rm -f "${XDG_STATE_HOME:-$HOME/.local/state}/vmafx-runner/pause"

# 2. Verify runner is online (Step 4):
gh api repos/VMAFx/vmafx/actions/runners --jq '.runners[] | select(.status=="online") | .name'

# 3. Re-enable the lane:
gh variable set SYCL_ARC_RUNNER_ENABLED -b true -R VMAFx/vmafx
```

### Step 6 — rotate / remove

```bash
# Registration tokens expire after 1 h and are single-use with --ephemeral; nothing to rotate.
# Rotate the probe PAT: create a new one (Step 0), then
gh secret set SYCL_RUNNER_PROBE_TOKEN -R VMAFx/vmafx
# Remove a stale registration (e.g. container killed mid-job):
gh api repos/VMAFx/vmafx/actions/runners --jq '.runners[] | select(.labels[].name=="sycl-arc") | .id' \
  | xargs -r -I{} gh api -X DELETE repos/VMAFx/vmafx/actions/runners/{}
docker compose -f dev/docker-compose.runner.yml down -v            # also drops the scratch volume
```

---

## 4. Troubleshooting

| Symptom | Cause | Fix |
| --- | --- | --- |
| Probe: `GET repos/.../actions/runners failed (... 403 ...)` | `SYCL_RUNNER_PROBE_TOKEN` missing or lacks Administration: read | Step 0 |
| Probe: `no self-hosted runner with label 'sycl-arc' is registered` while enabled | container not running (supervisor paused, stopped, or waiting on token) | Step 3 supervisor setup, or resume (Step 5) |
| `sycl-ls` shows no `level_zero:gpu` inside the container | wrong node (PCI re-enumeration), missing `seccomp=unconfined`, or a NEO/kernel ABI mismatch | Step 2; [ADR-0541](../adr/0541-dev-container-sycl-hip-runtime-fix.md) |
| `config.sh` / checkout: `Permission denied` under `/actions-runner/_work` | scratch volume created by an older image with a root-owned mount point | `docker compose ... down -v`, rebuild (Step 1) |
| job queued forever | a runner was registered without `x64` or `sycl-arc` | Step 6 remove, re-register |

---

## 5. Files

| Path | Role |
| --- | --- |
| `dev/Containerfile.runner` | image: `vmaf-dev-mcp:local` + pinned runner tarball, non-root user, `_work` ownership |
| `dev/docker-compose.runner.yml` | device passthrough, limits, volume, env; `ARC_RENDER_NODE` override |
| `dev/scripts/runner-supervisor.sh` | host supervisor loop (job wait, fresh token mint, compose up, backoff, pause file) |
| `dev/systemd/vmafx-sycl-arc-runner.service` | `systemd --user` service unit managing `runner-supervisor.sh` |
| `dev/scripts/runner-entrypoint.sh` | `config.sh --ephemeral --unattended` then `run.sh`; any other argv is exec'd (smoke tests) |
| `dev/scripts/arc-render-node.sh` | resolves the single Intel render node from `/sys/class/drm` |
| `scripts/ci/check-runner-available.sh` | hosted probe; tests in `scripts/ci/tests/test-runner-available.sh` |
| `scripts/ci/tests/test-runner-available.sh` | unit suite for `check-runner-available.sh` |
| `scripts/ci/tests/test-runner-supervisor.sh` | unit suite for `runner-supervisor.sh` with stubbed `gh` and `docker` |
| `.github/workflows/sycl-parity.yml` | the two jobs |
| `.github/actionlint.yaml` | declares the `sycl-arc` / `gpu-full` labels for actionlint |

## 6. Invariants

See `scripts/ci/AGENTS.md` § "Self-hosted SYCL Arc runner invariants
(ADR-1177)": fork heads never reach the label, the device list is Arc-only,
the probe never treats an API error as "unregistered", and the aggregator
never accepts a skip while the lane is enabled.
