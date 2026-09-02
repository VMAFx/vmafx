<!-- markdownlint-disable MD013 MD060 -->
# Remote Storage Streaming (rclone)

`vmafx-node` streams reference and distorted video directly from remote storage
without writing the content to local disk or RAM.  It uses
[rclone](https://rclone.org) — a single Go binary that supports 70+ storage backends —
bundled into the node container image.

## Supported remote types

Any rclone remote works.  Common examples:

| Backend | URI scheme | Notes |
|---------|-----------|-------|
| Amazon S3 | `s3://bucket/path` | Also MinIO, Ceph, Wasabi, Backblaze B2 S3, Cloudflare R2 |
| Google Cloud Storage | `gcs://bucket/path` | |
| Azure Blob Storage | `azblob://container/blob` | |
| SFTP | `sftp://user@host/path` | SSH key or password auth |
| HTTP / HTTPS | `http://host/path` | Anonymous or basic-auth |
| Local filesystem | `/path/to/file.yuv` | No rclone needed; passthrough |
| rclone native syntax | `remote:bucket/path` | Any named remote from rclone.conf |

For the full list see <https://rclone.org/overview/>.

## Storage modes

`vmafx-node` supports three storage modes, selected by the `VMAFX_STORAGE_MODE`
environment variable (or `storage.mode` in `values.yaml`):

| Mode | Mechanism | When to use |
|------|-----------|-------------|
| `http-serve` (default) | `rclone serve http remote: --addr :PORT` | Multi-input jobs (ref + dis); no FUSE required |
| `mount` | `rclone mount remote: /mnt/<job-id>` (FUSE) | Sources requiring random access; fallback |
| `auto` | Selects `http-serve` unless FUSE unavailable | General purpose |

### HTTP-serve mode (recommended)

The node spawns one `rclone serve http` subprocess per resolved input, waits
for it to become reachable, and passes `http://127.0.0.1:PORT/<path>` directly
to ffmpeg via `-i`.  libavformat's built-in HTTP demuxer reads the bytes; rclone
fetches them from the remote backend.

Advantages:

- No FUSE kernel dependency.
- Both `--reference` and `--distorted` can be served simultaneously on
  different ports.
- Works in standard distroless containers without `SYS_ADMIN` capability.

### FUSE-mount mode (fallback)

The node mounts the remote at a per-job temporary directory, waits for the
asset to appear on the mount, and returns the local path to ffmpeg.

Requirements:

- The `fuse3` package must be present in the container (`docker/Dockerfile.node`
  includes it).
- The pod must have `securityContext.capabilities.add: [SYS_ADMIN]` and
  `/dev/fuse` device access.
- The eBPF FUSE-bypass research (Research-0733) targets reducing round-trip
  overhead in this mode.

## Providing credentials

Credentials are provided via an rclone configuration file (`rclone.conf`).
The Helm chart mounts this file from a Kubernetes Secret at `/etc/vmafx/rclone.conf`.

### Step 1 — write rclone.conf

Create an `rclone.conf` file with your remote definitions:

```ini
[s3-prod]
type = s3
provider = AWS
region = us-east-1
# Leave access_key_id / secret_access_key empty to use the EC2 IAM role.

[gcs-prod]
type = google cloud storage
project_number = 123456789

[sftp-archive]
type = sftp
host = archive.example.com
user = vmafx
key_file = /etc/vmafx/ssh-key
```

### Step 2 — pass the config to the Helm chart

```bash
helm upgrade --install vmafx deploy/helm/vmafx \
  --set-file storage.rclone.config=./rclone.conf \
  --set storage.mode=http-serve \
  --set node.enabled=true
```

The chart creates a `<release>-rclone-config` Secret containing `rclone.conf`
and mounts it read-only at `/etc/vmafx/rclone.conf` in every `vmafx-node` pod.

### Step 3 — submit a scoring job

Jobs reference assets by URI.  The `vmafx-controller` accepts job requests
with `reference_uri` and `distorted_uri` fields.  Examples:

```text
s3://my-bucket/src01_hrc00_576x324.yuv
gcs://vmaf-corpus/src01_hrc01_576x324.yuv
sftp://archive.example.com/corpus/ref.yuv
/local/path/to/ref.yuv
```

## URI scheme reference

| URI | rclone remote:path mapping |
|-----|---------------------------|
| `s3://bucket/key/file.yuv` | `s3:bucket/key/file.yuv` |
| `gcs://bucket/dir/file.yuv` | `gcs:bucket/dir/file.yuv` |
| `azblob://container/blob.yuv` | `azblob:container/blob.yuv` |
| `sftp://user@host/path/file.yuv` | `sftp:user@host/path/file.yuv` |
| `rclone://s3-prod:bucket/file.yuv` | `s3-prod:bucket/file.yuv` |
| `s3-prod:bucket/file.yuv` | passed through unchanged |
| `http://host/file.yuv` | passed through unchanged (ffmpeg HTTP) |
| `/local/path/file.yuv` | passthrough — no rclone |
| `file:///local/path/file.yuv` | passthrough — no rclone |

## Performance notes

- **HTTP-serve mode latency**: rclone starts in ~100–200 ms.  The node waits
  up to 15 s (`serveReadyTimeout`) for the server to be reachable.  For
  high-throughput pipelines, consider pre-warming by keeping the serve process
  alive across jobs (planned for Phase 4b.6).
- **FUSE mode latency**: mount takes ~500 ms–2 s depending on the remote.
  The readiness poller waits up to 20 s (`mountReadyTimeout`).
- **Bandwidth**: rclone uses the full available network bandwidth.  AWS S3 to
  EC2 within the same region typically delivers 500 MB/s+ per connection.
- **eBPF FUSE bypass**: Research-0733 investigates using eBPF to reduce FUSE
  kernel-userspace round-trip overhead in mount mode.  Expected benefit: ~30%
  latency reduction per `read()` syscall on the FUSE path.

## Troubleshooting

### rclone: command not found

The `vmafx-node` image bundles rclone at `/usr/local/bin/rclone`.  Verify:

```bash
docker run --rm --entrypoint /usr/local/bin/rclone ghcr.io/vmafx/vmafx-node:latest version
```

### Authentication errors

Verify the rclone.conf is mounted correctly:

```bash
kubectl exec -it <node-pod> -- cat /etc/vmafx/rclone.conf
```

Check for IAM role availability on EC2/GKE:

```bash
kubectl exec -it <node-pod> -- /usr/local/bin/rclone ls s3:my-bucket --config /etc/vmafx/rclone.conf
```

### FUSE mount fails

Ensure the pod has `SYS_ADMIN` capability and `/dev/fuse` access:

```yaml
securityContext:
  capabilities:
    add: [SYS_ADMIN]
  allowPrivilegeEscalation: true
volumes:
  - name: fuse-dev
    hostPath:
      path: /dev/fuse
```

### Switching to HTTP-serve mode

If FUSE is not available, switch to HTTP-serve (default):

```bash
helm upgrade vmafx deploy/helm/vmafx --set storage.mode=http-serve
```

## Architecture

```text
vmafx-node
  │
  ├── pkg/storage.Storage (interface)
  │     ├── HTTPServeStorage  — rclone serve http :PORT → http://127.0.0.1:PORT/path
  │     ├── FUSEMountStorage  — rclone mount remote: /tmp/vmafx-rclone-<id>/
  │     └── LocalStorage      — passthrough (no rclone)
  │
  └── cmd/vmafx-node/executor.go
        Executor.Execute():
          refURL, cleanRef = store.Prepare(job.ReferenceURI)
          disURL, cleanDis = store.Prepare(job.DistortedURI)
          vmaf --reference refURL --distorted disURL ...
          cleanRef(); cleanDis()
```

## See also

- [ADR-0719](../adr/0719-vmafx-node-rclone-integration.md) — design record for this feature.
- [ADR-0709](../adr/0709-vmafx-phase4b-distributed-platform.md) — Phase 4b umbrella.
- [rclone documentation](https://rclone.org) — full remote configuration reference.
- Research-0733 — eBPF FUSE-bypass research.
