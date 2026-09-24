- **Cryptographic hash locks for all Python installs (ADR-1305)** —
  enforced `--require-hashes` across all GitHub Actions workflows,
  Dockerfiles, workstation setup scripts, and Makefile targets using
  manifest-driven lock authority (`requirements/locks/manifest.json`)
  compiled with reviewed `uv 0.12.18`. Pure sdist documentation
  dependencies pin `setuptools` and `wheel` under `--no-build-isolation`,
  `dev-linters.txt` targets Python 3.12 portability, Makefile variable
  expansions and Nox AST installs are brought under contract checking,
  requirement paths must exactly match manifest-owned outputs or aliases,
  package-specific Nox development locks replace editable extra resolution,
  SLSA builder tag requirements are preserved, installer tooling (`pip`)
  is excluded from bootstrap build locks, Dependency Review truthfully
  permits dual-licensed `pkg:pypi/text-unidecode` package-wide, and
  root `Dockerfile` isolates Python installs into a dedicated virtual environment.
  Follow-up fail-closed fixtures narrow dependency-only classification to the
  explicit allowlist, make the no-PyYAML parser honor quoted block keys, keep
  annotated and `getattr` Nox aliases under inspection, and enforce manifest
  output/input/consumer paths as repo-relative on POSIX and Windows.
