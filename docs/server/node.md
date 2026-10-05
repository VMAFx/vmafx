<!-- markdownlint-disable MD013 MD060 -->
# vmafx-node — Worker Binary

`vmafx-node` is the data-plane scoring worker in the VMAFX distributed
platform (Phase 4b, ADR-0709). It serves the `VmafxScoring` gRPC API and
executes score requests against `libvmaf`.

## Quick start (local)

Start a node; it listens on `:50052` by default.

```bash
export VMAFX_LOG_LEVEL=debug
./vmafx-node
```

The node defaults to CPU scoring. Set `VMAFX_BACKEND` explicitly, or use a
GPU-specific container target, to select another compiled backend.

## gRPC service the node serves

The node hosts the **`VmafxScoring`** service (the same contract as
`vmafx-server`) on `VMAFX_GRPC_LISTEN`, so any gRPC client can dispatch scoring
directly to a node. See
[ADR-1109](../adr/1109-vmafx-node-serve-scoring-grpc.md).

| RPC | Shape | Notes |
| --- | --- | --- |
| `Score` | unary | File-path reference/distorted pair → pooled VMAF + features. |
| `ScoreStream` | bidirectional stream | In-memory per-frame scoring (ADR-0933). One `StreamConfig`, then `FramePair` messages, then EOF; the node returns one `FrameScore` per frame plus a terminal `AggregateScore`. See [grpc-streaming.md](../architecture/grpc-streaming.md). |
| `Health` | unary | Liveness; answers even when no scorer is configured. |

The scoring engine is the shared cgo `pkg/libvmaf`. The node resolves models
from `VMAFX_MODEL_DIR`.

- If no `vmaf` binary or model dir is available, the node still serves
  `Health` and returns `codes.FailedPrecondition` from the scoring RPCs.
- The controller-pull worker loop (`PullWork → Execute → ReportResult`) is a
  separate _client_ role, described in the next section.

Example:

```bash
grpcurl -plaintext localhost:50052 vmafx.v1.VmafxScoring/Health
# {"ok": true, "message": "ok"}
```

## Pulling jobs from the controller

Set `VMAFX_CONTROLLER_ADDR` to the controller's gRPC address and the node
takes jobs from the controller's queue as well as serving direct calls
([ADR-1524](../adr/1524-vmafx-node-controller-client.md)):

```bash
export VMAFX_CONTROLLER_ADDR=vmafx-controller:9090   # the controller's gRPC port
export VMAFX_BACKEND=cpu                              # the backend this node runs
./vmafx-node
# INFO controller client started controller=vmafx-controller:9090 node=<host> slots=1 backends=[cpu] ...
# INFO registered with controller node_id=... attempt=1
```

Without `VMAFX_CONTROLLER_ADDR` the node logs `controller client disabled`
and serves direct `VmafxScoring` calls only.

What the node does with the address:

1. **Register.** `RegisterNode` announces the node name (`VMAFX_NODE_ID`,
   default the host name), the backend it runs and its slot count. If the
   controller is unreachable or refuses, the node retries with jittered
   exponential backoff (0.5 s growing to 30 s) until it answers.
2. **Heartbeat.** Every `VMAFX_CONTROLLER_HEARTBEAT_INTERVAL` (10 s) the node
   reports the jobs it runs. When the answer names one of them as cancelled
   (`CancelJob` on the controller), the node cancels that job, which kills its
   `vmaf` process, and reports it as failed, `cancelled by the controller: ...`
   ([ADR-1567](../adr/1567-job-cancel-reaches-node.md)); the log says
   `controller cancelled the job; stopping it`. When the controller answers that it no
   longer knows the session, refuses a call with `PermissionDenied`, or no
   heartbeat has been accepted for 60 s (the controller evicts a node after
   60 s of silence), the node registers again.
3. **Pull.** Each of the `VMAFX_NODE_SLOTS` slots calls `PullWork`. An empty
   answer waits about one `VMAFX_CONTROLLER_POLL_INTERVAL` (2 s, jittered);
   a failed call backs off up to 30 s.
