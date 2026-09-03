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
| [1.0.0](https://github.com/VMAFx/vmafx/milestone/1) | First release: correctness, a working release pipeline, tiny-AI complete, and one model retraining pass |
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
history. 1.0.0 is ordered, and the ordering matters more than the contents:

1. The release pipeline has to work at all
2. `master` green and the PR queue drained
3. Whole-tree lint debt down
4. The open bugs in [`docs/state.md`](state.md) closed
5. Backend parity, MCP coverage, modernization and tiny-AI finished
6. Benchmark and tuning pass, so performance work is in the tree **before** training
7. **Model retraining last** — a single pass, once everything else is release-ready

That last point is a deliberate constraint rather than a scheduling accident: the
retraining run is long and expensive, so it happens once, against a tree that is
already final.

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
