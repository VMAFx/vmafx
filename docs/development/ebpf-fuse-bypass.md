# eBPF descriptor tracker for vmafx-node rclone mounts

`vmafx-node` can run an eBPF program that records every file descriptor
opened under its rclone mount directory. It is off by default; set
`VMAFX_EBPF_BYPASS=1` to start it. A node that asks for it and cannot run
it does not start. This page shows how to switch it on, what the host must
provide, and what the tracker does and does not do
([ADR-1539](../adr/1539-node-ebpf-tracker-wiring.md), design in
[ADR-0779](../adr/0779-ebpf-fuse-bypass.md)).

!!! warning "Tracking only: no read is bypassed"
    The tracker observes which descriptors the scoring process opens under the
    mount. ADR-0779 meant those descriptors to be read from rclone's local cache
    file instead of through FUSE, but no code does that, and none can in the
    current design: the vmaf CLI, a separate process, reads the mounted files
    itself, and the node mounts with `--vfs-cache-mode off`, so no cache file
    exists. Reads therefore go through FUSE as before. The earlier latency
    figure (37 times faster) described that unbuilt read path; nothing in the
    tree measures or reproduces it.

## Switch it on

The tracker watches the directory the storage layer mounts under, so both
must agree:

```bash
export VMAFX_STORAGE_MODE=mount                       # or auto on a host with FUSE
export VMAFX_STORAGE_MOUNT_ROOT=/rclone-mount/jobs    # per-job mount points go here
export VMAFX_EBPF_MOUNT_PREFIX=/rclone-mount          # contains the mount root
export VMAFX_EBPF_BYPASS=1
./vmafx-node
# INFO ebpf descriptor tracker running mount_prefix=/rclone-mount/
```

