# eBPF FUSE bypass for vmafx-node rclone mounts

vmafx-node fetches video clips via an rclone HTTP-serve FUSE mount.  For clips
that are already cached locally by rclone, reads that go through the FUSE daemon
add an unnecessary round-trip that inflates p50 latency by ~37× compared to
direct host-file reads.  The eBPF bypass removes that round-trip for warm-cache
reads.

## How it works

The bypass is a probe-only design: an eBPF tracepoint program (`rclone_bypass.bpf.c`)
intercepts `openat`/`close` syscalls and records, in a BPF hash map, every file
descriptor opened under the configured mount prefix (`/rclone-mount/` by default).
The Go-side loader (`cmd/vmafx-node/bpf/bypass_loader.go`) reads that map and
keeps an in-process cache of bypass-eligible FDs.  The actual read bypass
is then implemented in the application: instead of `read(2)` on the FUSE-backed
FD, the node opens the corresponding backing cache file directly.

The eBPF program does **not** modify any kernel memory or intercept data paths —
it is purely observational.

## Enabling the bypass

Set the environment variable before starting vmafx-node:

```bash
export VMAFX_EBPF_BYPASS=1
```

The feature is **off by default**.  It requires:

- Linux kernel 5.15 or newer (BPF CO-RE, ring buffer, `bpf_d_path`).
- `CAP_BPF` (Linux 5.8+) or `CAP_SYS_ADMIN`.
- `/sys/kernel/btf/vmlinux` mounted in the container.

In Kubernetes, add to the pod security context:

```yaml
securityContext:
  capabilities:
    add: ["CAP_BPF"]
  # or for older kernels / broader compatibility:
  privileged: true
env:
  - name: VMAFX_EBPF_BYPASS
    value: "1"
```

## Custom mount prefix

By default the loader watches `/rclone-mount/`.  Override with:

```bash
export VMAFX_EBPF_MOUNT_PREFIX=/my-custom-mount/
```

(The Go `bpf.New()` constructor accepts an explicit prefix as well.)

## Smoke test / benchmark

Run the latency comparison (requires a live rclone mount and `CAP_BPF`):

```bash
VMAFX_EBPF_BYPASS=1 \
VMAFX_EBPF_SMOKE_RCLONE_MOUNT=/rclone-mount \
go test -v -run TestReadLatencyComparison -timeout 120s \
  ./cmd/vmafx-node/bpf/
```

Expected output:

```text
baseline p50 read latency (no bypass): 370ms
bypass p50 read latency:               10ms
speedup ratio (baseline/bypass):       37.0x
```

## Build / regenerating BPF objects

The BPF object is compiled from `cmd/vmafx-node/bpf/rclone_bypass.bpf.c` using
`clang` + `bpf2go`.  A compile-time stub (`rclone_bypass_stub.go`) lets the
package build in CI without the BPF toolchain.

To regenerate after modifying the BPF C source:

```bash
# Install prerequisites (Debian/Ubuntu)
apt-get install -y clang libbpf-dev linux-headers-$(uname -r)

# Regenerate
go generate ./cmd/vmafx-node/bpf/
```

Commit both the updated `.c` file and the newly generated `rcloneBypass_bpf*.go`
/ `rcloneBypass_bpf*.o` files.

## Risks and mitigations

| Risk | Mitigation |
| --- | --- |
| Kernel version < 5.15 | `Start()` returns a clear error; bypass is skipped gracefully |
| Missing `CAP_BPF` | Same — error logged, node continues without bypass |
| BPF verifier rejects program after kernel upgrade | `VMAFX_EBPF_BYPASS` off by default; upgrade testing required before enabling |
| Mount prefix misconfiguration | All reads fall through to FUSE as before; no data corruption possible |
| Container security policy blocks BPF | Documented above; requires explicit opt-in in Helm values |

## See also

- [ADR-0779](../adr/0779-ebpf-fuse-bypass.md) — design rationale and alternatives.
- [ADR-0709](../adr/0709-vmafx-phase4b-distributed-platform.md) —
  Phase 4b platform overview.
- [ADR-0713](../adr/0713-vmafx-node-impl.md) — vmafx-node worker binary.
- Research-0733 — rclone FUSE overhead profiling (37× p50 measurement).
