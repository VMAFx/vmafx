# Roadmap

VMAFx tracks its plan in GitHub, not in a document that drifts. This page is a
map of where that plan lives and how the releases are sequenced.

- **Board** — [VMAFx Roadmap](https://github.com/orgs/VMAFx/projects/1) (public)
- **Milestones** — [all milestones](https://github.com/VMAFx/vmafx/milestones)
- **Epics** — issues labelled [`epic`](https://github.com/VMAFx/vmafx/issues?q=is%3Aissue+is%3Aopen+label%3Aepic),
  each with a child task list

## Releases

| Milestone | Theme |
| --- | --- |
| [1.0.0](https://github.com/VMAFx/vmafx/milestone/1) | First release: RC1 correctness and tester reports, RC2 stabilisation, RC3 twin exactness, RC4 first full Rust metric, RC5 deduplication, RC6 GPU capability table, RC7 benchmarking and tuning, RC8 real model retraining, then final |
| [1.1](https://github.com/VMAFx/vmafx/milestone/2) | New metrics (ΔE-ITP, PU21, NIQE, BRISQUE, Y-FUNQUE+), their GPU twins, and the tools surface |
| [1.2](https://github.com/VMAFx/vmafx/milestone/3) | Cloud-native foundation: server mode, observability, containers and Kubernetes |
| [1.3](https://github.com/VMAFx/vmafx/milestone/4) | Cloud-native scale-out: operator, controller/node, multi-vendor GPU scheduling |
| [2.0](https://github.com/VMAFx/vmafx/milestone/5) | Language modernization — Go tools, Rust pilots, C++23 internals — completing the cloud-native arc |

Two milestones are deliberately **rolling** rather than tied to a release:

- [Models & benchmarks](https://github.com/VMAFx/vmafx/milestone/6) — retraining
  cadence, benchmark baselines, corpus work.
- [Code health & deduplication](https://github.com/VMAFx/vmafx/milestone/7) — the
  fork adds and changes a lot, so slimming it is recurring work, not a one-off.

## How 1.0.0 is gated

The fork's first release candidate, `v1.0.0-rc.1`, was published on
2026-09-27; older tags are inherited upstream history.
[ADR-1341](adr/1341-rc-correctness-benchmark-retrain-sequence.md) gives each
first-release candidate one responsibility, and
[ADR-1421](adr/1421-rc3-rc8-candidate-map.md) maps the stages to tags (it
supersedes the candidate mapping of
[ADR-1352](adr/1352-rc-phase-shift-plus-one.md)), so that each stage number
matches its `v1.0.0-rc.N` tag:

| Stage | In scope | Exit boundary |
| --- | --- | --- |
| **RC1 — correctness and tester readiness** | Release-blocking correctness, reliability, security, build, packaging, backend usability, and a portable report path for outside hardware | The exact candidate head is green; no confirmed RC1 blocker or untriaged `docs/state.md` row remains; a tester can return artifact/environment identity, device and tool versions, backend availability, correctness/parity results, commands, logs, and failures |
| **RC2 — stabilisation and repair** | The dependency updates and correctness fixes merged since rc.1, delivered to testers through the same report path; no benchmark or training work | The RC1 boundary, re-established on the rc.2 head |
| **RC3 — twin exactness** ([#1721](https://github.com/VMAFx/vmafx/issues/1721)) | Every GPU and SIMD twin returns the CPU extractor's scores bit for bit, or carries a measured tolerance recorded in an ADR; no SYCL kernel uses scratch memory | Per-twin parity measured at `--precision max` on the Netflix pairs, the 1080p checkerboard pairs and the 4K fixture; Netflix golden assertions unchanged |
| **RC4 — first full Rust metric** ([#1723](https://github.com/VMAFx/vmafx/issues/1723)) | The whole `vmaf_v1.0.16_3d0h` path (cambi, speed_chroma, integer adm3, integer motion3, model prediction) in Rust; the C ABI is unchanged and the GPU twins stay CUDA, SYCL and HIP code | The Rust path is bit-identical to the C path on the parity fixtures |
| **RC5 — deduplication** ([#1724](https://github.com/VMAFx/vmafx/issues/1724)) | One implementation per behaviour across GPU twins and host code, the Rust code included; `libgpudispatch` extracted ([#1455](https://github.com/VMAFx/vmafx/issues/1455)) | Scores unchanged against the RC3 reference; duplicated code removed rather than moved |
| **RC6 — GPU capability source of truth** ([#1725](https://github.com/VMAFx/vmafx/issues/1725)) | A per-vendor capability table generated from `nvcc --list-gpu-arch`, `ocloc` and ROCm `llc -mcpu=help`, checked in with a CI drift check; dispatch and kernel parameters read it, with a generic fallback for unknown devices; every kernel compiled and statically audited for every target (scratch, spills, register ceiling, fp64) | Drift check green; audit clean for every listed target |
| **RC7 — benchmark and tune** ([#1245](https://github.com/VMAFx/vmafx/issues/1245)) | Comparable benchmark baselines, profiling, hardware-generation retuning, and measured performance fixes, including the speed RC3 gave up for exactness | Results identify the exact artifact, fixtures, host, drivers and runtimes; accepted wins are re-measured and preserve correctness/parity |
| **RC8 — real retraining** ([#1246](https://github.com/VMAFx/vmafx/issues/1246), [#1242](https://github.com/VMAFx/vmafx/issues/1242)) | The locked one-shot model retraining programme on the clean, tuned tree | Model-quality gates, model cards, registry/signing metadata, and unchanged Netflix golden assertions pass |
| **Final `v1.0.0`** | Accepted RC8 output plus any required repair candidate | Publication preflight passes on the immutable final tag |

“Done fixing” is deliberately bounded rather than a promise that no future bug
will be found. RC1 and RC2 are ready when there are no confirmed, actionable
release blockers and no untriaged rows. Performance-only findings belong to
RC7, real training belongs to RC8, and externally blocked work stays explicitly
deferred with its trigger and evidence.

If any later candidate exposes a correctness regression, fix it and rerun the
affected stage evidence before proceeding. Do not pull general benchmarking
into RC1 to RC6, or real training before RC7 evidence is accepted. Speed that
RC3 gives up for exactness is recorded as a tuning row and recovered in RC7; it
is not traded back for a tolerance. The RC1 and RC2 report envelope may run a
short correctness and device-engagement smoke; it does not make a performance
claim.

Ordinary Renovate and other version PRs remain mergeable throughout the
sequence when normal required checks, review, pinning, and component-specific
validation pass. Security updates are prioritised rather than being the only
allowed updates. Because evidence is exact-head, any later merge requires the
affected candidate checks or measurements to be rerun.

## Things that do not change

Some guarantees are load-bearing for downstream users and hold across every
milestone above, including 2.0:

- The **Netflix golden values** are never edited. They are the numerical
  ground truth; if scores drift, the code is wrong.
- The **`libvmaf.so` ABI** and the FFmpeg `libvmaf` filter name stay stable, even
  as the internals move to C++23 and parts of the tooling move to Go and Rust.
- The public **C API** under `core/include/libvmaf/` stays source-compatible.
- Release artifacts are **built in the container**, never from a host build.

## Contributing against the roadmap

Pick an epic, read its task list, and open a PR that closes one line of it. Epics
are intentionally coarse — sub-tasks become their own issues when someone starts
them, so the tracker reflects work in progress rather than a wish list.
