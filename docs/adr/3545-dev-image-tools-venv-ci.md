# ADR-3545: Tools virtual environment and CI gate for Ubuntu dev images

- **Status**: Accepted
- **Date**: 2026-11-14
- **Tags**: docker, dev, ci

## Context

The three Ubuntu compatibility images under `docker/dev/`
(`ubuntu-26.04.Dockerfile`, `ubuntu-26.04-cuda.Dockerfile`, and
`ubuntu-26.04-sycl.Dockerfile`) copy `build-config.env` and
`scripts/setup/ubuntu.sh` into `/tmp/` and invoke `bash /tmp/ubuntu.sh`
(issue 3455). Three defects kept those images broken and untested:

1. **Unstaged lock files and wrong root derivation.** `ubuntu.sh` derived
   `REPO_ROOT` from `${BASH_SOURCE[0]}/../..`, which resolved to `/` from
   `/tmp/ubuntu.sh`, and read `$REPO_ROOT/requirements/locks/build.txt` and
   `dev-linters.txt`, neither of which the Dockerfiles copied.
2. **PEP 668 externally managed Python.** On Ubuntu 26.04 (Python 3.14),
   `python3 -m pip install --user` inside the container failed immediately with
   `error: externally-managed-environment` before `pip` even opened the lock
   file. On hosted GitHub runners `pip install --user` still succeeded in
   `.github/workflows/nightly.yml`, hiding the container failure.
3. **Silent boolean-flag mismatch and missing CI build lane.** `ubuntu.sh`
   compared `INSTALL_LINTERS`, `ENABLE_CUDA`, and `ENABLE_SYCL` against
   `"true"`, while all three Dockerfiles and `nightly.yml` passed `1`. Linters
   silently never installed, and the CUDA/SYCL Dockerfiles accidentally relied
   on `ENABLE_CUDA=1` and `ENABLE_SYCL=1` being ignored because CUDA comes from
   `scripts/ci/install-cuda-toolkit.sh` and oneAPI comes from the SYCL base
   image. No CI workflow built `docker/dev/ubuntu-26.04*.Dockerfile`.

## Decision

1. **Single tools virtual environment in `scripts/setup/ubuntu.sh`.**
   `ubuntu.sh` honours `REPO_ROOT="${VMAFX_REPO_ROOT:-<checkout derivation>}"`,
   validates `ENABLE_CUDA`, `ENABLE_SYCL`, and `INSTALL_LINTERS` against the
   exact allow-list `true|false` (exiting `2` on any other value before running
   `apt-get` or `sudo`), and verifies that every required lock file exists and
   is readable before invoking `pip` (exiting `1` with guidance to run from a
   checkout or set `VMAFX_REPO_ROOT`). Python build and lint tools always
   install into one virtual environment at `VMAFX_TOOLS_VENV` (default
   `${XDG_DATA_HOME:-$HOME/.local/share}/vmafx/tools-venv`) with
   `--require-hashes` (refusing an existing non-empty directory that lacks
   `pyvenv.cfg`). When `GITHUB_PATH` is set, `ubuntu.sh` appends
   `$VMAFX_TOOLS_VENV/bin` so subsequent workflow steps retain `meson` and
   `ninja` on `PATH`. `.github/workflows/nightly.yml` sets
   `INSTALL_LINTERS: "false"` explicitly on the TSan setup step to preserve its
   existing linter-free behavior.
