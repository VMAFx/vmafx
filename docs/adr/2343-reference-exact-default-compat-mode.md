<!-- markdownlint-disable MD013 MD060 -->
# ADR-2343: Reference-exact extractors by default, with a named Netflix compatibility mode

- **Status**: Accepted
- **Date**: 2026-10-07
- **Deciders**: maintainer
- **Tags**: metrics, correctness, release, rc, testing

## Context

[ADR-0024](0024-netflix-golden-preserved.md) fixes Netflix's reference test
outputs as the ground truth of VMAFx's numerical correctness, and the project
has treated "the Netflix extractor's behaviour" as the default ever since. That
is the right ground truth for the `vmaf_v1.0.16` models, whose training inputs
are those exact features. It is not the same thing as agreeing with the original
published implementation of each metric. Another open-source video-metric
library recently found its NIQE, BRISQUE, VIIDEO and Video-BLIINDS ports
diverging from the original MATLAB code in the reference model, antialiased
half-scale resizing, port fidelity and tie-breaking. VMAFx has never measured
its extractors against their originals, so it cannot say where its default
differs from the published metric, and the RC9 retrain would fit new models to
whatever the extractors happen to compute.

Issue [#2286](https://github.com/VMAFx/vmafx/issues/2286) holds the scope. The
maintainer decided where the measurement happens (Q-032) and what the default
becomes (Q-033). The decisions had no ADR, and the roadmap, the RC7 and RC9
epics and the retrain runbook did not mention them.

## Decision

1. **Where.** Every extractor with an original reference implementation is
   proven against it in RC7, as a reference-conformance column of the RC7 CPU
   capability table ([#1885](https://github.com/VMAFx/vmafx/issues/1885)),
   before the RC8 benchmarks and the RC9 retrain.
2. **Fork-added extractors** (NIQE, BRISQUE, PU21, SpEED-QA, FUNQUE+ and the
   like) are fixed to match their originals.
3. **Netflix-inherited extractors that differ from the original** (VIF, ADM,
   SSIM, MS-SSIM, PSNR-HVS and any others the measurement finds): the default
   becomes the reference-exact behaviour. Netflix's behaviour stays available as
   a named **compatibility mode**, an option and a CLI flag generated from the
   option groups of the RC4 definition (work package 8).
4. **The Netflix golden gate runs explicitly in the compatibility mode.** No
   golden assertion changes (agent hard rule 1 and ADR-0024 stand): the gate
   selects the mode its assertions were recorded in.
5. **The v1 models read compatibility-mode features** (their training inputs)
   until the RC9 retrain, which trains on reference-exact features. Scores of
   the shipped `vmaf_v1.0.16*` models are unchanged.
6. **Twins.** The GPU and SIMD twins keep exact parity in both modes; the RC6
   and RC7 tables list the mode with the parity cell.
7. **Harness.** The reference is run by GNU Octave in a pinned container (no
   MATLAB licence) or as the original C / C++ binary, fetched at a pinned hash
   and never vendored unless its licence allows it. Each extractor runs on the
   Netflix pair, both checkerboard pairs, the 4K Big Buck Bunny clip and the
   10-bit sparks; deltas per feature and per frame are recorded. MATLAB
   semantics (`imresize` antialiasing, `round` ties, `double` accumulation,
   edge modes) are reproduced exactly or the difference is explained.
8. **Evidence.** Per extractor, the reference-exact mode equals the original
   within a stated, derived bound (bit-exact where the arithmetic allows); a
   planted divergence, for example a resize without antialiasing, fails the
   harness; the golden gate passes in compatibility mode with unchanged
   assertions; the v1 model scores are unchanged in compatibility mode. The
   result is documented in `docs/metrics/reference-conformance.md`, one row per
   extractor with the original, its version, the delta and the mode.

This ADR supersedes the standing practice that the Netflix extractor's
behaviour is the default. It amends ADR-0024 only in how the gate selects its
mode, not in what it asserts.

Not decided here, and left to the change that implements it: the name of the
compatibility option and flag. The existing `--netflix-compat` CLI flag (legacy
CLI defaults: CPU backend, `%.6f` precision, the `vmaf_v0.6.1` model) is a
separate switch; whether the new mode is spelled through it or beside it is
chosen with work package 8's option groups. Which inherited extractors differ is
a result of the RC7 measurement, not an assumption.

## Alternatives considered

| Option | Pros | Cons | Outcome |
|---|---|---|---|
| Reference-exact default, Netflix behaviour as a named compatibility mode (**chosen**) | The default agrees with the published metrics; the golden gate and the v1 models keep working unchanged through the mode | Two modes to test on every twin; the v1 models need the mode until the retrain | Chosen |
| Netflix behaviour stays the default, reference-exact as a variant | No default change | The default stays unmeasured against the originals; the retrain would train on features that differ from the published metric | Not chosen |
| Document the differences only | Cheapest | The project still computes a metric it cannot name | Not chosen |
| Measure in RC3 or after 1.0 | Earlier or later evidence | RC3 is twin exactness against the CPU, and the retrain needs the answer first (ADR-1341 phase rule) | Not chosen |

## Consequences

- **Positive**: every extractor has a measured answer to "does it match the
  original"; the retrain trains on reference-exact features; the golden gate is
  explicit about the mode it protects.
- **Negative**: both modes need exact twins and test cells in the RC6 and RC7
  tables; the v1 models carry a mode dependency until RC9.
- **Neutral / follow-ups**: `docs/roadmap.md` and `docs/development/release.md`
  list the conformance column in RC7; the retrain runbook records that the RC9
  features are reference-exact; #1885 and #1246 are edited to match. The
  implementation (harness, mode option, per-extractor fixes, the conformance
  page) is [#2286](https://github.com/VMAFx/vmafx/issues/2286) in RC7 and needs
  no further ADR unless the mode name collides with an existing flag.

## References

- `Q-032`: "RC7 CPU table (reference conformance column)"
- `Q-033`: "MATLAB default + a named Netflix compat mode", the golden gate in compatibility mode, v1 models on compatibility features until the RC9 retrain, a superseding ADR
- [ADR-0024](0024-netflix-golden-preserved.md), [ADR-1341](1341-rc-correctness-benchmark-retrain-sequence.md), [ADR-1490](1490-rc3-rc9-candidate-map-cpu-capability.md), ADR-2342
- Issues [#2286](https://github.com/VMAFx/vmafx/issues/2286), [#1885](https://github.com/VMAFx/vmafx/issues/1885), [#1246](https://github.com/VMAFx/vmafx/issues/1246)
