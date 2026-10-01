<!-- markdownlint-disable MD013 MD060 -->
# ADR-1421: Map the first-release candidates RC3 to RC8

- **Status**: Accepted
- **Date**: 2026-10-01
- **Deciders**: lusoris
- **Tags**: release, rc, process

## Context

[ADR-1341](1341-rc-correctness-benchmark-retrain-sequence.md) gave each
first-release candidate one responsibility, and
[ADR-1352](1352-rc-phase-shift-plus-one.md) mapped them to tags: `v1.0.0-rc.1`
(RC1) correctness and the outside-tester report path, `v1.0.0-rc.2` (RC2)
stabilisation and repair, `v1.0.0-rc.3` (RC3) benchmarks, profiling and tuning,
and `v1.0.0-rc.4` (RC4) the one-shot real retrain.

Since rc.1 the cleaned-up tree keeps surfacing correctness and performance
defects with little effort. The RC3 work session that preceded this decision
merged about 70 pull requests, most of them twin-exactness fixes: GPU and SIMD
twins that returned scores differing from the CPU extractor in their last
digits or more, and SYCL kernels that return wrong values on Intel GPUs under
the xe driver when they use scratch memory ([ADR-1395](1395-sycl-kernels-no-scratch.md)).
Benchmarks and a retrain of roughly 125 to 130 hours would be invalidated by
that stream of corrections and by the restructuring still planned (a first full
Rust metric, deduplication of the twins, a capability-driven dispatch), because
every one of them changes either the scores or the timings.

The maintainer therefore asked for more candidates that only serve correctness
and performance preparation before the retrain, and for the first full Rust
metric to move ahead of benchmarking, written against the settled reference.

Toolchains also publish their supported targets in machine-readable form. On the
development host on 2026-10-01: `nvcc` 13.4 lists `compute_75` to `compute_121`;
`ocloc` lists the Intel platforms from `tgl` through `dg2` to `bmg`, and
`ocloc query` returns common traits; the ROCm `llc -mcpu=help` lists the `gfx`
targets with their features. Kernels can therefore be compiled and statically
audited for hardware nobody on the project owns. Timing and driver behaviour
still need devices, which is RC7 and the outside-tester path.

## Decision

We will replace ADR-1352's candidate-to-tag mapping with the following map. This
ADR supersedes only that mapping; the rest of ADR-1341 stays.

