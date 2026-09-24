# ADR-1305: Hash-locked Python dependency installs and OpenSSF supply-chain hardening

- **Status**: Accepted
- **Date**: 2026-09-23
- **Deciders**: Lusoris
- **Tags**: `security`, `dependencies`, `supply-chain`, `python`, `ci`

## Context

Python dependencies across the repository were previously installed via ad-hoc
`pip install` commands across GitHub Actions workflows, multi-stage Dockerfiles,
workstation setup scripts (`scripts/setup/`), and Makefile targets. While top-level
manifests specified version bounds, dependencies were not cryptographically
pinned with artifact hashes (`--require-hashes`).

This left several critical supply-chain vulnerabilities and portability gaps:

1. **Supply-chain exposure**: Unhashed installs allowed PyPI to serve mutated
   wheels or compromised dependencies at run time, failing OpenSSF Scorecard
   `Pinned-Dependencies` requirements and HISS-11 hermetic supply chain invariants.
2. **PEP 517 build-isolation failures with hashed sdists**: Documentation build
   tools (e.g. `mkdocs-minify-plugin`) depend on pure source distributions
   (`csscompressor`, `htmlmin2`, `jsmin`). Under `--require-hashes`, pip's default
   build isolation creates an isolated temporary environment and attempts to fetch
   unhashed build backends (`setuptools`, `wheel`) from PyPI, aborting the build.
3. **Python 3.12 portability**: Running `dev-linters.in` compilation locked against
   Python 3.14 omitted environment markers and packages required on supported
   workstations running Python 3.12 (such as `tomli`).
4. **Makefile drift**: Local developer targets (`$(VENV_PIP)`, `$(MESON)`, `$(NINJA)`,
   `lint-tools`, `cythonize-deps`) executed unhashed pip installs, allowing local
   development environments to diverge from CI.
