## CodeQL C/C++ build pinned to C++23 (2026-10-19)

`fix/codeql-cli-notes`, [ADR-3355](adr/3355-codeql-cpp-extractor-cxx23.md). The `codeql-cpp`
job of `.github/workflows/security-scans.yml` passes `-Dcpp_std=c++23` to `meson setup`; a
sync or rebase of that workflow keeps the option, and
`scripts/ci/test_security_workflow_contract.py` fails without it. Move it to `c++26` only when
CodeQL lists C++26 as supported.
