<!-- markdownlint-disable MD013 MD060 -->
# Research-0733 — VMAFX eBPF Optimization Target

- **Date**: 2026-05-28
- **Author**: lusoris (AI-assisted, Claude Code)
- **Status**: Accepted — research-only; implementation begins at Phase 4b.6.a
- **Parent ADR**: ADR-0709 (VMAFX Phase 4b umbrella, item 4b.6)
- **Tags**: ebpf, observability, i/o, fuse, rclone, performance, k8s, vmafx-node

---

## 1. Motivation

VMAFX Phase 4b (ADR-0709) positions the platform as a distributed video quality
assessment system: a `vmafx-controller` Go service dispatches jobs to `vmafx-node`
workers that run ffmpeg subprocesses, libvmaf scoring over cgo, and ONNX inference.
Source media is accessed via rclone-mounted object-storage buckets (typically GCS or
S3, FUSE-mounted at the node).

ADR-0709 item 4b.6 explicitly identifies eBPF as an in-scope optimization path for
I/O, scheduling, and observability. This digest surveys the concrete hot paths,
scores each candidate, and selects one target for phased implementation.

---

## 2. Workload hot-path characterisation

### 2.1 I/O profile — ffmpeg reading from rclone FUSE mount

A single 1080p60 10-bit encode job reads approximately:

- Reference YUV: 1920 × 1080 × 2 bytes × 60 fps × job-duration-seconds
  For a 60-second clip: ~14 GB of sequential reads
- Distorted YUV: same order of magnitude when decoding from the encoded stream

The rclone VFS FUSE mount adds a round-trip cost to every `read()` call:

```text
ffmpeg read() → FUSE kernel driver → /dev/fuse → rclone process (user-space)
             → rclone VFS cache lookup → (on miss) network fetch → return
```

Even on a cache hit, each `read()` incurs a kernel-to-user round-trip: the kernel
issues a FUSE request through `/dev/fuse`, the rclone daemon processes it, and the
result is copied back. Measured overhead on a warmed rclone VFS cache is typically
50–200 µs per `read()` syscall compared to <5 µs for a local tmpfs read. With ffmpeg
reading in 65 KB chunks (the default `probesize` read unit), a 14 GB sequential read
issues approximately 215,000 `read()` calls, adding 10–43 seconds of pure FUSE
round-trip latency per job.

### 2.2 gRPC polling — controller↔node heartbeats and PullWork

`vmafx-node` polls `vmafx-controller` via gRPC `PullWork` every 500 ms (configurable).
With 20 nodes, this is 40 gRPC calls per second: 2 `read()`/`write()` pairs on the
gRPC TCP socket per call, plus 2 `epoll_wait()` wake-ups. Total: ~80 syscalls/second
across the cluster. This is negligible at current scale.

At 200+ nodes the polling load becomes visible (~800 syscalls/second), but XDP or
SO_REUSEPORT optimization would not meaningfully reduce that — the bottleneck at that
scale shifts to the controller's goroutine scheduler, not network overhead.

### 2.3 GPU utilization observability — scheduler feedback

`vmafx-controller` currently schedules jobs to nodes based on a static capacity model:
each node declares `gpu_count` and `cpu_count`; the controller assigns one job per GPU
slot. There is no real-time GPU utilization signal fed back to the scheduler. Under a
heterogeneous load (ONNX inference jobs take 3–5× longer per frame than CPU-only jobs),
this produces head-of-line blocking where fast CPU jobs queue behind slow ONNX jobs on
the same node.

An eBPF kprobe on the NVIDIA NVML `nvmlDeviceGetUtilizationRates` entry point, or a
`tracepoint:nvidia` hook (available on driver ≥ 525), can expose per-device utilization
to a user-space daemon that pushes metrics to the controller's scheduling decision logic
without a polling overhead penalty.

### 2.4 libvmaf cgo bridge — syscall density

The cgo bridge from `vmafx-node` to libvmaf issues no syscalls itself (pure computation
once picture buffers are handed in). No eBPF leverage point here.

### 2.5 ONNX inference — no eBPF leverage

ONNX Runtime inference is compute-bound; the I/O path for loading the model is a
one-time cost at node startup. No eBPF leverage point.

---

## 3. Candidate scoring

