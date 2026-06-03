# ADR-0779: eBPF FUSE read-path bypass for vmafx-node rclone mounts

- **Status**: Proposed
- **Date**: 2026-05-29
- **Deciders**: lusoris
- **Tags**: `ebpf`, `node`, `rclone`, `performance`, `phase4b`, `fork-local`

## Context

vmafx-node fetches video clips via rclone HTTP-serve, exposed as a FUSE mount
under `/rclone-mount/`.  Profiling (Research-0733) showed that small sequential
reads (4–64 KiB) through the FUSE daemon add a round-trip through a kernel
upcall mechanism: each `read(2)` on a FUSE-backed file wakes the rclone FUSE
daemon, which in turn issues its own HTTP range request.  For clips that have
already been cached locally by rclone, this daemon hop is pure overhead.

Measured baseline on a 100 MiB sequential read from a warm rclone HTTP-serve
cache: ~370 ms p50.  Target after bypass: ~10 ms p50 (37× improvement,
Research-0733 headline figure).

The bypass is intentionally probe-only: the eBPF program marks file descriptors
opened under the configured mount prefix in a shared BPF hash map.  The Go-side
loader reads that map; the actual "bypass" is the application opening the
backing cache file directly via `pread(2)` instead of going through FUSE.  No
kernel memory is modified by the eBPF program.

The feature is gated behind `VMAFX_EBPF_BYPASS=1` (default off) so it can be
validated incrementally without affecting production deployments that have not
yet confirmed CAP_BPF availability.

## Decision

We will ship an eBPF tracepoint-based FD tracker (`cmd/vmafx-node/bpf/`) that:

1. Intercepts `sys_enter_openat` / `sys_exit_openat` for paths under the
   configured mount prefix and records the resulting fd in a BPF hash map.
2. Intercepts `sys_enter_close` to evict the fd from the map on close.
3. Emits ring-buffer events so the Go loader's in-process cache stays warm
   without polling.
4. Is compiled from `rclone_bypass.bpf.c` via `bpf2go`; a compile-time stub
   (`rclone_bypass_stub.go`) makes the package build in CI without a BPF
   toolchain.
5. Is enabled only when `VMAFX_EBPF_BYPASS=1` at process start.

The loader uses `github.com/cilium/ebpf` (v0.21.0) as the Go-side BPF library.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| FUSE passthrough mode (kernel 5.15 `FUSE_PASSTHROUGH`) | Zero eBPF complexity; kernel-native | Requires rclone to opt in; rclone upstream does not yet expose `FUSE_PASSTHROUGH` in stable builds | Not available with current rclone |
| LD_PRELOAD shim intercepting `open`/`read` | No kernel privileges needed | Fragile with static binaries and CGo; does not compose well with Go runtime | Too brittle |
| io_uring fixed-file registration | Low syscall overhead for bulk reads | Requires rework of all read paths; no FUSE-specific benefit without the FD tracking layer anyway | Larger scope; orthogonal to the FUSE problem |
| rclone VFS cache + direct path access | Simple; no eBPF | Only works when the clip is 100% cached; partial-cache case still funnels through FUSE | Cannot guarantee full pre-cache in the general case |

## Consequences

- **Positive**: p50 read latency for warm-cache clips drops from ~370 ms to
  ~10 ms (37×) when bypass is active.  No FUSE daemon involvement for reads
  after the initial open.
- **Negative**:
  - Requires Linux 5.15+ and CAP_BPF (or CAP_SYS_ADMIN).
  - Pod security context must allow `privileged: true` or a fine-grained
    seccomp/AppArmor profile that permits BPF syscalls and `tracefs` access.
  - The compiled `.o` object must be regenerated when the BPF C source changes
    (`go generate ./cmd/vmafx-node/bpf/`); the stub enables CI to pass without
    the BPF toolchain, but production images must include the real object.
  - `VMAFX_EBPF_BYPASS=1` is off by default; operators must explicitly opt in
    and confirm their kernel version and capability grants.
- **Neutral / follow-ups**:
  - Helm chart values (`deploy/helm/vmafx-node/values.yaml`) need a
    `ebpfBypass.enabled` flag that sets the env var and adds
    `CAP_BPF` to the container security context.
  - A container build step
    (`RUN apt-get install -y clang libbpf-dev linux-headers-$(uname -r)`)
    must be added to the node Dockerfile when `VMAFX_EBPF_BYPASS` is
    promoted from experimental to default.
  - The smoke test (`TestReadLatencyComparison`) should be wired into a manual
    perf-gate job in CI once a privileged runner is available.

## References

- Research-0733 (37× p50 latency measurement, rclone FUSE overhead profiling).
- [ADR-0709](0709-vmafx-phase4b-distributed-platform.md) — Phase 4b distributed
  platform; eBPF listed as a component.
- [ADR-0713](0713-vmafx-node-impl.md) — vmafx-node worker binary design.
- `github.com/cilium/ebpf` v0.21.0 — Go BPF library.
- Linux kernel docs: `Documentation/filesystems/fuse.rst`,
  `Documentation/bpf/`.
- req: "Implement the eBPF FUSE bypass per Research-0733 (37× p50 latency win)."
