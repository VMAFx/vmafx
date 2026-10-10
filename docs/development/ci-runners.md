# CI runner pools

The fork's CI runs on a hybrid pool: GitHub-hosted runners by default,
with selected jobs opt-in to a self-hosted [actions-runner-controller
(ARC)](https://github.com/actions/actions-runner-controller)
[`arc-runners`](https://github.com/VMAFx/vmafx/settings/actions/runners)
scale set in the maintainer's personal Kubernetes cluster.

## How a job picks its pool

Jobs choose their pool with a ternary `runs-on` expression keyed on the
repository variable `ARC_RUNNERS_ENABLED`:

```yaml
runs-on: ${{ vars.ARC_RUNNERS_ENABLED == 'true' && 'arc-runners' || 'ubuntu-latest' }}
```

When `ARC_RUNNERS_ENABLED` is `true`, the job is dispatched to an ARC pod with
the `arc-runners` label. When `false` (or unset), the job stays on its hosted
fallback label, which differs per job (see [Pilot status](#pilot-status)).

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

## Pilot status

Two jobs use the ternary; every other job is pinned to a hosted runner label.
After the pilot is green for at least one day on at least five PRs, the
remaining candidates ramp up in this order: the Windows MSVC CUDA and oneAPI
SYCL legs, then the other build legs.

| Job | Workflow | Fallback when `ARC_RUNNERS_ENABLED` is not `true` |
| --- | --- | --- |
| `Cppcheck (Whole Project)` | `lint-and-format.yml` | `ubuntu-26.04` |
| Sanitizers | `tests-and-quality-gates.yml` | `ubuntu-latest` |

## Windows GPU Build Setup

`Windows MSVC+CUDA` and `Windows MSVC+SYCL` in `libvmaf-build-matrix.yml`
are required compile-only gates. GitHub-hosted Windows
runners do not expose GPUs, so these jobs verify that the MSVC toolchain,
headers, libraries, and backend compile/link paths stay healthy.

The CUDA leg installs the toolkit with `scripts/ci/install-cuda-toolkit.ps1`
from NVIDIA's redistributable archives, at the release named by `CUDA_VERSION`
in `build-config.env`, the same coordinated pin as every other site
([ADR-1285](../adr/1285-cuda-coordinated-pin-lockstep.md)). The script resolves
the component versions from NVIDIA's manifest at run time and exports
`CUDA_PATH`, the versioned `CUDA_PATH_V<major>_<minor>` variable and the CUDA
`bin` directory before the workflow runs `nvcc.exe --version`. If a future CUDA
bump changes Windows package names or install paths, update
[ADR-0664](../adr/0664-windows-cuda-toolkit-installer.md) (the original
network-installer design) and the script together.

## Windows ARM64 lane

`Windows ARM64 MSVC` ([ADR-1260](../adr/1260-windows-arm64-cpu-lane.md))
runs on the GitHub-hosted `windows-11-vs2026-arm` runner: Windows 11 Arm64
with Visual Studio 2026, free for public repositories. It is the only lane
that compiles `core/src/feature/arm64/` and the `#if ARCH_AARCH64` branches
with `cl.exe`, and it executes the meson `fast` suite on ARM64 silicon. The
lane is advisory: it reports on every non-draft PR and master push but is not
in the required-aggregator list.

What the job pins, and why:

| Pin | Value | Reason |
| --- | --- | --- |
| Runner label | `windows-11-vs2026-arm` | The `windows-11-arm` label is being migrated to the same Visual Studio 2026 image between 2026-09-21 and 2026-09-30 (`actions/runner-images` #14602); the explicit label keeps one image. After the migration the two labels are interchangeable. |
| MSVC environment | `TheMrMilchmann/setup-msvc-dev` with `arch: arm64` | Runs `vcvarsall.bat arm64`, the ARM64-hosted native toolset; `amd64_arm64` would select the x64-hosted cross compiler under emulation. The job runs `cl.exe` and greps its banner for `for ARM64` before configuring. |
| Python | `actions/setup-python` 3.14.7 and `pip install meson ninja` | Published for `win32`/`arm64`; `ninja` has a `win_arm64` wheel. No nasm: the build only probes it on x86. |
| Configure | As the x64 legs: `/experimental:c11atomics`, `--default-library=static`, `-Denable_float=true`, no CUDA or SYCL | One MSVC configuration across lanes. |
| Output check | PowerShell check that `install\bin\vmaf.exe` has PE machine `0xAA64` | An accidental x64 build cannot pass under emulation. |

Microsoft Defender's real-time protection stays on in this image (the image
readme: Tamper Protection keeps the image build from turning it off), unlike
the x64 Windows images. Deleting a program that a test has just built and run
can fail for a moment with `PermissionError` (WinError 5). A test that runs a
program from a temporary directory removes it with
`scripts/lib/scratch_program.py` (`remove_program()`), which retries only that
file, a bounded number of times, before the directory is cleaned up.

CUDA is off in this lane. CUDA 13.3.1 shipped no `windows-arm64` packages and
13.4.1 was the first release that does; the repository pin is now the CUDA
release in `build-config.env`, and a CUDA-on-WoA leg is not part of this lane
today.

## What lives in the cluster

Outside the scope of this repository:

- The ARC operator itself (Helm chart from
  [`actions/actions-runner-controller`](https://github.com/actions/actions-runner-controller))
- A `RunnerScaleSet` named `arc-runners` registered against this
  repository
- Container images with the toolchains each migrated job needs (CUDA,
  oneAPI and so on), added per ramp-up PR

The repo-side contract is just the workflow `runs-on` selector.
