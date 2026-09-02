<!-- markdownlint-disable MD013 MD060 -->
# ADR-0719: vmafx-node rclone Integration — Remote-Asset Streaming Without Disk Materialisation

- **Status**: Accepted
- **Date**: 2026-05-28
- **Deciders**: Lusoris
- **Tags**: `architecture`, `go`, `node`, `rclone`, `storage`, `ffmpeg`, `phase4b`, `fork-local`

## Context

Phase 4b (ADR-0709) introduces `vmafx-node` as the scoring worker in the distributed
VMAFX platform.  Each node pulls jobs from the controller, resolves the reference and
distorted video URIs, runs ffmpeg encoding, and scores via libvmaf.

A key requirement from Phase 4b is that the node must not materialise the full source
video to disk or RAM before scoring.  At CHUG / K150K / BVI-DVC scale, a single reference
sequence can exceed 10 GB; writing it to ephemeral node storage before scoring would:

- Saturate cluster I/O bandwidth.
- Require large ephemeral disk allocations in the k8s PodSpec.
- Eliminate the per-job cost benefit of horizontal scaling.

rclone (<https://rclone.org>) provides a single binary that can access 70+ storage backends
(S3, GCS, Azure Blob, SFTP, HTTP, local filesystem, etc.) and expose them to consumer
processes either as an HTTP server (`rclone serve http`) or as a POSIX FUSE mount
(`rclone mount`).  The user identified rclone as the correct tool for this use case.

The architecture dispatch for Phase 4b.5 evaluated three rclone streaming modes:

- **HTTP-serve**: `rclone serve http remote: --addr :PORT` + ffmpeg `-i http://localhost:PORT/path`.
  Zero-copy, multi-input capable, no FUSE kernel dependency. Selected as primary mode.
- **FUSE-mount**: `rclone mount remote: /mnt/<job-id>` + ffmpeg reads from local path.
  Requires `fuse3` in the container; adds FUSE kernel round-trip overhead. Selected as fallback.
- **Pipe**: `rclone cat | ffmpeg -i pipe:0`. Single input only; two-input pipe juggling
  is fragile. Not selected.

## Decision

rclone is bundled into the `vmafx-node` distroless image via a multi-stage Docker build
(`docker/Dockerfile.node`).  The primary storage mode is HTTP-serve; FUSE-mount is the
fallback.  The storage layer is exposed as the `pkg/storage.Storage` interface:

```go
type Storage interface {
    Prepare(ctx context.Context, sourceURI string) (readableURL string, cleanup func(), err error)
    Mode() Mode  // "http-serve" | "mount" | "auto"
}
```

Three implementations ship:

- `HTTPServeStorage` — spawns `rclone serve http` per-job; returns `http://127.0.0.1:PORT/<path>`.
- `FUSEMountStorage` — spawns `rclone mount` per-job to a tmpdir; returns local path.
- `LocalStorage` — passthrough for `file://` or bare paths.

The `cmd/vmafx-node/executor.go` `Executor.Execute()` method calls `store.Prepare(ref)`
and `store.Prepare(dis)` before the ffmpeg subprocess and defers `cleanup()` for both.

The rclone configuration is provided via a Kubernetes Secret mounted at
`/etc/vmafx/rclone.conf`.  The Helm chart (`deploy/helm/vmafx/`) adds:

- `templates/secret-rclone-config.yaml` — creates the Secret from `values.storage.rclone.config`.
- `templates/node-deployment.yaml` — the vmafx-node Deployment, mounting the Secret read-only.
- `values.yaml` additions: `storage.mode`, `storage.rclone.config`, and `node.*` block.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **`rclone serve http` (HTTP-serve)** — chosen primary | Zero-copy, multi-input concurrent, no FUSE, ffmpeg libavformat HTTP demuxer native | Per-job subprocess spawn; PORT selection | Best fit: multi-input (ref + dis), no kernel dependency |
| `rclone mount` (FUSE) | POSIX-transparent random access, works with any libav input | FUSE kernel dependency, slower than HTTP-serve, SIGKILL-to-unmount lifecycle complexity | Ships as fallback mode; not primary |
| `rclone cat pipe` | No HTTP server, no FUSE | Single input only; `ffmpeg -i pipe:0` cannot handle both ref+dis simultaneously | Not viable for two-input scoring |
| k8s PVC + CSI driver | Native k8s; no rclone binary | Forces materialisation to PVC; cloud-provider-specific CSI; no zero-copy | Per ADR-0709: "Copy files to node ephemeral storage... not zero-copy" |
| S3-FUSE (goofys, s3fs) | Lightweight | S3-only; no unified multi-backend support | rclone covers 70+ backends with one binary |

## Consequences

**Positive:**

- Zero-copy streaming: reference and distorted videos stream from S3 / GCS / Azure Blob /
  SFTP / HTTP without any intermediate disk write.
- Unified storage abstraction: `pkg/storage.Storage` interface decouples the executor
  from any specific remote backend.
- 70+ rclone backends supported out of the box; no per-backend node code required.
- Credentials are managed as Kubernetes Secrets; no credentials baked into the image.
- HTTP-serve mode adds no FUSE kernel dependency to the node container.

**Negative:**

- rclone binary (~55 MB uncompressed) increases the node image size.
- Per-job `rclone serve http` subprocess adds ~100 ms startup latency before ffmpeg can
  read the first byte.  The readiness poller in `waitForHTTP` compensates.
- FUSE-mount fallback requires `fuse3` and `/dev/fuse` device access in the k8s PodSpec
  (add `securityContext.capabilities.add: [SYS_ADMIN]` when using mount mode).

**Neutral / follow-ups:**

- Phase 4b.6 (eBPF) will investigate whether eBPF can reduce FUSE round-trip overhead
  for the mount-mode fallback (Research-0733 target: FUSE bypass).
- The HTTP-serve port is ephemeral (OS-assigned free port); no port reservation in the
  PodSpec is needed.
- When HTTP-serve and mount mode both run concurrently for a job (e.g., ref via HTTP,
  dis via local path), the `LocalStorage` passthrough short-circuits the rclone spawn
  for the local input.

## References

- [ADR-0709](0709-vmafx-phase4b-distributed-platform.md) — Phase 4b umbrella; item 4b.5.
- [ADR-0711](0711-vmafx-controller-impl.md) — vmafx-controller Phase 4b.1.
- Research-0733 — eBPF FUSE-bypass research (target: FUSE round-trip overhead in mount mode).
- `req` — user direction (paraphrased): "I hope it was rclone that can stream directly
  into the encoder without materialising files to disk or RAM." (2026-05-28)
- `req` — architecture dispatch (paraphrased): use rclone to stream files without copying
  to disk or RAM first; HTTP-serve mode recommended as primary mode. (2026-05-28)
- <https://rclone.org/commands/rclone_serve_http/>
- <https://rclone.org/commands/rclone_mount/>
