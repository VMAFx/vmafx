<!-- markdownlint-disable MD013 MD060 -->
# ADR-3567: Raise the Windows MSVC+SYCL CI oneAPI pin to 2026.2.0.661 for MSVC 14.52 STL1000 compatibility

- **Status**: Accepted
- **Date**: 2026-11-19
- **Deciders**: lusoris
- **Tags**: `ci`, `windows`, `sycl`, `build`

## Context

On runner image `win25-vs2026` `20261116.315`, Visual Studio Enterprise 2026
updated from `18.11.12328.437` (MSVC toolset `14.51.36231` on image
`20261103.298`) to `18.12.12405.243` (MSVC toolset `14.52.37025`). In MSVC
Build Tools `14.52`, `yvals_core.h` raised the minimum supported Clang version
from Clang 20 to Clang 22 (`#if __clang_major__ < 22` ->
`_EMIT_STL_ERROR(STL1000, "Unexpected compiler version, expected Clang 22 or newer.")`).

The `Windows MSVC+SYCL` leg of
[`.github/workflows/libvmaf-build-matrix.yml`](../../.github/workflows/libvmaf-build-matrix.yml)
remained pinned to Intel oneAPI Base Toolkit `2025.3.0.372`
([ADR-0121](0121-windows-gpu-build-only-legs.md)), whose `icx-cl` uses a Clang
21 front end (`_installdir/compiler/2025.3/lib/clang/21/`). Meson's C++23
`<expected>` probe in `Configure libvmaf (SYCL)` failed with `error STL1000`
and aborted configuration.

Intel's 2026 release notes identify Clang 22 as the C and C++ front end from
compiler release 2026.0.0, providing the source for the Clang 22-or-later
baseline; the installer resource path discussed below is a separate observation.

## Decision

1. **Raise `WINDOWS_BASEKIT_URL` in `libvmaf-build-matrix.yml` to oneAPI
   `2026.2.0.661`.** The `Install Intel oneAPI (windows)` step now downloads
   the unified Intel oneAPI Toolkit offline installer
   `https://registrationcenter-download.intel.com/akdlm/IRC_NAS/728c4418-1b40-47a9-ad51-31530fb27c94/intel-oneapi-toolkit-2026.2.0.661_offline.exe`
   (`2,402,678,488` bytes, `Last-Modified: 2026-10-16`). On 2026-11-19,
   its central directory contained package
   `packages/intel.oneapi.win.cpp-dpcpp-common,v=2026.2.0+864/`, whose
   `cupPayload.cup/filelist.json` lists
   `_installdir/compiler/2026.2/lib/clang/23/include/adcintrin.h` (line 2000).
   This path suggests Clang 23 resources; it does not establish the version
   reported by `icx-cl` or prove that MSVC accepts the compiler.
2. **Skip Visual Studio 2026 IDE integration during bootstrap.** The installer
   central directory read on 2026-11-19 contains
   `packages/intel.oneapi.win.dpcpp-compiler.ide_vs180.vs2026,v=2026.2.0+41/`.
   The runner carries Visual Studio 2026, so the bootstrapper command adds
   `-p=NEED_VS2026_INTEGRATION=0` next to `-p=NEED_VS20{17,19,22}_INTEGRATION=0`.
3. **Guard the pin with a workflow contract test.**
   [`scripts/ci/test_windows_sycl_toolset_contract.py`](../../scripts/ci/test_windows_sycl_toolset_contract.py)
   parses `.github/workflows/libvmaf-build-matrix.yml` and verifies that the
   SYCL leg pins oneAPI `2026.2.0.661`, keeps step `if:` conditions intact,
   rejects `_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH`, and fails if the pin is
   moved to the CUDA leg only.

The first `Windows MSVC+SYCL` run on the pull request head tests both the
component ID and compiler front end: the install step fails for an unknown
component, and the build fails with `STL1000` for a front end below Clang 22.

## How the installer facts were read

