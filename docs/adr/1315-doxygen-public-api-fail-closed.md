<!-- markdownlint-disable MD013 MD060 -->

# ADR-1315: Drive libvmaf public C API Doxygen warnings to zero and fail closed

- **Status**: Accepted
- **Date**: 2026-09-25
- **Deciders**: lusoris
- **Tags**: `docs`, `ci`, `api`, `public-surface`, `doxygen`

## Context

[ADR-0953](0953-doxygen-public-api-clean.md) created `core/doc/Doxyfile.public-api` and
the `.github/workflows/doxygen-public-api.yml` CI workflow to establish a clean
documentation baseline for the public C headers under `core/include/libvmaf/`.
While [ADR-1297](1297-ci-gate-every-reporting-check.md) promoted the `Doxygen Public API`
workflow to a required status check in `required-aggregator.yml`, the check was configured
with `DOXYGEN_WARNING_CEILING: "228"` rather than failing closed, and `WARN_AS_ERROR`
remained disabled in the Doxyfile.

On 2026-09-22, state item `T-DOXYGEN-PUBLIC-API-WARNINGS-REGRESSED-2026-09-22` recorded
that the public API had accumulated 228 warning lines in `build/doxygen-public-api/warnings.log`.
A complete inventory and classification of the 228 warnings revealed three underlying causes:

1. **Vendored Pelorus interop mirror (159 warnings)**: `core/include/libvmaf/pelorus/`
   (`interop.h`, `denoise.h`, `deband.h`) was vendored in [ADR-1113](1113-vendor-pelorus-interop-abi.md)
   as a byte-identical read-only mirror of external `VMAFx/pelorus`. `scripts/sync-pelorus-interop.sh`
   strictly enforces zero git tree drift against the upstream tag. Furthermore, these headers
   are internal plugin interop definitions and are not installed by `core/include/libvmaf/meson.build`.
   Modifying them locally violates tree sync parity, while leaving them in scope produced 17
   undocumented compounds and 142 undocumented struct members.
2. **Syntax and command mismatches (47 warnings)**:
   - `@field` tags in struct doc comments (`libvmaf.h`, `libvmaf_cuda.h`, `model.h`) are not
     recognized by Doxygen (which requires inline `/**< ... */`), causing the members to be
     flagged as undocumented.
   - `@thread-safety` annotations in `dnn.h` and `picture_v2.h` (headers containing `@file`
     directives) triggered `warning: Found unknown command '@thread'`.
   - Multi-variable member declarations (e.g. `unsigned w, h;` in `libvmaf.h`, `libvmaf_cuda.h`,
     `libvmaf_sycl.h`, `picture_v2.h`, and `model.h`) attach inline comments only to the final
     symbol, leaving preceding symbols undocumented.
   - Cross-symbol `@ref` links in struct doc comments cannot be resolved across translation
     units by Doxygen from struct scope, emitting unresolvable reference warnings.
3. **Missing public struct field documentation (22 warnings)**:
   - All 11 members of `VmafPicture2` in `picture_v2.h` ([ADR-0928](0928-vmaf-picture-v2-explicit-backend-state.md))
     lacked member documentation.
   - Anonymous nested sub-struct variables (`pic_params` in `libvmaf.h`, `libvmaf_cuda.h`,
     `libvmaf_sycl.h`) lacked doc comments on the variable itself.
   - Nested confidence-interval and bootstrap score members (`ci`, `p95`, `bootstrap` in `model.h`)
     were undocumented.

## Decision

We will eliminate all 228 public-API Doxygen warnings, enforce a zero-warning contract
at both the Doxyfile and workflow levels, and guard the contract with executable tests:

1. **Scope Doxyfile.public-api to installed public headers**:
   Add `*/pelorus/*` to `EXCLUDE_PATTERNS` in `core/doc/Doxyfile.public-api`. Pelorus headers
   are external vendored mirrors not installed by meson.
2. **Remediate all public C headers**:
   - Split all multi-variable member declarations into individual single-variable declarations
     with dedicated `/**< ... */` comments.
   - Remove deprecated `@field` doc blocks and replace them with inline `/**< ... */` comments.
   - Document all nested sub-struct instances and `VmafPicture2` members.
   - Convert struct-scope `@ref` links to backtick literals (`` `...` ``) per ADR-0953.
   - Standardize all `@thread-safety` annotations across `core/include/libvmaf/*.h` to
     `@note Thread safety: ...`.
3. **Fail closed at zero warnings**:
   - Set `WARN_AS_ERROR = YES` in `core/doc/Doxyfile.public-api`.
   - Set `DOXYGEN_WARNING_CEILING: "0"` in `.github/workflows/doxygen-public-api.yml`.
4. **Source-level regression guards**:
   Add `PublicHeaderDoxygenContractTest` to `core/test/test_gpu_public_header_docs.py`
   (in meson's `fast` suite) to assert no `@field` or `@thread` tags return, ensure the
   Doxyfile and workflow keep `WARN_AS_ERROR = YES` and ceiling 0, and run Doxygen to
   verify zero warnings whenever the binary is present.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Zero warnings, fail-closed, and fast-suite regression test (chosen)** | Eliminates all warnings; prevents regression at developer and CI time; respects vendored mirror boundary | Requires touching public header comments and splitting multi-declarations | Restores intended ADR-0953 / ADR-1297 posture without weakening docs |
| Keep warning ceiling at 228 | Zero code changes | Allows future documentation regressions to accumulate silently; masks genuine bugs | Violates fail-closed quality policy |
| Modify `pelorus/*.h` in-tree | Documents pelorus structs | Breaks `scripts/sync-pelorus-interop.sh` and byte-parity with upstream pelorus repo | Pelorus headers are vendored read-only mirrors not installed by libvmaf |
| Enable `EXTRACT_ALL = YES` | Hides undocumented member warnings | Masks missing documentation behind empty stubs | Weakens public API documentation rigor |

## Consequences

- **Positive**:
  - `doxygen core/doc/Doxyfile.public-api` runs with 0 warnings and `WARN_AS_ERROR = YES`.
  - CI workflow `doxygen-public-api.yml` fails closed on any warning (`DOXYGEN_WARNING_CEILING: "0"`).
  - Fast-suite test `core/test/test_gpu_public_header_docs.py` catches syntax regressions before push.
  - Vendored pelorus mirror remains byte-identical to upstream.
- **Negative**:
  - Developers adding new public C symbols or structs must provide complete Doxygen comments or the build fails.
- **Neutral / follow-ups**:
  - Closes state item `T-DOXYGEN-PUBLIC-API-WARNINGS-REGRESSED-2026-09-22`.

## References

- [ADR-0788](0788-doxygen-thread-safety-tags.md): Original thread-safety annotations.
- [ADR-0928](0928-vmaf-picture-v2-explicit-backend-state.md): `VmafPicture2` public API.
- [ADR-0953](0953-doxygen-public-api-clean.md): Doxygen public-API clean baseline.
- [ADR-1113](1113-vendor-pelorus-interop-abi.md): Vendoring of Pelorus interop headers.
- [ADR-1297](1297-ci-gate-every-reporting-check.md): Required status checks aggregator.
- State item: `T-DOXYGEN-PUBLIC-API-WARNINGS-REGRESSED-2026-09-22`.
