<!-- markdownlint-disable MD013 -->
# AGENTS.md — requirements/

Parent: [../AGENTS.md](../AGENTS.md).
See also: [ADR-1305](../docs/adr/1305-hash-locked-python-installs.md) and [docs/development/python-dependency-locks.md](../docs/development/python-dependency-locks.md).

## Purpose

The `requirements/` directory contains declarative dependency specifications (`*.in`) and
authoritative, cryptographically hash-locked dependency pins (`requirements/locks/`).

## Architecture & Invariants

1. **Manifest authority (`requirements/locks/manifest.json`)**:
   - `manifest.json` is the sole declarative source of truth for all lock files in the repository.
   - Pinned generator `uv_version` is exact (`0.12.18`). Bumping uv requires reviewed manifest update.
   - Every lock file is generated strictly through `make python-locks-write` (`check_python_dependency_locks.py write`). Never edit `*-lock.txt` or `requirements/locks/*.txt` manually.
   - Each lock file contains `# vmafx-uv-version:`, `# vmafx-input-sha256:`, and `# vmafx-inputs:` header metadata.
   - Manifest validation rejects remote outputs, directory traversals, duplicate inputs, output flag overrides (`-o`), and unregistered `*-lock.txt` files.

2. **Build backend set (`requirements/locks/package-build.in`)**:
   - Contains all PEP 517 build backend tools and dependencies for locked, unisolated builds (`--no-deps --no-build-isolation`): `build`, `hatchling`, `editables`, `setuptools`, `wheel`, `packaging`, `Cython`, `numpy`, `scipy`.
   - Any package built from sdist or installed editable must have its backend dependencies satisfied here.

3. **Install policy**:
   - All CI and production pip commands must pass `--require-hashes -r <lockfile>`.
   - Local wheels require `--no-deps`.
   - Editable and local source tree installs require both `--no-deps` and `--no-build-isolation`.
   - Sdist installations under hash enforcement must use `--no-build-isolation` with backends pre-installed.

4. **Bot & Renovation workflow**:
   - Renovate targets `requirements/locks/*.in` and `docs/requirements.txt` via `pip_requirements` manager.
   - Renovate ignores compiled `*.txt` and `*-lock.txt` files (`ignorePaths`).
   - Dependency PRs updating `.in` or `manifest.json` are classified as dependency PRs by `scripts/ci/classify-dependency-pr.sh`.
