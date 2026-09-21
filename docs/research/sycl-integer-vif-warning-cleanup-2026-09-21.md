# SYCL integer VIF warning cleanup — 2026-09-21

## Finding and scope

`core/src/feature/sycl/integer_vif_sycl.cpp` hid strict-analysis debt behind
file-wide and function-level `NOLINT` ranges. Removing those suppressions
exposed three HISS-04 overlength phases, external-linkage findings for every
private helper, designated-initializer ordering warnings, and five forced-loop
unroll requests that oneAPI 2026 could not honour for every supported device
target.

This is a fork-local implementation cleanup under ADR-0141 and ADR-1142. It
changes no feature name, option, public header, metric definition, tolerance,
golden assertion, build option, or FFmpeg patch surface. No ADR is needed:
there is no policy choice or user-visible behaviour change, and the only
acceptable implementation removes the diagnostics while preserving the
integer arithmetic and callback contract.

## Preservation contract

- Keep every device-kernel operand fp64-free. The host stores the configured
  gain as `double`, then narrows it to `float` before kernel dispatch; score
  aggregation remains host-side `double`.
- Preserve tap order, multiply/add widths, rounding constants, shift order,
  mirror indexing, subgroup reduction order, and `sycl::fmin` gain limiting.
- Preserve ceiling downsample stride `(width + 1U) / 2U` and matching device
  allocation for odd intermediate widths.
- Preserve separate and fused execution modes, SIMD-16/SIMD-32 selection,
  graph callback ordering, allocation cleanup, and feature-name derivation.
- Keep the Netflix copyright notice and all Netflix golden assertions
  untouched.

The vertical convolution now has a typed accumulator phase. The horizontal
kernel has a work-item phase with a trivially copyable launch-parameter record.
Initialization is split into resource, device-selection, and graph-registration
phases while retaining the original cleanup points. Short anonymous namespaces
provide C++ internal linkage without a blanket suppression and without forming
an overlength HISS scope. Failed `#pragma unroll` requests are removed; the
compiler remains free to unroll profitable target-specific instances.

## Validation

The owned source was checked without a suppression, baseline, threshold, or
golden-data change:

- `praetorctl audit -base e0d31c5bcb43a7aab7fb845a592488635b061566
  -touched core/src/feature/sycl/integer_vif_sycl.cpp` reports the touched file
  clean and the repository ratchet unchanged at 1411.
- `scripts/ci/clang-tidy-sycl.sh -p core/build-sycl-vif
  core/src/feature/sycl/integer_vif_sycl.cpp --quiet` reports zero diagnostics
  in the owned source under the full configured profile.
- An independent oneAPI 2026.0 `icpx` compile using
  `-fsycl-targets=spir64_gen,spir64`, `-fp-model=precise`, and the current
  `-Xsycl-target-backend=spir64_gen` spelling exits zero with zero diagnostics.
- On an Intel Arc A380 that reports no native fp64, `test_sycl_vif_parity`,
  `test_sycl_vif_parity_large`, `test_vif_skip_scale0`, and
  `test_vif_lifecycle` pass (4/4).
- Relinking the same build with the untouched base-commit object gives exact
  full-precision equality for default and fused modes: 720/720 emitted metrics
  each on the 576x324 8-bit 48-frame fixture, and 45/45 each on a deterministic
  258x146 10-bit three-frame fixture whose later scales have odd dimensions.
  Maximum absolute delta is zero in all four comparisons.

The existing parity and lifecycle tests already exercise the public seam, so
no expected value or duplicate test harness is added. The local
`dev-llm-review` command was also attempted as a read-only second opinion, but
the installed repository virtualenv has a broken entry point
(`ModuleNotFoundError: vmaf_dev_llm.cli`); no local-model claim is included.
