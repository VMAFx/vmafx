# Research-2090: SYCL silent-revert residuals — 2026-09-24

## Question

BUG-048 retained two unchecked findings from the 2026-09-18 silent-revert
ledger:

1. Did commit `5d070b0b4` restore fp64 device arithmetic that commit
   `9f83d352a` had removed from the SYCL SpEED chroma and temporal kernels?
2. Did the same commit erase the explicit kernel-capture aliases added by
   `025656197`, and are those aliases still the current implementation shape?

The audit also reproduced the first open `docs/state.md` row,
`T-SYCL-SPEED-CHROMA-BOTH-SINGULAR-REGRESSED-2026-09-23`, on the local Intel
Arc A380. That exposed a separate live SpEED correctness defect while this
scope was already exercising the affected kernels.

The audit used exact `origin/master` at
`4e6916d16ac57647105d14a47a6680117d6b5738`. It compared current source with
both historical fixes and the removing commit; no historical commit was
cherry-picked.

## Historical findings

| Historical item | Current evidence | Disposition |
| --- | --- | --- |
| fp32 `speed_chroma_sycl.cpp` | The device region contains no `double`. Its covariance reduction now uses a compensated two-float expansion, a newer implementation than `9f83d352a`. | Already restored and superseded. |
| fp32 `speed_temporal_sycl.cpp` | The device region contains no `double`; means, covariance, local storage, and reduction are all `float`. | Already restored and superseded. |
| `float_psnr_sycl.cpp` `e_partials` alias | `FpsnrOutput` now carries the pointer across the kernel-capture boundary and the lambda writes `output.partials`. | Superseded by a stronger argument-struct boundary. |
| `integer_psnr_sycl.cpp` `e_sse` alias | `PsnrKernelArgs` now carries the pointer and the lambda constructs its atomic from `args.sse`. | Superseded by a stronger argument-struct boundary. |
| `integer_moment_sycl.cpp` `e_sums` alias | `git blame` attributed all four raw `atomic64(d_sums[...])` uses to removing commit `5d070b0b4`; no later refactor replaced that capture shape. | Live residual; restore the explicit alias. |

The historical alias patch described the issue as a low-severity consistency
finding and explicitly reported no numerical change. Restoring `e_sums` is
therefore a source-invariant repair, not a score change.

## Arc SpEED root cause

The existing 768x432 SpEED parity fixture gives a 384x216 chroma plane and only
`4 x 2 = 8` samples for a 25x25 covariance. It is rank-deficient by
construction, so it never exercises the regular solve. The 960x960 singular
regression first runs a textured frame with 36 chroma samples and reaches the
regular path. On exact master that first frame failed after the newer
non-finite-score guard exposed a NaN that the old clamp had published as
`speed_chroma_max_val` (1000).

Instrumentation narrowed the failure without changing the tolerance:

- both host covariance matrices were finite and exactly symmetric;
- all host and device-copied eigenvalues were finite and positive;
- all 36 device variances were finite and positive;
- recomputing entropy on the host from those exact device values was finite;
- the device entropy was NaN for every block.

Changing the captured `log2(2*pi*e)` constant from `sycl::log2` to
`std::log2` did not change the failure and was reverted. A targeted probe then
showed the real ABI mismatch: the host computed `sigma_nn=0.29` and
`log2e_2pi=4.09419`, while the device kernel read `0.29` when asked to emit the
captured `log2e_2pi` value and read a NaN through the next capture.

`speed_chroma_sycl.cpp` and `speed_temporal_sycl.cpp` each defined anonymous
SYCL kernels inside functions named `launch_score` and `launch_indterm` with
identical signatures. The compiler generated identical kernel type names in
the two object files. Comparing their `_ZTSZZ...` kernel tags before the fix
showed two intersections: `launch_score` and `launch_indterm`. Final device
linking could therefore select one twin's device image while the host launcher
packed the other twin's different closure layout.