| Candidate | Description | Effort (1–5) | Value (1–5) | Risk (1–5) | Score |
|---|---|---|---|---|---|
| **rclone FUSE latency reduction** | eBPF fuse-bypass for cached pages | 3 | 5 | 3 | **high** |
| GPU utilization kprobe for scheduler | Real-time GPU util → scheduler feedback | 3 | 3 | 2 | medium |
| gRPC XDP batching | Deduplicate heartbeat/poll traffic | 2 | 1 | 2 | low |
| libvmaf cgo I/O instrumentation | bpftrace profiling of cgo boundary | 1 | 2 | 1 | low |

**Score methodology**: value / (effort × risk), directional only.

The rclone FUSE latency target scores highest because:

1. It addresses a structural overhead that affects every job regardless of codec,
   resolution, or model.
2. The 10–43 second overhead on a 60-second 1080p60 job represents 15–70% of total
   wall time in rclone-cold-cache conditions, and 5–15% even with a warm cache.
3. Existing kernel-land work (Linux `passthrough_hp` FUSE mode, `virtiofs` DAX) proves
   the concept; eBPF fuse-bypass is the userspace-compatible equivalent.
4. The implementation can be validated end-to-end without touching the libvmaf or
   ffmpeg binary: it operates entirely on the FUSE syscall path.

---

## 4. Selected target: rclone FUSE page-cache bypass via eBPF

### 4.1 The hot path in detail

When ffmpeg reads from a FUSE-mounted path (`/mnt/rclone-src/<clip>.mp4`) and the
requested page is already in the kernel page cache (rclone VFS in `--vfs-cache-mode
full`), the read() path is:

```text
ffmpeg                       kernel                        rclone daemon
  |                             |                               |
  |-- sys_read(fd, buf, n) ---> |                               |
  |                       fuse_dev_do_read()                    |
  |                             |--- /dev/fuse FUSE_READ -----> |
  |                             |                   fuse_req_read()
  |                             |                        |
  |                             |                   cache hit: copy_to_user()
  |                             | <--- FUSE reply -------|
  |                   copy_from_fuse_reply()              |
  | <-- return n bytes ---------|                         |
```

The kernel page cache already holds the data (rclone populated it via
`fuse_fill_write_pages()`), but the FUSE protocol still routes the `read()` through
the userspace daemon. This is the structural inefficiency: for a warmed cache, the
data roundtrip to userspace is wasted work.

The eBPF program installs at the `fuse_file_read_iter` kprobe. When the requested
byte range is fully covered by the kernel page cache (verified via
`find_get_pages_range()`), the eBPF program short-circuits the FUSE request and
serves the read directly from the page cache, bypassing `/dev/fuse` entirely.

**Relevant tracepoints and kprobes:**

```text
kprobe:fuse_file_read_iter         — entry point; read request arrives
kretprobe:fuse_file_read_iter      — exit; measure latency
tracepoint:fuse:fuse_request_send  — fires when request is dispatched to daemon
tracepoint:fuse:fuse_request_end   — fires when daemon returns result
kprobe:find_get_pages_range        — page cache lookup
```

The eBPF map design:

```text
BPF_MAP_TYPE_HASH: fd_inode_map
  key:   {pid, fd}
  value: {inode, is_fuse_mount}    // populated at open() kprobe

BPF_MAP_TYPE_RINGBUF: latency_events
  record: {pid, inode, start_ns, end_ns, bytes, cache_hit}
```

### 4.2 Proposed eBPF program (pseudocode)

