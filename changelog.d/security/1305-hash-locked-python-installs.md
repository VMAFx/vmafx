- **Cryptographic hash locks for all Python installs (ADR-1305)** —
  enforced `--require-hashes` across all GitHub Actions workflows,
  Dockerfiles, workstation setup scripts, and Makefile targets using
  manifest-driven lock authority (`requirements/locks/manifest.json`)
  compiled with reviewed `uv 0.12.18`. Pure sdist documentation
  dependencies pin `setuptools` and `wheel` under `--no-build-isolation`,
  `dev-linters.txt` targets Python 3.12 portability, Makefile variable
  expansions are brought under contract checking, and SLSA builder tag
  requirements are preserved.
