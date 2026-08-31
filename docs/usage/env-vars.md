<!-- markdownlint-disable MD060 -->
# Environment variable reference

VMAFX reads a number of environment variables at runtime.  This page is the
canonical, code-audited list — last audited against commit `a5d0c683af8f0d15f`.

All variables are optional unless marked **required**.

---

## Core C library (`libvmaf`)

| Name | Type | Default | Description |
|---|---|---|---|
| `VMAF_CUDA_DISPATCH` | string | _(auto)_ | CUDA dispatch strategy. Accepts a bare strategy name (`adaptive`, `batched`, `serial`) or a per-feature override `feature=strategy[,...]`. See [CUDA dispatch knobs](#cuda-dispatch-knob). |
| `VMAF_DISTS_SQ_MODEL_PATH` | path | _(auto)_ | Absolute path to the DISTS-SQ ONNX model file. Overrides the `VMAF_TINY_MODEL_DIR` search. |
| `VMAF_FASTDVDNET_PRE_MODEL_PATH` | path | _(auto)_ | Absolute path to the FastDVDNet-Pre ONNX model file. |
| `VMAF_LPIPS_MODEL_PATH` | path | _(auto)_ | Absolute path to the LPIPS ONNX model file. |
| `VMAF_MOBILESAL_MODEL_PATH` | path | _(auto)_ | Absolute path to the MobileSal saliency ONNX model file. |
| `VMAF_SYCL_DISPATCH` | string | _(auto)_ | SYCL dispatch strategy.  Same syntax as `VMAF_CUDA_DISPATCH`.  Values: `direct`, `graph`.  See [SYCL dispatch knobs](#sycl-dispatch-knob). |
| `VMAF_SYCL_NO_GRAPH` | `0`/`1` | `0` | **Deprecated** — use `VMAF_SYCL_USE_GRAPH=false` instead.  Forces SYCL to the direct (non-graph) path when set to `1`.  Emits a deprecation warning on stderr.  Will be removed in v4.0. |
| `VMAF_SYCL_PROFILE` | `0`/`1` | `0` | Enable SYCL kernel profiling via `enable_profiling` queue property. |
| `VMAF_SYCL_TIMING` | `0`/`1` | `0` | Print per-kernel wall-clock timing to stderr. |
| `VMAF_SYCL_USE_GRAPH` | `true`/`false` | _(auto)_ | Force SYCL graph-replay (`true`) or direct dispatch (`false`) globally. Takes precedence over `VMAF_SYCL_NO_GRAPH`. |
| `VMAF_TINY_MODEL_DIR` | path | _(auto)_ | Chroot-style search directory for all tiny-AI ONNX models.  When set, model loading rejects any path that does not start with this prefix. |
| `VMAF_TRANSNET_V2_MODEL_PATH` | path | _(auto)_ | Absolute path to the TransNet v2 ONNX model file. |

> **Note:** Seven per-feature `VMAF_*_MODEL_PATH` variables exist today
> (`DISTS_SQ`, `FASTDVDNET_PRE`, `LPIPS`, `MOBILESAL`, `TRANSNET_V2`, plus
> two more added at the same time as the feature extractor is registered).
> Each can be used independently of `VMAF_TINY_MODEL_DIR` — the per-feature
> variable takes precedence.

---

## AI scripts (`ai/scripts/`)

See also [ai/scripts-env-vars.md](../ai/scripts-env-vars.md) for per-corpus
overrides not listed here.

| Name | Type | Default | Description |
|---|---|---|---|
| `VMAF_BIN` | path | `core/build-cpu/tools/vmaf` | Path to the `vmaf` CLI binary used by Python AI scripts and feature-extraction pipelines. |
| `VMAF_BVI_DVC_RAW_DIR` | path | `<repo>/.workingdir2/bvi-dvc-raw` | Root of the raw BVI-DVC dataset for `train_predictor_v2_realcorpus.py`. |
| `VMAF_CHUG_DIR` | path | `<repo>/.corpus/chug` | Root of the CHUG shard tree used by `chug_extract_features.py` and `chug_to_corpus_jsonl.py`. |
| `VMAF_DATA_ROOT` | path | `~/datasets` | Parent directory for datasets whose sub-paths are not otherwise overridden (e.g. `$VMAF_DATA_ROOT/konvid-1k`). |
| `VMAF_HW_TAG` | string | `ryzen-9950x3d+rtx4090+arc-a380` | Hardware identifier stamped into benchmark artefacts by `measure_quant_drop_per_ep.py`. |
| `VMAF_KONVID_1K_DIR` | path | `$VMAF_DATA_ROOT/konvid-1k` | Root of the KonViD-1k dataset. |
| `VMAF_KONVID_150K_DIR` | path | `<repo>/.workingdir2/konvid-150k` | Root of the KonViD-150k dataset used by `extract_k150k_features.py`, `konvid_150k_to_corpus_jsonl.py`, and `train_konvid_mos_head.py`. |
| `VMAF_NETFLIX_CORPUS_DIR` | path | `<repo>/.workingdir2/netflix` | Root of the Netflix internal MOS corpus (non-public). |

---

## MCP server (`cmd/vmafx-mcp`, Python MCP server)

| Name | Type | Default | Description |
|---|---|---|---|
| `VMAF_MCP_ALLOW` | colon-separated paths | _(built-in roots)_ | Additional filesystem roots that the MCP server is allowed to read YUV files from.  Paths outside any allowed root are rejected. |
| `VMAF_MCP_ASYNC` | string | `asyncio` | anyio backend for the Python MCP server.  Pass `trio` to switch to the Trio event loop. |
| `VMAF_ROOT` | path | _(auto-detect)_ | Override the repo root detected by the Go MCP server (`cmd/vmafx-mcp`). |
| `VMAF_TUNE_BIN` | path | _(PATH lookup)_ | Path to the `vmaf-tune` binary used by MCP scoring tools. |

---

## Go controller (`cmd/vmafx-controller`)

The controller is wired on the golusoris `fx` framework (ADR-1119). Config is
loaded via golusoris/config: the `VMAFX_` prefix is stripped, the name is
lowercased, and **every** underscore is replaced with the `.` delimiter, so the
koanf key is the dotted form in the _golusoris key_ column. The HTTP and gRPC
modules own the listeners.

> **Breaking change (ADR-1119):** the controller previously read `VMAFX_PORT` and
> `VMAFX_GRPC_PORT` as bare port numbers. It now reads `VMAFX_HTTP_ADDR` /
> `VMAFX_GRPC_LISTEN`, which take a full listen address (`:8080`, not `8080`),
> and the golusoris-native defaults `:8080` / `:9090` apply — the legacy
> wire ports (`8080` / `50051`) are **not** carried. The pre-fx auth/JWKS CLI
> flags are removed; configure auth via the env vars below. Operators must
> migrate.

| Name | Type | Default | golusoris key | Description |
|---|---|---|---|---|
| `VMAFX_HTTP_ADDR` | `host:port` | `:8080` | `http.addr` | HTTP listen address (serves `/healthz`, `/readyz`, `/metrics`, `/v1/score`). |
| `VMAFX_GRPC_LISTEN` | `host:port` | `:9090` | `grpc.listen` | gRPC listen address (serves both `VmafxScoring` and `VmafxController`). |
| `VMAFX_DB_PATH` | path | `vmafx-controller.db` | `db.path` | Path to the embedded SQLite job + node-persistence database (kept, not migrated to golusoris.Jobs — ADR-1119). |
| `VMAFX_LOG_LEVEL` | string | `INFO` | `log.level` | Structured log level: `DEBUG`, `INFO`, `WARN`, `ERROR`. Read directly by golusoris (bridged from `VMAFX_LOG_LEVEL`, golusoris#234). |
| `VMAFX_MODEL_DIR` | path | _(none)_ | `model.dir` | Directory containing VMAF `.json` model files passed to the libvmaf scorer. |
| `VMAFX_VMAF_BINARY` | path | _(PATH lookup)_ | `vmaf.binary` | Path to the `vmaf` CLI binary. Falls back to `PATH` lookup if unset. |
| `VMAFX_AUTH_DISABLED` | bool | `false` | `auth.disabled` | Disable JWT auth (dev/internal only — never in production). When set, a synthetic `dev` tenant with admin role is injected. |
| `VMAFX_JWKS_ENDPOINT` | URL | _(none)_ | `jwks.endpoint` | JWKS endpoint URL for RS256 verification, e.g. `https://idp.example.com/.well-known/jwks.json`. Required unless auth is disabled. |
| `VMAFX_AUTH_ISSUER` | string | _(none)_ | `auth.issuer` | Expected JWT `iss` claim. Required unless auth is disabled. |
| `VMAFX_AUTH_AUDIENCE` | string | _(none)_ | `auth.audience` | Expected JWT `aud` claim (optional; audience check skipped when empty). |
| `VMAFX_AUTH_TENANT_CLAIM` | string | `tid` | `auth.tenant_claim` | JWT claim carrying the tenant id. Declared as a golusoris CompoundKey so its underscore is preserved. |
| `VMAFX_AUTH_ROLES_CLAIM` | string | `vmafx_roles` | `auth.roles_claim` | JWT claim carrying the roles list. Declared as a golusoris CompoundKey. |

---

## Go server (`cmd/vmafx-server`)

The server runs on the [golusoris](https://github.com/golusoris/golusoris) fx
framework (ADR-1119). Config is loaded by golusoris' koanf layer under the
`VMAFX_` env prefix with `_` mapping to the `.` key separator
(`VMAFX_HTTP_ADDR` → `http.addr`). The framework's HTTP and gRPC modules own the
listen addresses, so the server takes **full listen addresses** (`:8080`), not
bare port numbers.

> **Breaking change (ADR-1119).** The pre-fx server used `VMAFX_PORT` /
> `VMAFX_GRPC_PORT` (bare port numbers, e.g. `8080`). These are replaced by
> `VMAFX_HTTP_ADDR` / `VMAFX_GRPC_LISTEN`, which take a full listen address
> (e.g. `:8080`, `:9090`). Update deployment manifests accordingly. Note the
> default gRPC address changed from `:50051` to golusoris' `:9090`.

| Name | Type | Default | Description |
|---|---|---|---|
| `VMAFX_HTTP_ADDR` | `host:port` | `:8080` | HTTP listen address (serves `/healthz`, `/livez`, `/readyz`, `/startupz`, `/metrics`, `/v1/score`, `/v1/health`, `/v1/ready`, `/swagger`). golusoris key `http.addr`. |
| `VMAFX_GRPC_LISTEN` | `host:port` | `:9090` | gRPC listen address (`VmafxScoring`). golusoris key `grpc.listen`. |
| `VMAFX_LOG_LEVEL` | string | `INFO` | Structured log level: `DEBUG`, `INFO`, `WARN`, `ERROR`. |
| `VMAFX_MODEL_DIR` | path | _(none)_ | Model directory passed to the libvmaf scorer (golusoris key `vmaf.model_dir`). |
| `VMAFX_VMAF_BINARY` | path | _(PATH lookup)_ | Path to the `vmaf` CLI binary (golusoris key `vmaf.binary`). |
| `VMAFX_MAX_CONCURRENT_SCORES` | int | _(NumCPU)_ | Cap on simultaneous in-flight `Score` calls; excess requests get HTTP 429 / gRPC `ResourceExhausted`. |
| `VMAFX_SWAGGER_TRY_IT_OUT` | string | _(off)_ | Set to `1` to enable the Swagger UI "try it out" live-execution path. |

---

## Go worker node (`cmd/vmafx-node`)

The node is wired on the golusoris fx framework (ADR-1119). golusoris config
reads these `VMAFX_*` env vars via koanf: it strips the `VMAFX_` prefix,
lowercases, and replaces **every** underscore with the `.` delimiter, so the
listed env var maps to the dotted koanf key shown in the third column.

| Name | koanf key | Type | Default | Description |
|---|---|---|---|---|
| `VMAFX_GRPC_LISTEN` | `grpc.listen` | `host:port` | `:50052` | gRPC listen address for the node's `VmafxScoring` service. **Breaking — replaces `VMAFX_NODE_ADDR`** (ADR-1119). |
| `VMAFX_FFMPEG_BIN` | `ffmpeg.bin` | path | `ffmpeg` (PATH) | Path to the `ffmpeg` binary used by the startup encoder probe.  The node Docker image sets this to `/usr/local/bin/ffmpeg` (ADR-0717). |
| `VMAFX_VMAF_BINARY` | `vmaf.binary` | path | _(FindBinary lookup)_ | Path to the `vmaf` CLI binary backing the unary `Score` RPC. |
| `VMAFX_MODEL_DIR` | `model.dir` | path | _(binary default)_ | Directory containing VMAF `.json` model files. |
| `VMAFX_BACKEND` | `backend` | string | `cpu` | Scoring backend label attached to executor spans / results. |
| `VMAFX_SIDECAR_SOCKET` | `sidecar.socket` | path | `/tmp/vmafx-sidecar.sock` | Unix socket of the online-training sidecar (ADR-0781). |
| `VMAFX_LOG_LEVEL` | `log.level` | string | `info` | Structured log level (read by the golusoris `log` module; honours the `VMAFX_` prefix — ADR-1119). |
| `VMAFX_LOG_FORMAT` | `log.format` | string | `auto` | Log handler: `auto` (tint on TTY, else JSON), `tint`, or `json`. |

> **Migration:** the pre-fx node bound on `VMAFX_NODE_ADDR` (default `:50052`).
> That variable is removed; set `VMAFX_GRPC_LISTEN` instead (the historical
> `:50052` default is preserved). The node remains gRPC-only — its Kubernetes
> probe is the `VmafxScoring/Health` RPC; there is no HTTP `/livez` / `/readyz`
> until `golusoris.HTTP` is added to the node graph.

---

## Go operator (`cmd/vmafx-operator`)

| Name | Type | Default | Description |
|---|---|---|---|
| `VMAFX_OPERATOR_LEADER_ELECTION` | `true`/`false` | `false` | Enable leader election for high-availability deployments. Set to `true` when running multiple operator replicas. |
| `VMAFX_OPERATOR_LEADER_ELECTION_ID` | string | `vmafx-operator.vmafx.dev` | Lease name used when leader election is enabled. |
| `VMAFX_OPERATOR_METRICS_ADDR` | `host:port` | `:8080` | Bind address for the Prometheus metrics endpoint. |
| `VMAFX_OPERATOR_HEALTH_PROBE_ADDR` | `host:port` | `:8081` | Bind address for `/healthz` and `/readyz` health-probe endpoints. |
| `VMAFX_OPERATOR_GRACEFUL_SHUTDOWN` | duration | `30s` | Manager graceful-shutdown timeout. |
| `VMAFX_OPERATOR_WEBHOOK_PORT` | integer | `0` | Admission-webhook port; `0` disables webhooks. |
| `VMAFX_OPERATOR_WEBHOOK_HOST` | host | _(all interfaces)_ | Admission-webhook bind host. |
| `VMAFX_LOG_LEVEL` | string | `info` | Shared structured log level: `debug`, `info`, `warn`, or `error`. |
| `VMAFX_CONTROLLER_GRPC_ADDR` | `host:port` | `vmafx-controller.<ns>.svc.cluster.local:9090` | gRPC address of the vmafx-controller used by job reconciliation. |
| `VMAFX_CONTROLLER_HTTP_ADDR` | URL | `http://vmafx-controller.<ns>.svc.cluster.local:8080` | HTTP address of the vmafx-controller used by health reconciliation. |

---

## Test-only

These variables are read exclusively by test harnesses and CI gates.  They
are not read by production code paths.

| Name | Type | Default | Description |
|---|---|---|---|
| `VMAF_BIN_FOR_TESTS` | path | _(auto-detect)_ | Explicit path to the `vmaf` binary for integration tests (`test_chug_extract_features_smoke.py`, ADR-0543 backend-enforcement tests).  When unset the tests probe the canonical build paths and `PATH`. |
| `VMAF_TEST_DATA` | path | _(repo-relative)_ | Override for the test-data root in Python-harness tests. |

---

## CUDA dispatch knob

`VMAF_CUDA_DISPATCH` controls how CUDA feature extractors submit work to the
driver.  Three strategies are available:

| Value | Behaviour |
|---|---|
| `adaptive` | Runtime heuristic: selects `batched` above the 720p frame-area threshold, `serial` below it.  **Default** when the variable is unset. |
| `batched` | Submits a full drain-batch of per-extractor events and waits once (ADR-0483). Lowest latency at ≥ 1080p. |
| `serial` | Synchronises after each extractor.  Lower overhead at small resolutions or single-extractor runs. |

Per-feature overrides follow the `feature=strategy[,feature2=strategy2]`
syntax, for example:

```bash
VMAF_CUDA_DISPATCH=adaptive,integer_vif=batched ./build/tools/vmaf ...
```

See [ADR-0483](../adr/0483-gpu-dispatch-parse-dedup.md) for the dispatch
parse grammar.

---

## SYCL dispatch knob

`VMAF_SYCL_DISPATCH` controls the SYCL graph-replay strategy.  Two values
are available:

| Value | Behaviour |
|---|---|
| `direct` | Submit kernels directly to an in-order queue (no graph).  Lower per-frame overhead at small resolutions. |
| `graph` | Use SYCL graph replay (ADR-0483).  Reduces kernel-launch overhead at ≥ 720p. |

When the variable is unset, an area-threshold heuristic selects `graph`
above 1280 × 720 pixels and `direct` below.  `VMAF_SYCL_USE_GRAPH` (boolean)
provides a simpler global override.

`VMAF_SYCL_NO_GRAPH` is a **deprecated** alias for `VMAF_SYCL_USE_GRAPH=false`.
Setting it to `1` still works for one release but prints a warning to stderr:

```text
VMAF_SYCL_NO_GRAPH deprecated; use VMAF_SYCL_USE_GRAPH=false. Will be removed in v4.0.
```

See [ADR-0483](../adr/0483-gpu-dispatch-parse-dedup.md) for the full grammar.