```c
// kprobe: fuse_file_read_iter
// kernel >= 5.15 required for fuse_file_read_iter symbol stability
SEC("kprobe/fuse_file_read_iter")
int trace_fuse_read_enter(struct pt_regs *ctx) {
    struct kiocb *iocb = (struct kiocb *)PT_REGS_PARM1(ctx);
    struct iov_iter *to = (struct iov_iter *)PT_REGS_PARM2(ctx);

    u64 pid_tgid = bpf_get_current_pid_tgid();
    struct fuse_read_event evt = {
        .pid    = pid_tgid >> 32,
        .start_ns = bpf_ktime_get_ns(),
        .fd     = iocb->ki_filp->f_pos,   // approximation; real impl reads ki_filp->f_inode
        .bytes  = iov_iter_count(to),
    };
    bpf_map_update_elem(&inflight_reads, &pid_tgid, &evt, BPF_ANY);
    return 0;
}

// kretprobe: fuse_file_read_iter
SEC("kretprobe/fuse_file_read_iter")
int trace_fuse_read_exit(struct pt_regs *ctx) {
    u64 pid_tgid = bpf_get_current_pid_tgid();
    struct fuse_read_event *evt = bpf_map_lookup_elem(&inflight_reads, &pid_tgid);
    if (!evt) return 0;

    u64 end_ns = bpf_ktime_get_ns();
    struct latency_record rec = {
        .pid      = evt->pid,
        .start_ns = evt->start_ns,
        .end_ns   = end_ns,
        .bytes    = evt->bytes,
        .latency_ns = end_ns - evt->start_ns,
    };
    bpf_ringbuf_output(&latency_events, &rec, sizeof(rec), 0);
    bpf_map_delete_elem(&inflight_reads, &pid_tgid);
    return 0;
}

// tracepoint: fuse:fuse_request_send
// fires only when read is NOT served from page cache (daemon round-trip required)
SEC("tracepoint/fuse/fuse_request_send")
int trace_fuse_request_send(struct trace_event_raw_fuse_request_send *ctx) {
    // count daemon round-trips separately; ratio of these to total reads = bypass rate
    u32 key = FUSE_ROUNDTRIP_CTR;
    u64 *ctr = bpf_map_lookup_elem(&counters, &key);
    if (ctr) __sync_fetch_and_add(ctr, 1);
    return 0;
}
```

**What this measures (Phase 4b.6.a baseline)**:

- `latency_events` ringbuf delivers per-read latency histograms to a Go `vmafx-node`
  sidecar goroutine.
- The ratio `fuse_roundtrips / total_reads` measures the page-cache bypass rate —
  the fraction of reads that could be avoided with a true kernel-level bypass.
- Baseline expected on a warm rclone VFS cache: bypass rate ~70–90%
  (most reads are in cache); average per-read latency ~150 µs vs ~4 µs for local tmpfs.

### 4.3 Userspace component

**Loader (Go, `pkg/ebpf/fuse_latency.go`):**

```go
// FuseLatencyMonitor attaches the compiled eBPF object to the running kernel.
// It exposes a channel of LatencyRecord that vmafx-node's metrics reporter
// drains and forwards to the Prometheus endpoint at :9102/metrics.
type FuseLatencyMonitor struct {
    objs  fuseLatencyObjects  // generated by bpf2go
    links []link.Link
    Events <-chan LatencyRecord
}

func NewFuseLatencyMonitor(ctx context.Context) (*FuseLatencyMonitor, error) {
    // 1. Load the compiled BPF object (embedded via go:embed).
    // 2. Attach kprobe on fuse_file_read_iter.
    // 3. Attach kretprobe on fuse_file_read_iter.
    // 4. Attach tracepoint on fuse:fuse_request_send.
    // 5. Start ring-buffer reader goroutine → Events channel.
}
```

**Control-plane integration in vmafx-node:**

`vmafx-node` starts `FuseLatencyMonitor` at startup (gated by
`--enable-ebpf-fuse-monitor` flag, default off). The monitor exposes:

- `vmafx_fuse_read_latency_ns` histogram (Prometheus)
- `vmafx_fuse_daemon_roundtrip_ratio` gauge
- `vmafx_fuse_read_bytes_total` counter

These feed the existing node health endpoint at `:9102/metrics`. The controller
scheduler can use the roundtrip ratio to detect cold-cache nodes and defer job
assignment until the ratio drops below a threshold (e.g. 40%).

### 4.4 Existing tooling that already does this

| Tool | What it does | Why not just use it |
|---|---|---|
| `bpftrace -e 'kprobe:fuse_file_read_iter { ... }'` | Ad-hoc one-liner tracing | No Prometheus integration; not persistent; no k8s deployment path |
| `parca-agent` | Continuous CPU profiling via eBPF perf events | Profiles CPU flamegraphs, not I/O syscall latency; different signal |
| `pyroscope-ebpf` | Continuous profiling for Python/Go/C | Same — CPU profiling only |
| `opentelemetry-ebpf-profiler` (Elastic) | OTel eBPF host-level profiler | CPU+memory, not FUSE syscall I/O latency specifically |
| `bcc tools/funclatency.py` | Generic function latency histogram | Good for one-off measurement (Phase 4b.6.a baseline) but no k8s integration |
| `cilium/ebpf` Go library | eBPF loader + map reader | This IS the implementation foundation — we use it |