| Candidate | Owns | Epic |
| --- | --- | --- |
| `v1.0.0-rc.1` (RC1) | Correctness and the outside-tester report path (published, unchanged). | - |
| `v1.0.0-rc.2` (RC2) | Stabilisation and repair (unchanged). | - |
| `v1.0.0-rc.3` (RC3) | Twin exactness: every GPU and SIMD twin returns the CPU extractor's scores bit for bit, or carries a measured tolerance recorded in an ADR; no SYCL kernel uses scratch memory. | [#1721](https://github.com/VMAFx/vmafx/issues/1721) |
| `v1.0.0-rc.4` (RC4) | The first full Rust metric: the whole `vmaf_v1.0.16_3d0h` path (cambi, speed_chroma, integer adm3, integer motion3, model prediction) in Rust, bit-identical to the C implementation, with the C ABI unchanged. GPU twins stay CUDA, SYCL and HIP code. Builds on the pilot of [ADR-0707](0707-vmafx-rust-pilot-feature.md). | [#1723](https://github.com/VMAFx/vmafx/issues/1723) |
| `v1.0.0-rc.5` (RC5) | Deduplication: one implementation per behaviour across GPU twins and host code, including the Rust code. `libgpudispatch` is extracted ([#1455](https://github.com/VMAFx/vmafx/issues/1455)). | [#1724](https://github.com/VMAFx/vmafx/issues/1724) |
| `v1.0.0-rc.6` (RC6) | GPU capability source of truth: a per-vendor table generated from `nvcc --list-gpu-arch`, `ocloc` and the ROCm `llc -mcpu=help`, checked in, with a CI drift check. Dispatch and kernel parameters read it, with a generic fallback for unknown devices. Every kernel is compiled and statically audited for every target (scratch memory, spills, register ceiling, fp64). | [#1725](https://github.com/VMAFx/vmafx/issues/1725) |
| `v1.0.0-rc.7` (RC7) | Benchmarks, profiling and tuning (ADR-1341's benchmark evidence, pinned to the rc.7 artifact). | [#1245](https://github.com/VMAFx/vmafx/issues/1245) |
| `v1.0.0-rc.8` (RC8) | The one-shot real retrain and the remaining tiny-AI training (ADR-1341's retrain evidence, pinned to the rc.8 artifact). | [#1246](https://github.com/VMAFx/vmafx/issues/1246), [#1242](https://github.com/VMAFx/vmafx/issues/1242) |

Final `v1.0.0` follows accepted RC8 evidence and any required repair candidate.

Ordering rationale. Exactness comes first so that all later work has a settled
reference. The Rust path follows directly, written against exact behaviour, and
precedes deduplication so that the deduplication pass covers the Rust code. The
capability table follows deduplication so that it parameterises one
implementation instead of four. Benchmarks come last before training.

Rules carried over or added:

- Everything else in ADR-1341 stays: the phase order, exact-head evidence,
  revalidation after later merges, immutable tags, and the rule that no work
  leaks into an earlier candidate. Renovate and version-update pull requests are
  not frozen in any candidate; the Renovate updates pending on 2026-10-01 land
  inside RC3.
- Benchmarking and tuning stay out of RC3 to RC6. Training never starts before
  RC7 evidence is accepted.
- Speed lost to exactness in RC3 is recorded as a tuning row and recovered in
  RC7. It is never traded back for a tolerance.
- A candidate that finds a correctness regression fixes it and reruns the
  affected evidence before the next candidate proceeds.

Identifiers that keep their names, for the reasons in ADR-1352: `tools/rc1-tester/`
(it serves every correctness candidate) and the backlog IDs `T-RC2-BENCH-TUNE`
and `T-RC3-MODEL-RETRAIN` (stable identities under
[ADR-1303](1303-backlog-checklist-tracker-schema.md)). Their names no longer
match the candidate that owns the work (benchmarks are RC7, the retrain RC8).
The `RC3` and `RC4` phase labels in the `tools/rc1-tester` catalog are updated
to `RC7` and `RC8`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Keep ADR-1352's map (RC3 benchmarks, RC4 retrain) | No document churn; fewest candidates before the final release. | Benchmarks and the retrain run on a tree still receiving exactness fixes and restructuring; their results would be invalidated. | The evidence would not survive the work already known to be pending. |
| Two added candidates, with deduplication and capability sharing one | One fewer candidate. | Deduplication would run against a table not yet written, or the table against code not yet deduplicated; the two passes have different exit evidence. | The capability table should parameterise one implementation, which needs deduplication to finish first. |
| One added candidate (exactness only) | Smallest change to ADR-1352. | Leaves deduplication, the Rust path and capability work with no candidate, so they would leak into benchmark or training candidates. | Violates the rule that no work leaks across phases. |
| Rust after capability (deduplication before Rust) | Deduplicates the C and GPU code first. | The Rust code would be written after deduplication and escape its pass, and the capability table would be built before the Rust path settles. | Chosen order lets the deduplication pass cover the Rust code. |
| Rust before deduplication (**chosen**) | Rust is written against exact behaviour; deduplication covers it. | Rust work starts before the code is deduplicated, so the Rust port reproduces behaviour that still exists in several C and GPU copies. | Accepted; the deduplication pass then covers the Rust code too. |
| Rust scope limited to `speed_chroma` | Small, quick to finish. | Not a usable metric by itself, and it leaves a second Rust phase with no candidate. | Too small to prove a whole metric against C bit for bit. |
| Rust scope: the whole `vmaf_v1.0.16_3d0h` path (**chosen**) | A complete metric whose result is checkable against C bit for bit. | Largest scope of the options. | Chosen: it is the smallest scope that yields a whole metric. |
| Rust scope: the whole path including GPU twins in Rust | One language everywhere. | GPU twins in Rust depend on toolchains and device access that the project cannot verify for every vendor. | GPU twins stay CUDA, SYCL and HIP code. |
| Capability work as a research digest only | Cheap; no new tooling. | It goes stale; dispatch and kernel parameters would still be hand-set per vendor. | Not a source of truth anything can read. |
| Capability as a generated table plus an all-target audit (**chosen**) | Always current by construction; dispatch reads it; uncovered hardware is still audited. | Needs generators, a drift check and a per-target compile matrix. | Chosen: updating becomes a scripted regeneration. |
| Deduplication inside the repository only | Smaller scope. | The dispatch logic stays tied to this repository. | `libgpudispatch` is extracted as well ([#1455](https://github.com/VMAFx/vmafx/issues/1455)). |
| Deduplication plus extraction of `libgpudispatch` (**chosen**) | One dispatch implementation reusable outside the tree. | Adds an extracted component to maintain. | Chosen by the maintainer. |

## Consequences

- **Positive**: Each candidate has one checkable exit bar. Benchmark and training
  evidence is taken on a tree that is no longer being corrected or
  restructured. Capability tables and the all-target audit cover hardware nobody
  owns.
- **Negative**: The final release is four candidates further away than under
  ADR-1352. Historical records keep the older names and must be read with the
  map in force on their date: ADR-1341's body, ADR-1352's body, ADR-1342,
  ADR-1346, ADR-1348, earlier `docs/rebase-notes.md` entries, `CHANGELOG.md`,
  closed ledger rows, and `docs/state.md` update notes dated before 2026-10-01.
  The disposition labels in `docs/state.md` are renamed later, not in this change.
- **Neutral / follow-ups**: The GitHub milestone text and the epics follow this
  map (#1245 is RC7, #1246 and #1242 are RC8, and #1721, #1723, #1724 and #1725
  are new); GitHub state is outside this change. Until the ledger relabel sweep
  (`T-STATE-LEDGER-RC-RELABEL-2026-10-01`), the existing disposition label
  "RC3 performance and backend acceleration" reads as RC3 for rows about wrong or
  inexact scores, and as RC7 for rows about throughput or host residuals with
  correct scores, with RC5 for duplication and RC6 for per-platform parameters;
  "RC4 training and model validation" reads as RC8. The same change updates the
  release guide, roadmap, retrain runbook, tester guide, model card,
  dependency-bot policy, ledger classification, `AGENTS.md` section 11 with its
  compiled projections, and the `tools/rc1-tester` inventory. This decision adds
  no dependency, build-time fetch, runtime surface or SBOM component.

## References

- `req` (verbatim): "i think we need to add 1-3 more rc's in the epics/milestones, now that our fork is more cleaned up we actually start to find the good shit again without much effort, so i think for correctness and performance we need 3 more rc's that are only this before we retrain... its a lot of legs and I think one of them needs to do a deduplication again and perhaps we need to do a proper research by ability of the gpu's, i bet we can do a lot of perfect code without having the card to test becaus cuda is just by a version, so we actually know what every ability adds? (automated and always up to date) but i think with amd and intel that shouldnt be a problem as well..."
- `Q1.1` (phase map, verbatim): "RC3–RC6, retrain RC7 (Recommended)"
- `Q1.2` (capability depth, verbatim): "well option one but if we find good sources, we can just automate the data and have a sot in our repo? that?? and the rest are fallbacks? or cases, dunno? something like that, so that would solve the performance dilemma on sycl as well for sure... and updating is easy then and almost templated/scripted as well i guess?" (option one = "Matrix + compile and audit every target")
- `Q1.3` (dedup scope, verbatim): "option 2 and you need to move another milestone into an additional rc pre tuning/benching/training: the first full rust metric and I assume we should do the new netflix model with it? which is work but that will get people to love vmafx because then our knowledge about vmaf we have in hindsight and in this repo will start make this fly on cpu and gpu..." (option 2 = "Also extract libgpudispatch")
- `Q2.1` (final map, verbatim): "Rust before dedup"
- `Q2.2` (Rust scope, verbatim): "The whole v1.0.16 path in Rust (Recommended)"
- `req` (verbatim, the eleven pending Renovate updates): "bring them all in rc3 if possible!"
- [ADR-1341](1341-rc-correctness-benchmark-retrain-sequence.md) — the sequence this ADR keeps except for the candidate mapping.
- [ADR-1352](1352-rc-phase-shift-plus-one.md) — the mapping this ADR supersedes.
- [ADR-0707](0707-vmafx-rust-pilot-feature.md) — the Rust pilot RC4 builds on.
- [ADR-1395](1395-sycl-kernels-no-scratch.md) — the scratch-memory defect RC3 closes out.
- [ADR-1303](1303-backlog-checklist-tracker-schema.md) — stable backlog IDs.
- Epics [#1721](https://github.com/VMAFx/vmafx/issues/1721), [#1723](https://github.com/VMAFx/vmafx/issues/1723), [#1724](https://github.com/VMAFx/vmafx/issues/1724), [#1725](https://github.com/VMAFx/vmafx/issues/1725), [#1245](https://github.com/VMAFx/vmafx/issues/1245), [#1246](https://github.com/VMAFx/vmafx/issues/1246), [#1242](https://github.com/VMAFx/vmafx/issues/1242) and [#1455](https://github.com/VMAFx/vmafx/issues/1455).
