<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-0996: eBPF FUSE bypass for rclone zero-copy path in vmafx-node

- **Status**: Proposed
- **Date**: 2026-06-03
- **Deciders**: Lusoris
- **Tags**: `ci`, `go`, `ebpf`, `rclone`, `performance`, `security`, `supply-chain`

## Context

The vmafx-node Go worker fetches media clips from object storage via an rclone FUSE
mount. Each clip open under the FUSE mount incurs a FUSE kernel round-trip plus an
rclone network fetch even for clips that have been pulled to a local cache since the
previous run. At 1080p clip sizes this round-trip adds 370 ms p50 latency per clip
open (Research-0733). With a 150 k-clip dataset this dominates wall time.

A probe-only eBPF tracepoint program can track file-descriptor opens under the rclone
FUSE mount prefix without the overhead of a full FUSE intercept. By maintaining a BPF
hash map of bypass-eligible FDs and draining events via a ring buffer, the in-process
cache stays warm without polling, collapsing warm-cache clip-open latency to ~10 ms
(37× improvement). Research-0733 measured this on the fork's RTX 4090 dev machine.

The feature is gated behind `VMAFX_EBPF_BYPASS=1` (default off) and requires
Linux 5.15+ and `CAP_BPF`. A compile-time stub (`rclone_bypass_stub.go`) allows CI
builds without a BPF toolchain, preserving cross-platform compatibility.

## Decision

We will ship a probe-only eBPF tracepoint program (`rclone_bypass.bpf.c`) plus a Go
loader using `cilium/ebpf v0.21.0` in `cmd/vmafx-node/bpf/`. The feature is opt-in via
`VMAFX_EBPF_BYPASS=1`. BPF objects are generated artifacts regenerated with
`go generate ./cmd/vmafx-node/bpf/`. A stub auto-selects on non-Linux hosts or
when the BPF toolchain is absent.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Full FUSE intercept (custom FUSE driver) | Complete control over all I/O paths | Requires kernel module; far higher complexity; breaks rclone compatibility | Too invasive; deployment requires root |
| Polling the rclone cache directory | Simple to implement; no kernel deps | High polling overhead; latency floor ~100 ms even at aggressive poll intervals; wastes CPU | Latency too high; CPU cost unacceptable at 150 k-clip scale |
| inotify watch on rclone cache dir | No BPF required; works on older kernels | inotify is per-inode; races between open and add; does not track FD → file mapping reliably | Race condition; inotify events arrive after the open, not before |
| Disable rclone FUSE; use direct S3 SDK | Eliminates FUSE overhead entirely | Requires S3 credentials in every worker pod; breaks the mount-abstraction that lets us swap object stores | Credential sprawl; mount abstraction is a design invariant (ADR-0709) |

## Consequences

- **Positive**: 37× warm-cache clip-open latency improvement (370 ms → 10 ms p50);
  unlocks full 150 k-clip throughput on a single node without pre-staging;
  opt-in design means existing deploys are unaffected.
- **Negative**: Requires Linux 5.15+, `CAP_BPF`, and `clang + bpf2go` for regeneration;
  adds `cilium/ebpf v0.21.0` as a runtime dependency; widens the Linux-only surface.
- **Neutral / follow-ups**: BPF objects must be regenerated when the tracepoint
  struct layout changes (`go generate ./cmd/vmafx-node/bpf/`); this is an
  `AGENTS.md` invariant in `cmd/vmafx-node/bpf/`.

## Supply-chain impact

- **New dependencies**: `cilium/ebpf v0.21.0` (runtime, Apache-2.0,
  `https://github.com/cilium/ebpf`).
- **Build-time fetches**: `go generate` invokes `bpf2go` (installed via Go toolchain);
  pinned by `go.sum`.
- **Sigstore-signable**: Go module hash in `go.sum` provides integrity anchor.
- **CVE surface delta**: adds a BPF loader; BPF programs are kernel-verified and
  run in read-only probe mode — no packet manipulation, no memory writes outside
  the map.

## References

- Research-0733: 37× latency improvement measurement data.
- Open DRAFT PR: #137 (`feat(node): eBPF FUSE bypass for rclone`).
- ADR-0709: Phase 4b distributed platform (rclone mount-abstraction invariant).
- ADR-0719: vmafx-node rclone integration (the surface this optimizes).