5. **SLSA GitHub generator constraints**: OpenSSF Scorecard recommends commit SHA
   pinning for GitHub Actions; however, `slsa-framework/slsa-github-generator`
   explicitly requires an exact `@vX.Y.Z` tag for its trusted builder verification
   (slsa-verifier#12; ADR-1128). Any mechanical conversion to SHAs breaks provenance
   verification.

## Decision

We enforce hermetic, cryptographically verified Python installations across the
entire repository using manifest-driven hash locks.

1. **Manifest-driven lock authority (`requirements/locks/manifest.json`)**:
   - All lock files are generated deterministically using a reviewed, pinned `uv`
     binary (`uv_version: 0.12.18`).
   - Every lock file includes header metadata containing the generator version,
     the inputs list, and a SHA-256 digest of input file contents and compiler
     arguments.
   - Manifest output, input, and alias-consumer paths are local repository-relative
     paths under both POSIX and Windows semantics; absolute, traversal, remote,
     and whitespace-padded values are invalid.
   - Offline verification is performed via:
     `scripts/ci/check_python_dependency_locks.py check`.
   - Explicit network refresh is executed via:
     `scripts/ci/check_python_dependency_locks.py write`
     (exposed as `make python-locks-write`).

2. **Strict install command policy across all surfaces**:
   - All executable pip invocations in `.github/workflows/`, `Dockerfile*`,
     `scripts/setup/*.sh`, and `Makefile` must specify
     `--require-hashes -r <lockfile>`.
   - Local wheels must explicitly pass `--no-deps`.
   - Local editable and source tree installs must explicitly pass both
     `--no-deps` and `--no-build-isolation`.
   - The checker scans all shell scripts, workflows, Dockerfiles, Makefiles, and
     literal `session.install(...)` calls in `noxfile.py` for non-compliant
     invocations. Plain, annotated, and literal `getattr` aliases do not escape
     the Nox scan. Dynamic Nox install arguments fail closed.
   - A requirement target is accepted only when it exactly matches a manifest
     output or an explicit `install_aliases` entry. Basename and suffix matching
     are forbidden, so `/tmp/untrusted/requirements/locks/build.txt` cannot
     impersonate the reviewed lock.

3. **PEP 517 build-isolation dependencies and `--no-build-isolation`**:
   - `docs/requirements.txt` explicitly includes `setuptools>=77.0.1` and
     `wheel>=0.45.1` so they are hashed into `docs/requirements-lock.txt`.
   - `requirements/locks/package-build.in` / `.txt` provides the complete
     hash-locked backend set (`build`, `hatchling`, `editables`, `setuptools`,
     `wheel`, `packaging`, `Cython`, `numpy`, `scipy`) required for offline,
     isolated-build-free editable installs and sdist builds (such as
     `libsvm-official`).
   - Hashed sdist consumers (`docs.yml` and `lint-and-format.yml`) invoke:
     `pip install --no-build-isolation --require-hashes -r docs/requirements-lock.txt`.

4. **Python 3.12 portability for developer linters**:
   - `requirements/locks/manifest.json` configures `dev-linters.txt` to compile
     with `--universal --python-version 3.12 --generate-hashes`, resolving
     markers and fallbacks for both Python 3.12 and 3.14.

5. **Hash-locked Nox orchestration**:
   - Nox itself is pinned in `requirements/locks/nox.in` and installed from the
     generated `requirements/locks/nox.txt` with `--require-hashes`.
   - Every fork-local package session installs a manifest-owned development lock,
     then installs only its local source with `--no-deps --no-build-isolation`.
     Each development lock includes the package's PEP 517 backend inputs.
   - The ROI-score and ensemble-kit sessions request Python 3.12 because their
     package metadata excludes Python 3.14. Nox may obtain the supported
     standalone interpreter when it is absent locally.

6. **Executable Makefile pip installs under checker**:
   - Makefile venv bootstrap targets (`$(VENV_PIP)`, `$(MESON)`, `$(NINJA)`)
     install from `requirements/locks/build.txt`.
   - `lint-tools` installs from `requirements/locks/dev-linters.txt`.
   - `cythonize-deps` installs from `requirements/locks/cythonize.txt`.
   - Phony Make targets `python-locks-check` and `python-locks-write` provide
     local entrypoints.
   - `python-locks-check` is integrated into `make lint`.

7. **Tooling and CI impact integration**:
   - Pre-commit registers `check-python-dependency-locks` and
     `test-python-dependency-locks`.
   - CI impact planner (`.github/ci-impact.json`) adds `"requirements/"` prefix
     and routes requirements changes to the `python` testing lane.
   - Dependency PR classifier (`scripts/ci/classify-dependency-pr.sh`) allows the
     explicit dependency surface, including `requirements/*` and
     `requirements*.in`, for automated bot PRs. Generic basename-wide `*.in`
     and `manifest.json` exemptions are forbidden outside their owned subtree.
   - Renovate (`renovate.json`) monitors `requirements/locks/*.in`,
     `docs/requirements.txt`, and ignores compiled `*.txt` and `*-lock.txt`.

8. **Preservation of SLSA tag requirement**:
   - Workflows calling `slsa-framework/slsa-github-generator` retain exact
     semantic tags (`@v...`), which are exempted from SHA-only rules to
     preserve SLSA cryptographic attestation.

9. **Installer tooling exclusion from bootstrap build locks**:
   - `requirements/locks/build.in` pins build dependencies (`meson`, `ninja`) only.
   - Installer tooling (`pip`) is excluded from build locks to prevent pip from
     attempting to uninstall runner- or Debian-managed pip packages that lack
     RECORD metadata.

10. **Truthful package-wide license review for `text-unidecode`**:
    - `actions/dependency-review-action` evaluates SPDX license expressions under
      `deny-licenses: GPL-3.0, AGPL-3.0`.
    - `python-slugify` brings in `text-unidecode`, dual-licensed under
      `Artistic-1.0-Perl OR GPL-1.0-only OR GPL-2.0-or-later`, consumed under
      `Artistic-1.0-Perl`.
    - GitHub Dependency Review matches PURLs package-wide (ignoring versions);
      `allow-dependencies-licenses: pkg:pypi/text-unidecode` explicitly and
      truthfully allows the package using exact package-wide purl syntax.

11. **Container Python virtual environment isolation**:
    - Root `Dockerfile` installs locked Python packages into an isolated virtual
      environment (`/opt/vmaf-venv`), avoiding conflicts with Debian system
      packages (such as `python3-packaging`) without `--break-system-packages`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| `pip-compile` (pip-tools) | Standard tooling | Slow resolution; platform-specific wheel hashes unless run on every OS | `uv pip compile` generates multi-platform universal hashes deterministically and orders of magnitude faster. |
| Poetry / Pipenv locks | Rich metadata | Non-standard format; requires third-party tool at install time rather than native `pip --require-hashes` | Incompatible with minimal Docker containers and air-gapped CI environments. |
| Basename or suffix recognition for copied locks | No manifest aliases to maintain | Any attacker-controlled path ending in a known lock name is misclassified as trusted | Exact manifest-owned aliases bound to explicit consumer paths and context preserve copied-container paths without weakening lock identity. |
| One repository-wide Nox development lock | Fewer generated files | Co-resolves unrelated heavy packages and ignores incompatible `requires-python` ranges | Per-session locks preserve package isolation and use the interpreter version each package supports. |
| Blanket SHA pinning for all Actions | 100% Scorecard check | Breaks SLSA builder attestation (`slsa-verifier` rejects SHA refs) | SLSA builder contract requires tag refs; overriding tag breaks release provenance. |

## Consequences

- **Positive**:
  - Full cryptographic tamper-evidence on every Python package installed in CI,
    containers, and developer environments.
  - Nox bootstrap and per-package sessions obey the same manifest authority as
    CI and container installs.
  - Offline lint and pre-commit checks prevent unhashed or unpinned dependencies
    from entering the repository.
  - Reproducible builds across Python 3.12, 3.13, and 3.14.
  - Scorecard supply-chain hardening without breaking SLSA attestation.
- **Negative**:
  - Updating a Python dependency requires running `make python-locks-write` to
    update lock files and input digests.
- **Neutral / follow-ups**:
  - Pinned `uv` generator version in `manifest.json` should be bumped
    periodically in lockstep with toolchain updates.

## References

- req: "finish the pre-RC1 Python dependency hash-lock and OpenSSF/CII
  hardening in this isolated worktree" — direct operator mandate to enforce
  hermetic, cryptographically verified Python dependency installs
  repository-wide.
- OpenSSF Best Practices Badge
  [Project 14549: VMAFx](https://www.bestpractices.dev/en/projects/14549).
- [ADR-1126](1126-retire-isort-for-ruff.md) — Single import-sorting canon under
  Ruff (no standalone isort).
- ADR-1128 — SLSA Provenance and Builder Verification.
- [ADR-1152](1152-dependency-pr-gate-exemption.md) — Dependency PR classification.
- OpenSSF Scorecard
  [Pinned-Dependencies Documentation](https://github.com/ossf/scorecard/blob/main/docs/checks.md#pinned-dependencies).
- slsa-github-generator
  [Issue #12: Workflow reference by tag](https://github.com/slsa-framework/slsa-github-generator/issues/12).
- [PEP 517](https://peps.python.org/pep-0517/) — A build-system independent
  format for source trees.
- [PEP 660](https://peps.python.org/pep-0660/) — Editable installs for
  pyproject.toml.
