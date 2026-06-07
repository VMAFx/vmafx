## Performance

- **vmafx-node eBPF FUSE bypass** (`cmd/vmafx-node/bpf/`): probe-only eBPF
  tracepoint program that tracks file descriptors opened under the rclone FUSE
  mount prefix and marks them bypass-eligible in a BPF hash map.  The Go-side
  loader (cilium/ebpf v0.21.0) drains a ring-buffer event stream to keep an
  in-process FD cache warm.  Bypass cuts p50 read latency for warm-cache clips
  from ~370 ms to ~10 ms (37×, Research-0733).  Gated behind
  `VMAFX_EBPF_BYPASS=1` (default off); requires Linux 5.15+ and `CAP_BPF`.
  ADR-0779.