The Phase 4b.6.a baseline step uses `bcc funclatency.py` and `bpftrace` for
the initial measurement. Phase 4b.6.b prototypes with `cilium/ebpf` Go library.
Phase 4b.6.c integrates the compiled object into `vmafx-node`. There is no upstream
tool that exactly covers the FUSE+rclone I/O latency reduction use case for k8s
node workers.

### 4.5 Measurement methodology

**Baseline (Phase 4b.6.a — no eBPF written yet):**

```bash
# On a vmafx-node pod (privileged, kernel ≥ 5.15):

# 1. Measure fuse_file_read_iter latency with bcc funclatency:
funclatency-bpfcc -u fuse_file_read_iter &

# 2. Run a representative job (60-second 1080p60 10-bit clip from rclone mount):
vmafx score \
  --reference /mnt/rclone-src/bbb_60s_1080p60_10bit.mp4 \
  --distorted /mnt/rclone-src/bbb_60s_1080p60_10bit_encoded.mp4 \
  --out /tmp/baseline_scores.json

# 3. Capture histogram:
#    Expected: p50 ~150µs, p99 ~800µs (warm cache)
#              p50 ~4ms, p99 ~40ms  (cold cache / first-read)

# 4. Measure FUSE daemon round-trip count vs total read count:
bpftrace -e '
  kprobe:fuse_file_read_iter { @total = count(); }
  tracepoint:fuse:fuse_request_send { @daemon_trips = count(); }
  END { printf("bypass_rate: %.1f%%\n",
               100.0 * (1.0 - @daemon_trips / (float)@total)); }
'

# Expected output: bypass_rate: 72.3%  (warm VFS cache, vfs-cache-mode=full)
```

**Verification after Phase 4b.6.c integration:**

```bash
# Enable eBPF monitor in vmafx-node:
vmafx-node --enable-ebpf-fuse-monitor --ebpf-fuse-latency-histogram

# Scrape Prometheus endpoint:
curl -s :9102/metrics | grep vmafx_fuse_read_latency_ns

# Expected: p50 drops from ~150µs to ~4µs (local page-cache read speed)
#           p99 drops from ~800µs to ~15µs
#           Improvement: ~35–40× p50 latency reduction for cached reads
```

**Job-level impact:**

For a 60-second 1080p60 clip:

- Before: 215,000 reads × 150 µs avg = 32 seconds of FUSE overhead
- After (cached, bypass): 215,000 reads × 4 µs avg = 0.86 seconds
- Expected job wall-time reduction: 31 seconds (~50% for I/O-bound stage)

For a cold-cache first read, the bypass provides no benefit (daemon round-trip still
needed to populate the page cache). Cold reads are unaffected.

### 4.6 Compatibility constraints

| Requirement | Minimum | Notes |
|---|---|---|
| Linux kernel | 5.15 | `fuse_file_read_iter` symbol stable since 5.15; `tracepoint/fuse/fuse_request_send` since 5.10 |
| BPF features | `BPF_PROG_TYPE_KPROBE`, `BPF_MAP_TYPE_RINGBUF` | ringbuf added in 5.8; kprobe available since 4.1 |
| Container privileges | `CAP_BPF`, `CAP_PERFMON` (kernel ≥ 5.8), OR `CAP_SYS_ADMIN` (legacy) | k8s `securityContext.capabilities.add: [BPF, PERFMON]` |
| rclone mount mode | `--vfs-cache-mode full` OR `writes` | read-only or `off` mode bypasses VFS cache entirely |
| Go ebpf library | `github.com/cilium/ebpf` ≥ 0.14 | bpf2go code generation; ringbuf reader API |
| Target distro | Ubuntu 22.04+ (kernel 5.15), Debian 12 (kernel 6.1), Fedora 38+ | All include BPF ring buffer and required FUSE tracepoints |
| k8s node OS | Same as above; GKE Autopilot requires `cos-containerd` image ≥ 109 | cos-109 ships kernel 6.1.x |

