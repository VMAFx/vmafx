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
runs-on: ${{ vars.ARC_RUNNERS_ENABLED == 'true' && (github.event_name != 'pull_request' || github.event.pull_request.head.repo.full_name == github.repository) && 'arc-runners' || 'ubuntu-26.04' }}
```

When `ARC_RUNNERS_ENABLED` is `true`, the job is dispatched to an ARC pod with
the `arc-runners` label. When `false` (or unset), the job stays on its hosted
fallback label, which differs per job (see [Pilot status](#pilot-status)).

A pull request from a fork always runs on the hosted fallback, whatever the
variable says: the middle condition sends only pushes, dispatches, schedules
and pull requests from a branch of this repository to the pool
([ADR-3286](../adr/3286-arc-runners-fork-guard.md)). Approving a fork's
workflow runs does not change where they run. A job moved to the pool copies
the whole expression; `Tooling Tests` refuses a selector that names
`arc-runners` in any other form.

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
| Sanitizers | `tests-and-quality-gates.yml` | `ubuntu-26.04` |

## Hosted Linux image

Every job that runs on a GitHub-hosted Linux runner names its image by version:
`ubuntu-26.04`, or `ubuntu-26.04-arm` for the arm64 legs
([ADR-3251](../adr/3251-hosted-runner-label-explicit.md)). No workflow names
`ubuntu-latest`, one of its larger-runner forms or `ubuntu-slim`. Docker and
release jobs follow the same rule.

The reason is a date. GitHub moves `ubuntu-latest` from Ubuntu 24.04 to 26.04 in
a rollout from 2026-10-19 to 2026-11-19
([actions/runner-images#14748](https://github.com/actions/runner-images/issues/14748),
read 2026-10-11). A job on the floating label changes compiler, Python and glibc
on a day inside that window, without a commit in this repository; a job on a
versioned label changes them in the pull request that edits the label.

Two cases name another versioned image:

| Case | Where it is declared | Ends |
| --- | --- | --- |
| The job's subject is the older image. Today: `verify-native-artifacts` in `supply-chain.yml` runs on `ubuntu-24.04`, the oldest hosted image that loads the native Linux bundle ([ADR-1354](../adr/1354-native-bundle-release-track.md)). | `COMPATIBILITY_TARGETS` in `scripts/ci/check_hosted_runner_labels.py`, with the record that says why | when the record changes |
| The job fails on `ubuntu-26.04` and needs time. | one entry per job in `.config/lint-exceptions.d/hosted-runner-label.toml`: workflow file, job, label, reason, end date | at most 90 days after it is written |

No job carries a dated fallback today. The last two (`ffmpeg-work` and `ffmpeg-
sycl-work`, gcc 15.2 warnings in FFmpeg's sources) ended when the shared fix
series `v0.5.0` took the fix (`T-CI-FFMPEG-WARNINGS-UBUNTU-2604-2026-10-11` in
[state](../state.md)); the declaration file is removed with its last entry, and
a job that needs a new fallback adds the file again.

`check-hosted-runner-labels` refuses every other label; `make lint-actions` runs
it after actionlint. The entry format and the cases the check covers are in
[Hosted runner labels](pre-commit-hooks.md#hosted-runner-labels).

To move to the next image, edit `ALLOWED` in the script, the labels in
`.github/workflows/` and the declaration in `.github/actionlint.yaml` in one
pull request, and let the hosted run of that pull request prove each job. A job
that fails there keeps the old label in the same pull request, with a dated
fallback. The macOS jobs still name `macos-latest` and the Windows jobs name
their versioned labels; neither is covered by this check.

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

## Gitea mirror runner: artifact steps

The workflows also run on a Gitea mirror through Gitea's act runner. From v4
on, `actions/upload-artifact` and `actions/download-artifact` refuse every
server that is not github.com, `*.ghe.com` or `*.localhost`, so such a step
failed on Gitea with `GHESNotSupportedError`. Gitea serves the v3 artifact
protocol, and the act runner sets `GITEA_ACTIONS=true` in every job
([ADR-3368](../adr/3368-gitea-artifact-path.md)).

Every artifact step therefore comes as a pair:

```yaml
- name: Upload log
  if: ${{ (always()) && env.GITEA_ACTIONS != 'true' }}
  uses: actions/upload-artifact@<v7 pin>
  with: {name: log, path: build/log.txt}
- name: Upload log (Gitea)
  if: ${{ (always()) && env.GITEA_ACTIONS == 'true' }}
  uses: actions/upload-artifact@<v3 pin>
  with: {name: log, path: build/log.txt}
```

The Gitea twin keeps the step's condition and its `with:` inputs. A download
that needs an input v3 does not have (`pattern`, `merge-multiple`) cannot run
on Gitea; its twin is a `run:` step that prints a warning naming the missing
inputs, so the skip shows in the job log. On github.com every twin is skipped.

`scripts/ci/check_gitea_artifact_steps.py` refuses an unguarded step, a
missing or mismatched twin, a skip where v3 would do and a v3 pin outside a
twin; it runs in `make lint-actions` and as the `check-gitea-artifact-steps`
pre-commit hook. Add a new artifact step with the v4+ pin only and run:

```bash
python3 scripts/ci/check_gitea_artifact_steps.py --write
```

which adds the guard and the twin in place. Renovate keeps the v3 pins below
v4 (`renovate.json`, ADR-3368).

## What lives in the cluster

Outside the scope of this repository:

- The ARC operator itself (Helm chart from
  [`actions/actions-runner-controller`](https://github.com/actions/actions-runner-controller))
- A `RunnerScaleSet` named `arc-runners` registered against this
  repository
- Container images with the toolchains each migrated job needs (CUDA,
  oneAPI and so on), added per ramp-up PR

The repo-side contract is just the workflow `runs-on` selector.
