<!-- markdownlint-disable MD013 MD060 -->
# ADR-3568: Share oneAPI 2026.2 across Windows SYCL CI and the tester zip

- **Status**: Accepted
- **Date**: 2026-11-20
- **Deciders**: lusoris
- **Tags**: `ci`, `windows`, `sycl`, `build`

## Context

The Windows tester zip's `x64-sycl` build still used oneAPI 2025.3.0.372
with Clang 21 after the `win25-vs2026` runner moved to MSVC 14.52. Its STL
requires Clang 22 or newer and aborts with `STL1000`. ADR-3567 moved the
Windows MSVC+SYCL CI leg to oneAPI 2026.2.0.661. The required Windows tester
zip leg continued to fail and blocked the Required Checks Aggregator.

ADR-3567 measured six targets that the new `ocloc` refuses even though
Meson includes them by default: `tgllp`, `adl-s`, `adl-p`, `adl-n`, `rpl-s`
and `rpl-p`. The build matrix already probes the accepted targets and
disables Visual Studio 2026 integration in the installer. The tester zip
still used Meson's default targets and lacked the integration switch. Its
runtime manifest and licence record describe the previous installer's DLLs.
The staging helper used to stop at the first unmatched file pattern.

## Decision

Pin the Windows oneAPI release, offline installer URL, size, SHA-256 and
expected AOT refusals in `build-config.env` and load them in both Windows
SYCL jobs. Take that config from the workflow recipe when the tester builds
an older source ref. In both jobs, verify the installer before extraction,
skip Visual Studio 2026 integration and probe `ocloc` before passing its
accepted targets to Meson. Use this PR's Windows CI run to measure the
runtime DLL names. Report every unmatched runtime pattern and list the DLLs
present in each affected directory in a single staging error.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Keep two pins with an equality test | Workflows can continue to load pins as before | Each release requires duplicate changes and a consistency check | Both jobs use the same installer and can read one source |
| Define `_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH` | Gets past the STL version check without changing the installer | Builds with an unsupported compiler and STL pairing | The toolchain can satisfy the runner's compiler requirement |
| Drop the SYCL tester zip from the required set | Removes the immediate merge blocker | Allows a broken downloadable artifact to reach the default branch | The required check should continue to cover this artifact |

## Consequences

- **Positive**: One installer update applies to both jobs. The AOT check
  detects changes to the refused set before the build, and the runtime error
  reports all missing patterns in one run.
  The SYCL zip recipe overlays the AOT probe's parser for older source refs, and
  both Windows SYCL selectors follow changes to the shared pin and its loader.
- **Negative**: A change to Intel's accepted targets stops both jobs until
  the expected set and the reason for the change are recorded.
- **Neutral / follow-ups**: Windows Tester Zip run 44821793070 on 2026-11-20
  listed `sycl9.dll` in place of `sycl8.dll` and `ur_loader_shared.dll` in
  place of `ur_loader.dll`. The runtime manifest and licence record use the
  measured names. After pruning, the build requires each run-time DLL name to
  occur as a whole name in another Intel DLL that the bundle ships. Check the
  recorded UMF and TCM versions against the resolved component directories
  printed by `prepare_build.py intel-runtime`. This decision replaces the tester-pin
  consequence of ADR-3567 and the tester pin of ADR-1566.

## References

- [ADR-3567](3567-windows-sycl-oneapi-2026-2.md) records the compiler
  compatibility failure, installer measurements and AOT probe results.
- [ADR-1566](1566-windows-sycl-tester-zip.md) describes the tester zip and
  its runtime packaging and licensing requirements.
- Source: maintainer request vmafx-merge-queue-critical-20261120
  (2026-11-20, Windows SYCL tester zip, paraphrased).
