<!-- markdownlint-disable MD013 MD060 -->
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
(`cmd/vmafx-mcp`) share the scoring-related variables. The Go server's own
variables are in its [generated table](#mcp-server-cmdvmafx-mcp).

| Name | Type | Default | Description |
|---|---|---|---|
| `VMAF_MCP_ALLOW` | colon-separated paths | _(built-in roots)_ | Additional filesystem roots the MCP server may read YUV files from. Paths outside every allowed root are rejected. |
| `VMAF_MCP_ASYNC` | string | `asyncio` | anyio backend for the Python server. `1`, `true`, `yes` or `trio` select Trio; any other non-empty name is passed to anyio as the backend. |
| `VMAF_MCP_MAX_CONCURRENT` | integer | `8` | Maximum simultaneous scoring calls in the Python server. |
| `VMAF_MCP_SUBPROCESS_TIMEOUT_S` | seconds | `600` | Timeout of each subprocess the Python server runs; a non-positive or malformed value falls back to the default. |
| `VMAF_ROOT` | path | _(auto-detect)_ | Repo root used by the MCP servers to find the test clips and tools. |
| `VMAF_TUNE_BIN` | path | _(PATH lookup)_ | Path to the `vmaf-tune` binary used by MCP tuning tools. |
| `VMAF_PER_SHOT_BIN`, `VMAF_ROI_BIN`, `VMAF_BENCH_BIN`, `VMAF_VPL_BIN` | path | _(next to `vmaf`)_ | Override the binary behind the `vmaf-perShot`, `vmaf_roi`, `vmaf_bench` and `vmaf_vpl` MCP tools. |

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

<!-- BEGIN GENERATED: vmafx-api environment vmafx-controller (scripts/codegen/vmafx-api.py) -->

| Variable | Key | Type | Default | Chart value | Description |
|---|---|---|---|---|---|
| `VMAFX_HTTP_ADDR` | `http.addr` | `host:port` | `:8080` | `controller.httpPort` | HTTP listen address of `/healthz`, `/readyz`, `/metrics` and `POST /v1/score`, a full address. |
| `VMAFX_HTTP_TIMEOUTS_READ` | `http.timeouts.read` | duration | `30s` |  | Deadline for reading a whole request; `0` keeps the framework default. |
| `VMAFX_HTTP_TIMEOUTS_HEADER` | `http.timeouts.header` | duration | `5s` |  | Deadline for reading the request headers (slow-client guard); `0` keeps the framework default. |
| `VMAFX_HTTP_TIMEOUTS_WRITE` | `http.timeouts.write` | duration | `60s` |  | Deadline for writing a response; `0` keeps the framework default. |
| `VMAFX_HTTP_TIMEOUTS_IDLE` | `http.timeouts.idle` | duration | `120s` |  | Keep-alive idle timeout; `0` keeps the framework default. |
| `VMAFX_HTTP_TIMEOUTS_SHUTDOWN` | `http.timeouts.shutdown` | duration | `30s` |  | Drain time of the HTTP server at shutdown; `0` keeps the framework default. |
| `VMAFX_HTTP_LIMITS_HEADER` | `http.limits.header` | bytes | `1048576` |  | Largest request header block; `0` keeps the default. |
| `VMAFX_HTTP_LIMITS_BODY` | `http.limits.body` | bytes | `10485760` |  | Largest request body; `0` keeps the default. `VMAFX_HTTP_LIMITS_UNLIMITED` removes the cap. |
| `VMAFX_HTTP_LIMITS_UNLIMITED` | `http.limits.unlimited` | bool | `false` |  | Serve request bodies of any size; `VMAFX_HTTP_LIMITS_BODY` is then ignored. |
| `VMAFX_GRPC_LISTEN` | `grpc.listen` | `host:port` | `:9090` | `controller.grpcPort` | gRPC listen address of `VmafxScoring` and `VmafxController`, a full address. |
| `VMAFX_GRPC_TLS` | `grpc.tls` | bool | `false` |  | Serve gRPC over TLS; needs `VMAFX_GRPC_CERT_FILE` and `VMAFX_GRPC_KEY_FILE`. |
| `VMAFX_GRPC_CERT_FILE` | `grpc.cert_file` | path | _(unset)_ |  | PEM certificate of the gRPC listener (with `VMAFX_GRPC_TLS`). |
| `VMAFX_GRPC_KEY_FILE` | `grpc.key_file` | path | _(unset)_ |  | PEM private key of the gRPC listener (with `VMAFX_GRPC_TLS`). |
| `VMAFX_GRPC_MAX_RECV_SIZE` | `grpc.max_recv_size` | bytes | `4194304` |  | Largest gRPC message received; `0` keeps the gRPC default. |
| `VMAFX_GRPC_MAX_SEND_SIZE` | `grpc.max_send_size` | bytes | `4194304` |  | Largest gRPC message sent; `0` keeps the gRPC default. |
| `VMAFX_VMAF_BINARY` | `vmaf.binary` | path | `vmaf` on `PATH` |  | Path of the `vmaf` CLI behind the scorer; a missing binary stops the program at startup. |
| `VMAFX_MODEL_DIR` | `model.dir` | path | _(unset)_ |  | Directory of the VMAF `.json` models the scorer loads. |
| `VMAFX_STORE_BACKEND` | `store.backend` | string | `sqlite` | set by the chart | Where jobs and node sessions live: `sqlite` or `postgres` ([job persistence](../server/controller.md#job-persistence)); anything else stops the controller. |
| `VMAFX_DB_PATH` | `db.path` | path | `vmafx/vmafx-controller.db` under the user state directory | set by the chart | SQLite job database (`sqlite`). Unset, the controller uses `$XDG_STATE_HOME` (else `~/.local/state`) on Linux and the BSDs and the user configuration directory on macOS and Windows; it never writes to the working directory. The image and the chart set `/data/vmafx-controller.db`. |
| `XDG_STATE_HOME` | read directly | path | `~/.local/state` |  | Base of the default `VMAFX_DB_PATH` on Linux and the BSDs; a relative value is ignored. |
| `VMAFX_DB_DSN` | `db.dsn` | string | _(unset)_ | `controller.store.postgresql.mode`, `controller.store.postgresql.external.secretName`, `controller.store.postgresql.external.secretKey` | Secret. PostgreSQL connection string; required with `postgres` and by `vmafx-controller migrate` and `import-sqlite`. The standard `PG*` variables fill what it leaves out. |
| `VMAFX_STORE_LEASE_TTL` | `store.lease_ttl` | duration | `60s` | `controller.store.leaseTTL` | Lease of a pulled job (`postgres`); a negative value stops the controller. |
| `VMAFX_STORE_SESSION_TTL` | `store.session_ttl` | duration | `60s` | `controller.store.sessionTTL` | Lifetime of a node session without a heartbeat (`postgres`). |
| `VMAFX_STORE_SWEEP_INTERVAL` | `store.sweep_interval` | duration | `5s` | `controller.store.sweepInterval` | Period of the lease sweep (`postgres`). |
| `VMAFX_STORE_BACKOFF_BASE` | `store.backoff_base` | duration | `5s` | `controller.store.backoffBase` | Delay before a job whose lease expired is handed out again (`postgres`); doubles up to `VMAFX_STORE_BACKOFF_MAX`. |
| `VMAFX_STORE_BACKOFF_MAX` | `store.backoff_max` | duration | `5m` | `controller.store.backoffMax` | Cap of that delay (`postgres`). |
| `VMAFX_AUTH_DISABLED` | `auth.disabled` | bool | `false` | `auth.disabled` | Turn token verification off (development only): every call acts as tenant `dev` with the admin role. Refused with a tenant source. |
| `VMAFX_JWKS_ENDPOINT` | `jwks.endpoint` | URL | _(unset)_ | `auth.jwksEndpoint` | JWKS endpoint of the identity provider. Required unless auth is disabled or a tenant source is set (then refused) ([auth](../server/auth.md)). |
| `VMAFX_AUTH_ISSUER` | `auth.issuer` | string | _(unset)_ | `auth.issuer` | Expected `iss` claim; required like `VMAFX_JWKS_ENDPOINT`. |
| `VMAFX_AUTH_AUDIENCE` | `auth.audience` | string | _(unset)_ | `auth.audience` | Expected `aud` claim; empty skips the check. Refused with a tenant source. |
| `VMAFX_AUTH_TENANT_CLAIM` | `auth.tenant_claim` | string | `tid` | `auth.tenantClaim` | Claim that carries the tenant ID. Refused with a tenant source. |
| `VMAFX_AUTH_ROLES_CLAIM` | `auth.roles_claim` | string | `vmafx_roles` | `auth.rolesClaim` | Claim that carries the list of roles. Refused with a tenant source. |
| `VMAFX_AUTH_TENANTS_SOURCE` | `auth.tenants.source` | string | _(unset)_ | set by the chart | `kubernetes` (the namespace's `VmafxTenant` resources) or `file`: verify each token against its tenant's provider ([tenant registry](../server/auth.md#tenant-registry)). Excludes the provider variables above and `VMAFX_AUTH_DISABLED`. |
| `VMAFX_AUTH_TENANTS_FILE` | `auth.tenants.file` | path | _(unset)_ |  | YAML or JSON file of `VmafxTenant` documents (source `file`). |
| `VMAFX_AUTH_TENANTS_NAMESPACE` | `auth.tenants.namespace` | string | the pod's namespace | set by the chart | Namespace of the `VmafxTenant` resources (source `kubernetes`). |
| `VMAFX_AUTH_TENANTS_REFRESH` | `auth.tenants.refresh` | duration | `30s` |  | Re-read interval of the tenant source, `1s` to `1h`; the tenant set is refused after ten intervals without a successful read. |
| `VMAFX_SCORING_ROOTS` | `scoring.roots` | list | _(unset: every input refused)_ | `auth.scoringRoots` | Comma-separated scoring roots of every caller without a tenant registry; `{tenant}` becomes the caller's tenant ID ([scoring roots](../server/auth.md#scoring-roots)). Refused with a tenant registry. |
| `VMAFX_LOG_LEVEL` | `log.level` | string | `info` |  | Log level: `debug`, `info`, `warn` or `error`, any case; an unknown value gives `info`. |
| `VMAFX_LOG_FORMAT` | `log.format` | string | `auto` |  | Log handler: `auto` (tint on a terminal, else JSON), `tint` or `json`; logs go to stderr. |
| `VMAFX_OTEL_ENABLED` | `otel.enabled` | bool | `true` |  | OpenTelemetry master switch; `false` installs no-op providers even with an endpoint. |
| `VMAFX_OTEL_ENDPOINT` | `otel.endpoint` | `host:port` | _(unset)_ |  | OTLP/gRPC collector (`otel-collector:4317`); wins over `OTEL_EXPORTER_OTLP_ENDPOINT`. Neither set: no export ([OpenTelemetry](../observability/otel.md)). |
| `VMAFX_OTEL_INSECURE` | `otel.insecure` | bool | `true` |  | Plaintext gRPC to the collector; `false` dials with TLS. |
| `VMAFX_OTEL_SERVICE_NAME` | `otel.service.name` | string | `OTEL_SERVICE_NAME`, else the binary name |  | `service.name` resource attribute. |
| `VMAFX_OTEL_SERVICE_VERSION` | `otel.service.version` | string | the build version |  | `service.version` resource attribute. |
| `VMAFX_OTEL_SERVICE_NAMESPACE` | `otel.service.namespace` | string | _(unset)_ |  | `service.namespace` resource attribute. |
| `VMAFX_OTEL_SAMPLE_RATIO` | `otel.sample.ratio` | number | `1.0` |  | Parent-based trace sample ratio in `[0, 1]`; `OTEL_TRACES_SAMPLER` and its argument are not read. |
| `VMAFX_OTEL_EXPORT_TRACES` | `otel.export.traces` | bool | `true` |  | Export traces. |
| `VMAFX_OTEL_EXPORT_METRICS` | `otel.export.metrics` | bool | `true` |  | Export metrics. |
| `VMAFX_OTEL_EXPORT_LOGS` | `otel.export.logs` | bool | `true` |  | Export the logs signal; application logs are not bridged to it today. |
| `OTEL_SERVICE_NAME` | read directly | string | _(unset)_ |  | `service.name` when `VMAFX_OTEL_SERVICE_NAME` is unset (OTel standard). |
| `OTEL_SDK_DISABLED` | read directly | string | _(unset)_ |  | `true` (exactly) installs no-op providers (OTel standard). |
| `OTEL_EXPORTER_OTLP_ENDPOINT` | read directly | URL | _(unset)_ |  | Collector as a URL (`http://host:4317`) when `VMAFX_OTEL_ENDPOINT` is unset; set, export is on (OTel standard). |
| `OTEL_EXPORTER_OTLP_TRACES_ENDPOINT` | read directly | URL | _(unset)_ |  | Per-signal collector URL for traces; set, export is on (OTel standard). |
| `OTEL_EXPORTER_OTLP_METRICS_ENDPOINT` | read directly | URL | _(unset)_ |  | Per-signal collector URL for metrics; set, export is on (OTel standard). |
| `OTEL_EXPORTER_OTLP_LOGS_ENDPOINT` | read directly | URL | _(unset)_ |  | Per-signal collector URL for logs; set, export is on (OTel standard). |
| `POD_NAME` | read directly | string | _(unset)_ |  | Pod name (Kubernetes downward API), added to log lines and OTel resources as `k8s.pod.name`. |
| `POD_NAMESPACE` | read directly | string | _(unset)_ |  | Pod namespace, added as `k8s.namespace.name`. |
| `POD_IP` | read directly | string | _(unset)_ |  | Pod IP, added as `k8s.pod.ip`. |
| `NODE_NAME` | read directly | string | _(unset)_ |  | Kubernetes node name, added as `k8s.node.name`. |
| `SERVICE_ACCOUNT` | read directly | string | _(unset)_ |  | Service account name, added as `k8s.serviceaccount.name`. |

<!-- END GENERATED: vmafx-api environment vmafx-controller -->

### Server (`cmd/vmafx-server`)

<!-- BEGIN GENERATED: vmafx-api environment vmafx-server (scripts/codegen/vmafx-api.py) -->

| Variable | Key | Type | Default | Chart value | Description |
|---|---|---|---|---|---|
| `VMAFX_HTTP_ADDR` | `http.addr` | `host:port` | `:8080` |  | HTTP listen address of the REST API, `/swagger`, `/metrics` and the probes, a full address. |
| `VMAFX_HTTP_TIMEOUTS_READ` | `http.timeouts.read` | duration | `30s` |  | Deadline for reading a whole request; `0` keeps the framework default. |
| `VMAFX_HTTP_TIMEOUTS_HEADER` | `http.timeouts.header` | duration | `5s` |  | Deadline for reading the request headers (slow-client guard); `0` keeps the framework default. |
| `VMAFX_HTTP_TIMEOUTS_WRITE` | `http.timeouts.write` | duration | `15m` |  | Deadline for writing a response; raised so a long `POST /v1/score` completes ([limits](../server/configuration.md#limits-and-timeouts)); `0` keeps the framework default. |
| `VMAFX_HTTP_TIMEOUTS_IDLE` | `http.timeouts.idle` | duration | `120s` |  | Keep-alive idle timeout; `0` keeps the framework default. |
| `VMAFX_HTTP_TIMEOUTS_SHUTDOWN` | `http.timeouts.shutdown` | duration | `30s` |  | Drain time of the HTTP server at shutdown; `0` keeps the framework default. |
| `VMAFX_HTTP_LIMITS_HEADER` | `http.limits.header` | bytes | `1048576` |  | Largest request header block; `0` keeps the default. |
| `VMAFX_HTTP_LIMITS_BODY` | `http.limits.body` | bytes | `10485760` |  | Largest request body; `0` keeps the default. `VMAFX_HTTP_LIMITS_UNLIMITED` removes the cap. |
| `VMAFX_HTTP_LIMITS_UNLIMITED` | `http.limits.unlimited` | bool | `false` |  | Serve request bodies of any size; `VMAFX_HTTP_LIMITS_BODY` is then ignored. |
| `VMAFX_GRPC_LISTEN` | `grpc.listen` | `host:port` | `:9090` |  | gRPC listen address of `VmafxScoring`, a full address. |
| `VMAFX_GRPC_TLS` | `grpc.tls` | bool | `false` |  | Serve gRPC over TLS; needs `VMAFX_GRPC_CERT_FILE` and `VMAFX_GRPC_KEY_FILE`. |
| `VMAFX_GRPC_CERT_FILE` | `grpc.cert_file` | path | _(unset)_ |  | PEM certificate of the gRPC listener (with `VMAFX_GRPC_TLS`). |
| `VMAFX_GRPC_KEY_FILE` | `grpc.key_file` | path | _(unset)_ |  | PEM private key of the gRPC listener (with `VMAFX_GRPC_TLS`). |
| `VMAFX_GRPC_MAX_RECV_SIZE` | `grpc.max_recv_size` | bytes | `67108864` |  | Largest gRPC message received; `0` keeps the gRPC default. |
| `VMAFX_GRPC_MAX_SEND_SIZE` | `grpc.max_send_size` | bytes | `4194304` |  | Largest gRPC message sent; `0` keeps the gRPC default. |
| `VMAFX_VMAF_BINARY` | `vmaf.binary` | path | `vmaf` on `PATH` |  | Path of the `vmaf` CLI behind the scorer; a missing binary stops the program at startup. |
| `VMAFX_MODEL_DIR` | `model.dir` | path | _(unset)_ |  | Directory of the VMAF `.json` models the scorer loads. |
| `VMAFX_MAX_CONCURRENT_SCORES` | `max.concurrent.scores` | integer | number of CPUs |  | Cap on simultaneous `Score` calls over HTTP and gRPC; excess calls get HTTP 429 or `ResourceExhausted`. A value below 1 or not a number keeps the default. |
| `VMAFX_SWAGGER_TRY_IT_OUT` | read directly | `1` | off |  | `1` enables the live "try it out" execution of the Swagger UI. |
| `VMAFX_LOG_LEVEL` | `log.level` | string | `info` |  | Log level: `debug`, `info`, `warn` or `error`, any case; an unknown value gives `info`. |
| `VMAFX_LOG_FORMAT` | `log.format` | string | `auto` |  | Log handler: `auto` (tint on a terminal, else JSON), `tint` or `json`; logs go to stderr. |
| `VMAFX_OTEL_ENABLED` | `otel.enabled` | bool | `true` |  | OpenTelemetry master switch; `false` installs no-op providers even with an endpoint. |
| `VMAFX_OTEL_ENDPOINT` | `otel.endpoint` | `host:port` | _(unset)_ |  | OTLP/gRPC collector (`otel-collector:4317`); wins over `OTEL_EXPORTER_OTLP_ENDPOINT`. Neither set: no export ([OpenTelemetry](../observability/otel.md)). |
| `VMAFX_OTEL_INSECURE` | `otel.insecure` | bool | `true` |  | Plaintext gRPC to the collector; `false` dials with TLS. |
| `VMAFX_OTEL_SERVICE_NAME` | `otel.service.name` | string | `OTEL_SERVICE_NAME`, else the binary name |  | `service.name` resource attribute. |
| `VMAFX_OTEL_SERVICE_VERSION` | `otel.service.version` | string | the build version |  | `service.version` resource attribute. |
| `VMAFX_OTEL_SERVICE_NAMESPACE` | `otel.service.namespace` | string | _(unset)_ |  | `service.namespace` resource attribute. |
| `VMAFX_OTEL_SAMPLE_RATIO` | `otel.sample.ratio` | number | `1.0` |  | Parent-based trace sample ratio in `[0, 1]`; `OTEL_TRACES_SAMPLER` and its argument are not read. |
| `VMAFX_OTEL_EXPORT_TRACES` | `otel.export.traces` | bool | `true` |  | Export traces. |
| `VMAFX_OTEL_EXPORT_METRICS` | `otel.export.metrics` | bool | `true` |  | Export metrics. |
| `VMAFX_OTEL_EXPORT_LOGS` | `otel.export.logs` | bool | `true` |  | Export the logs signal; application logs are not bridged to it today. |
| `OTEL_SERVICE_NAME` | read directly | string | _(unset)_ |  | `service.name` when `VMAFX_OTEL_SERVICE_NAME` is unset (OTel standard). |
| `OTEL_SDK_DISABLED` | read directly | string | _(unset)_ |  | `true` (exactly) installs no-op providers (OTel standard). |
| `OTEL_EXPORTER_OTLP_ENDPOINT` | read directly | URL | _(unset)_ |  | Collector as a URL (`http://host:4317`) when `VMAFX_OTEL_ENDPOINT` is unset; set, export is on (OTel standard). |
| `OTEL_EXPORTER_OTLP_TRACES_ENDPOINT` | read directly | URL | _(unset)_ |  | Per-signal collector URL for traces; set, export is on (OTel standard). |
| `OTEL_EXPORTER_OTLP_METRICS_ENDPOINT` | read directly | URL | _(unset)_ |  | Per-signal collector URL for metrics; set, export is on (OTel standard). |
| `OTEL_EXPORTER_OTLP_LOGS_ENDPOINT` | read directly | URL | _(unset)_ |  | Per-signal collector URL for logs; set, export is on (OTel standard). |
| `POD_NAME` | read directly | string | _(unset)_ |  | Pod name (Kubernetes downward API), added to log lines and OTel resources as `k8s.pod.name`. |
| `POD_NAMESPACE` | read directly | string | _(unset)_ |  | Pod namespace, added as `k8s.namespace.name`. |
| `POD_IP` | read directly | string | _(unset)_ |  | Pod IP, added as `k8s.pod.ip`. |
| `NODE_NAME` | read directly | string | _(unset)_ |  | Kubernetes node name, added as `k8s.node.name`. |
| `SERVICE_ACCOUNT` | read directly | string | _(unset)_ |  | Service account name, added as `k8s.serviceaccount.name`. |

<!-- END GENERATED: vmafx-api environment vmafx-server -->

### Worker node (`cmd/vmafx-node`)

<!-- BEGIN GENERATED: vmafx-api environment vmafx-node (scripts/codegen/vmafx-api.py) -->

| Variable | Key | Type | Default | Chart value | Description |
|---|---|---|---|---|---|
| `VMAFX_HTTP_ADDR` | `http.addr` | `host:port` | `:9090` | `node.metricsPort` | HTTP listen address of `/metrics` and the `/livez`, `/readyz` and `/startupz` probes, a full address. |
| `VMAFX_HTTP_TIMEOUTS_READ` | `http.timeouts.read` | duration | `30s` |  | Deadline for reading a whole request; `0` keeps the framework default. |
| `VMAFX_HTTP_TIMEOUTS_HEADER` | `http.timeouts.header` | duration | `5s` |  | Deadline for reading the request headers (slow-client guard); `0` keeps the framework default. |
| `VMAFX_HTTP_TIMEOUTS_WRITE` | `http.timeouts.write` | duration | `60s` |  | Deadline for writing a response; `0` keeps the framework default. |
| `VMAFX_HTTP_TIMEOUTS_IDLE` | `http.timeouts.idle` | duration | `120s` |  | Keep-alive idle timeout; `0` keeps the framework default. |
| `VMAFX_HTTP_TIMEOUTS_SHUTDOWN` | `http.timeouts.shutdown` | duration | `30s` |  | Drain time of the HTTP server at shutdown; `0` keeps the framework default. |
| `VMAFX_HTTP_LIMITS_HEADER` | `http.limits.header` | bytes | `1048576` |  | Largest request header block; `0` keeps the default. |
| `VMAFX_HTTP_LIMITS_BODY` | `http.limits.body` | bytes | `10485760` |  | Largest request body; `0` keeps the default. `VMAFX_HTTP_LIMITS_UNLIMITED` removes the cap. |
| `VMAFX_HTTP_LIMITS_UNLIMITED` | `http.limits.unlimited` | bool | `false` |  | Serve request bodies of any size; `VMAFX_HTTP_LIMITS_BODY` is then ignored. |
| `VMAFX_GRPC_LISTEN` | `grpc.listen` | `host:port` | `:50052` | `node.grpcPort` | gRPC listen address of the node's `VmafxScoring` service, a full address. |
| `VMAFX_GRPC_TLS` | `grpc.tls` | bool | `false` |  | Serve gRPC over TLS; needs `VMAFX_GRPC_CERT_FILE` and `VMAFX_GRPC_KEY_FILE`. |
| `VMAFX_GRPC_CERT_FILE` | `grpc.cert_file` | path | _(unset)_ |  | PEM certificate of the gRPC listener (with `VMAFX_GRPC_TLS`). |
| `VMAFX_GRPC_KEY_FILE` | `grpc.key_file` | path | _(unset)_ |  | PEM private key of the gRPC listener (with `VMAFX_GRPC_TLS`). |
| `VMAFX_GRPC_MAX_RECV_SIZE` | `grpc.max_recv_size` | bytes | `4194304` |  | Largest gRPC message received; `0` keeps the gRPC default. |
| `VMAFX_GRPC_MAX_SEND_SIZE` | `grpc.max_send_size` | bytes | `4194304` |  | Largest gRPC message sent; `0` keeps the gRPC default. |
| `VMAFX_VMAF_BINARY` | `vmaf.binary` | path | `VMAF_BIN`, `/usr/local/bin/vmaf`, then the build trees | set by the chart | Path of the `vmaf` CLI behind the `Score` RPC and controller jobs. The chart sets `/usr/local/bin/vmaf`; without a binary the node serves only `Health`. |
| `VMAF_BIN` | read directly | path | _(unset)_ |  | Path of the `vmaf` CLI when `VMAFX_VMAF_BINARY` is unset. |
| `VMAFX_MODEL_DIR` | `model.dir` | path | _(unset)_ | `persistence.models.mountPath`, `persistence.models.enabled` | Directory of the VMAF `.json` models. The chart sets the models volume, or the image's `/usr/local/share/vmafx/model`. |
| `VMAFX_BACKEND` | `backend` | string | `cpu` | `gpu.vendor` | Backend the node runs (`cpu`, `cuda`, `hip`, `sycl`, `metal`); advertised to the controller and passed to the `vmaf` CLI as `--backend` for controller jobs. The chart sets it from `gpu.vendor`. |
| `VMAFX_NODE_ID` | `node.id` | string | host name | set by the chart | Node name sent to `RegisterNode`; the chart sets the pod name. |
| `VMAFX_NODE_SLOTS` | `node.slots` | integer | `1` |  | Controller jobs the node runs at once, 1 to 64. |
| `VMAFX_CONTROLLER_ADDR` | `controller.addr` | `host:port` | _(unset)_ | set by the chart | gRPC address of the controller; set, the node registers and pulls jobs ([ADR-1524](../adr/1524-vmafx-node-controller-client.md)). |
| `VMAFX_CONTROLLER_TLS` | `controller.tls` | bool | `false` |  | Dial the controller with TLS (system roots unless `VMAFX_CONTROLLER_CA_FILE` is set). |
| `VMAFX_CONTROLLER_CA_FILE` | `controller.ca_file` | path | system roots |  | PEM bundle that verifies the controller certificate; needs `VMAFX_CONTROLLER_TLS`. |
| `VMAFX_CONTROLLER_SERVER_NAME` | `controller.server_name` | string | host of the address |  | TLS server name override; needs `VMAFX_CONTROLLER_TLS`. |
| `VMAFX_CONTROLLER_TOKEN_FILE` | `controller.token_file` | path | _(unset)_ | `node.controllerToken.secretName` | File holding the bearer token for the controller, read again on every call; an expired JWT is not sent. Not together with `VMAFX_CONTROLLER_TOKEN`. |
| `VMAFX_CONTROLLER_TOKEN` | `controller.token` | string | _(unset)_ |  | Secret. Bearer token for the controller given inline; not together with `VMAFX_CONTROLLER_TOKEN_FILE`. |
| `VMAFX_CONTROLLER_RPC_TIMEOUT` | `controller.rpc_timeout` | duration | `10s` |  | Deadline of every controller call; a value that is not a positive duration stops the node. |
| `VMAFX_CONTROLLER_HEARTBEAT_INTERVAL` | `controller.heartbeat_interval` | duration | `10s` |  | Heartbeat period. |
| `VMAFX_CONTROLLER_POLL_INTERVAL` | `controller.poll_interval` | duration | `2s` |  | Wait after an empty `PullWork`. |
| `VMAFX_FFMPEG_BIN` | `ffmpeg.bin` | path | `ffmpeg` on `PATH` |  | `ffmpeg` of the startup encoder probe; the node image sets `/usr/local/bin/ffmpeg` ([ADR-0717](../adr/0717-vmafx-node-ffmpeg-latest.md)). |
| `VMAFX_SIDECAR_SOCKET` | `sidecar.socket` | path | `/tmp/vmafx-sidecar.sock` |  | Unix socket of the online-training sidecar ([ADR-0781](../adr/0781-sidecar-sgd-ema-online-trainer.md)). |
| `VMAFX_STORAGE_MODE` | `storage.mode` | string | `auto` | `storage.mode` | How rclone-remote job sources are read: `http-serve`, `mount` or `auto`; anything else, or `mount` without FUSE, stops the node ([ADR-1526](../adr/1526-node-storage-streamed-inputs.md)). |
| `VMAFX_STORAGE_MOUNT_ROOT` | `storage.mount_root` | path | temp directory | set by the chart | Parent of `mount` mode's per-job mount points; must lie under `VMAFX_EBPF_MOUNT_PREFIX` when the tracker is on. |
| `VMAFX_RCLONE_BIN` | `rclone.bin` | path | `rclone` on `PATH` |  | rclone binary; a missing one only fails rclone sources. |
| `VMAFX_RCLONE_CONFIG` | `rclone.config` | path | rclone's default | `storage.rclone.config` | rclone configuration file with the remotes and their credentials. |
| `VMAFX_EBPF_BYPASS` | `ebpf.bypass` | bool | `false` | `node.ebpf.enabled` | Start the eBPF descriptor tracker (little-endian Linux); a host that cannot run it stops the node ([ADR-1539](../adr/1539-node-ebpf-tracker-wiring.md)). |
| `VMAFX_EBPF_MOUNT_PREFIX` | `ebpf.mount_prefix` | path | `/rclone-mount/` | `node.ebpf.enabled`, `node.ebpf.mountPrefix` | Path whose opens the tracker records; must contain `VMAFX_STORAGE_MOUNT_ROOT`. |
| `VMAFX_LOG_LEVEL` | `log.level` | string | `info` |  | Log level: `debug`, `info`, `warn` or `error`, any case; an unknown value gives `info`. |
| `VMAFX_LOG_FORMAT` | `log.format` | string | `auto` |  | Log handler: `auto` (tint on a terminal, else JSON), `tint` or `json`; logs go to stderr. |
| `VMAFX_OTEL_ENABLED` | `otel.enabled` | bool | `true` |  | OpenTelemetry master switch; `false` installs no-op providers even with an endpoint. |
| `VMAFX_OTEL_ENDPOINT` | `otel.endpoint` | `host:port` | _(unset)_ |  | OTLP/gRPC collector (`otel-collector:4317`); wins over `OTEL_EXPORTER_OTLP_ENDPOINT`. Neither set: no export ([OpenTelemetry](../observability/otel.md)). |
| `VMAFX_OTEL_INSECURE` | `otel.insecure` | bool | `true` |  | Plaintext gRPC to the collector; `false` dials with TLS. |
| `VMAFX_OTEL_SERVICE_NAME` | `otel.service.name` | string | `OTEL_SERVICE_NAME`, else the binary name |  | `service.name` resource attribute. |
| `VMAFX_OTEL_SERVICE_VERSION` | `otel.service.version` | string | the build version |  | `service.version` resource attribute. |
| `VMAFX_OTEL_SERVICE_NAMESPACE` | `otel.service.namespace` | string | _(unset)_ |  | `service.namespace` resource attribute. |
| `VMAFX_OTEL_SAMPLE_RATIO` | `otel.sample.ratio` | number | `1.0` |  | Parent-based trace sample ratio in `[0, 1]`; `OTEL_TRACES_SAMPLER` and its argument are not read. |
| `VMAFX_OTEL_EXPORT_TRACES` | `otel.export.traces` | bool | `true` |  | Export traces. |
| `VMAFX_OTEL_EXPORT_METRICS` | `otel.export.metrics` | bool | `true` |  | Export metrics. |
| `VMAFX_OTEL_EXPORT_LOGS` | `otel.export.logs` | bool | `true` |  | Export the logs signal; application logs are not bridged to it today. |
| `OTEL_SERVICE_NAME` | read directly | string | _(unset)_ |  | `service.name` when `VMAFX_OTEL_SERVICE_NAME` is unset (OTel standard). |
| `OTEL_SDK_DISABLED` | read directly | string | _(unset)_ |  | `true` (exactly) installs no-op providers (OTel standard). |
| `OTEL_EXPORTER_OTLP_ENDPOINT` | read directly | URL | _(unset)_ |  | Collector as a URL (`http://host:4317`) when `VMAFX_OTEL_ENDPOINT` is unset; set, export is on (OTel standard). |
| `OTEL_EXPORTER_OTLP_TRACES_ENDPOINT` | read directly | URL | _(unset)_ |  | Per-signal collector URL for traces; set, export is on (OTel standard). |
| `OTEL_EXPORTER_OTLP_METRICS_ENDPOINT` | read directly | URL | _(unset)_ |  | Per-signal collector URL for metrics; set, export is on (OTel standard). |
| `OTEL_EXPORTER_OTLP_LOGS_ENDPOINT` | read directly | URL | _(unset)_ |  | Per-signal collector URL for logs; set, export is on (OTel standard). |
| `POD_NAME` | read directly | string | _(unset)_ |  | Pod name (Kubernetes downward API), added to log lines and OTel resources as `k8s.pod.name`. |
| `POD_NAMESPACE` | read directly | string | _(unset)_ |  | Pod namespace, added as `k8s.namespace.name`. |
| `POD_IP` | read directly | string | _(unset)_ |  | Pod IP, added as `k8s.pod.ip`. |
| `NODE_NAME` | read directly | string | _(unset)_ |  | Kubernetes node name, added as `k8s.node.name`. |
| `SERVICE_ACCOUNT` | read directly | string | _(unset)_ |  | Service account name, added as `k8s.serviceaccount.name`. |

<!-- END GENERATED: vmafx-api environment vmafx-node -->

### Operator (`cmd/vmafx-operator`)

<!-- BEGIN GENERATED: vmafx-api environment vmafx-operator (scripts/codegen/vmafx-api.py) -->

| Variable | Key | Type | Default | Chart value | Description |
|---|---|---|---|---|---|
| `VMAFX_CONTROLLER_TLS` | `controller.tls` | bool | `false` |  | Dial the controller with TLS (system roots unless `VMAFX_CONTROLLER_CA_FILE` is set). |
| `VMAFX_CONTROLLER_CA_FILE` | `controller.ca_file` | path | system roots |  | PEM bundle that verifies the controller certificate; needs `VMAFX_CONTROLLER_TLS`. |
| `VMAFX_CONTROLLER_SERVER_NAME` | `controller.server_name` | string | host of the address |  | TLS server name override; needs `VMAFX_CONTROLLER_TLS`. |
| `VMAFX_CONTROLLER_TOKEN_FILE` | `controller.token_file` | path | _(unset)_ | `operator.controllerToken.secretName` | File holding the bearer token for the controller, read again on every call; an expired JWT is not sent. Not together with `VMAFX_CONTROLLER_TOKEN`. |
| `VMAFX_CONTROLLER_TOKEN` | `controller.token` | string | _(unset)_ |  | Secret. Bearer token for the controller given inline; not together with `VMAFX_CONTROLLER_TOKEN_FILE`. |
| `VMAFX_OPERATOR_METRICS_ADDR` | `operator.metrics_addr` | `host:port` | `:8080` | set by the chart | Bind address of the Prometheus metrics endpoint; `0` disables it. |
| `VMAFX_OPERATOR_HEALTH_PROBE_ADDR` | `operator.health_probe_addr` | `host:port` | `:8081` | set by the chart | Bind address of `/healthz` and `/readyz`. |
| `VMAFX_OPERATOR_LEADER_ELECTION` | `operator.leader_election` | bool | `false` | `operator.leaderElect` | Leader election; `true` for several replicas. |
| `VMAFX_OPERATOR_LEADER_ELECTION_ID` | `operator.leader_election_id` | string | `vmafx-operator.vmafx.dev` |  | Lease name of the leader election. |
| `VMAFX_OPERATOR_GRACEFUL_SHUTDOWN` | `operator.graceful_shutdown` | duration | `30s` |  | Graceful-shutdown timeout of the manager. |
| `VMAFX_OPERATOR_WEBHOOK_PORT` | `operator.webhook_port` | integer | `0` |  | Admission-webhook port; `0` disables the webhooks. |
| `VMAFX_OPERATOR_WEBHOOK_HOST` | `operator.webhook_host` | host | all interfaces |  | Admission-webhook bind host. |
| `VMAFX_CONTROLLER_GRPC_ADDR` | read directly | `host:port` | `vmafx-controller.<namespace>.svc.cluster.local:9090` | `controller.enabled`, `controller.grpcPort` | gRPC address of the controller, used by the `VmafxJob` reconciler. |
| `VMAFX_CONTROLLER_HTTP_ADDR` | read directly | URL | `http://vmafx-controller.<namespace>.svc.cluster.local:8080` | `controller.enabled`, `controller.httpPort` | HTTP address of the controller with its scheme, used by the `VmafxNode` health probe (`/healthz` is appended). |
| `VMAFX_LOG_LEVEL` | `log.level` | string | `info` | `operator.logLevel` | Log level: `debug`, `info`, `warn` or `error`, any case; an unknown value gives `info`. |
| `VMAFX_LOG_FORMAT` | `log.format` | string | `auto` |  | Log handler: `auto` (tint on a terminal, else JSON), `tint` or `json`; logs go to stderr. |
| `VMAFX_OTEL_ENABLED` | `otel.enabled` | bool | `true` |  | OpenTelemetry master switch; `false` installs no-op providers even with an endpoint. |
| `VMAFX_OTEL_ENDPOINT` | `otel.endpoint` | `host:port` | _(unset)_ |  | OTLP/gRPC collector (`otel-collector:4317`); wins over `OTEL_EXPORTER_OTLP_ENDPOINT`. Neither set: no export ([OpenTelemetry](../observability/otel.md)). |
| `VMAFX_OTEL_INSECURE` | `otel.insecure` | bool | `true` |  | Plaintext gRPC to the collector; `false` dials with TLS. |
| `VMAFX_OTEL_SERVICE_NAME` | `otel.service.name` | string | `OTEL_SERVICE_NAME`, else the binary name |  | `service.name` resource attribute. |
| `VMAFX_OTEL_SERVICE_VERSION` | `otel.service.version` | string | the build version |  | `service.version` resource attribute. |
| `VMAFX_OTEL_SERVICE_NAMESPACE` | `otel.service.namespace` | string | _(unset)_ |  | `service.namespace` resource attribute. |
| `VMAFX_OTEL_SAMPLE_RATIO` | `otel.sample.ratio` | number | `1.0` |  | Parent-based trace sample ratio in `[0, 1]`; `OTEL_TRACES_SAMPLER` and its argument are not read. |
| `VMAFX_OTEL_EXPORT_TRACES` | `otel.export.traces` | bool | `true` |  | Export traces. |
| `VMAFX_OTEL_EXPORT_METRICS` | `otel.export.metrics` | bool | `true` |  | Export metrics. |
| `VMAFX_OTEL_EXPORT_LOGS` | `otel.export.logs` | bool | `true` |  | Export the logs signal; application logs are not bridged to it today. |
| `OTEL_SERVICE_NAME` | read directly | string | _(unset)_ |  | `service.name` when `VMAFX_OTEL_SERVICE_NAME` is unset (OTel standard). |
| `OTEL_SDK_DISABLED` | read directly | string | _(unset)_ |  | `true` (exactly) installs no-op providers (OTel standard). |
| `OTEL_EXPORTER_OTLP_ENDPOINT` | read directly | URL | _(unset)_ |  | Collector as a URL (`http://host:4317`) when `VMAFX_OTEL_ENDPOINT` is unset; set, export is on (OTel standard). |
| `OTEL_EXPORTER_OTLP_TRACES_ENDPOINT` | read directly | URL | _(unset)_ |  | Per-signal collector URL for traces; set, export is on (OTel standard). |
| `OTEL_EXPORTER_OTLP_METRICS_ENDPOINT` | read directly | URL | _(unset)_ |  | Per-signal collector URL for metrics; set, export is on (OTel standard). |
| `OTEL_EXPORTER_OTLP_LOGS_ENDPOINT` | read directly | URL | _(unset)_ |  | Per-signal collector URL for logs; set, export is on (OTel standard). |
| `POD_NAME` | read directly | string | _(unset)_ |  | Pod name (Kubernetes downward API), added to log lines and OTel resources as `k8s.pod.name`. |
| `POD_NAMESPACE` | read directly | string | _(unset)_ |  | Pod namespace, added as `k8s.namespace.name`. |
| `POD_IP` | read directly | string | _(unset)_ |  | Pod IP, added as `k8s.pod.ip`. |
| `NODE_NAME` | read directly | string | _(unset)_ |  | Kubernetes node name, added as `k8s.node.name`. |
| `SERVICE_ACCOUNT` | read directly | string | _(unset)_ |  | Service account name, added as `k8s.serviceaccount.name`. |

<!-- END GENERATED: vmafx-api environment vmafx-operator -->

### OpenTelemetry identity (all Go services)

The `VMAFX_OTEL_*` variables (service name and version, exporter, sampling) are
read by every Go binary and are listed in each binary's table;
[OpenTelemetry](../observability/otel.md) explains how they combine with the
standard `OTEL_*` variables.

### MCP server (`cmd/vmafx-mcp`)

The Go MCP server also reads the scoring variables of the
[MCP server](#mcp-server) section, which the Python server shares.

<!-- BEGIN GENERATED: vmafx-api environment vmafx-mcp (scripts/codegen/vmafx-api.py) -->

| Variable | Key | Type | Default | Chart value | Description |
|---|---|---|---|---|---|
| `VMAF_BIN` | read directly | path | `/usr/local/bin/vmaf`, then the build trees |  | Path of the `vmaf` CLI behind the scoring tools, looked up on every tool call. |
| `VMAFX_CONTROLLER_ADDR` | read directly | `host:port` | `localhost:9090` |  | gRPC address of `vmafx-controller` for the control-plane tools, read on every call. |
| `VMAFX_CONTROLLER_TOKEN` | read directly | string | _(unset)_ |  | Secret. Bearer token sent with every controller call of the control-plane tools (plaintext transport). With auth on, `get_job` and `list_jobs` need `vmafx:reader`, `submit_job` and `cancel_job` `vmafx:writer`. |
| `VMAFX_MCP_TRANSPORT` | `mcp.transport` | string | `stdio` |  | `stdio` or `http` (streamable HTTP); anything else stops the server. |
| `VMAFX_MCP_HTTP_ADDR` | `mcp.http.addr` | `host:port` | `:3000` |  | Listen address of the HTTP transport; an address without a host takes `VMAFX_MCP_HTTP_BIND`. |
| `VMAFX_MCP_HTTP_BIND` | read directly | host | `127.0.0.1` |  | Host of a listen address that names none; `0.0.0.0` listens on all interfaces ([ADR-0967](../adr/0967-mcp-http-transport-security-hardening.md)). |
| `VMAFX_MCP_HTTP_TOKEN` | read directly | string | _(unset: every request is refused unless `VMAFX_MCP_HTTP_NO_AUTH=1`)_ |  | Secret. Bearer token the HTTP transport requires in `Authorization: Bearer <token>`. |
| `VMAFX_MCP_HTTP_NO_AUTH` | read directly | `1` | off |  | `1` turns the HTTP transport's authentication off; the server logs a warning. |
| `VMAFX_MCP_DIRECT` | read directly | `1` | off |  | `1` scores through libvmaf by cgo instead of a `vmaf` subprocess ([ADR-0931](../adr/0931-mcp-cgo-direct-replace-subprocess.md)); read on every tool call. |
| `VMAFX_SERVER_ADDR` | read directly | `host:port` | `localhost:9090` |  | gRPC address of `vmafx-server` for `vmaf_score_remote`. |
| `VMAFX_GRPC_TIMEOUT` | read directly | seconds | `30` |  | Deadline of each control-plane RPC; a value that is not a positive integer keeps the default. |
| `VMAF_MCP_ALLOW` | read directly | path list | built-in roots |  | Additional roots, separated by the OS path-list separator, under which file paths are accepted. |
| `VMAF_PER_SHOT_BIN` | read directly | path | next to `vmaf`, then `/usr/local/bin` and the build trees |  | Binary of the `vmaf-perShot` tool. |
| `VMAF_ROI_BIN` | read directly | path | next to `vmaf`, then `/usr/local/bin` and the build trees |  | Binary of the `vmaf_roi` tool. |
| `VMAF_BENCH_BIN` | read directly | path | next to `vmaf`, then `/usr/local/bin` and the build trees |  | Binary of the `vmaf_bench` tool. |
| `VMAF_VPL_BIN` | read directly | path | next to `vmaf`, then `/usr/local/bin` and the build trees |  | Binary of the `vmaf_vpl` tool. |
| `VMAF_ROOT` | read directly | path | the repository root when it holds the fixtures, else `/workspace` |  | Data root of the fixture clips `run_benchmark` uses. |
| `VMAF_TUNE_BIN` | read directly | path | `vmaf-tune` on `PATH`, else the repository's |  | `vmaf-tune` binary of the tuning tools. |
| `VMAFX_LOG_LEVEL` | `log.level` | string | `info` |  | Log level: `debug`, `info`, `warn` or `error`, any case; an unknown value gives `info`. |
| `VMAFX_LOG_FORMAT` | `log.format` | string | `auto` |  | Log handler: `auto` (tint on a terminal, else JSON), `tint` or `json`; logs go to stderr. |
| `VMAFX_OTEL_ENABLED` | `otel.enabled` | bool | `true` |  | OpenTelemetry master switch; `false` installs no-op providers even with an endpoint. |
| `VMAFX_OTEL_ENDPOINT` | `otel.endpoint` | `host:port` | _(unset)_ |  | OTLP/gRPC collector (`otel-collector:4317`); wins over `OTEL_EXPORTER_OTLP_ENDPOINT`. Neither set: no export ([OpenTelemetry](../observability/otel.md)). |
| `VMAFX_OTEL_INSECURE` | `otel.insecure` | bool | `true` |  | Plaintext gRPC to the collector; `false` dials with TLS. |
| `VMAFX_OTEL_SERVICE_NAME` | `otel.service.name` | string | `OTEL_SERVICE_NAME`, else the binary name |  | `service.name` resource attribute. |
| `VMAFX_OTEL_SERVICE_VERSION` | `otel.service.version` | string | the build version |  | `service.version` resource attribute. |
| `VMAFX_OTEL_SERVICE_NAMESPACE` | `otel.service.namespace` | string | _(unset)_ |  | `service.namespace` resource attribute. |
| `VMAFX_OTEL_SAMPLE_RATIO` | `otel.sample.ratio` | number | `1.0` |  | Parent-based trace sample ratio in `[0, 1]`; `OTEL_TRACES_SAMPLER` and its argument are not read. |
| `VMAFX_OTEL_EXPORT_TRACES` | `otel.export.traces` | bool | `true` |  | Export traces. |
| `VMAFX_OTEL_EXPORT_METRICS` | `otel.export.metrics` | bool | `true` |  | Export metrics. |
| `VMAFX_OTEL_EXPORT_LOGS` | `otel.export.logs` | bool | `true` |  | Export the logs signal; application logs are not bridged to it today. |
| `OTEL_SERVICE_NAME` | read directly | string | _(unset)_ |  | `service.name` when `VMAFX_OTEL_SERVICE_NAME` is unset (OTel standard). |
| `OTEL_SDK_DISABLED` | read directly | string | _(unset)_ |  | `true` (exactly) installs no-op providers (OTel standard). |
| `OTEL_EXPORTER_OTLP_ENDPOINT` | read directly | URL | _(unset)_ |  | Collector as a URL (`http://host:4317`) when `VMAFX_OTEL_ENDPOINT` is unset; set, export is on (OTel standard). |
| `OTEL_EXPORTER_OTLP_TRACES_ENDPOINT` | read directly | URL | _(unset)_ |  | Per-signal collector URL for traces; set, export is on (OTel standard). |
| `OTEL_EXPORTER_OTLP_METRICS_ENDPOINT` | read directly | URL | _(unset)_ |  | Per-signal collector URL for metrics; set, export is on (OTel standard). |
| `OTEL_EXPORTER_OTLP_LOGS_ENDPOINT` | read directly | URL | _(unset)_ |  | Per-signal collector URL for logs; set, export is on (OTel standard). |
| `POD_NAME` | read directly | string | _(unset)_ |  | Pod name (Kubernetes downward API), added to log lines and OTel resources as `k8s.pod.name`. |
| `POD_NAMESPACE` | read directly | string | _(unset)_ |  | Pod namespace, added as `k8s.namespace.name`. |
| `POD_IP` | read directly | string | _(unset)_ |  | Pod IP, added as `k8s.pod.ip`. |
| `NODE_NAME` | read directly | string | _(unset)_ |  | Kubernetes node name, added as `k8s.node.name`. |
| `SERVICE_ACCOUNT` | read directly | string | _(unset)_ |  | Service account name, added as `k8s.serviceaccount.name`. |

<!-- END GENERATED: vmafx-api environment vmafx-mcp -->

### Tuning CLI (`cmd/vmafx-tune`)

The Go `vmafx-tune` shares the `VMAFTUNE_*` variables of
[`vmaf-tune`](#vmaf-tune) with the Python tool.

<!-- BEGIN GENERATED: vmafx-api environment vmafx-tune (scripts/codegen/vmafx-api.py) -->

| Variable | Key | Type | Default | Chart value | Description |
|---|---|---|---|---|---|
| `VMAFX_MODEL_DIR` | read directly | path | `/usr/local/share/vmafx/model` |  | Directory searched for `<name>.onnx` when `--model` names no file (`predict`, `sidecar`, `auto`). |
| `VMAFX_TUNE_ENCODE_TIMEOUT` | read directly | duration | `60m` |  | Deadline of each `ffmpeg` encode; a value that does not parse keeps the default. |
| `VMAFX_TUNE_PROBE_TIMEOUT` | read directly | duration | `30s` |  | Deadline of each `ffprobe` call; a value that does not parse keeps the default. |
| `VMAFX_TUNE_SCORE_TIMEOUT` | read directly | duration | `30m` |  | Deadline of each `vmaf` run; a value that does not parse keeps the default. |
| `VMAFTUNE_WORKDIR` | read directly | path | OS temp directory |  | Scratch parent of `tune-per-shot` when `--workdir` is not given; used only when it can be created and written. |
| `VMAFTUNE_VAAPI_DEVICE` | read directly | path | first Intel render node, else `/dev/dri/renderD128` |  | VAAPI render node of QSV encodes; `auto` means unset. |
| `VMAFTUNE_SALIENCY_FALLBACK_OK` | read directly | `1` | off |  | `1` lets `recommend-saliency` run a plain encode on an encoder without ROI support instead of exiting 2. |
| `XDG_CACHE_HOME` | read directly | path | `~/.cache` |  | Parent of the `vmaf-tune/sidecar` cache when `--cache-dir` is not given. |
| `VMAFX_LOG_LEVEL` | `log.level` | string | `info` |  | Log level: `debug`, `info`, `warn` or `error`, any case; an unknown value gives `info`. |
| `VMAFX_LOG_FORMAT` | `log.format` | string | `auto` |  | Log handler: `auto` (tint on a terminal, else JSON), `tint` or `json`; logs go to stderr. |
| `VMAFX_OTEL_ENABLED` | `otel.enabled` | bool | `true` |  | OpenTelemetry master switch; `false` installs no-op providers even with an endpoint. |
| `VMAFX_OTEL_ENDPOINT` | `otel.endpoint` | `host:port` | _(unset)_ |  | OTLP/gRPC collector (`otel-collector:4317`); wins over `OTEL_EXPORTER_OTLP_ENDPOINT`. Neither set: no export ([OpenTelemetry](../observability/otel.md)). |
| `VMAFX_OTEL_INSECURE` | `otel.insecure` | bool | `true` |  | Plaintext gRPC to the collector; `false` dials with TLS. |
| `VMAFX_OTEL_SERVICE_NAME` | `otel.service.name` | string | `OTEL_SERVICE_NAME`, else the binary name |  | `service.name` resource attribute. |
| `VMAFX_OTEL_SERVICE_VERSION` | `otel.service.version` | string | the build version |  | `service.version` resource attribute. |
| `VMAFX_OTEL_SERVICE_NAMESPACE` | `otel.service.namespace` | string | _(unset)_ |  | `service.namespace` resource attribute. |
| `VMAFX_OTEL_SAMPLE_RATIO` | `otel.sample.ratio` | number | `1.0` |  | Parent-based trace sample ratio in `[0, 1]`; `OTEL_TRACES_SAMPLER` and its argument are not read. |
| `VMAFX_OTEL_EXPORT_TRACES` | `otel.export.traces` | bool | `true` |  | Export traces. |
| `VMAFX_OTEL_EXPORT_METRICS` | `otel.export.metrics` | bool | `true` |  | Export metrics. |
| `VMAFX_OTEL_EXPORT_LOGS` | `otel.export.logs` | bool | `true` |  | Export the logs signal; application logs are not bridged to it today. |
| `OTEL_SERVICE_NAME` | read directly | string | _(unset)_ |  | `service.name` when `VMAFX_OTEL_SERVICE_NAME` is unset (OTel standard). |
| `OTEL_SDK_DISABLED` | read directly | string | _(unset)_ |  | `true` (exactly) installs no-op providers (OTel standard). |
| `OTEL_EXPORTER_OTLP_ENDPOINT` | read directly | URL | _(unset)_ |  | Collector as a URL (`http://host:4317`) when `VMAFX_OTEL_ENDPOINT` is unset; set, export is on (OTel standard). |
| `OTEL_EXPORTER_OTLP_TRACES_ENDPOINT` | read directly | URL | _(unset)_ |  | Per-signal collector URL for traces; set, export is on (OTel standard). |
| `OTEL_EXPORTER_OTLP_METRICS_ENDPOINT` | read directly | URL | _(unset)_ |  | Per-signal collector URL for metrics; set, export is on (OTel standard). |
| `OTEL_EXPORTER_OTLP_LOGS_ENDPOINT` | read directly | URL | _(unset)_ |  | Per-signal collector URL for logs; set, export is on (OTel standard). |
| `POD_NAME` | read directly | string | _(unset)_ |  | Pod name (Kubernetes downward API), added to log lines and OTel resources as `k8s.pod.name`. |
| `POD_NAMESPACE` | read directly | string | _(unset)_ |  | Pod namespace, added as `k8s.namespace.name`. |
| `POD_IP` | read directly | string | _(unset)_ |  | Pod IP, added as `k8s.pod.ip`. |
| `NODE_NAME` | read directly | string | _(unset)_ |  | Kubernetes node name, added as `k8s.node.name`. |
| `SERVICE_ACCOUNT` | read directly | string | _(unset)_ |  | Service account name, added as `k8s.serviceaccount.name`. |

<!-- END GENERATED: vmafx-api environment vmafx-tune -->

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
