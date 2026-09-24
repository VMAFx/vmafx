<!-- markdownlint-disable MD013 -->
# Research: Python Dependency Hash-Lock and OpenSSF Supply-Chain Audit

- **Date**: 2026-09-23
- **Author**: Lusoris
- **Subject**: Pre-RC1 Python Dependency Hash-Locking, PEP 517 Build Isolation, and OpenSSF/CII Hardening
- **ADR Reference**: [ADR-1305](../adr/1305-hash-locked-python-installs.md)

## 1. Executive Summary

This research document details the audit of all Python dependency installation
surfaces across `VMAFx/vmafx`, the enforcement of cryptographic artifact hashes
(`--require-hashes`), and compliance with OpenSSF Scorecard and CII Best Practices
criteria ahead of the 1.0 RC1 release.

Prior to this hardening, Python dependencies were installed using unhashed commands
in CI workflows, Dockerfiles, workstation setup scripts, and Makefile targets.
While top-level packages were pinned or bounded, transitive dependencies were resolved
dynamically from PyPI at installation time.

We established a manifest-driven lock authority (`requirements/locks/manifest.json`)
using `uv pip compile` (pinned to `uv 0.12.18`) to generate tamper-evident,
cryptographically verified SHA-256 artifact hashes.

## 2. Inventory of Python Install Surfaces

A repository-wide audit identified five distinct consumer categories:

1. **GitHub Actions Workflows (`.github/workflows/*.yml`)**:
   - `build.yml`, `docs.yml`, `ffmpeg-integration.yml`, `fuzz.yml`, `go-ci.yml`,
     `libvmaf-build-matrix.yml`, `lint-and-format.yml`, `nightly-bisect.yml`,
     `rust-ci.yml`, `sanitizers.yml`, `security-scans.yml`, `supply-chain.yml`,
     `tests-and-quality-gates.yml`.
   - All migrated from bare `pip install ...` to `pip install --require-hashes -r <lockfile>`.

2. **Container Build Surfaces (`Dockerfile*`, `dev/Containerfile`, `mcp-server/vmaf-mcp/Dockerfile`)**:
   - Build-stage and runtime-stage pip installs migrated to `--require-hashes`.
   - Hadolint validation confirmed cache hygiene (`--no-cache-dir`) and clean exit 0.

3. **Platform Workstation Setup Scripts (`scripts/setup/`)**:
   - `alpine.sh`, `arch.sh`, `fedora.sh`, `macos.sh`, `ubuntu.sh`, `windows.ps1`.
   - Updated to consume `requirements/locks/build.txt` with `--require-hashes`.

4. **Makefile Developer Targets (`Makefile`)**:
   - `$(VENV_PIP)` bootstrap, `$(MESON)`, and `$(NINJA)` targets now consume
     `requirements/locks/build.txt`.
   - `lint-tools` consumes `requirements/locks/dev-linters.txt`.
   - `cythonize-deps` consumes `requirements/locks/cythonize.txt`.
   - Checker regex (`PIP_NAME_RE`) enhanced to scan variable expansions `$(VENV_PIP)`
     and `${VENV_PIP}`.

5. **Nox Developer Orchestration (`noxfile.py`)**:
   - Nox is installed from a dedicated manifest-owned hash lock rather than an
     unconstrained `pip install nox` bootstrap.
   - Literal `session.install(...)` arguments are parsed from the Python AST and
     evaluated under the same hash/local-source policy as shell commands.
   - Each package receives its own development lock, avoiding cross-package
     resolution and preserving package-specific Python bounds.

## 3. Technical Discoveries & Resolutions

### 3.1 PEP 517 Build Isolation Failure with Pure Source Distributions

When installing `docs/requirements-lock.txt` under `--require-hashes`, pip failed
with:

```text
ERROR: In --require-hashes mode, all requirements must have their versions pinned with ==.
These packages were not pinned:
    setuptools, wheel
```

**Root cause**: `mkdocs-minify-plugin` depends on `csscompressor==0.9.5`,
`htmlmin2==0.1.13`, and `jsmin==3.0.1`, which publish only source distributions
(`.tar.gz` sdists) on PyPI. Pip's default PEP 517 build isolation spawns an isolated
ephemeral virtual environment to build wheels from sdists and attempts to download
unhashed `setuptools` and `wheel` from PyPI.

