<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-2795: Integer ADM evaluates two viewing distances in one context

- **Status**: Accepted
- **Date**: 2026-10-08
- **Deciders**: lusoris
- **Tags**: feature, adm, rust, upstream-port, performance

## Context

Two models that differ only in the ADM viewing distance, such as
`vmaf_v1.0.16_3d0h` and `vmaf_v1.0.16_5d0h`, register two `adm` contexts. Each
runs the four-scale wavelet transform and the decouple stage, although neither
depends on the viewing distance; only the contrast-sensitivity weights, the
denominator and the contrast masking do.

Upstream Netflix/vmaf `33e5f0aca` adds an optional `merge` callback to the
extractor descriptor, which the registry calls when a second instance of the
same extractor with other options arrives. `cffd5b77d` implements it for
integer ADM: an option `adm_norm_view_dist_extra` (`nvde`), a second
feature-name dictionary rendered at that distance, and a per-frame loop that
runs the transform and decouple once per scale and the weighting once per
distance. Upstream's version has two gaps, reproduced on its master
`9cb9479f2` (docs/development/known-upstream-bugs.md):

- a third model at the distance a context already evaluates second is
  registered as a context of its own, both write the same feature names, and
  the run fails (`vmaf` exits 234);
- a context with `debug=true` is folded in, and its debug scores are lost.

The fork has three things upstream does not: Rust twins (ADR-1713), which
copy the C descriptor and so its callbacks; a feature-name dictionary the
Rust twin shim builds itself; and the GPU twins' option-table parity tests.

## Decision

We will port both commits and close the two gaps:

- `merge` goes into `VmafFeatureExtractor` as upstream has it. The registry
  (`fex_ctx_vector.cpp`) offers a context only after no registered context
  duplicates it, only to contexts that are not initialized and carry the
  same extractor name and callback.
- `adm_merge_view_dist()` folds an incoming context whose options differ only
  in `adm_norm_view_dist` (compared through the feature names with the
  distance neutralised, plus `adm_skip_aim`), declines an incoming context with
  `debug` or a second distance of its own, and absorbs one at the distance
  the existing context already evaluates second.
- A second descriptor hook, `extend_name_dict`, adds the second distance's
  keys (`<base>:nvde`) to the one dictionary an instance builds. The C
  extractor's `init()` and the Rust twin shim both call it, so both file the
  same names, and it refuses a second distance whose names are the first's.
- `adm_rust` evaluates both distances the same way and files the second under
  the same keys.
- The CUDA, SYCL, HIP and Metal twins take the option in one pull request each
  (Q-298); until then their option-table tests record the missing option as a
  gap that fails once it closes.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Port as upstream has it | Smallest diff to upstream | Keeps both gaps: a three-model run fails, debug scores vanish | The gaps are user-visible failures |
| Second dictionary field per instance (upstream) | Mirrors upstream | The Rust shim builds its own dictionary and would need a second ABI callback (`VmafxRsHost` change) | One dictionary with suffixed keys needs no ABI change |
| Merge in the model loader instead of the registry | Knows both models | Misses `--feature` registrations and the Rust selection path | The registry sees every registration |
| Leave the Rust twin unmerged (no callback copy) | No Rust change | Rust runs would score two instances; the twin would not mirror the C extractor (ADR-1713) | Q-298 asks for the Rust mirror |

## Consequences

- **Positive**: a two-distance run computes the transform and decouple once;
  every score stays bit-identical to separate runs (C, Rust, serial and
  threaded); the fork's two-model output equals upstream master's.
- **Negative**: `extend_name_dict` is a fork-only descriptor hook; an upstream
  sync that touches `feature_extractor.h` keeps it.
- **Neutral / follow-ups**: one pull request per GPU backend removes its
  recorded gap.

## References

- Q-298 (maintainer, 2026-10-08): "D4 nvde, cffd5b77d + 33e5f0aca: full scope
  — C + Rust mirror (ADR-1713) AND bit-identical on CUDA, SYCL, HIP and Metal
  now (no DEFAULT_ONLY). Stack it as needed (C+Rust first, then one PR per
  backend is fine), each with == parity tests and exact_twins entries; device
  runs under the locks; Metal evidence is outside hardware — mark pending with
  the tester path if no Mac is available."
- Upstream Netflix/vmaf `33e5f0aca` ("libvmaf/feature: add optional merge
  callback to VmafFeatureExtractor") and `cffd5b77d` ("libvmaf/adm: share
  computation across viewing distances").
- [ADR-1713](1713-rc4-rust-extractor-framework.md) — Rust twins.
- [ADR-2056](2056-float-adm-debug-key-refusal.md) — debug key collisions.
