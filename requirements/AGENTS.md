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
   - Manifest validation requires output, input, and alias-consumer bindings to be local repository-relative paths under both POSIX and Windows semantics. It rejects remote paths, surrounding whitespace, POSIX absolutes, Windows drive/UNC paths, directory traversal, duplicate inputs, output flag overrides (`-o`), repeated install targets, and unregistered `*-lock.txt` files.
   - `install_aliases` are exact, reviewed objects bound to explicit repo-relative consumer paths and context (preventing unrelated workflows from consuming alias paths). Consumer identity uses exact separator-normalized equality: `evil/scripts/setup/ubuntu.sh` never inherits authority from `scripts/setup/ubuntu.sh`. Duplicate JSON keys and unreferenced/dead aliases are rejected. Never replace exact equality with basename/suffix heuristics; a lookalike path outside the manifest is untrusted.

2. **Build backend set and installer tooling exclusion**:
   - `requirements/locks/package-build.in` contains all PEP 517 build backend tools and dependencies for locked, unisolated builds (`--no-deps --no-build-isolation`): `build`, `hatchling`, `editables`, `setuptools`, `wheel`, `packaging`, `Cython`, `numpy`, `scipy`.
   - Any package built from sdist or installed editable must have its backend dependencies satisfied here.
   - Build locks (`requirements/locks/build.in`, `requirements/locks/build.txt`) cannot contain installer tooling such as `pip`; build locks pin build tools (`meson`, `ninja`) only to avoid collision or uninstallation failures with runner- or distro-managed pip installations lacking RECORD metadata.

3. **Install policy**:
   - All CI and production pip commands must pass `--require-hashes -r <lockfile>`.
   - Local wheels require `--no-deps`.
   - Editable and local source tree installs require both `--no-deps` and `--no-build-isolation`.
   - Sdist installations under hash enforcement must use `--no-build-isolation` with backends pre-installed.
   - `noxfile.py` sessions use one manifest-owned development lock per package compiled with `--universal` (workstation-portable, without Linux-only `--python-platform`), and each Nox session must explicitly pin its Python interpreter version to agree with the lock target. Nox itself comes from `requirements/locks/nox.txt`; never restore an unconstrained `pip install nox` bootstrap.

4. **Bot & Renovation workflow**:
   - Renovate targets `requirements/locks/*.in` and `docs/requirements.txt` via `pip_requirements` manager.
   - Renovate ignores compiled `*.txt` and `*-lock.txt` files (`ignorePaths`).
   - Dependency PRs updating `.in` inputs under `requirements/`, or package inputs named `requirements*.in`, are classified as dependency PRs by `scripts/ci/classify-dependency-pr.sh`. Generic basename-wide `*.in` and `manifest.json` exemptions are forbidden; a similarly suffixed source template remains fully gated.