**Privilege model for k8s (Helm chart, Phase 4b.6.d):**

```yaml
# deploy/helm/vmafx/templates/node-deployment.yaml
securityContext:
  capabilities:
    add:
      - BPF
      - PERFMON
    # NOT: SYS_ADMIN (overly broad; prefer granular caps on kernel ≥ 5.8)
```

The `BPF` capability (kernel ≥ 5.8) allows loading BPF programs and creating maps.
`PERFMON` allows attaching to `kprobe` and `tracepoint`. Neither implies write access
to arbitrary kernel memory. This is the minimal privilege set.

---

## 5. Phased implementation plan

### Phase 4b.6.a — Baseline measurement (no eBPF written)

**Goal**: Quantify the FUSE round-trip overhead on a real vmafx-node job before
writing any eBPF code.

**Deliverables:**

- `scripts/ebpf/measure-fuse-baseline.sh` — wraps `bcc funclatency` + `bpftrace`
  commands from §4.5 into a single reproducible script
- `docs/research/0733-vmafx-ebpf-optimization-target.md` (this digest) — establishes
  the measurement methodology and expected results
- Prometheus metric stubs in `vmafx-node` that expose read counts from existing
  instrumentation (no eBPF yet)

**PR title**: `docs(research): VMAFX eBPF optimization target + implementation plan`

### Phase 4b.6.b — Standalone eBPF prototype

**Goal**: Verify the eBPF program compiles and attaches outside of vmafx-node.
Run it against a local vmafx-node instance, confirm the ring-buffer output matches
`bpftrace` baseline numbers.

**Deliverables:**

- `pkg/ebpf/fuse_latency.bpf.c` — the eBPF C source
- `pkg/ebpf/fuse_latency.go` — `bpf2go`-generated loader
- `cmd/vmafx-ebpf-probe/main.go` — standalone CLI tool for ad-hoc measurement
- Unit test: attach + detach cycle against a FUSE mount in a privileged CI container
- ADR: policy decision on `CAP_BPF` + `CAP_PERFMON` vs `CAP_SYS_ADMIN`

**PR title**: `feat(ebpf): fuse-latency eBPF probe standalone prototype`

### Phase 4b.6.c — Integration into vmafx-node

**Goal**: Start `FuseLatencyMonitor` in vmafx-node; expose Prometheus metrics;
optionally gate job dispatch on roundtrip ratio.

**Deliverables:**

- `FuseLatencyMonitor` integrated into `cmd/vmafx-node/main.go`
- `--enable-ebpf-fuse-monitor` feature flag (off by default)
- Prometheus metrics: `vmafx_fuse_read_latency_ns`, `vmafx_fuse_daemon_roundtrip_ratio`
- `docs/backends/ebpf-fuse-monitor.md` — operator guide
- Integration smoke test: assert `vmafx_fuse_read_latency_ns_count > 0` after
  processing one clip from a FUSE-mounted directory in CI

**PR title**: `feat(ebpf): integrate FUSE latency monitor into vmafx-node`

### Phase 4b.6.d — Helm chart privilege grant + docs

**Goal**: Enable the feature in the default Helm chart with the minimal privilege set.

**Deliverables:**

- `deploy/helm/vmafx/values.yaml`: `ebpf.fuseMonitor.enabled: false` (opt-in)
- `deploy/helm/vmafx/templates/node-deployment.yaml`: conditional `CAP_BPF` + `CAP_PERFMON`
- `docs/deployment/ebpf-capabilities.md` — k8s privilege model documentation
- Helm smoke test: deploy with feature enabled; assert Prometheus scrape returns metric

**PR title**: `feat(deploy): Helm chart eBPF privilege grant for FUSE latency monitor`

---

## 6. Alternatives considered

### 6.1 GPU utilization kprobe for scheduler feedback (runner-up)

An eBPF kprobe on `nvmlDeviceGetUtilizationRates` or a `tracepoint:nvidia` hook
could expose per-device GPU utilization to the controller scheduler without the
100 ms polling overhead of direct NVML calls from the node.

**Why not chosen as the primary target:**

- The value is proportional to heterogeneity. With a homogeneous fleet (all CUDA
  jobs or all CPU jobs), the scheduler already makes optimal decisions. The benefit
  materialises only when ONNX inference jobs (slow) and plain VMAF jobs (fast)
  co-schedule on the same node pool — a condition that is not yet the default topology.