4. **Score.** The job's inputs must resolve, symlinks followed on the node,
   under the scoring roots the controller sends with the job
   ([scoring roots](auth.md#scoring-roots)); otherwise, or when the job
   carries no roots, the job fails with `scoring input "<input>" ... outside
   the tenant's scoring roots` before any file is opened. The real path of a
   local input is what the CLI reads. The job's sources are prepared as described in
   [Job sources](#job-sources-local-paths-urls-and-rclone-remotes), then the
   job runs through the vmaf CLI with `--backend` set to the job's backend, or
   the node's `VMAFX_BACKEND` when the job names none. The CLI then runs that
   backend or fails; it does not pick another one.
5. **Report.** `ReportResult` carries the pooled score and features, or the
   error. A failed report is retried up to 8 times; a request the controller
   rejects as malformed is not retried. A NaN or infinite value is reported
   as a failure that names it, because the controller cannot store it.

Every call has its own deadline, `VMAFX_CONTROLLER_RPC_TIMEOUT` (10 s).

**Backends.** The node advertises exactly one backend, its `VMAFX_BACKEND`:
`cpu`, `cuda`, `hip`, `sycl` or `metal`. The scheduler gives it jobs that
name that backend or none. `auto` cannot be advertised; with the controller
client enabled the node refuses to start on it. A host with GPUs of two
vendors runs one node process per backend.

**Authentication.** When the controller verifies tokens, give the node a
bearer token that carries a tenant claim and the node role, `vmafx:node`,
which the controller requires for the Node API and which reaches nothing else
([roles](auth.md#roles-and-rbac)). A token with `vmafx:admin` no longer
registers a node. Put it in a file and set
`VMAFX_CONTROLLER_TOKEN_FILE`; the node reads the file on every call, so a
rotated Kubernetes projected token or Secret applies without a restart. A JWT
whose `exp` has passed is not sent: the call fails with `controller token file
<path> holds a token that expired at <time>; whatever writes it did not
refresh it`. The operator reads the same variables (`pkg/controllerclient`,
[ADR-1569](../adr/1569-operator-controller-auth.md)).
`VMAFX_CONTROLLER_TOKEN` takes the token inline instead (not both). Set
`VMAFX_CONTROLLER_TLS=true` when the controller serves TLS
(`VMAFX_GRPC_TLS`); `VMAFX_CONTROLLER_CA_FILE` and
`VMAFX_CONTROLLER_SERVER_NAME` adjust the verification. With TLS on, gRPC
never sends the token over a plaintext connection.

**Startup refusals.** The node does not start when the client is enabled
and there is no `vmaf` binary (a node that cannot score must not take jobs),
when `VMAFX_BACKEND` cannot be advertised, or when a controller setting is
malformed: an unparseable duration, a slot count outside 1 to 64, both token
sources, a CA file without TLS, or a CA file that holds no certificate. The
error names the setting.

**Shutdown.** On `SIGTERM` the node stops pulling, lets a running job finish
until the stop deadline, then cancels it and reports it as failed with
`node shutting down, job interrupted`. The controller does not tell a node
that a job was cancelled: the node finishes it and the controller ignores the
late report.

## Job sources: local paths, URLs and rclone remotes

A controller job's `reference` and `distorted` can be:

| Source | Example | How the node reads it |
| --- | --- | --- |
| Local path | `/data/ref.y4m`, `file:///data/ref.y4m` | Directly, in every mode |
| http(s) URL | `https://media.example/ref.y4m` | Streamed, in every mode; no rclone |
| rclone remote | `s3://bucket/ref.y4m`, `rclone://prod:bucket/ref.y4m`, `remote:path/ref.y4m` | Through rclone, as `VMAFX_STORAGE_MODE` says |

The scorer reads Y4M, so a source must be a Y4M clip.

`VMAFX_STORAGE_MODE` picks how rclone remotes are read
([ADR-1526](../adr/1526-node-storage-streamed-inputs.md)):

- **`http-serve`**: the node starts `rclone serve http` for the source's
  directory and streams the file into the vmaf CLI through a pipe. Nothing is
  written to the node's disk, and no FUSE is needed.
- **`mount`**: the node runs `rclone mount` under `VMAFX_STORAGE_MOUNT_ROOT`
  (default: the temp directory) and the CLI reads the file from the mount.
  It needs `/dev/fuse` and `fusermount3` (or `fusermount`); without them the
  node does not start.
- **`auto`** (the default): `mount` when FUSE is usable, else `http-serve`.
  The node logs `storage mode auto resolved` with the mode and the reason.

Any other value stops the node at startup. When either input is streamed (an
http(s) URL or `http-serve`), a stream that breaks fails the job: the CLI
would otherwise score the frames it received, because it treats a short
distorted clip as the end of the clip. Streaming needs a Unix host.

rclone reads its remotes and credentials from `VMAFX_RCLONE_CONFIG` (an
`rclone.conf`), or from its own defaults when unset; `VMAFX_RCLONE_BIN` names
the binary. Without rclone on the node, jobs on rclone remotes fail with the
reason, and the node says so at startup.

### Mount mode in a container {#mount-mode-in-a-container}

The published node image carries rclone, the setuid FUSE helper
`fusermount3` and the util-linux `mount` and `umount` it runs
([ADR-1593](../adr/1593-helm-node-fuse-and-ebpf.md)). `fusermount3` calls
`mount` whenever `/etc/mtab` exists, which Docker creates in every container.
The container needs the FUSE device, and its capability bounding set needs
the two capabilities `fusermount3` mounts with. The node process itself
still runs as UID 65532 with no effective capability:

```bash
docker run --device /dev/fuse \
  --cap-drop ALL --cap-add SYS_ADMIN --cap-add DAC_READ_SEARCH \
  -e VMAFX_STORAGE_MODE=mount \
  ghcr.io/vmafx/vmafx-node:<tag>
```

- `--security-opt no-new-privileges` (Kubernetes
  `allowPrivilegeEscalation: false`) stops the setuid helper; mounts then fail.
- Without `DAC_READ_SEARCH`, `fusermount3` cannot reach the node's per-job
  mount points (mode 0700) and the mount fails with `Permission denied`.
- On an AppArmor host, the runtime's default profile denies `mount`: add
  `--security-opt apparmor=unconfined`, or a profile that allows FUSE mounts.

Kubernetes: set `node.fuse` in the Helm chart ([Kubernetes deployment](#kubernetes-deployment)).

## Configuration (12-factor env vars)

| Variable | Default | Description |
| --- | --- | --- |
| `VMAFX_GRPC_LISTEN` | `:50052` | gRPC listen address for the node's worker service. |
| `VMAFX_FFMPEG_BIN` | `ffmpeg` (PATH) | Path to the `ffmpeg` binary.  The node Docker image sets this to `/usr/local/bin/ffmpeg` (ADR-0717). |
| `VMAFX_VMAF_BINARY` | automatic lookup | Path to the `vmaf` CLI binary used for scoring. |
| `VMAFX_MODEL_DIR` | binary default | Directory containing VMAF model files. The node image sets `/usr/local/share/vmafx/model`. |
| `VMAFX_BACKEND` | `cpu` | Backend the node runs: `cpu`, `cuda`, `hip`, `sycl` or `metal`. Controller jobs pass it to the vmaf CLI as `--backend`. |
| `VMAFX_SIDECAR_SOCKET` | `/tmp/vmafx-sidecar.sock` | Online-training sidecar Unix socket. |
| `VMAFX_CONTROLLER_ADDR` | _(unset)_ | Controller gRPC address; set, the node pulls jobs from the controller. |
| `VMAFX_CONTROLLER_TOKEN_FILE` | _(unset)_ | File holding the bearer token, read on every call. |
| `VMAFX_CONTROLLER_TOKEN` | _(unset)_ | Bearer token given inline. |
| `VMAFX_CONTROLLER_TLS` | `false` | Dial the controller with TLS. |
| `VMAFX_CONTROLLER_CA_FILE` | system roots | PEM bundle that verifies the controller certificate (needs TLS). |
| `VMAFX_CONTROLLER_SERVER_NAME` | from the address | TLS server name override (needs TLS). |
| `VMAFX_CONTROLLER_RPC_TIMEOUT` | `10s` | Deadline of every controller call. |
| `VMAFX_CONTROLLER_HEARTBEAT_INTERVAL` | `10s` | Heartbeat period. |
| `VMAFX_CONTROLLER_POLL_INTERVAL` | `2s` | Wait after an empty `PullWork`. |
| `VMAFX_NODE_ID` | host name | Node name sent to `RegisterNode`. The Helm chart sets the pod name. |
| `VMAFX_NODE_SLOTS` | `1` | Jobs the node runs at once (1 to 64). |
| `VMAFX_STORAGE_MODE` | `auto` | How rclone-remote sources are read: `http-serve`, `mount` or `auto`. |
| `VMAFX_STORAGE_MOUNT_ROOT` | temp directory | Parent of `mount` mode's per-job mount points. |
| `VMAFX_RCLONE_BIN` | `rclone` (PATH) | rclone binary. |
| `VMAFX_RCLONE_CONFIG` | rclone's default | rclone configuration file with the remotes and their credentials. |
| `VMAFX_EBPF_BYPASS` | off | `1` starts the eBPF descriptor tracker; the node stops if it cannot run it ([eBPF tracker](../development/ebpf-fuse-bypass.md)). |
| `VMAFX_EBPF_MOUNT_PREFIX` | `/rclone-mount/` | Path whose opens the tracker records; must contain `VMAFX_STORAGE_MOUNT_ROOT`. |
| `VMAFX_LOG_LEVEL` | `info` | Structured log level: `debug`, `info`, `warn`, `error` |
| `VMAFX_LOG_FORMAT` | `auto` | Log handler: `auto`, `tint`, or `json`. |

See also the [full environment variable reference](../usage/env-vars.md) for the
complete table.

## Backend selection

`VMAFX_BACKEND` defaults to `cpu`; the binary does not probe the host and
silently change backends. The published CPU image also defaults to `cpu`.
Locally built `node-cuda`, `node-rocm`, and `node-sycl` targets set `cuda`,
`hip`, and `sycl` respectively. A requested backend must be present in the
libvmaf build and usable on the host.

## Observability

The node emits structured logs and OpenTelemetry data through the shared
golusoris runtime. It is gRPC-only and does not expose a Prometheus HTTP
listener. The Helm chart probes the TCP listener on the configured gRPC port;
gRPC clients can use the `VmafxScoring/Health` RPC for an application-level
health check.

## Kubernetes deployment

The Helm chart (`deploy/helm/vmafx/`) ships a node worker pool Deployment gated
on `.Values.node.enabled`. With `controller.enabled` the nodes register with
the chart's own controller (`<release>-controller.<namespace>.svc:9090`,
[ADR-1589](../adr/1589-helm-controller-workload.md)); `node.controllerAddr`
points them at another controller instead (its gRPC port). With neither, the
nodes serve direct scoring only. With `networkPolicy.enabled`, the chart also
opens egress from the nodes to the controller's gRPC port: the chart's
controller pods, or for `node.controllerAddr` the pods
`networkPolicy.allow.nodeToController.podSelector` selects on
`nodeToController.port` (9090).

```yaml
# values.yaml
node:
  enabled: true
  replicaCount: 3
  controllerToken:
    secretName: vmafx-node-token   # key "token": a JWT with vmafx:node
  nodeSelector:
    nvidia.com/gpu.present: "true"
  tolerations:
    - key: nvidia.com/gpu
      operator: Exists
      effect: NoSchedule

gpu:
  enabled: true
  vendor: nvidia
  count: 1
```

```bash
helm upgrade --install vmafx deploy/helm/vmafx/ -f values.yaml
```

For a controller that verifies tokens, `node.controllerToken.secretName`
names the Secret holding the node's token (key `node.controllerToken.key`,
default `token`); the chart mounts it read-only and sets
`VMAFX_CONTROLLER_TOKEN_FILE`, which the node re-reads on every call, so
updating the Secret rotates the token.

`storage.mode` becomes `VMAFX_STORAGE_MODE` (chart default `http-serve`;
`mount` and `auto` are the other accepted values) and `storage.mountRoot`
becomes `VMAFX_STORAGE_MOUNT_ROOT`. `storage.rclone.config` holds the
`rclone.conf` contents; the chart mounts it as a Secret at
`/etc/vmafx/rclone.conf` and only then sets `VMAFX_RCLONE_CONFIG`.

`mount` needs FUSE in the pod, which `node.fuse` provides
([ADR-1593](../adr/1593-helm-node-fuse-and-ebpf.md)); the chart refuses
`storage.mode: mount` without it. The pod gets `/dev/fuse` from a FUSE device
plugin, named by its extended resource (with
[squat/generic-device-plugin](https://github.com/squat/generic-device-plugin)
and its default domain, `devic.es/fuse`):

```yaml
storage:
  mode: mount
node:
  enabled: true
  fuse:
    enabled: true
    resourceName: devic.es/fuse
    # appArmorProfile: {type: Unconfined}   # on AppArmor hosts
```

The container keeps UID 65532 and a read-only root file system; its
capability bounding set becomes `SYS_ADMIN` and `DAC_READ_SEARCH` and
`allowPrivilegeEscalation` becomes `true`, both only for the setuid
`fusermount3`. Such a pod no longer meets the Pod Security `baseline` or
`restricted` profile, so its namespace must allow `privileged`. A
`storage.mountRoot` outside `/tmp` gets an `emptyDir`. `node.ebpf` turns on
the eBPF descriptor tracker on top of this
([eBPF tracker](../development/ebpf-fuse-bypass.md#kubernetes)).

## Container images

| Docker target | Published tag | Runtime |
| --- | --- | --- |
| `node-cpu` | `vX.Y.Z` (amd64 + arm64) | distroless Debian 13 |
| `node-cuda` | not yet published | Debian 13 runtime + CUDA libraries copied from the pinned CUDA image |
| `node-rocm` | not yet published | Debian 13 runtime + ROCm libraries copied from the pinned ROCm image |
| `node-sycl` | not yet published | Debian 13 runtime + oneAPI runtime libraries |

The pinned toolkit versions live in `build-config.env` (`CUDA_VERSION`,
`ROCM_VERSION`, `ONEAPI_VERSION`) and are consumed by
`docker/Dockerfile.node`; read them there rather than from this page. The
current release track uses CUDA 13.4.2 and ROCm 10.1.0 libraries, which are
copied out of Ubuntu 26.04 based images, and oneAPI 2026.1.

The release workflow currently publishes only `node-cpu`. All targets use the
same native-architecture FFmpeg dependency collector, so arm64 stages resolve
`aarch64-linux-gnu` libraries rather than copying an amd64-only path.

The release workflow builds each architecture on a native GitHub runner
(amd64 on `ubuntu-latest`, arm64 on `ubuntu-26.04-arm`), then merges the two
into one multi-arch index that is signed, attested and given an SBOM as a whole
([ADR-1349](../adr/1349-native-arch-node-image-build.md)). Built under QEMU
emulation instead, the arm64 half did not finish within two hours.

Building the node from source needs clang for its eBPF object, which is
generated at build time ([node eBPF build guide](../development/node-ebpf-build.md));
the image build below carries it.

Build example:

```bash
docker build -f docker/Dockerfile.node \
  --target node-cuda \
  -t vmafx-node:cuda13 .
```

## Graceful shutdown

On `SIGTERM` the node:

1. Gracefully stops the gRPC server and drains in-flight scoring RPCs.
2. Drains the controller client (when enabled): no new jobs, running jobs
   until the stop deadline, then reports of the jobs it had to cancel.
3. Stops and joins the online-feedback sidecar drainer.
4. Closes the scorer.

## Development

```bash
# Run unit tests.
go test ./pkg/gpu/ ./pkg/ai/ ./cmd/vmafx-node/ -v

# Run the node locally on its default gRPC port.
go run ./cmd/vmafx-node/
```

See also: [ADR-0713](../adr/0713-vmafx-node-impl.md),
[ADR-0709](../adr/0709-vmafx-phase4b-distributed-platform.md),
[ADR-0711](../adr/0711-vmafx-controller-impl.md).
