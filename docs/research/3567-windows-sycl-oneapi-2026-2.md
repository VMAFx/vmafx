<!-- markdownlint-disable MD013 -->
# Research-3567: Windows MSVC+SYCL leg on oneAPI 2026.2.0.661

- **Status**: Active
- **Workstream**: [ADR-3567](../adr/3567-windows-sycl-oneapi-2026-2.md), [ADR-0121](../adr/0121-windows-gpu-build-only-legs.md)
- **Last updated**: 2026-11-19

## Question

Can the `Windows MSVC+SYCL` leg of `.github/workflows/libvmaf-build-matrix.yml` build with oneAPI 2026.2.0.661 after the hosted runner image moved to Visual Studio 2026 and MSVC 14.52, whose STL refuses Clang front ends older than 22 (`STL1000`)?

## Sources

- [ADR-3567](../adr/3567-windows-sycl-oneapi-2026-2.md): runner image change, Intel release-note statement, installer file-list reading, earlier Builds runs.
- Builds runs `44475666625`, `44487361326` and `44499808688` of `libvmaf-build-matrix.yml`.
- Download of the pinned installer URL, 2026-11-19.

## Findings

- Builds run `44475666625`, job `135395829372`, head `e9133a6bea`: oneAPI 2026.2.0.661 installed and configuration passed; the AOT link stopped at the first refused target, `tgllp`.
- Builds run `44487361326`: `ocloc ids` accepted 13 default targets and refused six: `tgllp`, `adl-s`, `adl-p`, `adl-n`, `rpl-s` and `rpl-p`.
- Builds run `44499808688`, job `135476883933`, head `f56947e1b5`: the probe accepted 13 targets and refused the declared six; the 2,757-step build, `test_sycl_kernel_registration` and `test_sycl_coff_anchor` passed, with zero `STL1000` log lines.
- The pinned installer measured 2,402,678,488 bytes and SHA-256 `e5774d5435835b66b16c4369a6c9704720fe2df9d67cdc48922c93782c152ee0` on 2026-11-19; the workflow checks both before extraction.

## Alternatives explored

[ADR-3567](../adr/3567-windows-sycl-oneapi-2026-2.md#alternatives-considered) records the two rejected options: MSVC toolset 14.51 (absent from the runner image) and `_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH` (unsupported escape hatch).

## Open questions

- The `spir64` run-time fallback has not been measured on a device from any of the six refused families.
- The Windows SYCL tester zip still uses `ONEAPI_WINDOWS_VERSION` `2025.3.0.372`; the 2026.2 runtime file names are not measured.

## Related

- ADRs: [ADR-3567](../adr/3567-windows-sycl-oneapi-2026-2.md), [ADR-0121](../adr/0121-windows-gpu-build-only-legs.md), [ADR-1566](../adr/1566-windows-sycl-tester-zip.md)
- Contract test: `scripts/ci/test_windows_sycl_toolset_contract.py`
