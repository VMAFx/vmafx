<!-- markdownlint-disable MD013 MD060 -->
# Phase 4b Status Digest — 2026-05-29

ADR-0709 defines nine implementation phases for the VMAFX distributed platform.
This digest records which phases are complete (code on `origin/master`) and which
remain open, based on a cross-reference of PR merge records on VMAFx/vmafx and the
current state of `origin/master`.

## Status table

| Phase | Description | ADR | PR | Status |
|---|---|---|---|---|
| 4b.1 — controller | vmafx-controller: job queue, node registry, scheduler | ADR-0711 | #27 MERGED 2026-05-28 | **COMPLETE** |
| 4b.2 — vmafx-node | Go worker binary (cgo libvmaf + ONNX inference) | ADR-0709 §2 | #32 MERGED 2026-05-28 | **COMPLETE** |
| 4b.3 — operator skeleton | kubebuilder + CRDs (VmafxJob, VmafxNode, VmafxModelTraining) | ADR-0714 | #33 MERGED 2026-05-28 | **COMPLETE** |
| 4b.4 — ffmpeg-bundled node | ffmpeg n8.2 pinned + ffmpeg-patches in Dockerfile | ADR-0709 §5 | #36 MERGED 2026-05-28 | **COMPLETE** |
| 4b.5 — rclone integration | rclone mount in node; stream from S3/GCS without local disk copy | ADR-0709 §6 | #39 MERGED 2026-05-28 | **COMPLETE** |
| 4b.6 — server gRPC | vmafx-server in Go (gRPC + HTTP, observability) | ADR-0703 | #15 MERGED 2026-05-28 | **COMPLETE** |
| 4b.7 — CI slimdown | 1 build per OS + state-of-the-art sanitizers | ADR-0710 | #23 MERGED 2026-05-28 | **COMPLETE** |
| 4b.8 — C ABI break | libvmaf.so.3 → .so.4 scoping ADR | ADR-0767 | #109 OPEN (scoping ADR only; impl pending) | **PARTIAL** — scoping ADR open; implementation not started |
| 4b.9 — native build sunset | Drop .deb/.rpm/.so publication; Docker + Helm only | ADR-0728 | #52 MERGED 2026-05-28 | **COMPLETE** |

## Research digests

| Item | Digest | PR | Status |
|---|---|---|---|
| eBPF optimization target (ADR-0709 §7) | `docs/research/0733-vmafx-ebpf-optimization-target.md` | #29 MERGED 2026-05-28 | **COMPLETE** — target identified (rclone FUSE read-path) |
| eBPF FUSE bypass implementation | `feat(node): eBPF FUSE bypass for rclone (ADR-0779)` | #137 OPEN | **IN REVIEW** |
| Sidecar training architecture (ADR-0709 §9) | `docs/research/0733-vmafx-sidecar-training-architecture.md` | #30 MERGED 2026-05-28 | **COMPLETE** — architecture defined; v1 impl deferred to phase 4c |

## Summary

Seven of nine implementation phases are complete:
4b.1 (controller), 4b.2 (node), 4b.3 (operator), 4b.4 (ffmpeg-bundled), 4b.5 (rclone),
4b.6 (gRPC server), 4b.7 (CI slimdown), and 4b.9 (native build sunset).

Two items remain open:

- **4b.8 (C ABI break)**: PR #109 carries the scoping ADR (ADR-0767, libvmaf.so.3 → .so.4).
  The implementation — rewriting the public C API surface to C++23/Rust/Go and updating
  `ffmpeg-patches/` in the same PR — has not started.

- **eBPF FUSE bypass implementation** (ADR-0779): PR #137 is open in review; the
  research digest (PR #29) and target selection are complete.

## Notes on verification method

`origin/master` (VMAFx/vmafx) is a shallow clone (depth 50). Phase 4b PRs (#21–#39)
are not directly visible in the shallow history. Merge status was verified via
`gh pr view` on VMAFx/vmafx, cross-referenced with commit SHAs in local worktree
branches. The `fix(pr-39)` follow-up commit (9d3f3c8c13) and the `feat!: sunset
legacy native build modes` commit (bfd4c436bb, PR #52) are both present in the shallow
`origin/master`, confirming the 4b PRs did land.
