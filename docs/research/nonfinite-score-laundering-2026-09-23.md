# Non-finite score laundering in the metric engine

## Finding

A sweep of 466 feature-collector append sites found twelve expressions where
IEEE-754 unordered comparisons hid a failed computation behind a finite score.
The defect was not that a NaN could exist; it was that `MIN`, `MAX`, a threshold
comparison, or a default output converted it into a number downstream could no
longer distinguish from a measurement. The most severe mappings were
SSIMULACRA2 `NaN -> 100.0`, ADM AIM `0/0 -> 1.0`, and SSIM/MS-SSIM
`NaN -> max_db`.

The finite paths are deliberately unchanged. Netflix golden assertions remain
the authority and were not edited. The implementation validates immediately
before a laundering comparison, returns `-EINVAL`, and avoids publishing any
output for the failed frame. Where an enclosing function was already at the
strict size limit, a pure scoring helper became the testable seam rather than
adding an inline exception.

## Reproductions

Three tests were first run against bug-preserving helper extractions:

- `piecewise_linear_mapping(NAN, ...)` returned success and changed `42.0` to
  the plausible prediction `0.0`;
- ADM with finite `num/den` and `aim_num/aim_den == 0/0` returned success and
  changed the AIM output to the perfect `1.0`;
- SSIMULACRA2's final mapping accepted `NAN` and returned the perfect `100.0`;
- TransNet accepted a `NAN` logit and published boundary flag `0.0`.

Each test failed before the finite guard and passes after it. They also cover
infinities, unchanged caller outputs on failure, finite ratios and thresholds,
ADM's legitimate flat-frame result, SSIMULACRA2's finite sign split, and the
zero-logit TransNet threshold.

## Backend scope

SSIMULACRA2 duplicated its host-side edge split and polynomial pool across the
scalar reference, AVX2, AVX-512, NEON, SVE2, CUDA, HIP, SYCL and Metal hosts.
Only fixing the scalar file would leave backend-dependent failure semantics.
`ssimulacra2_score.h` is therefore the single implementation for both
operations; each extractor still performs its own named frame log and returns
`-EINVAL` before collector publication.

ADM's helper validates raw operands, denominators and computed results before
writing either output. The `den == 0` flat-frame branch remains exactly `1.0`
for both ADM and AIM, but it no longer overrides a non-finite operand. The ADM3
helper retains the defined all-zero harmonic mean while rejecting a non-finite
blend before `adm_min_val` can hide it.

MS-SSIM previously appended the luma plane before it knew whether an enabled
chroma plane was valid. Its extraction now computes and validates all enabled
planes and all L/C/S atoms first, then appends them, preserving the fail-frame
atomicity stated by ADR-1302.

## Reproduce

```sh
meson setup core/build-nonfinite core -Db_lto=false \
  -Denable_cuda=false -Denable_sycl=false -Denable_hip=false \
  -Denable_metal=disabled -Denable_dnn=disabled
ninja -C core/build-nonfinite -j4
meson test -C core/build-nonfinite --print-errorlogs \
  test_adm_nonfinite_score test_ssimulacra2_nonfinite \
  test_ssimulacra2_coverage test_predict test_transnet_v2 \
  test_float_ms_ssim_coverage
```

The release gate remains `make test-netflix-golden`; a passing unit test is not
a substitute for the unchanged golden scores.

On the completed worktree, the focused six-test command passes 6/6, the full
Meson fast suite passes 148/148, and the five-file Netflix Python gate reports
`271 passed, 12 skipped`. No Netflix-authored assertion or expected value is
changed. Release builds with CUDA 13.4/NVCC, ROCm 7.2/HIPCC (`gfx1100`), and
oneAPI 2026.0/SYCL (SPIR-V JIT) also compile the shared SSIMULACRA2 guard and
their respective host twins successfully. Metal remains compile-verified by
CI because this Linux workstation has no Apple toolchain.

The backend-twin edits are the same mechanical helper substitution in files
that carry historical HISS debt. The local governance gate was therefore run
with `PRAETOR_TOUCHED_DEBT_DELTA_REASON` set to that exact parity rationale;
the stricter debt-delta audit passes at 268 active findings against the 276
finding baseline, with no new debt.
