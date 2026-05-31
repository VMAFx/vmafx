### vmafx-node: rclone remote-asset streaming (Phase 4b.5)

`vmafx-node` now bundles rclone and streams reference and distorted video
directly from remote storage (S3, GCS, Azure Blob, SFTP, HTTP, and 70+ other
backends) without materialising content to disk or RAM.

### New: `pkg/storage` — Storage interface

```go
// Prepare returns a URL or path that ffmpeg can read directly.
store.Prepare(ctx, "s3://bucket/ref.yuv")
```

Three implementations:

- `HTTPServeStorage` — `rclone serve http` per-job; ffmpeg reads via HTTP (default).
- `FUSEMountStorage` — `rclone mount` per-job to tmpdir; ffmpeg reads local path (fallback).
- `LocalStorage` — passthrough for `file://` or bare paths.

### New: `docker/Dockerfile.node`

Multi-stage build bundling rclone + ffmpeg + libvmaf + vmafx-node into a
distroless image.  Verify bundled rclone:

```bash
docker run --rm --entrypoint /usr/local/bin/rclone ghcr.io/vmafx/vmafx-node:latest version
```

### New: Helm chart storage + node sections

`values.yaml`:
```yaml
storage:
  mode: http-serve  # http-serve | mount | auto
  rclone:
    config: |
      [s3-prod]
      type = s3
      region = us-east-1

node:
  enabled: true
  replicaCount: 2
```

The chart creates a `<release>-rclone-config` Secret and mounts it at
`/etc/vmafx/rclone.conf` in every node pod.

See `docs/usage/storage.md` for credentials, URI scheme reference, and performance notes.