2. **Dockerfile lock staging and runtime tool path.** Each
   `docker/dev/ubuntu-26.04*.Dockerfile` declares `ARG VMAFX_REPO_ROOT=/tmp`
   after `FROM` (visible to `RUN` without persisting into the runtime image),
   copies `requirements/locks/build.txt` and
   `requirements/locks/dev-linters.txt` into `/tmp/requirements/locks/`, cleans
   `/tmp/requirements` at the end of `RUN`, sets `INSTALL_LINTERS=true` and
   `VMAFX_TOOLS_VENV=/opt/vmafx-tools`, prepends `/opt/vmafx-tools/bin` to
   `PATH` (retaining `/usr/local/cuda/bin` on the CUDA image), and removes the
   inert `ENABLE_CUDA=1` / `ENABLE_SYCL=1` variables in favour of a comment
   naming each toolkit's source. The existing `COPY build-config.env
   scripts/setup/ubuntu.sh /tmp/` and `&& bash /tmp/ubuntu.sh \` lines remain
   byte-identical for `UbuntuAptSnapshotContract` (ADR-3541).
3. **Own-input CI lane in `.github/workflows/dev-container-build.yml`.** Reuse
   the existing dev-container workflow (HISS-19) by adding selector
   `ubuntu_dev_images` (`own_paths_only: true`) in `.github/ci-impact.json`,
   validated and exported by the `impact` job, and a 3-way `ubuntu-dev-images`
   matrix job (`cpu`, `cuda`, `sycl`, `timeout-minutes: 60` pending ten
   measured runs) that builds each image on non-draft light and full tiers when
   inputs change and smoke-tests `meson`, `ninja`, `ruff`, `shellcheck`, and
   `shfmt` (plus `bash -lc 'command -v meson'` on `sycl`). The required
   `dev-container-build` aggregator job (`Dev Container Build`) gates
   `ubuntu-dev-images` on `true:success` or `false:skipped`, and
   `.github/ci-tier.json` lists `ubuntu_dev_images` alongside
   `dev_container_inputs` on that own-input lane.

## Alternatives considered

| Axis | Chosen | Alternatives rejected | Why rejected |
| --- | --- | --- | --- |
| Python tool isolation | Dedicated `python3 -m venv "$VMAFX_TOOLS_VENV"` with `--require-hashes` | `pip install --break-system-packages`; `pipx` | `--break-system-packages` mutates the distro-managed Python environment PEP 668 protects and mixes apt and pip files under `/usr/local`; `pipx` adds an extra bootstrap dependency and does not natively install multi-package hash-locked requirement files into one shared `bin` directory. |
| CI workflow placement | Extend `.github/workflows/dev-container-build.yml` with `ubuntu_dev_images` (`own_paths_only: true`) | Separate `.github/workflows/ubuntu-dev-images.yml` workflow | A separate workflow duplicates tier/impact checkout plumbing (violating HISS-19) and spends extra check-suite / planner runs on unrelated pull requests. |
| Lock path layout in Dockerfiles | `ARG VMAFX_REPO_ROOT=/tmp` + `COPY ... /tmp/requirements/locks/` | Mirror `scripts/setup/ubuntu.sh` at `/tmp/repo/scripts/setup/ubuntu.sh` | Moving `ubuntu.sh` out of `/tmp/` breaks the shared `COPY build-config.env scripts/setup/ubuntu.sh /tmp/` and `&& bash /tmp/ubuntu.sh \` contract shapes checked by `UbuntuAptSnapshotContract` (ADR-3541). |

## Consequences

- `scripts/setup/ubuntu.sh` has one deterministic Python installation path on
  workstations, GitHub hosted runners, and Ubuntu 24.04/26.04 containers, with
  no `--user` fallback or PEP 668 conflict.
- Callers running `ubuntu.sh` interactively add `$VMAFX_TOOLS_VENV/bin` to
  `PATH` (printed at completion and documented in the Ubuntu install guide);
  GitHub Actions steps receive it automatically through `GITHUB_PATH`, and
  container consumers receive `/opt/vmafx-tools/bin` through `ENV PATH`.
- Invalid boolean flag values (`1`, `0`, `yes`) fail fast with exit code `2`
  instead of silently disabling or ignoring options.
- `UbuntuDevImageLockContract` in `scripts/ci/test_fail_closed_ci.py` enforces
  `ARG VMAFX_REPO_ROOT`, lock staging completeness against `ubuntu.sh`,
  `/opt/vmafx-tools/bin` on `PATH`, and fail-fast flag/lock checks offline; the
  `ubuntu-dev-images` CI job verifies full image builds and tool execution when
  inputs change.

## References

- [ADR-3338: Own-input lane for the dev container][adr-3338].
- [ADR-3541: Pin Ubuntu development packages to one archive snapshot][adr-3541].
- [PEP 668: Externally managed Python environments][pep-668].

[adr-3338]: 3338-dev-container-own-input-lane.md
[adr-3541]: 3541-dev-image-apt-snapshot.md
[pep-668]: https://peps.python.org/pep-0668/
