<!-- markdownlint-disable MD060 -->
# Environment variable reference

This page lists the environment variables the VMAFx code reads at runtime,
grouped by component. All variables are optional unless marked **required**.
To re-check the list against the tree, grep for the names
(`grep -rhoE '"VMAF[A-Z0-9_]*"' core cmd mcp-server ai`).

!!! note "No variable selects the CLI backend"
    The `vmaf` CLI chooses its backend with flags (`--backend`, `--no_cuda`,
    ...), not with an environment variable. The dispatch variables below tune
    how an already selected GPU backend submits work. See
    [cli.md](cli.md#backend-selection).

## Core C library (`libvmaf`)

### Model locations

| Name | Type | Default | Description |
|---|---|---|---|
| `VMAF_TINY_MODEL_DIR` | path | _(unset)_ | Chroot-style search directory for tiny-AI ONNX models. When set, model loading rejects any path that does not start with this prefix. |
| `VMAF_DISTS_SQ_MODEL_PATH` | path | _(auto)_ | Absolute path to the DISTS-SQ ONNX model. |
| `VMAF_FASTDVDNET_PRE_MODEL_PATH` | path | _(auto)_ | Absolute path to the FastDVDNet-Pre ONNX model. |
| `VMAF_LPIPS_MODEL_PATH` | path | _(auto)_ | Absolute path to the LPIPS ONNX model. |
| `VMAF_MOBILESAL_MODEL_PATH` | path | _(auto)_ | Absolute path to the MobileSal saliency ONNX model. |
| `VMAF_TRANSNET_V2_MODEL_PATH` | path | _(auto)_ | Absolute path to the TransNet v2 ONNX model. |

Five per-feature `VMAF_*_MODEL_PATH` variables exist (the five above). Each
works independently of `VMAF_TINY_MODEL_DIR`, and the per-feature variable takes
precedence.

### Feature implementation

| Name | Type | Default | Description |
|---|---|---|---|
| `VMAF_FEATURE_IMPL` | `c` or `rust` | `c` | Which implementation runs a CPU feature extractor. `rust` replaces each extractor that has a Rust twin by it (logged at INFO) and keeps the C extractor, with a WARNING, where none exists; device twins are not affected. Any other value makes feature registration fail. Read once per process. Has an effect only in a build configured with `-Denable_rust_features=true`. The JSON report's `feature_backends` names the extractor that ran (`psnr_rust` for a twin). See [Rust extractor framework](../development/rust-extractor-framework.md) ([ADR-1713](../adr/1713-rc4-rust-extractor-framework.md)). |

### GPU dispatch

| Name | Type | Default | Description |
|---|---|---|---|
| `VMAF_CUDA_DISPATCH` | string | `direct` | CUDA dispatch strategy: `direct` or `graph`, per CUDA extractor. `graph` is accepted but not implemented; it logs a warning and runs `direct`. See [CUDA dispatch](#cuda-dispatch). |
| `VMAF_SYCL_DISPATCH` | string | _(auto)_ | SYCL dispatch strategy: `direct` or `graph`, per feature. See [SYCL dispatch](#sycl-dispatch). |
| `VMAF_SYCL_USE_GRAPH` | string | _(auto)_ | A value starting with `1` forces graph replay globally. Any other value is ignored; it does not force direct dispatch. |
| `VMAF_SYCL_NO_GRAPH` | string | _(unset)_ | **Deprecated.** `1` forces direct dispatch. Prints a deprecation warning when set; removal is scheduled for v4.0. `VMAF_SYCL_USE_GRAPH=1` takes precedence. |

### SYCL diagnostics and tuning

| Name | Type | Default | Description |
|---|---|---|---|
| `VMAF_SYCL_PROFILE` | `1` | off | Enable SYCL kernel profiling through the queue's `enable_profiling` property. |
| `VMAF_SYCL_TIMING` | `1` | off | Print per-extractor wall-clock timing to stderr. |
| `VMAF_SYCL_IMPORT_DEBUG` | `1` | off | Log the addresses of the shared import buffers, to check they are not aliased. |
| `VMAF_SYCL_CHECKSUM` | `1` | off | Log a CRC of each imported ref / dis device buffer per frame, to localise import corruption. |
| `VMAF_SYCL_SCRATCH_SELFTEST` | `0` | on | Set to `0` to skip the first-use scratch-memory self-test of the SYCL device ([ADR-1395](../adr/1395-sycl-kernels-no-scratch.md)). |

## Dispatch strategy syntax

The two `*_DISPATCH` variables share one grammar
([ADR-0483](../adr/0483-gpu-dispatch-parse-dedup.md)): a comma-separated list
of `feature:strategy` tokens, matched case-sensitively. A bare strategy name
such as `VMAF_CUDA_DISPATCH=graph` matches no feature and has no effect. The
first token naming a feature wins, and a token with an unknown strategy is
skipped.

### CUDA dispatch

libvmaf reads `VMAF_CUDA_DISPATCH` when a CUDA extractor initialises, and the
feature name of a token is the extractor's registered name (`vif_cuda`,
`float_ssim_cuda`, ...).

| Value | Behaviour |
|---|---|
| `direct` | Submit directly. **Default** for every extractor. |
| `graph` | Accepted but not implemented: logs `CUDA graph dispatch requested ... not implemented; falling back to direct` and runs `direct`. |

```bash
VMAF_CUDA_DISPATCH=vif_cuda:graph,float_ssim_cuda:direct ./build/tools/vmaf --backend cuda ...
```

### SYCL dispatch

| Value | Behaviour |
|---|---|
| `direct` | Submit kernels directly to an in-order queue (no graph). Lower per-frame overhead at small resolutions. |
| `graph` | Use SYCL graph replay. Reduces kernel-launch overhead at 720p and above. |

The strategy is resolved in this order, first match wins:

1. `VMAF_SYCL_DISPATCH` for the feature.
2. `VMAF_SYCL_USE_GRAPH=1`, which selects `graph`.
3. `VMAF_SYCL_NO_GRAPH=1`, which selects `direct` (deprecated).
4. The zero-copy VA-surface import path, which selects `direct`
   ([ADR-1121](../adr/1121-sycl-qsv-zerocopy-p010-normalization.md)): the
   combined graph
   is a throughput loss there. Steps 1 and 2 still force `graph`.
5. The feature's own dispatch hint, if it declares one.
6. An area threshold: `graph` at 1280 x 720 pixels or more, `direct` below.

When `VMAF_SYCL_NO_GRAPH` is set, libvmaf prints:

```text
VMAF_SYCL_NO_GRAPH deprecated; use VMAF_SYCL_USE_GRAPH=false. Will be removed in v4.0.
```

That message is misleading: `VMAF_SYCL_USE_GRAPH=false` changes nothing. To
force direct dispatch use `VMAF_SYCL_DISPATCH=<feature>:direct` or the
deprecated `VMAF_SYCL_NO_GRAPH=1`.

### HIP dispatch

The HIP backend has one submission path and reads no dispatch variable.
`VMAF_HIP_DISPATCH`, listed here before, was read by a function nothing
called and has been removed
([ADR-1571](../adr/1571-gpu-dispatch-env-consulted.md)).

## Python harness (`compat/python-vmaf`)

| Name | Type | Default | Description |
|---|---|---|---|
| `VMAF_FORCE_BACKEND` | string | _(none)_ | Scoring backend (`cuda`, `sycl`, `cpu`, ...) for harness runs that call the `vmaf` CLI; appends `--backend <value>`. An explicit `backend` option wins. |
| `VMAF_BACKEND` | string | _(none)_ | Fallback alias for `VMAF_FORCE_BACKEND`. |
| `VMAF_BUILD_DIR` | path | `core/build` | Build directory whose `tools/vmaf` the harness runs. |
| `VMAF_PATH` | path | _(none)_ | External `vmaf` path used instead of the in-tree build. |
| `VMAF_WORKSPACE` | path | `compat/python-vmaf/workspace` | Where the harness reads and writes working files. |
| `VMAF_RESOURCE` | path | `compat/python-vmaf/resource` | Resource (test clip) root. |

`FFMPEG_PATH` and `MATLAB_PATH` are not environment variables: the harness reads
them from an optional `externals.py` module ([python.md](python.md)).

## AI scripts (`ai/scripts/`)

See also [ai/scripts-env-vars.md](../ai/scripts-env-vars.md) for per-corpus
overrides not listed here.

| Name | Type | Default | Description |
|---|---|---|---|
| `VMAF_BIN` | path | `core/build-cpu/tools/vmaf` | Path to the `vmaf` CLI binary used by Python AI scripts and feature-extraction pipelines. |
| `VMAF_MODEL_PATH` | path | _(default model)_ | File path override for the teacher model the AI scoring helpers use; an explicit model argument wins. |
| `VMAF_BVI_DVC_RAW_DIR` | path | `<repo>/.corpus/bvi-dvc-raw` | Root of the raw BVI-DVC dataset for `train_predictor_v2_realcorpus.py`. |
| `VMAF_CHUG_DIR` | path | `<repo>/.corpus/chug` | Root of the CHUG shard tree used by `chug_extract_features.py` and `chug_to_corpus_jsonl.py`. |
| `VMAF_CHUG_OUTPUT_DIR` | path | `<repo>/.corpus/chug` | Output directory of `train_chug_hdr_mos_head.py`. |
| `VMAF_CORPUS_DIR` | path | `<repo>/.corpus/netflix` | Corpus root for `calibrate_nr_threshold.py`. |
| `VMAF_DATA_ROOT` | path | `~/datasets` | Parent directory for datasets whose sub-paths are not otherwise overridden (for example `$VMAF_DATA_ROOT/konvid-1k`). |
| `VMAF_HW_TAG` | string | `ryzen-9950x3d+rtx4090+arc-a380` | Hardware identifier stamped into benchmark artefacts by `measure_quant_drop_per_ep.py`. |
| `VMAF_KONVID_1K_DIR` | path | `$VMAF_DATA_ROOT/konvid-1k` | Root of the KonViD-1k dataset. |
| `VMAF_KONVID_150K_DIR` | path | `<repo>/.corpus/konvid-150k` | Root of the KonViD-150k dataset used by `extract_k150k_features.py`, `konvid_150k_to_corpus_jsonl.py` and `train_konvid_mos_head.py`. |
| `VMAF_NETFLIX_CORPUS_DIR` | path | `<repo>/.corpus/netflix` | Root of the Netflix internal MOS corpus (non-public). |
| `VMAF_TINY_AI_CACHE` | path | `~/.cache/vmaf-tiny-ai` | Cache of per-pair scores reused by the Netflix and KonViD loaders. |
| `VMAF_TINY_AI_SCRATCH` | path | system temp dir | Scratch root for dataset extraction and ONNX export scripts. Must be an absolute path. |
| `VMAFX_RUNS_DIR` | path | `<repo>/runs` | Output root for training and quantisation metrics. |

## `vmaf-tune`

| Name | Type | Default | Description |
|---|---|---|---|
| `VMAFTUNE_WORKDIR` | path | OS temp dir | Parent directory for the scratch tree of a per-shot run, used when `--workdir` is not given and the directory is writable. |
| `VMAFTUNE_VAAPI_DEVICE` | path | _(driver default)_ | VAAPI render node injected into hardware-encoder init chains. |
| `VMAFTUNE_SALIENCY_FALLBACK_OK` | `1` | off | Equivalent to `--saliency-fallback-plain`: an encoder with no saliency path runs a plain encode instead of exiting 2. |

## MCP server

The Python MCP server (`mcp-server/vmaf-mcp`) and the Go server
(`cmd/vmafx-mcp`) share the scoring-related variables.

| Name | Type | Default | Description |
|---|---|---|---|
| `VMAF_MCP_ALLOW` | colon-separated paths | _(built-in roots)_ | Additional filesystem roots the MCP server may read YUV files from. Paths outside every allowed root are rejected. |
| `VMAF_MCP_ASYNC` | string | `asyncio` | anyio backend for the Python server. `1`, `true`, `yes` or `trio` select Trio; any other non-empty name is passed to anyio as the backend. |
| `VMAF_MCP_MAX_CONCURRENT` | integer | `8` | Maximum simultaneous scoring calls in the Python server. |
| `VMAF_MCP_SUBPROCESS_TIMEOUT_S` | seconds | `600` | Timeout of each subprocess the Python server runs; a non-positive or malformed value falls back to the default. |
| `VMAF_ROOT` | path | _(auto-detect)_ | Repo root used by the MCP servers to find the test clips and tools. |
| `VMAF_TUNE_BIN` | path | _(PATH lookup)_ | Path to the `vmaf-tune` binary used by MCP tuning tools. |
| `VMAF_PER_SHOT_BIN`, `VMAF_ROI_BIN`, `VMAF_BENCH_BIN`, `VMAF_VPL_BIN` | path | _(next to `vmaf`)_ | Override the binary behind the `vmaf-perShot`, `vmaf_roi`, `vmaf_bench` and `vmaf_vpl` MCP tools. |
| `VMAFX_MCP_DIRECT` | `1` | off | Go server only: score through libvmaf by cgo instead of a `vmaf` subprocess ([ADR-0931](../adr/0931-mcp-cgo-direct-replace-subprocess.md)). Any other value keeps the subprocess path. |

### MCP HTTP transport (`vmaf-mcp --transport http`)

| Name | Type | Default | Description |
|---|---|---|---|
| `VMAFX_MCP_HTTP_BIND` | address | `127.0.0.1` | Listen address. Set `0.0.0.0` to listen on all interfaces ([ADR-0967](../adr/0967-mcp-http-transport-security-hardening.md)). |
| `VMAFX_MCP_HTTP_TOKEN` | string | _(unset)_ | Bearer token required in `Authorization: Bearer <token>`. If unset and `VMAFX_MCP_HTTP_NO_AUTH` is also unset, every request gets 401. |
| `VMAFX_MCP_HTTP_NO_AUTH` | `1` | off | Disable authentication; logs a warning on start. |
| `VMAFX_MCP_HTTP_TLS_CERT` | path | _(unset)_ | PEM certificate. With `VMAFX_MCP_HTTP_TLS_KEY` it enables TLS; without them the server logs a warning and serves plain HTTP. |
| `VMAFX_MCP_HTTP_TLS_KEY` | path | _(unset)_ | PEM private key. |

The HTTP transport also reads `VMAFX_PORT` (listen port, default 8080,
overridden
by `--port`), `VMAFX_LOG_LEVEL`, `VMAFX_VMAF_BINARY` (same as `VMAF_BIN`) and
`VMAFX_MODEL_DIR` (extra model search root).

### Go MCP client of the controller and server

| Name | Type | Default | Description |
|---|---|---|---|
| `VMAFX_CONTROLLER_ADDR` | `host:port` | `localhost:9090` | gRPC address of the controller. |
| `VMAFX_SERVER_ADDR` | `host:port` | `localhost:9090` | gRPC address of `vmafx-server`. |
| `VMAFX_CONTROLLER_TOKEN` | string | _(unset)_ | Bearer token attached to every controller RPC. |
| `VMAFX_GRPC_TIMEOUT` | seconds | `30` | Per-RPC deadline; a malformed or non-positive value falls back to the default. |

## Go services (golusoris)

The controller, server and node run on the
[golusoris](https://github.com/golusoris/golusoris) `fx` framework
([ADR-1119](../adr/1119-golusoris-go-framework-adoption.md)). Configuration is
loaded with koanf: the `VMAFX_` prefix is stripped, the name is lowercased, and
every underscore becomes the `.` key delimiter, so the table columns show the
resulting key. The HTTP and gRPC modules own the listeners, and listen
addresses are full addresses (`:8080`), not bare ports.

!!! warning "ADR-1119 migration"
    The pre-fx services read bare port numbers and removed names:
    `VMAFX_PORT` and `VMAFX_GRPC_PORT` (controller and server), and
    `VMAFX_NODE_ADDR` (node). Use `VMAFX_HTTP_ADDR`, `VMAFX_GRPC_LISTEN` and,
    for
    the node, `VMAFX_GRPC_LISTEN` with a full address. The golusoris defaults
    (`:8080`, `:9090`) apply, the legacy wire ports (`8080`, `50051`) are not
    carried, and the controller's auth and JWKS CLI flags are gone; configure
    them through the variables below. Operators must migrate.

### Controller (`cmd/vmafx-controller`)

| Name | Type | Default | golusoris key | Description |
|---|---|---|---|---|
| `VMAFX_HTTP_ADDR` | `host:port` | `:8080` | `http.addr` | HTTP listen address (serves `/healthz`, `/readyz`, `/metrics`, `/v1/score`). |
| `VMAFX_GRPC_LISTEN` | `host:port` | `:9090` | `grpc.listen` | gRPC listen address (serves both `VmafxScoring` and `VmafxController`). |
| `VMAFX_DB_PATH` | path | `vmafx-controller.db` | `db.path` | Embedded SQLite job and node-persistence database (kept, not migrated to golusoris.Jobs). |
| `VMAFX_LOG_LEVEL` | string | `INFO` | `log.level` | `DEBUG`, `INFO`, `WARN` or `ERROR`. |
| `VMAFX_MODEL_DIR` | path | _(none)_ | `model.dir` | Directory of VMAF `.json` model files passed to the libvmaf scorer. |
| `VMAFX_VMAF_BINARY` | path | _(PATH lookup)_ | `vmaf.binary` | Path to the `vmaf` CLI binary. |
| `VMAFX_AUTH_DISABLED` | bool | `false` | `auth.disabled` | Disable JWT auth (dev and internal only, never in production). A synthetic `dev` tenant with the admin role is injected. |
| `VMAFX_JWKS_ENDPOINT` | URL | _(none)_ | `jwks.endpoint` | JWKS endpoint for RS256 verification, for example `https://idp.example.com/.well-known/jwks.json`. Required unless auth is disabled or a tenant source is set (then refused). |
| `VMAFX_AUTH_ISSUER` | string | _(none)_ | `auth.issuer` | Expected JWT `iss` claim. Required unless auth is disabled or a tenant source is set (then refused). |
| `VMAFX_AUTH_AUDIENCE` | string | _(none)_ | `auth.audience` | Expected JWT `aud` claim; the check is skipped when empty. |
| `VMAFX_AUTH_TENANT_CLAIM` | string | `tid` | `auth.tenant_claim` | JWT claim carrying the tenant id. A golusoris CompoundKey, so its underscore is preserved. |
| `VMAFX_AUTH_ROLES_CLAIM` | string | `vmafx_roles` | `auth.roles_claim` | JWT claim carrying the roles list. A CompoundKey. |
| `VMAFX_AUTH_TENANTS_SOURCE` | string | _(none)_ | `auth.tenants.source` | `kubernetes` (the namespace's `VmafxTenant` resources) or `file`: verify tokens per tenant ([tenant registry](../server/auth.md#tenant-registry)). Excludes the five global provider variables above and `VMAFX_AUTH_DISABLED=true`. |
| `VMAFX_AUTH_TENANTS_FILE` | path | _(none)_ | `auth.tenants.file` | YAML or JSON file of `VmafxTenant` documents (source `file`). |
| `VMAFX_AUTH_TENANTS_NAMESPACE` | string | pod namespace | `auth.tenants.namespace` | Namespace of the `VmafxTenant` resources (source `kubernetes`). |
| `VMAFX_SCORING_ROOTS` | list | _(none: every input refused)_ | `scoring.roots` | Comma-separated scoring roots of every caller without a tenant registry; `{tenant}` becomes the caller's tenant ID ([scoring roots](../server/auth.md#scoring-roots)). |
| `VMAFX_AUTH_TENANTS_REFRESH` | duration | `30s` | `auth.tenants.refresh` | Re-read interval of the tenant source, `1s` to `1h`; the tenant set is refused after ten intervals without a successful read. |

### Server (`cmd/vmafx-server`)

| Name | Type | Default | Description |
|---|---|---|---|
| `VMAFX_HTTP_ADDR` | `host:port` | `:8080` | HTTP listen address (serves `/healthz`, `/livez`, `/readyz`, `/startupz`, `/metrics`, `/v1/score`, `/v1/health`, `/v1/ready`, `/swagger`). Key `http.addr`. |
| `VMAFX_GRPC_LISTEN` | `host:port` | `:9090` | gRPC listen address (`VmafxScoring`). Key `grpc.listen`. The pre-fx default was `:50051`. |
| `VMAFX_GRPC_CERT_FILE`, `VMAFX_GRPC_KEY_FILE` | path | _(unset)_ | TLS certificate and key for the gRPC listener. |
| `VMAFX_GRPC_MAX_RECV_SIZE`, `VMAFX_GRPC_MAX_SEND_SIZE` | bytes | _(gRPC default)_ | gRPC message size limits. |
| `VMAFX_LOG_LEVEL` | string | `INFO` | `DEBUG`, `INFO`, `WARN` or `ERROR`. |
| `VMAFX_MODEL_DIR` | path | _(none)_ | Model directory passed to the libvmaf scorer. The server's koanf key is `model.dir`, as for the controller and node. |
| `VMAFX_VMAF_BINARY` | path | _(PATH lookup)_ | Path to the `vmaf` CLI binary. Key `vmaf.binary`. |
| `VMAFX_MAX_CONCURRENT_SCORES` | integer | _(NumCPU)_ | Cap on simultaneous `Score` calls; excess requests get HTTP 429 or gRPC `ResourceExhausted`. |
| `VMAFX_SWAGGER_TRY_IT_OUT` | string | _(off)_ | Set to `1` to enable the Swagger UI "try it out" live-execution path. |

### Worker node (`cmd/vmafx-node`)

| Name | koanf key | Type | Default | Description |
|---|---|---|---|---|
| `VMAFX_GRPC_LISTEN` | `grpc.listen` | `host:port` | `:50052` | gRPC listen address of the node's `VmafxScoring` service. Replaces `VMAFX_NODE_ADDR`; the historical `:50052` default is kept. |
| `VMAFX_GRPC_CERT_FILE`, `VMAFX_GRPC_KEY_FILE`, `VMAFX_GRPC_MAX_RECV_SIZE`, `VMAFX_GRPC_MAX_SEND_SIZE` | `grpc.*` | | _(unset)_ | The same gRPC TLS and size settings as the server. |
| `VMAFX_FFMPEG_BIN` | `ffmpeg.bin` | path | `ffmpeg` (PATH) | `ffmpeg` binary used by the startup encoder probe. The node image sets `/usr/local/bin/ffmpeg` ([ADR-0717](../adr/0717-vmafx-node-ffmpeg-latest.md)). |
| `VMAFX_VMAF_BINARY` | `vmaf.binary` | path | _(FindBinary lookup)_ | `vmaf` CLI binary behind the unary `Score` RPC. |
| `VMAFX_MODEL_DIR` | `model.dir` | path | _(binary default)_ | Directory of VMAF `.json` model files. |
| `VMAFX_BACKEND` | `backend` | string | `cpu` | Backend the node runs (`cpu`, `cuda`, `hip`, `sycl`, `metal`); advertised to the controller and passed to the vmaf CLI as `--backend` for controller jobs. |
| `VMAFX_SIDECAR_SOCKET` | `sidecar.socket` | path | `/tmp/vmafx-sidecar.sock` | Unix socket of the online-training sidecar ([ADR-0781](../adr/0781-sidecar-sgd-ema-online-trainer.md)). |
| `VMAFX_CONTROLLER_ADDR` | `controller.addr` | `host:port` | _(unset)_ | Controller gRPC address; set, the node registers and pulls jobs ([ADR-1524](../adr/1524-vmafx-node-controller-client.md)). |
| `VMAFX_CONTROLLER_TOKEN_FILE` / `VMAFX_CONTROLLER_TOKEN` | `controller.token_file` / `controller.token` | path / string | _(unset)_ | Bearer token for the controller, from a file read on every call or inline (not both); an expired JWT is not sent. |
| `VMAFX_CONTROLLER_TLS`, `VMAFX_CONTROLLER_CA_FILE`, `VMAFX_CONTROLLER_SERVER_NAME` | `controller.tls`, `controller.ca_file`, `controller.server_name` | bool, path, string | `false`, system roots, from the address | TLS to the controller; the CA file and server name need TLS. |
| `VMAFX_CONTROLLER_RPC_TIMEOUT`, `VMAFX_CONTROLLER_HEARTBEAT_INTERVAL`, `VMAFX_CONTROLLER_POLL_INTERVAL` | `controller.rpc_timeout`, `controller.heartbeat_interval`, `controller.poll_interval` | duration | `10s`, `10s`, `2s` | Per-call deadline, heartbeat period, wait after an empty `PullWork`. |
| `VMAFX_NODE_ID` | `node.id` | string | host name | Node name sent to `RegisterNode`. |
| `VMAFX_NODE_SLOTS` | `node.slots` | integer | `1` | Controller jobs run at once (1 to 64). |
| `VMAFX_STORAGE_MODE` | `storage.mode` | string | `auto` | How rclone-remote job sources are read: `http-serve`, `mount` or `auto`; anything else stops the node ([ADR-1526](../adr/1526-node-storage-streamed-inputs.md)). |
| `VMAFX_STORAGE_MOUNT_ROOT` | `storage.mount_root` | path | temp directory | Parent of `mount` mode's per-job mount points. |
| `VMAFX_RCLONE_BIN`, `VMAFX_RCLONE_CONFIG` | `rclone.bin`, `rclone.config` | path | `rclone`, rclone's default | rclone binary and configuration file. |
| `VMAFX_EBPF_BYPASS` | `ebpf.bypass` | bool | off | Starts the eBPF descriptor tracker; a host that cannot run it stops the node ([ADR-1539](../adr/1539-node-ebpf-tracker-wiring.md)). |
| `VMAFX_EBPF_MOUNT_PREFIX` | `ebpf.mount_prefix` | path | `/rclone-mount/` | Mount prefix the tracker watches; must contain `VMAFX_STORAGE_MOUNT_ROOT`. |
| `VMAFX_LOG_LEVEL` | `log.level` | string | `info` | Structured log level. |
| `VMAFX_LOG_FORMAT` | `log.format` | string | `auto` | `auto` (tint on a TTY, else JSON), `tint` or `json`. |

The node is gRPC-only. Its Kubernetes probe is the `VmafxScoring/Health` RPC;
there is no HTTP `/livez` or `/readyz` until `bootstrap.HTTP` joins the node
graph.

### Operator (`cmd/vmafx-operator`)

| Name | Type | Default | Description |
|---|---|---|---|
| `VMAFX_OPERATOR_LEADER_ELECTION` | `true`/`false` | `false` | Enable leader election; set `true` when running several operator replicas. |
| `VMAFX_OPERATOR_LEADER_ELECTION_ID` | string | `vmafx-operator.vmafx.dev` | Lease name used when leader election is enabled. |
| `VMAFX_OPERATOR_METRICS_ADDR` | `host:port` | `:8080` | Bind address of the Prometheus metrics endpoint. |
| `VMAFX_OPERATOR_HEALTH_PROBE_ADDR` | `host:port` | `:8081` | Bind address of `/healthz` and `/readyz`. |
| `VMAFX_OPERATOR_GRACEFUL_SHUTDOWN` | duration | `30s` | Manager graceful-shutdown timeout. |
| `VMAFX_OPERATOR_WEBHOOK_PORT` | integer | `0` | Admission-webhook port; `0` disables webhooks. |
| `VMAFX_OPERATOR_WEBHOOK_HOST` | host | _(all interfaces)_ | Admission-webhook bind host. |
| `VMAFX_LOG_LEVEL` | string | `info` | Shared log level: `debug`, `info`, `warn` or `error`. |
| `VMAFX_CONTROLLER_GRPC_ADDR` | `host:port` | `vmafx-controller.<ns>.svc.cluster.local:9090` | Controller gRPC address used by job reconciliation. |
| `VMAFX_CONTROLLER_HTTP_ADDR` | URL | `http://vmafx-controller.<ns>.svc.cluster.local:8080` | Controller HTTP address used by health reconciliation. |
| `VMAFX_CONTROLLER_TOKEN_FILE` / `VMAFX_CONTROLLER_TOKEN` | path / string | _(unset)_ | Bearer token job reconciliation sends to the controller, from a file read on every call or inline (not both); an expired JWT is not sent. |
| `VMAFX_CONTROLLER_TLS` / `VMAFX_CONTROLLER_CA_FILE` / `VMAFX_CONTROLLER_SERVER_NAME` | `true`/`false` / path / host | `false` / _(unset)_ | TLS to the controller; the CA file and server name need TLS. Same keys as the node's. |

### OpenTelemetry identity (all Go services)

| Name | Type | Default | Description |
|---|---|---|---|
| `VMAFX_OTEL_SERVICE_NAME` | string | the standard `OTEL_SERVICE_NAME`, else the binary name | `service.name` resource attribute. |
| `VMAFX_OTEL_SERVICE_VERSION` | string | the build version | `service.version` resource attribute. |

## Online-training sidecar (`ai/sidecar`)

The sidecar is the Python online trainer that the node feeds over a Unix
socket.

| Name | Type | Default | Description |
|---|---|---|---|
| `VMAFX_SIDECAR_SOCKET` | path | `/tmp/vmafx-sidecar.sock` | Unix socket the sidecar listens on; must match the node's setting. |
| `VMAFX_SIDECAR_CHECKPOINT_DIR` | path | `/mnt/vmafx-models/online` | Checkpoint directory. |
| `VMAFX_SIDECAR_REPLAY_CAPACITY` | integer | `10000` | Replay-buffer capacity. |
| `VMAFX_SIDECAR_PENDING_CAPACITY` | integer | `10000` | Pending-sample queue capacity. |
| `VMAFX_SIDECAR_BATCH_SIZE` | integer | `32` | Training batch size. |
| `VMAFX_SIDECAR_REPLAY_MIX` | float | `0.5` | Replay mix ratio, in the range [0, 1). |
| `VMAFX_SIDECAR_LR` | float | `0.0001` | Learning rate. |
| `VMAFX_SIDECAR_EMA_DECAY` | float | `0.999` | EMA decay of the served weights. |
| `VMAFX_SIDECAR_CKPT_INTERVAL_S` | seconds | `600` | Interval between checkpoints. |
| `VMAFX_SIDECAR_MIN_SAMPLES_CKPT` | integer | `1000` | Minimum new samples before a checkpoint is written. |
| `VMAFX_SIDECAR_N_FEATURES` | integer | `80` | Input feature count of the model (also of the fallback model when no base model loads). |

## Test-only

These variables are read by test harnesses and CI gates, not by the shipped
tools.

| Name | Type | Default | Description |
|---|---|---|---|
| `VMAF_BIN_FOR_TESTS` | path | _(auto-detect)_ | Explicit path to the `vmaf` binary for integration tests (`test_chug_extract_features_smoke.py`, the ADR-0543 backend-enforcement tests). When unset, the tests probe the canonical build paths and `PATH`. |
| `VMAF_TEST_DATA` | path | _(repo-relative)_ | Test-data root override for Python-harness tests. |
| `VMAF_SYCL_AOT_JOBS` | integer | `4` | Parallel compiles of the `sycl-aot` meson test suite. |

## Former section names

Older pages and records link to these headings; each points to the section
that now holds its content.

### CUDA dispatch knob

Now under [CUDA dispatch](#cuda-dispatch).

### SYCL dispatch knob

Now under [SYCL dispatch](#sycl-dispatch).

## History

- **ADR-1119 migration.** The Go services moved to the golusoris `fx`
  framework; see the migration warning above for the renamed variables.