`VMAFX_EBPF_BYPASS` and `VMAFX_EBPF_MOUNT_PREFIX` are in the
[node's environment table](../server/node.md#configuration-12-factor-env-vars).
`1` or `true` starts the tracker; `0`, `false` or unset leaves it off, and any
other value stops the node. The prefix (default `/rclone-mount/`) is an
absolute path of at most 255 bytes.

## What stops the node

With `VMAFX_EBPF_BYPASS=1` the node refuses to start, and logs the reason,
when:

- the storage layer does not mount (`VMAFX_STORAGE_MODE=http-serve`, or `auto`
  on a host without FUSE): the tracker would observe nothing;
- `VMAFX_STORAGE_MOUNT_ROOT` (default: the temp directory) does not lie under
  `VMAFX_EBPF_MOUNT_PREFIX`;
- the kernel is older than 5.15 (ring buffer, kernel BTF, bounded loops);
- `/sys/kernel/btf/vmlinux` is missing (mount `/sys/kernel/btf` into the
  container);
- the syscall tracepoints are not visible in tracefs (`/sys/kernel/tracing`
  or `/sys/kernel/debug/tracing`), where the loader looks up their ids;
- the process lacks `CAP_BPF` and `CAP_PERFMON`, or `CAP_SYS_ADMIN`;
- the mount prefix is relative or longer than 255 bytes (it used to be cut
  short silently);
- the kernel refuses the program or a tracepoint attach.

Every reason that applies is listed. An unprivileged process on a 7.2 kernel,
for example, stops with:

```text
VMAFX_EBPF_BYPASS is set but the eBPF tracker cannot start: ebpf: syscall
tracepoints not visible in tracefs (mount /sys/kernel/tracing): stat
/sys/kernel/tracing/events/syscalls/sys_enter_openat/id: permission denied
stat /sys/kernel/debug/tracing/events/syscalls/sys_enter_openat/id: permission denied
ebpf: process lacks CAP_BPF and CAP_PERFMON (or CAP_SYS_ADMIN); CapEff=0x0
(Kubernetes: securityContext.capabilities.add [BPF, PERFMON])
```

On hosts other than Linux, `VMAFX_EBPF_BYPASS=1` always stops the node.

## Kubernetes

`node.ebpf` in the Helm chart turns the tracker on
([ADR-1593](../adr/1593-helm-node-fuse-and-ebpf.md)). It needs `mount`
storage mode and FUSE in the pod (`node.fuse`, see
[the node guide](../server/node.md#kubernetes-deployment)):

```yaml
storage:
  mode: mount
node:
  enabled: true
  fuse:
    enabled: true
    resourceName: devic.es/fuse
  ebpf:
    enabled: true
    mountPrefix: /rclone-mount/    # the default
```

The chart then:

- sets `VMAFX_EBPF_BYPASS=1` and `VMAFX_EBPF_MOUNT_PREFIX`, and
  `VMAFX_STORAGE_MOUNT_ROOT` to the prefix (on an `emptyDir`) unless
  `storage.mountRoot` names a directory under it;
- runs the container as UID 0 with its capabilities dropped to `BPF`,
  `PERFMON` and `SYS_ADMIN` (`SYS_ADMIN` for the FUSE mounts). A non-root
  container process has no effective capabilities, whatever the pod adds;
- mounts the host's `/sys/kernel/tracing` read-only, where the loader finds
  the tracepoint ids. Kernel BTF is visible through the container's own
  `/sys` and needs no mount.

It refuses `node.ebpf` without `storage.mode: mount` and `node.fuse`, a
relative prefix or one longer than 255 bytes, a `storage.mountRoot` outside
the prefix, and `env.VMAFX_EBPF_*`. The pod no longer meets the Pod Security
`baseline` profile (root, these capabilities, a `hostPath` volume); its
namespace must allow `privileged`.

The same settings outside Kubernetes, which load and attach the program on a
7.2 kernel under Docker's default seccomp profile:

```bash
docker run --user 0 --cap-drop ALL --cap-add BPF --cap-add PERFMON --cap-add SYS_ADMIN \
  --device /dev/fuse -v /sys/kernel/tracing:/sys/kernel/tracing:ro \
  -e VMAFX_STORAGE_MODE=mount -e VMAFX_STORAGE_MOUNT_ROOT=/rclone-mount \
  -e VMAFX_EBPF_BYPASS=1 --tmpfs /rclone-mount ghcr.io/vmafx/vmafx-node:<tag>
```

Without the tracefs mount the node stops with `syscall tracepoints not
visible in tracefs`; as UID 65532 it stops with `process lacks CAP_BPF and
CAP_PERFMON`.

## How it works

1. `rclone_bypass.bpf.c` attaches to the `sys_enter_openat`,
   `sys_exit_openat` and `sys_enter_close` tracepoints. An `openat` of a path
   that starts with the prefix records the returned descriptor in the
   `bypass_fds` map and sends an event on the `events` ring buffer; a `close`
   removes it.
2. The loader (`cmd/vmafx-node/bpf/bypass_loader.go`) keeps the reported
   descriptors in an in-process cache (`IsBypassFD`, `TrackedFDs`). The cache
   holds at most 4096 entries: when full it drops the descriptors the kernel
   side has closed.
3. At shutdown the node logs how many descriptors the cache holds and detaches
   the program.

The program only reads; it changes no kernel state other than its own maps.

## Build and regenerate the BPF object

The compiled object (`rclonebypass_bpfel.o`, for amd64, arm64 and the other
little-endian Go targets) is generated at build time and is not committed
([ADR-1622](../adr/1622-bpf-object-generated-at-build-time.md)):
building the node with the tracker needs clang with the BPF target, `llvm-strip`
and the libbpf headers. Its Go binding (`rclonebypass_bpfel.go`) is committed.
`vmlinux.h` next to the program is a minimal header with only the types it
uses. Generate the object with:

```bash
make node-bpf        # scripts/dev/gen-node-bpf.sh
```

A node built without it stops at `VMAFX_EBPF_BYPASS=1` and names this command.
Without clang the command stops and names it; there is no pre-built object to
download. The pinned compiler,
the install commands and the digest check are in the
[node eBPF build guide](node-ebpf-build.md).

On a big-endian architecture the node refuses `VMAFX_EBPF_BYPASS`.
`TestEmbeddedObjectMatchesMirrors` checks the object's programs, maps and
struct sizes against the Go side without kernel privileges;
`TestEmbeddedObjectMatchesPinnedDigest` checks that the pinned clang's object
has the recorded sha256.

## Licence

`rclone_bypass.bpf.c` is EUPL-1.2 like the rest of the node. The program
declares `"GPL"` to the kernel (its `SEC("license")` string): the kernel
links it against GPL-2.0 code at load time, and it calls helpers the kernel
offers only to GPL-compatible programs (`bpf_probe_read_user_str`,
`bpf_probe_read_kernel`). EUPL-1.2's compatibility clause (Article 5, with
the GPL v. 2 and v. 3 in its Appendix) allows that combination to be
distributed under the GPL. [ADR-1559](../adr/1559-ebpf-kernel-licence-string.md)
records the reasoning; `TestEmbeddedObjectLicence` fails when the object
declares anything else. A string the kernel does not count as GPL-compatible
stops the program from loading:

```text
load program: invalid argument: cannot call GPL-restricted function from non-GPL compatible program
```

## Verification

`TestEmbeddedObjectLoadsIntoKernel` loads the programs and attaches the
tracepoints on a host that passes `Preflight`, and skips elsewhere with the
reason. It passed on kernel 7.2.8 in a privileged container
(`docker run --privileged -v /sys/kernel/tracing:/sys/kernel/tracing`) with
the test binary from `CGO_ENABLED=0 go test -c ./cmd/vmafx-node/bpf/`. A run
of the tracker inside a node pod on a Kubernetes cluster has not been made.

## See also

- [ADR-1539](../adr/1539-node-ebpf-tracker-wiring.md): wiring, fail-closed
  checks, why no read path uses the tracker.
- [ADR-1559](../adr/1559-ebpf-kernel-licence-string.md): the licence string
  the program declares to the kernel.
- [ADR-0779](../adr/0779-ebpf-fuse-bypass.md),
  [ADR-0996](../adr/0996-ebpf-fuse-bypass-rclone.md): the original design.
- [ADR-1526](../adr/1526-node-storage-streamed-inputs.md): the storage modes.
