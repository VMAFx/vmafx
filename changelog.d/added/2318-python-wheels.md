- **Build and publish pip-installable binary wheels for the `vmafx` Python package (#2318).**
  CI builds platform wheels bundling `libvmafx` for Linux (x86_64 and aarch64 manylinux),
  macOS (arm64), and Windows (x64), repaired via `auditwheel`, `delocate`, and `delvewheel`.
  The release pipeline publishes verified wheels and sdists to PyPI via Trusted Publishing
  on final `v*` tags, with automated dry runs exercising wheel builds and fixture scoring in PRs.