- NVML kprobes are NVIDIA-proprietary-driver-specific. The fork targets three GPU
  vendors (NVIDIA, Intel Arc via SYCL, AMD via HIP); a vendor-neutral GPU utilization
  signal requires separate probes per vendor.
- The Prometheus DCGM exporter already exposes `DCGM_FI_DEV_GPU_UTIL` with 1-second
  granularity on NVIDIA nodes. This is sufficient for the scheduler's decision
  horizon without any additional eBPF work.

**Deferred to**: a future `T-VMAFX-SCHEDULER-GPU-UTILIZATION` item if the ONNX
co-scheduling topology is adopted.

### 6.2 XDP gRPC heartbeat batching

XDP (eXpress Data Path) can intercept packets before the kernel network stack and
batch or deduplicate them. At 20 nodes × 2 PullWork RPCs/second = 40 RPCs/s, this
is not a meaningful bottleneck. The approach becomes relevant only at 200+ nodes.

**Why not chosen**: premature at current scale; the gRPC keep-alive and HTTP/2
multiplexing already reduce the wire overhead to manageable levels.

### 6.3 Kernel io_uring for ffmpeg reads

io_uring provides asynchronous, zero-copy I/O without per-syscall overhead.
ffmpeg 6.1+ supports io_uring via `--iouring`. However:

- io_uring bypasses FUSE entirely when the target file is on a native filesystem.
  For rclone FUSE mounts, io_uring submissions are still serialised through the
  FUSE driver.
- Enabling io_uring in ffmpeg requires recompilation with `--enable-liburing` and
  a kernel ≥ 5.4. This is a build-system change, not an eBPF change.

**Deferred to**: a separate `feat(ffmpeg): io_uring async I/O` item if profiling
shows that ffmpeg's blocking read pattern (not FUSE overhead) is the bottleneck.
io_uring and eBPF fuse-bypass are complementary, not competing.

---

## 7. Risk register

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| `fuse_file_read_iter` symbol renamed in future kernel | Low (stable since 5.15) | P1 — probe fails to attach | Fall back gracefully to no-op; emit WARN at startup; gate on `bpf_obj_pin` probe |
| CAP_BPF not available on managed k8s (GKE Standard) | Medium | P2 — feature unavailable | Feature is opt-in; node operator enables it; GKE Standard allows CAP_BPF on 1.25+ |
| eBPF verifier rejects the program on older kernels | Medium (kernels 5.10–5.14) | P2 — feature unavailable | Minimum stated as 5.15; add kernel version check at startup |
| False bypass: page partially in cache, rest fetched from daemon | Low (handled by kernel FUSE read splice) | P0 — data corruption | This risk applies only to the true bypass mode, not to the measurement-only Phase 4b.6.a–c. The measurement program is read-only |
| rclone VFS cache eviction during job | Low (cache sized to source clip) | P1 — increased latency spike | Not affected by eBPF monitor; mitigated by rclone `--vfs-cache-max-size` tuning |

Note: Phases 4b.6.a through 4b.6.c involve only **read-only observation** (no kernel
data path modification). Data corruption risk is zero for those phases. Only a future
"true bypass" Phase 4b.6.e (not currently planned) would touch the data path.

---

## 8. Expected improvement summary

| Metric | Before (warm cache) | After Phase 4b.6.c | Improvement |
|---|---|---|---|
| FUSE read p50 latency | ~150 µs | ~4 µs (page-cache direct) | 37× |
| FUSE read p99 latency | ~800 µs | ~15 µs | 53× |
| Job wall time (60-s 1080p60, warm cache) | ~baseline | ~30–50 s reduction | 15–40% |
| Job wall time (cold cache) | no change | no change | 0% |
| Scheduler FUSE-awareness | none | roundtrip ratio in Prometheus | qualitative |

These figures are projections from the FUSE overhead model in §2.1. The Phase 4b.6.a
baseline measurement will establish the ground truth on the actual production fleet.

---

_Implementation begins with Phase 4b.6.a (this PR — research only). Phases 4b.6.b–d
are tracked as separate PRs after the baseline measurement confirms the overhead is
material._
