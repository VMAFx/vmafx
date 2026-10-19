<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1918: Samples above 2^bpc - 1 are invalid input; an opt-in check refuses them

- **Status**: Accepted
- **Date**: 2026-10-06
- **Deciders**: lusoris
- **Tags**: `api`, `cli`, `correctness`, `cuda`, `sycl`, `hip`, `metal`, `rc3`, `fork-local`

## Context

A picture of bit depth 9 to 15 stores its samples in `uint16_t`, so it can
carry values above 2^bpc - 1. No libvmaf entry point checks it, and the RC3
accumulator audit (`docs/development/accumulator-bounds.md`) found more than
a dozen integers that the CPU keeps wide, or truncates, but that a SIMD path or
a GPU twin narrows, or the reverse, once a sample passes the limit: the twins
then give scores that differ from the CPU's
(`T-OUT-OF-RANGE-SAMPLES-TWIN-DIVERGENCE-2026-10-05`). With in-range samples
every one of those integers is safe.

## Decision

The API states the caller contract: every sample is at most 2^bpc - 1, and
out-of-range input is invalid, with twin results that may differ. An opt-in
check enforces it: `vmaf_set_sample_range_check_enabled(VmafContext *, int)`
(additive, after the precedent of `vmaf_set_perceptual_weight_enabled()`) and
the CLI flag `--check-sample-range` (underscore alias `--check_sample_range`,
as the fork's other kebab-case flags have). When on,
`vmaf_read_pictures()` scans both pictures before any extractor and returns
`-EINVAL` for the first out-of-range sample, logging picture, plane, row,
column and value; a device picture returns `-ENOTSUP`. Off by default, with no
sample read: the default path costs one flag test per call. The FFmpeg filters
do not get the option.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Contract plus opt-in check (chosen; maintainer's popup choice) | Default path unchanged in cost and output; a caller with raw input of unknown range can find the bad sample; one implementation for every backend | Out-of-range input without the check still scores differently across twins | Chosen |
| Always check | Every backend agrees by construction | A full read of every sample on every frame, for input that is almost always in range | Cost on the default path |
| Clamp out-of-range samples | Never fails | Changes the input the caller passed and hides the error; would need a copy of a picture the caller owns | Silent substitution |
| Make every twin follow the CPU's widths and truncations | Out-of-range input scores the same everywhere | More than a dozen kernels across CUDA, HIP, SYCL, Metal, AVX2, AVX-512 and NEON changed for invalid input | Large change for input the contract rules out |
| Also expose the check on the FFmpeg filters | Parity with the CLI | Their frames come from FFmpeg's decoders and scalers, which keep samples in range; a patch to the series for a case that does not arise there | Not needed |

## Consequences

- **Positive**: the contract is written down (`libvmaf.h`, `docs/api/sample-range.md`);
  a frame with a bad sample can be found and refused with its position.
- **Neutral**: the twins stay as they are for out-of-range input; the state
  row closes on the contract and the check.
- **Negative**: one more public entry point and CLI flag to keep.

## References

- Q: "Contract + opt-in check (Recommended)" (maintainer popup, 2026-10-06, relayed by the orchestrator).
- State row `T-OUT-OF-RANGE-SAMPLES-TWIN-DIVERGENCE-2026-10-05` and `docs/development/accumulator-bounds.md` (RC3 accumulator audit).
