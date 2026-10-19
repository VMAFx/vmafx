<!-- markdownlint-disable MD013 MD060 -->
# Remote Storage Streaming (rclone)

The `pkg/storage` package turns a remote asset URI into something ffmpeg can
read without writing the content to local disk or RAM. It uses
[rclone](https://rclone.org), a single Go binary that supports 70+ storage
backends, bundled into the node container image at `/usr/local/bin/rclone`. The
version is pinned by `RCLONE_VERSION` in `build-config.env` (rclone 1.75.1); the
image builds that release from its module source, so its source is published
with the image ([licensing](../licensing.md)).

!!! warning "Not yet wired into `vmafx-node`"
    `pkg/storage` is implemented and unit-tested, but no non-test code under
    `cmd/` calls it. Today `vmafx-node` passes the `reference` and `distorted`
    fields of a `ScoreRequest` straight to the scorer, and those fields are
    absolute paths (`proto/vmafx/v1/vmafx.proto`), so remote URIs are **not** resolved.
    The sections below describe the package's behaviour and the intended job
    flow. The Helm chart already exports `VMAFX_STORAGE_MODE` to the node, but
    no
    Go code reads it yet.

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

`pkg/storage` defines three modes (`storage.Mode`):

| Mode | Mechanism | When to use |
|------|-----------|-------------|
| `http-serve` (default) | `rclone serve http remote: --addr :PORT` | Multi-input jobs (ref + dis); no FUSE required |
| `mount` | `rclone mount remote: /mnt/<job-id>` (FUSE) | Sources requiring random access; fallback |
| `auto` | Resolves to `http-serve` | General purpose |

The Helm chart's `storage.mode` value documents `http-serve | rclone`
(`deploy/helm/vmafx/values.yaml`), which does not match the package's mode
names; reconcile the two when the package is wired in.

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
The Helm chart mounts this file from a Kubernetes Secret at
`/etc/vmafx/rclone.conf`.

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

The intended flow is that a job names its assets by URI. Today the scoring
request carries `reference` and `distorted` as absolute paths, so these URIs
only resolve once `pkg/storage` is wired into the node. Examples:

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

- **HTTP-serve start-up.** rclone starts in about 100 to 200 ms. The package
  waits up to 15 s (`serveReadyTimeout`) for the server to be reachable.
- **FUSE start-up.** A mount takes about 500 ms to 2 s depending on the
  remote. The readiness poller waits up to 20 s (`mountReadyTimeout`).
- **Bandwidth.** rclone uses the full available network bandwidth. AWS S3 to
  EC2 within the same region typically delivers 500 MB/s or more per
  connection.
- **eBPF FUSE bypass.** Research-0733 investigates using eBPF to cut the FUSE
  kernel-userspace round-trip overhead in mount mode. It is research only: the
  projections (15 to 40% job wall-time on warm-cache nodes, and a 37x lower p50
  FUSE read latency) are in [state.md](../state.md), and no code ships.

## Troubleshooting

### rclone: command not found

The `vmafx-node` image bundles rclone at `/usr/local/bin/rclone`.  Verify:

Images published after `v1.0.0-rc.2` have zstd layers and need Docker Engine 23.0 or
later, Docker Desktop 4.19 or later, Podman or containerd 1.5 or later ([what can pull
them](docker.md#what-can-pull-the-images)).

```bash
docker run --rm --entrypoint /usr/local/bin/rclone ghcr.io/vmafx/vmafx-node:<tag> version
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

The package provides the storage layer; the node side is the intended wiring
and is not implemented (see the warning at the top).

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
          today:    scorer.Score(reference, distorted, model)   (paths, as received)
          intended: refURL, cleanRef = store.Prepare(reference)
                    disURL, cleanDis = store.Prepare(distorted)
                    vmaf --reference refURL --distorted disURL ...
                    cleanRef(); cleanDis()
```

## See also

- [ADR-0719](../adr/0719-vmafx-node-rclone-integration.md) — design record for
  this feature.
- [ADR-0709](../adr/0709-vmafx-phase4b-distributed-platform.md) — Phase 4b
  umbrella.
- [rclone documentation](https://rclone.org) — full remote configuration
  reference.
- Research-0733 — eBPF FUSE-bypass research.

## History

- **v1.0.0-rc.1.** The node image first published without rclone, so remote
  inputs failed until the image was rebuilt with it.
