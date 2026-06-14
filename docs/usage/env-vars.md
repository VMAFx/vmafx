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

| Name | Type | Default | Description |
|---|---|---|---|
| `VMAFX_DB_PATH` | path | `vmafx-controller.db` | Path to the SQLite database for job and node persistence. |
| `VMAFX_GRPC_PORT` | port | `50051` | gRPC listen port (serves both `VmafxScoring` and `VmafxController` services). |
| `VMAFX_LOG_LEVEL` | string | `INFO` | Structured log level: `DEBUG`, `INFO`, `WARN`, `ERROR`. |
| `VMAFX_MODEL_DIR` | path | _(none)_ | Directory containing VMAF `.json` model files passed to the libvmaf scorer. |
| `VMAFX_PORT` | port | `8080` | HTTP listen port (serves `/healthz`, `/readyz`, `/metrics`, `/v1/score`). |
| `VMAFX_VMAF_BINARY` | path | _(PATH lookup)_ | Path to the `vmaf` CLI binary.  Falls back to `PATH` lookup if unset. |

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

| Name | Type | Default | Description |
|---|---|---|---|
| `VMAFX_FFMPEG_BIN` | path | `ffmpeg` (PATH) | Path to the `ffmpeg` binary.  The node Docker image sets this to `/usr/local/bin/ffmpeg` (ADR-0717). |
| `VMAFX_LOG_LEVEL` | string | `info` | Structured log level. |
| `VMAFX_NODE_ADDR` | `host:port` | `:50052` | gRPC listen address for the node's worker service. |

---

## Go operator (`cmd/vmafx-operator`)

| Name | Type | Default | Description |
|---|---|---|---|
| `VMAFX_OPERATOR_LEADER_ELECT` | `true`/`false` | `false` | Enable leader election for high-availability deployments.  Set to `true` when running multiple operator replicas. |
| `VMAFX_OPERATOR_LOG_LEVEL` | string | `info` | Log level for the zap logger: `debug`, `info`, `warn`, `error`. |
| `VMAFX_OPERATOR_METRICS_ADDR` | `host:port` | `:8081` | Bind address for the Prometheus metrics endpoint. |
| `VMAFX_OPERATOR_PROBE_ADDR` | `host:port` | `:8082` | Bind address for `/healthz` and `/readyz` health-probe endpoints. |

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
