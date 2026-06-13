# ADR-0709: VMAFX Phase 4b — Distributed Platform Umbrella ADR

Proposes the architectural pivot from single-binary scoring tool to a distributed
video-quality, encoding, and ML platform. Defines the controller/node/operator
component split, ffmpeg worker integration, rclone zero-copy storage, eBPF
research path, Go ONNX Runtime AI inference in the node, Python sidecar
continuous training, C ABI break with ffmpeg-patches update, and native build
sunset (Docker images + Helm chart as the only release artifacts).

Nine-phase implementation plan (4b.1–4b.9) establishes the dependency order for
per-sweep child ADRs and PRs.

See [ADR-0709](../../docs/adr/0709-vmafx-phase4b-distributed-platform.md) and
[Phase 4b architecture diagram](../../docs/architecture/phase4b-distributed-platform.md).
