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
| [1.0.0](https://github.com/VMAFx/vmafx/milestone/1) | First release: RC1 correctness and tester reports, RC2 benchmarking and tuning, RC3 real model retraining, then final |
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

The fork has never cut a release; every existing tag is inherited upstream
history. [ADR-1341](adr/1341-rc-correctness-benchmark-retrain-sequence.md)
gives each first-release candidate one responsibility:

| Stage | In scope | Exit boundary |
| --- | --- | --- |
| **RC1 — correctness and tester readiness** | Release-blocking correctness, reliability, security, build, packaging, backend usability, and a portable report path for outside hardware | The exact candidate head is green; no confirmed RC1 blocker or untriaged `docs/state.md` row remains; a tester can return artifact/environment identity, device and tool versions, backend availability, correctness/parity results, commands, logs, and failures |
| **RC2 — benchmark and tune** | Comparable benchmark baselines, profiling, hardware-generation retuning, and measured performance fixes | Results identify the exact artifact, fixtures, host, drivers and runtimes; accepted wins are re-measured and preserve correctness/parity |
| **RC3 — real retraining** | The locked one-shot model retraining programme on the clean, tuned tree | Model-quality gates, model cards, registry/signing metadata, and unchanged Netflix golden assertions pass |
| **Final `v1.0.0`** | Accepted RC3 output plus any required repair candidate | Publication preflight passes on the immutable final tag |

“Done fixing” is deliberately bounded rather than a promise that no future bug
will be found. RC1 is ready when there are no confirmed, actionable RC1
blockers and no untriaged rows. Performance-only findings belong to RC2, real
training belongs to RC3, and externally blocked work stays explicitly deferred
with its trigger and evidence.

If RC2 or RC3 exposes a correctness regression, fix it and rerun the affected
stage evidence before proceeding. Do not pull general benchmarking into RC1 or
real training before RC3. RC1's report envelope may run a short correctness and
device-engagement smoke; it does not make a performance claim.

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
