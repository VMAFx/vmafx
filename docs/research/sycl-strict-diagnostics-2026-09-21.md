# SYCL strict-diagnostics cleanup — 2026-09-21

## Finding

The oneAPI feature-kernel path had two independent blind spots. Several SYCL
translation units built successfully while emitting file-local clang-tidy and
HISS findings, and the Meson AOT command forwarded an Intel device selector
with unqualified `-Xs` even though the command also emitted a portable
`spir64` target. oneAPI 2026 therefore reported the selector as unused. The
custom-command compilation-database translator also did not understand the
target-qualified replacement, so changing the build flag alone would have
broken the analyzer projection.

This is whole-tree policy: a diagnostic is actionable regardless of whether
the affected source began in Netflix/vmaf or in the fork. No golden assertion,
numeric tolerance, baseline, or suppression was changed.

## Repair and preservation contract

- Split long kernel, allocation, submission, collection, and cleanup paths at
  existing phase boundaries; preserve filter coefficients, loop and reduction
  order, option values, buffer geometry, feature names, and fp32-only device
  arithmetic.
- Replace narrowing and widening expressions with explicit size-domain
  arithmetic and validate every allocation used by the touched path.
- Scope `-device <list>` to `spir64_gen` with
  `-Xsycl-target-backend=spir64_gen`, retaining `spir64` as the portable
  fallback.
- Let Meson's built-in `c_std` and `cpp_std` fallback lists select compiler
  spellings. Keep only the MSVC C `/std:clatest` override needed to exceed its
  `c17` mapping.
- Strip both legacy and target-scoped AOT backend arguments from the synthetic
  clang-tidy command. A pre-commit unit test locks the Meson and translator
  contracts together.

## Alternatives considered

Ignoring diagnostics from inherited sources was rejected because origin does
not change runtime risk. Adding `NOLINT`, lowering a ratchet, or filtering the
affected rules was rejected because each finding had a direct structural or
type-safe repair. Dropping the portable `spir64` target would silence the AOT
warning but remove fallback-device support. Keeping manual language-standard
probing was rejected because it duplicated Meson's selected standard and was
itself the source of configure warnings.

## Validation

The acceptance gate is a oneAPI object build plus file-local clang-tidy and
HISS checks for every touched SYCL translation unit, the SYCL AOT contract unit
test, and a fresh Meson configure that reports no standard-option or AOT
warning. Cppcheck and repository pre-commit hooks run over the final staged
change. Exact commands and tool versions are preserved in the pull-request
receipt.
