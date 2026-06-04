<!-- markdownlint-disable MD013 MD041 MD060 -->

# ADR-1011: Add `static` to TU-internal CUDA helper functions — VIF, ADM, motion

- **Status**: Accepted
- **Date**: 2026-06-04
- **Deciders**: Lusoris
- **Tags**: `core`, `cuda`, `correctness`, `build`

## Context

Three CUDA feature-extractor files contain TU-internal helper functions that are
not declared in any header and are never called from outside their translation
unit, yet are missing the `static` storage-class specifier:

- **`integer_vif_cuda.c:331,362`** — `filter1d_8` and `filter1d_16`: called only
  via function-pointer assignment `s->filter1d_8 = filter1d_8` on line 300 within
  the same TU.  The exported symbols clash with any future code that defines
  functions with the same common names.

- **`integer_adm_cuda.c:118–432`** — Nine device-dispatch helpers
  (`dwt2_8_device`, `adm_dwt2_s123_combined_device`, `adm_dwt2_16_device`,
  `adm_csf_device`, `i4_adm_csf_device`, `adm_csf_den_s123_device`,
  `adm_csf_den_scale_device`, `i4_adm_cm_device`, `adm_cm_device`).  The HIP
  twin file `integer_adm_hip.c` already marks all of these `static` — this PR
  restores parity.

- **`integer_motion_cuda.c:247`** — `calculate_motion_score`: called only through
  `s->calculate_motion_score` assigned in the same TU.

When libvmaf is statically linked into a host application that also defines any of
these common-named symbols, the linker will silently pick one definition, producing
incorrect results or crashes depending on link order.  The `-fvisibility=hidden`
compiler flag (used by Meson for shared-lib builds) removes them from the DSO's
export table but does not eliminate the ODR risk in static-lib builds.

## Decision

Add `static` to each of the 12 function definitions.  No header declarations
exist for these functions, so no header change is required.  The HIP twin
(`integer_adm_hip.c`) already follows this pattern.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| `-fvisibility=hidden` build flag | Fixes DSO exports | Does not fix static-lib ODR; already present | Insufficient alone |
| NOLINT | Zero diff | Masks real ODR hazard; CLAUDE.md requires justification | No justification; not a load-bearing invariant |
| Add `static` | Eliminates ODR risk, matches HIP twin | Trivial diff | Chosen |

## Consequences

- **Positive**: 12 symbols removed from object-level export namespace; ODR hazard
  eliminated for static-lib builds; parity with HIP twin restored.
- **Neutral**: No behaviour change; function-pointer assignments are within the same
  TU so visibility change is transparent to callers.
- **Negative**: None.

## References

- r3/r4 audit findings: `[r3-symbol-visibility]` cluster
- ADR-0141 (touched-file cleanup rule)
- `integer_adm_hip.c` — reference implementation already uses `static` for all helpers
