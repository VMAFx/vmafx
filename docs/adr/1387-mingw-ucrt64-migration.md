<!-- markdownlint-disable MD013 MD060 -->
# ADR-1387: Migrate Windows MinGW CI leg from deprecated MINGW64 to UCRT64

- **Status**: Accepted
- **Date**: 2026-09-30
- **Deciders**: lusoris
- **Tags**: ci, windows, mingw, ucrt64, build

## Context

Since `msys2/setup-msys2` v2.33.0 (PR #1605), the Windows MinGW leg of `.github/workflows/libvmaf-build-matrix.yml` emitted a deprecation warning:

```text
[msystem-mingw64] MINGW64 is deprecated. Migrate to UCRT64 or CLANG64. To acknowledge and suppress this warning, add 'msystem-mingw64' to 'suppress-deprecation-warnings'.
```

The matrix leg previously configured `msystem: MINGW64` with `MINGW_PACKAGE_PREFIX: mingw-w64-x86_64` under job name `Windows MinGW64`. Per HISS-10 (warning hygiene), CI warnings represent technical debt to resolve rather than acknowledge and suppress.

Beyond the CI deprecation warning, MINGW64 links against the legacy `msvcrt.dll` runtime provided for backwards compatibility in Windows. This legacy runtime has known limitations:

1. `fmaf()` is not fused in `msvcrt.dll`, causing precision and rounding discrepancies between scalar and SIMD/FMA paths ([ADR-1253](1253-scalar-fma-not-fused-on-msvcrt.md)).
2. `msvcrt.dll` lacks per-thread locale support (`_configthreadlocale(_ENABLE_PER_THREAD_LOCALE)` returns `-1`), forcing fallback to process-global locale modifications during thread-local locale isolation windows.
3. MSYS2 designated UCRT64 as its default, primary modern GCC environment targeting Microsoft's Universal C Runtime (`ucrtbase.dll`, standard on Windows 10 and Windows Server 2016+).

## Decision

1. **Migrate the MSYS2 environment to UCRT64**: In `.github/workflows/libvmaf-build-matrix.yml`, update the `windows` matrix row from `msystem: MINGW64` to `msystem: UCRT64`, and update package prefix from `mingw-w64-x86_64` to `mingw-w64-ucrt-x86_64`.
2. **Update job display name and required check context**: Rename the matrix display name from `Windows MinGW64` to `Windows UCRT64`. In `.github/workflows/required-aggregator.yml`, update `const required = [...]` to check `Windows UCRT64` instead of `Windows MinGW64`.
3. **Derive cache and artifacts**: Cache keys (`${{ runner.os }}-${{ matrix.msystem }}-...`) and artifact names (`${{ matrix.msystem }}-vmaf`) automatically derive from `matrix.msystem`, producing `${{ runner.os }}-UCRT64-...` and `UCRT64-vmaf`.
4. **Update documentation**: In `docs/getting-started/building-on-windows.md`, document MSYS2 UCRT64 environment setup with `mingw-w64-ucrt-x86_64-*` package names.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Suppress warning in `setup-msys2` (`suppress-deprecation-warnings: msystem-mingw64`) | Zero code changes; preserves legacy `msvcrt.dll` testing | Leaves deprecation debt open; eventual removal breaks CI; violates HISS-10 | Postpones necessary migration and retains `msvcrt.dll` bugs |
| Migrate to `CLANG64` | Uses modern UCRT | Changes both C runtime and compiler from GCC to Clang; loses GCC testing on Windows | We want to retain GCC MinGW coverage on Windows; Clang and MSVC are tested separately |
| Migrate to `UCRT64` (chosen) | Modern Universal C Runtime (`ucrtbase.dll`); conforms to C99/C11 standards; eliminates MSYS2 deprecation warning; keeps MinGW GCC compiler toolchain | Renames CI context and artifact from MINGW64 to UCRT64 | Direct replacement matching MSYS2 upstream direction |

## Consequences

- **Positive**: Clears MSYS2 setup-msys2 v2.33.0 deprecation warning in CI; tests libvmaf under modern UCRT on Windows GCC; improves math/locale consistency.
- **Negative**: Artifact name changes from `MINGW64-vmaf` to `UCRT64-vmaf`; requires updating `required-aggregator.yml`.
- **Neutral / follow-ups**: Stack alignment verification (`scripts/ci/check-win64-stack-alignment.py`) and full unit test suite continue to run under UCRT64.

## References

- Issue: [#1609](https://github.com/VMAFx/vmafx/issues/1609)
- [ADR-0115](0115-ci-trigger-master-only-and-matrix-consolidation.md) (matrix consolidation)
- [ADR-0116](0116-ci-workflow-naming-convention.md) (matrix job naming)
- [ADR-1253](1253-scalar-fma-not-fused-on-msvcrt.md) (scalar FMA on msvcrt)
- [ADR-1254](1254-win64-cannot-realign-the-stack.md) (Win64 stack alignment guard)
- [ADR-1297](1297-ci-gate-every-reporting-check.md) (required check aggregation)