The minimum fix gives the enclosing functions role-specific identities:

- `launch_chroma_score` and `launch_chroma_indterm`;
- `launch_temporal_score` and `launch_temporal_indterm`.

After rebuilding both objects, the generated kernel-tag intersection is empty.
No arithmetic expression, score tolerance, fixture, or golden value changed.

## Red caps

`core/test/test_sycl_kernel_source_contract.py` is the portable source seam. It
checks that:

- both SpEED device regions remain fp64-free;
- the four role-specific SpEED launch names remain distinct across translation
  units;
- the current PSNR argument structs remain intact;
- integer moment uses `e_sums` for all four atomics.

Three planted mutations prove the fp64, ambiguous-kernel-name, and raw-capture
regression shapes are rejected. Before the moment production edit, the live
source test failed with:

```text
integer_moment_sycl.cpp: missing explicit e_sums capture alias
integer_moment_sycl.cpp: kernel uses the raw d_sums parameter
```

`core/test/test_sycl_speed_singular_parity.c` is the runtime red cap. Exact
master failed its chroma case on the Arc before the role-specific naming fix;
the same executable passes afterward at the unchanged `1e-4` ADR-0214
tolerance.

## Alternatives considered

| Option | Result |
| --- | --- |
| Cherry-pick `9f83d352a` and `025656197` | Rejected. It would overwrite later SpEED correctness work and both PSNR argument-struct refactors. |
| Change `sycl::log2` or loosen the parity tolerance | Rejected by experiment. The host-math substitution left the failure unchanged, and finite input values proved this was neither an accuracy delta nor a domain error. |
| Rename only `launch_score` | Rejected. Object inspection proved `launch_indterm` had the same cross-translation-unit identity collision even though its two bodies currently agree closely enough not to expose a score failure. |
| Add explicit template kernel-name classes | Valid but unnecessary. Role-specific enclosing-function names already produce unique compiler kernel identities with less boilerplate. |
| Restore only `e_sums`, uniquely name both SpEED launcher pairs, and guard the contracts | Selected. This is the minimum repair for every live residual found by the bounded audit. |

No new ADR is needed: ADR-0220 already owns the fp64-free device contract,
ADR-0214 owns the unchanged parity threshold, and the remaining changes repair
implementation defects rather than choose new architecture.

## Scope and reproduction

The change is internal to fork-local SYCL source and tests. It changes no public
header, C API, CLI flag, Meson option, FFmpeg patch, output schema, score
snapshot, or Netflix golden assertion.

Portable reproducer:

```sh
python3 core/test/test_sycl_kernel_source_contract.py -v
```

Arc runtime reproducer:

```sh
ONEAPI_DEVICE_SELECTOR=level_zero:gpu meson test -C build-sycl \
  test_sycl_speed_chroma_parity test_sycl_speed_temporal_parity \
  test_sycl_speed_singular_parity test_sycl_float_moment_parity \
  test_sycl_kernel_source_contract --print-errorlogs
```

## Verification

The focused source contract passes all four tests. With oneAPI 2026.1.1 and
Level Zero `1.17.39758+10`, the local Intel Arc A380 run is green:

```text
test_sycl_kernel_source_contract         OK
test_sycl_float_moment_parity            OK
test_sycl_speed_chroma_parity            OK
test_sycl_speed_temporal_parity          OK
test_sycl_speed_singular_parity          OK
Ok: 5  Fail: 0
```

The clean CPU build's complete fast suite passes 148/148. `make verify-all`
passes, as does the full pre-commit policy over every changed file.

The scoped SYCL clang-tidy measurement used the baseline's exact clang-tidy
22.1.8 and a freshly generated oneAPI 2026.0 compilation database. All three
touched translation units compile without a tool failure, report zero
translation-unit diagnostics, and contain zero uncited suppressions. The 62
reported diagnostics are confined to pre-existing included headers. The
committed baseline was not rewritten.
