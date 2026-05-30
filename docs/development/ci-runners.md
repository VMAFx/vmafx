# CI runner pools

The fork's CI runs on a hybrid pool: GitHub-hosted runners by default,
with selected jobs opt-in to a self-hosted [actions-runner-controller
(ARC)](https://github.com/actions/actions-runner-controller)
[`arc-runners`](https://github.com/VMAFx/vmafx/settings/actions/runners)
scale set in the maintainer's personal Kubernetes cluster.

## How a job picks its pool

Jobs that opted into the hybrid pattern use a ternary `runs-on`
expression keyed on the repo-level variable `ARC_RUNNERS_ENABLED`:

```yaml
runs-on: ${{ vars.ARC_RUNNERS_ENABLED == 'true' && 'arc-runners' || 'ubuntu-latest' }}
```

When `ARC_RUNNERS_ENABLED` is `true`, the job is dispatched to an ARC
pod with the `arc-runners` label. When `false` (or unset), the job
stays on `ubuntu-latest`.

## Operator: flipping the variable

1. Open `Settings → Secrets and variables → Actions → Variables` on
   the repository.
2. Edit `ARC_RUNNERS_ENABLED`. Default `false`. Set to `true` to opt
   the migrated jobs into the ARC pool.
3. Push any commit (or use `gh workflow run`) to re-trigger CI on the
   open PRs you want to test against.

## Operator: when ARC is degraded

If the ARC scale set is offline, jobs that selected `arc-runners`
will sit queued indefinitely (no auto-fallback — see
[ADR-0359](../adr/0359-arc-runners-pilot.md)). To recover:

1. Flip `ARC_RUNNERS_ENABLED` back to `false`.
2. Cancel any stuck PR's CI runs:

   ```bash
   gh run list --repo VMAFx/vmafx --branch <pr-branch> --status queued \
       --json databaseId -q '.[].databaseId' | xargs -I{} gh run cancel {} --repo VMAFx/vmafx
   ```

3. Re-trigger CI on each affected PR (push an empty commit, or
   `gh workflow run`).
4. Address the cluster-side issue separately.

## Pilot status (2026-05-09)

| Job | Workflow | Pool selector |
| --- | --- | --- |
| `Cppcheck (Whole Project)` | `lint-and-format.yml` | ternary (pilot) |

All other jobs hard-pinned to `ubuntu-latest` / `macos-latest` /
`windows-latest` as before. After the pilot is green for ≥ 1 day on
at least 5 PRs, ramp up to:

1. Sanitizers (asan / tsan / msan)
2. Vulkan + CUDA + SYCL build legs
3. Windows MSVC + CUDA / oneAPI SYCL legs

## Windows GPU Build Setup

`Build — Windows MSVC + CUDA (build only)` and `Build — Windows MSVC + oneAPI
SYCL (build only)` are required compile-only gates. GitHub-hosted Windows
runners do not expose GPUs, so these jobs verify that the MSVC toolchain,
headers, libraries, and backend compile/link paths stay healthy.

The CUDA leg installs CUDA 13.2.0 directly from NVIDIA's Windows network
installer. It requests only the packages needed by the build:

- `nvcc_13.2`
- `cudart_13.2`
- `crt_13.2`
- `nvvm_13.2`
- `visual_studio_integration_13.2`

The workflow exports `CUDA_PATH`, `CUDA_PATH_V13_2`, and the CUDA `bin`
directory before running `nvcc.exe --version`. If a future CUDA bump changes
Windows package names or install paths, update
[ADR-0664](../adr/0664-windows-cuda-toolkit-installer.md) and the workflow
together.

## What lives in the cluster

Outside the scope of this repository:

- The ARC operator itself (Helm chart from
  [`actions/actions-runner-controller`](https://github.com/actions/actions-runner-controller))
- A `RunnerScaleSet` named `arc-runners` registered against this
  repository
- Container images with the toolchains each migrated job needs (CUDA,
  Vulkan SDK, oneAPI, etc.) — added per-ramp-up PR

The repo-side contract is just the workflow `runs-on` selector.