On 2026-11-19, HTTP Range requests to the installer URL above read bytes
`2401629912-2402678487` and `272536971-276731274` (inclusive). The first
range contains the ZIP central directory at bytes `2402513991-2402666449`;
locate `packages/intel.oneapi.win.cpp-dpcpp-common,v=2026.2.0+864/cupPayload.cup`
there. The second starts at that entry's ZIP local header. Inflate its outer
DEFLATE stream, then read the first nested ZIP member, `filelist.json`, using
ZIP LZMA decompression. Line 2000 names the resource path above. These ranges
read 5 MiB in total; neither a compiler executable nor a runtime package was
executed or inspected.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| **Raise the Windows SYCL CI pin to oneAPI `2026.2.0.661`** (chosen) | The installer lists Clang 23 resources; the CI build will test compiler compatibility | `windows-tester-bundle.yml` remains on `ONEAPI_WINDOWS_VERSION` (`2025.3.0.372`) until its redistributable runtime spec is refreshed | — |
| Select MSVC toolset `14.51` via `TheMrMilchmann/setup-msvc-dev` `toolset: 14.51` | Would keep oneAPI `2025.3.0.372` unchanged | Image `win25-vs2026` `20261116.315` does not carry `14.51` side by side (`Windows2025-VS2026-Readme.md` lists only `14.52.37025`, `14.44`, and `14.29`), so `-vcvars_ver=14.51` fails in `vcvarsall.bat` | Toolset `14.51` is absent from the runner image |
| Define `_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH` | No installer change | Disables `yvals_core.h` compiler version checks (`STL1000`, `STL1001`, `STL1002`); unsupported by Microsoft STL after compiler bug workarounds for Clang < 22 are removed | Unsupported escape hatch |

## Consequences

- **Positive**: `Configure libvmaf (SYCL)` on `windows-2025` (`win25-vs2026`)
  is intended to compile against MSVC 14.52's STL headers;
  `scripts/ci/test_windows_sycl_toolset_contract.py` checks the workflow pin.
- **Negative**: `libvmaf-build-matrix.yml` pins `2026.2.0.661` while
  `build-config.env`'s `ONEAPI_WINDOWS_VERSION` (`2025.3.0.372`, consumed by
  `windows-tester-bundle.yml` under [ADR-1566](1566-windows-sycl-tester-zip.md))
  remains separate until the Windows SYCL tester zip's runtime closure and
  SHA-256 lock are updated together. The 2026.2 runtime file names have not
  been measured.
- **Neutral / follow-ups**: Supersedes the Windows oneAPI `2025.3.0.372` pin of
  [ADR-0121](0121-windows-gpu-build-only-legs.md) for `libvmaf-build-matrix.yml`.

## References

- [ADR-0121](0121-windows-gpu-build-only-legs.md) — Windows GPU build-only
  matrix legs (initial `2025.3.0.372` pin).
- [ADR-1566](1566-windows-sycl-tester-zip.md) — Windows SYCL tester zip
  (`ONEAPI_WINDOWS_VERSION` in `build-config.env`); its lane pin is superseded
  in part by this ADR, while the tester zip pin stays.
- Intel oneAPI DPC++/C++ Compiler 2026 Release Notes
  (<https://www.intel.com/content/www/us/en/developer/articles/release-notes/oneapi-dpcpp/2026.html>):
  version 2026.0.0 supports the C and C++ standards through the Clang 22 front
  end (checked 2026-11-19; source statement supplied with this change).
- Intel oneAPI Toolkit Installation Guide for Windows 2026.2
  (`https://www.intel.com/content/www/us/en/docs/oneapi-toolkit/installation-guide-windows/latest/install-oneapi-toolkit-with-installer.html`,
  published `2026-10-15`, modified `2026-10-27`).
- `actions/runner-images` `win25-vs2026/20261116.315`
  (`images/windows/Windows2025-VS2026-Readme.md`) and `microsoft/STL`
  `stl/inc/yvals_core.h` (`STL1000`, Clang 22 requirement).
