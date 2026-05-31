<!-- markdownlint-disable MD060 -->
# ADR-0691: VMAFX Phase 1C — Drop Legacy Build Paths

- **Status**: Accepted
- **Date**: 2026-05-28
- **Deciders**: lusoris
- **Tags**: `ci`, `build`, `vmafx`

## Context

The VMAFX rebrand (umbrella ADR-0686, PR #1546) identified several CI build
legs that no longer serve the fork's technical goals and carry ongoing
maintenance cost. The fork targets 64-bit x86-64 and ARM64 platforms; GPU
backends are exercised exclusively through Linux or MSVC toolchains; ONNX
Runtime static linking with downstream consumers has known symbol-collision
issues. Removing these legs reduces CI wall-clock time per PR, eliminates
platform-specific workaround accumulation, and narrows the supported
configuration surface to configurations the fork actively uses and intends
to ship.

## Decision

Remove the following CI build configurations from `.github/workflows/`:

1. **MinGW64 Windows build** (`Build — Windows MinGW64 (CPU)`): the
   `windows:` job in `libvmaf-build-matrix.yml`, including the MSYS2/MinGW-w64
   setup, ccache caching, and `vmaf.exe` artifact upload. Windows coverage
   is retained through the MSVC + CUDA and MSVC + oneAPI SYCL legs, which
   exercise the fork-added GPU paths and are the build configurations the
   fork supports for Windows production use. Also remove the corresponding
   required-check entry from `required-aggregator.yml`.

2. **i686 / no-asm 32-bit Linux gcc** (`Build — Ubuntu i686 gcc (CPU, no-asm)`):
   the matrix entry with `i686: true` and `--cross-file=build-aux/i686-linux-gnu.ini
   -Denable_asm=false`, the `Install dependencies (ubuntu i686)` step, and all
   `!matrix.i686` guards in subsequent step conditions. The fork is 64-bit only.

3. **Static + DNN combos**: no such combinations existed in the current matrix
   (DNN legs used dynamic linking; the static legs did not enable DNN). This
   item is recorded as a no-op for traceability against the umbrella ADR-0686
   scope.

4. **`python/vmaf/` classic-harness wheel publication**: no wheel-publication
   job for the classic harness existed in CI at the time of this ADR. The
   Netflix golden test suite (`python/test/*_test.py`) runs unchanged through
   `tox` in the existing matrix legs. This item is also a no-op.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep MinGW64 leg | Retains GCC Windows build coverage | ~8-10 min per PR for a configuration the fork does not ship; accumulating MinGW-specific workarounds (ccache.exe renaming, locale fixes, MSYS2 PATH surgery) | The fork ships Windows binaries via MSVC; MinGW is not a supported distribution channel |
| Keep i686 leg | Pins the Netflix#1481 `_mm256_extract_epi64` workaround contract | ~5-6 min per PR; 32-bit is outside the fork's scope; cross-build marks all tests SKIP so correctness is not actually verified | The fork is 64-bit only; the ASM-disable workaround is documented in ADR-0151 for historical record |
| Keep static+DNN option | Would catch ONNX Runtime static-link symbol collisions | Already absent from the matrix; adding it would create the problem it claims to prevent | Not present; no-op |

## Consequences

- **Positive**: PR CI queue is shorter. Fewer platform-specific workarounds
  accumulate. The Windows required-check list accurately reflects the
  configurations the fork ships.
- **Negative**: No GCC Windows build coverage. If a GCC-specific Windows
  regression is introduced, CI will not catch it. Accepted trade-off given
  the fork's MSVC-based Windows release path.
- **Neutral / follow-ups**: The `build-aux/i686-linux-gnu.ini` cross-file
  remains in the repo for historical reference; it is no longer referenced
  by any CI job. ADR-0151 (documenting the 32-bit ASM workaround) is not
  removed — it is a historical record.

## References

- Umbrella: [ADR-0686](0686-vmafx-rebrand-umbrella.md) (VMAFX rebrand Phase 1)
- Folded-in MinGW ADR: [ADR-0115](0115-windows-build-integrated.md)
- i686 workaround: [ADR-0151](0151-i686-no-asm-ci.md)
- Required checks aggregator: [ADR-0313](0313-required-checks-aggregator.md)
- PR: `#1547` (chore(build): drop legacy build paths per VMAFX rebrand plan)
- Source: `req` — per user direction in the Phase 1C task brief (2026-05-28)
