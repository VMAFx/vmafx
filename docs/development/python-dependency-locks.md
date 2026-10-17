<!-- markdownlint-disable MD013 -->
# Python dependency hash-locking and supply-chain policy

VMAFx enforces cryptographic artifact hashes (`--require-hashes`) for all Python
dependency installations across the repository. This guarantees reproducible,
hermetic builds, protects against upstream PyPI package mutation or dependency
confusion, and satisfies OpenSSF Scorecard supply-chain integrity criteria.

## Overview

All Python dependencies are declared in input requirements files (`.in`,
`pyproject.toml`,
`docs/requirements.txt`) and deterministically locked with SHA-256 artifact
digests
into lock files (`requirements/locks/*.txt`, `*/*-lock.txt`).

The central authority is `requirements/locks/manifest.json`, which pairs every
lock file with its source files and compilation arguments using a reviewed,
pinned
version of `uv` (`0.12.18`).

The manifest validates every path it names:

- Outputs, inputs and alias consumer bindings are local repo-relative paths
  under both POSIX and Windows path semantics.
- Drive/UNC or POSIX absolutes, traversal, remote references and surrounding
  whitespace fail validation.
- Each install command may name the lock's repository output or one of its
  explicit `install_aliases`. Aliases exist only for reviewed paths created by
  container copies or platform scripts.
- Each alias is bound to an explicit repo-relative consumer path and context,
  so unrelated workflows cannot consume alias paths.
- Consumer identity is exact after separator normalization: a nested
  `evil/scripts/setup/ubuntu.sh` path does not inherit the alias authority
  assigned to `scripts/setup/ubuntu.sh`. The checker never trusts a matching
  basename or suffix, and it rejects unreferenced aliases.

### Architecture

```text
Input manifests (.in, pyproject.toml)
         │
         ▼
uv pip compile (pinned uv 0.12.18) ───► Manifest Writer (make python-locks-write)
         │
         ▼
Committed hash-lock files (*.txt, *-lock.txt)
         │
         ├─────────────────────────────────────────┐
         ▼                                         ▼
CI Workflows & Dockerfiles                Local Workstation (make lint-tools, cythonize-deps)
(pip install --require-hashes -r ...)     (pip install --require-hashes -r ...)
```

## Available targets

| Command | Purpose |
| --- | --- |
| `make python-locks-check` | Validates that all lock files match their inputs and that every executed `pip install` in the repo uses `--require-hashes` or local `--no-deps`. Offline, fast. |
| `make python-locks-write` | Networked refresh: runs the pinned `uv` compiler to update all lock files and re-stamp metadata headers. |
| `make lint-tools` | Installs project linting tools (`ruff`, `black`, `mypy`) into `.venv` from `requirements/locks/dev-linters.txt` using `--require-hashes`. |
| `make cythonize-deps` | Installs C-extension build dependencies (`Cython`, `numpy`, `setuptools`, `packaging`) from `requirements/locks/cythonize.txt` using `--require-hashes`. |
| `python3 -m pip install --require-hashes -r requirements/locks/nox.txt` | Installs the reviewed Nox release and its transitive dependencies before running local package sessions. |

## How to add or update a dependency

1. **Update the source specification**:
   - For developer linters: edit `requirements/locks/dev-linters.in`.
   - For documentation: edit `docs/requirements.txt`.
   - For python package: edit `python/pyproject.toml` or
     `python/requirements-*.in`.
   - For MCP server: edit `mcp-server/vmaf-mcp/pyproject.toml`.
   - For AI / training: edit `ai/pyproject.toml`.
   - For a Nox package session: edit that package's `pyproject.toml` and its
     manifest entry; use an overlay `.in` file only for test tools not declared
     by the package.

2. **Regenerate lock files**:

   ```bash
   make python-locks-write
   ```

   This invokes `scripts/ci/check_python_dependency_locks.py write` with the
   reviewed
   `uv` release. It compiles each lock target in a temporary directory, writes
   the SHA-256 fingerprint header, and verifies all locks.

   The fingerprint (`# vmafx-input-sha256:`) covers the compile arguments and
   every input file byte for byte, with one exception: in a `pyproject.toml`
   input, the `version = ...` line of the `[project]` table is left out
   ([ADR-1344](../adr/1344-lock-fingerprint-ignores-project-version.md)).
   release-please rewrites that line on every release PR, and the project's
   own version never changes what the resolver picks. A `version` key in any
   other table, and every dependency change, still makes the lock stale.

3. **Verify locally**:

   ```bash
   make python-locks-check
   ```

4. **Verify package Python floors**:

   Declared `requires-python` floors in tracked `pyproject.toml` files must
   honestly reflect the minimum Python version required by the package's
   direct dependencies and all optional extra groups
   ([ADR-3335](../adr/3335-python-floors-match-dependencies.md)).

   ```bash
   make python-floors-check
   ```

   The verification script `scripts/ci/check_python_floors.py` verifies this
   property by compiling each tracked package's requirements in an isolated
   directory with all extras at its declared floor. The check is kept separate
   from `make lint` because dependency resolution requires network access.

   > [!NOTE]
   > **Resolve-proven vs run-proven gap**: While the check guarantees that the
   > dependency tree resolves without conflict at the declared floor,
   > repository CI executes test suites under Python 3.14. The declared
   > floors are resolve-proven rather than run-proven across older
   > interpreters.

## Sdist packages and PEP 517 build isolation

When pip installs a pure source distribution (`.tar.gz` sdist) under
`--require-hashes`,
its default PEP 517 build isolation attempts to spawn a temporary environment
and
download unhashed build backends (`setuptools`, `wheel`) from PyPI.

To support pure sdists (e.g. `mkdocs-minify-plugin` dependencies) securely:

1. Pinned versions of `setuptools` and `wheel` are declared in
   `docs/requirements.txt`
   and hashed into `docs/requirements-lock.txt`.
2. Installation commands must pass `--no-build-isolation`:

   ```bash
   pip install --no-build-isolation --require-hashes -r docs/requirements-lock.txt
   ```

## Enforcement and CI gates

- **Pre-commit**: `check-python-dependency-locks` runs on commits touching
  Python
  manifests, lock files, workflows, Dockerfiles, setup scripts, or Makefile.
- **Install-surface scanner**: executable pip calls and literal
  `session.install(...)` calls in `noxfile.py` must reference a manifest-owned
  lock exactly. Dynamic Nox arguments and unmanifested requirement paths fail
  closed. Plain and annotated aliases plus literal `getattr(session, "install")`
  aliases stay in the same scan.
- **Workflow checkout-order scanner**: repo-local requirements, packages, helper
  scripts and local actions must follow an unconditional root checkout pinned to
  a full commit SHA. Without PyYAML, the fallback parser accepts only its
  audited
  block-style subset (including simple quoted `jobs`, job-id and `steps` keys)
  and rejects inline/flow-style mappings instead of treating an unparsed
  workflow as empty.
- **CI Impact Planner**: changes to `requirements/` trigger the `python` CI lane
  in `.github/ci-impact.json`.
- **Bot PR exemption**: automated Renovate and Dependabot PRs updating the exact
  dependency allowlist are classified as dependency-only. Generic basename-wide
  `*.in` or `manifest.json` matching is forbidden, so source templates such as
  `core/include/libvmaf/version.h.in` remain fully gated.
- **Renovate configuration**: `renovate.json` proposes updates against the
  source inputs (`.in`, `pyproject.toml`, `docs/requirements.txt`) and ignores
  compiled lock files.