**Resolution**:

1. Added `setuptools>=77.0.1` and `wheel>=0.45.1` directly into `docs/requirements.txt`
   so their hashes are compiled into `docs/requirements-lock.txt`.
2. Updated consumers in `.github/workflows/docs.yml` and `.github/workflows/lint-and-format.yml`
   to invoke:
   `pip install --no-build-isolation --require-hashes -r docs/requirements-lock.txt`.

### 3.2 Python 3.12 Portability for Developer Linters

`requirements/locks/dev-linters.in` defines the formatting, linting, and AST tooling
(`ruff`, `black`, `mypy`, `pre-commit`, `semgrep`). When compiled strictly with
`--python-version 3.14`, universal markers omitted dependencies required by
workstations running Python 3.12 (e.g. `tomli`).

**Resolution**:
Configured `manifest.json` entry for `dev-linters.txt` to pass
`--python-version 3.12`. The generated lockfile resolves universal hashes that function
correctly across Python 3.12, 3.13, and 3.14.

### 3.3 SLSA Generator Tag Exception

OpenSSF Scorecard flags unpinned GitHub Actions (actions not pinned to commit SHAs).
However, `slsa-framework/slsa-github-generator` explicitly enforces:

> "The generators MUST be referenced by tag in order for the slsa-verifier to be
> able to verify the ref of the trusted builder."

Pinning `slsa-github-generator` to a commit SHA causes `slsa-verifier` to fail
cryptographic provenance checks. Therefore, `slsa-github-generator` is intentionally
exempted from SHA pinning and maintained at exact `@vX.Y.Z` release tags, codified in
ADR-1128 and preserved in this hardening.

### 3.4 Lock-path identity and scanner bypasses

Early scanner variants accepted any local `-r` target and then any path whose
basename or suffix resembled a registered lock. Those checks were insufficient:
an injected package could follow a real `-r`, leading pip flags could hide the
`install` subcommand, and an attacker-controlled path such as
`/tmp/untrusted/requirements/locks/build.txt` could impersonate the reviewed file.

The final contract accepts only exact outputs or explicit `install_aliases` from
`requirements/locks/manifest.json`. Each alias is strictly bound to an explicit
consumer path and context, rejecting directory traversal, normalized traversal, Windows
drive/UNC paths, unsupported absolutes, and duplicate JSON keys, while enforcing
consumer liveness. Pip option parsing strictly splits joined short options (`-qrfoo`,
`-rmalicious.txt`, `-cconstraints.txt`), captures global pre-subcommand flags, and rejects
untrusted flags. Nox AST scanner restricts receiver authority to `@nox.session`
parameters, tracks/rejects session and method aliases, and evaluates shell runners fail-closed.
All Nox development locks are workstation-portable (`--universal`), and Git consumer
discovery fails closed with `ContractError`.

### 3.5 Nox interpreter and dependency isolation

The pre-existing Nox sessions installed package extras directly and inherited the
interpreter running Nox. On a Python 3.14 workstation this violates the declared
`<3.13` bounds of ROI-score and ensemble-kit. The hardened sessions use individual
manifest-owned development locks compiled with `--universal` and pin those two sessions
to Python 3.12 (and Python 3.14 for packages resolving on 3.14).

## 4. Verification Evidence

- `scripts/ci/check_python_dependency_locks.py check`: Exit 0 (25 locks validated, all installs verified).
- `python3 -m unittest scripts/ci/tests/test_python_dependency_locks.py`: Exit 0 (68 tests passing).
- Pinned Nox 2026.8.17 installation from `requirements/locks/nox.txt`: Exit 0.
- `nox -l`: Exit 0; ROI-score and ensemble-kit resolve to Python 3.12.
- `nox -s vmaf_tune -- --collect-only -q`: Exit 0; locked session install
  succeeded and collected 2,053 tests.
- `python3 scripts/ci/tests/test_ci_impact.py`: Exit 0 (31 tests passing).
- `bash scripts/ci/test-classify-dependency-pr.sh`: Exit 0 (29 tests passing).
- `actionlint`: Clean exit 0 across all workflows.
- `shellcheck`: Clean exit 0 across all scripts.
- `hadolint`: Clean exit 0 across all Dockerfiles.
