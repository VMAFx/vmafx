# Change Log

> The Unreleased section tracks VMAFx changes. Release-please turns these
> entries and Conventional Commits into ordinary SemVer releases.

## [Unreleased]
### Added

- **Two ADM viewing distances share one extractor (Netflix/vmaf `33e5f0aca`,
  `cffd5b77d`).** When two models need `adm` with options that differ only in
  `adm_norm_view_dist` (for example `vmaf_v1.0.16_3d0h` and `_5d0h`), libvmaf
  runs one `adm`: the wavelet transform and decouple once per scale, the
  weighting per distance, with every score bit-identical to separate runs. The
  new option `adm_norm_view_dist_extra` (`nvde`) requests a second distance
  directly; `adm_rust` does the same. Unlike upstream, a third model at the
  second distance is absorbed instead of failing the run, and a `debug`
  context keeps its scores
  ([Two viewing distances share one `adm`](docs/metrics/adm.md#two-viewing-distances-share-one-adm)).


- **VMAFx core API: contexts, models, host frames and scores (RC4, ADR-1852,
  ADR-1906).** A program can now score videos through `vmafx/*.h` alone:
  contexts with their own log callback (`VmafxContextConfig.log_callback`),
  which receives every message raised for the context, worker threads
  included, while nothing of it reaches the process log,
  context options (`vmafx_context_set_option`), feature option sets
  (`vmafx_options_set`), extractor, model and model-set registration
  (`vmafx_context_use_feature`, `vmafx_context_use_model`,
  `vmafx_context_use_model_set`, `vmafx_context_import_score`), feature
  resolution (`vmafx_feature_resolve`), refcounted models and model sets with
  the SHA-256 of the bytes as loaded (`vmafx_model_load`,
  `vmafx_model_load_file`, `vmafx_model_hash`, `vmafx_model_set_load`, ...;
  a model load logs to the callback of its `VmafxModelConfig`),
  the CPU device (`vmafx_device_create`), host frames allocated or borrowed
  without a copy (`vmafx_frame_create_host`, `vmafx_frame_wrap_host`),
  submission (`vmafx_submit`, `vmafx_flush`), frame retention
  (`vmafx_context_frame_retention`) and synchronous per-frame and pooled
  scores for features, models and model sets (`vmafx_score_frame`,
  `vmafx_score_pooled`, `vmafx_feature_score_pooled`,
  `vmafx_score_frame_model_set`, `vmafx_score_pooled_model_set`), equal bit for
  bit to the `libvmaf.h` calls. One frame can be scored by several contexts
  without a copy. Errors also name what kind of subject failed and the
  function (`vmafx_error_subject_kind`, `vmafx_error_function`); an input
  struct below its introduction size is the new `VMAFX_E_ABI`. ABI 0.1.1. See
  [the VMAFx API page](docs/api/vmafx/index.md).


- **VMAFx device frames on CUDA (RC4, ADR-1929, ADR-2023).** In a build with
  the CUDA backend the VMAFx API creates CUDA devices by index or from your
  context and stream (`vmafx_device_create` with `VMAFX_BACKEND_CUDA`,
  `vmafx_device_count`, `vmafx_device_info`), scores a context on one
  (`vmafx_context_use_device`; features registered afterwards run on their
  CUDA twins) and imports frames without a copy through the host
  (`vmafx_frame_import`): CUDA device pointers are read where they are, NV12,
  P010 and P016 are planarised on the device, CUDA arrays and OpenGL textures
  (the new `VMAFX_MEMORY_GL_TEXTURE`) are read out on the device. Acquire
  fences of kind `VMAFX_FENCE_CUDA_EVENT` are waited on by the device's
  stream, and the new `VMAFX_FENCE_GL_SYNC` orders a GL producer's rendering;
  release fences (`VMAFX_FENCE_HOST`, `VMAFX_FENCE_CUDA_EVENT`) are signalled
  after the last reader in every context, and the new release callback of
  `VmafxFrameImport` (`release`, `user`) lets a producer make its stream wait
  on the release event before it reuses its memory. CUDA frame pools
  (`vmafx_frame_pool_create` with a CUDA device) hand out device frames.
  Imported frames score bit for bit as the same frames uploaded from the
  host. ABI 0.1.4. See
  [CUDA devices](docs/api/vmafx/index.md#cuda-devices).


- **VMAFx device frames, fences and frame pools (RC4, ADR-1852, ADR-1929).**
  The VMAFx API gains the shared contract of zero-copy frame import:
  device enumeration and information (`vmafx_device_count`,
  `vmafx_device_info`, `vmafx_device_describe`, `vmafx_device_profile`; the
  size-prefixed `VmafxDeviceInfo` grows by the format envelope later),
  devices from external handles and profiling flags (`VmafxDeviceDesc`),
  attaching a device to a context (`vmafx_context_use_device`), fences of
  every kind (`VmafxFence`; `vmafx_fence_create`, `vmafx_fence_signal`,
  `vmafx_fence_wait`, `vmafx_fence_destroy`; the new status
  `VMAFX_E_TIMEOUT`), frame import with an acquire fence
  (`vmafx_frame_import`, `VmafxFrameImport`; NV12, P010 and P016 converted
  to planar, never through a host copy), release fences signalled after the
  last reader of a frame (`vmafx_frame_release_fence`), frame pools
  (`vmafx_frame_pool_create`, `vmafx_frame_pool_acquire`,
  `vmafx_frame_pool_destroy`), admission that names every refusing extractor
  (`vmafx_context_admit`) and the import rule of decision D8
  (`vmafx_context_import_frame`: one retry after a host wait of at most
  `VmafxContextConfig.import_retry_wait_ns`, 10 seconds by default, then a
  failure that names the import). This build implements host memory and host fences
  on the CPU device; the CUDA, SYCL, HIP and Metal imports follow behind the
  same functions. An imported frame scores bit for bit as the same frame
  created on the host. ABI 0.1.3. See
  [device frames and fences](docs/api/vmafx/index.md#device-frames-and-fences).


- **VMAFx frame colour and the sample range check (VMAFx API 0.1.6).** A
  frame carries its colour in `VmafxFrameDesc.color`, and
  `vmafx_context_set_default_color()` gives the colour of the frames that
  carry none; a model with a `conversion_target` converts every pair from it,
  and a pair of another colour after the first converted one is refused with
  `VMAFX_E_BUSY`. The context option `check_sample_range` of
  `vmafx_context_set_option()` refuses a sample above 2^bpc - 1. The libvmaf
  functions `vmaf_set_input_colorimetry()` and
  `vmaf_set_sample_range_check_enabled()` are compat functions on these, so the
  `vmaf` command line links against the split library again
  ([Frame colour](docs/api/vmafx/index.md#frame-colour),
  [ADR-2094](docs/adr/2094-libvmaf-compat-library-split.md)).


- **libvmaf is a compat library on the VMAFx API (RC4 WP6).** The engine and
  the VMAFx API ship as `libvmafx.so.1` (pkg-config `libvmafx`), which exports
  `vmafx_*` symbols only; `libvmaf.so.3` keeps the libvmaf API and is written
  on the exported VMAFx functions alone, so `pkg-config --libs libvmaf` now
  gives `-lvmaf -lvmafx`. Programs built for libvmaf, unpatched upstream
  FFmpeg and GStreamer included, build and score unchanged; binaries linked
  against an earlier `libvmaf.so.3` run against this one. The CUDA and SYCL
  functions (HIP and Metal in builds with them) remain the engine's until
  their device-frame work lands. Guide:
  [Migrating from libvmaf.h](docs/api/vmafx/index.md#migrating-from-libvmafh),
  [migration table](docs/api/vmafx/compat.md)
  ([ADR-2094](docs/adr/2094-libvmaf-compat-library-split.md)).
- **VMAFx API 0.1.6 additions** for the compat library
  ([reference](docs/api/vmafx/reference.md)): the libvmaf bridge for pictures,
  models and model sets (`vmafx_frame_from_picture`, `vmafx_frame_to_picture`,
  `vmafx_model_from_libvmaf`, `vmafx_model_libvmaf_handle`,
  `vmafx_model_set_from_libvmaf`, `vmafx_model_set_libvmaf_handle`);
  context-owned preallocated frames (`vmafx_context_preallocate`,
  `vmafx_context_acquire_frame`); `vmafx_context_backend`,
  `vmafx_context_attach_sidedata`, `vmafx_backend_name`; frame converters
  (`vmafx_frame_converter_create`, `vmafx_frame_convert`,
  `vmafx_frame_converter_destroy`); tiny-AI models and sessions
  (`vmafx/dnn.h`: `vmafx_dnn_available`, `vmafx_context_use_tiny_model`,
  `vmafx_context_set_codec_context`, `vmafx_context_is_codec_aware`,
  `vmafx_context_set_tiny_resize`, `vmafx_dnn_session_*`,
  `vmafx_dnn_verify_signature`); the embedded MCP server (`vmafx/mcp.h`:
  `vmafx_mcp_*`); with their structs, handles and constants.
- **Opt-in deprecation warnings for `libvmaf.h`.** Compile with
  `-DVMAF_ENABLE_DEPRECATION_WARNINGS` to have every libvmaf call name its
  VMAFx successor; the warnings become the default in 1.1 and the functions
  go in 2.0 (ADR-1852 decision D7).
- **Upstream consumer conformance.** `scripts/ci/upstream-ffmpeg-compat.sh`
  and `scripts/ci/upstream-gstreamer-compat.sh` build unpatched upstream
  FFmpeg (`FFMPEG_TAG`) and the upstream GStreamer `vmaf` element
  (`GST_PLUGINS_BAD_VERSION`, kept current by Renovate) against an installed
  libvmaf and compare their scores with the `vmaf` command line and with a
  reference library as exact text; the `Upstream Consumers` workflow runs both
  ([guide](docs/development/upstream-consumers.md), #2237).


- Every scoring option is now defined once, in the option groups of
  `core/api/vmafx.toml`, and generated into each surface: the `vmaf`
  option table and `--help` text, the input schemas both MCP servers serve,
  the `ScoreOptions` proto message and its OpenAPI schema, the AVOption table
  of the coming `vmafx` FFmpeg filter (`ffmpeg-patches/src/vf_vmafx_options.h`)
  and the option tables of `docs/usage/cli.md`, `docs/usage/ffmpeg.md`,
  `docs/mcp/tools.md` and `docs/server/api-contract.md`. Every command-line
  spelling `vmaf` accepted before is still accepted (ADR-2044).
- The `vmaf` JSON report carries a `provenance` object (ABI version, active
  backend, extractor count, build version) next to `backend_used` (#2142).
- MCP scoring tools accept `view_distance` and `display_height` (the ADM
  extractor's viewing distance and display height) and declare the
  device-target arguments `target_width`, `target_height` and
  `target_scaling`, which accept only their defaults until device-targeted
  scoring lands.


- **VMAFx window scores and the window clock (RC4, ADR-1852, ADR-2074).**
  The VMAFx API gains asynchronous pooled scores over a range of frames:
  `vmafx_window_submit` takes a `VmafxWindowRequest` (a model, a model set or
  a feature, a set of pooling methods `VmafxPoolMask`, `first` / `last`, an
  optional callback) and returns at once; `vmafx_window_poll`,
  `vmafx_window_wait` and the callback deliver a `VmafxWindowResult` (one
  value per method, `n_frames`, `n_scored`, a partial flag) once every frame
  of the window is final, with the values of the synchronous pooled call bit
  for bit; `vmafx_window_release` cancels or frees it. Each context's
  completion thread finds completion as worker threads finish frames, so a
  window completes whether or not the producer calls again; callbacks run on
  a separate callback thread. `vmafx_flush` completes
  open windows over the frames the stream had. `VmafxWindowClock`
  (`vmafx_window_clock_create`, `_frame`, `_finish`, `_destroy`) cuts a stream
  into windows of `n_stats` seconds or `n_stats_frames` frames (#2138), and
  `vmafx_context_max_in_flight` reports the most frames a context holds after
  a submit (#2238). Windows over `motion2` / `motion3`, and so over VMAF
  models, complete at the flush in this release. ABI 0.1.8. See
  [window scores](docs/api/vmafx/windows.md).


- Added `ACCESSIBILITY.md` (commitment, scope, known limitations, how to report
  a barrier), an accessibility issue form and the `accessibility` label, a
  "Your first contribution" and "What to expect" section in `CONTRIBUTING.md`,
  the Contributor Covenant enforcement guidelines and a route for reports about
  the maintainer in `CODE_OF_CONDUCT.md`, and a continuity and succession
  section in `GOVERNANCE.md`. `README.md` links the code of conduct, support
  and accessibility pages; `SUPPORT.md` no longer lists the removed Vulkan
  backend. ADR-2461 (Accepted) adds a lighter deliverables track for small
  pull requests.


- **vmafx-controller can keep its jobs in PostgreSQL and run as several
  replicas ([ADR-2350](docs/adr/2350-cloud-native-platform.md)).**
  `VMAFX_STORE_BACKEND=postgres` with `VMAFX_DB_DSN` stores jobs, attempts and
  node sessions in PostgreSQL: any replica serves any node, a node keeps
  working through a controller restart without registering again, a pulled
  job is leased and returns to the queue when its node stops renewing it, and
  a late report from a node that lost its lease is refused, so a job keeps one
  result. `vmafx-controller migrate` applies the schema and
  `vmafx-controller import-sqlite --from <file>` copies the jobs of the SQLite
  queue. `/readyz` answers not ready while the database is unreachable or its
  schema is older than the controller needs. The SQLite queue stays the
  default.


- Added the credits page `docs/credits.md`, rendered from the curated list
  `docs/credits.yaml`: every third-party project, vendored file, model, dataset,
  paper, tool, action, image and font VMAFx ships, adapts or uses, with its
  relation to the project and the licence its upstream states. `make
  docs-fragments-check` fails on page drift, an uncredited vendored or
  inherited path, an unused `LICENSES/*.txt`, a skill derived from an upstream
  with no entry, and an entry path that is gone. See ADR-2485.


- Added the required check `DCO Sign-off`: every commit of a pull request must
  carry a `Signed-off-by:` line (`git commit -s`; `git rebase --signoff` fixes a
  branch). Renovate, Dependabot and GitHub Actions bot commits in their own pull
  requests and the release pull request are exempt, and pull requests created
  before the cutoff in `scripts/ci/dco-cutoff.txt` are grandfathered. Run it locally with
  `python3 scripts/ci/check-dco.py --base origin/master --head HEAD`. See
  `docs/development/dco.md` and ADR-2462.


- **The Helm chart runs vmafx-controller as several replicas on PostgreSQL
  ([ADR-2350](docs/adr/2350-cloud-native-platform.md)).**
  `controller.store.backend: postgres` with `controller.replicas` renders a
  rolling-update controller Deployment without a volume, a CloudNativePG
  `Cluster` (`postgresql.mode: cnpg`, the operator is a prerequisite) or the
  connection URI of an external database from a Secret, and a migration Job
  that runs `vmafx-controller migrate` once per controller image. Several
  replicas bring a PodDisruptionBudget and a spread across nodes, and
  `networkPolicy.enabled` opens the database to the controller. The SQLite
  store stays the default and one replica; the chart refuses more. A new E2E
  case kills a controller replica and the node of a running job mid-job on
  kind and checks that every job is completed exactly once
  ([integration tests](docs/k8s/integration-tests.md#controller-failover-case),
  [job store and replicas](docs/development/k8s-deployment.md#controller-store)).


- **Mini retrain and a resumable stage runner for the retrain tooling** (ADR-1898, issue #1246).
  `make mini-retrain` runs extraction, feature checks, combination, training and export of
  `vmaf_tiny_v2` to `v4` and `fr_regressor_v1`, validation, registry validation and a PLCC / SROCC / RMSE
  gate on a generated 144-row corpus in about 40 seconds. Every stage writes a manifest with seed,
  digests, library versions, lock digest, container id and resource use; a killed run resumes from the
  manifests; a missing or corrupt input stops the run with the stage name before anything runs. The
  Tiny AI job runs it for changes under `ai/`, and a nightly workflow runs it too. See the runbook section 13.


- **Observability: alerts, recording rules and runbooks (RC4, ADR-2349,
  #2430).** `deploy/prometheus/vmafx-rules.yaml` is generated from the metric
  definition: recording rules for the job failure, Score error and slow Score
  ratios (5m, 30m, 1h, 6h) and the hourly score median, and eight alerts:
  `VMAFxComponentDown`, `VMAFxNoLiveNodes`, `VMAFxQueueAging`, the
  multi-window burn-rate alerts `VMAFxJobErrorBudgetBurn`,
  `VMAFxScoreErrorBudgetBurn` and `VMAFxScoreLatencyBudgetBurn` (objectives
  99 %), `VMAFxScoreRegression` and `VMAFxMetricsReadErrors`. Each links a
  runbook page under `docs/observability/runbooks/`, and its promtool unit
  test (`deploy/prometheus/vmafx-rules.test.yaml`) has a firing and a
  non-firing case; `make check-prometheus-rules` runs them with a pinned
  promtool. See [alerts](docs/development/observability.md#alerts-and-recording-rules).


- **Observability stack with Docker Compose (RC4, ADR-2349, ADR-2399,
  #2430).** `deploy/compose/observability/` runs `vmafx-server`,
  `vmafx-controller` and a CPU `vmafx-node` with Prometheus (the generated
  rules, rendered at start from `monitoring-values.yaml`, the Helm chart's
  `monitoring.slo` / `burnRates` / `alerts` keys), an OpenTelemetry Collector,
  Tempo, Loki and Grafana (the generated dashboards and linked data sources).
  The VMAFx images build from the checkout on the first `up`.
  `scripts/ci/observability-compose-smoke.sh` (`make
  observability-compose-smoke`) sends traffic and checks every component is
  scraped, the rules are healthy, every dashboard query returns data, Grafana
  is provisioned and traces reach Tempo. See
  [the guide](docs/observability/compose.md).


- **Observability: Quality, Nodes and devices, Live sessions and GPU exporter
  dashboards, linted in CI (RC4, ADR-2349, #2430).** Three more dashboards are
  generated from the metric definition: Quality (score levels per model and
  tenant, the bad tail, a score heatmap, the share below 70), Nodes and
  devices (backend per node, slot use, failures and run time per node, GPU
  memory, host memory and CPU) and Live sessions (open ScoreStream sessions,
  frames per second, outcomes, duration); the Overview links to them. One
  dashboard per vendor GPU exporter (NVIDIA dcgm-exporter, AMD
  device-metrics-exporter, Intel XPU Manager) covers utilisation, memory,
  encoder and decoder load and power, to import where that exporter runs.
  `vmafx-node` serves its GPU memory per device (`nvidia-smi` on CUDA nodes,
  the amdgpu sysfs files on HIP nodes); `vmafx-server` and `vmafx-node` serve
  the ScoreStream session families; a failed read of scraped values counts in
  `vmafx_metrics_read_errors_total`. Every dashboard passes Grafana's
  dashboard-linter `--strict` (`make lint-dashboards`, pinned release). See
  [dashboards](docs/development/observability.md#dashboards).


- **Monitoring in the Helm chart: a monitor per component, the alert rules
  with your SLOs, the dashboards (RC4, ADR-2399, #2430).** With
  `monitoring.enabled` the chart renders a ServiceMonitor for the server,
  the controller and the nodes and a PodMonitor for the operator
  (`monitoring.components` switches each), a PrometheusRule with the
  generated alerts and recording rules, and one ConfigMap per Grafana
  dashboard for the dashboard sidecar (`monitoring.dashboards`, vendor GPU
  dashboards opt-in). The SLO objectives, burn-rate windows and factors and
  the queue-age and score-regression thresholds are values
  (`monitoring.slo`, `monitoring.burnRates`, `monitoring.alerts`), checked by
  the chart's schema; `go run ./tools/obsgen -render-rules -values <file>`
  writes the same rules as a plain rule file. With `networkPolicy.enabled`,
  `networkPolicy.allow.metricsScrape` admits the scrape. The burn-rate alerts
  state their threshold as `(factor * (1 - objective))`. See
  [monitoring on Kubernetes](docs/observability/kubernetes.md).


- **Observability: one metric definition, node `/metrics`, queue and quality
  metrics, a generated Overview dashboard (RC4, ADR-2349, #2430).** Every
  Prometheus family the services serve is defined once in
  `pkg/observability/metricdef`, with its labels and their cardinality bound;
  the services build their collectors from it, and
  [the metric reference](docs/observability/metrics.md) is generated from it.
  `vmafx-node` serves `/metrics`, `/livez`, `/readyz` and `/startupz` on
  `VMAFX_HTTP_ADDR` (default `:9090`): backend and vendor, slots, running
  jobs, jobs by backend and outcome, job run time. The controller adds
  cancelled and requeued jobs (by reason), the age of each tenant's oldest
  pending job, queue wait and time to result; the server and controller add
  the quality family `vmafx_quality_score` per tenant, model and profile (the
  profile reads `none` until requests carry one); every
  component serves `vmafx_build_info`. The Overview dashboard is generated
  with the Grafana Foundation SDK (`go run ./tools/obsgen -write`) under
  `deploy/grafana/dashboards/`; it replaces `deploy/grafana/vmafx-overview.json`,
  five of whose seven queries named series nothing emits, and a test fails
  any shipped panel that queries a series nothing emits. See
  [observability](docs/development/observability.md#metrics).


- **Observability operator guide (RC4, ADR-2349, #2430).**
  [docs/observability/](docs/observability/index.md) starts with what VMAFx
  reports and ships, the three ways to install it (Helm, Compose, your own
  Prometheus and Grafana) and a first-hour checklist, and continues with a
  [tour of every dashboard](docs/observability/dashboards.md): the question
  each row answers, how to read it, and the runbook to open when an alert
  fires.


- **SLO report, usage and cost, and capacity dashboards (RC4, ADR-2349,
  #2430).** `VMAFx SLO report` shows each SLO's compliance, objective, error
  budget left and burn rate over its time range (30 days by default), from
  the objectives the rules record (`vmafx:slo_objective`).
  `VMAFx Usage and cost` counts per tenant the finished jobs, their run time
  and the scores, and prices them at `monitoring.cost.perJobSecond` (the run
  time of every finished job) and `monitoring.cost.perJob` (completed and
  failed jobs; cancelled jobs are not charged), the same for every backend,
  in `monitoring.cost.currency`; an unset price leaves its cost panels empty.
  `VMAFx Capacity` sets the nodes' measured capacity against the demand, with
  the headroom, the demand's growth and linear forecasts of the queue and the
  demand. The Helm chart ships them like the other dashboards; the Compose
  smoke test checks them. See
  [the dashboard tour](docs/observability/dashboards.md#slo-report).


- **Rust integer ADM extractor (`adm_rust`)**: builds configured with
  `-Denable_rust_features=true` contain a Rust port of the fixed-point `adm`
  extractor. It takes every option of `adm` and returns its `adm2`, `aim`,
  `adm3`, scale and debug scores bit for bit; select it with
  `VMAF_FEATURE_IMPL=rust` or `--feature adm_rust`
  ([ADM](docs/metrics/adm.md#rust-implementation-adm_rust), #1723, ADR-1713).


- **`cambi` runs in Rust, bit-identical to the C extractor (`cambi_rust`).**
  A build with `-Denable_rust_features=true` registers `cambi_rust`, a
  statement-by-statement port of the scalar `cambi.c` path (preprocessing,
  spatial mask, mode filter, sliding-histogram c-values, top-K pooling and the
  luminance model). It reads the C extractor's option table and implements
  every option except `heatmaps_path`, which it refuses at init. Its per-frame
  scores equal the C extractor's at `--precision max` on the Netflix 576x324
  pair, both 1080p checkerboard pairs, the 10-bit sparks pair and BBB
  3840x2160, for the default options and the `vmaf_v1.0.16` models' options.
  Select it with `--feature cambi_rust` or `VMAF_FEATURE_IMPL=rust`; the C
  extractor stays the default. See
  [the CAMBI page](docs/metrics/cambi.md#rust-twin) and
  [ADR-1713](docs/adr/1713-rc4-rust-extractor-framework.md).


- **`speed_chroma` has a Rust implementation that returns the C extractor's
  scores bit for bit.** Build with `-Denable_rust_features=true`, then select it
  with `--feature speed_chroma_rust` or `VMAF_FEATURE_IMPL=rust`; C stays the
  default. It covers every option the `vmaf_v1.0.16*` models set (prescale 1.0,
  0.5 and 0.6) and all four prescale methods. See
  [the SpEED page](docs/metrics/speed.md#rust-twin) and
  [ADR-1713](docs/adr/1713-rc4-rust-extractor-framework.md).


- A weekly research radar over public video-quality sources: a public source registry (`docs/research/radar/sources.yaml`), a scheduled digest workflow (`research-radar.yml`, `scripts/research/radar_collect.py`) and a documented triage procedure with a licence and patent gate ([ADR-2171](docs/adr/2171-research-radar.md), [docs/research/radar/](docs/research/radar/README.md)).


- **Rust twins of C feature extractors, selectable at run time (RC4
  framework, [ADR-1713](docs/adr/1713-rc4-rust-extractor-framework.md)).**
  A build with `-Denable_rust_features=true` links one Rust archive into
  `libvmaf` and registers each Rust twin as `<name>_rust` next to its C
  extractor, with the C extractor's options, feature names and flags.
  `VMAF_FEATURE_IMPL=rust` makes every registration path use the twin where
  one exists and logs the C fallback where none does; `--feature psnr_rust`
  picks a twin directly; the JSON report's `feature_backends` names the
  extractor that ran. The C extractors stay the default. The first twin,
  `psnr_rust`, returns the C scores bit for bit on the Netflix pair, both
  checkerboard pairs, a 10-bit pair and 200 frames of 4K.
  `scripts/ci/rust_twin_diff.py` proves a twin equal to its C extractor (same
  binary, equal doubles on every metric of every frame), the `Rust` workflow
  runs clippy on every workspace crate, checks the cbindgen header and runs the
  new `rust` Meson suite. See
  [Rust extractor framework](docs/development/rust-extractor-framework.md).


- **The prediction step of the `vmaf_v1.0.16*` models can run in Rust.** A build
  with `-Denable_rust_features=true` and `VMAF_FEATURE_IMPL=rust` evaluates the
  feature normalisation, chroma correction, nu-SVR, score transform and clip in
  the new `vmafx-predict` crate. The score is bit-identical to the C predictor
  at `--precision max`; the C predictor stays the default. See
  [Models](docs/models/overview.md#rust-prediction-experimental).


- **The integer `motion` extractor has a Rust twin, `motion_rust`, that returns the C
  extractor's scores bit for bit.**
  With `-Denable_rust_features=true`, `VMAF_FEATURE_IMPL=rust` (or
  `--feature motion_rust`) computes `motion_sad_score`, `motion2` and `motion3`,
  including the five-frame window and the moving average, in Rust; the default
  stays the C extractor. See [Motion](docs/metrics/motion.md#rust-implementation).


- Every score now carries a full provenance record (#2142, ADR-2073): library,
  ABI and build (commit, compilers, build options, the strict floating-point
  policy, backends, a `build_id` digest), the SIMD level and device, the
  context options, the frames, every model with the SHA-256 of its bytes and
  its overrides, the extractor, options, backend and exactness class of every
  feature (option-decorated names included), the command line, and a digest
  over the record and every score's bits. The C API reads it with
  `vmafx_context_provenance()`, `vmafx_context_model_provenance()`,
  `vmafx_context_feature_provenance()`, `vmafx_feature_provenance()`,
  `vmafx_context_annotation()` and `vmafx_context_provenance_json()`, and adds
  to it with `vmafx_context_annotate()` and `vmafx_context_set_encode_record()`
  (the digest of a VMAFx/pelorus#81 encode record). ABI 0.1.5.
- `vmafx_report_write()` writes a report with the record: a `provenance`
  object and `score_format` in JSON, a `<provenance>` element in XML, CSV and
  SUB unchanged with an optional `<path>.provenance.json` sidecar
  (`vmaf --provenance-sidecar`). Every `vmaf` report and every report an API
  user writes through `vmaf_write_output()` carries it; the scoring server and
  both MCP servers return it.
- `vmaf --verify-provenance <report>` re-runs the command line a JSON report
  recorded and compares every configuration field and score bit for bit,
  naming the first difference (exit 0 match, 1 difference, 2 cannot check);
  `vmafx_report_open()`, `vmafx_report_field()` and `vmafx_report_verify()`
  do the same for API users. Documented in `docs/usage/provenance.md`.


- `vmafx-server` raises two framework defaults that did not fit scoring: the
  gRPC receive limit is now 64 MiB (a 1080p `ScoreStream` frame pair is 6.2 MB
  and the old 4 MiB limit rejected it) and the HTTP write timeout is 15 minutes
  (a synchronous `POST /v1/score` that took more than 60 s lost its
  connection). `VMAFX_GRPC_MAX_RECV_SIZE` and `VMAFX_HTTP_TIMEOUTS_WRITE`
  still override both. New page `docs/server/configuration.md` documents the
  source precedence (environment over file over defaults), the underscore rule
  of the environment transform and every server limit (#1251).
- Server log lines share one field set (`request_id`, `rpc`, `route`, `model`,
  `backend`, `duration_s`, `error`) defined in `pkg/observability`; the legacy
  `POST /v1/score` path logs it today (#1251).


- The scoring server's `ScoreRequest` (gRPC and `POST /v1/score`) takes
  `options`: raw `.yuv` geometry, backend and device, threads, subsample,
  precision, features, tiny model and more, so the server can score raw
  `.yuv` pairs. Every scoring response, the `ScoreStream` aggregate included,
  carries `provenance`: the library record, the model the server loaded with
  its SHA-256, the backend receipt and the precision. The versioned contract
  and its compatibility policy are on `docs/server/api-contract.md`; a
  contract test (`meson test test_vmafx_score_contract`) checks that the CLI,
  the C API, gRPC and REST give the same score bit for bit (#2155).


- **GitHub Sponsors is live (ADR-2689).** `SPONSORS.md` lists the four monthly tiers ($5, $25, $100, $500), the one-time amounts and the sponsors by tier, and the new documentation page "Support VMAFx" explains what sponsorship pays for (CI time, cloud GPU test time, an AI workstation). Sponsorship buys recognition only; there is no paid support or paywall. Euro routes: Ko-fi now, Patreon coming.


- **Preview of the VMAFx C API, generated from one definition (RC4,
  ADR-1852).** New headers `vmafx/vmafx.h` and `vmafx/libvmaf_bridge.h` with
  `vmafx_context_create` / `vmafx_context_destroy`, version, provenance,
  extractor and feature-score queries and errors that name what failed; a
  standard-library Python binding (`bindings/python/vmafx/`); and
  `scripts/codegen/vmafx-api.py`, which generates the headers, the binding,
  the ABI layout test, the reference page and the `libvmaf.h` shims for
  `vmaf_init`, `vmaf_close`, `vmaf_version` and `vmaf_feature_score_at_index`
  from `core/api/vmafx.toml`. `libvmaf.h` behaviour is unchanged. ABI 0.1 is a
  preview until `v1.0.0`. See [the VMAFx API page](docs/api/vmafx/index.md)
  and [API generation](docs/development/api-generation.md).


- **VMAFx API generator: header split, symbol versions and ABI gates (RC4,
  ADR-1852).** The VMAFx API headers follow the design's layout:
  `vmafx/vmafx.h` includes `version.h`, `types.h`, `error.h`, `context.h`,
  `device.h`, `frame.h`, `model.h`, `score.h`, `provenance.h`, `report.h`,
  `dnn.h` and `mcp.h`, each usable on its own; `vmafx/libvmaf_bridge.h` stays
  optional. On Linux every `vmafx_*` symbol carries the version node of the ABI
  minor that introduced it (`VMAFX_0.1`). The definition format
  (`core/api/vmafx.toml`) gains header groups, callbacks, flag sets, fixed
  arrays, nested sized structs, per-entry `since` and `deprecated`, and option
  groups; `scripts/codegen/vmafx-api.py` also writes the linker version
  script, the Windows export list, the exported-symbol list, the header
  install list, one reference page per header, and a changelog draft
  (`--changelog <ref>`). New Meson tests check the definition is append-only
  against the merge base and run the generator's own tests. See
  [API generation](docs/development/api-generation.md).


### Changed

- **The `actionlint` hook and `make lint-actions` cannot hang
  ([ADR-2199](docs/adr/2199-actionlint-bounded-run.md)).** They run actionlint
  through `scripts/ci/run_actionlint.py`, which gives it 90 seconds
  (`ACTIONLINT_TIMEOUT_S`) and fails with exit 124 and the cause named instead
  of hanging a commit or a push when a `run:` script is larger than the pipe
  actionlint writes it to (a user over `fs.pipe-user-pages-soft`).


- **Integer ADM runs on NEON at every scale on aarch64 (Netflix/vmaf
  `8bc5a5c6a`, `b41d2340a`).** The contrast masking of scale 0 and of scales
  1 to 3, the DWT of scales 1 to 3 and the decouple of scales 1 to 3 (at an
  enhancement gain limit of 1) have NEON kernels. They return the scalar
  kernels' bits: scores are byte-identical at every `--cpumask` setting
  ([Arm backend](docs/backends/arm/overview.md)).


- **ADR-2167 is Accepted.** The `-qpfile` handling of libx264 (offsets applied through `quant_offsets`)
  shipped in #2385 and the maintainer accepted the decision on 2026-10-07; the ADR status and index
  say so. No code changes.


- **`scripts/ci/AGENTS.md` is split into four area indexes.** The generated index sat at 15,982 of
  16,000 bytes. `scripts/docs/agents_index.py` now supports areas: `AGENTS.d/_area-<slug>.md` and an
  `area:` key on each page. `AGENTS.md` lists the areas and `AGENTS-<slug>.md` holds the pages of
  each (gates, release, tidy, tests), every file well under the unchanged limit. Other directories
  render as before. `docs/development/agents-index.md` describes it.


- **CI runs the tier a pull request owes, not the whole suite on every push
  ([ADR-2169](docs/adr/2169-ci-fewer-runs.md)).** A pull request from this repository
  (Renovate included) runs lint, format, the fast suite and the governance gates; the
  platform, GPU, coverage, container and FFmpeg lanes run on the master push, on pull
  requests from forks and on an own pull request labelled `ci: full`. The generated
  release pull request runs the release contract only until it carries
  `autorelease: cut`. Draft pull requests start no job. Renovate groups minor and patch
  updates into one weekly pull request and rebases only on conflict; security updates
  still open at any time. See "Which jobs run when" in `docs/development/ci.md`.


- **The distributed platform has an architecture for running without local state
  ([ADR-2350](docs/adr/2350-cloud-native-platform.md)).** Job state and node
  sessions move from the controller's embedded SQLite queue to PostgreSQL, so
  several controller replicas can serve any node; nodes keep the gRPC protocol
  and claim work through leases; River runs retries and follow-up steps; node
  pools scale on queue depth with KEDA; results become signed OCI artifacts; job
  events go out as CloudEvents; and the protobuf, CRDs, OpenAPI and Helm values
  schema are generated from a platform definition. A standalone profile keeps
  SQLite. This change only records the decision: the chart, the controller and
  the documentation pages say that the single-replica SQLite queue is
  transitional. Nothing in a running installation changes.


- **Controller job metrics are per tenant (RC4, ADR-2349).**
  `vmafx_controller_jobs_submitted_total`, `_completed_total`,
  `_failed_total`, `vmafx_controller_jobs_pending` and `_jobs_running` carry
  a `tenant` label, so a query on the bare series returns one series per
  tenant: wrap it in `sum()` for the total. A repeated result report of a
  finished job is no longer counted again, and `vmafx_server_score_duration_seconds`
  buckets reach 30 minutes (0.05 s to 1800 s) so long clips land in a bucket.
  `vmafx-server` no longer serves the controller's job counters, which it
  never incremented.


- **The documentation renders formulas as math
  ([ADR-2705](docs/adr/2705-docs-math-katex.md)).** Write `$...$` inline or
  `$$...$$` as a display block; KaTeX 0.18.9 typesets it from files the site
  serves itself, with no third-party host. The metric, backend, API, usage and
  development pages that wrote formulas as code text now use TeX, and
  `scripts/docs/check_math.py` fails the docs build on a formula KaTeX rejects.
  See [Writing math](docs/development/docs-site-design.md#writing-math).


- **The route for Go saliency inference is decided.** [ADR-2377](docs/adr/2377-go-saliency-through-mobilesal-binding.md)
  records that `vmafx-tune` runs the saliency model through the core's MobileSal extractor and the
  generated Go binding, with no second ONNX Runtime integration. No code changes yet; RC5 implements
  `--use-saliency` and `--saliency-aware` on it before the Python `vmaf-tune` is deleted.


- **The chart no longer sets `VMAFX_BACKEND` on the scoring server.** The
  server's Deployment, StatefulSet and Job carried it, but `vmafx-server`
  never read it: it takes its backend from each request's `backend` score
  option. The install notes no longer print a `BACKEND` line, and the server
  container has an `env` list only when `env` holds values. Nodes keep
  `VMAFX_BACKEND` from `gpu.vendor`. The unused named templates
  `vmafx.podSpec`, `vmafx.containerSpec`, `vmafx.volumes` and
  `vmafx.sidecarContainer` are removed; no chart template included them.
  `vmafx-mcp` no longer copies `VMAFX_LOG_LEVEL` and `VMAFX_LOG_FORMAT` into
  `LOG_LEVEL` and `LOG_FORMAT`, which nothing read. The upgrade notes are in
  `docs/development/k8s-deployment.md`
  ([ADR-2350](docs/adr/2350-cloud-native-platform.md)).


- **The Helm chart's values schema checks Kubernetes fields with the
  Kubernetes 1.26 types, and the values file and schema are generated
  ([ADR-2350](docs/adr/2350-cloud-native-platform.md)).** `values.yaml` and
  `values.schema.json` are written from `api/vmafx-platform.toml`;
  `values.yaml` is unchanged and the schema keeps its rules in a uniform
  layout. The values the chart copies into pod specs now have the types the
  oldest supported Kubernetes release gives them: `tolerations`, `affinity`,
  `topologySpreadConstraints`, `podSecurityContext`, `securityContext`, the
  probes, `node.volumes` and `node.volumeMounts`, the update strategies,
  `envFrom`, `ingress.tls`, node selectors, labels and annotations. `helm
  install`, `helm upgrade` and `helm lint` now refuse a value Kubernetes would
  refuse, such as `tolerationSeconds: "60"`, a spread constraint without
  `topologyKey` or a node selector value `1`, and name the key; they check it
  even when the workload that uses it is disabled. The upgrade notes in
  `docs/development/k8s-deployment.md` list what each key refuses. Resource
  quantities may now be decimal numbers. The chart now declares
  `artifacthub.io/license: EUPL-1.2 AND Apache-2.0`, because the schema
  carries the Apache-2.0 Kubernetes type schemas, and ships
  `THIRD-PARTY-NOTICES.txt` with their attribution and the Apache-2.0 text
  ([ADR-2673](docs/adr/2673-chart-licence-kubernetes-schemas.md)).


- **The Windows icx-cl and icpx builds no longer print an override warning on every compile.** The strict floating-point line of `intel-llvm-cl` is `/fp:precise /clang:-fno-fast-math /clang:-fcomplex-arithmetic=full /clang:-ffp-contract=off` instead of `/fp:precise /Qfma-`, and the SYCL compiles and device link of the MSVC build take the `-fno-fast-math -fcomplex-arithmetic=full` reset the Linux icpx already has. Same arithmetic: equal compiler front-end arguments apart from the complex-arithmetic token, equal predefined macros, byte-identical objects and device bitcode ([Research-2170](docs/research/2170-windows-strict-fp-spelling-2026-10-07.md), [ADR-2170](docs/adr/2170-warnings-are-errors-per-leg.md)).


- **`M_PI` and `M_E` come from `<math.h>` on every platform (Netflix/vmaf
  `4e150067b`).** The build defines `_USE_MATH_DEFINES` on Windows (MSVC,
  clang-cl, icx-cl and MinGW-w64), and the local copies of the constants
  (fourteen in the feature sources, the SYCL twins included, and four in
  tests) are gone. Scores are unchanged: the copies held the same double.


- `test_dnn_session_api.c` spells its invalid session pointer as the literal `0xdeadbeefULL`, which MSVC accepts without C4312 and clang-tidy accepts without `performance-no-int-to-ptr`; the value is unchanged.


- **A static MSVC build installs `vmaf.lib` and `vmafx.lib` (Netflix/vmaf
  `3b4dd350e`).** MSVC, clang-cl and icx-cl builds with
  `--default-library=static` name their installed libraries the way the MSVC
  linker opens `-lvmaf` / `-lvmafx`, instead of Meson's `libvmaf.a` /
  `libvmafx.a`; consumers such as FFmpeg's MSVC toolchain link them without
  renaming ([Library files of an MSVC build](docs/getting-started/building-on-windows.md#library-files-of-an-msvc-build)).


- **A compiler or linker warning now fails the Windows MSVC legs that print none.** `Windows MSVC+CUDA`,
  `Windows ARM64 MSVC` and `Windows MSVC+CUDA (full)` configure with `scripts/ci/werror-args.sh msvc`
  (`-Dwerror=true`: `/WX` on every `cl.exe` compile, `-WX` on every `link.exe` link, `--Werror
  all-warnings` on every nvcc fatbin). The icx-cl leg (`Windows MSVC+SYCL`) is not at zero yet and is
  listed with its count in [the CI overview](docs/development/ci.md#warnings-are-errors-adr-2170). See
  [ADR-2170](docs/adr/2170-warnings-are-errors-per-leg.md).


- The Windows MSVC builds no longer print the C runtime, declaration and
  command-line warnings: `strdup`, `close`, `sscanf`, `getenv`, `_wfopen`,
  `_wopen` and `_open` are called through the CRT's own non-deprecated
  spellings (`src/compat/crt_portable.h`, `_wfsopen`, `_wsopen_s`, `_sopen_s`);
  `strncpy` became a bounded `memcpy`; `thread_locale.cpp` declares its state as
  the `struct` the header names (C4099); the pdjson test copies no longer rename
  `push` / `pop` as macros, which broke `#pragma warning(push)` in the system
  headers (C4615, C4079, C4081); unknown `STDC FP_CONTRACT` / `clang fp
  contract` pragmas are not given to `cl.exe` (C4068); the SIMD libraries get the
  `-mavx2` family only from compilers that take it (D9002). Configure `cl.exe`
  builds with `-Dc_std=none` (D9025; `docs/getting-started/install/windows.md`).


- The scoring sources compile without an MSVC warning (about 8,000 C4305 / C4244 /
  C4267 / C4334 sites across the CPU extractors, their SIMD twins and the CUDA
  host code): every implicit double-to-float, 64-to-32-bit and size_t-to-int
  conversion is written out, and a float table carries the `f` suffix only where
  the literal converts to the same bits. No score changes: each touched
  translation unit compiles to the same machine code as before (checked with
  GCC on x86-64, clang on aarch64 and the CUDA host objects), and the Netflix
  golden gate is unchanged.


- The unit tests compile without an MSVC warning (about 71,000 per Windows job
  before): float tables carry the `f` suffix (every literal checked to equal the
  value the implicit double-to-float conversion gave), narrowing conversions are
  explicit, and C test cases are declared `(void)`. No test value or tolerance
  changed.


- **POSIX-only build parts stay off Windows, and `preflight.sh --stage msvcism`
  refuses an unguarded POSIX header
  ([ADR-2646](docs/adr/2646-posix-only-build-options.md)).** On Windows,
  `-Denable_mcp=true` and `-Dfuzz=true` now stop configure with an error that
  names the POSIX dependency instead of failing in the compiler, and the
  `vmaf_vpl` tool is not looked for (configure prints why). The `msvcism` stage
  fails on a `<unistd.h>`, `<dlfcn.h>`, `<sys/socket.h>` or other POSIX-only
  include outside a platform conditional in a source the Windows build compiles
  (`scripts/dev/find-posix-only-headers.py`), and fails when that scan cannot run.


- **Packaging**: ADR-2383 lets the macOS and Windows package channels (Homebrew, winget and the others in the distribution manifest) take their artifacts from the attested native release pipelines, as a bounded exception to the container-only rule; Linux artifacts stay container-built.


- **The vendored Pelorus conformance fixture reads its files back with `_fsopen(..., _SH_DENYNO)` on Windows.** `scripts/sync-pelorus-interop.sh` pins `11e183ec0aed` (VMAFx/pelorus #91, fixing #90): `fixture_equals()` and `fixture_path_exists()` of `core/test/test_pelorus_interop.c` no longer call the deprecated `fopen()` there, which icx-cl reported. No behaviour or ABI change (ABI 1.3).


- **The vendored Pelorus interop sources are re-vendored at the pelorus commit that opens the qp-report CSV with `_wfsopen`.** `scripts/sync-pelorus-interop.sh` pins `4aae30711c65` (VMAFx/pelorus #89, fixing #88): `open_utf8()` calls `_wfsopen(..., _SH_DENYNO)` instead of the deprecated `_wfopen()` on Windows, with the same sharing. The mirror's local `_wfsopen` edit is gone; every vendored file is byte-identical to pelorus again apart from the banner and the include rewrite. No behaviour or ABI change (ABI 1.3).


- **The Go binaries' environment, its documentation and the chart's
  `VMAFX_*` entries are generated from one definition
  ([ADR-2350](docs/adr/2350-cloud-native-platform.md)).** The `[[config]]`
  entries of `api/vmafx-platform.toml` list every environment variable
  `vmafx-controller`, `vmafx-server`, `vmafx-node`, `vmafx-operator`,
  `vmafx-mcp` and `vmafx-tune` read, the framework's `VMAFX_LOG_*` and
  `VMAFX_OTEL_*` keys included. They generate each binary's golusoris
  CompoundKeys, one environment table per binary (on its page and in
  `docs/usage/env-vars.md`, with the chart values that set each variable),
  and `deploy/helm/vmafx/templates/_config.gen.tpl`, which writes every
  `VMAFX_*` entry of every chart workload, conditions and Secret references
  included. `helm template` output is unchanged for the CI, end-to-end and
  documented values sets; topic pages link to the generated tables instead of
  repeating rows. `controllerclient.CompoundKeys` is deprecated: the binaries
  no longer read it.


- **The custom resources are generated from the platform definition
  ([ADR-2350](docs/adr/2350-cloud-native-platform.md)).** `api/vmafx-platform.toml`
  now also declares the `vmafx.dev/v1` resources `VmafxJob`, `VmafxNode`,
  `VmafxModelTraining` and `VmafxTenant`; `scripts/codegen/vmafx-api.py` writes
  their Go types under `api/vmafx/v1`, and `scripts/codegen/crd_generate.py`
  runs controller-gen (pinned in `go.mod`) for the deepcopy code, the CRDs and
  the operator's RBAC role. `deploy/helm/vmafx/crds/` is the only CRD tree:
  `config/crd/bases/` and the per-kind roles under `config/rbac/` are gone, and
  `VmafxTenant` has a Go type. The installed schemas are unchanged apart from
  descriptions; within `v1` a resource now only grows, which a compatibility
  check enforces. `VmafxJobSpec.Priority` is an `int32` in Go, as the schema
  already was, and the generated role includes the leader-election lease the
  chart already granted.


- **The gRPC services are generated from one platform definition
  ([ADR-2350](docs/adr/2350-cloud-native-platform.md)).** The scoring service
  and the controller are described in `api/vmafx-platform.toml`, from which
  `scripts/codegen/vmafx-api.py` writes `proto/vmafx/v1/vmafx.proto` and
  `proto/vmafx/controller/v1/controller.proto`; `vmafx_api.proto` moves to
  `proto/vmafx/v1/`. One buf configuration (`buf.yaml`, `buf.gen.yaml`, run by
  `scripts/codegen/proto_generate.py`) generates the Go bindings at their
  existing import paths, and `cmd/vmafx-controller/proto/` with its protoc
  script is gone. The wire format is unchanged: every message, field number,
  enum value and RPC is the same, and `buf breaking` with the wire and JSON
  rules guards it from now on. gRPC reflection reports the new file names
  (`vmafx/v1/vmafx.proto`, `vmafx/controller/v1/controller.proto`).


- **The praetor governance engine moves from `7458a220e1c9` to `3a766f2d56ad`
  ([ADR-2784](docs/adr/2784-praetor-pin-3a766f2d.md)).** `reuse lint` now runs in CI as the
  required `REUSE lint` job of praetor's `.github/workflows/reuse.yml` (actions pinned to commits),
  replacing the step of the Pre-Commit job; the commit hook and `make lint` still run it locally.
  The engine's audit also checks the order of `REUSE.toml` annotations and that `LICENSE` holds the
  declared licence's text, both of which pass. Its new build-warnings gate (HISS-10) reads the
  warnings-as-errors switch only as a literal on the build command, so the 16 workflows whose lanes
  it reads as ungated are declared in `.config/lint-exceptions.d/HISS-10.toml` until RC4 spells the
  switch there and gates the remaining legs. Regenerated by the engine: both managed workflows
  (draft step on `bash`), the API gate program and its new `placeholder.go`, the figure-engine
  hook and notes, and the DevContainer bundle. The HISS baseline stays at 0.


- **The praetor governance engine moves from `afb739ed81f3` to `7458a220e1c9`
  ([ADR-2440](docs/adr/2440-praetor-pin-7458a220.md)).** The emitted `praetor-api.yml` and
  `praetor-docs.yml` now push only on `master`, listen for `ready_for_review` and stop a draft
  pull request with a failing first step, so they no longer start on pushes to other branches.
  The two `push_branch_exceptions` of `.github/ci-tier.json` are gone, and a new routing-contract
  test pins the draft-stop step shape. The engine also refuses a repository whose
  `core.hooksPath` leaves the managed hooks directory, and counts model names such as `A380`
  correctly in the caveman article density. `Go API Compatibility` joins the aggregator's strict list, because the workflow now runs on `ready_for_review`. Regenerated by the engine: both workflows, the
  DevContainer bundle and the agent evasion hook. The HISS baseline stays at 0.


- **The praetor governance engine moves from `04cc813ff054` to `afb739ed81f3`
  ([ADR-2321](docs/adr/2321-praetor-pin-afb739ed.md)).** Praetor now lints every tracked
  nested `AGENTS.md` in the internal register; the 19 nested files and 195 `AGENTS.d/` pages
  that failed `praetorctl caveman check --kind=context` are rewritten, and the generated index
  header follows. The audit compares the declared SLSA Build Level (3) with the one the
  workflows reach (2): the gap is declared in `.config/lint-exceptions.d/HISS-11.toml`, which
  `scripts/ci/praetor_tidy_coverage.py` renders into `.standards.yaml` (expires 2027-01-04).
  Engine-written files regenerated: the Markdown gate lock (katex 0.19.0), the DevContainer
  bundle, `.paperclip/harness.json` and `rules.md`, the agent evasion hook; the register block
  no longer names skills the repository does not carry. The HISS baseline stays at 0.


- **Praetor's text-register skills are installed and tracked.** `social-text`, `caveman` and
  `adhd-format` live under `.agents/skills/` (no longer ignored) with their projections in
  `.claude/skills/`, byte-identical to the praetor pin; the register block of `AGENTS.md` and the
  vendor context files name them again. No code changes.


- **The roadmap and release pages list the full RC4 and RC5 scope.**
  [ADR-2342](docs/adr/2342-rc-map-amendment-2026-10.md) records the scope decisions of 2026-10-06 and
  2026-10-07 (RC4 work packages for bindings, the FFmpeg series redesign, input formats, engineering
  principles, the observability package and the cloud-native platform; RC5 live alignment,
  interlaced video, region masks, container input, bits per pixel, HandBrake and the run-result
  timeline; the 1.1 to 1.3 placement of the newer issues), and `docs/roadmap.md` and
  `docs/development/release.md` follow it. No candidate number or milestone changes.


- **The plan for reference-exact extractors is written down.**
  [ADR-2343](docs/adr/2343-reference-exact-default-compat-mode.md) records that RC7 proves every
  extractor against its original implementation, that the default becomes reference-exact with
  Netflix's behaviour as a named compatibility mode the golden gate runs in, and that the RC9
  retrain trains on reference-exact features. The roadmap, the release page and the retrain
  runbook say so. No extractor or score changes yet.


- **A pull request no longer carries the rendered changelog, ADR index or rebase
  notes ([ADR-2197](docs/adr/2197-render-generated-docs-at-landing.md)).** It
  adds fragments: `changelog.d/<section>/*.md`,
  `docs/adr/_index_fragments/<slug>.md` and the new
  `docs/rebase-notes.d/<slug>.md`. `CHANGELOG.md`, `docs/adr/README.md`, the
  ADR by-tag and title pages and `docs/rebase-notes.md` are written by
  `make docs-render` when pull requests land (the merge train per batch, the
  release cut); `scripts/ci/deliverables-check.sh` refuses a pull request that
  edits one. `docs/adr/_index_fragments/_order.txt` is frozen: later rows follow
  in the order they landed. GitHub no longer reports such a pull request as
  conflicting after master moves.


- The backend receipt of the JSON report (`backend_used`, `feature_backends`)
  is written by the library's report writer instead of being spliced into the
  file by the `vmaf` CLI, so reports written through the API carry it too;
  `feature_backends` now comes before `backend_used` in the file. The
  `provenance` object of WP8 keeps its six members and gains the full record
  (#2142, ADR-2073).


- **Update `github.com/riverqueue/river` to `v0.49.0`.** Updates the River queue
  module along with its `riverdriver`, `riverdriver/riverpgxv5`, `rivershared`,
  and `rivertype` subpackages in `go.mod` and `go.sum`.


- **Rust replaces the host-side C and C++ by 3.0 ([ADR-2478](docs/adr/2478-rust-core-migration.md)).** The public C ABI stays and is exported from Rust; the C implementation of each layer is the differential oracle until it is deleted; native GPU device sources and C-only host glue stay on an exception list. The C++23 core item of the 2.0 plan is superseded. Phases, milestones 2.1 to 2.5 and 3.0, and the epic [#2567](https://github.com/VMAFx/vmafx/issues/2567) are in the [roadmap](docs/roadmap.md). No code path changes.


- The MCP scoring tools (`vmaf_score`, `vmaf_score_encoded`,
  `describe_worst_frames`) use the library default model when `model` is
  omitted (`vmaf_v1.0.16_3d0h` in this release) instead of `vmaf_v0.6.1`;
  pass `model="version=vmaf_v0.6.1"` to reproduce earlier numbers. Their
  validation follows the generated schema: `threads` 0 (single-threaded) is
  accepted, a `feature` list with a non-string or empty entry is refused
  instead of filtered, and errors name the argument (`invalid tiny_crf 64:
  must be <= 63`).
- The scoring server returns lossless scores (`precision` `max`) by default,
  so its scores equal the CLI's and the C API's bit for bit; a request may
  still ask for another precision. A request body with a field the contract
  does not know is refused with 400.
- `vmaf --help` lists every option with its values and default, generated
  from the API definition.


- The deliverables gate (`scripts/ci/deliverables-check.sh`) and the pull
  request template recognise `small PR (ADR-2461)`: for a pull request of at
  most 100 changed lines in one top-level directory that touches no source of
  `core/`, public header, CLI, build option, golden test, FFmpeg patch or ADR,
  the research digest, decision matrix, `AGENTS.md` note and rebase note are
  waived. The marker is refused, with the reasons, when the diff does not
  qualify. See `docs/development/pr-body-sentinel-guide.md`.


- **Citing an ADR in source no longer edits a shared registry
  ([ADR-2200](docs/adr/2200-source-adr-citations-derived.md)).** The source
  ADR-citation gate derives each binding from the tree; `scripts/ci/source-adr-citations.json`
  keeps only the retired and fixture records and `--write` is gone.


- **SpEED filters only the decimated samples on x86 too, with an AVX2
  vertical pass (Netflix/vmaf `ad42c532`, `9cb9479f`).** `speed_chroma` and
  `speed_temporal` evaluate the anti-alias filter at the samples the 16x
  decimation keeps on every target, where x86 used to filter the whole plane
  and then decimate. The vertical pass runs on AVX2 where the host has it.
  Scores are byte-identical to before at every `--cpumask` setting
  ([SpEED CPU SIMD dispatch](docs/metrics/speed.md#cpu-simd-dispatch)).


- `docs/state.md` records, for each of the fork's open Netflix/vmaf pull
  requests #1631 to #1668, whether the fork already carries the fix, covers it
  by another route or is not affected, with the file, test or ADR that shows it,
  and notes that #1634 (clip the integer AIM score) was closed because the fork
  keeps integer AIM unclipped. Documentation only.


- **The macOS Metal leg is gated, and the SYCL spill probe no longer fills the build log.** Meson
  1.12 names `-lc++` twice on every link that carries an Objective-C++ object (224 ld64
  warnings per run); the Metal links now tell ld64 duplicate libraries are expected
  (`-Wl,-no_warn_duplicate_libraries`, reason in `core/src/metal/meson.build`), so the leg takes
  `werror: true`. The AOT compile of `scratch_check.cpp`, whose deliberate register-spill kernel
  makes the device compiler warn on every target, runs through `core/src/sycl/run_captured.py`,
  which prints that output only when the compile fails
  ([ADR-2170](docs/adr/2170-warnings-are-errors-per-leg.md)).


- **A compiler or linker warning now fails the CI leg that prints none today.** The gated legs of the
  build matrix (gcc, clang, Apple clang, icx / icpx, MinGW, CUDA and HIP builds), the ASan, UBSan and
  TSan builds, and the libvmaf builds of the Go, Rust and FFmpeg jobs pass `-Dwerror=true` and the
  linker's fatal-warnings switch through `scripts/ci/werror-args.sh`; with it `-Dwerror=true` also
  reaches the nvcc (`--Werror all-warnings`) and hipcc (`-Werror`) device compiles. Legs that are not
  at zero yet stay as they were and are listed with their cause in
  [the CI overview](docs/development/ci.md#warnings-are-errors-adr-2170). Release builds and container
  images do not use the switch. See [ADR-2170](docs/adr/2170-warnings-are-errors-per-leg.md).


- **Host fences wait on a condition variable, and the MSVC builds have a timed
  wait.** `vmafx_fence_wait()` on a host fence, and `vmafx_window_wait()`,
  sleep until the fence is signalled instead of polling every 50 µs (1 ms on
  Windows); on Linux the deadline is kept on `CLOCK_MONOTONIC`. The Win32
  pthread shim of the MSVC, clang-cl and icx-cl builds gains
  `pthread_cond_timedwait()`, so `test_thread_pool_backpressure` now builds and
  runs on the MSVC lanes too
  ([Threads on MSVC](docs/getting-started/building-on-windows.md#threads-on-msvc)).


- **A Windows SYCL zip regression is seen before it merges, and a cut needs every tester leg green.**
  A pull request now builds the x64 SYCL tester zip when it changes anything that leg reads
  (selector `windows_tester_zip_sycl`), in the light CI tier too (`own_input_lanes` of
  `.github/ci-tier.json`). `scripts/release/check-candidate-legs.py` requires every Windows
  zip, macOS bundle and tester-image leg to be green on the exact commit, and the Release
  Script Contract runs it on the cut pull request; the release guide lists the dispatches
  to make first. See ADR-2198.


- **icx and the clang-cl style drivers stop warning about our own compile flags.** `icx` and `icpx`
  reported `-ffp-contract=off` after `-fp-model=precise` as `-Woverriding-option` on every compile
  (7,300 times in one CI leg). The strict policy now spells `-fp-model=precise -fno-fast-math
  -fcomplex-arithmetic=full -ffp-contract=off`; on 182 translation units of this tree the objects
  are byte-identical to the old spelling ([ADR-2170](docs/adr/2170-warnings-are-errors-per-leg.md)).
  clang-cl and icx-cl are no longer offered `-pedantic`, `-fvisibility=hidden` and
  `-fvisibility-inlines-hidden`, which they ignored with a warning per compile.


- **Option tables, extractor tables and tag declarations no longer print compiler warnings.**
  The clang, gcc, icpx and Apple clang legs reported `-Wmissing-field-initializers`
  (`{NULL}` / `{0}` option terminators, positional test tables), `-Wreorder-init-list` and
  `-Wc99-designator` (the Metal option and extractor tables), `-Wmismatched-tags`
  (`VmafThreadLocaleState`, declared `struct` in the C header and `class` in the C++ file),
  `-Wimplicit-fallthrough`, `-Wkeyword-macro` (vendored cJSON redefining `true` / `false` in
  C23), `-Wtautological-constant-out-of-range-compare` (`vmaf_next_fex_capacity()` on 64-bit
  hosts) and `-Wmacro-redefined` (`DIV_ROUND_UP` in the HIP ADM twin). Every fix is
  value-preserving: initialisers are reordered or completed, `[[fallthrough]]` replaces
  comments, the capacity check compares in `size_t`. No score, option default or exported
  symbol changes.


- **Unused code, ignored attributes and deprecated calls no longer print compiler warnings.**
  Test tables and helpers that only a skipped or disabled configuration reaches are compiled only
  there (`-Wunused-function` / `-Wunused-variable` in the ms_ssim_decimate, cambi, ssimulacra2 and
  read_pictures tests, the registry helpers of `model_loader.c` on Windows, the HIP error mappers
  without `hipcc`); `VMAF_EXPORT` is empty for GCC on MinGW, where the visibility attribute was
  ignored and drew a warning under LTO; the Win32 pthread shim spells `__stdcall` only in the host
  pass of a SYCL build; `VmafRef` is constructed with count 1 instead of calling the deprecated
  `std::atomic_init()`; the ONNX Runtime headers are `-isystem`; and a test that linked with an
  explicit `link_language : 'cpp'` no longer repeats `-lc++` on macOS. No score, symbol or
  option changes.


### Fixed

- **The API generator's format test compares against the pinned
  clang-format.** It ran whichever clang-format was installed, and the hosted
  Linux image's 18.1.3 formats the generated `*_INIT` macros differently from
  the 23.1.2 the repository pins, so the test failed on every hosted leg. It
  now uses only the pinned major (`VMAFX_CLANG_FORMAT` names one explicitly)
  and skips, naming the version it found, otherwise; the Tooling Tests job
  installs the pinned release.


- CI: the required gates that share a matrix (`Linux Intel LLVM`, `macOS Clang+Metal`,
  `Windows MSVC+CUDA (full)`, `FFmpeg Ubuntu gcc`, `FFmpeg macOS clang`) judge their own
  leg's job instead of the matrix aggregate, so one failing leg no longer turns the other
  legs' required checks red (`scripts/ci/gate_leg_result.py`).


- **Eighteen test files compare exact results with `core/test/float_bits.h`, and four CodeQL findings in tests are fixed in code (no alert dismissed).**
  `cpp/equality-on-floats` (alerts 1404-1405, 1411-1415, 1417-1458, 1482-1487, 1491-1492, 1496-1498, 1484-1485): the Metal math replays, the DNN tests, `test_predict`, `test_cambi_full_ref_wide_source`, `test_speed_cov_count_division` and `float_moment_sum_model.h` assert with `vmaf_test_identical_f32/_f64`, which is stricter than `==` (a ±0 mismatch fails, a NaN fails) and never a tolerance; `test_speed_cov_count_division` drops its private bit copy.
  `cpp/constant-comparison` (1495): the always-false `total > 2 * MODEL_JSON_MAX` check of `splice_model_json()` is now a live bound on the model plus the block (`SPLICE_TOTAL_MAX`), and `test_splice_model_json_bounds` fails when any of the three bounds is removed.
  `cpp/world-writable-file-creation` (1500) and `cpp/unused-static-variable` (1499): `test_read_pictures_convert` creates its model file owner-only and defines the 16-bit target only where zimg uses it.


- **The controller no longer writes its job queue into the working directory.**
  With `VMAFX_DB_PATH` unset, `vmafx-controller` opened `vmafx-controller.db`
  relative to the directory it started in, which is how three queue files ended
  up committed under `cmd/vmafx-controller/`. The default is now
  `vmafx/vmafx-controller.db` under the user's state directory
  (`$XDG_STATE_HOME`, else `~/.local/state`; the user configuration directory
  on macOS and Windows), created with mode 0700; with no such directory the
  controller refuses to start and names `VMAFX_DB_PATH`. The image and the Helm
  chart set `/data/vmafx-controller.db` and are unchanged. The committed files
  are removed and ignored. See [the controller guide](docs/server/controller.md#configuration).


- **`VMAFX_GRPC_CERT_FILE`, `VMAFX_GRPC_KEY_FILE`,
  `VMAFX_GRPC_MAX_RECV_SIZE` and `VMAFX_GRPC_MAX_SEND_SIZE` reach the
  controller's gRPC server.** `vmafx-controller` did not declare these keys
  as golusoris CompoundKeys, so the variables became `grpc.cert.file` and
  similar keys nothing reads: the size limits stayed at the framework's
  4 MiB, and `VMAFX_GRPC_TLS=true` stopped the controller at startup with an
  empty certificate path. The controller's list is now generated with the
  other binaries' ([ADR-2350](docs/adr/2350-cloud-native-platform.md)).


- **The CUDA VIF twin reads each picture with its own row pitch.** `vif_cuda`
  read both input pictures with the pitch of the engine's own device
  pictures, so a CUDA picture with another pitch (a frame imported where its
  producer holds it) scored wrong VIF values; it now uses each picture's
  stride.


- **The dev container runs the command it is given and exits.** `dev/scripts/dev-mcp-entrypoint.sh`
  ignored its arguments and always kept the container running, so every
  `docker run vmaf-dev-mcp:local <command>` left a container (and its healthcheck) up. A start with
  a command now runs it and exits with its status; a start without one still stays up for
  `docker exec`. The `smoke-probe-cron` compose service now runs its probe loop.
  `docs/development/dev-mcp.md` shows the one-shot forms.


- **Copyright headers drop vendor tool notices and the provenance gate enforces ADR-0861.**
  Four tracked shell scripts (`scripts/ci/setup-envtest.sh`,
  `scripts/dev/test-cleanup-agent-state.sh`, `scripts/release/verify-release-version.sh`,
  `scripts/release/tests/test-verify-release-version.sh`) retained residual dual-notice
  lines missed by the ADR-0861 sweep. Those lines are removed while preserving Lusoris
  copyright and SPDX licence identifiers, and `scripts/dev/relicense_fork_files.py --check`
  now fails if a header copyright notice names a prohibited vendor or tool.


- **Float extractors report their errors through the log (ADR-1906).** The
  allocation and stride errors of the float ADM, SSIM, MS-SSIM, motion and VIF
  code (`error: ...` lines) went to standard output, where they mixed with
  anything a program writes there and ignored the log level. They are now
  `ERROR` log lines: on stderr at the configured level for `libvmaf.h` and
  the CLI, and in the context's log callback for the VMAFx API.


- **`-qpfile` works on libx264, and the saliency tools no longer run a libx264
  encode without the ROI they asked for
  ([ADR-2167](docs/adr/2167-ffmpeg-x264-qpfile-quant-offsets.md)).** Patch
  `0007` gave the libx264 wrapper a `-qpfile` option that called
  `x264_param_parse(.., "qpfile", ..)`; libx264 has no such parameter (the x264
  command line reads the file), so the encoder never opened. The option now
  reads the file and applies each frame's per-macroblock QP offsets through
  x264's `quant_offsets` (+12 on every macroblock: 4186 bytes, none: 20123,
  -12: 104468 on six 576x324 frames), and fails at open, naming why, when
  adaptive quantization is off (`-preset ultrafast` turns it off), the block
  grid is not the video's macroblock grid or the file is malformed. The Go
  and Python saliency code passed `-x264-params qpfile=`, which FFmpeg only
  warns about (`Error parsing option`) before encoding without the ROI; they
  now pass `-qpfile`, which stock FFmpeg refuses. The saliency tests'
  encode-runner stub no longer records the `ffmpeg -version` probe as the
  encode (two tests failed on that, depending on the order they ran in).


- **`motion` was NaN in every row of every extracted feature table.** libvmaf emits no
  `integer_motion` key; the first-order motion score is `VMAF_integer_feature_motion_sad_score`.
  `ai/data/feature_extractor.py` now reads it, so the `motion` column of `FULL_FEATURES` holds values.
  Tables extracted before this change carry an all-NaN `motion` column and the `verify_features` stage of
  the mini retrain refuses them. `extract_full_features.py` also gains `--assume-dims WxH` for corpora
  that are not 1920x1080.


- **Helm: a node without a models volume scores with the image's models.**
  The node Deployment pointed `VMAFX_MODEL_DIR` at `persistence.models.mountPath`
  even when no models volume was mounted (the default), so every job failed
  with "model not found". It now uses the mount path only with
  `persistence.models.enabled` and `/usr/local/share/vmafx/model` otherwise.


- **The operator's events reach every namespace it reconciles
  ([ADR-2647](docs/adr/2647-operator-events-cluster-wide.md)).** The chart
  allowed the operator to create events only in the release namespace, but
  Kubernetes stores an event in the namespace of the resource it describes,
  so the `CheckpointWritten` event of a `VmafxModelTraining` in another
  namespace was refused and lost. A new `<release>-operator-events`
  `ClusterRole` grants `create` and `patch` on events in every namespace and
  nothing else; pods and the leader-election lease stay in the release
  namespace's `Role`.


- **Helm: the server's ServiceMonitor no longer scrapes StatefulSet pods
  twice, and finds the release from another namespace.** With
  `workload: StatefulSet` it also matched the headless Service, so every
  server pod was a second target and sums over the server's series doubled;
  it now skips Services labelled `vmafx.dev/headless`. With
  `monitoring.serviceMonitor.namespace` set to another namespace it selected
  nothing; it now selects the release namespace. The node's HTTP listener
  follows `node.metricsPort` (`VMAFX_HTTP_ADDR`).


- **The Windows icx-cl (SYCL) build no longer reports the C runtime's deprecated calls.** The tiny-AI model-path lookup and the model loader read the environment through `vmaf_getenv_portable()`, the tiny-model sidecar copies a feature name with `VMAF_STRDUP`, and the tests open files through `vmaf_fopen_utf8()` and temporary files through the new `vmaf_tmpfile_portable()` (`tmpfile_s()` under MSVC and icx-cl). A model path read from `VMAF_*_MODEL_PATH` is now copied into a buffer the extractor owns, so the loader's own environment read cannot overwrite it on Windows; a path longer than 4095 bytes is refused with a log line. No score changes. The Windows SYCL leg no longer passes `/experimental:c11atomics` to icx-cl, which ignored it, and `UNUSED_FUNCTION` marks the function for clang-cl and icx-cl too.


- **Container images build again.** Every image that builds libvmaf stopped
  at its licence scan because two headers the build generates
  (`vmafx_build_info.h`, `vmafx_build_commit.h`) had no entry in the licence
  manifest. Both are listed now, and a test fails when a new generated header
  lacks one.


- Build: libvmaf compiles on macOS again. `feature_collector.h` included C++ standard headers
  (through `model.h`) inside an `extern "C"` block, which libc++ rejects with "templates must
  have C++ linkage"; headers are now included before the C-linkage block.


- **The MCP tools advertise and use the library's default model.** `vmaf_score`, `vmaf_score_encoded`
  and `describe_worst_frames` of both MCP servers declared `version=vmaf_v0.6.1` as the default of
  `model` and scored with it when the argument was omitted; they now use `vmaf_v1.0.16_3d0h`, the
  default of the library, the CLI and the server (ADR-1169). Pass `model` to keep scoring with
  another model. The controller's gRPC contract documented the same stale default and says
  `vmaf_v1.0.16_3d0h` now. The default-model gate reads the `version=` spelling and the controller
  contract, so the drift cannot return unnoticed.


- Build: the Windows MinGW UCRT64 build compiles again. The VMAFx API's printf-format
  attribute now names MinGW's own archetype (`__MINGW_PRINTF_FORMAT`), so GCC accepts `%zu`
  under UCRT instead of failing with `-Werror=format`.


- The last MSVC warnings of the first Windows run after the zero-warning series
  are fixed: the CUDA / HIP decouple helpers and the x86 motion round constant
  shift in 64 bits (C4334; the operand never exceeds 30 bits), and three
  conversions in `get_noise_constant()`, the scaled frame size passed to
  `vif_scale_frame_s()` and the second `--feature` option copy are written out.


- **Traces, metrics and logs leave the services when an OTLP endpoint is
  set.** `vmafx-server`, `vmafx-controller`, `vmafx-node`, `vmafx-operator`
  and `vmafx-mcp` never built their OpenTelemetry exporters, so nothing was
  exported whatever the endpoint; they now build them at start and log
  `otel: configured` with `active=true`. The documented
  `OTEL_EXPORTER_OTLP_ENDPOINT=host:4317` form sent everything to
  `localhost:4317`: the standard variable takes a URL
  (`http://otel-collector:4317`), `VMAFX_OTEL_ENDPOINT` takes `host:port`. See
  [OpenTelemetry](docs/observability/otel.md#environment-variables).


- **The GPU container images, the tester images and the Linux release download
  carry `libvmafx.so.1` next to `libvmaf.so.3` (ADR-2094).** Since the library
  split the `vmaf` CLI and the compat `libvmaf.so.3` both load the VMAFx engine
  library, but these builds copied only the `libvmaf.so*` files: their library
  checks refused the result, or the CLI could not start. The release download
  now has six library files; see
  [Release download](docs/getting-started/index.md#release-download).


- **The roadmap rows of 1.1 and 1.3 list the issues that are in those milestones.**
  The no-reference model (#2166) is a 1.1 item because the live no-reference mode
  (#2413) needs it; the 1.3 row now names the artefact detectors (#2272), ST-GREED
  (#2394) and the decision issue (#2167) instead.


- **A build with `-Denable_rust_features=true` registers TAD and no longer
  exports the Rust standard library from `libvmaf.so`
  ([ADR-1713](docs/adr/1713-rc4-rust-extractor-framework.md)).** The TAD
  pilot was compiled but never registered (`--feature tad` failed with
  "problem loading feature extractor"), because the define that gated it never
  reached `feature_extractor.cpp`. The Rust archive's symbols are now kept out
  of the dynamic symbol table with `--exclude-libs` (GNU ld, lld). The Rust
  build also needs no network any more: TAD's unused build-time cbindgen
  dependency is gone and cargo runs `--offline --locked`.


- `vmafx-server` no longer cuts a `Score` or `ScoreStream` RPC that runs
  longer than about two minutes: the gRPC framework rotates connections after
  2 minutes with a 5 s grace for running RPCs, and the server now gives them
  30 minutes, the bound of one vmaf run (#1251).


- `vmafx-server` `GET /readyz` now returns 503 when the vmaf binary the scorer
  runs has been removed, is no longer executable, or when `model.dir` is not a
  directory, instead of staying 200 for as long as the process holds a scorer
  object. The same check is a readiness check (`vmaf-binary`) on the golusoris
  status registry (#1251).


- **The SYCL dma-buf import no longer closes the caller's descriptor.**
  `vmaf_sycl_dmabuf_import()` handed the caller's descriptor to Level Zero,
  whose compute runtime 26.35 closes it when the buffer is imported again
  while its first import is alive; `libvmaf_sycl.h` leaves the descriptor to
  the caller, and the VA surface import closed the same number again, which
  could close an unrelated descriptor another thread had opened in between.
  Level Zero now gets a private duplicate, and the caller's descriptor stays
  open on every driver.


- **The Windows SYCL build compiles again.** Since the math constants come
  from `<math.h>` (#2638), the SYCL feature sources need `_USE_MATH_DEFINES`
  on Windows, which the project-wide argument did not reach: icpx compiles
  them in custom targets. Both SYCL argument lists now carry the define, and
  `test_sycl_math_constants_contract.py` keeps every icpx compile line on it.


- **A cross-device parity run that compared nothing passed** (`T-TINY-AI-CROSS-DEVICE-PARITY-UNGATED-2026-09-25`).
  `vmaf_train.cross_backend.CrossBackendReport.ok` is now False when no output was compared, when a requested
  provider is missing, or when ONNX Runtime accepted a provider but ran the session on the CPU, so
  `vmaf-train cross-backend --fail-on-mismatch` no longer exits 0 on a CPU-only host. New
  `scripts/ci/tiny_ai_cross_device_parity_gate.py` checks `vmaf_tiny_v2` (1e-4) and `smoke_fp16_v0` (1e-2)
  between two providers and names the missing provider; it is not yet wired into a hardware job.


- **The vmaf-tune tools build FFmpeg command lines that do what they say.**
  Repeated `-x265-params` (two-pass stats, saliency zones, HDR SEI) or
  `-x264-params` / `-svtav1-params` / `-vvenc-params` options are joined into one
  (FFmpeg keeps only the last, so a pass-2 encode lost its stats file or its
  zones); the per-shot probe and signalstats passed the shot's start frame
  index to `-ss`, which reads seconds, and now convert it with the frame rate;
  `hevc_nvenc` no longer gets `-master_display` / `-max_cll`, which FFmpeg
  rejects and which aborted the encode; `/dev/null` became `os.DevNull`; the
  saliency check keys on the ROI keys instead of any `-x265-params`. The
  `libvmaf_cuda` recipes convert NVDEC's NV12 with `scale_cuda` (the filter
  accepts `yuv420p` and `yuv444p16` only).


- **A VMAFx error names a long path in full.** `VmafxError` kept 95 bytes of
  its subject, so a model file whose path was longer was named by a cut-off
  path; subjects now keep 1023 bytes and messages 1023.


- **Model hashes are the same on Windows.** The repository checked the model
  JSON files out with CRLF line endings on Windows, so a Windows build's
  built-in models and any model file loaded there reported a different
  `vmafx_model_hash()` than on Linux and macOS (and than `sha256sum` of the
  published file). The model JSON files now check out with LF on every
  platform.


- **The Windows SYCL tester zip passes its import check.** `cfgmgr32.dll`, which the Level Zero loader imports, is a System32 DLL and is now accepted by `scripts/ci/check-windows-bundle-imports.py`; an unknown DLL is still refused. The x64-sycl leg failed on it, so no rc.3 Windows zip was published.


- Windows: `vmaf_open_utf8()` and libsvm's model writer no longer abort the process
  when a caller passes a POSIX file mode such as `0644`. The change from `_wopen` /
  `_open` to `_wsopen_s` / `_sopen_s` (the CRT's non-deprecated spellings) made the
  CRT reject permission bits other than `_S_IREAD` and `_S_IWRITE` as an invalid
  parameter; the mode is masked to those two bits, as the old calls effectively did.


### Security

- **`golang.org/x/net` moves from v0.59.0 to v0.60.0 and the Go toolchain from 1.27.1 to 1.27.2**
  for GO-2026-6617 (an HTTP/2 server crash from an HPACK encoder race) and twelve standard-library
  advisories published with it (`html/template`, `net/http` and its HTTP/2 copy, `crypto/tls`, `os`,
  `mime/multipart`). `go.mod` declares `go 1.27.2`, and the release and development images build on
  the `golang:1.27-trixie` digest that carries Go 1.27.2. `govulncheck ./...` reached the vulnerable
  symbols from the controller, the tune executor, `pkg/libvmaf` and `tools/obssmoke`; it now reports none.

## [1.0.0-rc.3] - 2026-10-07

This release collects 585 changelog entries.
They are recorded in full, unedited, in
[`docs/changelog-archive/1.0.0-rc.3.md`](docs/changelog-archive/1.0.0-rc.3.md) — too long to read inline here.

| Section | Entries |
| --- | --- |
| Added | 42 |
| Changed | 235 |
| Fixed | 299 |
| Security | 9 |
- **An AMD GPU tester image measures every HIP twin on an outside tester's AMD
  GPU with one command and no build.**
  `ghcr.io/vmafx/vmafx:<version>-tester-hip` (linux/amd64) runs on Linux with
  `--device /dev/kfd --device /dev/dri` and the render group. It finds every AMD
  GPU of the 25 gfx targets ROCm 10.0.0 supports (Instinct MI100 to MI350, Radeon
  RX 5000, 6000, 7000 and 9000 series, Radeon 680M, 780M, 860M, 890M and 8060S
  graphics, Steam Deck) and reports per GPU its
  gfx target and family, every HIP twin against the CPU at `--precision max`, the
  parity gate's HIP cells, the HIP device tests, and the verdict of
  `T-HIP-TWINS-OTHER-TARGETS-2026-10-03` for its family. It ships only the ROCm
  10.0.0 runtime files the HIP build loads, unmodified, with their licences and the
  source of the LGPL libraries among them. See
  [the tester guide](docs/usage/tester-image.md#e-amd-gpu-image-linux)
  and [ADR-1511](docs/adr/1511-amd-gpu-tester-image.md).


- **VMAFx core API: contexts, models, host frames and scores (RC4, ADR-1852,
  ADR-1906).** A program can now score videos through `vmafx/*.h` alone:
  contexts with their own log callback (`VmafxContextConfig.log_callback`),
  which receives every message raised for the context, worker threads
  included, while nothing of it reaches the process log,
  context options (`vmafx_context_set_option`), feature option sets
  (`vmafx_options_set`), extractor, model and model-set registration
  (`vmafx_context_use_feature`, `vmafx_context_use_model`,
  `vmafx_context_use_model_set`, `vmafx_context_import_score`), feature
  resolution (`vmafx_feature_resolve`), refcounted models and model sets with
  the SHA-256 of the bytes as loaded (`vmafx_model_load`,
  `vmafx_model_load_file`, `vmafx_model_hash`, `vmafx_model_set_load`, ...;
  a model load logs to the callback of its `VmafxModelConfig`),
  the CPU device (`vmafx_device_create`), host frames allocated or borrowed
  without a copy (`vmafx_frame_create_host`, `vmafx_frame_wrap_host`),
  submission (`vmafx_submit`, `vmafx_flush`), frame retention
  (`vmafx_context_frame_retention`) and synchronous per-frame and pooled
  scores for features, models and model sets (`vmafx_score_frame`,
  `vmafx_score_pooled`, `vmafx_feature_score_pooled`,
  `vmafx_score_frame_model_set`, `vmafx_score_pooled_model_set`), equal bit for
  bit to the `libvmaf.h` calls. One frame can be scored by several contexts
  without a copy. Errors also name what kind of subject failed and the
  function (`vmafx_error_subject_kind`, `vmafx_error_function`); an input
  struct below its introduction size is the new `VMAFX_E_ABI`. ABI 0.1.1. See
  [the VMAFx API page](docs/api/vmafx/index.md).


- **vmafx-controller reads and enforces its tenant configuration
  ([ADR-1519](docs/adr/1519-controller-tenant-registry.md)).** With
  `VMAFX_AUTH_TENANTS_SOURCE=kubernetes` the controller lists the
  `VmafxTenant` resources of its namespace (`=file`: a YAML/JSON file of
  them) and accepts only those tenants: each token is verified with the
  identity provider of the tenant it names and must carry that tenant's ID,
  `enabled: false` suspends a tenant (403 / `PERMISSION_DENIED`), roles
  outside `allowedRoles` are dropped and a token without vmafx roles gets
  `defaultRole`. The set is re-read every `VMAFX_AUTH_TENANTS_REFRESH`
  (default 30 s); the controller does not start on an invalid tenant or an
  inconsistent setting, and refuses every token once it has not read its
  tenants for ten intervals. The Helm chart switches to it when
  `auth.tenants` is set (or `auth.tenantSource: kubernetes`), grants the read
  access and opens the API server in the NetworkPolicy.


- **`vmaf-dev-llm[modelcard]` extra** (ADR-1528). It installs `onnx`,
  `onnxruntime`, `pandas`, `pyarrow` and `scipy`, which `vmaf-dev-llm modelcard`
  needs to read the ONNX graph and to score the model with `--features`.
  Without them the card leaves those facts out. See
  [the dev-llm README](dev-llm/README.md#install).


- **Charts in the documentation, drawn from repository data.** Three
  Vega-Lite charts render to static SVG in light and dark at docs-generation
  time and turn interactive in the browser, with exact values on hover: the
  status of every GPU twin against the CPU extractor (landing page and
  [Backends](docs/backends/index.md)), the upstream-parity allowlist by
  extractor ([Upstream parity guard](docs/development/upstream-parity.md)) and
  the per-frame VMAF of the 576x324 snapshots
  ([Netflix benchmark baselines](docs/development/netflix-benchmark-baselines.md)).
  Each has a text alternative and its data table.
  `scripts/docs/generate-charts.py` builds them from
  `scripts/ci/exact_twins.d/`, `scripts/ci/upstream_parity.d/` and
  `testdata/`, and `make docs-fragments-check` fails when a render, the data or
  a page block drifts. The renderer, vl-convert-python 1.9.0.post1, is
  hash-pinned in the docs lock; the browser bundle of Vega, Vega-Lite and
  vega-embed is vendored with its licence texts and loads only on chart pages
  ([Documentation site design](docs/development/docs-site-design.md#charts),
  [ADR-1508](docs/adr/1508-docs-site-toolchain-and-charts.md)).


- **The documentation site redesign has a decided toolchain.** The site stays
  on MkDocs 1.6.1 with Material for MkDocs 9.7.x, with the design built in
  Material's CSS layer, and moves to Zensical once it meets measured exit
  criteria, before Material's end of life on 2027-05-05. Charts are Vega-Lite
  specs beside repository data, diagrams use the `tools/figures/` engine, and
  prose is to be set at 60 to 75 characters per line (81 to 88 today). The ADR
  pages and tag pages leave the sidebar and are reached through the ADR index
  and tag pages
  ([ADR-1508](docs/adr/1508-docs-site-toolchain-and-charts.md),
  [ADR-1510](docs/adr/1510-adr-nav-collapse-behind-index.md),
  [Research-2137](docs/research/2137-docs-site-toolchain-charts-diagrams.md)).


- **Every exact GPU twin is measured at 8, 10, 12 and 16 bits and in 4:2:0,
  4:2:2 and 4:4:4.** `scripts/ci/exact_twin_matrix.py` runs each twin declared
  in `scripts/ci/exact_twins.d/` against `--backend cpu` at `--precision max` on
  generated 357x353 fixtures, with no tolerance. `test_cuda_exact_twin_matrix`,
  `test_sycl_exact_twin_matrix` and `test_hip_exact_twin_matrix` run it on a
  device. [The matrix page](docs/development/exact-twin-matrix.md) records the
  result per backend, and `test_exact_twin_matrix_contract` fails while a
  declared CUDA, SYCL or HIP twin has no full, passing row there.


- **The FFmpeg `libvmaf` filter declares the input colour to libvmaf (FFmpeg patch 0022, [ADR-2093](docs/adr/2093-upstream-hdr-groundwork-input-colorimetry.md)).**
  On the first frame pair the filter (and the software path of `libvmaf_sycl`) maps the AVFrame
  range, primaries, transfer and matrix of each input and calls `vmaf_set_input_colorimetry()`,
  so a model with a `conversion_target` converts HDR input. Inputs without colour tags, and
  models without a target, score as before. See
  [FFmpeg usage](docs/usage/ffmpeg.md#input-colour-tags-and-hdr-models).


- **A static check keeps every GPU source from advancing a wide sample pointer
  by a byte stride.** `test_gpu_byte_stride_contract` (fast suite, no device)
  scans the CUDA, HIP, SYCL and Metal sources under `core/src`. It fails on a
  pointer to samples wider than a byte that is offset or indexed by a stride
  counted in bytes, the defect that made an upstream CUDA motion kernel read
  every other row of 16-bit input. Two SYCL kernels whose stride counts
  elements above scale 0 now name that unit. See
  [row addressing above 8 bits](docs/development/gpu-backend-template.md#row-addressing-above-8-bits).


- **A device test holds every `vmaf_v1.0.16*` model wholly on CUDA, SYCL and
  HIP, bit for bit.** `test_cuda_v1_models_no_fallback`,
  `test_sycl_v1_models_no_fallback` and `test_hip_v1_models_no_fallback` score
  the eight built-in v1 models and the default model on the 576x324 `src01`
  pair (8, 10 and 12 bits 4:2:0, 10 bits 4:2:2) and 16 frames of the 3840x2160
  pair in `testdata/bbb`. Each test fails on any CPU extractor in
  `feature_backends` and on any value that differs from the CPU run at
  `--precision max`. See
  [the gate page](docs/development/cross-backend-gate.md#whole-models-on-one-backend).


- **Hardware we need: a page listing the machines the project wants tester reports from,
  and an issue form that covers all five tester packages.** The new page
  `docs/usage/hardware-we-need.md` has one table: per hardware family (Apple M-series,
  Arm with and without SVE2, Intel and AMD AVX-512, AVX2-only x86, NVIDIA Ampere / Hopper /
  Blackwell / Ada, AMD CDNA / RDNA1 to RDNA4, Intel Xe-LP / Xe-LPG / Xe2 / Xe-HPG) it names
  the package to run, the open row of the bug ledger a report closes, and the status
  (covered by a project host, or the number and worst verdict of the reports under
  `docs/hardware-reports/`). The status is generated by
  `scripts/docs/generate-hardware-reports.py`, so `make docs-fragments-check` fails on a
  stale table, on a GPU family of the tester image's row maps with no row, and on a ledger
  id that does not exist. The "Hardware report" issue form now offers the macOS bundle, the
  container image, the Intel, NVIDIA and AMD GPU images, and "Other / built from source".


- **HDR-VMAF groundwork from upstream: input colorimetry, a model `conversion_target`,
  conversion in `vmaf_read_pictures()` (ports of Netflix/vmaf `ed61076b2`, `1ddf81607`,
  `a6c0ba6d5`, `130569c45`, `efe90c8b8`, `5c3f4fb90`; [ADR-2093](docs/adr/2093-upstream-hdr-groundwork-input-colorimetry.md)).**
  A model file may declare a `conversion_target` (colorspace, optional pixel format and
  bit depth); `vmaf_read_pictures()` then converts both pictures to it with zimg before
  extraction, on the host and before any GPU upload. The source colorimetry comes from the
  new `--color_range_ref/_dist`, `--color_primaries_ref/_dist`, `--color_trc_ref/_dist` and
  `--color_matrix_ref/_dist` flags (all four of an input, or none) and, in the C API, from
  the new `vmaf_set_input_colorimetry()`: `VmafPicture` keeps its layout, so the colour is
  declared on the context instead of in each picture. Models without a target, which is
  every shipped model, are unaffected. The Python harness passes `color_ref` / `color_dist`
  from `optional_dict` to `vmafexec`. zimg stays the opt-in `-Denable_zimg=true`. See
  [CLI](docs/usage/cli.md#input-colorimetry),
  [model files](docs/models/v1.md#model-declared-conversion-target) and
  [Pictures](docs/api/pictures.md#converting-to-a-models-conversion-target).


- **The Helm chart deploys vmafx-controller, and releases publish its image
  ([ADR-1589](docs/adr/1589-helm-controller-workload.md)).**
  `controller.enabled` renders a one-replica controller (Recreate, SQLite job
  queue on a ReadWriteOnce claim) with a Service carrying its HTTP (8080) and
  gRPC (9090) ports; the nodes and the operator are pointed at it,
  `node.controllerToken` / `operator.controllerToken` mount their bearer
  tokens from Secrets, and the NetworkPolicies open the flows between them.
  `ghcr.io/vmafx/vmafx-controller:<tag>` is built like the other Go images,
  signed, with SBOMs, licence notices and a `<tag>-source` image; the binary
  gains `--version`. **Migration:** `auth.*` now configures only the
  controller workload and needs `controller.enabled`; a release that ran a
  controller through `image.repository` fails to render and moves to
  `controller.enabled` (docs/development/k8s-deployment.md, "Upgrading to the
  controller workload").


- **Helm: `node.fuse` and `node.ebpf`
  ([ADR-1593](docs/adr/1593-helm-node-fuse-and-ebpf.md)).** `node.fuse` gives
  the node pods `/dev/fuse` through a FUSE device plugin's resource and the
  capability bounding set mount mode needs; `node.ebpf` turns on the eBPF
  descriptor tracker (`VMAFX_EBPF_BYPASS`) with UID 0, `BPF`, `PERFMON` and
  `SYS_ADMIN` and the host's tracefs read-only. `storage.mode: mount` without
  `node.fuse` is now refused at render time; it used to deploy a node that
  could not start.


- **`scripts/dev/hip_dispatch_drop_probe.hip` checks whether an AMD GPU runs
  every command of a HIP stream.** Built with `hipcc`, it runs frames of one
  memset, several small kernels and a readback on one stream and reports the
  frames with wrong results and the kernel launches that never ran. On the
  maintainers' gfx1036 iGPU (ROCm 7.2.4) about one frame in 10^4 loses a run
  of its commands, which makes a HIP twin report a wrong score for that frame
  on master as well; the probe tells whether a driver update fixed it. See
  [the HIP backend guide](docs/backends/hip/overview.md#known-issue-the-gfx1036-loses-stream-commands)
  (`T-HIP-GFX1036-DROPPED-DISPATCHES-2026-10-01`).


- **An Intel GPU tester image measures every SYCL twin on an outside tester's
  Intel GPU with one command and no build.**
  `ghcr.io/vmafx/vmafx:<version>-tester-sycl` (linux/amd64) runs on Linux with
  `--device /dev/dri` or on Windows with WSL2 through `/dev/dxg`, finds every
  Intel GPU and reports per GPU its family, every SYCL twin against the CPU at
  `--precision max`, the parity gate's SYCL cells, the 69 SYCL device tests, the
  scratch-memory audit, and which state rows the GPU's measurements close; its
  first target is the Xe-LP half of
  `T-SYCL-ROW-KERNELS-SG16-OTHER-DEVICES-2026-10-02` on a UHD 770. The tester
  report's schema version 3 adds a backend-neutral `gpu` section the CUDA and
  HIP kits reuse. See
  [the tester guide](docs/usage/tester-image.md#c-intel-gpu-image-linux-or-windows-with-wsl2)
  and [ADR-1505](docs/adr/1505-intel-gpu-tester-image.md).


- **Cancelling a running job stops it on its node
  ([ADR-1567](docs/adr/1567-job-cancel-reaches-node.md)).** `CancelJob` used
  to mark the job `CANCELLED` while the node's `vmaf` process ran on to the
  end. The node's heartbeat now lists the jobs it runs, the controller answers
  with the cancelled ones, and the node kills their `vmaf` processes and
  reports them as `cancelled by the controller`, within one heartbeat interval
  (`VMAFX_CONTROLLER_HEARTBEAT_INTERVAL`, 10 s by default). The two new
  heartbeat fields are additive; an older node is not told.


- **`vmaf_feature_backend_twin()` and `vmaf_registered_feature_extractor()`**
  in `libvmaf.h`. The first tells a caller which device twin model dispatch
  would use for a CPU extractor on the context's backend, and whether that twin
  can honour the given options and picture size. The second lists the
  registered extractors and the backend each one runs on. Both are additive;
  `vmaf_use_feature()` still selects by exact name. See
  [the C API reference](docs/api/index.md#device-twins-and-the-extractors-that-ran).
- **`feature_backends` in the CLI's JSON output**: one
  `{"extractor": ..., "backend": ...}` entry per registered extractor, next to
  `backend_used`, so a run that mixes device twins and CPU extractors says so.


- **Required check `Licence Provenance`.** Every pull request and every push
  to `master` now runs `scripts/dev/relicense_fork_files.py --check`
  (ADR-1250): a fork-authored file with another licence, a file that
  reproduces upstream code without that code's notice, and a stale entry in the
  reviewed provenance file fail the merge gate. The upstream tree it compares
  against is the Netflix/vmaf commit the repository records as the head it is
  at parity with (`docs/development/known-upstream-bugs.md`, read by the new
  `scripts/ci/upstream_parity_pin.py`), so a commit pushed upstream cannot turn
  the check red on an unrelated pull request. The tool refuses a shallow
  checkout. Guide:
  [docs/development/licence-provenance-check.md](docs/development/licence-provenance-check.md)
  ([ADR-1474](docs/adr/1474-relicense-helper-headers-and-ci-check.md),
  closes `T-RELICENSE-CHECK-PENDING-2026-10-02`).


- **The macOS tester bundle measures every open Metal state row in one run**
  (ADR-1496). The parity gate (`scripts/ci/cross_backend_parity_gate.py`) has a
  `metal` backend and a `--hold-exact <backend>` option that compares that
  backend's cells exactly at `--precision max` before an `exact_twins.d`
  fragment lists it; the bundle carries the gate and runs it on its four
  fixtures as `--backends cpu metal --hold-exact metal` (report section
  `metal_gate`, a check of the verdict). Every Metal parity test compares with
  `==` (ciede at the `1e-9` `LIBM_TWINS` bound) on the CUDA, HIP and SYCL
  twins' cases, adds the cases the open rows need (full-range 16-bit content,
  10- to 16-bit input with large differences, option sets, identical pairs, one
  frame, frames below 16 and 17 pixels, `adm_noise_weight=0`,
  `adm_enhn_gain_limit` 1.2 and 1.5, `motion3` and the motion SAD score), runs
  every case after a failure and prints a verdict per case, which the report
  keeps (`unit_tests.cases`). `tools/rc1-tester/image/metal-rows.json` maps each
  open Metal row to the cases, fixture metrics and gate cells that close it, and
  the report gives each row a verdict (`metal_rows`). Report schema 2 adds the
  three; schema 1 reports stay valid. The Metal tests also build on every host
  as self-tests with the CPU extractor in the twin's place (suite
  `metal-selftest`). Guides: `docs/usage/tester-image.md`,
  `docs/backends/metal/index.md`, `docs/development/cross-backend-gate.md`.


- **Mini retrain and a resumable stage runner for the retrain tooling** (ADR-1898, issue #1246).
  `make mini-retrain` runs extraction, feature checks, combination, training and export of
  `vmaf_tiny_v2` to `v4` and `fr_regressor_v1`, validation, registry validation and a PLCC / SROCC / RMSE
  gate on a generated 144-row corpus in about 40 seconds. Every stage writes a manifest with seed,
  digests, library versions, lock digest, container id and resource use; a killed run resumes from the
  manifests; a missing or corrupt input stops the run with the stage name before anything runs. The
  Tiny AI job runs it for changes under `ai/`, and a nightly workflow runs it too. See the runbook section 13.


- **`motion_five_frame_window` works, and the four `vmaf_v1.0.16_hfr_*` models
  score.** The option of the `motion` and `motion_v2` extractors takes each
  frame's SAD against the frame two back, and `motion2` from the SADs of the
  frames before and after; the fork declared it and returned `-ENOTSUP`
  (ADR-0337, ADR-0994), so the HFR models that set it could not be used. It
  is Netflix's code (`a2b59b77`, `a4a1492d`) with its arithmetic unchanged:
  on 31 clips, with eight option sets, on the scalar, AVX2 and default paths,
  serial and with worker threads, all 95 130 values equal Netflix `9e48141b`
  at 17 significant digits. `--model version=vmaf_v1.0.16_hfr_3d0h` and its
  three siblings run on every backend; on `cuda`, `sycl`, `hip` and `metal`
  the motion feature is computed by the CPU extractor and the rest on the
  device. See [Motion, five-frame window](docs/metrics/motion.md#five-frame-window),
  [VMAF v1 models](docs/models/v1.md) and
  [ADR-1478](docs/adr/1478-motion-five-frame-window-port.md).
- **A preallocated picture pool needs four pictures for the five-frame
  window, and only for it.** While an extractor with the option is registered
  the library keeps the reference pictures of the two frames before the
  current one; otherwise it keeps what it kept before. A pool from
  `vmaf_preallocate_pictures()` below four pictures next to such an extractor
  is refused with `-EINVAL` and one error line naming `pic_cnt` and the
  minimum, whichever of the two calls comes second, instead of stalling on the
  third frame. Without the option a pool of three works as before. The `vmaf`
  tool preallocates four pictures in a run without `--threads` (three
  before). Callers that allocate each picture with `vmaf_picture_alloc()`,
  the FFmpeg filters among them, need no change. See
  [the C API reference](docs/api/index.md#ownership-and-lifetime).


- **`adm` has a NEON scale-zero decouple on aarch64.** `adm_decouple_neon()`
  (`core/src/feature/arm64/adm_neon.c`, ported from Netflix/vmaf `9e48141b`)
  decouples four columns at a time for integral enhancement gain limits and
  hands fractional limits to the scalar kernel, so it returns the scalar
  kernel's bits at every `adm_enhn_gain_limit` from 1 to 100. No score
  changes; only the time of the scale-zero decouple does. `test_integer_adm_simd`
  now runs on aarch64 and holds the kernel to the scalar one at gain limits
  1, 1.2, 1.5, 2, 3, 7 and 100.


- **`vmafx-node` pulls jobs from the controller.** With
  `VMAFX_CONTROLLER_ADDR` set, the node registers with the controller,
  heartbeats, takes jobs with `PullWork`, scores them on the backend it
  advertises (`VMAFX_BACKEND`, passed to the vmaf CLI as `--backend`) and
  reports the results; before, no component called the controller's Node API
  and submitted jobs stayed `PENDING`. Retries use jittered backoff, every call
  has a deadline, a refused session is renewed, a bearer token comes from
  `VMAFX_CONTROLLER_TOKEN_FILE` (re-read per call) or `VMAFX_CONTROLLER_TOKEN`,
  and TLS is `VMAFX_CONTROLLER_TLS`. The node refuses to start without a vmaf
  binary, with `VMAFX_BACKEND=auto` or with a malformed setting. The Helm chart
  sets the address only from `node.controllerAddr` (no default pointing at a
  missing Service) and opens node-to-controller egress under
  `networkPolicy.enabled`. See [the node guide](docs/server/node.md#pulling-jobs-from-the-controller)
  and [ADR-1524](docs/adr/1524-vmafx-node-controller-client.md).


- **`VMAFX_EBPF_BYPASS=1` starts the node's eBPF descriptor tracker, and a
  host that cannot run it stops the node.** The loader under
  `cmd/vmafx-node/bpf` was never started and `VMAFX_EBPF_MOUNT_PREFIX` was read
  by no code; the tree held a stub instead of the compiled program. The node
  now starts the tracker when asked (storage must mount under the prefix) and
  refuses to start, listing every reason, on a kernel older than 5.15, without
  kernel BTF or visible syscall tracepoints, or without `CAP_BPF` and
  `CAP_PERFMON` (or `CAP_SYS_ADMIN`). The compiled program is embedded. The
  tracker records descriptors only; no read is bypassed. See
  [the eBPF tracker page](docs/development/ebpf-fuse-bypass.md) and
  [ADR-1539](docs/adr/1539-node-ebpf-tracker-wiring.md).


- **`vmafx-node` scores jobs whose sources are rclone remotes or http(s)
  URLs.** The executor now prepares a job's reference and distorted through
  `pkg/storage` (`VMAFX_STORAGE_MODE`: `http-serve`, `mount` or `auto`, the
  default); before, it handed them to the vmaf CLI unchanged, so only local
  paths worked. In `http-serve` mode, and for any http(s) URL, the clips are
  streamed into the CLI through pipes without touching the node's disk, and a
  stream that breaks fails the job instead of yielding the score of the frames
  it delivered. `auto` picks `mount` when FUSE is usable and logs its choice;
  an unknown mode, or `mount` without FUSE, stops the node at startup.
  `libvmaf.Scorer.ScoreReaders` and `storage.Open` are new; `storage.New` is
  deprecated. See [job sources](docs/server/node.md#job-sources-local-paths-urls-and-rclone-remotes)
  and [ADR-1526](docs/adr/1526-node-storage-streamed-inputs.md).


- **An NVIDIA GPU tester image measures every CUDA twin on an outside tester's
  NVIDIA GPU with one command and no build.**
  `ghcr.io/vmafx/vmafx:<version>-tester-cuda` (linux/amd64) runs on Linux with
  `--gpus all` (NVIDIA Container Toolkit; CDI `--device nvidia.com/gpu=all`
  works too) and, not yet proven, on Windows with WSL2 under Docker Desktop. It
  finds every NVIDIA GPU of compute capability 8.0 or newer and reports per GPU
  its family, compute capability and the kernel code it ran, every CUDA twin
  against the CPU at `--precision max`, the parity gate's CUDA cells, the 66
  CUDA device tests, and the verdict of
  `T-CUDA-TWINS-OTHER-ARCHITECTURES-2026-10-03` for its family. The image ships
  no NVIDIA file: it uses the host's driver. See
  [the tester guide](docs/usage/tester-image.md#d-nvidia-gpu-image-linux-or-windows-with-wsl2)
  and [ADR-1509](docs/adr/1509-nvidia-gpu-tester-image.md).


- **`vmaf_picture_convert()`: zimg picture conversion, additive variant
  (port of Netflix/vmaf `0497a0f29`, [ADR-1822](docs/adr/1822-additive-picture-convert.md)).**
  New public API in `libvmaf/picture.h`: `VmafColor`, the colour enums,
  `VmafPictureConvertTarget`, `vmaf_picture_convert_context_init_with_color()`,
  `vmaf_picture_convert()` and `vmaf_picture_convert_context_close()`. It converts
  pixel format, bit depth, size and colour description through zimg (>= 2.7),
  enabled with the new `-Denable_zimg=true` Meson option (default off; the
  functions return `-ENOTSUP` without it). `VmafPicture` is unchanged: the
  source colour is passed as an argument instead of the `VmafPicture::color`
  member upstream adds, which would move `ref` and `priv`. See
  [Pictures](docs/api/pictures.md#converting-pictures-vmaf_picture_convert).


- **Python harness: `SubjectiveDatasetReader` and `SubjectiveDatasetTester`,
  and per-video sizes, resampling, `fps_cmd` and `workfile_yuv_type` in
  dataset files (port of Netflix/vmaf `2e6bbb657`, with the tests of
  `3685aa3c1` and `2f2bb601b`).** `read_dataset()` and
  `run_test_on_dataset()` are built on the two classes; the tester keeps the
  results and correlation stats of a run. A reference and a distorted video
  may now have different `width` / `height` (with `quality_width` /
  `quality_height` to meet at) and their own `resampling_type`; before, a
  size mismatch failed an assertion and a dataset file gave an asset one
  resampling type. Documented in `docs/usage/python.md`.


- **RC3 home GPU retest kit** (`scripts/dev/rc3-home-gpu-retest.sh`): runs the
  verify-and-time commands that the `docs/state.md` rows carry for the RTX 4090
  (CUDA), the Arc A380 (SYCL) and the gfx1036 iGPU (HIP), one entry per row,
  with every device run under that device's lock. It writes a log and the JSON
  of every run per row plus a summary table, and `--baseline DIR` compares a
  pull request's run with an earlier run on `master`. See
  [the retest guide](docs/development/rc3-home-gpu-retest.md) (ADR-1386).


- **The tester, Windows, macOS, production, operator / node and supply-chain
  workflows are built and smoke-tested before they publish
  ([ADR-1595](docs/adr/1595-pr-time-verify-push-only-workflows.md)).** A pull request
  that changes the tester image's or the Windows zip's inputs now builds the amd64
  image or the x64 zip; the macOS bundle is built weekly; the new `Release Dry Run`
  workflow builds the release images (no push) and the `vmaf-mcp` wheel, sdist and
  SBOMs on pull requests that touch their inputs and weekly. Nothing is pushed,
  signed or attested outside a release. See
  `docs/development/release-workflow-verification.md`.


- **`make test-affected BASE=<sha> HEAD=<sha>` runs the Python test suites a
  change touches, locally, in cached hash-locked environments.**
  It maps the changed files to the suites of `.github/test-suites.json`, builds
  each suite's virtual environment from its lock files once, and fails on a test
  failure, a time-cap overrun or a skip for a missing dependency or input. See
  [Run the affected suites locally](docs/development/test-suites.md#run-the-affected-suites-locally).


- **Opt-in sample range check: `vmaf --check-sample-range` and
  `vmaf_set_sample_range_check_enabled()`.** A 10- or 12-bit picture stores
  its samples in 16 bits, so it can carry values above 2^bpc - 1, which are
  invalid input (the CPU extractors and their GPU twins may then score
  differently). With the check on, `vmaf_read_pictures()` refuses such a frame
  with `-EINVAL` before extracting anything and logs the picture, plane, row,
  column and value; the command line stops with a non-zero exit status. Off by
  default, at the cost of one flag test per frame
  ([ADR-1918](docs/adr/1918-sample-range-contract-opt-in-check.md),
  [Sample range](docs/api/sample-range.md)).
- **Rust twins of C feature extractors, selectable at run time (RC4
  framework, [ADR-1713](docs/adr/1713-rc4-rust-extractor-framework.md)).**
  A build with `-Denable_rust_features=true` links one Rust archive into
  `libvmaf` and registers each Rust twin as `<name>_rust` next to its C
  extractor, with the C extractor's options, feature names and flags.
  `VMAF_FEATURE_IMPL=rust` makes every registration path use the twin where
  one exists and logs the C fallback where none does; `--feature psnr_rust`
  picks a twin directly; the JSON report's `feature_backends` names the
  extractor that ran. The C extractors stay the default. The first twin,
  `psnr_rust`, returns the C scores bit for bit on the Netflix pair, both
  checkerboard pairs, a 10-bit pair and 200 frames of 4K.
  `scripts/ci/rust_twin_diff.py` proves a twin equal to its C extractor (same
  binary, equal doubles on every metric of every frame), the `Rust` workflow
  runs clippy on every workspace crate, checks the cbindgen header and runs the
  new `rust` Meson suite. See
  [Rust extractor framework](docs/development/rust-extractor-framework.md).


- **actionlint pre-commit hook and Makefile target**: Wired `actionlint`
  pinned to `v1.7.12` (HISS-11 hermetic supply chain pin) into
  `.pre-commit-config.yaml` to validate all 35 GitHub Actions workflow files
  under `.github/workflows/` against `.github/actionlint.yaml`. Added
  `make lint-actions` target and documented workflow linting in
  `docs/development/pre-commit-hooks.md`.


- **`-Dsycl_device_asan=true` instruments the SYCL backend with the DPC++ device AddressSanitizer.** The option puts the
  flags on every SYCL compile and the link (`-Dcpp_args` never reaches them, so a build configured that way
  instruments no kernel and still prints the sanitizer banner). `scripts/dev/sycl_device_asan_check.sh` (meson test
  `test_sycl_device_asan_probe`) runs a probe kernel on the device and checks what the sanitizer reports and what it
  cannot see. On oneAPI 2026.0 with an Arc A380 an instrumented libvmaf is not a usable check: a pointer held in a
  struct captured by value reads as null. See [the device sanitizer page](docs/backends/sycl/device-sanitizer.md).


- **A tester image and a macOS bundle let someone outside the project test the fork
  without building it, and send the result for credit.** `ghcr.io/vmafx/vmafx:<tag>-tester`
  (linux/amd64 and linux/arm64) and a macOS arm64 `.tar.gz` each run one command that
  prints a JSON report: host facts, every CPU extractor at `--precision max` with default
  dispatch against scalar C, baked reference scores, SIMD unit tests, and the Netflix
  golden gate (image) or every Metal twin against the CPU (bundle). The container runs
  under `--network none --read-only --cap-drop ALL`; both packages are built only by
  hosted workflows from a tagged commit and carry cosign signatures and build
  provenance. Reports are added as `docs/hardware-reports/<date>-<cpu>.json`, checked by
  `scripts/ci/check-hardware-reports.py`. See
  [the tester guide](docs/usage/tester-image.md),
  [ADR-1492](docs/adr/1492-tester-image-arm64-report.md) and
  [ADR-1493](docs/adr/1493-macos-tester-bundle.md).


- **Upstream parity guard: `make upstream-parity` compares this tree's CPU
  extractors with Netflix/vmaf at the recorded parity head, every emitted
  value at `%.17g`** (ADR-1487). It builds both trees in the dev container
  image, where every comparison is made (elsewhere the guard refuses, or
  with `--unpinned` reports an advisory verdict), runs 16 shared extractors,
  their option variants and the shipped models on the scalar path and the
  default dispatch, and fails on a difference that no recorded deviation
  covers, on one larger than its recorded bound, and on a recorded deviation
  that no longer exists. `make upstream-parity-full` runs the whole matrix
  twice, the second time with the heap filled, and fails on an output of
  this tree that changes or on a finite bound over an upstream value that
  does. The deviations are fragments under `scripts/ci/upstream_parity.d/`,
  listed in `docs/development/upstream-parity-allowlist.md`; the guide is
  `docs/development/upstream-parity.md`.


- **`vmaf --list-backends` reports which scoring backends the binary can use.**
  It prints, as JSON, every backend the CLI knows (cpu, cuda, sycl, hip,
  metal), whether it is compiled in, and whether its state initialises on this
  host, then exits without reading any input. See
  [the CLI reference](docs/usage/cli.md#which-backends-this-binary-can-use).
- **Preview of the VMAFx C API, generated from one definition (RC4,
  ADR-1852).** New headers `vmafx/vmafx.h` and `vmafx/libvmaf_bridge.h` with
  `vmafx_context_create` / `vmafx_context_destroy`, version, provenance,
  extractor and feature-score queries and errors that name what failed; a
  standard-library Python binding (`bindings/python/vmafx/`); and
  `scripts/codegen/vmafx-api.py`, which generates the headers, the binding,
  the ABI layout test, the reference page and the `libvmaf.h` shims for
  `vmaf_init`, `vmaf_close`, `vmaf_version` and `vmaf_feature_score_at_index`
  from `core/api/vmafx.toml`. `libvmaf.h` behaviour is unchanged. ABI 0.1 is a
  preview until `v1.0.0`. See [the VMAFx API page](docs/api/vmafx/index.md)
  and [API generation](docs/development/api-generation.md).


- **VMAFx API generator: header split, symbol versions and ABI gates (RC4,
  ADR-1852).** The VMAFx API headers follow the design's layout:
  `vmafx/vmafx.h` includes `version.h`, `types.h`, `error.h`, `context.h`,
  `device.h`, `frame.h`, `model.h`, `score.h`, `provenance.h`, `report.h`,
  `dnn.h` and `mcp.h`, each usable on its own; `vmafx/libvmaf_bridge.h` stays
  optional. On Linux every `vmafx_*` symbol carries the version node of the ABI
  minor that introduced it (`VMAFX_0.1`). The definition format
  (`core/api/vmafx.toml`) gains header groups, callbacks, flag sets, fixed
  arrays, nested sized structs, per-entry `since` and `deprecated`, and option
  groups; `scripts/codegen/vmafx-api.py` also writes the linker version
  script, the Windows export list, the exported-symbol list, the header
  install list, one reference page per header, and a changelog draft
  (`--changelog <ref>`). New Meson tests check the definition is append-only
  against the merge base and run the generator's own tests. See
  [API generation](docs/development/api-generation.md).


- **A Windows CUDA tester zip measures every CUDA twin on a tester's Windows PC**
  (ADR-1516, `T-CUDA-WINDOWS-BUILD-NEVER-RUN-ON-A-GPU-2026-10-04`).
  `vmafx-tester-windows-x64-cuda-<version>.zip` is the Windows tester zip with
  the MSVC build's CUDA backend: `run.cmd` runs the CPU checks and, on every
  NVIDIA GPU of the RTX 30 series or newer, every CUDA twin against the CPU,
  the parity gate and the CUDA device tests, through the display driver's
  `nvcuda.dll`. The zip ships no NVIDIA file; it carries the CUDA Toolkit EULA
  for the NVIDIA code inside the kernels and the nv-codec-headers notices. It
  is the first run of the Windows CUDA build on a GPU: the hosted Windows lanes
  only compile it. See
  [the tester guide](docs/usage/tester-image.md#with-an-nvidia-gpu-the-cuda-zip).


- **A Windows SYCL tester zip measures every SYCL twin on a tester's Intel GPU**
  (ADR-1566). `windows-tester-bundle.yml` builds a fourth zip,
  `vmafx-tester-windows-x64-sycl-<version>.zip`, with Intel's `icx-cl`. It carries the
  SYCL device tests, the parity gate, the scratch audit and the SYCL row map, and runs
  them on every Intel GPU of the PC through its own Level Zero loader. `-fsycl`
  requires the dynamic C runtime, so the Visual C++ runtime DLLs, Intel's
  `credist.txt`-listed SYCL runtime and the loader lie beside every program. The
  Windows SYCL build has never run on a GPU (`T-SYCL-WINDOWS-BUILD-NEVER-RUN-ON-A-GPU-2026-10-04`).


- **Windows tester zips for x64 and Arm64** (ADR-1515,
  `T-TESTER-WINDOWS-NATIVE-EVIDENCE-2026-10-04`). A tester unpacks
  `vmafx-tester-windows-<x64|arm64>-<version>.zip` and runs `run.cmd` from
  PowerShell or the Command Prompt; it writes the same JSON report as the other
  tester packages. The zip holds the MSVC build of `vmaf.exe` and its unit tests
  with the C runtime linked in (`/MT`), the Netflix test videos and a bundled
  Python interpreter, so nothing needs to be installed. The report compares the
  MSVC build's AVX2 and AVX-512 (or NEON) code with its scalar code and with
  scores recorded by the same build, and runs the SIMD, dispatch and
  Windows-only unit tests. The zips are built by the hosted Windows runners
  (`.github/workflows/windows-tester-bundle.yml`), carry their licence notices,
  and are published with a build attestation, an attested SPDX SBOM and a cosign
  signature as `tester-windows-*` prereleases. The report gains `--output` to
  write the JSON as UTF-8, the host platform `windows` and the package kind
  `windows-zip`. See [section F of the tester guide](docs/usage/tester-image.md#f-native-windows-zip-x64-or-arm64).


### Changed

- Migrated the Windows MSYS2 MinGW build matrix leg in
  `.github/workflows/libvmaf-build-matrix.yml` from the deprecated `MINGW64`
  environment linking legacy `msvcrt.dll` to `UCRT64` linking the Universal C
  Runtime (`ucrtbase.dll`), using `mingw-w64-ucrt-x86_64-*` packages. Updated the
  required status check name in `.github/workflows/required-aggregator.yml` to
  `Windows UCRT64` (ADR-1387, #1609).


- **`core/src/feature/adm.c` is at the lint and HISS standard (ADR-1142).**
  `compute_adm()`, the float ADM driver, was one function of 260 lines with
  eight `goto`s; it keeps its name and signature and is now a 59-line function
  over helpers for the buffers, the wavelet of a scale, a scale's three sums
  and the accumulation over the four scales. clang-tidy reports nothing for
  the file on the cpu, cuda, hip, sycl and arm64 lanes (31 before on each);
  the HISS baseline loses its nine rows (260 to 251). No score changes: every
  recorded `adm` and `float_adm` output and model score is identical on x86
  (scalar, AVX2, AVX-512) and on aarch64 (scalar, NEON). The stale tidy
  baseline entries of `float_adm.c` on the hip, sycl and arm64 lanes are
  removed; the file already measured 0.


- **The ADM headers are at the lint standard (ADR-1142).**
  `core/src/feature/adm_tools.h`, `adm_csf_tools.h`, `adm_options.h` and
  `integer_adm.h` report no clang-tidy finding on the cpu, cuda, hip, sycl and
  arm64 lanes (145, 145, 145, 156 and 145 before). `adm_tools.h` no longer
  carries the nine `ADM_CM_THRESH_S_*` macros of upstream, which nothing has
  expanded since the closed form `adm_cm_thresh3x3_s()` replaced them
  (ADR-1141). No score changes: every object file of an x86 and of an aarch64
  build is byte-identical before and after.


- **The documented range of `integer_aim` is corrected: it is not bounded by
  1.** The fixed-point `adm` extractor reports the additive impairment divided
  by the reference's detail as it is, and `float_adm` clips the same ratio at
  1; both follow upstream Netflix/vmaf. On a reference without detail the two
  differ: a flat grey 64x64 reference against the same picture with isolated
  patches gives `integer_aim` 3.1756 (upstream master prints 3.175585) and a
  float `aim` of 1, and with the default model's weight and floor
  `integer_adm3` 0.5 against a float `adm3` of 0.7. No score changes. The
  metrics guide now states both ranges, the definition of `adm3_score` and
  what to expect on such content, and a test pins both behaviours so that
  neither changes unnoticed (ADR-1417,
  `T-ADM-INTEGER-AIM-ABOVE-ONE-2026-10-01`;
  [features](docs/metrics/features.md)).


- Code comments in `feature_mobilesal.c`, `hip/picture_hip.c` and `picture.h` cite ADR-0639
  (the scaffold-audit P1 record) where they cited ADR-0613, the vmaf-tune dynamic optimizer.


- ADR-0643, ADR-0665, ADR-0666 and ADR-0673 lose the unfilled allocator template block that preceded their real text (their first heading read `<fill in title>`); a status update records it. Four ADR numbers that other ADRs cite and that had no file (ADR-0228, 0636, 0867, 0979) get a short record each, written from the commits and ADRs that name them.


- 101 ADRs gain a generated `## Errata 2026-10-06` block (118 corrections:
  ADR numbers that point at an unrelated record, paths that moved or never
  existed, links with the wrong label, and ADR numbers that never had a file,
  with a pointer to the real record, or to the retired-number registry, or to a
  tombstone record). ADR bodies and status lines are unchanged; ADR-0767 gets
  its block in the status pull request.


- 34 ADR status headers read in the one form the drift gate parses (`- **Status**: Accepted`): 12 bullet variants and 22 table or heading headers (no value changed; empty `Supersedes` rows dropped). ADR-0003 and ADR-0019 link their successor ADR-1277. ADR-1129, ADR-1225 and ADR-0954 gain a dated status update: the pins they quote have moved (`build-config.env` is the authority) and the HIP dispatch strategy file was removed by #2030. `docs/state.md` cites ADR-0639 (scaffold-audit P1) where it said ADR-0613.


- 44 ADRs that a later record supersedes in part carry that in their status line, in the form `Accepted (Superseded-in-part 2026-10-06 by [ADR-N] for <scope>)` (the form of ADR-0667, ADR-1166, ADR-1591 and ADR-1685), and ADR-0272, which ADR-0291 replaces whole, reads `Superseded by [ADR-0291]`. ADR-0566 and ADR-0584 also move from a table header to the bullet header. Bodies are unchanged.


- Accepted with a dated status update: ADR-1762, ADR-1822 and ADR-1828 (implemented by #2101, #2140, #2141), ADR-1202 (singularity is reported separately; the mechanism changed to `SpeedInternalSingularTally`), ADR-0686 (what stands and what ADR-1127, ADR-1151, ADR-1250, ADR-1699, ADR-1852 replaced) and ADR-0709 (implemented except the kuttl breadth, which RC5 finishes). ADR-0767 is Superseded by ADR-1852 and ADR-0780 by ADR-1142. ADR-0613, ADR-0614, ADR-0617 and ADR-0618 stay Proposed for 1.4 (#2261), ADR-0565 and ADR-0459 for 1.5 (#2262), ADR-0401 for 1.2 (#2248); ADR-0388 is a Deferred row in `docs/state.md` (#2241). ADR-1127 reads Accepted (Superseded-in-part by ADR-1151). The drift-gate exception of ADR-1202 is removed and the one of ADR-0613 is renewed to 2027-04-06.


- **Eight deliberate differences from Netflix's libvmaf are recorded, each with
  its measured size and the upstream pull request that would end it**
  (ADR-1479 to ADR-1486): `ciede` on 4:2:2, `speed_temporal` with
  `speed_prescale` above 1, a failing extractor failing the run, integer `adm`
  on frames of 17 to 32 pixels, chroma planes of odd-sized pictures,
  `float_ms_ssim` on anti-correlated frames, `apsnr` of a plane without error,
  and `float_motion` with `motion_add_scale1` and `motion_add_uv`. No
  behaviour changes; the scores were already these.


- 78 ADRs that still read `Proposed` although the decision is in force now carry
  the status the tree supports: 77 `Accepted` (one scoped to its Phase 1) and one
  `Superseded`. Fifteen stay `Proposed` with the missing part named in the pull
  request. `scripts/ci/check-adr-status-drift.py` fails when a `Proposed` ADR is
  cited by an old implementing commit, unless an unexpired entry in
  `scripts/ci/adr-status-exceptions.json` names the missing part (5 entries today).


- **RC4 adds a new VMAFx C API and moves the FFmpeg filters to VMAFx names
  (decision record, ADR-1852).** The new API (`vmafx/*.h`, `libvmafx.so.1`)
  and every other surface (bindings, CLI / FFmpeg / MCP / gRPC option tables,
  reference docs) are generated from one definition; `libvmaf.h` stays as a
  separate, deprecated compatibility library until 2.0. Every FFmpeg filter
  capability of the patch series continues under a VMAFx name (`vmafx`,
  `vmafx_tune`, `vmafx_pre`, `-vmafx-profile`), and the `vmaf`-named filters
  are removed in the same RC4 change. Nothing changes in this release; see
  [ADR-1852](docs/adr/1852-vmafx-api-redesign.md) and the
  [roadmap](docs/roadmap.md).


- **The pages `AGENTS.md` imports are grouped and corrected.** `docs/development/rebase-sensitive-invariants.md`
  is now organised under H2 sections by area with a contents list (every invariant kept); its
  stale MCP, HIP and "placeholder ADR" texts and a dangling "See" are fixed. Rule 8 of
  `docs/development/agent-hard-rules.md` names `make docs-fragments-write`, rule 12 the real MCP
  attachment (`docker exec -i vmaf-dev-mcp vmafx-mcp`)
  ([invariants](docs/development/rebase-sensitive-invariants.md),
  [hard rules](docs/development/agent-hard-rules.md)). FFmpeg patch impact: none.


- Rewrote agent-facing documentation into caveman internal register across 6 subtree AGENTS.md files: `cmd/vmafx-controller/AGENTS.md`, `cmd/vmafx-mcp/AGENTS.md`, `compat/python-vmaf/AGENTS.md`, `deploy/helm/vmafx/AGENTS.md`, `mcp-server/AGENTS.md`, and `pkg/tune/AGENTS.md`. All load-bearing tokens and invariants preserved under determinism and context gates.


- Rewrote agent-facing documentation into caveman internal register across 10 subtree AGENTS.md files: `ai/AGENTS.md`, `dev/AGENTS.md`, `cmd/vmafx-node/AGENTS.md`, `pkg/libvmaf/AGENTS.md`, `ai/sidecar/AGENTS.md`, `docker/AGENTS.md`, `bindings/rust/vmafx-sys/AGENTS.md`, `requirements/AGENTS.md`, `tools/vmaf-roi-score/AGENTS.md`, and `tools/rc1-tester/AGENTS.md`. All load-bearing tokens and invariants preserved under determinism and context gates.


- Rewrote agent-facing documentation into caveman internal register across 6 subtree AGENTS.md files: `gen/go/AGENTS.md`, `docs/research/AGENTS.md`, `internal/app/scoringservice/AGENTS.md`, `.zed/AGENTS.md`, `pkg/model/AGENTS.md`, and `api/vmafx/v1/AGENTS.md`. All load-bearing tokens and invariants preserved under determinism and context gates.


- **Core and GitHub AGENTS.md files use the internal register.**
  `core/AGENTS.md`, `core/test/AGENTS.md`, `.github/AGENTS.md`,
  `core/src/AGENTS.md`, `core/tools/AGENTS.md`, `core/src/dnn/AGENTS.md`,
  `core/src/hip/AGENTS.md`, `core/src/cuda/AGENTS.md`, `scripts/AGENTS.md`,
  and `core/src/sycl/AGENTS.md` conform to the caveman register required by
  ADR-1249. Every code span, command, identifier, link, and invariant is
  preserved verbatim and verified against `praetorctl caveman check` and
  `caveman_keep_check.py`.


- **Remaining core and scripts AGENTS.md files use the internal register.**
  `core/src/metal/AGENTS.md`, `core/src/mcp/AGENTS.md`,
  `core/include/libvmaf/AGENTS.md`, `scripts/lib/AGENTS.md`,
  `scripts/dev/AGENTS.md`, and `.zed/AGENTS.md` conform to the caveman register
  required by ADR-1249. Every code span, command, identifier, link, and
  invariant is preserved verbatim and verified against `praetorctl caveman check`
  and `caveman_keep_check.py`.


- **`ai/AGENTS.md` is now a generated index over one page per topic.**
  Its text moved unchanged into 33 pages under `ai/AGENTS.d/`; the
  index is 11 384 bytes where the file was 82 517 bytes
  ([ADR-1454](docs/adr/1454-agents-index-and-topic-pages.md),
  [agents index and topic pages](docs/development/agents-index.md)).


make `core/src/AGENTS.md` a generated index over `AGENTS.d/` topic pages ([ADR-1454](../docs/adr/1454-agents-index-and-topic-pages.md)).


- **`core/test/AGENTS.md` is now a generated index over one page per topic.**
  Its text moved unchanged into 18 pages under `core/test/AGENTS.d/`; the
  index is 8 157 bytes where the file was 44 812 bytes
  ([ADR-1454](docs/adr/1454-agents-index-and-topic-pages.md),
  [agents index and topic pages](docs/development/agents-index.md)).


- **`core/tools/AGENTS.md` is now a generated index over one page per topic.**
  Its text moved unchanged into 13 pages under `core/tools/AGENTS.d/`; the
  index is 3 848 bytes where the file was 28 649 bytes
  ([ADR-1454](docs/adr/1454-agents-index-and-topic-pages.md),
  [agents index and topic pages](docs/development/agents-index.md)).


make `core/AGENTS.md` a generated index over `AGENTS.d/` topic pages ([ADR-1454](../docs/adr/1454-agents-index-and-topic-pages.md)).


- **`dev/AGENTS.md` is now a generated index over one page per topic.**
  Its text moved unchanged into 14 pages under `dev/AGENTS.d/`; the
  index is 4 018 bytes where the file was 26 203 bytes
  ([ADR-1454](docs/adr/1454-agents-index-and-topic-pages.md),
  [agents index and topic pages](docs/development/agents-index.md)).


- **`core/src/dnn/AGENTS.md` is now a generated index over one page per topic.**
  Its text moved unchanged into 15 pages under `core/src/dnn/AGENTS.d/`; the
  index is 7 639 bytes where the file was 26 021 bytes
  ([ADR-1454](docs/adr/1454-agents-index-and-topic-pages.md),
  [agents index and topic pages](docs/development/agents-index.md)).


- **`core/src/feature/cuda/AGENTS.md` is now a generated index over one page per topic.**
  Its text moved unchanged into 31 pages under `core/src/feature/cuda/AGENTS.d/`; the
  index is 6 238 bytes where the file was 87 417 bytes
  ([ADR-1454](docs/adr/1454-agents-index-and-topic-pages.md),
  [agents index and topic pages](docs/development/agents-index.md)).


- **`core/src/feature/hip/AGENTS.md` is now a generated index over one page per topic.**
  Its text moved unchanged into 40 pages under `core/src/feature/hip/AGENTS.d/`; the
  index is 8 608 bytes where the file was 87 990 bytes
  ([ADR-1454](docs/adr/1454-agents-index-and-topic-pages.md),
  [agents index and topic pages](docs/development/agents-index.md)).


- **`core/src/feature/sycl/AGENTS.md` is now a generated index over one page per topic.**
  Its text moved unchanged into 38 pages under `core/src/feature/sycl/AGENTS.d/`; the
  index is 9 873 bytes where the file was 81 518 bytes
  ([ADR-1454](docs/adr/1454-agents-index-and-topic-pages.md),
  [agents index and topic pages](docs/development/agents-index.md)).


- **`core/src/feature/x86/AGENTS.md` is now a generated index over one page per topic.**
  Its text moved unchanged into 16 pages under `core/src/feature/x86/AGENTS.d/`; the
  index is 6 787 bytes where the file was 26 912 bytes
  ([ADR-1454](docs/adr/1454-agents-index-and-topic-pages.md),
  [agents index and topic pages](docs/development/agents-index.md)).


- **`core/src/feature/AGENTS.md` is now a generated index over one page per topic.**
  Its text moved unchanged into 57 pages under `core/src/feature/AGENTS.d/`; the
  index is 10 546 bytes where the file was 129 503 bytes
  ([ADR-1454](docs/adr/1454-agents-index-and-topic-pages.md),
  [agents index and topic pages](docs/development/agents-index.md)).


- **`.github/AGENTS.md` is now a generated index over one page per topic.**
  Its text moved unchanged into 22 pages under `.github/AGENTS.d/`; the
  index is 6 397 bytes where the file was 40 199 bytes
  ([ADR-1454](docs/adr/1454-agents-index-and-topic-pages.md),
  [agents index and topic pages](docs/development/agents-index.md)).


- `core/src/hip/AGENTS.md` is now a generated index over 12 topic pages
  under `core/src/hip/AGENTS.d/` (ADR-1454).


- `core/src/feature/metal/AGENTS.md` (34 KB) is a generated index over 15 topic pages
  under `core/src/feature/metal/AGENTS.d/` (ADR-1454), so an agent that edits one Metal
  twin reads only the pages for the files it touches. The text moved unchanged; the
  migration check reports nothing lost. This closes the last file of the agents-index
  migration.


- **A large subtree `AGENTS.md` is now a generated index over one page per
  topic.** `scripts/ci/AGENTS.md` (93,543 bytes) is the first: its text moved
  unchanged into 46 pages under `scripts/ci/AGENTS.d/`, and the file itself is
  a 14,133-byte index that tells an agent which pages to read for the paths
  it is about to touch. Measured on three tasks, an agent now loads 21% to 23%
  of what it loaded before. To record an invariant in such a directory, edit
  or add a page and run `make docs-fragments-write`;
  `make docs-fragments-check` fails on a stale index, on a page above 12,000
  bytes, on an index above 16,000 bytes and on a page whose path globs match
  no file. `scripts/docs/agents_migration_check.py` proves that a migration
  moved every paragraph and every identifier
  ([ADR-1454](docs/adr/1454-agents-index-and-topic-pages.md),
  [agents index and topic pages](docs/development/agents-index.md)).


- **`tools/vmaf-tune/AGENTS.md` is now a generated index over one page per topic.**
  Its text moved unchanged into 23 pages under `tools/vmaf-tune/AGENTS.d/`; the
  index is 5 822 bytes where the file was 81 122 bytes
  ([ADR-1454](docs/adr/1454-agents-index-and-topic-pages.md),
  [agents index and topic pages](docs/development/agents-index.md)).


- The agent pages for CUDA `vif` and `psnr_hvs`, HIP `cambi` and the HIP
  kernel template record the code shapes the lint work of 2026-10-02
  introduced (staged `filter1d.cu` kernels, `psnr_hvs_load_module()`,
  `cambi_hip_arena_at()`, `hip_handle.h`). `docs/state.md`: the shared
  `float_ssim` / `float_ms_ssim` frame-sum row moved to "Recently closed"
  (all six backend parts were fixed on 2026-10-02), and the HIP and CUDA lint
  row no longer lists the `filter1d.cu` function-size rows as open.


- `AGENTS.md` section 10 (and the compiled vendor context files) and
  `docs/development/licence-provenance-check.md` now add the `upstream` remote
  with `git remote add --no-tags`, so a fresh clone does not fetch Netflix's
  tags back into `VMAFx/vmafx` after their removal (ADR-1805).


- **The aarch64 NEON / SVE2 sources and their tests are at the lint and HISS standard (ADR-1142).**
  `core/src/feature/arm64/{convolve_neon,ssimulacra2_neon,ssimulacra2_sve2,ssimulacra2_host_neon}.c`,
  `ms_ssim_decimate_neon.h`, `core/src/arm/cpu.h`, `core/src/feature/simd_dx.h` and
  the NEON tests under `core/test/` report no clang-tidy finding on the arm64
  lane, and the eleven SSIMULACRA 2 NEON / SVE2 kernels that exceeded 60 lines
  are split into helpers. The scalar parts the NEON, SVE2 and host files each
  carried (XYB, the SSIM and edge-difference sums, the YUV conversion) now live
  once in `core/src/feature/arm64/ssimulacra2_arm64_common.h`. Every operation
  runs in the same order: the NEON and SVE2 parity tests and the aarch64
  Netflix golden gate pass under qemu-user, so no score changes.


- **`testdata/bench_upstream_ab.py` takes its score verdict from the upstream
  parity guard and builds upstream at the recorded parity head** (ADR-1487).
  The `--max-score-delta` option and its `1e-5` ceiling on the six-decimal
  pooled score are gone: the model's values are compared at `%.17g` against
  the allowlist of recorded deviations. `--upstream-ref` still names another
  commit or tag; `--fork-build` names the golden-profile build that is timed
  and checked (default: in the guard's work directory); with `--upstream-bin`
  the parity check is reported as not run. Outside the dev container image
  the verdict is marked advisory.


- `black` and `ruff` now read every Python file in the tree in the pre-commit hooks, `make lint-py`,
  `make format` and `make format-check`, not only `python/ ai/ scripts/ tools/`. About 80 files were
  reformatted and the ruff findings fixed (no behaviour change; the syntax tree of the
  reformatted files is identical); the files that cannot meet a tool are declared with a reason
  and an expiry in `.config/lint-exceptions.d/` (`docs/development/pre-commit-hooks.md`).


- **The BRISQUE model's terms are stated as the LIVE release notice grants
  them.** The fork had described the bundled LIVE model as research- and
  education-only under a "research-use exception". The BRISQUE release notice
  (`LICENSES/LicenseRef-LIVE-BRISQUE.txt`) permits use, copying, modification
  and distribution "for any purpose, provided that the copyright notice in its
  entirety appear in all copies", and asks that LIVE and CPS at UT Austin be
  acknowledged, with two citations, in any publication that reports research
  using it. `NOTICE-brisque`, the model card, the BRISQUE metric page and the
  tester licence record now say so
  ([ADR-1507](docs/adr/1507-brisque-live-notice-terms.md),
  [BRISQUE](docs/metrics/brisque.md#licence-of-the-bundled-model)).


- **CAMBI accepts native 144p encode dimensions (port of Netflix/vmaf `4f3f71b68`).**
  The minimum of `enc_width` and `enc_height` is 144 (was 180 and 150), on the CPU
  extractor and on the CUDA, HIP, SYCL and Metal twins. See
  [CAMBI](docs/metrics/cambi.md).


- **The first-release candidates absorb the work added to 1.0.0 on
  2026-10-05 without new numbers (ADR-1868).** RC4 also brings the new VMAFx
  API, the VMAFx-named FFmpeg filters and provenance on every score; RC5 also
  consolidates the tools, adds the new metrics (ΔE-ITP, PU21, NIQE, BRISQUE,
  Y-FUNQUE+, HDR-SSIM, HDR-MS-SSIM, XPSNR) with exact twins and the Metal
  SpEED twins; RC8 also readies the training tooling; RC9's one-shot retrain
  waits for all of it. See [the roadmap](docs/roadmap.md) and
  [the release guide](docs/development/release.md).


- **The first-release candidates now cover large and unusual inputs and
  device-targeted scoring ([ADR-1880](docs/adr/1880-format-envelope-device-targets.md)).**
  RC3 adds an integer-overflow audit of every extractor and twin at 8K and 16K
  with 16-bit samples and 8K exactness cells; the RC6 and RC7 capability
  tables declare, per backend and device, the supported resolutions (up to
  16K), bit depths, chroma layouts and odd or portrait sizes, each row backed
  by a test; RC8 measures throughput per resolution; RC5 adds device profiles
  (phone, tablet, laptop, TV, VR per eye) that score one decode for several
  displays. See [the roadmap](docs/roadmap.md).


- **`ciede2000` no longer depends on the compiler or on the C library's `powf`
  for its squares; scores of GCC-built binaries move by up to 2e-11.**
  `ciede.c` squared a `float` with `powf(x, 2)`. GCC calls the C library
  there; clang and icx replace the call by a product, which is the correctly
  rounded square, and glibc's `powf` returns the other neighbouring `float` on
  about 0.12 % of the arguments. A GCC build and a clang build therefore
  differed on 65 of 180 measured frames (the Netflix 576x324 pair at 8 to 16
  bits and as 10-bit 4:2:2, Sparks, both 1920x1080 checkerboard pairs, Big
  Buck Bunny at 1920x1080 and 3840x2160), by at most 2.0e-11. The source now
  writes the product, and the 13 `pow(x, 2)` of the formula as products too
  (their values do not change)
  ([ADR-1467](docs/adr/1467-ciede-squares-as-products.md)). x86-64 and
  aarch64 builds with GCC and with clang return the same `ciede2000` on all
  180 frames. What you see: `ciede2000` from a GCC-built binary moves on those
  65 frames by at most 2.0e-11, far below the default `%.6f`; a clang or icx
  build does not move; MSVC and macOS builds compute the same expression as
  every other build (not measured here). The CUDA, SYCL and HIP twins are
  closer to the CPU: at most 5.2e-12 from a GCC build (2.0e-11 before), and
  `ciede_cuda` itself moves by up to 1.1e-13 on 3 of 180 frames because it
  now multiplies as well. `test_ciede_device_math` runs on every
  architecture.


- **`ciede2000` follows Netflix's arithmetic again in two products; scores
  move by up to 1.3e-9.** `ciede2000()` multiplies two `float` chromas under
  a square root and three `float` factors in its rotation term. Netflix's
  source forms those products in `float`; since a CodeQL sweep in May 2026
  (PR #552) this fork widened the first operand to `double`, which changed
  the value. The casts are gone
  ([ADR-1476](docs/adr/1476-ciede-upstream-expression.md)), and the CUDA,
  SYCL and HIP twins form the same `float` products. Measured against
  Netflix master (`9e48141b`, GCC 16.2.1, glibc 2.44) at `--precision max`
  on 327 frames from 8x8 to 3840x2160: 153 frames are identical (7 before).
  The rest has recorded causes: 119 frames differ by at most 2.2e-11 because
  the fork squares a `float` by multiplying where upstream calls
  `powf(x, 2)` (ADR-1467; a property of glibc 2.44's `powf`, absent with
  glibc 2.43), 48 are 4:2:2 input, where the fork reads the
  chroma planes with the right subsampling flags, and 7 are odd frame sizes,
  where the fork rounds the chroma size up. What you see: `ciede2000` moves
  on almost every frame by at most 1.3e-9 (1.0e-8 on frames of 24x24 and
  smaller), far below the default `%.6f`. The twins stay within their
  `1e-9` bound: at most 2.1e-12 from the CPU on an RTX 4090, an Arc A380 and
  a gfx1036 (153 frames, 3840x2160 included). The Netflix golden gate is
  unchanged (271 passed, 12 skipped, x86-64 and aarch64).


- `clang-format` now reads `.hip` and `.metal` sources in the pre-commit hook, `make format`,
  `make format-check` and the native hook; the 18 kernel files that were not clean are
  formatted (line breaks only, device code unchanged). See
  `docs/development/pre-commit-hooks.md`, "clang-format reads `.hip` and `.metal`".


- `docs/state.md`: the two open Pelorus rows (the world-writable x265 fixture and the
  narrow `fopen` of the qp-report reader on Windows) are closed. Both were fixed in
  `VMAFx/pelorus` (issues #60 to #62) and are in the vendored mirror, which
  `scripts/sync-pelorus-interop.sh` reports as free of drift against a fresh clone.


- The ten products that CodeQL's `cpp/integer-multiplication-cast-to-long`
  reported after the upstream-parity reverts (ADR-1475, ADR-1476, ADR-1488) in
  `ciede.c`, `third_party/xiph/psnr_hvs.c`, `x86/psnr_hvs_avx2.c`,
  `arm64/psnr_hvs_neon.c`, `integer_adm_kernels.h`, `adm_tools.h` and `iqa/convolve.c` now
  write the conversion of the product's result explicitly, as
  `sqrt((double)(a * b))`. The arithmetic and the object code are unchanged;
  the query reports only implicit widenings, and the `// codeql[...]` comments
  they carried do not suppress in this repository's CodeQL setup.


- **The exact-twin, replay and recorded-value tests compare floating-point
  results by their bits ([ADR-1502](docs/adr/1502-float-bit-identity-test-helper.md)).**
  `core/test/float_bits.h` holds when two values have the same bit pattern and
  are not NaN, which is stricter than the `==` the tests used: `==` accepted
  +0 for -0. With it, 69 of the 73 open CodeQL alerts are fixed in code
  (`cpp/equality-on-floats`, `cpp/integer-multiplication-cast-to-long`,
  `cpp/missing-header-guard`, `cpp/commented-out-code`,
  `cpp/unused-static-function`). No library or tool object file changes (GCC
  and clang, x86-64 and aarch64), and no score moves.


- Composite actions under `.github/actions/` are now checked in pre-commit, CI and
  `make lint-actions`: the GitHub action schema (`check-github-actions`) and, through
  `scripts/ci/check_composite_actions.py`, their structure and shellcheck of every
  `run:` block. actionlint reads workflows only
  (`docs/development/pre-commit-hooks.md`, "Composite actions").


- **Six more CUDA twins are held to the CPU's bits by the parity gate.**
  `motion_cuda` (also with `debug=true`), `motion_v2_cuda`, `psnr_cuda`,
  `float_ssim_cuda` and `float_ms_ssim_cuda` (with and without `enable_lcs`)
  and `cambi_cuda` return the CPU extractor's scores bit for bit: measured on
  an RTX 4090 at `--precision max` on 196 frames from 40x40 to 3840x2160 at 8
  to 16 bits, full-range noise included, on 200 frames of BBB 3840x2160 and
  under 18 option sets. They are now listed as exact twins, so the gate
  compares them with tolerance 0 where it allowed 5e-5, and
  `test_cuda_exact_twins` asserts equality on a device. The sweep behind it
  covered all 21 gate features and found three defects, fixed separately
  (`float_moment_cuda` and `float_psnr_cuda` on high-bit-depth content, a
  missing `motion` output). With the twins made exact by their own changes,
  every CUDA gate feature is exact except `ciede` (within 1.4e-11) and
  `speed_chroma` (within 1.4e-6), which differ by the math library only
  ([ADR-1457](docs/adr/1457-cuda-exact-twins-declared.md),
  [CUDA backend](docs/backends/cuda/overview.md#exact-twins-declared-as-a-group-2026-10-02)).


- **The CUDA runtime and host files are clean under clang-tidy (ADR-1142).**
  `core/src/cuda/picture_cuda.c` initialises its copy descriptors with their
  memory types instead of a zero that is no enumerator; `picture_cuda.h` and
  `cuda_helper.cuh` get include guards that are not reserved identifiers;
  `integer_psnr_hvs_cuda.c` loads its kernel module in a helper, converts its
  kernel arguments explicitly and bounds its plane loop by the size of the
  header's offset table (14 findings the baseline did not record);
  `integer_cambi_cuda.c`, `common.h` and `cuda_helper.cuh` carry the cited
  suppressions for constructs C requires. Every file under `core/src/cuda/`
  and `core/src/feature/cuda/` measures 0 in the `cuda` lane; its baseline
  drops from 734 to 728. No behaviour change: every CUDA twin returns the
  same values as before on an RTX 4090 (18 192 of 18 192 values of the
  sweep).


- **The CUDA MS-SSIM twin no longer copies frames through the host.**
  `float_ms_ssim_cuda` converted every plane of every frame on the host
  (a device-to-host copy, a wait, `picture_copy()` and an upload); the
  conversion now runs on the device with the same arithmetic, so its scores
  are unchanged and the frame stays on the GPU.


- **The parity gate covers `speed_chroma` on CUDA, at `5e-6`.**
  `speed_chroma_cuda` reproduces the CPU extractor's arithmetic and rounds
  `log2` correctly; the CPU extractor calls the C library's `log2f`, and
  glibc's is the neighbouring `float` for up to 1 % of its arguments.
  Measured on an RTX 4090 at `--precision max` (Netflix 576x324 at 8, 10, 12
  and 16 bits, both 1080p checkerboard pairs, 200 frames of BBB 3840x2160):
  776 of 789 values are identical to the CPU, the other 13 differ by one to
  five steps of the 32-bit score (1.4e-6 at most), and all 789 are identical
  when the CPU run uses a correctly rounded `log2f`
  ([ADR-1430](docs/adr/1430-cuda-speed-chroma-log2f-bound.md)). The twin and
  its scores do not change. The gate had no `speed_chroma` cell before; the
  CUDA parity test compared one score of one frame at `1e-4` on a fixture
  that never reached the scoring path, and now compares all three scores of
  every frame to one part in a million on one that does.


- **Every CUDA kernel is built without FMA contraction, and
  `float_ms_ssim_cuda` is bit-identical to the CPU.** nvcc fuses `a * b + c`
  into one FMA by default; six of the 21 CUDA kernels were built with
  `--fmad=false` and fifteen were not, while the CPU build and the SYCL twins
  never fuse. All kernels now take one flag list
  (`cuda_device_strict_fp_args`), so a plain multiply-add rounds twice on the
  device as it does on the host, and a kernel whose CPU reference fuses on
  purpose writes the fused operation explicitly
  ([ADR-1403](docs/adr/1403-cuda-strict-fp-every-kernel.md)). Measured on an
  RTX 4090 against `--backend cpu` at `--precision max`: `adm`, `vif`,
  `motion`, `motion_v2`, `psnr`, `psnr_hvs`, `float_psnr`, `float_moment`,
  `cambi`, `float_adm`, `float_ssim`, `ssim`, `ssimulacra2`, `speed_chroma` and
  `speed_temporal` produce exactly the values they produced before. `float_ms_ssim_cuda`, whose
  kernels now follow the CPU extractor operation for operation, goes from up
  to 4.4e-6 away to bit-identical on every frame of the Netflix pair, the
  1080p checkerboard pairs and BBB 3840x2160, per-scale `enable_lcs` outputs
  included. `ciede_cuda`, `float_vif_cuda` and `float_motion_cuda` move in
  their last digits and stay inside the cross-backend tolerance where they
  were. No twin is measurably slower at 3840x2160. Re-run any stored CUDA
  output of those four twins.
  `-Denable_nvcc=false` (CUDA kernels through clang) configures again and
  agrees with the nvcc build on 18 of 19 twins
  ([CUDA backend](docs/backends/cuda/overview.md#floating-point-model-no-fma-contraction-adr-1403)).


- **CUDA twins take the CPU extractor's options and arithmetic (ADR-1373).** `psnr_cuda`
  now accepts `enable_mse`, `enable_apsnr`, `reduced_hbd_peak` and `min_sse`
  through the same `core/src/feature/psnr_score.h` helpers as the CPU
  extractor, `apsnr_*` aggregates included; `integer_ssim_cuda` accepts
  `enable_db` / `clip_db`; `float_ssim_cuda` accepts `enable_lcs` (a device
  kernel reduces the L, C and S terms) / `enable_db` / `clip_db`; and
  `float_motion_cuda` accepts `motion_max_val` and weights its debug `motion`
  score by `motion_fps_weight` like the CPU. Before, a model or a
  `--backend cuda --feature psnr=enable_mse=true`-style request that set one of
  these options computed the feature on the CPU, and naming the twin with the
  option failed with `unknown option`. `float_ssim_cuda` now computes the
  CPU's per-pixel `l * c * s` with the CPU's rounding and rounds the frame
  mean to fp32, so with `enable_db` identical frames report the CPU's value
  (identical flat frames: 72.247 dB, where the twin reported `+inf`);
  `integer_ssim_cuda` computes each pixel's term as the CPU does;
  `motion_v2_cuda` publishes the CPU's fps-weighted, capped SAD score and
  emits `motion2_v2` / `motion3_v2` for a one-frame input; `psnr_cuda` sums
  every frame into `apsnr_*` under `--subsample`, and its chroma accumulators
  can no longer be cleared while the chroma kernels run. `float_ssim_cuda`
  still accepts `enable_chroma`, which the CPU `float_ssim` does not have, and
  warns that it is ignored. Measured on an RTX 4090: the PSNR, `motion_v2`
  and `float_motion` options give the CPU's scores exactly, `ssim` stays
  within 7.3e-13 dB and `float_ssim` within 6.9e-6 dB of the CPU; see
  [the CUDA backend guide](docs/backends/cuda/overview.md#cpu-options-on-the-psnr-ssim-and-float-motion-twins).


- **`vif_cuda` is declared bit-identical to the CPU `vif` extractor, with a
  proof that covers every input.** The CPU reads its logarithms from a table
  of 32768 values built with the host math library; `vif_cuda` computes them
  on the device, which on an AMD GPU had moved 77 of the values (ADR-1435). A
  new test launches a probe kernel and compares the device's value with the
  CPU's table for all 32768 entries: all are equal on an RTX 4090 (CUDA 13.4,
  glibc 2.44), although the device's `log2f()` differs from the host's by one
  unit in the last place for 307 arguments. The parity gate now compares the
  CPU and CUDA `vif` cells with tolerance 0; 1392 of 1392 scores on 348
  frames are identical at `--precision max`. No scoring kernel, stored score
  or frame time changes. If the test fails on another host or CUDA release,
  the twin has to read the CPU's table as the HIP twin does
  ([ADR-1456](docs/adr/1456-cuda-vif-device-log2-pinned.md),
  [CUDA backend](docs/backends/cuda/overview.md#vif_cuda-returns-the-cpus-scores-bit-for-bit-2026-10-02)).


- **`vif_cuda=enable_chroma=true` reports its scores as
  `integer_vif_scale0_enable_chroma` to `integer_vif_scale3_enable_chroma`.**
  The CUDA VIF twin cleared its no-op `enable_chroma` option before naming
  its features, so a run with the option reported the default names
  `integer_vif_scale0` to `integer_vif_scale3`. It now names them from the
  options the caller set, as every other extractor does. The scores do not
  change (VIF stays luma-only), and a run without the option keeps the
  default names. A script that reads the old names from an
  `enable_chroma=true` run needs the suffix
  ([ADR-1836](docs/adr/1836-cuda-vif-enable-chroma-names.md)).


- **The CUDA integer VIF filter kernels are assembled from short stages
  (ADR-1142).** The four kernel bodies of
  `core/src/feature/cuda/integer_vif/filter1d.cu` were 133 to 225 lines each,
  the last functions of the GPU feature code above the 60-line limit. They
  now call inlined stages (tile load, taps, rounding, write-back), and the
  8-bit and 16-bit horizontal kernels are one template. The debt baseline
  drops from 260 to 256 recorded infractions. No behaviour change: `vif` and
  every other CUDA twin return the same values as before on an RTX 4090.


- **`vif_cuda` reads the CPU's log2 table instead of computing logarithms on
  the device.** The fixed-point `vif` takes its logarithms from a table the
  host math library fills. The CUDA twin evaluated `log2f()` on the device,
  which gave the same table on an RTX 4090 with CUDA 13.4 and glibc 2.44 but
  would not have to with another math library or CUDA release (on an AMD GPU
  the same construction was wrong on 77 of 32768 values). The twin now
  uploads the CPU's table when it starts and looks every logarithm up, as the
  HIP, SYCL and Metal twins do, so it returns the CPU's scores by
  construction. No score changes on the measured host (1392 of 1392 scores
  on 348 frames identical at `--precision max`, before and after) and the
  frame time is unchanged
  ([ADR-1462](docs/adr/1462-cuda-vif-reads-host-log2-table.md),
  [CUDA backend](docs/backends/cuda/overview.md#vif_cuda-returns-the-cpus-scores-bit-for-bit-2026-10-02)).


- Deleted the 26 Netflix release tags that VMAFx/vmafx had inherited (`v1.0.2`
  to `v1.5.3`, `v2.0.0` to `v2.3.1`, `v3.0.0`, `v1.3.6rc`, `v1.3.7rc`,
  `v3.0.0-rc`), each recorded first in
  `scripts/release/inherited-upstream-tags.json`. `go list -m -versions
  github.com/VMAFx/vmafx` on the repository now lists the fork's versions only;
  `proxy.golang.org` keeps its cache, so pin a version (`go get
  github.com/VMAFx/vmafx@v1.0.0-rc.2`) until its `@latest` shows one. New
  `scripts/release/delete-inherited-upstream-tags.py` (dry run by default) and
  a `pre-push` guard that refuses a Netflix tag or a tag name outside the fork's
  patterns; fetch `upstream` with `--no-tags` (ADR-1805).


- **The dev container is pushed only into a private package.**
  `dev-container-publish.yml` reads the visibility of
  `ghcr.io/vmafx/vmafx-dev-mcp` before it builds and refuses to push unless
  the package is private, also when the visibility cannot be read: the image
  holds the full CUDA toolkit, Intel's oneAPI Base Kit and the ROCm payload,
  which may be used internally but not redistributed
  ([ADR-1564](docs/adr/1564-dev-image-private-guard.md),
  [publishing](docs/development/publishing.md)).


- Every build now stores its GPU device code compressed at the strongest
  setting of its toolchain, through the new Meson option `compress_device_code`
  (default `true`): nvcc compresses every fatbin entry in `size` mode (before,
  only PTX was compressed and the cubins went in raw), hipcc and icpx use
  `--offload-compress` at zstd level 22, and the SYCL SPIR-V image, generated
  by the final link, is compressed there too. The CUDA kernels shrink from
  10.97 MB to 2.60 MB, the HIP kernels for the 25 tester targets from 17.9 MB
  to 1.09 MB, and `libvmaf.so` from 14.2 to 5.9 MB (CUDA), 21.0 to 4.2 MB (HIP)
  and 12.8 to 10.2 MB (SYCL); the device code and every score are unchanged.
  The build fails when a fatbin, code object bundle or SYCL image is stored
  raw, and configure fails when a compiler cannot compress: builds with the
  clang CUDA driver (`enable_nvcc=false`) or AdaptiveCpp need
  `-Dcompress_device_code=false` (ADR-1590).


- **The documentation sidebar no longer lists every ADR.** The `ADRs` entry
  now holds the ADR index, the template and the tag index; individual ADRs and
  tag pages are reached from those pages, from links and from search
  ([ADR-1510](docs/adr/1510-adr-nav-collapse-behind-index.md)). Every page
  stays on the site, and each page is far smaller because the navigation it
  carries shrank: see
  [Documentation site design](docs/development/docs-site-design.md#navigation).
  `scripts/docs/generate-adr-nav.sh` is retired, so a new ADR no longer edits
  `mkdocs.yml`.


- Corrected the C API pages against the public headers and split the overview
  into lifecycle, pictures, and models-and-features pages. The runnable example
  no longer releases pictures after a failed `vmaf_read_pictures` (the context
  owns them) and prints the current score. `api/predictor.md` documents the real
  `vmaftune.predictor` API instead of one that never existed, `api/ladder.md` the
  real `select_knees` / `emit_manifest` signatures, `api/dnn.md` the real
  `vmaf_dnn_verify_signature`, and `api/gpu.md` the implemented D3D11 import and
  the 19 HIP and 17 Metal extractors.


- Rewrote the backend guide around a newcomer's choice: `backends/index.md`
  opens with a table of hardware, backend, build option, `--backend` value, SDK
  and exactness status, then explains selection and numerical agreement. The
  SYCL, HIP and CUDA overviews (13,000, 10,000 and 7,400 words) are now short
  overviews with twin tables and open gaps only; per-twin notes, AOT and
  zero-copy details and the dated history moved to their own pages. Corrected:
  `VMAF_CUDA_DISPATCH` is `direct|graph`, HIP registers 19 extractors (the
  `-ENOSYS` stubs are gone), Metal 17, `ciede_cuda` exists, and
  `--backend sycl --feature cambi` runs the SYCL twin (ADR-1359).


- Corrected the contributor pages against the build, the workflows and the
  scripts. `build-flags.md` documents every Meson option with the project
  defaults (`buildtype=release`, `default_library=both`); `oneapi-install.md`
  gains a "Building with icx / icpx" section (glibc math since ADR-1495, strict
  FP, golden gate on gcc or clang); `cross-backend-gate.md` lists Metal as a
  gate backend and explains exact twins for users; `release.md` matches the
  release-please configuration and the required checks; `ci.md` is split into
  shorter pages. Features that are not wired (eBPF FUSE bypass, perf regression
  gate) are described as such.


- Rewrote the newcomer path of the documentation. The home page leads with
  five steps (get VMAFx, score a first pair, choose a backend, use the CLI, API
  or FFmpeg filter, reference). Getting started offers the published container
  images and the release download next to the source build, and the new
  "Score your first pair" page runs a score on the tracked test clips and
  explains the JSON output. The install pages now give the setup-script
  switches as `ENABLE_CUDA=true` / `ENABLE_SYCL=true` (the documented `=1`
  never had an effect), configure from the repository root with
  `meson setup build core`, install Meson from the hash-pinned lock on Ubuntu,
  add the MSVC `/experimental:c11atomics` flag, cover Metal on macOS, and share
  one Intel QSV page instead of four copies. The roadmap explains the release
  candidates for users and cites ADR-1490 for the RC7 to RC9 numbering.


- Corrected the MCP, server and architecture pages against the code: 24 tools
  in the Go MCP server and 19 in the Python one, environment-only configuration
  of the controller, server and node (ADR-1119; the documented `--port` style
  flags do not exist), the real Prometheus metric names, the CRD group
  `vmafx.dev/v1` with four CRDs, and runtime versions from `build-config.env`.
  The C4 diagrams now render (Mermaid instead of PlantUML), the FAQ no longer
  calls HIP "planned", and benchmark tables without date, host and command are
  marked as not citable.


- Corrected the metric and model pages against the extractor option tables
  and the exact-twin declarations. `features.md` is now the feature index: one
  coverage table of every registered extractor with its GPU twins and their
  exactness, then the option reference; ADM, CIEDE2000, float moment, SpEED and
  the tiny-AI extractors have their own pages. Corrected: `vif_enhn_gain_limit`
  defaults to 100.0, integer `motion` `debug` defaults to false, `motion_v2`
  has seven options, `speed_chroma` emits u, v and uv, the five-frame motion
  window reads frames n-3, n-1 and n+1, and the default model is
  `vmaf_v1.0.16_3d0h`.


- Reorganised the documentation navigation around a newcomer path: Get started
  (install, score a first pair, Docker, releases), Choose a backend, Use VMAFx
  (CLI, FFmpeg, Python, testing on your hardware, encoding workflows), the
  C API, then the reference sections (metrics and models, tiny AI, MCP, server,
  architecture), Development, ADRs, Reference and Records. Every page that was
  in the navigation still is; the pages the documentation audit added, the
  tester image and macOS bundle, the hardware reports and the exact-twin table
  are now reachable from it.


- Corrected the tiny-AI pages and model cards against the code and
  `model/tiny/registry.json`. The tiny-AI index is now an entry page with one
  runnable `--tiny-model` command that links every tiny-AI page. Corrected:
  `vmaf-train` has 15 subcommands, `--tiny-model` takes a path (not a registry
  id), the per-frame output is a feature named after the sidecar, the registry
  has 26 entries, the op allowlist 74, `--feature name=opt=val` replaces the
  nonexistent `--feature_params`, and `fr_regressor_v2` reads a 14-element codec
  block. Cards for feature-vector models warn that the same run must compute
  the features they read.


- Corrected the usage pages against the code and restructured the longest ones.
  `cli.md` documents every flag of `vmaf --help`, says the progress and pooled
  lines appear only on a terminal, that an unknown `--tiny-codec` exits non-zero
  and that the Sigstore bundle comes from `model/tiny/registry.json`.
  `env-vars.md` gives `VMAF_CUDA_DISPATCH` as `direct|graph`, states that only
  `VMAF_SYCL_USE_GRAPH=1` has an effect, and lists the variables the code reads.
  `ffmpeg.md` covers the full patch series (0001 to 0020), the `-1 = disabled`
  device defaults and the filter options it did not document. `docker.md` lists
  the published images, `bench.md` the three features `--validate` checks, and
  the worked examples show the current scores (76.667831 with `vmaf_v0.6.1`,
  82.816060 with the default model).


- Rebuilt the `vmaf-tune` documentation against the argparse code. The overview
  is a short page with a subcommand table that links every topic page; new pages
  cover `corpus`, `predict`, `report`, `auto`, `compare`, `benchmark`,
  `tune-per-shot`, the work directory, prefiltering, multi-pass and the codec
  adapter families. Corrected: the ladder CRF sweep (`20,25,30,35,40`), the 4K
  model (`vmaf_v1.0.16_1d5h_2160`), `--duration-frames`, `--output`, single-value
  `corpus --encoder`, the bisect default model, and the cache (Python API only,
  off by default; the documented cache flags never existed).


- `make docs-build` runs the strict MkDocs build the docs workflow and the
  pre-push gate run, and `make docs-serve` starts the live preview. The Metal
  lane comment of the build matrix no longer calls the build stub-only, ADR-0581
  names ADR-0597 in its status line, and the rebase notes carry the
  `vmaf_cuda_picture_get_pix_fmt()` accessor of PR #1118.


- The CUDA / NVDEC path of the `libvmaf_cuda` FFmpeg filter is no longer called
  "zero-copy" in the documentation: no frame goes through host memory, but each
  decoded frame is copied device to device into libvmaf's picture pool
  (ADR-1685). The HIP upload page no longer says the CUDA backend imports
  external memory (no source file does), and Research-0086 carries a dated note
  that its licence lines predate ADR-1250.


- **Thirty-six pages that were outside the site navigation are audited and reachable.** Each was
  checked against the code: the contributor, CI, architecture, Kubernetes and observability pages
  were corrected (coverage gate and CI job names rewritten from the workflows, publishing, cargo-deny,
  picture v2, MCP direct path and OTel metric names fixed) and added to the navigation; eleven dated
  audits, profiles and plans sit under Records with a snapshot note; the Vulkan image-import page and
  an executed plan were deleted ([navigation](mkdocs.yml)). FFmpeg patch impact: none.


- **The documentation search covers user pages only.** ADR bodies, research
  digests, the rebase notes, the state ledger and the changelog archive leave
  the search index, which shrinks from 29.3 MB (8.9 MB gzipped) to 5.8 MB
  (1.7 MB gzipped) and loads on every first page view. ADR and research titles
  stay findable through the indexes, the ADR tag pages and two new title lists
  (`docs/adr/titles.md`, `docs/research/titles.md`); record text is searched
  with GitHub code search
  ([Documentation site design](docs/development/docs-site-design.md#search),
  [ADR-1512](docs/adr/1512-docs-search-user-pages-only.md)).


- **The documentation site has its own design.** A stylesheet on top of
  Material for MkDocs (`docs/stylesheets/vmafx.css`) sets a palette taken from
  the project banner for light and dark mode, holds prose to about 65
  characters per line, sets headings at weight 600 to 700 in the full text
  colour, and sets tables and admonitions at 14.5 px and code at 14 px. The
  landing page leads with what VMAFx is, a first command, the newcomer steps
  and a card per backend. Inter and JetBrains Mono are served from the site
  under the SIL Open Font License instead of a font CDN, and
  `make docs-fragments-check` holds them to the hashes in their `vendor.json`.
  The design, its tokens and the measured values are described in
  [Documentation site design](docs/development/docs-site-design.md)
  ([ADR-1508](docs/adr/1508-docs-site-toolchain-and-charts.md)).


- **Diagrams in the documentation are figures checked against the code.**
  Eight architecture, pipeline and flow diagrams are drawn with the figure
  engine in `tools/figures/`: the tiny-AI pipeline, backend dispatch, the test
  gates, the merge and release flow, the outside-tester flow, the Phase 4b
  platform, the operator's reconcilers and the controller's job lifecycle. Each
  plays its scenarios in the browser, falls back to a static SVG, carries a text
  description, follows the site's light and dark palette, and names the code it
  was drawn from; `make docs-figures` fails when an anchor no longer exists. The
  ASCII-art diagrams of those pages and the one Mermaid diagram are gone, and
  the site no longer loads the Mermaid script from a CDN
  ([Documentation site design](docs/development/docs-site-design.md#diagrams),
  [ADR-1508](docs/adr/1508-docs-site-toolchain-and-charts.md)).


- The set of GPU twins the parity gate compares exactly is no longer a literal
  in `scripts/ci/cross_backend_calibration.py`: each (feature, backend) is one
  file under `scripts/ci/exact_twins.d/` (`adr:` and `evidence:`), the loader
  validates the directory, and the table in
  `docs/development/cross-backend-exact-twins.md` is generated from it by
  `make docs-fragments-write`. Declaring a twin exact edits no shared file
  (ADR-1428).


- **The NEON float ADM kernels are at the lint and HISS standard (ADR-1142).**
  `core/src/feature/arm64/float_adm_dwt2_neon.c` held the wavelet as one
  function of 136 lines; `float_adm_dwt2_neon()` keeps its name and signature
  and now calls one helper for the vertical pass of a row and one for the
  horizontal pass. clang-tidy reports nothing for it and for
  `float_adm_neon.c` on the arm64 lane (6 and 5 before); the HISS baseline
  loses its row (247 to 246). No score changes: every recorded `adm` and
  `float_adm` output and model score is identical on aarch64 for scalar and
  NEON dispatch. `adm_avx2.c` and `adm_avx512.c` gain their SPDX line, and a
  suppression in `adm_tools.c` that no longer suppressed anything is removed.


- **`float_adm` no longer depends on the processor; its scores move by about
  1e-7 on x86.** One step of float ADM divides two wavelet coefficients. On
  x86 the quotient was formed from the processor's reciprocal-estimate
  instruction (`RCPSS`) and one correction step, as upstream Netflix does.
  That instruction is specified by an error bound, not bit for bit, so the
  same frames could score differently on two x86 machines, and differently
  again on ARM and under MSVC, which never used it. `float_adm` now divides
  on every host and with every compiler
  ([ADR-1442](docs/adr/1442-float-adm-reference-divides.md)). Measured on a
  Ryzen 9 9950X3D against the previous build: 147 of 791 `float_adm` scores
  change, by at most 1.3e-7, and the `vmaf_float_v0.6.1`,
  `vmaf_float_v0.6.1neg` and `vmaf_float_4k_v0.6.1` models by at most 1.2e-5
  on a frame and 2.7e-6 on a clip's mean (Netflix 576x324 at 8, 10, 12 and 16
  bits, both 1080p checkerboard pairs, BBB 3840x2160). The fixed-point `adm`
  and the default models do not change, ARM and MSVC builds do not change,
  the Netflix golden tests pass unchanged, and the extractor is not slower.
  `float_adm_cuda` divides as well: it equals the CPU extractor of any
  machine (2034 of 2034 outputs on an RTX 4090) and no longer measures the
  host's instruction when it starts. Scores stored from an x86 build of an
  earlier release differ from new ones by the amounts above.


- **The x86 float ADM kernels are at the lint and HISS standard (ADR-1142).**
  `core/src/feature/x86/float_adm_avx2.c` and `float_adm_avx512.c` held the
  wavelet as one function of 118 and 196 lines; `float_adm_dwt2_avx2()` and
  `float_adm_dwt2_avx512()` keep their names and signatures and now call one
  helper for the vertical pass of a row and one for the horizontal pass.
  clang-tidy reports nothing for the two files on the cpu, cuda, hip and sycl
  lanes (7 and 26 before on each); the HISS baseline loses its two rows (260
  to 258). No score changes: no dispatch table calls these kernels, and an
  old-against-new comparison of every exported function returns the same bits
  on 71,840 inputs.


- **`float_adm` uses AVX2 and AVX-512 on x86, with unchanged scores
  (ADR-1473).** The wavelet and the contrast-sensitivity stage run through
  kernels that return the scalar code's bits: every four-tap sum starts at
  `+0` and multiplies before it adds, and the filtered CSF value is a double
  product narrowed to float. `--cpumask` selects scalar (63), AVX2 (48) or
  AVX-512 (0); every `float_adm` output and the float model scores are
  identical on all three and identical to the previous release. One thread on
  a Ryzen 9 9950X3D: 1.45 to 1.11 ms per 576x324 frame, 17.9 to 15.1 ms at
  1920x1080, 76.2 to 62.9 ms at 3840x2160. The kernels existed but nothing
  called them, and they differed from the scalar code (a negative zero where
  it returns a positive one, a float product where it multiplies in double).
  Two unused reduction kernels that could not reproduce the scalar sums
  (`float_adm_csf_den_scale_avx2` / `_avx512`, `float_adm_sum_cube_avx2` /
  `_avx512`) are removed.


- **The cross-backend parity gate covers every registered CUDA, SYCL and HIP
  twin.** `speed_temporal` was the one feature whose three GPU twins were
  registered and compared by no gate cell. It is now a gate feature, with a
  derived bound of `4e-5` (five float steps of a score below 128): measured
  at `--precision max` on an RTX 4090, a gfx1036 and an Arc A380, the twins
  return the CPU's value on every frame except 2 of 104 frames of BBB
  3840x2160, where glibc's `log2f` is not correctly rounded and the CPU is
  one float step (4.8e-7) away. The `psnr` cell now compares `psnr_cb` and
  `psnr_cr` as well as `psnr_y`, and the single-feature gate
  (`cross_backend_vif_diff.py`) gained `ssim`. A new test fails when a
  registered twin has no gate feature. The Metal twins stay outside the gate,
  which has no `metal` backend
  ([ADR-1460](docs/adr/1460-gate-speed-temporal-and-uncovered-twins.md),
  [gate guide](docs/development/cross-backend-gate.md)).


- The Required Checks Aggregator now requires praetor's `Go API Compatibility`
  check (`required` list). `scripts/ci/check-aggregator-names.sh` gained the
  `# required-aggregator-job: <name>` marker, which fails when no workflow job
  reports the name, so a rename of a job in a byte-locked workflow can no
  longer pass the name check.


- The Go binaries take the golusoris modules they use from
  `internal/app/bootstrap` (`bootstrap.Core`, `bootstrap.HTTP`) instead of the
  golusoris root package, which imports every module golusoris has. This drops
  59 unused modules from the build, among them the mail client that pulled
  `golang.org/x/crypto/md4` and the password-hashing helper that pulled
  `golang.org/x/crypto/argon2` (ADR-1899). Configuration keys, endpoints and
  behaviour are unchanged.


- **Netflix golden assertions: Netflix's own 2026-04/05 re-records ported verbatim
  (ports of Netflix/vmaf `5c7770080`, `005988ead`, `4679db83c`, `d93495f5c`,
  `e3827e4dd`, [ADR-1828](docs/adr/1828-port-netflix-golden-updates.md)).**
  162 expected values and their `places` in `python/test/quality_runner_test.py`,
  `result_test.py`, `routine_test.py`, `local_explainer_test.py` and
  `vmafexec_test.py` now read exactly as upstream has them; the fork's CPU
  build reproduces all 156 exercised values at upstream's places. The rule
  against editing golden assertions now names this one exception: Netflix's own
  update, copied verbatim after a measurement, never a fork-chosen value.


- **`VMAF_CUDA_DISPATCH` is read, `VMAF_HIP_DISPATCH` is gone
  ([ADR-1571](docs/adr/1571-gpu-dispatch-env-consulted.md)).** Neither
  variable did anything: the functions that read them were never called.
  libvmaf now reads `VMAF_CUDA_DISPATCH` when a CUDA extractor initialises,
  keyed by the extractor's registered name (`VMAF_CUDA_DISPATCH=vif_cuda:graph`
  logs that graph capture is not implemented and runs direct).
  `VMAF_HIP_DISPATCH`, a per-feature HIP switch no other backend has, and the
  unused `vmaf_hip_dispatch_supports()` are removed with their docs; HIP
  routing is unchanged, and setting the variable still has no effect.


- **The CUDA, SYCL and HIP motion twins compute `motion_five_frame_window`.**
  `motion_cuda`, `motion_sycl`, `motion_hip` and the three `motion_v2` twins
  keep the frame two back on the device and derive `motion2` / `motion3` with
  the CPU's own window function, so a GPU run of a `vmaf_v1.0.16_hfr_*`
  model keeps its motion feature on the device. Every output equals the
  CPU's bit for bit on an RTX 4090, an Arc A380 and a gfx1036, and the parity
  gate has two exact cells for it, `motion_mffw` and `motion_v2_mffw`. With
  the option a twin publishes `motion2` and `motion3` at the end of the run,
  as the CPU does. `motion_metal` and `motion_v2_metal` leave the option to
  the CPU extractor. See
  [Motion, five-frame window](docs/metrics/motion.md#five-frame-window) and
  [ADR-1491](docs/adr/1491-gpu-motion-five-frame-window.md).


- **Helm: `storage.mode` accepts `http-serve`, `mount` and `auto`.** The
  previous `rclone` value matched no node mode and was ignored; the schema now
  refuses it. Use `http-serve` (the default) or `auto`. `storage.mountRoot`
  sets `VMAFX_STORAGE_MOUNT_ROOT`, and `VMAFX_RCLONE_CONFIG` is set only when
  `storage.rclone.config` provides the file. See
  [ADR-1526](docs/adr/1526-node-storage-streamed-inputs.md).


- **Three GPU helper headers credit the reference code they reproduce.** They
  were created under `EUPL-1.2` and replay arithmetic of an upstream extractor
  for a twin. Each now names `EUPL-1.2 AND` the licences of exactly that code
  and carries its copyright notices above the fork's
  ([ADR-1474](docs/adr/1474-relicense-helper-headers-and-ci-check.md)):
  `metal/float_ms_ssim_option_semantics.h` adds `BSD-2-Clause-Patent`
  (Netflix); `hip/float_ssim/ssim_decimate.h` adds
  `BSD-2-Clause-Patent AND BSD-3-Clause` (Netflix, Tom Distler);
  `sycl/sycl_integer_ssim_math.h` adds `BSD-2-Clause` (Xiph.Org). All paths
  are under `core/src/feature/`. No code line changes.


- **The cambi and psnr_hvs HIP host code and the cambi device header are
  clean under clang-tidy (ADR-1142).** The `hip` lint lane is configured
  without hipcc and therefore analyses the `-ENOSYS` stubs of the HIP host
  files, not the bodies a device runs. A hipcc build showed 35 findings in
  those bodies: `integer_cambi_hip.c` (24) now binds its device arena through
  one accessor instead of sixteen casts through `void *`, and
  `integer_psnr_hvs_hip.c` (11) passes its kernel arguments with explicit
  conversions. `integer_cambi/cambi_hip_device.h` widens eleven row and
  column offsets before the multiplication (findings the lane sees and its
  baseline did not record). No behaviour change: every HIP twin returns the
  same values as before on a gfx1036 (17 800 of 17 800 values of the sweep).


- **Five more HIP twins are held to the CPU's bits by the parity gate.**
  `motion_hip` (also with `debug=true`), `motion_v2_hip`, `psnr_hip`,
  `integer_ms_ssim_hip` (the HIP twin of `float_ms_ssim`, with and without
  `enable_lcs`) and `cambi_hip` return the CPU extractor's scores bit for bit:
  measured on a gfx1036 at `--precision max` on 178 frames from 480x270 to
  3840x2160 at 8 to 16 bits, and with their options. They are now listed as
  exact twins, so the gate compares them with tolerance 0 where it allowed
  5e-5, and `test_hip_exact_twins` asserts equality on a device. The same
  sweep found `float_psnr_hip` and `float_moment_hip` identical on real clips
  but not on all input (up to 7.6e-8 dB at 10 to 16 bits with large
  differences; second moments up to 1.0e-4 at 16 bits); they stay under their
  tolerance. The table of every HIP twin is in
  [the HIP backend page](docs/backends/hip/overview.md#which-hip-twins-return-the-cpus-bits-2026-10-01)
  ([ADR-1437](docs/adr/1437-hip-exact-twins-declared.md),
  [Research-1437](docs/research/1437-hip-twin-exactness-sweep.md)).


- **`float_motion_hip` emits `motion3` and takes every CPU `float_motion`
  option (ADR-1404).** The HIP twin wrote `motion` and `motion2` only and
  lacked `motion_blend_factor` (`mbf`), `motion_blend_offset` (`mbo`),
  `motion_filter_size` (`mfs`), `motion_add_scale1` (`mdc`) and
  `motion_add_uv` (`mau`), so `--backend hip --feature float_motion` dropped
  `motion3` from the output and a request with one of those options was
  computed on the CPU. It now emits `VMAF_feature_motion3_score` with the
  CPU's blend, selects the blur filter in the kernel, adds the half-size SAD
  with a second kernel and runs both on the U and V planes for
  `motion_add_uv`. On a gfx1036 every option is within 1e-5 of the CPU on the
  Netflix 576x324 pair and a 3840x2160 clip, and `motion` / `motion2` with the
  previous options are bit-identical to the previous build.


- **The HIP runtime and four HIP host files are clean under clang-tidy
  (ADR-1142).** `core/src/hip/kernel_template.c` and `core/src/hip/common.c`
  convert the stream and event handles they keep as `uintptr_t` through
  `hip_handle.h` instead of integer-to-pointer casts; `core/src/hip/stubs.c`
  carries the ADR-1138 `NULL` bracket; `speed_chroma_hip.c`,
  `speed_temporal_hip.c`, `float_adm_hip.c` and `hip_hsaco_stubs.c` lose
  their remaining findings. The `hip` lane baseline drops from 752 to 733
  and the `cpu`, `cuda`, `sycl` and `arm64` lanes by 2 each. No behaviour
  change: every HIP twin returns the same values as before on a gfx1036
  (17 800 of 17 800 values of the sweep).


- **HIP extractors share one upload of each frame (ADR-1408).** Every HIP
  extractor used to copy the planes it reads to the device itself and wait
  for that copy, so a run with several extractors uploaded the same frame
  several times: 31 planes for a 4:2:0 frame pair with thirteen extractors.
  The `VmafContext` now uploads each plane once per frame and the extractors
  read that copy (`psnr`, `float_psnr`, `float_moment`, `ciede`, `ssim`,
  `float_ssim`, `vif`, `float_vif`, `adm`, `float_adm`, `motion`, `motion_v2`
  and `float_motion` on HIP). No score changes: every metric of every frame is
  bit-identical before and after. On a gfx1036
  `--backend hip --model version=vmaf_float_v0.6.1` goes from 57.3 to 46.9 ms
  per 1920x1080 frame (17.5 to 21.3 frames per second) and from 294 to 226 ms
  per 3840x2160 frame; `vmaf_v0.6.1` and runs whose time is all device
  kernels are unchanged. `--subsample` stays correct: a plane an extractor
  still reads is not overwritten.


- **The parity gate bounds `speed_chroma` between the CPU and the HIP twin at
  `5e-6` instead of `5e-5`** (ADR-1452). `speed_chroma_hip` rounds `log2`
  correctly and `speed.c` calls the C library's `log2f`; on a gfx1036 13 of
  990 values differ from a glibc CPU, by 1.4e-6 at most, and none with a
  correctly rounded `log2f` preloaded, the CUDA twin's figures (ADR-1430). No
  score changes. `test_hip_speed_chroma_parity` now compares all three scores
  of every frame on a fixture that reaches the scoring path.


- **Every HIP kernel is built with contraction off (ADR-1407).** hipcc fuses
  `a * b + c` into one multiply-add for device code by default, and all but
  three HIP kernels were built that way, so they rounded differently from the
  CPU extractors they mirror. One flag list, `hip_strict_fp_args`
  (`-ffp-contract=off`, `-fhip-fp32-correctly-rounded-divide-sqrt`), now
  applies to every kernel, and the per-kernel flag table is gone. On a gfx1036
  `float_adm_hip` moves from 2.5e-5 to 2.5e-6 from the CPU on the Netflix
  576x324 pair and `float_ssim_hip` from 1.8e-7 to 1.2e-7; `float_vif_hip`'s
  worst frame moves from 2.7e-5 to 3.8e-5 with the same mean; twelve twins
  produce bit-identical output; every twin stays inside its parity tolerance.
  `float_vif_hip` is 4% slower at 3840x2160 and no other twin changes
  measurably. `test_hip_fp_arith_contract` checks the arithmetic on the
  device and `test_hip_strict_fp_policy.py` the build files.


- **Four HIP twins take the CPU extractor's options (ADR-1382).** `psnr_hip`
  now accepts `enable_mse`, `enable_apsnr`, `reduced_hbd_peak` and `min_sse`
  through the CPU's own `core/src/feature/psnr_score.h`, `apsnr_*` aggregates
  included; `integer_ssim_hip` accepts `enable_db` / `clip_db`,
  `float_ssim_hip` `enable_lcs` / `enable_db` / `clip_db` (`float_ssim_l/c/s`
  computed on the device), and `float_motion_hip` `motion_max_val`, with its
  debug `motion` score now weighted by `motion_fps_weight` like the CPU's.
  Before, a model that set one of these options computed the feature on the
  CPU, and naming the twin with the option failed with `unknown option`.
  `float_ssim_hip` now scores each pixel as the CPU does (`l * c * s` from the
  CPU's luminance, contrast and structure terms) and rounds the frame mean to
  fp32 like the CPU, so with `enable_db` identical frames report what the CPU
  reports (72.247 dB on a flat frame, where the CPU's fp32 arithmetic leaves
  1 - 2^-24) instead of a forced `+inf`; `integer_ssim_hip` scores identical
  frames from 3x3 up exactly 1, as the CPU does. `motion_v2_hip` now stores
  its SAD weighted by `motion_fps_weight` and capped at `motion_max_val` like
  the CPU (it weighted at fold time and never capped) and scores one-frame
  runs; `psnr_hip` now sees every frame under `--subsample`, so `apsnr_*`
  covers the whole clip; `motion_hip` defaults `debug` to false and emits
  `VMAF_integer_feature_motion_sad_score`, as the CPU `motion` does. The
  parity gate (`scripts/ci/cross_backend_parity_gate.py`) takes `--backends
  hip` and a `float_ssim_lcs` cell. On a gfx1036 `psnr_hip` with all four
  options matches the CPU exactly, `apsnr_*` included, and the parity gate
  passes every HIP cell; see
  [the HIP backend guide](docs/backends/hip/overview.md#measured-on-a-gfx1036-2026-10-01).


- **Two workflows, one Rust example and the NIQE moment fit meet the HISS
  rules.** `docker-image.yml` and `pr-type-label.yml` no longer discard a
  command's exit status with `|| true` (a failed label call now prints a
  warning), the `vmafx-sys` `score` example returns an error instead of
  calling `process::exit`, and `niqe_extract_aggd()` is split into three
  helpers with the same float operations in the same order (NIQE scores are
  byte-identical on the Netflix pair). The HISS baseline loses 6 infractions.


- **The vendored Pelorus interop sources are re-vendored at a pin that carries
  the HISS splits.** `pel_blob_pack`, `pel_blob_find_section`,
  `pel_qp_report_from_blocks` and `pel_x265_csv_parse` are split upstream in
  `VMAFx/pelorus` and `core/src/interop/pelorus_*.c`, the Pelorus headers and
  the conformance fixture are re-rendered from that pin with
  `scripts/sync-pelorus-interop.sh --update` (ABI 1.3 unchanged, the
  conformance fixture passes). The pin also brings Pelorus's UTF-8 path opening
  for the qp-report CSV reader. The HISS baseline loses the nine rows the
  mirror carried.


- **The x86 SSIMULACRA 2 kernels meet the HISS-04 size limits.** The 11
  functions over 60 lines in `ssimulacra2_avx2.c`, `ssimulacra2_avx512.c` and
  `ssimulacra2_host_avx2.c` (the XYB conversion, the SSIM and edge-difference
  maps, both blur passes and the picture-to-linear-RGB conversion) are split
  into static helpers with the same intrinsics, FMA pattern and summation order.
  Scores are byte-identical at every dispatch level (scalar, AVX2, AVX-512) on
  the Netflix pair and both 1080p checkerboard pairs at `--precision max`. The
  HISS baseline loses those 11 infractions.


- **The ADR allocator and its tests meet the HISS shell rules (ADR-1142).**
  `scripts/adr/next-free.sh` and its three tests handle the exit status of every
  command they used to discard with `|| true`: a grep that finds nothing is
  accepted by status, a failed fetch or ADR-claim cleanup prints a warning,
  local files are listed by testing that they exist, and the shallow-safety test
  sources the extracted function instead of `eval`ing it. The allocator picks
  the same numbers as before. The HISS baseline loses 31 infractions.


- **The CI gate tests, preflight and the RC3 retest runner meet the HISS shell
  rules (ADR-1142).** The shell tests of the CI gates handle the exit status of
  the commands they used to discard, `scripts/dev/preflight.sh` and
  `scripts/dev/rc3-home-gpu-retest.sh` run under `set -euo pipefail` (every
  probe whose non-zero status means no hit now says so), and
  `scripts/ci/tests/test-preflight-msvcism.sh` pins that the msvcism stage still
  fails on a planted `nullptr` and a single-paren `__attribute__`. The HISS
  baseline loses 18 infractions.


- **The CI gate scripts meet the HISS shell rules (ADR-1142).** The CI gate
  scripts under `scripts/ci/` (ADR numbering, worktree drift, container and
  base-image checks, default-model and local-data contracts, state ledger
  checks, PR classification, release-PR exemption, twin drift, PR-body
  validation and the CUDA and oneAPI installers) accept a grep that finds
  nothing by its exit status and report any other failure, and every `curl` of
  the two installers has a connect and total time limit (`--connect-timeout`,
  `--max-time`). Each gate passes and fails on the same inputs as before. The
  HISS baseline loses 48 infractions.


- **The codex hooks, dev scripts and CLI shell tests meet the HISS shell rules
  (ADR-1142).** The agent hooks under `.codex/hooks/`, the dev-container scripts
  under `dev/scripts/` and the CLI shell tests under `core/tools/test/` handle
  the exit status of every command they used to discard with `|| true`, run with
  the full strict mode (`set -eu`), and bound their daemon loops
  (`SUPERVISOR_ITERATION_CAP`, `PROBE_MAX_CYCLES`). `smoke-probe-loop.sh` keeps
  its embedded Python in variables so `probe_backend` and `_mcp_call` are under
  60 lines. A failed formatter or an unwritable work directory is now reported
  on stderr instead of vanishing. The HISS baseline loses 38 infractions.


- **The dev, release, git-hook and e2e scripts meet the HISS shell rules
  (ADR-1142).** The git hooks (`pre-push-pr-body-lint.sh`, `pre-rebase`, the
  native `pre-commit.sh`), the release scripts, `sync-pelorus-interop.sh`,
  `bench-multi-resolution.sh`, the e2e score smoke test, the ensemble kit and
  the dev helpers handle the exit status of every command they used to discard
  with `|| true`; their `curl` calls have time limits; `pre-commit.sh`,
  `bench-multi-resolution.sh` and `sync-pelorus-interop.sh` are split so no
  function is over 60 lines; the ensemble kit's sourced platform helper turns on
  strict mode only when run directly. New tests pin the native pre-commit hook
  and the benchmark's ok and skip cells. The HISS baseline loses 56 infractions.


- **The predicted `vmaf` score of an icx-built binary moves by up to 7e-12 and
  now matches a GCC build except where Intel's math library differs.** Since
  the strict floating-point flags became a project-wide compiler argument
  ([ADR-1461](docs/adr/1461-strict-fp-every-translation-unit.md), #1829), an
  icx build compiles `svm.cpp`, `predict.c`, `model.c` and `libvmaf.c` with
  `-fp-model=precise -ffp-contract=off`; before, they took `-O3` alone, which
  under icx is its fast floating-point model. No extractor value changed. The
  model score changed on every frame with a non-zero score (160 of 163
  measured frames), by at most 7.05e-12, towards the GCC build: frames whose
  features are identical in both builds but whose `vmaf` differs went from 153
  of 163 to 15 of 163 (Netflix 576x324 frame 42: 83.13509129537665 before,
  83.1350912953696 after, the GCC value). GCC builds did not move. What
  still separates an icx build from a GCC build is Intel's math library
  (`libimf`). The published container images are built with icx; scores
  printed at the default `%.6f` are not affected by a change of this size.
  See
  [build flags](docs/development/build-flags.md#floating-point-contraction-is-off-everywhere).


- **The known-upstream-bugs page lists the upstream GPU defects checked on
  2026-10-05.** The CUDA motion kernel that advanced a 16-bit pointer by the
  byte stride (upstream #1566, fixed upstream by #1552) does not affect the
  fork. The three integer ADM defects of upstream #1564 were fixed in the fork
  earlier. Each row names the fork's code and the test that holds it. See
  [known upstream bugs](docs/development/known-upstream-bugs.md#upstream-gpu-defects-checked-against-the-fork-2026-10-05).


- CI: a push to `master` no longer cancels the workflow runs of the previous
  master commit. The concurrency group of every push-to-master workflow carries
  the commit SHA on master and `cancel-in-progress` stays on for pull request
  refs; publish, release, Scorecard and Pages deploy stay serialised. A contract
  test (`scripts/ci/tests/test_master_concurrency_contract.py`) enforces it
  (ADR-1673).


- **Metal: 18 twins are declared exact after the first Apple device report.** An
  outside tester ran the macOS tester bundle on an Apple M4 Pro (issue #2118,
  `docs/hardware-reports/2026-10-05-apple-m4-pro.json`). The parity gate, holding
  every Metal cell exact at `--precision max`, measured 0 on the four fixtures for
  `float_adm`, `float_moment`, `float_motion`, `float_ms_ssim` (with `_lcs` and
  `_chroma`), `float_psnr`, `float_ssim` (with `_lcs`), `float_vif`, `motion` (with
  `motion_debug` and the five-frame window), `motion_v2` (with the five-frame
  window), `psnr`, `ssim` and `ssimulacra2`, and their parity tests passed every
  `==` case. Each is now a `scripts/ci/exact_twins.d/<feature>.metal` fragment, so
  the gate compares those Metal cells with tolerance 0 without `--hold-exact`.
  The same report, the NVIDIA (RTX 3050) and Intel (UHD 770) reports of
  2026-10-05 are under `docs/hardware-reports/`.


- **Metal kernels compile without fast math or FP contraction**
  (ADR-1498): every `.metal` file takes `-fno-fast-math -ffp-contract=off`,
  so fp32 `+ - * /`, `sqrt` and `fma` are correctly rounded and no `a * b + c`
  is fused behind the source's back, the policy the CUDA, HIP and SYCL
  kernels already follow. The arithmetic of a ported Metal twin lives in a
  header on `core/src/feature/metal/metal_portable.h` that also compiles on
  the host, where a test holds it against the CPU extractor. Guide:
  `docs/backends/metal/index.md`.


- **The tiny model cards quote the terms of the data each model was trained
  on.** `fr_regressor_v1` to `v3`, `vmaf_tiny_v1` to `v4`, `nr_metric_v1`,
  `learned_filter_v1`, `saliency_student_v1` and `v2` and `lpips_sq_v1` were
  trained on the Netflix Public Dataset, KoNViD-1k, BVI-DVC, DUTS-TR or
  ImageNet-derived weights. Each card now quotes those datasets' terms as the
  datasets state them, marks the research-only limits, and states the fork's
  reading for shipping the weights as the fork's own. Two earlier descriptions
  that no dataset page supports were withdrawn. The models stay; RC9 retrains
  them on data cleared for redistribution
  ([ADR-1570](docs/adr/1570-tiny-model-dataset-terms-retrain-rc9.md),
  [dataset terms](docs/ai/training-data.md#dataset-terms)).


- The unit tests compile without an MSVC warning (about 71,000 per Windows job
  before): float tables carry the `f` suffix (every literal checked to equal the
  value the implicit double-to-float conversion gave), narrowing conversions are
  explicit, and C test cases are declared `(void)`. No test value or tolerance
  changed.
- The scoring sources compile without an MSVC warning (about 8,000 C4305 / C4244 /
  C4267 / C4334 sites across the CPU extractors, their SIMD twins and the CUDA
  host code): every implicit double-to-float, 64-to-32-bit and size_t-to-int
  conversion is written out, and a float table carries the `f` suffix only where
  the literal converts to the same bits. No score changes: each touched
  translation unit compiles to the same machine code as before (checked with
  GCC on x86-64, clang on aarch64 and the CUDA host objects), and the Netflix
  golden gate is unchanged.


- **Building `vmafx-node` now generates its eBPF object; none is committed
  ([ADR-1622](docs/adr/1622-bpf-object-generated-at-build-time.md)).** OpenSSF
  Scorecard's `Binary-Artifacts` check flags a committed ELF, so
  `cmd/vmafx-node/bpf/rclonebypass_bpfel.o` is gone from the tree and built
  from `rclone_bypass.bpf.c` by `make node-bpf`
  (`scripts/dev/gen-node-bpf.sh`). Producing it needs clang with the BPF target,
  `llvm-strip` and the libbpf headers; without them the script stops and names
  the tool. The Go code still compiles without the object, and a node built
  without it refuses `VMAFX_EBPF_BYPASS=1`. CI and the container images use the pinned clang 19.1.7
  (`BPF_CLANG_VERSION` in `build-config.env`) and check the object's sha256.
  The node image and its behaviour are unchanged. See the
  [node eBPF build guide](docs/development/node-ebpf-build.md).


- Every published archive and image now uses the strongest compression its
  documented consumers open (ADR-1591). The macOS tester bundle is
  `vmafx-tester-macos-arm64-<version>.tar.xz` (xz level 9, 27 MB instead of
  70 MB; unpack with `tar -xf`). The Windows tester zips deflate every entry at
  zlib level 9: they claimed level 9 but carried level 6, because `zipfile`
  ignores a `ZipFile`'s level for `ZipInfo` entries (2 % smaller). The release
  `models.tar.gz` and `licenses.tar.gz` and the git-archive source tarballs are
  gzip level 9, and every layer the image workflows create is gzip level 9
  (zstd would need Docker Engine 23.0 or later). The `vmaf-rc1-report` zip
  bundle deflates at level 9 too.


- The generated upstream parity allowlist page
  (`docs/development/upstream-parity-allowlist.md`) keeps its "Pending"
  section with "None" when no difference is pending, so links to it stay
  valid. `docs/development/upstream-parity.md` carries the guard's result on
  master after every revert and port landed, measured in the rebuilt dev
  image.


- **The vendored Pelorus interop sources are re-vendored at the pelorus commit that clears their clang-tidy findings.** `scripts/sync-pelorus-interop.sh` pins `5f5614b0229d` (VMAFx/pelorus #78): the conformance test's long checks are split into helpers, blob headers are patched through `memcpy`, and each translation unit carries one cited `modernize-use-nullptr` block. No behaviour or ABI change (ABI 1.3).


- **Restore `adm_sum_cube_s_p3`, `adm_csf_den_scale_s_p3`, and `adm_cm_s_p3` fast-path
  functions in `adm_tools.c` (ADR-0463 / BUG-048 B3).**
  The specialized `adm_p_norm == 3.0` fast-paths eliminate all per-pixel `powf()`
  calls and branch overhead on the hot path for default VMAF evaluation.
  Scores remain 100% bit-identical to the baseline generic path across all 48
  frames on the Netflix 576x324 reference pair at `--precision max`.
  Dispatched once per scale in `core/src/feature/adm.c`.


- `ai/scripts/extract_k150k_features.py`: auto-select `/dev/shm` as the
  YUV scratch directory when `/dev/shm` is writable and has at least 20 GiB
  free (Win 3 — Research-0135).  Eliminates NVMe I/O for the ~1.5 GiB
  per-clip raw YUV intermediate, saving an estimated 5–15 s per clip on
  NVMe-bound hosts.  Falls back to the OS temp directory when `/dev/shm` is
  absent, unwritable, or has insufficient free space.  Pass `--scratch-dir`
  to override auto-selection.


- **`vmaf` reads its two inputs ahead of scoring, on one thread each
  (ADR-1366).** The CLI used to read the reference frame, then the distorted
  frame, then score the pair, all on one thread; at 3840x2160 the two reads
  cost about 7 ms per frame whatever the backend did. Each input now has a
  reader thread that stays up to two frames ahead, so reading overlaps scoring
  and the two files are read at the same time. At 3840x2160 8-bit 4:2:0,
  `--feature psnr` drops from about 7-8 to about 3.5-4 ms per frame on the CPU,
  serial or with `--threads 16`, and `psnr`, `motion` and `adm` on an Arc B580
  from about 8 to about 4; runs limited by extraction keep their speed.
  Scores, frame order, `--frame_cnt`, `--frame_skip_*`, the progress line and
  the exit codes are unchanged, and the JSON is identical at
  `--precision max`. The picture pool holds four more pictures (about 50 MB at
  4K 8-bit). Inputs that may share a read position are still read on the main
  thread: on Linux and macOS the same file or pipe on both sides and
  `--no-reference`, on Windows anything but two regular files. See
  [Input read-ahead](docs/usage/cli.md#input-read-ahead).


- **`cambi_cuda` runs entirely on the device (ADR-1379).** The CUDA CAMBI twin
  no longer downloads the distorted picture, preprocesses it on the host or
  reads the image and mask back at each of the five scales for host c-values
  and pooling: every stage runs on the GPU, the twin reads the plane the CUDA
  engine already uploaded, and each frame reads back 88 bytes and waits once,
  in `collect()`. Scores equal `--backend cpu` to the last bit whenever the
  CPU's own double top-K sum is exact, and otherwise differ only by that sum's
  rounding. Like `cambi.c`, the twin now rejects an adjusted window above
  65 x 65 ("cambi: window_size N too large for reciprocal LUT") instead of
  reading past the reciprocal table. On an RTX 4090 every frame of the Netflix
  576x324 pair and of BBB 3840x2160 equals `--backend cpu`, and a 4K frame
  takes 6.01 ms instead of 64.71 ms. The old twin also crashed on wide, short
  frames such as 1920x128, where the new one matches the fixed CPU extractor.
  See [CAMBI](docs/metrics/cambi.md#cuda).


- **Preallocate pinned host pictures for zero-copy 4K CLI CUDA upload (ADR-1406).**
  The `vmaf` CLI now preallocates pinned host pictures via `VmafPicturePool` when
  running CUDA-accelerated feature extractors, eliminating the synchronous driver
  bounce buffer copy on pageable host memory. At 3840×2160 on RTX 4090, default
  model frame time drops from 5.42 ms to 4.87 ms/frame (10.1% faster), `psnr_cuda`
  from 2.29 ms to 2.10 ms/frame (8.3% faster), `adm_cuda` from 3.99 ms to 3.65 ms/frame
  (8.5% faster), and `vif_cuda` from 2.32 ms to 1.82 ms/frame (21.6% faster). Scores
  remain bit-identical with zero drift across all features. If pinned memory allocation
  fails, the pool transparently falls back to pageable memory.


- **`float_ssim` runs on the CUDA device at every scale and equals the CPU's
  score (ADR-1399).** `float_ssim_cuda` computed scale 1 only, so at
  1920x1080 and 3840x2160 `--backend cuda --feature float_ssim` and models
  computed the feature on the CPU and printed a fallback warning. The twin now
  reduces both pictures on the device the way the CPU does (the automatic
  scale and every `scale` from 1 to 10) and adds its two Gaussian passes in
  double precision like the CPU. On an RTX 4090 every measured frame equals
  `--backend cpu` at `--precision max` (576x324, 1920x1080 and 3840x2160; 8,
  10, 12 and 16 bits; `enable_lcs`, `enable_db` and `clip_db` included), where
  the twin used to be 1 to 3 units in the last fp32 place off on every frame.
  A 3840x2160 frame takes 3.0 ms through the CLI instead of 18.9 ms with the
  CPU fallback (11.0 ms for the CPU extractor on 16 threads). An explicit
  `scale=1` on a large picture is slower than before, 3.9 ms instead of 3.1 ms
  per 3840x2160 frame, because the double-precision sums then cover the full
  picture. `--feature float_ssim_cuda` no longer fails at sizes that decimate;
  it fails only when the reduced picture is smaller than 11x11. See the
  [CUDA guide](docs/backends/cuda/overview.md#float_ssim-runs-on-the-device-at-every-scale-adr-1399-2026-10-01).


- **Compact nonzero terms on device for `psnr_hvs_cuda`, reducing 4K frame time from 12.16 ms to 3.39 ms while preserving bit-exact CPU parity (ADR-1397).**
  The kernel compacts nonzero terms before device-to-host readback via block bitmasks and parallel prefix scan, dropping 4K readback size from 64.8 MB to ~11.0 MB (83% reduction) and reducing the host addition chain from 16.2M terms to ~2.7M terms. Because all kernel terms are non-negative squares and `x + 0.0f == x` in IEEE-754 single-precision float addition, omitting zero terms preserves the CPU's exact sequence and numerical sum bit-for-bit at `--precision max`. Closes `T-CUDA-PSNR-HVS-EXACT-SUM-THROUGHPUT-2026-10-01`.


- **CUDA `psnr_hvs` reads the device pictures directly**
  (`T-CUDA-PSNR-HVS-HOST-ROUNDTRIP-2026-09-29`, the CUDA port of ADR-1369).
  `psnr_hvs_cuda` no longer copies each frame to the host, converts it there and
  uploads float planes: the kernel reads the raw 8- to 12-bit samples, two threads
  per 8x8 block, one launch for every plane. Output is bit-identical to the previous
  twin at 8, 10 and 12 bits; 9- and 11-bit input, which it scored as -1.57 dB and NaN
  (`T-CUDA-PSNR-HVS-ODD-BPC-2026-09-30`), now matches the CPU. On an RTX 4090 a
  3840x2160 frame takes 3.20 ms instead of 18.28 ms (1920x1080: 0.43 instead of
  4.40). See
  [the CUDA backend guide](docs/backends/cuda/overview.md) and
  [the psnr_hvs page](docs/metrics/psnr-hvs.md#gpu-twins), which also explains why
  the CPU extractor differs from every GPU twin by up to 1.1e-2 dB at 3840x2160.


- **`psnr_cuda`, `float_moment_cuda` and the CUDA motion SAD kernel spend
  far less GPU time per frame (ADR-1392).** The kernels added one atomic per
  warp to their 64-bit accumulators, which serialised them in the L2; they
  now add one per block and accumulator. PSNR and moment threads sum eight
  coalesced pixels each, PSNR selects its plane without copying both
  pictures to every thread's stack, and the motion SAD kernel (shared by
  `motion_cuda` and `motion_v2_cuda`) computes its vertical filter pass once
  per block. On an RTX 4090 with 3840x2160 8-bit frames the PSNR kernel drops
  from 1,750.5 to 17.7 us per frame, the motion SAD kernel from 136.5 to
  59.6 us and the moment kernel from 460.0 to 15.3 us (CUPTI, median of three
  traces); scores are unchanged. The whole-frame time barely moves, because
  copying each 4K frame to the device takes about 2.1 ms on that host's PCIe
  link, far longer than any of these kernels now runs.


- **The CUDA SpEED twins run entirely on the device (ADR-1380).**
  `speed_chroma_cuda` and `speed_temporal_cuda` no longer copy their planes to
  the host to filter them or solve the 25x25 eigenvalue problem and QR system
  there: the whole chain runs on the GPU, fed by device-to-device copies of the
  planes the engine already uploaded, with one 40-byte readback and one wait
  per frame. Every rounding the CPU performs is spelled with a round-to-nearest
  intrinsic, so the scores move from within 1e-4 of `--backend cpu` to equal to
  it, against a CPU build that rounds `log2f` correctly and does not fuse
  multiply-adds (an icx build without `-march=native`; a gcc build on glibc
  differs in the last bits on a few frames). On an RTX 4090 every frame of the
  Netflix 576x324 pair and of BBB 3840x2160 is identical, a 4K
  `speed_chroma_cuda` frame takes 6.89 ms instead of 24.90 ms, and
  `speed_temporal_cuda`, which failed at 1920x1080 and above with
  `CUDA_ERROR_INVALID_VALUE`, now runs 4K at 5.88 ms per frame. The `lanczos4`
  prescale of the SYCL and CUDA twins is not exact: up to 5.7e-4 relative from
  the CPU on a smooth 1080p gradient on an RTX 4090. See
  [SpEED](docs/metrics/speed_qa.md#cuda-the-same-chain-on-the-device).


- **`ssimulacra2_cuda` runs entirely on the device (ADR-1391).** The CUDA
  ssimulacra2 twin no longer copies the pictures to the host, converts colour,
  computes XYB, combines the SSIM and edge-difference maps or downsamples on the
  host, and no longer copies five buffers back and waits at every scale: each
  frame is one chain of kernels on the picture stream and one 864-byte readback.
  On an RTX 4090 a 3840x2160 frame takes about 7 ms instead of about 720 ms, a
  1920x1080 frame about 2 ms instead of 239, and a 576x324 frame under 1 ms
  instead of about 17 (the CPU extractor on 16 threads: about 170, 33 and
  2 ms).
  Its score is within about 1e-12 of `--backend cpu` (1.5e-12 at worst on the
  tested content, the same on every run) where it used to match bit for bit:
  the per-pixel terms are the CPU's double-precision expressions, added in a
  fixed tree instead of one after another. 4:0:0 input and frames below 8x8 now
  go to the CPU extractor. See [SSIMULACRA 2](docs/metrics/ssimulacra2.md).


- **The perf-gate guide says the gate is not wired into CI.** `docs/development/perf-gate.md`
  no longer describes a CI step or artifact that does not exist; it documents the by-hand run
  and what wiring it takes (RC8, ADR-1490) ([guide](docs/development/perf-gate.md)). FFmpeg
  patch impact: none.


- **`cambi_hip` runs every CAMBI stage on the device (ADR-1378).** The HIP
  twin no longer preprocesses the picture on the host or copies the image and
  mask back at every scale to compute the c-values and the top-K pooling
  there: each frame is one staged upload of the luma, the whole
  ADR-1357 pipeline on the extractor's stream and one 88-byte read of the
  exact per-scale sums, with `collect()` the only wait. Scores are expected
  to equal `--backend cpu` bit for bit wherever the CPU's own top-K sum is
  exact. `cambi_hip` now also refuses, as the CPU extractor does, a window
  whose adjusted size exceeds 65 x 65 ("cambi: window_size N too large for
  reciprocal LUT"). Not yet run on an AMD device: the verify and timing
  commands are in `docs/state.md` (`T-HIP-CAMBI-HOST-RESIDUAL-2026-09-29`)
  and [CAMBI](docs/metrics/cambi.md#hip).


- **`float_ssim` runs on the HIP device at 1080p and 4K (ADR-1405).**
  `float_ssim_hip` implemented scale 1 only, so pictures with a short side of
  384 px or more were computed on the CPU, with the warning
  `float_ssim_hip cannot run 3840x2160 8-bit pictures with these options`.
  The twin now decimates on the device at the automatic scale and every
  explicit one, with the CPU's reduced planes bit for bit, and falls back only
  when the decimated plane is smaller than the 11x11 SSIM window. On a gfx1036
  `float_ssim` is within 1.8e-6 of the CPU at 3840x2160 and 4.3e-6 at
  1920x1080; a 4K frame takes 5.5 ms against 11.9 ms for the CPU extractor on
  16 threads and 19.7 ms for the previous fallback, a 1080p frame 2.0 ms
  against 2.7 ms and 6.6 ms. Scale-1 scores are unchanged.


- **HIP `psnr_hvs` uploads native samples and converts on device (ADR-1369 port).**
  `integer_psnr_hvs_hip` uploads raw native samples via `vmaf_hip_picture_upload()`
  (uint8_t for 8 bpc, uint16_t for 9–12 bpc), converts them to integers on the
  device in `psnr_hvs_score.hip`, and fuses all plane dispatches into a single
  kernel (`n_dispatches_per_frame = 1`). This eliminates host float conversion loops
  and removes 6 unused pinned host staging allocations (`h_uint_ref` and `h_uint_dist`).
  On AMD gfx1036, 4K frame time drops from 221.79 ms to 18.90 ms/frame (CPU 16t is 6.04 ms/frame).
  Output scores are within 8.37e-05 dB of CPU reference at 576x324 and within area-scaled
  tolerance at 4K. Also resolves a latent scaling defect on 9-bit and 11-bit depths,
  validated by `test_psnr_hvs_deep_parity`.


- **The HIP SpEED twins run entirely on the device in the CPU's fp32
  arithmetic (ADR-1384).** `speed_chroma_hip` and `speed_temporal_hip` no
  longer copy and filter planes on the host or read the 25x25 covariance back
  for the eigenvalues and the QR solve: each frame is one staged upload, eight
  kernels (the ADR-1358 chain) and one result read, with `collect()` the only
  wait. The kernels are built without FMA contraction and with correctly
  rounded division and square root, so every per-frame score equals the CPU
  extractor's when the CPU's `log2f` is correctly rounded; against a glibc
  build, whose `log2f` misrounds about 0.4 % of arguments, a few chroma frames
  differ in the last float bits. The init-time setup is now one routine,
  `speed_internal_gpu_configure()`, shared with the SYCL twins. Request the
  twins by name (`--feature speed_chroma_hip`). Not yet run on an AMD device:
  see `docs/state.md` (`T-HIP-SPEED-HOST-RESIDUAL-2026-09-29`) and
  [SpEED](docs/metrics/speed_qa.md#hip-device-resident-cpu-fp32-arithmetic).


- **HIP `ssimulacra2` runs entirely on the device (ADR-1390).**
  `ssimulacra2_hip` no longer roundtrips through the host per scale; each frame
  is one upload of the raw Y/U/V planes in `submit()` and one 864-byte readback
  of per-scale totals in `collect()`. YUV-to-linear, XYB, IIR Gaussian blurs with
  a tiled shared-memory row pass (`SS2H_ROW_TILE` rows, single-wave blocks,
  two-slot ring, register prefetch), exact fp32-pair per-pixel SSIM and edge sums
  over a deterministic LDS reduction tree, and 2x2 downsample run on the device.
  On AMD gfx1036, a 3840x2160 frame takes 234.30 ms (CPU 16t takes 148.03 ms) and
  a 576x324 frame takes 5.68 ms (CPU 16t takes 2.89 ms). Scores match CPU
  reference within 1e-9 at `--precision max`: max abs diff is 1.123e-12 on
  576x324 and 5.826e-13 on 4K BBB. Rejects 4:0:0 input at init. See
  [SSIMULACRA 2](docs/metrics/ssimulacra2.md) and
  [ADR-1390](docs/adr/1390-hip-ssimulacra2-device-resident.md).


- **SpEED and `float_vif` with a bilinear prescale compute the column table
  once (port of Netflix/vmaf `78e11b52c`).** The source columns and weights
  of bilinear scaling depend only on the output column, so `speed_chroma` and
  `speed_temporal` compute them once per extractor instance and `float_vif`
  once per frame instead of once per pixel. Scores are bit-identical. With
  `speed_prescale_method=bilinear` (the `vmaf_v1.0.16_3d0h_2160` and
  `vmaf_v1.0.16_5d0h` models) `speed_chroma` alone took 4.8 ms per 3840x2160
  frame instead of 11.2 (one thread, median of three). Unlike upstream, which
  keeps the table on the stack and refuses bilinear outputs wider than 7680,
  the fork has no width limit.


- **SpEED filters only the samples it keeps on non-x86 targets (port of
  Netflix/vmaf `76ea5f03`, [Netflix/vmaf#1653](https://github.com/Netflix/vmaf/pull/1653)).**
  `speed_chroma` and `speed_temporal` blur each plane with a Gaussian
  anti-alias filter and keep one sample in 256. On aarch64 and every other
  target without the AVX2 convolution the extractor now evaluates the filter
  at the kept samples only (`vif_filter1d_dec16_s()`): the vertical pass runs
  for one row in 16 and the horizontal pass for one column in 16 of it. The
  result has the bits of the filter-then-decimate path, so no score changes
  (`core/test/test_speed_filter.c`; before/after reports under `qemu-aarch64`
  are byte-identical). x86 is unchanged. The NEON covariance kernel of the same
  upstream pull request (`15297286`) is not taken: its partial sums are not
  bit-identical to the scalar kernel; `docs/rebase-notes.md` has the numbers.
  `docs/backends/arm/overview.md` showed `--cpumask 0` as the scalar-only
  switch; `--cpumask` takes the bits to mask out, so that is `--cpumask 3` on
  aarch64 (corrected).


- **The SYCL integer ADM twin computes AIM on the device, so the default model's
  ADM no longer falls back to the CPU under `--backend sycl` (ADR-1362).**
  `adm_sycl` now emits `VMAF_integer_feature_aim_score` and
  `VMAF_integer_feature_adm3_score`, and every ADM output (adm2 and the four
  scales included) is now bit for bit equal to `--backend cpu` on
  8-bit and 10-bit input, odd and 17x17 frames, with the default and the
  default model's options. With the default model on an Arc B580, a 3840x2160
  frame drops from 48.4 to 9.2 ms at the default `--threads 0` and from 24.3 to
  9.7 ms at `--threads 16` (CPU backend with 16 threads: 27-32 ms). On a UHD 770
  the same run gets slower (4K: 75.7 to 87.5 ms, and 40.5 to 79.4 ms at
  `--threads 16`), because the iGPU now does the ADM work the CPU used to do
  beside it. The twin also accepts
  `adm_skip_aim`. Before, adm2 and the scales were up to 2.9e-7 from the CPU
  (double host finalisation) and, on 4K content, `integer_adm_scale2` up to
  1.40e-6 (a decouple ratio that wrapped at scales 1-3).
  See [SYCL backend](docs/backends/sycl/overview.md).


- `cambi_sycl` now runs every stage on the GPU — preprocessing, the spatial
  mask, the per-scale decimation and mode filter, the sliding-histogram
  c-values and the top-K pooling — and reads back one 88-byte block per
  frame instead of downloading the image and mask and computing the c-values
  on the host at every scale (ADR-1357). It reads the distorted plane from
  the shared SYCL frame upload and rides the combined command graph with the
  other SYCL extractors. At 3840x2160 (Big Buck Bunny, `--feature cambi_sycl`,
  ms/frame from `t(22) - t(2)`) the Arc B580 goes from 140 to 9.3 and the
  UHD 770 from 944 to 42; the default model on the UHD 770 goes from 976 to
  103 at 4K and from 138 to 20 at 576x324. Scores are bit-identical to `--backend cpu` whenever the CPU's
  own top-K double sum is exact (every 576x324 and 1080p fixture tested, 47
  of 50 Big Buck Bunny 4K frames); on the other frames the device returns the
  exactly rounded pooled mean and the CPU differs by its summation rounding
  (at most 2.2e-15 on Big Buck Bunny). `cambi.c` exports its reciprocal table
  as `vmaf_cambi_reciprocal_lut()` for GPU twins. The CUDA, HIP and Metal
  twins keep the host residual (RC3 rows in `docs/state.md`).


- **`ciede_sycl` is about twice as fast at 4K on Intel Arc.** The SYCL
  ciede2000 extractor no longer upscales U and V to luma resolution on the host
  before every frame. It uploads the planes at their native size and the kernel
  reads chroma at the subsampled position, as the CUDA and HIP extractors do.
  Measured at 3840x2160 8-bit 4:2:0, `--feature ciede_sycl` drops from 17.2 to
  8.4 ms per frame on an Arc B580 and from 52.9 to 46.0 ms on a UHD 770. Scores
  are unchanged, bit for bit.
- The CLI guide corrects the `--threads` default, which is serial (`0`), not
  the host's core count.


- **SYCL CLI preallocates pinned host USM pictures, dropping 4K upload latency from ~2.5 ms to 0.70 ms (ADR-1410).**
  The CLI picture pool allocates pictures in SYCL host USM (`sycl::malloc_host`) when `--backend sycl` is active, avoiding pageable memory staging and host copies on upload. Chroma planes in contiguous pinned host memory bypass staging buffers for direct DMA transfers. Measured on an Intel Arc A380 (Linux `xe` driver) with BBB 3840x2160: upload time dropped from 2.2–3.0 ms per frame down to 0.70 ms steady-state. Scores are bit-identical. Closes `T-SYCL-PAGEABLE-UPLOAD-HOST-STAGING-2026-09-29`.


- **`float_ssim` runs on SYCL at 1080p and 4K (ADR-1370).** The SYCL twin
  implemented only scale 1, so `--backend sycl --feature float_ssim` and every
  model on a picture with a short side of 384 px or more computed the feature
  on the CPU and printed `float_ssim_sycl cannot run 3840x2160 8-bit pictures
  with these options`. `float_ssim_sycl` now applies `float_ssim`'s automatic
  (or explicit, `scale=2..10`) decimation on the device, with planes identical
  to the CPU's bit for bit, and uploads raw samples instead of converting both
  planes to fp32 on the host. At 3840x2160 8-bit on an Arc B580 it takes 6.7 ms
  per frame, against 11.0 ms for the CPU extractor on 16 threads and 29.3 ms
  for the old fallback at the default thread count; scale 1 at 4K drops from
  10.6 to 7.8 ms. `enable_lcs`, `enable_db` and `clip_db` work at every scale.
  Only a plane that decimates below 11x11 still falls back. See
  [the SYCL backend guide](docs/backends/sycl/overview.md#float_ssim-decimation-on-the-device-2026-09-29).


- **SYCL `float_vif` kernels eliminate scratch memory and restore parity on Intel Arc A380 under the Linux `xe` driver (ADR-1395).**
  On the Arc A380 (`dg2-g11`, PCI `56a5`) under the Linux `xe` driver, SYCL kernels that use scratch memory or register spills return corrupted values. `launch_compute<0>` (previously spilled 14080 B private memory) and `launch_decimate<1>` (previously 2432 B private memory) are completely scratch-free (`private_mem_size == 0`, `spill_memory_size == 0` for both JIT and `dg2-g11` AOT). Filter coefficients are now evaluated via compile-time template constants with `#pragma unroll`, and `launch_compute<0>` utilizes the 256-register file (`VmafSyclKernelShape<32, 256>`), while scales 1-3 maintain default 128 GRF occupancy. Parity is restored: `test_sycl_float_vif_parity` and `test_sycl_float_vif_parity_large` pass; max absolute diff vs CPU is < 4e-5 on Netflix 576x324 and < 8e-6 on BBB 4K (was up to 0.3540 on master). 4K runtime improved from 25.61 ms/frame to 19.98 ms/frame (22% speedup).


- **`psnr_hvs_sycl` and `psnr_hvs_hip` compact nonzero terms on the device before readback (ADR-1397).**
  Because $x + 0.0\text{f} == x$ for every float value in the running sum, zero error terms
  contribute nothing to the plane score. Both twins now compute a 64-bit mask of nonzero terms per 8×8 block,
  perform work-group parallel prefix scans, and compact nonzero terms directly on the device into contiguous
  buffers prior to host readback. Readback size drops from ~198.3 MB to ~11.0 MB at 3840×2160 (94.4% reduction),
  from ~49.4 MB to ~1.39 MB at 1920×1080 (97% reduction), and from ~4.35 MB to ~0.21 MB at 576×324 (95% reduction).
  Host summation overhead in `vmaf_psnr_hvs_plane_score_compacted()` shrinks accordingly.
  Throughput at 3840×2160 improves from 39.23 ms to 29.30 ms/frame on Intel Arc A380 (SYCL) and from
  41.18 ms to 35.53 ms/frame on AMD gfx1036 (HIP). Every score on every frame remains bit-identical to
  `--backend cpu` of the same binary at `--precision max`, and the SYCL kernels remain completely scratch-free
  (0 private memory, 0 spills).


- **SYCL `psnr_hvs`, `psnr` and `motion_v2` read the frame the device already
  holds (ADR-1369).** A SYCL run uploads each plane of a frame once, and these
  twins now read it there: `psnr_hvs_sycl` no longer converts all three planes
  to float on the host and uploads them a second time, `motion_v2_sycl` no
  longer re-uploads the reference luma, and chroma goes up once per frame for
  every twin that reads it, from a pinned staging buffer. The psnr_hvs kernel
  runs two work-items per 8x8 block in one dispatch for all planes, and psnr
  adds one atomic per work-group instead of one per pixel. At 3840x2160,
  `psnr_hvs` drops from 17.1 to 7.6 ms per frame on an Arc B580 (16 CPU
  threads: 6.6) and from 124 to 60 on a UHD 770, where `psnr` drops from 25.3
  to 12.3; `motion_v2` loses about 1.6 ms of host copy and upload per frame.
  Every score is bit-identical to the previous twins. On the zero-copy VA import path, which imports luma only,
  `psnr_sycl` and `psnr_hvs_sycl` with chroma now fail the frame with an error
  instead of crashing. See
  [the SYCL backend guide](docs/backends/sycl/overview.md#psnr-psnr_hvs-and-motion_v2-share-the-uploaded-frame-adr-1369-2026-09-29).


- **The SYCL SpEED twins run entirely on the device and match the CPU bit for
  bit (ADR-1358).** `speed_chroma_sycl` and `speed_temporal_sycl` no longer
  filter, factorise the 25x25 covariance or wait on the queue on the host
  between device passes: each frame is one upload, one replayed SYCL graph and
  one result read. On an Arc B580, `speed_chroma` at 3840x2160 drops from 23.3
  to 7.5 ms per frame and `speed_temporal` from 60.4 to 7.6 (CPU with 16
  threads: 7.2 and 37.3); at 576x324 both run in under a millisecond. Every
  per-frame `speed_chroma_u/v/uv` and `speed_temporal` score now equals
  `--backend cpu` exactly, where previously most frames differed by up to
  4.2e-5. Request the twins by name (`--feature speed_chroma_sycl`):
  `--feature speed_chroma --backend sycl` runs the CPU extractor.
  `scripts/dev/speed_gpu_parity.py` checks and times any GPU twin against the
  CPU. See [SpEED](docs/metrics/speed_qa.md).


- **`ssimulacra2_sycl` runs entirely on the device, and `float_ms_ssim_sycl`
  waits once per frame (ADR-1363).** The SYCL ssimulacra2 twin no longer
  converts colour, computes XYB, downsamples or combines the SSIM and
  edge-difference maps on the host between device passes, and no longer copies
  five full-size buffers back per scale: each frame is one upload of the raw
  planes and one 864-byte readback. On an Arc B580 a 3840x2160 frame takes
  33 ms instead of 963 (the CPU extractor on 16 threads takes 167); on a UHD
  770, 445 instead of 1025. Its score is within about 1e-11 of `--backend cpu`
  (6.7e-12 at worst on the tested content, identical on every device) where it
  used to match bit for bit: the device has no fp64 and sums the per-pixel
  terms in a fixed tree of exact fp32 pairs. `float_ms_ssim_sycl` now enqueues
  every scale in `submit()` and waits once in `collect()` instead of once per
  scale; its output is unchanged. 4:0:0 input is rejected by `ssimulacra2_sycl`
  at init. `scripts/dev/speed_gpu_parity.py` takes `--feature` and
  `--max-abs-diff` to check and time any GPU twin. See
  [SSIMULACRA 2](docs/metrics/ssimulacra2.md).


- **Reuse caller-supplied `tmpbuf` in scalar VIF fallback filters (`vif_filter1d_s`, `_sq_s`, `_xy_s`) (ADR-0463 / BUG-048 B4).**
  The scalar VIF fallback paths in `core/src/feature/vif_tools.c` now reuse the
  scratch buffer already allocated by `compute_vif` instead of performing
  per-invocation `aligned_malloc` and `aligned_free` calls. This eliminates
  up to 12 dynamic heap allocations per frame on architectures without AVX2
  float convolution (such as ARM64 and fallback CPU paths) while keeping scores
  100% bit-identical.


- `tools/vmaf-tune`: batch `TuneCache` index writes via in-memory caching and a dirty flag, flushed once per sweep or on LRU eviction rather than rewriting `__index__.json` on every `get()` / `put()`.


- Roadmap: a post-1.0 embedding milestone covers zero-copy device-frame import
  with fences, asynchronous window scores, and Windows / macOS shared libraries with
  a CMake package; the licence stays EUPL-1.2 plus BSD-2-Clause-Patent with documented
  embedding rules (ADR-1685, `docs/roadmap.md`).


- The praetor governance engine moves from `f41e74d` to `6c772713a133`
  (ADR-1351), the newest praetor commit whose own CI is green. Its HISS
  scanners now find 244 existing issues the old engine did not measure: 140
  Python functions over 60 lines, 61 process exits from library code in
  Python, Go and Rust, 36 recursive Python functions and 7 Go calls without a
  deadline. The debt baseline records them, so the ratchet starts from 378
  entries instead of 182. The move refreshes the compiled agent context, the
  README governance block, the Paperclip harness, the branch ruleset template
  (now for `master`), the devcontainer's vendored praetor source and the
  documentation gate's locked files; the devcontainer keeps its
  `vmafx-dev-mcp` base image. `make verify-all` now also runs praetor's
  Documentation Governance gate (`make docs-lint` and `make docs-figures`),
  which needs Node.js 24. `.standards.yaml` declares the text register for
  every surface (agent-only text is `internal`, the terse `caveman` form).
  The audit in the git hooks passes `--offline`, which saves about 40 seconds
  per commit and push. Every workstation's `praetorctl` has to move to the new
  pin when this merges; the two engines do not read each other's trees
  ([CI guide](docs/development/ci.md#moving-the-praetor-pin)).


- The praetor governance engine moves from `0af07a733e65` to `04cc813ff054` (ADR-2153). The documentation gate gets a lint time budget of 240 s (`documentation.lint_timeout_seconds`, measured maximum of a lint child 11.4 s), which ends the intermittent 120 s failures of `Praetor Documentation Governance`. Praetor's clang-tidy translation-unit coverage gate is fed from the repository's own baselines and exception list by `scripts/ci/praetor_tidy_coverage.py` (hook `check-praetor-tidy-coverage`); 770 of 822 units are read and 52 excused. Engine-written files regenerated: `tools/markdownlint/verify.mjs` and the DevContainer bundle. The HISS baseline stays at 0.


- The praetor governance engine moves from `6c772713a133` to `0af07a733e65`
  (ADR-1506). The documentation gate's lock no longer contains `braces`
  (GHSA-vfj7-8cjw-p6xm), which clears the only finding of Scorecard's
  Vulnerabilities check. The gate also gains praetor's `Go API Compatibility`
  workflow. The engine now scans shell, workflow and systemd files, so the
  recorded debt figure rises from 227 to 503; the old engine records no growth
  on the same tree. Every workstation's `praetorctl` has to move to the new pin
  when this merges ([CI guide](docs/development/ci.md#moving-the-praetor-pin)).


- **Cross-backend gate: the `psnr_hvs` tolerance grows with the frame size.**
  The CPU `psnr_hvs` adds every coefficient error of a plane into one `float`,
  so its rounding error grows with the number of 8x8 blocks, and a correct GPU
  twin landed 8.4e-4 dB away at 3840x2160 against a fixed 5e-4 tolerance.
  `cross_backend_parity_gate.py` and `cross_backend_vif_diff.py` now multiply
  the `psnr_hvs` tolerance by √(N / N₅₇₆ₓ₃₂₄) above 576x324 (3.34e-3 at 4K);
  576x324 and smaller frames keep 5e-4. Both gates also stop crashing when a
  score is non-finite on both backends (JSON `null`, for example `psnr_hvs_cb`
  on identical chroma) (ADR-1361).


- **`psnr_hvs` returns Netflix's values again; scores move by up to 9.4e-7 dB
  on some frames.** The masking threshold of `psnr_hvs` is the square root of
  a product of two `float` values. Netflix's source forms that product in
  `float`; since a CodeQL sweep in May 2026 (PR #552) this fork widened it to
  `double`, which put the threshold one `float` step off on about one block
  in twenty. The cast is gone
  ([ADR-1488](docs/adr/1488-psnr-hvs-upstream-mask-product.md)). Measured
  against Netflix master (`9e48141b`, GCC 16.2.1, glibc 2.44) at
  `--precision max` on 319 frames from 8x8 to 3840x2160 at 8 to 12 bits:
  `psnr_hvs`, `psnr_hvs_y`, `psnr_hvs_cb` and `psnr_hvs_cr` are identical on
  every frame, scalar, AVX2 and default dispatch (before: 292, 310, 310 and
  309 of 319). What you see: on 27 of those frames a score moves by at most
  9.4e-7 dB, so a value printed with the default `%.6f` can change in its
  last digit. The AVX2 and NEON functions and the CUDA, HIP and SYCL twins
  return the new value bit for bit: 612 of 612 values identical to the CPU on
  an RTX 4090, a gfx1036 and an Arc A380 (3840x2160 included), and the
  parity gate still compares the three twins with tolerance 0. The SYCL twin
  takes a correctly rounded `float` root of the `float` product, which equals
  the CPU's `double` root rounded to `float` for every product; its integer
  square root (`sqrt_prod_rn()`) is removed. The Netflix golden gate is
  unchanged (271 passed, 12 skipped, x86-64 and aarch64).


- Split the oversized functions of the Python harness (`compat/python-vmaf/`:
  `routine.py`, `core/cross_validation.py`, `core/executor.py`,
  `tools/bd_rate.py`, `tools/testutils.py`) into private helpers so every
  function meets the HISS-04 limits (60 lines, McCabe 10, 50 statements).
  Public names, signatures, scores, output and error messages are unchanged.


- **Python harness: feature discovery, options and filters as Netflix has them
  (ports of Netflix/vmaf `d327ed67b`, `3dee96664`, `560c4e491`, `5c7770080`).**
  A feature reported under several option suffixes now gives one result key
  per suffix instead of only the shortest one; the `Cambi_FR_feature` key of
  the distorted CAMBI score is `..._cambi_*` again instead of
  `..._cambi_encbd*`. `VmafFeatureExtractor` passes `vif_prescale`,
  `vif_prescale_method`, `adm_bypass_cm`, `adm_adm3_apply_hm`, `adm_p_norm`,
  `adm_skip_aim_scale`, `motion_add_scale1` and `motion_add_uv` to the
  executable, and `VmafIntegerFeatureExtractor` passes `adm_skip_aim`; before,
  they were dropped without a word. `Asset` accepts `select_cmd` and runs the
  FFmpeg filters in Netflix's order (`format_cmd` and `fps_cmd` after
  `gblur_cmd` ... `yadif_cmd`). `TrainTestModel` takes a
  `chroma_correction_parameter`. Documented in `docs/usage/python.md`.


- **The Python wheel's `vmafx-mcp` script is a deprecated alias; use `vmaf-mcp`.** `vmafx-mcp`
  is the Go server (`cmd/vmafx-mcp`). For one release the wheel's script of that name prints a
  notice on stderr and hands over to the Go binary when one is on `PATH`, otherwise it runs the
  Python server ([ADR-1521](docs/adr/1521-python-mcp-console-script-name.md),
  [release channel](docs/mcp/release-channel.md)). FFmpeg patch impact: none.


- The first-release candidate map has a new RC7, the CPU capability source of
  truth ([ADR-1490](docs/adr/1490-rc3-rc9-candidate-map-cpu-capability.md)): a
  generated, checked-in table of the CPU features each SIMD kernel needs, a
  per-function disassembly audit for x86 and aarch64, and every dispatch level
  run bit-exact against scalar under Intel SDE and qemu. Benchmarks, profiling
  and tuning move from RC7 to RC8 and the one-shot retrain from RC8 to RC9. The
  release guide, roadmap, retrain runbook, tester guide, model card,
  dependency-bot policy, the `docs/state.md` classification and the
  `vmaf-rc1-report` tool inventory use the new numbering.


- **The first-release candidate plan now has eight candidates.**
  `v1.0.0-rc.3` owns twin exactness (every GPU and SIMD twin returns the CPU
  extractor's scores bit for bit, or carries a measured tolerance), `rc.4` the
  first full Rust metric (the whole `vmaf_v1.0.16_3d0h` path), `rc.5`
  deduplication, `rc.6` a generated per-vendor GPU capability table with a
  static audit of every kernel for every target, `rc.7` benchmarks, profiling
  and tuning, and `rc.8` the one-shot model retrain. Benchmarks and the retrain
  move back so that they run on a tree that is no longer being corrected or
  restructured. The release guide, roadmap, retrain runbook, tester guide,
  model card and `vmaf-rc1-report list-tools` inventory show the new mapping
  (ADR-1421).


- `v1.0.0-rc.3` ships without waiting for outside-hardware reports
  ([ADR-1707](docs/adr/1707-rc3-exit-without-outside-hardware.md)). Outside reports
  arrived on 2026-10-05 (Apple M4 Pro macOS bundle #2118, RTX 3050 #2119, UHD 770 #2116 / #2122),
  closed three Metal rows (`T-GPU-FLOAT-ADM-FRAME-SUM-FLOOR-2026-10-01`,
  `T-GPU-FLOAT-ADM-TINY-FRAME-FLOOR-2026-10-01`, `T-GPU-TWIN-PARITY-GAPS-OUTSIDE-CUDA-2026-09-30`),
  declared 18 Metal twins exact, and verified Ampere sm_86. Release notes for rc.3:
  which twins were verified on which device.

  | Device | Status in rc.3 |
  | --- | --- |
  | NVIDIA RTX 4090 (Ada, sm_89), CUDA | verified: every CUDA twin exact or bounded for the math library |
  | NVIDIA RTX 3050 (Ampere, sm_86), CUDA | verified: 66 of 66 CUDA device tests pass, 19 parity-gate features identical |
  | Intel Arc A380 (Xe-HPG), SYCL | verified, no kernel in scratch memory |
  | Intel Arc B580 and Arc Pro B60 (Xe2), SYCL | verified ([ADR-1501](docs/adr/1501-sycl-float-adm-terms-large-grf-xe2.md)) |
  | AMD gfx1036 (RDNA2 graphics), HIP | verified |
  | Apple M4 Pro (Metal) | verified: 18 twins exact ([ADR-1498](docs/adr/1498-metal-twins-exact-designs.md)) |
  | CPU x86, AVX2 and AVX-512 (Zen 5) | verified against the scalar code |
  | CPU arm64 (NEON, SVE2) | verified under qemu, not on hardware |
  | NVIDIA Hopper, Blackwell, sm_80 Ampere | not yet verified |
  | AMD CDNA, RDNA1, RDNA3 to RDNA4, discrete RDNA2 | not yet verified |
  | Intel Xe-LP and Xe-LPG (UHD, Iris Xe, Arc graphics of Core Ultra) | not yet verified (UHD 770 reports measured; re-run pending after scratch fixes) |
  | Windows builds with an NVIDIA or an Intel GPU | not yet verified (built in CI, never run on a GPU) |

  The five rows that only these devices can close are carried in `docs/state.md`
  under "RC3 carried past rc.3: needs outside hardware", each with its tester
  package ([hardware we need](docs/usage/hardware-we-need.md)). A report that
  arrives later becomes a fix row in the next candidate; a twin that differs
  from the CPU is fixed, not given a tolerance.


- Release plan: RC4 now owns the whole device-memory import API (fences in both directions for CUDA, SYCL, HIP and Metal, NV12 and P010 on the GPU, FFmpeg filters taking hardware frames) next to the first full Rust metric, so `v1.0.0` scores device-resident frames without a host copy. It moved from the post-1.0 embedding milestone; the candidate numbering is unchanged ([ADR-1829](docs/adr/1829-rc4-zero-copy-import.md)).


- **Refactor CUDA test files part 1 for clang-tidy and HISS standard compliance (ADR-1142).**
  Brings 11 CUDA test files (`test_cuda_pic_preallocation.c`, `test_cuda_float_adm_parity.c`,
  `test_cuda_motion3_parity.c`, `test_cuda_psnr_parity.c`, `test_cuda_float_ms_ssim_parity.c`,
  `test_cuda_float_moment_parity.c`, `test_cuda_float_psnr_parity.c`, `test_cuda_speed_chroma_parity.c`,
  `test_cuda_ciede_parity.c`, `test_cuda_motion_v2_parity.c`, `test_cuda_preallocation_leak.c`)
  to 0 warnings in the `cuda` lane, tightening the baseline by 250 warnings (from 1149 to 899).
  Applies file-level ADR-1138 `modernize-use-nullptr` brackets, isolates variable declarations,
  and extracts helpers to satisfy function size and branch complexity constraints.


- **Refactor CUDA test files part 2 for clang-tidy and HISS standard compliance (ADR-1142).**
  Brings 10 CUDA test files (`test_cuda_ssim_parity.c`, `test_cuda_speed_chroma_smoke.c`,
  `test_cuda_speed_singular_parity.c`, `test_cuda_speed_temporal_parity.c`,
  `test_cuda_speed_temporal_smoke.c`, `test_cuda_drain_batch.c`,
  `test_cuda_picture_pinned_overflow.c`, `test_cuda_buffer_alloc_oom.c`,
  `test_cuda_single_frame_flush.c`, `test_cuda_arch_floor.c`) to 0 warnings in the
  `cuda` lane, tightening the baseline by 104 warnings (from 1110 to 1006).
  Applies file-level ADR-1138 `modernize-use-nullptr` brackets, isolates variable declarations,
  and extracts helpers to satisfy function size and branch complexity constraints.


- The oneAPI container image is published as `ghcr.io/vmafx/vmafx:<tag>-oneapi2026`,
  named for the oneAPI release it now carries. The same image is also tagged
  `<tag>-oneapi2025`, so existing scripts keep working, and the Dockerfile
  stage `final-oneapi2025` still builds it. Prefer `-oneapi2026` and
  `final-oneapi2026` in new scripts (ADR-1368).


- **Cloud-native work, GStreamer and an OBS-ready API join 1.0.0; the post-1.0
  roadmap is five themed releases ([ADR-2001](docs/adr/2001-release-scope-1-0-and-roadmap-to-2-0.md)).**
  RC4 adds the versioned scoring API contract, server mode with observability,
  a native GStreamer element with conformance of upstream's `vmaf` element, an
  API ready for OBS Studio and real-time FFmpeg GPU scoring; RC5 adds
  containers, Helm, the operator and the GPU pool arbiter; RC6 adds legacy GPU build variants (CUDA 12.x for sm_50 to sm_72, the Intel legacy compute runtime, every AMD target ROCm emits); RC7 grows to a bit-exact SIMD ladder on x86-64, AArch64, RISC-V, POWER and LoongArch; RC8 adds distributed
  throughput. After 1.0.0, releases 1.1 to 1.5 (integrations and live quality;
  encoder feedback, embedding and platforms; new metrics; metric A/B and more
  data; the next model generation) each run their own candidate cycle, and 2.0
  carries breaking changes only. See [the roadmap](docs/roadmap.md).


- **Ten fork-authored files carry the licence ADR-1250 gives them.** They were
  added in September 2026 with `BSD-2-Clause-Patent` or `BSD-3-Clause-Clear`
  tags, and `scripts/dev/relicense_fork_files.py` classifies them as fork work
  with no veto: no upstream path or name, no notice but the fork's, no outside
  author. They are `EUPL-1.2` now: `core/tools/vmaf_close_retry.c` and `.h`,
  `core/tools/test/test_vmaf_close_retry.c`,
  `core/tools/test/test_vmaf_read_error_exit.sh`,
  `core/test/test_gpu_option_alias_contract.py`,
  `core/test/test_predict_nonfinite_log_output.py`,
  `core/test/test_predict_source_authority.py`,
  `python/test/golden_gate_isolation_test.py`,
  `scripts/ci/setup-golden-build.sh` and
  `scripts/ci/tests/test_golden_gate_makefile_contract.py`. Only the tag line
  changes. The tool's other 31 pending entries are not applied: they are listed
  with what each needs in `T-RELICENSE-CHECK-PENDING-2026-10-02`
  ([ADR-1250](docs/adr/1250-eupl-fork-relicense.md)).


- Three pull-request checks now block a merge (ADR-1687): `Tester Image` (the
  amd64 tester image build and test), `Windows Tester Zip` (the x64 zip build,
  verify and SBOM) and `Release Dry Run` (the release images and the `vmaf-mcp`
  distribution and SBOM, nothing published). The two tester workflows start on
  every pull request and master push and build only when the CI impact planner
  selects their inputs (`tester_image`, `windows_tester_zip` in
  `.github/ci-impact.json`, the former path filters); a run that builds must
  pass. Their source-validation jobs are now named `Validate tester image source`
  and `Validate Windows zip source`. See
  `docs/development/release-workflow-verification.md`.


- The HIP backend is built against ROCm 10.1.0 (`rocm/dev-ubuntu-26.04:10.1.0-full`)
  in CI, the dev container and the published ROCm, node and AMD tester images. Its
  compiler is AMD clang 24 (ROCm 10.0.0 shipped 23); every HIP twin returns the same
  scores as with 10.0.0 on a gfx1036. The images now ship `libLLVM.so.24.0git` and
  `libclang-cpp.so.24.0git` beside the HIP runtime, the GPU target list is unchanged
  (25 targets, `gfx908` to `gfx1250`), and ROCm 10.1.0 no longer installs `rocm-smi`
  (`amd-smi` remains).


- Rust CI now runs `cargo fmt --all --check` and `cargo clippy --workspace --all-targets -- -D warnings`.
  Until now only `vmafx-sys` was linted; `vmafx`, `vmafx-tad` and any crate added to the
  workspace are covered without a workflow edit (`docs/development/rust.md`, "Linting").


- The copyright and SPDX hook now reads `.hip`, `.metal`, `.mm`, `.pyx`, `.rs` and `.sh` as well,
  skips no path by name, and takes its exceptions from a declared list with a reason and an
  expiry per file (`.config/lint-exceptions.d/`, `docs/development/pre-commit-hooks.md`).
  `adm_dwt2_cy.pyx` gains its SPDX line.


- **`scripts/dev/speed_gpu_parity.py` can leave a fixture out, with a stated reason.**
  `--skip-fixture 3840x2160 --skip-reason "<why>"` skips the untracked BBB fixture and prints a
  `SKIPPED` line; a missing fixture file without the option is now a usage error naming the
  file ([SpEED](docs/metrics/speed_qa.md#checking-a-gpu-twin-against-the-cpu)). FFmpeg patch
  impact: none.


- `docs/state.md` lists what stands between master and the `v1.0.0-rc.3` exit.
  Rows that are done leave the RC2 and RC3 dispositions: four close
  (the first full hosted run on master, the SYCL SpEED singular covariance on
  the Arc A380, the `ciede` math-library residual, which ADR-1426's measured
  bound closes, and a stale HIP `float_moment` gate row) and five that were
  already closed are no longer listed. Every remaining RC3 row states what is
  left, where it can be closed (this host, an Xe2 or Xe-LP device, or an Apple
  device) and whether the macOS tester bundle's report measures it.


- `docs/state.md` classifies every open row under the RC3 to RC8 candidate map
  of ADR-1421 (twin exactness, deduplication, GPU capability table, benchmarks
  and tuning, training), in place of the two ADR-1352 labels "RC3 performance
  and backend acceleration" and "RC4 training and model validation". The
  conflict-resolver guide in `docs/development/ci.md` and the home GPU retest
  guide name the new labels.


- `docs/state.md`: every open row opens with its release phase, owner area and
  next step, and the phase table lists each open row once. The 28 rows that
  carried no RC label were assigned to RC3, RC5, RC8 or RC9 (RC3 rows that need
  a device the project does not own sit in the carried group), and the two
  lint-sweep rows of 2026-09-16 are folded into
  `T-TIDY-GPU-LANES-NEWLY-MEASURED-FINDINGS-2026-10-02`.


- `scripts/dev/resolve-state-md-conflict.py` now resolves a conflicted
  `docs/state.md` by a three-way merge of the merge base and both sides, keyed
  by bug id, instead of letting master's side win. A branch that closes, edits
  or deletes a row keeps that change, and a later commit that rewrites a row an
  earlier commit added keeps the rewrite. Disposition rows merge their id lists
  as sets, and repeated rows with one label are folded into one. When both sides
  changed the same row differently the tool writes nothing and names it; rerun
  with `--take NAME=ours|theirs`. It runs `scripts/ci/check-state-md-rows.sh` on
  its result and always writes LF line endings. Its test suite now runs real
  `git rebase` conflicts in CI (ADR-1383).


- **Four CLI tool files meet the lint standard and carry their SPDX line.**
  `core/tools/cli_parse.h`: `CLISettings` is ordered by alignment (36 bytes of
  padding before, 4 after; one analyzer finding), and the three C-shared
  `typedef`s are inside a cited `NOLINT` block. `core/tools/vidinput.h`: its
  eleven `typedef`s and `video_input_pixel_format` are one definition for the C
  readers and the C++ CLI, cited likewise. `core/tools/vmaf_bench.c` and
  `core/tools/vmaf_vpl.c` already measured zero and only gain the line; their
  stale allowances in the SYCL tidy baseline are removed. The CLI behaves the
  same: exit code, standard output, standard error and the output file are
  byte-identical for 76 invocations (every output format, `--precision`,
  `--model`, `--feature`, frame and backend flags, YUV and Y4M input, the
  error paths) under both program names, on x86-64 and on aarch64
  ([ADR-1142](docs/adr/1142-whole-codebase-standards.md),
  [ADR-1250](docs/adr/1250-eupl-fork-relicense.md)).


- **Fourteen core/src and public header files conform to clang-tidy and HISS standards (part 1).**
  The first batch of core library sources and headers (`pdjson.c`, `dict.cpp`,
  `thread_locale.cpp`, `picture_pool.cpp`, `dict.h`, `picture_pool.c`,
  `model.c`, `model.h`, `framesync.c`, `pdjson.h`, `libvmaf/dnn.h`,
  `libvmaf/model.h`, `libvmaf/libvmaf.h`, `mcp/3rdparty/cJSON/cJSON.h`) were brought
  to zero clang-tidy debt and full HISS compliance under
  [ADR-1142](docs/adr/1142-clang-tidy-debt-ratchet.md). Public headers under
  `core/include/libvmaf/` preserve ABI byte-for-byte (HISS-14). Numerical
  correctness is bit-exact across all reference configurations at `--precision max`
  and the Netflix CPU golden gate passes 271/271.


- **Fourteen core/src and public header files conform to clang-tidy and HISS standards (part 2).**
  The second batch of core library sources and headers (`metadata.h`, `opt.h`,
  `libvmaf/picture.h`, `metadata_handler.h`, `picture_pool.h`, `svm.h`,
  `dict_internal.h`, `fex_ctx_vector.h`, `gpu_picture_pool.h`, `picture.h`,
  `ref.h`, `thread_locale.h`, `x86/cpu.h`, `framesync.h`) were brought
  to zero clang-tidy debt and full HISS compliance under
  [ADR-1142](docs/adr/1142-clang-tidy-debt-ratchet.md). Public headers under
  `core/include/libvmaf/` preserve ABI byte-for-byte (HISS-14). Numerical
  correctness is bit-exact across all reference configurations at `--precision max`
  and the Netflix CPU golden gate passes 271/271.


- **Fourteen core/src and public header files conform to clang-tidy and HISS standards (part 3).**
  The third and final batch of core library sources and public headers
  (`libvmaf/feature.h`, `cpu.h`, `mem.h`, `output.h`, `percentile.h`,
  `predict.h`, `read_json_model.h`, `thread_pool.h`, `libvmaf/libvmaf_cuda.h`,
  `x86/cpu.c`, `gpu_picture_pool.cpp`, `arm/cpu.h`, `log.c`, `log.h`) were
  brought to zero clang-tidy debt and full HISS compliance under
  [ADR-1142](docs/adr/1142-clang-tidy-debt-ratchet.md). In `x86/cpu.c`, function
  nesting depth was refactored with guard returns to satisfy
  `readability-function-size`. Public headers under `core/include/libvmaf/`
  preserve ABI byte-for-byte (HISS-14). Numerical correctness is bit-exact across
  all reference configurations at `--precision max` and the Netflix CPU golden
  gate passes 271/271.


- **Ten core C and C++ test files conform to clang-tidy and HISS standards (part 1).**
  The first batch of CPU and core test sources (`test_barten_csf.c`,
  `test_psnr_hvs_simd.c`, `test_propagate_metadata.c`, `test_thread_pool.c`,
  `test_context.c`, `test_predict.c`, `test_ciede.c`,
  `test_pic_preallocation.c`, `test_locale_handling.c`, and `test_dict.cpp`)
  were refactored to zero clang-tidy findings in the CPU lane under
  [ADR-1142](docs/adr/1142-clang-tidy-debt-ratchet.md). Functions exceeding
  branch, nesting, and statement thresholds were split into modular helpers
  satisfying HISS-04 / NASA JPL Rule 4, eliminating 2 recorded infractions from
  `.standards-baseline.json`. C23 `nullptr` diagnostics in C files are scoped under
  [ADR-1138](docs/adr/1138-c23-nullptr-msvc-compat.md), and `test_dict.cpp` uses
  anonymous namespaces and standard `nullptr`. All tests continue to pass.


- **Eleven core C and C++ test files conform to clang-tidy and HISS standards (part 2).**
  The second batch of CPU and core test sources (`test_feature_collector.c`,
  `test_luminance_tools.cpp`, `test_framesync.c`, `test_log.c`, `test_psnr.c`,
  `test_version.c`, `test_adm_csf.c`, `test_cpu.c`, `test_ref.c`,
  `test_moment_simd.c`, and `tiny_ai_test_template.h`) were brought to zero
  clang-tidy findings on the CPU lane (-17 recorded findings) under
  [ADR-1142](docs/adr/1142-clang-tidy-debt-ratchet.md). Functions exceeding
  branch and statement thresholds were split into modular helpers satisfying
  HISS-04 / NASA JPL Rule 4. C23 `nullptr` diagnostics in C files are scoped under
  [ADR-1138](docs/adr/1138-c23-nullptr-msvc-compat.md), and all files retain valid
  SPDX license identifiers. All tests continue to pass.


- **Core test files brought to lint and HISS standards (batch C).**
  The final batch of core C test files (`core/test/test.h`,
  `core/test/test_float_adm_dwt2_neon.c`, `core/test/test_float_adm_neon.c`,
  `core/test/test_float_motion_neon.c`, `core/test/test_motion_neon.c`,
  `core/test/test_psnr_neon.c`, `core/test/test_ssim_neon.c`,
  `core/test/test_gpu_picture_pool.c`, `core/test/test_integer_cambi_sycl.c`)
  was brought to full compliance under [ADR-1142](docs/adr/1142-clang-tidy-debt-ratchet.md).
  `test.h` modernized C++ typedefs to type aliases (`using`) and eliminated redundant
  void parameter lists. Missing SPDX license identifiers were restored per
  [ADR-1250](docs/adr/1250-eupl-fork-relicense.md). C23 `nullptr` diagnostics are
  scoped under [ADR-1138](docs/adr/1138-c23-nullptr-windows-portability.md) to maintain
  MSVC cl.exe compatibility across all architecture blocks. `test_pelorus_interop.c`
  is tracked as an exact-sync vendored Pelorus ABI mirror per
  [ADR-1113](docs/adr/1113-vendor-pelorus-interop.md).


- **Twelve CPU feature extractor and support files conform to clang-tidy and HISS standards (batch 1).**
  The first batch of CPU feature extractors and support headers (`integer_motion.c`,
  `integer_motion.h`, `convolution_internal.h`, `barten_csf_tools.h`, `blur_array.c`,
  `float_vif.c`, `math_utils.h`, `ms_ssim.c`, `ssim.c`, `integer_motion_v2.c`,
  `feature_collector.h`, and `ssim_tools.c`) were refactored to zero clang-tidy findings
  in the CPU lane under [ADR-1142](docs/adr/1142-clang-tidy-debt-ratchet.md). Functions
  exceeding NASA JPL Rule 4 complexity and line-count limits in `integer_motion_v2.c`
  were split into modular helpers, eliminating 2 recorded infractions from
  `.standards-baseline.json`. All 12 reference score configurations remain bit-identical
  at `--precision max` across scalar, AVX2, and AVX-512 cpumasks, and the Netflix CPU
  golden gate passes with unchanged counts.


- **Eleven CPU feature extractor and support files conform to clang-tidy standards (batch 2).**
  The second batch of CPU feature extractors and support headers (`speed_internal.h`,
  `feature_characteristics.h`, `motion.c`, `float_ssim.c`, `float_ms_ssim.c`,
  `vif_tools.h`, `vif_options.h`, `psnr_options.h`, `picture_copy.h`,
  `picture_copy.cpp`, and `null.c`) were refactored to zero clang-tidy findings
  in the CPU lane under [ADR-1142](docs/adr/1142-clang-tidy-debt-ratchet.md). All 12
  reference score configurations remain bit-identical at `--precision max` across
  scalar, AVX2, and AVX-512 cpumasks, and the Netflix CPU golden gate passes with
  unchanged counts.


- **Ten CPU feature extractor and support files conform to clang-tidy standards (batch 3).**
  The third batch of CPU feature extractors and support headers (`alias.c`,
  `feature_name.h`, `iqa/iqa.h`, `iqa/iqa_options.h`, `iqa/iqa_os.h`,
  `moment_options.h`, `motion_blend_tools.h`, `motion_options.h`, `motion_tools.h`,
  and `niqe_model.h`) were refactored to zero clang-tidy findings in the CPU lane
  under [ADR-1142](docs/adr/1142-clang-tidy-debt-ratchet.md). All 12 reference
  score configurations remain bit-identical at `--precision max` across scalar,
  AVX2, and AVX-512 cpumasks, and the Netflix CPU golden gate passes with unchanged
  counts.


- **Eight more feature sources and tests meet the lint and HISS standard and
  carry their SPDX line.** `core/src/feature/integer_ssim.c`: `calc_ssim()`
  (73 lines, the file's one HISS row) takes its two kernels and its row ring
  from `ssim_work_init()` / `ssim_work_free()`, in the allocation order of the
  Xiph.Org original, and loses its `NOLINT`. `core/test/test_feature_collector.c`:
  the two CUDA-only tests are split into helpers with every assertion kept, and
  the duplicate-owner test releases its fixtures before it asserts on the
  releases (one analyzer leak finding). `feature_collector.cpp`,
  `luminance_tools.h`, `moment.c`, `sycl/integer_adm_sycl.cpp`,
  `test_framesync.c` and `test_luminance_tools.cpp` already measured zero and
  only gain the line; their stale allowances in the CUDA, HIP and SYCL tidy
  baselines are removed. No score changes: `ssim`, `float_moment`, `cambi` and
  the default model are identical at `--precision max` under scalar, AVX2,
  AVX-512 and NEON dispatch, and the `adm` SYCL twin is identical to the CPU on
  333 frames ([ADR-1142](docs/adr/1142-whole-codebase-standards.md),
  [ADR-1250](docs/adr/1250-eupl-fork-relicense.md)).


- **Nine HIP test files conform to clang-tidy and HISS standards (part 1).**
  The first batch of HIP test sources (`test_hip_smoke.c`,
  `test_hip_float_adm_parity.c`, `test_hip_motion3_parity.c`,
  `test_hip_float_vif_parity.c`, `test_hip_float_moment_parity.c`,
  `test_hip_cambi_parity.c`, `test_hip_ssimulacra2_parity.c`,
  `test_hip_motion_v2_parity.c`, `test_hip_float_psnr_parity.c`) were brought to
  zero clang-tidy findings in the HIP lane (-245 baseline findings) under
  [ADR-1142](docs/adr/1142-clang-tidy-debt-ratchet.md). Functions exceeding
  branch and statement thresholds were split into clean helpers satisfying
  HISS-04 / NASA JPL Rule 4. C23 `nullptr` diagnostics are scoped under ADR-1138
  to maintain MSVC C portability, and `__HIP_PLATFORM_AMD__` is supplied by the
  build harness (ADR-1263). All tests pass on AMD gfx1036.


- **Ten HIP test files conform to clang-tidy and HISS standards (part 2).**
  The second batch of HIP test sources (`test_hip_wavefront_reduce.c`,
  `test_hip_psnr_hvs_parity.c`, `test_hip_speed_chroma_parity.c`,
  `test_hip_speed_temporal_parity.c`, `test_hip_speed_singular_parity.c`,
  `test_hip_ciede_parity.c`, `test_hip_motion_parity.c`,
  `test_hip_vif_parity.c`, `test_hip_psnr_parity.c`, `test_hip_adm_parity.c`)
  were brought to zero clang-tidy findings in the HIP lane (-156 baseline
  findings) and cross-lanes under
  [ADR-1142](docs/adr/1142-clang-tidy-debt-ratchet.md). Functions exceeding
  branch and statement thresholds were split into clean sub-helpers satisfying
  HISS-04 / NASA JPL Rule 4. C23 `nullptr` diagnostics are scoped under ADR-1138
  to maintain MSVC C portability, and `__HIP_PLATFORM_AMD__` is supplied by the
  build harness (ADR-1263). All tests pass on AMD gfx1036.


- Refactored ai/scripts exporter and validator utilities to satisfy HISS-04 complexity limits (ADR-1142).


- Refactored 12 `ai/scripts/` training and corpus tools to satisfy HISS-04 complexity/length limits, bounded loops, and proper entrypoint error reporting under ADR-1142.


- Refactored 11 `ai/scripts/` training, feature materialization, and model exporter tools to satisfy HISS-01, HISS-04, and HISS-07 standards (ADR-1142).


- Refactored `ai/src/` and `ai/lpips_export.py` functions to resolve HISS-02 while loops and HISS-04 size violations under ADR-1142.


- Refactored oversized test functions in 12 files under `ai/tests/` to satisfy HISS-04 modular size bounds under ADR-1142.


- Refactored Go packages (`cmd/` and `pkg/`) to resolve HISS-02 context timeouts and HISS-07 exit/panic violations under ADR-1142.


- Removed 48 HISS baseline rows from 17 Python modules without a change in behaviour (ADR-1142). Recursive walks of JSON-like documents (`aiutils` JSONL and run-manifest writers, the op allowlist scan, `FileSystemResultStore`, `vmaf-tune`'s `jsonio`, the repository-security policy diff, the FFmpeg input-contract label resolver) now use an explicit work stack; `sys.exit` calls moved out of library code (`vmaf-dev-llm` commands raise `typer.Exit`, the NEO fetcher reports through `_fatal`, scripts return their status from `main()`); the `materialize_*` and `collect_gpu_calibration_data` scripts are split into helpers. One visible difference: `persist_to_file` raises `PersistCacheError` for an unreadable cache file instead of ending the process with status 1.


- Split the long functions of the `vmaf-mcp` server (`_call_tool_dispatch` is now a table of one handler per tool, `_run_compare`, `_run_ladder` and `_run_tune_per_shot` share `_run_vmaf_tune`, `_run_benchmark`, `_build_roi_argv`, `_execute_probe`, `_run_vmaf_score_encoded`, `_eval_model_on_split`, the HTTP `/v1/score` handler) and the depth guard (explicit stack instead of recursion), plus `scripts/lib/safe_subprocess.run_async`, the ADR-link and dependency-lock checkers, the perf regression gate, `vmaf-roi-score`'s saliency mask and two test fixtures, into helpers without a change in behaviour: same tool names, argument errors, argv, progress notifications, exit codes and messages. The HISS baseline loses those 20 rows.


- Brought 29 `ai/` modules to the HISS standard without a change in behaviour (ADR-1142): the long drivers of `ai/scripts` (corpus aggregation, feature extraction, calibration, the MOS-head and FR-regressor trainers, the `batch_materialize_*` runners and the three manifest CSV parsers, which now share `corpus.base.parse_mos_stats`), `ai/src` (`bisect_model_quality`, the three `vmaf-train` commands, `audit_learned_filter`, `export_to_onnx`, the parquet writers), `ai/sidecar` (`SGDEMATrainer.step`, the server loop and two tests) and `ai/train/qat.py` (`run_qat`, 147 lines) are split into helpers that keep their public names and signatures. `train_konvid_mos_head` trains its fold and ship models through one `_fit_model` with the same seed and RNG draw order. The HISS baseline loses 39 rows.


- Split the 5 `vmaf-tune` functions still over the HISS-04 limit (`build_encode_request`, `FrFromNrAdapter.run`, `_compute_saliency`, `run_proxy`, `_extract_middle_luma_frame`) and 7 test helpers and tests (`_make_runners`, the fast-path and prefilter CLI tests, the bisect-duration and rate-quality sweep fixtures) into helpers, with the same requests, argv, messages and errors. The HISS baseline loses those 12 infractions.


- **The integer motion SIMD kernels meet the lint and HISS standard.**
  `core/src/feature/x86/motion_avx2.c`, `core/src/feature/x86/motion_avx512.c`
  and `core/src/feature/arm64/motion_neon.c` have no clang-tidy finding in any
  lane (7, 18 and 2 before) and no function over 60 lines (ten before): each
  pipeline is now a row loop over small inlined stages. No score changes: the
  old and the new kernels return the same bits on 286 952 generated cases with
  GCC, clang and icx, and `motion`, `motion_v2` and the default model are
  identical at `--precision max` under scalar, AVX2, AVX-512 and NEON dispatch.
  The three files carry their `SPDX-License-Identifier` line
  ([ADR-1142](docs/adr/1142-whole-codebase-standards.md),
  [ADR-1250](docs/adr/1250-eupl-fork-relicense.md)).


- **`compat/python-vmaf/__init__.py` meets the HISS standard and carries its
  SPDX line.** `call_vmafexec()` (118 lines) and
  `call_vmafexec_multi_features()` (77 lines) are assembled from pure helpers
  that each emit one part of the command; the command text is unchanged and
  now pinned by two tests. HISS baseline: two rows fewer
  ([ADR-1142](docs/adr/1142-whole-codebase-standards.md),
  [ADR-1250](docs/adr/1250-eupl-fork-relicense.md)).


- Added missing SPDX-License-Identifier declarations across 387 clean source
  and header files in accordance with ADR-1250 and repository provenance,
  skipping 131 files with baselined debt, 5 vendored Pelorus mirror paths,
  and 2 files undergoing concurrent review. Extended `scripts/ci/check-copyright.sh` and the `check-copyright`
  pre-commit hook to enforce valid SPDX license identifiers on languages named
  in ADR-1250 (C, C++, CUDA, Go, Python), backed by positive, negative, and
  boundary unit test suite in `scripts/ci/tests/test_check_copyright.py`.


- **Seven SYCL runtime files conform to clang-tidy and HISS standards (batch B6).**
  The SYCL runtime source files and headers (`core/src/sycl/common.cpp`,
  `core/src/sycl/d3d11_import.cpp`, `core/src/sycl/dmabuf_import.cpp`,
  `core/src/sycl/common.h`, `core/src/sycl/dmabuf_import.h`,
  `core/src/sycl/picture_sycl.h`, `core/src/sycl/picture_sycl.cpp`) were brought to zero non-host-FP clang-tidy
  debt and full HISS compliance under [ADR-1142](docs/adr/1142-whole-codebase-standards.md).
  Eliminated 6 HISS infractions (3 gotos, 3 oversized functions). Numerical
  correctness is bit-identical on Intel Arc A380 at `--precision max` on the
  Netflix 576x324 reference pair, and the SYCL scratch memory audit passes (zero scratch memory used).


- `cambi_sycl`'s `launch_reset` kernel now uses an explicit 1D `nd_range` and a
  scalar select chain for its per-scale top-K rank initialization instead of an
  array captured by value and indexed at runtime in the kernel closure
  (ADR-1395). This eliminates 1280 B of private array scratch memory and the
  896 B `RoundedRangeKernel` wrapper on Intel GPUs under the Linux xe driver,
  leaving every kernel in `integer_cambi_sycl.cpp` completely scratch- and
  spill-free. Bit-identical parity against `--backend cpu` is preserved on all
  tested fixtures (48/48 frames on Netflix 576x324, 50/50 frames on BBB 4K, max
  abs diff 0.0), with 4K throughput measured at 17.05 ms/frame on Arc A380
  (down from 18.53 ms/frame).


- **Six more SYCL twins are held to the CPU's bits by the parity gate.**
  `adm_sycl`, `motion_sycl` (also with `debug=true`), `motion_v2_sycl`,
  `psnr_sycl`, `float_ssim_sycl` (with and without `enable_lcs`) and
  `cambi_sycl` return the CPU extractor's scores bit for bit: measured on an
  Arc A380 at `--precision max` on 333 frames from 576x324 to 3840x2160 at 8
  to 16 bits, full-range noise included. They are now listed as exact twins,
  so the gate compares them with tolerance 0 where it allowed 5e-5, and
  `test_sycl_exact_twins` asserts equality on a device. With the twins made
  exact earlier, 19 of the 21 gate features are exact on SYCL; `ciede` is
  within its 1.4e-11 bound and `speed_chroma` depends on the build's math
  library. With the default model the VMAF score of every measured frame
  equals `--backend cpu`
  ([ADR-1451](docs/adr/1451-sycl-exact-twins-declared.md),
  [SYCL backend](docs/backends/sycl/overview.md#exact-twins-declared-as-a-group-2026-10-02)).


- **The parity gate bounds `speed_chroma` between the CPU and the SYCL twin at
  `5e-6` instead of `5e-5`**, the bound the CUDA and HIP twins of the same
  device chain already have (ADR-1430, ADR-1452). On an Arc A380
  `speed_chroma_sycl` equals the CPU extractor of its own icx build and
  `speed_chroma_cuda` on 918 of 918 values; against a GCC build's CPU 15
  differ, by 1.9e-6 at most, because glibc's `log2f` is not correctly rounded
  and Intel's is. The cell is therefore a bound, not an exact declaration. No
  score changes.


- **The sycl clang-tidy lane is at zero findings (ADR-1142).**
  `core/src/sycl/common.cpp` and eight SYCL tests (`test_sycl.c`, `test_sycl_pic_preallocation.c`,
  `test_sycl_kernel_scratch.c`, `test_sycl_kernel_registration.c`,
  `test_sycl_shared_frame_sticky_geometry.c`, `test_sycl_vif_min_dim.c`,
  `test_integer_cambi_sycl.c`) report no finding, and `scripts/ci/tidy-baseline-sycl.json` is empty.
  The `VMAF_SYCL_PROFILE`, `VMAF_SYCL_TIMING`, `VMAF_SYCL_IMPORT_DEBUG` and `VMAF_SYCL_CHECKSUM`
  switches are now read once at first use through `vmaf_gpu_dispatch_env_get()`, like
  `VMAF_SYCL_DISPATCH` (documented in `docs/api/gpu.md`). The generated sRGB EOTF table
  `ssimulacra2_eotf_lut.h` (and its generator) carries a cited `NOLINT` block. No score changes.


- **Four SYCL twins take the CPU extractor's options and match it
  (ADR-1365).** `psnr_sycl` now accepts `enable_mse`, `enable_apsnr`,
  `reduced_hbd_peak` and `min_sse` and matches `--backend cpu` bit for bit,
  `apsnr_*` aggregates included; the PSNR option math now lives in
  `core/src/feature/psnr_score.h`, shared by the CPU extractor and the twin.
  `integer_ssim_sycl` accepts `enable_db` / `clip_db`, `float_ssim_sycl`
  `enable_lcs` / `enable_db` / `clip_db` (`float_ssim_l/c/s` within 8.3e-7 of
  the CPU), and `float_motion_sycl` `motion_max_val`. Before, a model or a
  `--backend sycl --feature psnr=enable_mse=true`-style request that set one of
  these options computed the feature on the CPU, and naming the twin with the
  option failed with `unknown option`. Both SSIM twins now score an
  identical window exactly 1, so with `enable_db` identical frames report the
  CPU's `+inf` or `clip_db` ceiling; their default linear scores moved by at
  most 1.1e-8. Measured on an Arc B580 and a UHD 770; see
  [the SYCL backend guide](docs/backends/sycl/overview.md#cpu-options-on-the-psnr-ssim-and-float-motion-twins-2026-09-29).


- The SYCL twin option regression cases (flat-frame `float_ssim` with `enable_db`, `psnr` `enable_apsnr` with `--subsample 2`, `motion_v2` `motion_fps_weight` / `motion_max_val`, one-frame `motion_v2`) are recorded as proven on an Arc A380: each fails when its defect is planted back into the SYCL twin and passes on master. No code change; the state ledger row narrows to the Metal remainder.


- **Regression test for model-collection growth failure.** When the array of a
  model collection cannot grow, `libvmaf` returns `-ENOMEM` and keeps the
  collection and its models intact; upstream Netflix/vmaf loses them
  (Netflix/vmaf PR #1590). The behaviour is unchanged, and
  `test_model_collection_growth` now holds it in place
  ([C API](docs/api/index.md)).


- **Regression test for reading frames with an odd width or height.** The raw
  and y4m readers consume a whole frame when a dimension is odd, where upstream
  Netflix/vmaf loses framing and crashes (Netflix/vmaf PR #1604). The behaviour
  is unchanged, and `test_video_input_odd_dims` now holds it in place for both
  reader entry points ([CLI](docs/usage/cli.md)).


- The tester image and Windows zip pull-request builds (`Tester Image`,
  `Windows Tester Zip`) start only when a pull request touches their own inputs
  again: their impact selectors are declared `own_paths_only` (ADR-1700), so a
  change to `scripts/ci/`, a required workflow or another CI-wide file no longer
  starts them. A dispatch still builds.
- The amd64 tester image is built and tested every night at 00:29 UTC on
  `master` (ADR-1701); nothing is published. A library change that breaks the
  image shows up as a failed scheduled run of `Publish Tester Image`. See
  `docs/development/release-workflow-verification.md`.


- `cert-err33-c` (an ignored return value of a standard-library call) is in the
  `WarningsAsErrors` list of `.clang-tidy`. Every clang-tidy lane already measured
  zero findings of it, so the promotion ADR-0694 asked for changes no count; a new
  unchecked `fclose()` or `fputs()` now fails `Tidy Changed`.


- `scripts/ci/check-tidy-coverage.py` (pre-commit hook `check-tidy-coverage`) fails when a tracked C, C++, CUDA, HIP, Objective-C++ or Metal translation unit is in no clang-tidy lane's measured sources and not in `.config/lint-exceptions.d/clang-tidy-coverage.toml`; the Metal kernels, the Pelorus mirror and the other files no tool can read are listed there with a reason and an expiry. `write-compile-commands.py` now exports the Objective-C and Objective-C++ rules, so the macOS lane reads the `.mm` files. Details: [tidy lanes](docs/development/tidy-lanes.md). The macOS lane's baseline (`scripts/ci/tidy-baseline-metal.json`) records 939 findings in the Metal host code; `Tidy Metal` takes a `fix` dispatch input that uploads clang-tidy's own fixes as a patch.


- **Every translation unit is read by a clang-tidy lane, or excepted by name.**
  The `cpu` lane now configures the embedded MCP server, a new `clang` lane
  builds the libFuzzer harnesses (which need clang), and a macOS `metal` lane
  reads the Objective-C++ Metal host code with Homebrew's clang-tidy 22 against
  the Xcode SDK. The check that fails when a tracked
  translation unit is in no lane and not excepted, and the exception entries
  for the units no lane can read, land in the follow-up pull request.
  `tidy-ratchet.py` gains `--select` (measure one part
  of the tree) and a scoped write now records the files it measures. The
  embedded MCP server and the fuzz harnesses end at zero findings. See
  [docs/development/tidy-lanes.md](docs/development/tidy-lanes.md).


- **The clang-tidy lanes are measured in the dev container.**
  `make tidy-lane LANE=<cpu|cuda|hip|sycl|arm64|all>` (`scripts/dev/tidy-lane.sh`)
  copies a checkout into a throwaway container of the dev image, configures
  the lane with its real toolchain and runs the ratchet;
  `make tidy-lane-write` rewrites the lane's baseline
  ([ADR-1471](docs/adr/1471-tidy-lanes-in-dev-container.md),
  [measuring the clang-tidy lanes](docs/development/tidy-lanes.md)). The
  `cpu` baseline, written on a workstation before, failed the required
  `Tidy Ratchet` check on the first complete hosted run of master (24 files
  below it); the container reproduces the hosted report byte for byte. The
  `cuda`, `hip` and `sycl` lanes build with nvcc, hipcc and icpx, so they
  parse the device bodies and, for the first time, the kernels:
  `scripts/ci/gen-gpu-compile-commands.py` had found no `.cu` / `.hip` rule
  since the kernel targets list their headers, and now stops the lane when
  it cannot read one. The `arm64` cross lane runs in the same container. The
  five baselines are re-measured; the findings in the kernels (473 in the
  CUDA lane, 314 in the HIP lane) are recorded, not yet fixed. A check or a
  scoped write under a clang-tidy other than the baseline's stops with exit 5.


- The `cpu` clang-tidy lane and the changed-files job measure the ten MATLAB MEX
  sources of `compat/python-vmaf/matlab/` against self-authored stub `mex.h` and
  `matrix.h` (`scripts/ci/lint-stubs/matlab/`); their lint exceptions are removed.
  The first measurement fixed the mechanical findings and a defect in `ical_std.c`
  (`mxDestroyArray()` was called on a matrix's data pointer).


- **The Metal host code is clean under clang-tidy.** The macOS `metal` lane measures 0 findings on its 27 translation units (it started at 1640 once `write-compile-commands.py` stopped dropping the `.mm` files). `core/src/metal/objc_handle.h` now holds the one `uintptr_t` slot to Metal object bridge and `vmaf_metal_library_load()` the one metallib loader, replacing 17 copies; file-scope helpers sit in anonymous namespaces; no score, public API or FFmpeg patch changes. See [tidy lanes](docs/development/tidy-lanes.md).


- The clang-tidy `cpu` lane baseline is empty (70 findings in 23 files to 0).
  C headers shared by C and C++ translation units carry cited `NOLINT` blocks
  (ADR-1138, ADR-1470), `cJSON.h` macros parenthesise their arguments and
  `isnumeric()` no longer reads a `string_view` as a C string. No score or API change.


- **The CUDA kernels, their headers and two CUDA tests meet the clang-tidy
  standard.** The 62 files of the `cuda` lane that were not shared C headers
  owned by the `cpu` lane are cleaned: device pointers are rebuilt from a
  kernel argument's `CUdeviceptr` with one macro (`cuda_device_ptr.cuh`),
  helpers live in anonymous namespaces, the long VIF statistic and the
  SpEED / CAMBI / ADM / SSIM / PSNR-HVS kernels are split into helpers, and
  the shared device headers carry the ADR-1138 brackets for the C includers.
  No score moves: the CUDA twins return the same bits on the Netflix pair, both
  1080p checkerboard pairs and a 10-bit pair, and every CUDA device test passes.


- The HIP kernels of integer VIF, float VIF, PSNR-HVS and SSIMULACRA 2, and the
  shared GPU headers `ordered_sum.h`, `adm_angle_flag.h`, `ff_math.h`,
  `ciede_ff_math.h`, `adm_cm_accumulator.h`, `integer_adm.h`,
  `float_adm_gpu_common.h` and `float_vif_gpu_common.h`, now pass the hip-lane
  clang-tidy profile with zero findings (ADR-1142). Scores are unchanged: the
  HIP parity tests of the touched twins compare them with the CPU extractor on
  gfx1036. `VifBufferHip.ref` and `.dis` are typed pointers instead of
  `uintptr_t`; the struct layout is the same.


- The HIP kernels of float and integer ADM, float moment, float and integer
  motion, float and integer PSNR, motion v2 and float SSIM now pass the hip-lane
  clang-tidy profile with zero findings (ADR-1142). Scores are unchanged: the
  HIP parity tests of the touched twins compare them with the CPU extractor on
  gfx1036.


- The HIP device sources of CAMBI, CIEDE, MS-SSIM and SpEED, the HIP CAMBI replay
  test, the float ADM math probe and the SYCL fp-arith contract test now pass the
  hip-lane clang-tidy profile with zero findings (ADR-1142). Scores are unchanged:
  the HIP parity tests of the touched twins compare them with the CPU extractor
  on gfx1036.


- torch is installed only by the two training packages, `ai/` and
  `tools/ensemble-training-kit/` (ADR-1886). The vmaf-tune predictor trainer
  moved to `vmaf_train.predictor_train` (`python -m vmaf_train.predictor_train`
  in the ai/ environment), and vmaf-tune's `train` extra is gone; vmaf-tune
  still loads the trained ONNX predictors. `describe_worst_frames` in the
  Python MCP server describes frames with a local vision-language model through
  ONNX Runtime GenAI: install `vmaf-mcp[vlm]` (now `onnxruntime-genai`, no
  torch or transformers) and point `VMAF_MCP_VLM_MODEL` at a model directory
  such as the CPU build of Phi-3.5-vision-instruct-onnx. The server no longer
  downloads models or runs model-hub code; without a model it returns frame
  metadata with a note that names what is missing.


- **The fixed-point VIF log2 table has one definition for every backend.**
  `vif_log2_table_generate()` moved to `core/src/feature/vif_log2_table.h`,
  which `integer_vif.h` includes. The SYCL and Metal hosts of the `vif` twins
  built the same 32768 values with copies of the expression and now call it,
  as the CPU extractor and the HIP host do. No score changes: `vif_sycl`
  stays bit-identical to the CPU on an Arc A380 (48 of 48 and 50 of 50
  frames) and `vif_hip` on a gfx1036; the Metal change is not built or run on
  this host. The table "Which HIP twins return the CPU's bits" on the
  [HIP backend page](docs/backends/hip/overview.md) is re-measured.


- **`vmaf-roi-score` installs on Python 3.13 and 3.14** (ADR-1528). Its
  metadata allowed only Python 3.10 to 3.12, although nothing in the tool or
  its optional runtime dependencies needs that limit. The package's tests now
  run in CI on the pinned Python 3.14 interpreter
  (`Python Package Tests (vmaf-roi-score)`).


- **`vmaf-tune`'s `auto`, `bisect`, `executor`, `per_shot`, `prefilter` and
  `score` modules meet the HISS-04 size limits and cite the right ADRs.** The 14
  functions over 60 lines (among them `auto.run_auto` at 327 lines and
  `bisect.bisect_target_vmaf` at 368) are split into helpers without a change in
  behaviour, and the HISS baseline loses those 14 infractions. Comments and
  docstrings now cite the current records (the conformal intervals are ADR-0393,
  Phase F `auto` is ADR-0397, the workdir is ADR-0598), the Pelorus records in
  `prefilter` are named as Pelorus's, and the `auto` docstrings count ten
  short-circuits. The JSON `notes` text of `prefilter --smoke` and the
  production result changed with the citations.


- **`vmaf-tune`'s corpus, compare, encode and ladder modules meet the HISS-04
  size limits.** The 13 functions over 60 lines (among them `iter_rows` at 428
  lines) are split into helpers without a change in behaviour: the same rows,
  argv, subprocess order, errors and messages, checked against master by the
  test suite and by differential runs of the old and new modules. The HISS
  baseline loses those 13 infractions.


- **`vmaf-tune` returns the lowest-bitrate encode that meets a target VMAF in
  every command (breaking).** `recommend` (corpus and live mode, with and
  without `--with-uncertainty`), the ladder's default sampler and the `fast`
  search used to return the smallest CRF that cleared the target, the highest
  bitrate among the passing rows, while `compare` and the bisect already
  returned the cheapest. They now share one rule (ties go to the higher VMAF,
  then the lower CRF) in Python and Go. `fast` also stops returning a CRF that
  misses the target when a cheaper one meets it. A corpus row without
  `bitrate_kbps` is now an error for these picks. Migration: results that read
  the old pick change; rerun `recommend` and the ladder, and pin a CRF
  explicitly where the higher-quality encode is wanted. See ADR-1562.


- **The macOS Metal leg is gated, and the SYCL spill probe no longer fills the build log.** Meson
  1.12 names `-lc++` twice on every link that carries an Objective-C++ object (224 ld64
  warnings per run); the Metal links now tell ld64 duplicate libraries are expected
  (`-Wl,-no_warn_duplicate_libraries`, reason in `core/src/metal/meson.build`), so the leg takes
  `werror: true`. The AOT compile of `scratch_check.cpp`, whose deliberate register-spill kernel
  makes the device compiler warn on every target, runs through `core/src/sycl/run_captured.py`,
  which prints that output only when the compile fails
  ([ADR-2170](docs/adr/2170-warnings-are-errors-per-leg.md)).


- **A compiler or linker warning now fails the CI leg that prints none today.** The gated legs of the
  build matrix (gcc, clang, Apple clang, icx / icpx, MinGW, CUDA and HIP builds), the ASan, UBSan and
  TSan builds, and the libvmaf builds of the Go, Rust and FFmpeg jobs pass `-Dwerror=true` and the
  linker's fatal-warnings switch through `scripts/ci/werror-args.sh`; with it `-Dwerror=true` also
  reaches the nvcc (`--Werror all-warnings`) and hipcc (`-Werror`) device compiles. Legs that are not
  at zero yet stay as they were and are listed with their cause in
  [the CI overview](docs/development/ci.md#warnings-are-errors-adr-2170). Release builds and container
  images do not use the switch. See [ADR-2170](docs/adr/2170-warnings-are-errors-per-leg.md).


- **icx and the clang-cl style drivers stop warning about our own compile flags.** `icx` and `icpx`
  reported `-ffp-contract=off` after `-fp-model=precise` as `-Woverriding-option` on every compile
  (7,300 times in one CI leg). The strict policy now spells `-fp-model=precise -fno-fast-math
  -fcomplex-arithmetic=full -ffp-contract=off`; on 182 translation units of this tree the objects
  are byte-identical to the old spelling ([ADR-2170](docs/adr/2170-warnings-are-errors-per-leg.md)).
  clang-cl and icx-cl are no longer offered `-pedantic`, `-fvisibility=hidden` and
  `-fvisibility-inlines-hidden`, which they ignored with a warning per compile.


- **Option tables, extractor tables and tag declarations no longer print compiler warnings.**
  The clang, gcc, icpx and Apple clang legs reported `-Wmissing-field-initializers`
  (`{NULL}` / `{0}` option terminators, positional test tables), `-Wreorder-init-list` and
  `-Wc99-designator` (the Metal option and extractor tables), `-Wmismatched-tags`
  (`VmafThreadLocaleState`, declared `struct` in the C header and `class` in the C++ file),
  `-Wimplicit-fallthrough`, `-Wkeyword-macro` (vendored cJSON redefining `true` / `false` in
  C23), `-Wtautological-constant-out-of-range-compare` (`vmaf_next_fex_capacity()` on 64-bit
  hosts) and `-Wmacro-redefined` (`DIV_ROUND_UP` in the HIP ADM twin). Every fix is
  value-preserving: initialisers are reordered or completed, `[[fallthrough]]` replaces
  comments, the capacity check compares in `size_t`. No score, option default or exported
  symbol changes.


- **Unused code, ignored attributes and deprecated calls no longer print compiler warnings.**
  Test tables and helpers that only a skipped or disabled configuration reaches are compiled only
  there (`-Wunused-function` / `-Wunused-variable` in the ms_ssim_decimate, cambi, ssimulacra2 and
  read_pictures tests, the registry helpers of `model_loader.c` on Windows, the HIP error mappers
  without `hipcc`); `VMAF_EXPORT` is empty for GCC on MinGW, where the visibility attribute was
  ignored and drew a warning under LTO; the Win32 pthread shim spells `__stdcall` only in the host
  pass of a SYCL build; `VmafRef` is constructed with count 1 instead of calling the deprecated
  `std::atomic_init()`; the ONNX Runtime headers are `-isystem`; and a test that linked with an
  explicit `link_language : 'cpp'` no longer repeats `-lc++` on macOS. No score, symbol or
  option changes.


- **zimg picture conversion allows approximate gamma (port of Netflix/vmaf `5c3f4fb90`).**
  `vmaf_picture_convert()` builds its zimg graph with `allow_approximate_gamma`, as FFmpeg's
  `zscale` does: exact transfer functions are about 20 times slower for PQ and change scores
  negligibly. Only builds with `-Denable_zimg=true` are affected. See
  [Pictures](docs/api/pictures.md#converting-pictures-vmaf_picture_convert).


- Every published image, the dev container included, now stores its layers as zstd
  at BuildKit's strongest level (ADR-1594): 5 to 25 % smaller downloads (the CPU
  tester image 268 to 200 MB). Pulling them needs Docker Engine 23.0 or later,
  Docker Desktop 4.19 or later, containerd 1.5 or later, or Podman; an older Docker
  stops with `failed to register layer: ... archive/tar: invalid tar header`
  (`docs/usage/docker.md`, "What can pull the images"). Images published up to
  `v1.0.0-rc.2` keep gzip layers.
- The Windows tester zips are encoded by zopfli: still Deflate, which every Windows
  tool opens, and 3.7 to 4.2 % smaller than zlib's strongest level (the CUDA zip
  344 to 329 MB).


### Fixed

- **An aarch64 clang build and an aarch64 GCC build return the same scores
  ([ADR-1461](docs/adr/1461-strict-fp-every-translation-unit.md)).** clang
  fuses `a * b + c` into one multiply-add by default and GCC does so for C++;
  on aarch64, where the instruction is baseline, the feature library, the SVM
  and the model code were built that way, each compiler in its own places.
  The two builds differed on 680 of 3355 measured values: `speed_chroma` by up
  to 6.2e-5, `float_vif` scales by up to 3.5e-5, `speed_temporal` by 2.3e-2 on
  a checkerboard. Every C and C++ file is now built without contraction (a
  project-wide compiler argument), and the builds agree on 3350 of the 3355;
  the five left are `ciede2000` values 1.1e-12 apart that differ between the
  compilers on x86-64 too. x86-64 builds are unchanged. Scores from an
  existing aarch64 clang build (macOS, Linux on ARM) differ from a new one by
  the amounts above. `make test-netflix-golden-arm64` runs the Netflix golden
  gate against an aarch64 GCC or clang cross build under qemu-user on an x86
  host.


- **The ADM twins' decouple header is tested more, the Metal scale-0 angle flag sums in 64 bits, and a CUDA/HIP corner is found (`T-GPU-ADM-ANGLE-FLAG-S0-INT32-CORNER-2026-10-06`).**
  `test_adm_decouple_recip_{cuda,hip}` (the twins' own header compiled for the host) now also holds `decouple_r_s123()`, `get_best15_from32()`
  and both angle flags to the CPU's over 800 000 random draws and the corners of the int16 range, and each executable has its own
  `run_tests` root. That clears CodeQL `cpp/unused-static-function` alerts 1464-1479 (the header's functions are all used by a host
  build now) and exposes a real corner: with every band at -32768 the CUDA and HIP angle flag adds in int32 and wraps where the CPU's
  int64 sum does not (17 of 256 corner combinations; fixed in the same stack, ADR-2134). Metal's `iadm_angle_flag_s0()` had the same sums and is fixed. The two `cpp/include-non-header` findings of the
  device-source tests (1463, 1488) are declared exceptions (`codeql-include-non-header.toml`).


- **Integer ADM no longer scores isolated impairments above 1.** The scale-0
  contrast-masking threshold narrowed its centre tap to 16 bits, as upstream
  Netflix/vmaf does. A coefficient of 15360 or more wrapped the tap negative,
  and where such a coefficient stood alone the threshold added contrast instead
  of masking it: a flat grey 64x64 reference against the same picture with
  isolated 4x2 patches scored `integer_adm_scale0` 1.0829 and `integer_adm2`
  1.0355 where `float_adm` gives exactly 1. The tap is now 32 bits wide and the
  excess over the threshold is clamped in 64 bits, in the scalar, AVX2, AVX-512,
  CUDA, HIP, SYCL and Metal code (the second revision of Netflix/vmaf PR #1602,
  which upstream has not merged). Both patch pictures now score 1. The Netflix
  golden gate is unchanged (271 passed, 12 skipped before and after) and its 13
  fixture pairs give identical output at `--precision max` on the CPU and on
  the CUDA, HIP and SYCL twins. Scores change only where a scale-0 coefficient
  reaches 15360: independent full-range noise at 576x324 moves `integer_adm2`
  from 0.389548 to 0.389503 and `integer_adm_scale0` from 0.454972 to 0.454801.
  Until upstream merges #1602, integer ADM differs from upstream master on such
  content. The scalar tails of the AVX2 and AVX-512 kernels also no longer
  left-shift a negative threshold, which a UBSan build stopped on for a 24x24
  picture. The Metal change is source only; no Apple device was available
  (ADR-1402, `T-ADM-CM-CENTRE-TAP-WRAP-ABOVE-ONE-2026-10-01`,
  `T-ADM-CM-X86-TAIL-NEGATIVE-THRESHOLD-SHIFT-2026-10-01`;
  [features](docs/metrics/features.md)).


- **Integer ADM no longer shifts a negative masking threshold on the scalar
  path.** The scale-0 contrast-masking step computed
  `abs(x) - (threshold << shift)` in signed arithmetic, and the threshold is
  negative when one large coefficient stands among small ones. That shift is
  undefined in C: a sanitizer build of `vmaf --feature adm --cpumask 4294967295`
  stopped on full-range noise with `left shift of negative value`. The scalar
  path, which every aarch64 run uses, now computes the expression modulo 2^32,
  as the AVX2 and AVX-512 vector code already did. Scores are unchanged on
  every dispatch level ([features](docs/metrics/features.md)).


- **The CUDA and HIP scale-0 ADM angle flag equals the CPU's at every int16 corner (`T-GPU-ADM-ANGLE-FLAG-S0-INT32-CORNER-2026-10-06`, ADR-2134).**
  `decouple_angle_flag_s0()` summed int16 products in int32, which wraps when every band is -32768 (17 of 256 corner combinations gave another
  flag than the CPU's, and with it another gain-limited decouple branch). It now sums in unsigned 32 bits restored to int64, as the CPU's int64 sums.
  The form costs `adm_cm_aim_line_kernel_4` 209 registers (ADR-1226's budget was 208; plain int64 is 216), so that kernel has its own budget in
  `test_cuda_adm_cm_register_pressure`; the other kernels keep 208 and spill is zero. An RC7 row wins the register back.


- **`test_adm_decouple_recip_cuda` / `_hip` build with MSVC and finish on
  MinGW.** The host build of the twin's header named `__builtin_clz`, which
  cl.exe does not have, without the `compat_builtin.h` shim (C3861 on every
  Windows MSVC leg), and its CPU helpers refilled the 65 537-entry
  `div_lookup` table for every sample on Windows, past the 120 s timeout on
  the UCRT64 leg. `check-msvc-clz-shim.sh` now requires the shim in every host
  file that names `__builtin_clz`.


- **Integer ADM gives one result for a non-integer `adm_enhn_gain_limit`,
  whichever code path computes it.** The scalar code bounds a restored sample
  with the double product of the sample and the limit, truncated toward zero.
  The AVX2 and AVX-512 decouple kernels rounded that product to nearest, as
  upstream Netflix/vmaf does, and the SYCL twin formed it in Q31 fixed point,
  so with a limit such as 1.2 each was one off in a share of the samples:
  `integer_adm_scale0` differed from the scalar path by up to 1.2e-6 per frame
  on the Netflix 576x324 pair, 6.2e-6 on a 352x288 pair and 3.6e-5 on blurred
  blocks. The vector kernels now truncate, and the SYCL twin forms the
  truncated double product exactly from 64-bit integer arithmetic. The scalar
  path, AVX2, AVX-512 and `adm_sycl` (Arc A380) give identical output at
  `--precision max` for limits of 1, 1.2, 1.5 and 100 on 20 test pairs and on
  Big Buck Bunny at 3840x2160. Nothing changes at the limits the shipped models
  use (1 and 100), and the Netflix golden gate is unchanged (271 passed, 12
  skipped before and after). `adm_cuda` and `adm_hip` already truncated. The
  Metal twin multiplies in single precision and was not changed, because no
  Apple device was available (ADR-1413,
  `T-ADM-DECOUPLE-X86-FRACTIONAL-GAIN-ROUNDING-2026-10-01`,
  `T-SYCL-ADM-FRACTIONAL-GAIN-LIMIT-2026-09-29`,
  `T-METAL-ADM-GAIN-LIMIT-FLOAT32-2026-10-01`;
  [features](docs/metrics/features.md)).


- **`--backend hip` runs the default model's ADM on the GPU, with the CPU's
  values.** `adm_hip` had no AIM contrast-measure pass, so it could not emit
  `aim` and `adm3`, the ADM features the default model `vmaf_v1.0.16_3d0h`
  reads, and it carried no HIP flag, so `--backend hip` never selected it and
  the model's ADM ran on the CPU. The twin now computes AIM on the device
  with the CUDA twin's kernels, takes every rounding shift and its per-scale
  conclusion from the CPU extractor's own routines, accepts `adm_skip_aim`,
  and is selected by `--backend hip` and by `--feature adm`. Every output,
  `aim` and `adm3` included, is bit-identical to `--backend cpu` (4141 of
  4141 values on a gfx1036), and the default model's VMAF under
  `--backend hip` equals the CPU's on every frame measured. On the
  integrated gfx1036 the twin is slower than the 16-thread CPU extractor
  (218 against 14.5 ms per 3840x2160 frame); tuning is tracked for the
  benchmark candidate
  ([ADR-1525](docs/adr/1525-adm-hip-aim-device-pass.md)).


- **`adm_cuda` and `adm_hip` take the CPU's integer reciprocal in the scale-0
  decouple.** The twins computed `2^30 / o` as an fp32 quotient truncated to an
  integer; the CPU reads `div_lookup`, the integer quotient. They differ for 343
  of the 32767 positive operands, and for a reference coefficient above 16566
  the restored sample differs too (318294 pairs at the mismatching operands).
  Real video does not reach it: the Netflix pair, both 1080p checkerboards and
  BBB 3840x2160 were identical before and after (0 difference at
  `--precision max`). A frame with isolated sign-aligned patches of full-scale
  detail did: `integer_adm_scale0` differed by 1.4e-6 on the RTX 4090 and the
  gfx1036. `adm_recip_q30()` (`adm_decouple_inline.cuh` and `.hip`) now returns the
  CPU's `div_lookup` value for every `int16` operand, from an fp32 estimate
  and an exact fp32 correction; an integer division was measured and refused
  (it takes `adm_cm_line_kernel_8` from 148 to 228 registers). Frame time at
  3840x2160 does not change measurably (CUDA 4.96 to 5.12 ms, HIP 263 to 261 ms
  on a loaded host).


- **Integer ADM reports named refusal for invalid viewing geometries.** When
  `adm_norm_view_dist * adm_ref_display_height < 3240`, integer ADM (`adm` on CPU
  and GPU twins `adm_cuda`, `adm_hip`, `adm_sycl`, `adm_metal`) now logs an
  explanatory message naming the extractor, the option values, their product, the
  3240 floor (`1080p at 3H`), and `float_adm` as the accepting alternative,
  instead of failing with an unexplained `-EINVAL`.


- The site's sidebar lists only the ADR index, the template and the tag index,
  as ADR-1510 decided: #1951 landed without its `mkdocs.yml` change, so every
  ADR and tag page was still in the navigation of every page.


The master push `Required Checks Aggregator` reads only its own branch's runs: runs of release-please's release-notes branch or a verification branch on the same commit no longer make it wait or fail.


- **The master `Required Checks Aggregator` judges the push, not the pull
  request on the same commit.** A pull request the merge train lands by
  fast-forward shares its head commit with the master push, and its cancelled
  runs of the pull-request-only gates counted as failures of every master push.
  Outside a pull request the aggregator now leaves out check runs of
  pull-request workflow runs (`docs/development/ci.md`).


- **The `vmaf-train` (`ai/`) and `vmaf-dev-llm` wheels build again.** Their
  `force-include` repeated directories the packages already ship, which
  hatchling 1.32 refuses ("A second file is being added to the wheel
  archive"). The wheels still carry the dataset manifests, the training
  configurations and the prompt templates.


- **No `ai/scripts` file announces itself as "not yet implemented".** Ten files
  printed that text and exited 1. `gen_dists_sq_placeholder_onnx.py` and
  `gen_mobilesal_placeholder_onnx.py` are implemented: they rebuild the shipped
  `model/tiny/dists_sq.onnx` and `model/tiny/mobilesal.onnx` byte for byte, and
  `--check` fails when a file differs. The other eight stubs (a LOSO evaluator,
  the pVMAF benchmark, the LSVQ fetcher, two corpus converters, two trainers and
  a stub named after the real `scripts/gen_ssimulacra2_eotf_lut.py`) are removed.
  The two model cards describe what the generators write: the ONNX file only.


- The four FR regressor trainers (`train_fr_regressor.py`, `_v2.py`,
  `_v2_ensemble.py`, `_v3.py`) write `model/tiny/registry.json` through
  `vmaf_train.registry.write_registry_json()`. A non-finite number in a registry
  row is written as `null` instead of the non-standard `NaN` token no strict JSON
  reader accepts; a registry of finite values is byte-identical to before.


- **`libvmaf.h` and the API guide say what an index gap and an early query do.**
  `vmaf_read_pictures()` has always rejected a repeated or earlier index with
  `-EINVAL`; it also accepts an index that skips values, and then the motion
  extractors write no `motion2` / `motion3` for the pictures that follow, so
  reading them returns `-EAGAIN` even after the flush. A score asked for
  before the flush returns the value or `-EAGAIN`, never a partial value. The
  Doxygen of `vmaf_read_pictures()`, `vmaf_score_at_index()`,
  `vmaf_feature_score_at_index()` and the two pooled calls now carry both
  rules, and [the API guide](docs/api/index.md#scoring-before-the-flush-and-index-gaps)
  has a section on them (ADR-1429, Netflix/vmaf#910, #755, #1180). No
  behaviour changes.


- **`apsnr_*` is right on long, heavily distorted clips.** The `psnr`
  extractor and its CUDA, HIP, SYCL and Metal twins summed each plane's
  squared error over the clip in an unsigned 64-bit integer. At 16 bits with
  every sample at the maximum difference that sum wrapped at frame 2072 of
  1080p, 122 of 8K and 33 of 16K (at 12 bits at 530,502, 31,085 and 8,290),
  and `apsnr_*` came out several dB too high. The sum is 128 bits wide now;
  clips that never reached 2^64 keep their values.


- **The aarch64 CPU `float_moment` returns the scalar function's bits on
  every input and SVE vector length.** The NEON kernel added into lane
  accumulators and the SVE2 kernel added per-row vector sums, grouped by the
  vector length. On a 16-bit frame whose sum of squares passes 2^53 units
  (more than 2 097 152 pixels) the scalar `double` rounds on every add, so
  their second moments (`float_moment_ref2nd`, `float_moment_dis2nd`) could
  differ from the x86 and scalar results in the last digits (by 2.1e-6 on a
  second moment of 37918 for a 16-bit 3841x2160 test frame, under
  `qemu-aarch64`). Both kernels now add
  each value into one `double` in raster order, as the x86 kernels do.
  `test_moment_simd` asserts `==` for AVX2, AVX-512, NEON and SVE2 at 128,
  256, 512 and 2048 bits; its SVE2 case, and the NEON cases of
  `test_iqa_convolve`, had been skipped on every processor because they read
  the CPU flags before `vmaf_init_cpu()`. 8-, 10- and 12-bit scores and x86
  builds do not change. The kernels now run at about the scalar loop's speed
  ([ADR-1500](docs/adr/1500-arm-float-moment-scalar-order.md),
  [Arm backend](docs/backends/arm/overview.md#bit-exactness)).


- **`--threads` no longer breaks GPU twins that are selected by name.**
  `vmaf --backend hip --feature adm_hip --threads N` (and `float_vif_hip`)
  exited with `problem flushing context` for every `N` and wrote no score:
  a twin without a backend flag was handed to the CPU worker pool, whose
  workers call `extract()`, which a `submit()` / `collect()` extractor does
  not have. Such an extractor now runs on the thread that calls
  `vmaf_read_pictures()` whatever its flags, and threaded and unthreaded runs
  give the same scores bit for bit.


- **`testdata/benchmark_netflix.py` compares the src01 pair with the CLI golden.** The CPU row
  read `DIFF` against 76.66890519623612 (the Python harness's value); the reference is now
  76.66783025, the `vmafexec_test.py` assertion
  ([baselines](docs/development/netflix-benchmark-baselines.md)). FFmpeg patch impact: none.


- **`VmafVifNameSet` has the same size in C and in C++.** The internal header
  `core/src/feature/nonfinite_score.h` gave the enum a one-byte underlying
  type for C++ and left it `int`-sized in C. No value crossed between the two
  languages, so no score or output was affected; the enum has one definition
  now, and a device-free test rejects a C++-only narrow underlying type in
  any header a C source includes
  ([ADR-1470](docs/adr/1470-c-cxx-shared-enum-one-definition.md)).


- The C4 context and container pages list the relations between VMAFx, its
  containers and the external systems as tables. The Mermaid diagrams that
  #1957 put there do not render: the site enables no Mermaid since the
  diagrams moved to checked figures (ADR-1508).


- **`cambi` with `full_ref=true` scores the right distorted picture at 10 bits
  when the source is larger than the picture.** With `src_width` /
  `src_height` above the input size, the CPU extractor converted the 10-bit
  distorted plane with one copy at the input's row stride into a working
  picture allocated at the source width, so every row after the first was
  shifted and `cambi` and `cambi_full_reference` were wrong (10-bit Sparks
  frame 0: `cambi` 0.0048 instead of 0.3734). The plane is now copied row by
  row, and `cambi` no longer depends on `full_ref` or the source size. 8-, 9-,
  12- and 16-bit input was not affected; the Metal twin, which runs the same
  conversion on the host, is fixed with it. Upstream Netflix/vmaf has the
  same code.


- **`cambi` no longer reads and writes outside its buffers on wide, short
  frames, and scores tall, narrow frames the same on every path.** When the
  coarsest of CAMBI's five scales had no more rows than half the window,
  rounded down (`pad_size`; with the default window every height up to 176 at
  1920 wide, 240 at 2560 wide and 352 at 3840 wide), the c-values pass ran past
  both ends of the frame: AddressSanitizer reported heap-buffer-overflows and
  release builds could crash. The scalar, AVX2, AVX-512 and NEON paths now
  clip the window to the rows that exist, as the upstream fix does
  ([Netflix/vmaf#1628](https://github.com/Netflix/vmaf/issues/1628),
  [Netflix/vmaf#1629](https://github.com/Netflix/vmaf/pull/1629)). Frames with
  fewer than `pad_size` rows at that scale (up to 160, 224 and 336 rows at
  those widths) score differently where they completed before; for example a
  3840x128 horizontal ramp moves from 19.544347 to 19.512269 on the C path.
  Going beyond upstream Netflix/vmaf#1629, which bounds only the rows, the
  columns are bounded too (`MIN(pad_size, width)` in the four left-edge loops):
  scores change for frames narrower than `pad_size` at some scale (measured:
  64x1920 vertical ramp master 14.975700714938673 vs branch 14.964394451743877
  on the C path; the SIMD paths already agreed). On frames with fewer than
  `pad_size` columns at that scale (up to 80 wide at 1080 high, 160 at 1920
  high) the scalar walk read columns past the frame. Those frames now score on
  the C path (`--cpumask 63`, builds without SIMD) and on the CUDA, HIP and
  Metal twins, which ran that walk on the host (the CUDA and HIP twins have
  since moved it to the device), what the default dispatch
  already gave (64x1920 vertical ramp moves from 14.975700714938673 to
  14.964394451743877; another vertical ramp variant moves from 16.141046 to
  16.131541). All other frame sizes score as before
  ([CAMBI frame sizes](docs/metrics/cambi.md#frame-sizes)).


- **The CI fixture cache no longer replaces tracked test fixtures with an
  older revision.** The `python/test/resource` cache of the Builds, Build and
  Tests workflows also held the files git tracks there; a `restore-keys` hit
  wrote the revision of the run that saved the cache over the checkout, so a
  dataset fixture changed by a later commit came back without its new fields
  and every Ubuntu tox leg failed (`KeyError: 'dis_enc_width'`). The step that
  prunes unusable restored fixtures now takes `--restore-tracked` and puts
  every tracked file back to the checked-out revision.


- **`test_cuda_parity_gate_default_run` no longer times out on a build
  without CUDA while another job holds the CUDA device lock.** The test takes
  the per-device lock before it runs the gate, and it learned that the binary
  has no CUDA only from the gate's output afterwards. On a HIP-only or
  SYCL-only build directory it therefore waited for a device it cannot use and
  hit its 120 s timeout whenever the lock was busy. It now reads Meson's
  option record of the build directory first and skips at once when
  `enable_cuda` is off.


- **The CUDA parity-gate default run skips on a build without CUDA.**
  `test_cuda_parity_gate_default_run` is registered for every build, and on a
  libvmaf built without CUDA the `vmaf` CLI refuses `--backend cuda`, which
  the test reported as a failed parity gate: `--suite=gpu` on a HIP-only or
  SYCL-only build had one failing test. The refusal is now a skip, like a
  missing device, and `test_cuda_parity_gate_skip` pins the decision to the
  CLI's message without a device.


- **Cross-backend parity gate compares emitted default motion metrics.**
  `scripts/ci/cross_backend_parity_gate.py` and `cross_backend_vif_diff.py`
  read `integer_motion`, which the CLI only emits in debug mode, causing a
  `KeyError` in the motion cell during default runs. Updated the motion metric
  keys to `integer_motion2` and `integer_motion3` so the full default parity matrix
  completes without error across backends. Also updated
  `scripts/ci/test_cross_backend_feature_names.py` to test active backends instead
  of the removed Vulkan backend.


- **`motion_sycl` defaults `debug` to `false`, and the parity gate reports a
  missing metric as a cell error.** `motion_sycl` declared `debug` with default
  `true`, so a default run emitted `integer_motion` while the CPU, CUDA and HIP
  extractors did not; it now follows them (pass `debug=true` to get the score).
  `scripts/ci/cross_backend_parity_gate.py` and
  `scripts/ci/cross_backend_vif_diff.py` gain a `motion_debug` cell
  (`motion` with `debug=true`, comparing `integer_motion`, `integer_motion2`
  and `integer_motion3`). A metric that one backend does not emit now makes
  its cell `ERROR` and names the backend, instead of ending the whole matrix
  with `KeyError`. See
  [ADR-1418](docs/adr/1418-motion-parity-gate-metric-alignment.md)
  (`T-CI-PARITY-GATE-MOTION-DEBUG-DEFAULT-2026-09-29`).


- **`vmaf` CLI now accepts odd dimensions for raw YUV 4:2:0 and 4:2:2 inputs.**
  `validate_chroma_alignment()` (`core/tools/vmaf.cpp`, ADR-0461) previously
  refused odd widths for 4:2:0 and 4:2:2 inputs and odd heights for 4:2:0
  inputs. However, `.y4m` inputs with odd dimensions were already accepted and
  processed with ceiling chroma extent (`vmaf_chroma_extent()`,
  `core/src/picture_geometry.h`, PR #1643). Per user decision 2026-10-01
  ("Accept both (Recommended)"), the CLI accepts odd dimensions for raw YUV
  inputs with ceiling chroma, bit-identically matching `.y4m` scores for identical
  content (ADR-1398). Files with mismatched frame byte sizes continue to exit 2
  cleanly via `yuv_check_file_size()`.


The `vmaf` command-line tool now exits with the same status on every platform: a libvmaf error code modulo 256 (`-EINVAL` is 234). On Windows the raw negative 32-bit code used to leak out and a POSIX shell read it as something else.


- **`test_cli_exit_status` passes on Windows and macOS.** It expected Linux's
  `ENOSYS` (38) in one case; the expected status is now computed from the
  platform's value (`256 - ENOSYS`), so the Windows legs no longer fail on a
  correct exit status.


- **`--feature <name>` now runs on the GPU that `--backend` names.**
  `vmaf --backend sycl --feature ciede` used to initialise the SYCL device and
  compute `ciede2000` with the CPU extractor, one frame at a time, while the
  JSON reported `"backend_used": "sycl"`. With an explicit `--backend cuda`,
  `sycl`, `hip` or `metal`, a CPU extractor name now runs on that backend's
  twin (`ciede_sycl` here), chosen the way a model's features are
  ([ADR-1359](docs/adr/1359-cli-feature-backend-twin.md)). When the backend has
  no twin, or the twin cannot honour an option or the input size, the CPU
  extractor runs and one `vmaf: warning: --feature <name>: ...` line says why.
  Twin names such as `--feature ciede_sycl`, `--backend cpu`, `--backend auto`
  and runs without `--backend` behave as before.
- **`backend_used` reports the backend that computed the features.** It used to
  name the backend that was initialised, even when nothing ran on it. It now
  names the device when at least one extractor ran there and `cpu` otherwise;
  the new `feature_backends` array says where each extractor ran.


- **The `vmaf` CLI's read-ahead checks its invariants with assertions again.**
  A lint cleanup had replaced the seven `assert()`s of the frame reader by
  early returns, so a broken invariant would have dropped a frame or ended a
  stream without a message instead of stopping a debug build. No release
  carried the change. The clang-tidy finding that prompted it is a false
  positive of clang-tidy 22 on glibc 2.44 hosts; the CI image does not report
  it.


- **`vmaf` no longer leaks its option dictionaries when a run stops early.**
  The dictionaries built from `--feature name=opt=val` and from a `--model`
  feature overload (`--model version=...:vif.vif_enhn_gain_limit=1.0`) were
  released only by the libvmaf calls that take them. A run that stopped
  before those calls (an input that cannot be opened, an odd height with
  4:2:0, a model or feature that does not fit the frame, a feature after a
  failing one, an unknown extractor name, a feature pinned to a backend the
  run did not start) exited with them allocated, and LeakSanitizer builds
  failed with a 158 to 329 byte leak. `cli_free()` now
  releases every dictionary the settings still own, and each hand-off to
  libvmaf clears the settings' copy first. Exit codes and output are
  unchanged.


- `vmaf` prints `problem scoring picture N: libvmaf returned E` when libvmaf
  fails to score a frame it has read (a feature extractor refused the frame or
  its options), where it printed `problem reading pictures`, which read like
  the input read failure `problem while reading pictures` (exit 102). The exit
  status is unchanged. `docs/usage/cli.md` lists the case in the exit-code
  table.


- **CodeQL include-non-header alert #1309 resolved with internal test accessors and CI guard.**
  `core/test/test_feature_backend_twin.c` linked directly against `libvmaf` instead
  of unity-including `core/src/libvmaf.c`. Narrow internal accessors
  (`vmaf_backend_twin_verdict_for_test`, `vmaf_context_fake_backend_for_test`,
  `vmaf_context_set_gpumask_for_test`, `vmaf_context_append_registered_feature_extractor_for_test`,
  and `vmaf_context_resolve_context_fallbacks_for_test`) are declared in
  `core/src/libvmaf_priv.h` with static definitions in `core/src/libvmaf.c`. A new
  `scripts/ci/check-no-non-header-includes.sh` check runs in pre-commit and CI to
  prevent non-header source file inclusions under `core/test/`.
- **Scorecard SAST alert #6 resolved by running CodeQL Actions universally on every PR.**
  Scorecard's `sastToolInCheckRuns` evaluates PR head commits across the last 30 commits
  on master. Under [ADR-1389](docs/adr/1389-codeql-actions-universal-pr-sast.md),
  `CodeQL (Actions)` now runs unconditionally on all pull requests and pushes,
  providing 100% commit SAST coverage across docs-only and non-code PRs with
  negligible (~15–20s) overhead, and is enforced in the required checks aggregator.


- **The SBOM jobs unpack the wheel they just built instead of installing it unhashed, and the trainer's seeding no longer swallows an `ImportError`.**
  `release-dry-run.yml` and `supply-chain.yml` take the built `vmaf_mcp` wheel into the SBOM root with
  `python -m zipfile -e`; its dependencies stay installed under `--require-hashes`, and a build
  has no published hash for the wheel itself (OpenSSF Scorecard Pinned-Dependencies, alert 1462).
  `_set_seed()` in `ai/src/vmaf_train/predictor_train.py` asks `importlib.util.find_spec()` whether
  numpy and torch exist instead of catching `ImportError` around the import (CodeQL
  `py/empty-except`, alerts 1489 and 1490), so a broken installed package now raises.


- **`pkg/codecadapter` defines each codec once.** Eight codecs (libx264, libx265,
  libaom-av1, libvvenc, libsvtav1, libvpx-vp9, prores_videotoolbox,
  av1_videotoolbox) were written twice, a constructor nothing called and the
  literal in the registry list. The lists call the constructors; no argv, range
  or probe value changes. `TestEveryCodecIsDefinedOnce` keeps it that way.


- **CodeQL findings in the Metal math headers, two upstream headers and the Pelorus conformance test are fixed in code (alerts 1406-1408, 1459-1460, 1480-1481).**
  `metal_float_adm_math.h` and `metal_integer_vif_gain.h` compare doubles through `vmaf_mtl_f64_equal()` (`==` semantics, spelled without `==`; `cpp/equality-on-floats`);
  `vmaf_mtl_fm_blur()` takes its window by pointer instead of copying 100 bytes (`cpp/large-parameter`);
  `core/src/feature/ssim.h` and `ms_ssim.h` got include guards (`cpp/missing-header-guard`);
  `test_pelorus_interop` gives the two static helpers of the vendored x265 CSV parser a private name per copy through compiler arguments, so CodeQL stops reporting them unreachable (`cpp/unused-static-function`; the vendored source is untouched).
  No score changes.


- **A model registered with `vmaf_use_features_from_model()` can be destroyed
  at once.** The context's feature collector now owns a reference to every
  model it mounts (ADR-1755), so a model destroyed after registration, or
  before a `vmaf_close()` that fails and is retried, is no longer read after it
  is freed (a heap-use-after-free found with a metadata handler registered). The
  change is additive: code that keeps the model alive until `vmaf_close()`
  returns 0 behaves as before. The Rust crates' `Drop` of a context whose close
  failed twice now leaks the context and prints one line to stderr instead of
  calling `process::abort()`. Documented in `docs/api/lifecycle.md`,
  `docs/api/models-and-features.md` and `docs/api/rust-context-close.md`.


- The help text of `vmafx-tune predict --with-uncertainty` and
  `recommend --with-uncertainty`, and the docs, comments and test docstrings
  around the conformal intervals, cite ADR-0393 (the probabilistic head and
  conformal scaffold) instead of ADR-0279 (the libaom codec adapter), which an
  earlier renumbering left behind.


- **The controller evicts silent nodes after startup and returns their
  running jobs to the queue.** The node registry's reaper stopped about 15 s
  after startup, because it was tied to the fx start context, which fx lets
  expire after its start timeout; dead nodes stayed registered for ever. And
  an evicted node's running jobs stayed `RUNNING` for ever, although
  `controller.proto` promised they would be re-queued. The reaper now runs
  until shutdown, and an eviction returns the node's running jobs to
  `PENDING` ahead of newer work. See [the controller guide](docs/server/controller.md#node-api).


- **`make coverage-check` works.** `make coverage` now builds, tests and reports
  the way the `Coverage Gate` job does (gcovr, atomic counters, serial suite) and
  writes `build-coverage/coverage.json`; `coverage-check` hands that file and the
  local floors (37 % overall, 85 % critical) to `scripts/ci/coverage-check.sh`.
  The target used to pass an lcov `.info` file to a script that reads gcovr JSON,
  so it could never pass. The script refuses a non-gcovr input with exit 2.
  `make coverage` needs `gcovr` instead of `lcov`.


- **The `Cppcheck` check passes again.** The first complete hosted run on
  master since 2026-09-30 reported three findings (cppcheck 2.19.0,
  `--check-level=exhaustive`): `identicalInnerCondition` in
  `core/src/dict.cpp` (`if (*dict) return *dict;`) and
  `returnDanglingLifetime` twice in
  `core/test/test_video_input_odd_dims.c`, where a frame reader called
  through a function pointer was taken for an aggregate that keeps the
  address of a local. A later cleanup of `core/src/feature/adm.c` added
  eleven `invalidPointerCast` reports: the band planes were carved from a
  `char *` cursor with a direct `(float *)` cast. The cursor now has the
  sample type. No behaviour changes, and nothing is suppressed.


- **A clang build with link-time optimisation no longer loses a value its
  caller holds in `xmm0` when the library initialises on a host with
  AVX-512.** `vmaf_init_cpu()` runs one 512-bit instruction on such a host so
  the first frame does not wait for the 512-bit units, as inline assembly
  that declared `zmm0` clobbered. clang drops that declaration in a function
  not compiled for AVX-512; when link-time optimisation (the default build)
  inlined the function into its caller, a floating-point value the caller
  kept in `xmm0` came back as zero. The list now names `xmm0` too, which
  every compiler honours. Seen as a failing unit test (`45 - 20 * log10(x)`
  evaluated to 45) in the hosted `Ubuntu clang` jobs on runners with AVX-512;
  the `vmaf` tool of the same clang 22 build returned the same scores before
  and after. GCC builds were not affected.


- **Brought CPU lane back to clang-tidy baseline.** Resolved 13 clang-tidy
  regressions introduced by merges into `core/src/picture_pool.cpp`,
  `core/src/read_json_model.cpp`, `core/test/test_psnr_hvs_score.c`,
  `core/test/test_read_pictures_failure_ownership.c`, and `core/tools/vmaf.cpp`
  without baseline modifications or `NOLINT` waivers. Refactored callbacks,
  sign comparisons, test helpers, and assertion guards to preserve exact
  behavior and numerical equivalence across all CPU test suites.


- **CUDA: `adm_cm.fatbin` register pressure and spill stack eliminated.**
  Restructured `adm_cm_aim_line_kernel` into adaptive launch bounds
  (`adm_cm_aim_line_kernel_2` and `adm_cm_aim_line_kernel_4`, ADR-1226) and fused
  scales 1-3 (`i4_adm_cm_aim_line_kernel_fused`), eliminating the 255-register
  ceiling and 344-byte spill stack (`STACK:0`, `LOCAL:0` across all architectures,
  `REG <= 176` on sm_89, max 208 on sm_100/120) with bit-identical scores and
  19-31% whole-feature speedups (`T-CUDA-ADM-CM-REGISTER-PRESSURE-2026-09-07`).


- **`adm_cuda` returns the CPU's scores bit for bit.** The CUDA twin of the
  fixed-point ADM extractor carried its own copy of the CSF weight routine,
  which multiplied the exponent in `float` where the CPU multiplies in
  `double`, so its weights were 1 to 3 units in the last place off and
  `integer_adm_scale1..3`, `adm2` and `adm3` up to 2.1e-7 from the CPU. Its
  denominator kernels rounded each warp of a row where the CPU rounds the
  row, which shows on frames with little reference detail (6.6e-7), and
  derived the scale-0 rounding shift from an fp32 logarithm: on frames whose
  scale-0 border region has an area just above a power of two (81 areas up
  to 2^26, for example 962x13542) the denominator came out twice too large
  and `integer_adm_scale0` up to 0.12 too low. The twin now takes its CSF
  weights, rounding shifts and score conclusion from the CPU's own routines
  and folds the denominator once per row
  ([ADR-1416](docs/adr/1416-cuda-adm-cpu-row-rounding.md)). Measured on an
  RTX 4090 at `--precision max`: every output of every frame identical on
  the Netflix pair at 8, 10, 12 and 16 bits, both 1080p checkerboard pairs
  and BBB 3840x2160, with `debug=true` and with every option, including
  `adm_csf_mode` 1 to 3 and `adm_skip_scale0`. The parity gate compares this
  twin with tolerance 0. No change in time per frame (3.63 and 3.66 ms at
  3840x2160). Stored `adm_cuda` outputs change by up to 2.1e-7, and on the
  frame sizes above by the amounts given. The CPU extractor's scores do not
  change.


- **`ciede_cuda` computes the CPU's arithmetic and agrees with it to 1e-11.**
  The CUDA twin of `ciede` was up to 1.1e-5 from the CPU extractor and
  matched it on no frame. The CPU computes CIEDE2000 in double precision and
  stores intermediate values in `float`; the twin computed everything in
  `float`, with another form of the formula, and added per 16x16 block. The
  twin now evaluates the CPU's expressions in the CPU's types and adds the
  per-pixel values on the host in the CPU's order
  ([ADR-1426](docs/adr/1426-cuda-ciede-cpu-arithmetic.md)). Measured on an
  RTX 4090 at `--precision max`: 62 of 113 frames identical to the CPU and
  the rest within 1.4e-11 (Netflix 576x324 at 8, 10, 12 and 16 bits, both
  1080p checkerboard pairs, BBB 3840x2160). What is left is the math library:
  the CPU calls glibc, the device CUDA's functions, and a few pixels per
  million round to the neighbouring `float` (38 of 8.3 million on a 4K frame,
  because glibc's `powf` is not correctly rounded). The parity gate compares
  this twin at `1e-9` instead of `5e-3`. The price is time: a 3840x2160 frame
  takes 32.7 ms instead of 2.8 ms and a 576x324 frame 0.74 ms instead of
  0.34 ms, because the per-pixel math is now double precision; the CPU
  extractor takes 222 ms per 4K frame on sixteen threads. Stored `ciede_cuda`
  outputs change by up to 1.1e-5. The SYCL, HIP and Metal twins keep their
  `float` arithmetic and the `5e-3` tolerance.


- **A CPU extractor scores all three planes of device-resident input.** With
  the pictures in device memory (the FFmpeg `libvmaf_cuda` path, or the
  `DEVICE` picture preallocation) and an extractor that runs on the CPU,
  libvmaf downloaded the luma plane into the host picture only. The chroma
  planes stayed uninitialised: `psnr_cb` and `psnr_cr` came out as the 60 dB
  cap (the CPU gives 12.54 dB on the test pictures) and no error was reported.
  The download now takes every plane the picture has (Netflix/vmaf#1613).


- **`float_adm_cuda` returns the CPU's scores bit for bit.** The CUDA twin
  of `float_adm` was up to 1.3e-5 from the CPU extractor (`adm_scale0` at
  3840x2160) and matched it on 144 of 791 measured scores. Nine things
  differed: the association of the angle test's threshold (the 1.3e-5), the
  order of the sums, CSF weights from a copied formula that rounded
  differently, a true division where the CPU multiplies by a refined
  reciprocal estimate, the order of the masking threshold's terms, `float`
  constants and a `float` gain limit where the CPU uses `double`, and a
  floor of the frame sums at `1e-2` where the CPU's is `1e-10`. The last one
  could report `adm2 = 1` where the CPU reports 0, for content with almost no
  reference detail scored without the noise floor. The twin now runs the
  CPU's arithmetic in the CPU's types, adds each row on the device and the
  rows on the host in the CPU's order, and takes the weights, the reduced
  region, the pooling and the floor from the CPU's own routines
  ([ADR-1420](docs/adr/1420-cuda-float-adm-cpu-arithmetic.md)). The CPU's
  division is built on the processor's `RCPSS` estimate, so the twin probes
  that estimate when the extractor starts (about 10 ms) and evaluates it on
  the device. Measured on an RTX 4090 at `--precision max`: every output of
  every frame identical on the Netflix pair at 8, 10, 12 and 16 bits, both
  1080p checkerboard pairs and BBB 3840x2160, also with `debug=true` and
  with non-default `adm_enhn_gain_limit`, `adm_bypass_cm`,
  `adm_noise_weight`, `adm_skip_aim_scale` and viewing geometry. The parity
  gate compares this twin with tolerance 0. Not identical: `adm_p_norm`
  other than 1 or 3, where the twin is within 1.1e-7 of the CPU (the two
  `powf` implementations differ). A run of the twin alone takes 1.98 ms per
  3840x2160 frame instead of 1.87 ms; its kernels take 1.11 ms instead of
  0.76 ms. Stored `float_adm_cuda` outputs change in their low digits by at
  most 1.3e-5. The SYCL, HIP and Metal twins still agree with the CPU to
  four decimal places.


- **`float_moment_cuda` is bit-identical to the CPU `float_moment` extractor
  at 16 bits.** The CPU forms each sample's square in `float` before adding
  it, which at 16 bits is the square rounded to 24 bits; the CUDA twin added
  exact integer squares. Its second moments (`float_moment_ref2nd`,
  `float_moment_dis2nd`) were up to 1.0e-4 from the CPU's on 16-bit content
  with real low bits (0 of 77 such frames identical on an RTX 4090), while
  8-, 10- and 12-bit input and the first moments were identical. The 16-bit
  kernel now adds the CPU's float square, and all four outputs are identical
  on 262 of 262 measured frames at `--precision max`. The parity gate
  compares the CPU and CUDA `float_moment` cells with tolerance 0. One range
  stays within a derived bound instead: on a 16-bit frame of more than
  2 097 152 pixels whose sum of squares passes 2^53 the CPU's own sum rounds
  as it goes (2.7e-7 measured at 2560x1440). No measurable cost. Stored
  16-bit `float_moment_cuda` second moments change by up to 1.0e-4
  ([ADR-1453](docs/adr/1453-cuda-float-moment-cpu-float-squares.md),
  [CUDA backend](docs/backends/cuda/overview.md#float_moment_cuda-matches-the-cpu-float_moment-at-16-bits-2026-10-02)).


- **`float_motion_cuda` returns the CPU's scores bit for bit.** The CPU
  `float_motion` extractor adds the absolute differences of a row into one
  `float`, the row sums into another, and divides in `float`, so its score
  depends on that order. The CUDA twin summed each 16x16 block on the device
  and the blocks in `double` on the host, which left `motion`, `motion2` and
  `motion3` up to 1.36e-4 from the CPU on 1920x1080 checkerboards (above the
  5e-5 cross-backend tolerance), 2.4e-5 at 3840x2160 and 3.1e-6 on the
  Netflix 576x324 pair. It now adds each row on the device in the CPU's
  order and the rows on the host
  ([ADR-1409](docs/adr/1409-float-motion-twins-cpu-float-sum.md)). Measured
  on an RTX 4090 at `--precision max`: every frame identical on the Netflix
  pair, both 1080p checkerboard pairs and 200 frames of BBB 3840x2160, also
  at 10 bits and with the fps-weight, cap and blend options set; the time per
  3840x2160 frame did not change (3.00 and 2.98 ms). The parity gate compares
  this twin with tolerance 0. Stored `float_motion_cuda` outputs change in
  their low digits by at most those differences. The SYCL, HIP and Metal
  twins still agree with the CPU to four decimal places.


- **`float_motion_cuda` emits `motion3`, like the CPU `float_motion`.** The
  CUDA twin wrote `motion` and `motion2` only, so `--backend cuda --feature
  float_motion` lost `VMAF_feature_motion3_score` without a warning. It now
  publishes the CPU's `motion3` (the fps-weighted `motion2`, blended by
  `motion_blend_factor` / `motion_blend_offset` and capped at
  `motion_max_val`; frame 0 from the first SAD, `0` for a one-frame input)
  and accepts both blend options (aliases `mbf` / `mbo`). On an RTX 4090 it
  stays within 2.8e-6 of the CPU on the Netflix pair, like `motion2`. The
  SYCL, HIP and Metal twins still write no `motion3`
  (`T-GPU-FLOAT-MOTION3-MISSING-2026-09-30`).


- **`float_ms_ssim_cuda` adds the terms of every scale in the CPU's order**
  (ADR-1465). CPU `float_ms_ssim` adds the luminance, contrast and structure
  value of every window into three double-precision sums per scale, from the
  first window to the last; the twin added the same values in blocks, and on
  rare frames (four in 8.3 million noise frames) a per-scale mean rounded to
  the neighbouring `float`, once moving the score in its tenth digit. The
  device now stores every window's values and the host adds them in order,
  so `float_ms_ssim` and the fifteen `enable_lcs` outputs equal `--backend
  cpu` on those frames and on every measured one. Cost: about 2.1 ns per
  scored window: 0.33 to 0.81 ms at 576x324, 2.7 to 8.8 ms at 1920x1080,
  10.9 to 33.7 ms at 3840x2160
  (`T-CUDA-FLOAT-MS-SSIM-EXACT-THROUGHPUT-2026-10-02`).


- **`float_psnr_cuda` is bit-identical to the CPU `float_psnr` extractor at
  every bit depth.** The CPU squares each sample difference in `float` and
  adds the squares in `double`, which does not round. The CUDA twin added
  each 16x16 block in single precision: exact at 8 bits, and at 10, 12 and
  16 bits only while the differences in a block are small. On an RTX 4090 it
  matched the CPU on every frame of real clips and was up to 1.2e-7 dB off
  on high-bit-depth input with large differences (2 of 92 such frames
  identical). The kernel now adds the squares as integers, and 268 of 268
  measured frames are identical at `--precision max`, with `uncapped=true`
  too. The parity gate compares the CPU and CUDA `float_psnr` cells with
  tolerance 0. No measurable cost. Stored `float_psnr_cuda` scores of such
  input change by up to 1.2e-7 dB
  ([ADR-1455](docs/adr/1455-cuda-float-psnr-exact-block-sums.md),
  [PSNR](docs/metrics/psnr.md#float_psnr)).


- **`float_ssim_cuda` adds its frame sums in the CPU's order** (ADR-1464).
  CPU `float_ssim` adds every window's value into one double-precision sum
  from the first window to the last; the twin added the same values in
  blocks, and on rare frames (two in 31 million noise frames) the mean
  rounded to the neighbouring `float`. The device now stores every window's
  value and the host adds them in order, with `enable_lcs` the luminance,
  contrast and structure sums too, so `float_ssim` and `float_ssim_l`, `_c`,
  `_s` equal `--backend cpu` on every input: 9828 of 9828 measured values and
  880 000 of 880 000 on noise, the constructed frame of
  `core/test/float_ssim_order_frame.h` included. Cost: about 1 ns per scored
  window. Nothing measurable at the automatic scale of 1080p and 4K input;
  where the picture is scored at full size, 0.14 to 0.33 ms at 576x324 and
  3.9 to 12.6 ms for 3840x2160 with `scale=1`
  (`T-CUDA-FLOAT-SSIM-EXACT-THROUGHPUT-2026-10-02`).


- **`float_vif_cuda` returns the CPU's scores bit for bit.** The CUDA twin
  filtered with a table of Gaussian taps that the CPU `float_vif` extractor
  stopped using when it began to compute its filters at start-up (26 of the
  34 taps differ in the last digits), called the device `log2f` where the
  CPU evaluates a polynomial, kept `vif_sigma_nsq` in `float` where the CPU
  keeps it in `double`, and summed per 16x16 block where the CPU adds row by
  row in `float`. That left `vif_scale0..3` up to 3.8e-5 from the CPU on the
  Netflix 576x324 pair and 7.0e-6 at 3840x2160, with no frame identical. The
  twin now takes the taps from the CPU's own routine, evaluates the CPU's
  per-pixel statistic in its types, and adds the terms of each row on the
  device and the rows on the host in the CPU's order
  ([ADR-1412](docs/adr/1412-cuda-float-vif-cpu-arithmetic.md)). Measured on
  an RTX 4090 at `--precision max`: every output of every frame identical on
  the Netflix pair at 8, 10, 12 and 16 bits, both 1080p checkerboard pairs
  and BBB 3840x2160, also with `debug=true` and with non-default
  `vif_enhn_gain_limit`, `vif_sigma_nsq` and `vif_skip_scale0`. The parity
  gate compares this twin with tolerance 0. `float_vif_cuda` also accepts
  the CPU's per-scale floors `vif_scale1_min_val`, `vif_scale2_min_val` and
  `vif_scale3_min_val`. A run of the twin alone takes the same time per
  3840x2160 frame (1.97 and 1.96 ms); its kernels take 1.00 ms instead of
  0.72 ms. Stored `float_vif_cuda` outputs change in their low digits by at
  most 3.8e-5. The SYCL, HIP and Metal twins still agree with the CPU to four
  decimal places.


- **`motion_cuda` computes the CPU `motion` arithmetic.** The CUDA twin blurred
  each frame and differenced the blurred frames, while the CPU (since the
  upstream pipelined-motion port) blurs the frame difference and rounds after
  each filter pass; the two orders round differently (the SYCL twin with the
  same order was up to 2.0e-4 off on 17x17 frames and 1.3e-5 on the Netflix
  576x324 pair). `motion_cuda` now runs the kernel `motion_v2_cuda` already
  used, whose arithmetic is the CPU's, and its debug `integer_motion` score
  is the CPU's (weighted by `motion_fps_weight`, capped at `motion_max_val`).
  Each frame is ordered against the previous one on the device instead of by
  the engine's context barrier, and the eight-frame batch readback waits once
  instead of twice. `motion_v2_cuda`'s SAD is unchanged. On an RTX 4090
  `integer_motion2` / `integer_motion3` now equal the CPU's on the Netflix
  pair and on 50 frames of a 3840x2160 clip, where they were 1.26e-5 and
  6.9e-5 off (ADR-1372; `docs/state.md`,
  `T-CUDA-MOTION-BLUR-THEN-DIFF-2026-09-29`;
  [CUDA backend](docs/backends/cuda/overview.md#cpu-parity-motion-options-and-tiny-frames-2026-09-30)).
- **CUDA integer ADM and VIF guard tiny frames like their SYCL twins.** The
  integer ADM DWT kernels take their row and tap arithmetic from a header a
  device-free test replays for every plane height: from the 17-row ADM minimum
  up no load leaves the plane, and the scale-0 load is clamped into the plane
  below it. `vif_cuda` needs 16 pixels in each dimension; model dispatch and
  `--backend cuda --feature vif` compute smaller frames with the CPU `vif`, and
  `--feature vif_cuda` on such a frame fails `init()` instead of returning
  scores from clamped taps (ADR-1374).


- **`motion_cuda` emits `VMAF_integer_feature_motion_sad_score`, as the CPU
  `motion` extractor does.** The CPU writes the frame's SAD score on every
  frame (weighted by `motion_fps_weight`, capped at `motion_max_val`); the
  CUDA twin computed it and published it only as the debug `integer_motion`
  score, so the result of `--backend cuda --feature motion` lacked a key the
  CPU result has. It now writes it on every frame, bit-identical to the CPU
  on 348 of 348 measured frames on an RTX 4090, also with `debug`,
  `motion_force_zero`, `motion_moving_average` and weight, blend and cap
  options. `motion2` / `motion3` and the frame time are unchanged
  ([motion](docs/metrics/motion.md#output-features)).


- **CUDA: `test_cuda_runtime_unwind` pins allocating state on host-pinned pictures.**
  Host-pinned pictures (`vmaf_cuda_picture_alloc_pinned`) record the allocating
  state on `priv->cuda.state`, preventing a NULL dereference of `state->f` during
  unref (upstream Netflix/vmaf#1573 hunk a). A device-free test
  `test_pinned_picture_release_uses_the_allocating_state` exercises the allocation
  and unref through the fake driver table, ensuring the allocating state is pinned
  across platforms.


- **CUDA `psnr_hvs` returns the CPU extractor's scores bit for bit**
  (`T-PSNR-HVS-CPU-FLOAT-SUM-4K-2026-09-30`,
  [ADR-1397](docs/adr/1397-psnr-hvs-twins-cpu-float-sum.md)). The CPU adds every
  masked coefficient error of a plane into one running `float`, so its score
  depends on the order of the additions; `psnr_hvs_cuda` summed each block first
  and was up to 1.7e-2 dB from `--backend cpu` at 3840x2160, beyond the parity
  tolerance. The kernel now stores the 64 terms of every block in the CPU's
  arithmetic and the host adds them in the CPU's order: `psnr_hvs`, `psnr_hvs_y`,
  `psnr_hvs_cb` and `psnr_hvs_cr` are identical to the CPU at `--precision max`
  from 576x324 to 3840x2160 and at 8 to 12 bits, and the parity gate compares
  this twin with tolerance 0. `psnr_hvs_cuda` scores therefore change in their
  last digits (by up to 1.7e-2 dB at 3840x2160). The twin is slower for it: on
  an RTX 4090 a 3840x2160 frame takes 12.2 ms instead of 2.4 ms, and the term
  buffer needs 65 MB per 3840x2160 frame; tuning is tracked as
  `T-CUDA-PSNR-HVS-EXACT-SUM-THROUGHPUT-2026-10-01`. The HIP and SYCL twins
  keep their per-block sums until their rewrites land. See
  [the psnr_hvs page](docs/metrics/psnr-hvs.md#agreement-with-the-cpu-extractor).


- **`speed_chroma_cuda` and `speed_temporal_cuda` match the CPU with
  `speed_prescale_method=lanczos4`.** The CPU scaler evaluates each lanczos4
  weight in fp64 with `sin()` and rounds it once; the CUDA scale kernel
  evaluated the weights itself in fp32, a few ulp off on some of them, and
  SpEED amplifies that on smooth content. On an RTX 4090 the twins were up to
  8.8e-3 relative (0.27 absolute) from the CPU on a smooth synthetic field and
  1.0e-3 absolute on a 1920x1080 gradient with noise, beyond the cross-backend
  tolerance of 1e-4. The weights depend only on the output column and row, so
  the host now evaluates them once per run with the scaler's own routine and
  the kernel reads the table: nearest, bilinear, bicubic and lanczos4 prescale
  at 0.5 and 2.0 are all bit-identical to the CPU extractor. Scores with the
  other three methods, and without prescale, are unchanged. The SYCL and HIP
  twins still evaluate the weights on the device
  ([SpEED](docs/metrics/speed_qa.md#cuda-the-same-chain-on-the-device)).


- **`integer_ssim_cuda` returns the CPU's `ssim` bit for bit.** The CUDA twin
  of the fixed-point `ssim` extractor computed the CPU's moments and the
  CPU's per-pixel term, but added the terms per 16x8 block where the CPU adds
  them one after the other across the whole frame. Its score matched the CPU
  on no measured frame and was up to 1.1e-11 away (3.6e-10 with
  `enable_db`); on very small identical frames it could report `+inf` where
  the CPU reports about 156 dB. The twin now reads the per-pixel terms back
  and adds them on the host in the CPU's order
  ([ADR-1424](docs/adr/1424-cuda-ssim-cpu-frame-sum.md)). Measured on an RTX
  4090 at `--precision max`: identical on every frame of the Netflix pair at
  8, 10, 12 and 16 bits, both 1080p checkerboard pairs and BBB 3840x2160,
  with and without `enable_db` / `clip_db`, and on frames from 1x1 up. The
  parity gate now knows the `ssim` feature and compares the CUDA twin with
  tolerance 0. The price is time: a 3840x2160 frame takes 9.7 ms instead of
  2.2 ms, because 8.3 million terms are read back and added sequentially; at
  576x324 the difference is not measurable. The twin also needs 66 MB more
  device memory and as much pinned host memory at 3840x2160. The SYCL, HIP
  (above 64x64) and Metal twins still agree with the CPU to four decimal
  places or better.


- **`ssimulacra2_cuda` returns the CPU extractor's score bit for bit.** The
  CUDA twin of `ssimulacra2` computed the CPU's per-pixel terms but added
  them in a tree, where the CPU adds them one after the other into one
  `double`; every add rounds, so the two ended a few units in the last place
  apart. Measured on an RTX 4090 at `--precision max`, 8 of 113 frames
  matched and the rest were up to 7.3e-11 away. The twin now forms the sums
  of the CPU's loops on the device
  ([ADR-1433](docs/adr/1433-cuda-ssimulacra2-cpu-sum-order.md)): while a
  running sum stays between two powers of two, adding a term moves it by a
  whole number of steps, so the device adds those whole numbers per
  1024-pixel chunk in parallel, one pass over the chunks puts them together,
  and the few chunks in which the sum passes a power of two are added term by
  term. All 113 frames are identical (Netflix 576x324 at 8, 10, 12 and 16
  bits, both 1080p checkerboard pairs, BBB 3840x2160), and the parity gate
  compares the cell at 0 instead of `5e-3`. The price is time: a 3840x2160
  frame takes 15.6 ms instead of 7.8 ms and a 576x324 frame 1.7 ms instead of
  0.4 ms; the CPU extractor takes 126 ms per 4K frame on sixteen threads.
  Stored `ssimulacra2_cuda` scores change by up to 7.3e-11. The SYCL and HIP
  twins keep their tree sums and the `5e-3` tolerance.


- **Several CUDA instances on one device no longer get wrong `vif` scores.**
  `integer_vif_cuda` cleared its accumulators on its private stream while the
  scale 0 kernels that add into them ran on the picture stream, with nothing
  ordering the two. One instance never lost the race; with four instances on
  one `CUcontext` a late clear erased the first adds, and every run returned
  wrong `vif` scales (and, through the model, wrong VMAF) for tens of frames.
  The clear now runs on the picture stream. Measured on an RTX 4090 with four
  instances on one context, 48 frames of the Netflix 576x324 pair: 28 to 85
  wrong frames of 192 per feature in every run before, none in 105 runs after
  (Netflix/vmaf#1305).


- **The Python extension builds again on every compiler.**
  `compat/python-vmaf/core/adm_dwt2_cy.pyx` re-declares `adm.c`'s static
  `init_dwt_band_d()` for Cython. PR #1859 changed that helper's cursor from
  `char *` to `double *` and its length from bytes to samples, but the
  declaration kept `char *`, so the generated `adm_dwt2_cy.c` passed a `char *`
  where a `double *` is expected and no C compiler accepted it (clang 22 on
  Ubuntu x86-64 and ARM, Homebrew clang on macOS, GCC too): the wheel build of
  the Python harness failed. The declaration, the cursor and the call now use
  `double *` and a length in samples; the contract test
  `test_cython_adm_dwt_band_decl_contract` keeps them in step with `adm.c`.


- **macOS release builds no longer branch from `close()` into a feature
  extractor.** macOS declares the C library's `close()` with an assembler
  label, and in a full-LTO link (the release default) that symbol and the
  CPU extractors' `static close()` callbacks became the same symbol: the
  `close()` on the fdopen() failure paths of the output file, the CAMBI
  heatmap file and the SVM model save called an extractor's close with a
  file descriptor and crashed, and `test_adm_coverage` crashed on every macOS
  leg. The callbacks are named `close_fex`, and
  `test_libc_named_internal_functions` refuses a static C function named
  after a C library function.


- The `vmaf-dev-mcp` container starts again on an image built from the
  current `dev/Containerfile`. Its entrypoint ran `chmod 1777 /tmp` as the
  unprivileged `vmaf` user; uutils coreutils 0.10 in the updated Ubuntu
  26.04 base issues that call even when the mode is already 1777, so it
  failed and the container restarted in a loop. The mode is now changed
  only when it is wrong and `/tmp` belongs to the entrypoint's user.


- **Omit `-march=native` from the reference binary build in `vmaf-dev-mcp`.**
  Intel oneAPI `icx` contracts multiply-accumulate operations in unvectorized CPU
  extractor scalar loops when `-march=native` exposes FMA target capabilities,
  drifting SpEED scores from uncontracted reference builds by up to 7.9e-4 on
  1080p content and 3.38e-7 on the Netflix 576x324 48-frame pair. Removing
  `-Dc_args="-march=native"` restores bit-exact CPU reference parity with
  standard GCC reference builds, following the golden-gate isolation principles
  of [ADR-1317](docs/adr/1317-golden-gate-build-isolation.md).


- **Preserve explicit int8 paths in tiny-model DNN session loading.** When
  `vmaf_dnn_session_open()` was called with an explicit `.int8.onnx` path,
  `resolve_load_path()` lacked the `kInt8Suffix` early return present in
  `dnn_attach_api.c`, causing it to append a redundant `.int8` suffix and derive
  `<name>.int8.int8.onnx` before falling back to the fp32 path. The resolver now
  checks `kInt8Suffix` upfront and preserves explicit int8 paths directly.


- Restored the section links that older pages and ADRs use into the CLI,
  `vmaf_bench`, environment-variable and Getting started pages after their
  rewrite (#1934, #1938): each former section name is a short heading that
  points to the section now holding its content, and Getting started keeps its
  "Build from source (any platform)" heading. `mkdocs build --strict` passes
  on master again.


- **The Kubernetes E2E workflow bounds its tool downloads and stops hiding
  failed diagnostics.** The kind, kubectl, Helm and kuttl downloads had
  retries but no deadline, so a stalled server held the job until its
  45-minute limit; each now has `--max-time 300`. The failure diagnostics
  discarded every error with `|| true` and `2>/dev/null`; each command now
  reports a warning when it fails and the next one still runs.


- **The node's eBPF program declares `"GPL"` to the kernel instead of
  `"Dual BSD/GPL"`
  ([ADR-1559](docs/adr/1559-ebpf-kernel-licence-string.md)).** The source of
  the descriptor tracker is EUPL-1.2, but its compiled object claimed a BSD
  grant the project never made. Loaded into the kernel, the program is
  combined with GPL-2.0 code and calls GPL-only helpers, which EUPL-1.2's
  compatibility clause (Article 5, GPL v2 and v3 in its Appendix) allows to be
  distributed under the GPL. The object loads as before; only its licence
  section changed.


- **A feature score of the newest picture can be read with worker threads.**
  With `n_threads > 0`, `vmaf_feature_score_at_index()` and
  `vmaf_feature_score_pooled()` answered `-EINVAL` for a picture already read
  while a worker thread was still computing its score and the score store had
  no slot for it yet (the first picture of a feature, or the ninth). They now
  wait for the worker threads first, as they already did for `-EAGAIN`, and
  return the score. A feature no extractor writes is still `-EINVAL`.


- **The FFmpeg `libvmaf` and `libvmaf_cuda` filters no longer print a score
  after a mid-run error.** When a frame could not be copied or read, the run
  failed, but the filter still printed a `VMAF score:` line pooled over fewer
  frames than were decoded. Usually that line was an uninitialised
  `0.000000`. The filter now logs one error naming the frame and the cause,
  exits non-zero, and prints no score and writes no report. A failed flush or
  a failed pooled score prints no score line either. This differs from
  upstream FFmpeg on purpose
  ([ADR-1768](docs/adr/1768-ffmpeg-libvmaf-no-score-after-error.md)).
- **Float extractors report their errors through the log (ADR-1906).** The
  allocation and stride errors of the float ADM, SSIM, MS-SSIM, motion and VIF
  code (`error: ...` lines) went to standard output, where they mixed with
  anything a program writes there and ignored the log level. They are now
  `ERROR` log lines: on stderr at the configured level for `libvmaf.h` and
  the CLI, and in the context's log callback for the VMAFx API.


- **The FFmpeg `libvmaf_sycl` filter no longer scores fewer frames than it
  decoded, or the wrong surfaces.**
  - When a VA surface import failed, the filter used to pass the frame on
    unscored, so the pooled score covered fewer frames than the input
    without saying so. Now it tries again, three tries in all with 1 ms
    between them, and then stops with an error that names the frame. It
    prints no score after stopping
    ([ADR-1761](docs/adr/1761-sycl-filter-import-retry-then-fail.md)).
  - The reference input's surfaces were imported with the distorted
    input's VA display. With the two decoders on two VA devices, that
    scored other surfaces without an error: 0 of 24 frames equal to the
    CPU on an Arc A380, and a plausible pooled score. When a surface did
    not exist in that display, the import failed, which is the failure the
    skip used to hide. Each input is now imported with its own display.
  - The software path also left the last chroma row of an odd-height frame
    at zero; it now copies every row.
  - The `libvmaf_metal` filter already stopped on a failed import, but then
    printed a pooled score over the frames before the failure. It now
    prints no score after it stops, as `libvmaf_sycl` does.


- **`-qpfile` works on libx264, and the saliency tools no longer run a libx264
  encode without the ROI they asked for
  ([ADR-2167](docs/adr/2167-ffmpeg-x264-qpfile-quant-offsets.md)).** Patch
  `0007` gave the libx264 wrapper a `-qpfile` option that called
  `x264_param_parse(.., "qpfile", ..)`; libx264 has no such parameter (the x264
  command line reads the file), so the encoder never opened. The option now
  reads the file and applies each frame's per-macroblock QP offsets through
  x264's `quant_offsets` (+12 on every macroblock: 4186 bytes, none: 20123,
  -12: 104468 on six 576x324 frames), and fails at open, naming why, when
  adaptive quantization is off (`-preset ultrafast` turns it off), the block
  grid is not the video's macroblock grid or the file is malformed. The Go
  and Python saliency code passed `-x264-params qpfile=`, which FFmpeg only
  warns about (`Error parsing option`) before encoding without the ROI; they
  now pass `-qpfile`, which stock FFmpeg refuses. The saliency tests'
  encode-runner stub no longer records the `ffmpeg -version` probe as the
  encode (two tests failed on that, depending on the order they ran in).


- **`float_adm` and the models that read it use Netflix's CSF weights again;
  `vmaf_float_v0.6.1` moves by up to 2.2e-5 per frame.** Two inherited
  routines compute the contrast-sensitivity weights of float ADM: the Watson
  quantisation step (`adm_tools.h`) and the Barten model (`barten_csf_tools.h`,
  `adm_csf_mode=1`). Netflix keeps their intermediates in `float`; fork ports
  (#552, #760, #44) had widened them to `double` to quiet a static-analysis
  finding, which moved every weight by a few units in the last place. The
  fork goes back to Netflix's arithmetic
  ([ADR-1489](docs/adr/1489-float-adm-barten-upstream-float.md)). The one
  remaining difference from Netflix's `float_adm` on x86 is the division this
  fork chose in ADR-1442: against a Netflix build with the plain quotient,
  every `float_adm` value under 36 option sets and the score of every float
  model are identical on 658 measured frames, at scalar, AVX2 and AVX-512
  dispatch. What you see: `float_adm` scores move in the seventh decimal
  (`adm2` by at most 1.1e-7, the per-scale scores by at most 2.7e-7), the
  float models (`vmaf_float_v0.6.1`, `vmaf_float_v0.6.1neg`,
  `vmaf_float_4k_v0.6.1`, `vmaf_v0.6.0`) in the fifth (at most 2.7e-5 on a
  frame, 1.5e-5 on a clip's mean), and fixed-point `adm` with
  `adm_csf_mode=1` in the seventh (at most 1.6e-7); on the CPU and on the
  CUDA, SYCL, HIP and Metal twins alike. The default models and fixed-point
  `adm` in its default mode do not change. The Netflix golden gate passes
  unchanged.


- A second `float_adm` instance with `debug=true` is refused when it is registered,
  with a message naming the key `adm` both would write, instead of failing at the
  first frame with "problem reading pictures". The unsuffixed `adm` key stays (the
  Netflix tests read it); the CUDA, SYCL and HIP `float_adm` twins now file it
  unsuffixed like the CPU, where they added the option suffix.


- **`float_adm` refuses frames smaller than 17x17 instead of reading outside
  its buffers.** The float ADM extractor decomposes each frame into four
  wavelet levels. Below 17 pixels in width or height the coarsest level has a
  single sample, and the extractor read next to it: at 8 pixels or fewer the
  read lands before the start of a heap buffer (confirmed with
  AddressSanitizer), from 9 to 16 it picks up a sample of another level, so
  the scores were not meaningful (a random 8x8 pair scored `adm_scale3 =
  1.05`). `float_adm` and `float_adm_cuda` now fail at start with
  `float_adm requires width >= 17 and height >= 17 (got WxH)`, as the
  fixed-point `adm` extractor already does. Frames of 17x17 and larger are
  unaffected. The SYCL, HIP and Metal `float_adm` twins still accept smaller
  frames.


- **`--backend <gpu> --feature float_moment` runs the backend's twin.** The
  command computed `float_moment` on the CPU and warned that the backend had
  no twin, although `float_moment_cuda`, `float_moment_sycl`,
  `float_moment_hip` and `float_moment_metal` exist: the CPU extractor
  declared the pseudo-name `float_moment` instead of the four features it
  writes, so the lookup that pairs an extractor with its twin never matched.
  It now declares `float_moment_ref1st`, `float_moment_dis1st`,
  `float_moment_ref2nd` and `float_moment_dis2nd`. Naming the twin and the
  CPU extractor together (`--feature float_moment_cuda --feature
  float_moment`) no longer fails with `feature "float_moment_ref1st" cannot
  be overwritten`. Scores were correct before
  and are unchanged on the CPU; on an RTX 4090 the twin equals the CPU on
  every frame at 8, 10 and 12 bits and is within 7e-6 on the second moments
  at 16 bits. See [Float moment](docs/metrics/features.md).


- **`float_moment_cuda`, `float_moment_sycl` and `float_moment_hip` are
  bit-identical to the CPU `float_moment` on every frame, 16-bit frames of
  more than 2 097 152 pixels included.** The CPU adds the float squares into
  one `double` in raster order, and once that sum passes 2^53 units of 2^-16
  it rounds as it adds; the twins rounded the exact sum once and were up to
  5.1e-7 from the CPU's second moments there (0 of 16 frames of 16-bit
  3840x2160 noise and 0 of 4 of 16-bit 7680x4320 noise identical on each
  device). On such frames four more kernels now form the CPU's rounded sum
  from rows (`core/src/feature/float_moment_sum.h`), and all four outputs are
  identical on every measured frame at `--precision max` on an RTX 4090, an
  Arc A380 and a gfx1036. Frames that cannot pass 2^53 (every 8-, 10- and
  12-bit frame and every frame up to 2 097 152 pixels) run no new work. A
  16-bit 3840x2160 frame past 2^53 costs +0.03 ms on the RTX 4090, +2.8 ms on
  the A380 and +7.2 ms on the gfx1036. Stored 16-bit second moments of large
  bright frames from these twins change by up to 5.1e-7
  ([ADR-1497](docs/adr/1497-float-moment-twins-cpu-sum-past-2-53.md),
  [CUDA backend](docs/backends/cuda/overview.md#float_moment_cuda-matches-the-cpu-float_moment-past-253-units-too-2026-10-03)).


- **`float_psnr_cuda`, `float_psnr_sycl` and `float_psnr_hip` are
  bit-identical to the CPU `float_psnr` on every frame, 16-bit frames whose
  sum of squared differences passes 2^53 units included.** The CPU adds each
  row exactly and the rows into one `double`, which rounds there; the twins
  rounded the exact frame total once and were up to 3.3e-13 dB from the CPU
  (0 of 8 frames of 16-bit 3840x2160 half-range noise identical on CUDA and
  HIP, 1 of 8 on SYCL). Each kernel block now covers 256 pixels of one row,
  and the host adds each row's exact sum in the CPU's order
  (`core/src/feature/float_psnr_rows.h`). No measurable cost
  ([ADR-1499](docs/adr/1499-float-psnr-twins-cpu-row-order.md),
  [PSNR page](docs/metrics/psnr.md#past-253-units-2026-10-03)).


- **`float_ssim` on frames smaller than its 11x11 window is 0 on every CPU
  path, as in Netflix's libvmaf, and no longer reads or writes outside its
  workspace.** On a host with AVX2, AVX-512 or NEON an 8x8 frame scored
  values such as 0.46 or 0.81 that changed from run to run, and a frame of
  4x4 or smaller overran a heap buffer: the SIMD kernels were given the
  product of the two window extents, which is positive when both are
  negative. The scalar path (`--cpumask 63`) was always correct. Frames of
  11x11 and larger are unchanged.


- **`fr_regressor_v3` receives the codec block it was trained on.** libvmaf
  normalised every codec-aware model's preset and CRF slots the way
  `fr_regressor_v2` was trained (preset ordinal / 9, CRF / 63); v3 was trained
  with `preset_norm` 0.5 on every row and the CRF min-max normalised over
  19..37. A sidecar can now declare its normalisation (`codec_preset_norm`,
  `codec_crf_norm` and bounds), `fr_regressor_v3.json` does, and an encoding
  libvmaf cannot reproduce refuses the model. With v3, `--tiny-preset` has no
  effect and the run says so
  ([ADR-1558](docs/adr/1558-codec-block-encoding-from-sidecar.md)).


- **`motion` was NaN in every row of every extracted feature table.** libvmaf emits no
  `integer_motion` key; the first-order motion score is `VMAF_integer_feature_motion_sad_score`.
  `ai/data/feature_extractor.py` now reads it, so the `motion` column of `FULL_FEATURES` holds values.
  Tables extracted before this change carry an all-NaN `motion` column and the `verify_features` stage of
  the mini retrain refuses them. `extract_full_features.py` also gains `--assume-dims WxH` for corpora
  that are not 1920x1080.


- **The Gitleaks check scans only the commit it checked out.** It ran
  `git log --all` over a full-history checkout, so a finding on any branch in
  the repository, including a commit a force-push had already replaced,
  failed every other open pull request. Each run now scans the history of
  its own `HEAD`: on a pull request, master plus the PR's commits.


The controller's `--version` test passes on hosts without an installed libvmaf: it runs the binary with the dynamic loader's search path the test itself runs with, instead of failing to load the build tree's library.


The required Go job passes its gosec scan again: the `VMAF_BIN` lookup of the Go test helper carries a reasoned G703 exclusion, and `make lint-go` and the Go job now run the same scan, so `make lint` no longer reports the HISS rule fixtures.


- **The Go package documentation says how `vmafx-mcp` and `vmafx-server`
  reach libvmaf.** `pkg/libvmaf` and the MCP server's tool comment claimed the
  binaries do not link `libvmaf.so` at run time; they do, through cgo
  (`ScoreDirect`, `StreamScorer`, `DNNSession`), so the library must be
  installed next to them. The package documentation now lists all four paths
  into libvmaf, and a contract test fails when a Go package that links the
  library claims otherwise.


- **The `vmafx-operator`, `vmafx-server` and `vmafx-node` images carry the
  licences of everything they link, and the node image's FFmpeg is
  redistributable.** Each Go program's modules are read from its build
  information; their licence and notice files ship under
  `/usr/local/share/vmafx/licenses/go/`, the notices list every module with its
  licence, and the source of the copyleft modules (MPL-2.0, EUPL-1.2, LGPL) is
  published as `<image>:<tag>-source`, checked against the binary's module sums.
  The node image's FFmpeg is no longer configured `--enable-nonfree` (which made
  the binary declare itself not redistributable although no nonfree part was
  built); it is published under the GNU GPL version 3 or later with its exact
  patched source and configure line. The libraries the node image copies out of
  Debian packages keep their copyright files and package records, and rclone is
  built from its release's module source (`RCLONE_VERSION` in `build-config.env`
  replaces `RCLONE_IMAGE`), so its source is exact. Every image build fails on a
  file or module without a recorded licence
  ([ADR-1514](docs/adr/1514-go-and-node-image-licensing.md),
  [licensing](docs/licensing.md)).


- **Go tests that score with the `vmaf` CLI use the build under test.** They
  looked for `vmaf` on `PATH` or under `/usr/local/bin` and so passed or failed
  on whatever release the host had installed. `internal/vmaftest` resolves
  `VMAF_BIN`, else `core/build-cpu/tools/vmaf`, and fails the test, naming the
  build command, when neither exists. See
  [Go development](docs/development/languages.md).


- **The `test-netflix-golden` target checks for pytest before execution.** When
  invoked in a fresh worktree where `.venv` only contains build-time dependencies,
  `make test-netflix-golden` previously stopped with `No module named pytest`.
  The target now checks for `pytest` availability up front and fails with an
  actionable error directing the developer to the documented install command in
  `docs/development/languages.md`.


- **The CUDA, ROCm and oneAPI images carry the licences of what they contain,
  ship only the vendor files `vmaf` loads, and publish the source their copyleft
  parts require.** All three are now Debian 13 images. The CUDA image holds no
  NVIDIA library (the host driver provides `libcuda`) and passes on the CUDA
  Toolkit EULA terms for the NVIDIA code inside the kernels; the ROCm image holds
  the HIP runtime files instead of AMD's whole 29 GB development image, which
  carried a profiler library whose licence forbids redistribution, and its
  kernels now cover all 25 GPU targets ROCm 10.0.0 supports; the oneAPI image
  holds the redistributable SYCL runtime files from the compiler instead of
  Intel's full runtime set, and reaches the GPU through Level Zero only. Each
  image has `/usr/local/share/vmafx/licenses/THIRD_PARTY_NOTICES.txt`, fails its
  build when a file has no recorded licence, gets an attested SPDX SBOM, and is
  published with `<tag>-cuda13-source`, `<tag>-rocm10-source` or
  `<tag>-oneapi2026-source`. The vendor toolchains (`nvcc`, `hipcc`,
  `/opt/intel/oneapi`) are no longer in the images, and the ROCm and Intel
  libraries moved to `/usr/local/lib/rocm` and `/usr/local/lib/intel`
  ([ADR-1517](docs/adr/1517-gpu-image-licensing.md),
  [production images](docs/development/docker-production.md#gpu-variants),
  [licensing](docs/licensing.md)).


- **`motion_force_zero` no longer crashes `motion_cuda` and
  `float_motion_cuda`.** With the option set, the twins' `init()` switches
  them from the asynchronous `submit()` / `collect()` pair to a synchronous
  `extract()` that publishes zeros, but the engine had already chosen the
  asynchronous path and called the cleared `submit()` on the first frame:
  `--backend cuda --feature motion_cuda=motion_force_zero=true` (or
  `float_motion_cuda=...`) died with SIGSEGV. The engine now initialises such
  an extractor before it picks the path, so both twins publish zeros for
  every frame, as the CPU extractors do. The Metal motion twin makes the
  same switch and takes the same engine path; the HIP motion twins keep an
  asynchronous pair that writes the zeros (see the `motion_hip` entry)
  (`T-GPU-MOTION-FORCE-ZERO-FIRST-FRAME-SEGV-2026-09-30`).


- **Hardware we need: a processor row takes the verdict of the report's processor
  checks.** `scripts/docs/generate-hardware-reports.py` gave a CPU family row (for
  example "x86 with AVX2 and no AVX-512") the report's overall verdict, so a report
  whose CPU checks all passed but whose GPU section failed rated the processor row
  "worst fail". A CPU row now counts the dispatch and reference equivalence checks,
  the unit tests, the golden check and the image's file check; the native macOS row
  adds the Metal equivalence and the Metal gate, because it also closes the Metal
  rows. GPU rows keep their device's verdict. `CpuRowVerdictTests` in
  `scripts/docs/tests/test_hardware_needs.py` fails on the old generator.


- **The Helm chart no longer renders auth settings that nothing applies
  ([ADR-1519](docs/adr/1519-controller-tenant-registry.md)).** `auth.enabled`
  passed `VMAFX_AUTH_*` to the default `vmafx-server` image, which has no auth
  gateway, so the server ran unauthenticated; the render now fails unless
  `image.repository` names a vmafx-controller image and `workload` is
  `Deployment`. `enabled: false` in an `auth.tenants` entry rendered as
  `true`; it now renders as `false`.


- **Helm: Intel GPUs on the `xe` kernel driver schedule.** The chart requested
  `gpu.intel.com/i915` for every Intel GPU, so pods on nodes whose GPUs run on
  `xe` (Arc B-series and newer) stayed `Pending`. `gpu.intelDriver: xe`
  requests `gpu.intel.com/xe` (the default stays `i915`), and
  `gpu.resourceName` requests any other resource verbatim, such as a sharing
  or MIG resource. The NetworkPolicy rule for controller-to-node traffic now
  opens the node's gRPC port (`node.grpcPort`, 50052) instead of a fixed 50051.
  See the [GPU scheduling guide](docs/development/gpu-scheduling.md) and
  [ADR-1547](docs/adr/1547-helm-gpu-resource-name.md).


- **`adm_hip` is bit-identical to the CPU `adm` extractor, and no longer
  returns garbage for the first frame of a second context.** The HIP twin
  rounded the ADM denominator once per thread where the CPU rounds once per
  row, derived a rounding shift on the device with an fp32 logarithm, and
  concluded each scale with host copies of the CPU's routines. On a gfx1036 a
  low-detail 576x324 frame was 4.0e-7 off in `integer_adm_scale3`, BBB
  3840x2160 up to 1.4e-7 in `integer_adm_scale0`, and a 962x13542 frame scored
  `integer_adm_scale0` 0.860 where the CPU scores 0.979. The twin now takes
  its CSF weights, border, shifts and score conclusion from the CPU's own
  routines and folds the denominator once per row: 21 fixture pairs from
  18x22 to 3840x2160 at 8 to 16 bits are identical at `--precision max`, the
  per-scale sums of `debug=true` included, with every option set tried.
  Separately, the twin cleared its accumulators ahead of each frame's upload,
  and on that device such a clear is lost in the first context of a process
  that needs larger planes than the contexts before it: a program scoring a
  256x144 clip and then a 3840x2160 clip through the library got `invalid ADM
  reduction` on the second. The clear now follows the upload. The parity gate compares the CPU and HIP `adm` cells
  with tolerance 0. Re-run any stored `adm_hip` output
  ([ADR-1423](docs/adr/1423-hip-adm-cpu-row-rounding.md),
  [HIP backend](docs/backends/hip/overview.md#adm_hip-returns-the-cpus-values-bit-for-bit-2026-10-01)).


- **`ciede_hip` follows the CPU extractor's arithmetic** (ADR-1448). The HIP
  twin computed CIEDE2000 in single precision with another form of the
  formula and added per block: it matched `ciede` on no frame and was up to
  1.1e-5 from it. It now runs the CPU's statements on pairs of `float`
  values, from the headers the SYCL twin uses
  (`core/src/feature/ciede_ff_math.h`, `core/src/feature/ff_math.h`, moved
  out of `core/src/feature/sycl/` unchanged), and the host adds the
  per-pixel values in the CPU's order. Measured on a gfx1036 at
  `--precision max`: 115 of 178 frames identical and the rest within
  1.4e-11. What remains is glibc's `powf`, which is not correctly rounded
  (2 206 of 437 million pixels), and the last bits of a pair (8 pixels); the
  parity gate bounds the cell at `1e-9`. Stored `ciede_hip` scores change by
  up to 1.1e-5. A frame takes 2.7 times as long (49.6 ms at 1920x1080, 210 ms
  at 3840x2160). `ciede_sycl` returns the same bits as before on an Arc A380.


- **`float_moment_hip` and `vif_hip` no longer return a wrong first frame in
  a later, larger context of one process.** Both cleared their device
  accumulators ahead of the frame's plane upload. On a gfx1036 such a clear
  has no effect in the first context of a process that needs larger planes
  than the contexts before it, so the frame's sums were added onto the sums
  the earlier context had left in recycled device memory: after a 640x360
  context, the first frame of a 3840x2160 context had `float_moment_ref1st`
  130.53 where the CPU has 127.00, and `vif_hip` scale 0 at 0.6748 where the
  CPU has 0.6934. Every HIP extractor now uploads, then clears, then launches
  its kernels (`float_psnr_hip` is reordered too; its scores were right).
  The `vmaf` tool creates one context per process and was not affected;
  programs that score several clips through the library were. A device test
  per extractor and a source check over every HIP file hold the order
  ([ADR-1427](docs/adr/1427-hip-clear-after-upload.md),
  [HIP backend](docs/backends/hip/overview.md#a-frame-clears-its-accumulators-after-its-upload-adr-1427)).


- **`float_adm_hip` returns the CPU extractor's scores bit for bit**
  (ADR-1458). The HIP twin computed float ADM with eight differences from the
  CPU (the association of the angle test, partial sums per wave added in
  `double`, its own CSF weights, single-precision constants and gain limit,
  another order in the masking threshold, another floor) and matched it on
  224 of 1246 measured values, up to 1.3e-5 away. It now runs the CUDA twin's
  arithmetic from a shared header (`core/src/feature/float_adm_gpu_common.h`)
  and the CPU's own routines on the host. Measured on a gfx1036 at
  `--precision max`: 1246 of 1246 values identical, 3204 of 3204 with
  `debug=true`, and with five option sets. Stored `float_adm_hip` scores
  change by up to 1.3e-5. New: the option `adm_skip_aim_scale`, and frames
  below 17x17 are refused as on the CPU. No cost in frame time. The parity
  gate compares the cell at tolerance 0.


- **`float_moment_hip` is bit-identical to the CPU `float_moment` extractor
  at 16 bits.** The CPU forms each sample's square in `float` before adding
  it, which at 16 bits is the square rounded to 24 bits; the HIP twin added
  exact integer squares. Its second moments (`float_moment_ref2nd`,
  `float_moment_dis2nd`) were up to 1.0e-4 from the CPU's on 16-bit content
  with real low bits (0 of 77 such frames identical on a gfx1036), while 8-,
  10- and 12-bit input and the first moments were identical. The 16-bit
  kernel now adds the CPU's float square, and all four outputs are identical
  on 250 of 250 measured frames at `--precision max`. The parity gate compares
  the CPU and HIP `float_moment` cells with tolerance 0. One range stays
  within a derived bound instead: on a 16-bit frame of more than 2 097 152
  pixels whose sum of squares passes 2^53 the CPU's own sum rounds as it goes
  (2.7e-7 measured, 2.3e-5 at most at 3840x2160). No measurable cost. Stored
  16-bit `float_moment_hip` second moments change by up to 1.0e-4
  ([ADR-1447](docs/adr/1447-hip-float-moment-cpu-float-squares.md),
  [HIP backend](docs/backends/hip/overview.md#float_moment_hip-returns-the-cpus-moments-bit-for-bit-2026-10-02)).


- **`float_motion` on the HIP backend is bit-identical to the CPU, with every
  option.** The CPU extractor adds the absolute differences of a row into one
  `float`, the row sums into a second one, and divides in `float`; those
  running sums round at every step, so the score depends on the order of the
  additions. `float_motion_hip` added 16x16 blocks on the device and the
  blocks in `double` on the host, and was 3e-6 from the CPU on the Netflix
  576x324 pair, 1.4e-4 on 1080p checkerboards (above the 5e-5 cross-backend
  tolerance) and 2.2e-4 with `motion_add_scale1`. It now stores every absolute
  difference and adds each row in the CPU's order on the device, for the
  half-size term of `motion_add_scale1` and the chroma planes of
  `motion_add_uv` too, and `motion`, `motion2` and `motion3` equal
  `--backend cpu` at `--precision max` on a gfx1036: the Netflix pair at 8 and
  10 bits, both 1080p checkerboard pairs and BBB 3840x2160, with seven option
  sets (1617 of 1617 values; 237 before). The parity gate compares the CPU and
  HIP `float_motion` cells with tolerance 0. A 3840x2160 frame takes 19.8
  instead of 18.1 ms on that device, and 24.7 instead of 21.0 with
  `motion_add_scale1`. Re-run any stored HIP `float_motion` output
  ([ADR-1419](docs/adr/1419-hip-float-motion-cpu-float-sum.md),
  [HIP backend](docs/backends/hip/overview.md#float_motion_hip-options)).


- **`float_ms_ssim` on the HIP backend is bit-identical to the CPU.**
  `integer_ms_ssim_hip` accumulated its decimation and its Gaussian window
  sums as fp32 running sums, divided fp64 numerators by fp64 denominators and
  combined unrounded per-scale means, where the CPU extractor fuses each
  decimation tap, adds fp32 products in fp64, divides by fp32 denominators and
  combines fp32 means. On a gfx1036 no frame matched the CPU: the score was up
  to 3.0e-6 off and a per-scale mean up to 2.1e-5. The kernels now follow the
  CPU extractor operation for operation, as the CUDA twin does since ADR-1403,
  and every value of every frame is the CPU's at `--precision max`: the
  Netflix pair at 8 and 10 bits, both 1080p checkerboard pairs and BBB
  3840x2160, `enable_lcs`, `enable_db` and `clip_db` outputs included. The
  exact window sums cost time on that device: about 37 instead of 30 ms per
  1920x1080 frame and 169 instead of 158 ms per 3840x2160 frame. Re-run
  any stored HIP `float_ms_ssim` output
  ([HIP backend](docs/backends/hip/overview.md#integer_ms_ssim_hip)).


- **`float_ms_ssim` on HIP adds the windows of every scale in the CPU's
  order, so the scores are the CPU's on every frame.** The CPU extractor adds
  the luminance, contrast and structure terms of every window of a scale into
  one `double` each, left to right and top to bottom, and rounds each mean to
  `float`. The HIP twin (`integer_ms_ssim_hip`) computed the same terms
  (ADR-1403) and added them per wave and per 16x8 block. The sums differ in
  their last bits, which the rounding to `float` hides except on a mean next
  to a rounding boundary: on one constructed 176x176 noise frame the twin
  returned `float_ms_ssim_c_scale1` = 0.9854983687400818 where the CPU
  returns 0.9854984283447266, and `float_ms_ssim` differed by 1.3e-9. The
  twin now stores the three terms of every window and the host adds each
  scale in the CPU's order. On a gfx1036 at `--precision max` that frame
  returns the CPU's value on all 16 outputs, and 178 of 178 frames from
  480x270 to 3840x2160 at 8 to 16 bits stay identical, 2848 of 2848 values
  with `enable_lcs=true`. Cost on the gfx1036, medians of 32 interleaved
  pairs of runs: 37.3 to 42.7 ms per 1920x1080 frame and 183.1 to 200.8 ms
  per 3840x2160 frame, and 24 bytes of device and pinned host memory per
  window (65 MB at 1920x1080, 262 MB at 3840x2160)
  ([HIP backend](docs/backends/hip/overview.md#the-per-scale-sums-are-added-in-the-cpus-order-2026-10-02),
  construction of [ADR-1438](docs/adr/1438-hip-ssim-cpu-frame-sum.md)).


- **`float_psnr_hip` is bit-identical to the CPU `float_psnr` extractor at
  every bit depth.** The CPU adds the squared sample differences in `double`,
  which is exact. The HIP twin added each 16x16 block in single precision,
  which is exact at 8 bits and rounds at 10, 12 and 16 bits once the
  differences in a block are large. On real clips the two agreed; on
  full-range noise the twin was 6.3e-9 dB off at 10 bits, 2.5e-8 at 12 and
  1.8e-8 at 16, and 7.6e-8 on a bright 16-bit 1080p pair. The twin now adds
  the same squares as integers: 178 of 178 measured frames from 480x270 to
  3840x2160 at 8 to 16 bits are identical at `--precision max` on a gfx1036
  (167 before), with `uncapped=true` too, at the same time per frame. The
  parity gate compares the CPU and HIP `float_psnr` cells with tolerance 0.
  Stored `float_psnr_hip` scores of high-bit-depth clips with heavy
  distortion change by up to 7.6e-8 dB
  ([ADR-1440](docs/adr/1440-hip-float-psnr-exact-block-sums.md),
  [PSNR](docs/metrics/psnr.md#float_psnr)).


- **`float_ssim_hip` adds the frame's windows in the CPU's order, so the
  score is the CPU's on every frame.** The CPU `float_ssim` extractor adds
  the term of every window into one `double`, left to right and top to
  bottom, and rounds the mean to `float`. The HIP twin computed the same
  terms (ADR-1441) and added them per 16x16 block. The two sums differ in
  their last bits, which the rounding to `float` hides except on a mean next
  to a rounding boundary: on one constructed 64x64 frame the twin returned
  -4.222829659283889e-07 where the CPU returns -4.222829943500983e-07, one
  `float` step apart. The twin now stores the term of every window and the
  host adds the plane in the CPU's order, for `float_ssim` and for
  `float_ssim_l`, `float_ssim_c` and `float_ssim_s` under `enable_lcs`, at
  every `scale`. On a gfx1036 at `--precision max` that frame returns the
  CPU's bits, and 178 of 178 frames from 480x270 to 3840x2160 at 8 to 16
  bits stay identical, 712 of 712 values with `enable_lcs=true`, at `scale=1`
  and `scale=3` too. Cost: none measurable at the automatic scale (2.31 to
  2.45 ms per 1920x1080 frame and 5.86 to 5.86 ms per 3840x2160 frame, inside
  the spread between runs); with an explicit `scale=1`, 2 to 5 ms more per
  1920x1080 frame (19.6 to 22.0 ms; 22.7 to 27.7 ms with `enable_lcs=true`)
  and 8 bytes of device and pinned host memory per window and sum
  ([HIP backend](docs/backends/hip/overview.md#the-frame-sum-is-added-in-the-cpus-order-2026-10-02),
  construction of [ADR-1438](docs/adr/1438-hip-ssim-cpu-frame-sum.md)).


- **`float_ssim_hip` is bit-identical to the CPU `float_ssim` extractor.**
  The CPU adds the eleven products of a Gaussian window in `double` and
  rounds once per pass. The HIP twin added them in single precision, which
  rounds at every tap: on a gfx1036, 27 of 178 measured frames had the CPU's
  score and the others were up to 4.8e-7 away (5.4e-7 in the `enable_lcs`
  contrast and structure means). The twin now forms its window sums and its
  luminance, contrast and structure terms through the arithmetic
  `float_ms_ssim_hip` already shares with the CPU: all 178 frames from
  480x270 to 3840x2160 at 8 to 16 bits are identical at `--precision max`,
  with `enable_lcs`, `scale`, `enable_db` and `clip_db` too. The parity gate
  compares the CPU and HIP `float_ssim` cells with tolerance 0. It costs
  time: 2.0 ms instead of 1.7 ms per 1920x1080 frame and 5.2 ms instead of
  4.9 ms per 3840x2160 frame at the default scale, and a third more with
  `scale=1` (23.4 ms instead of 17.7 ms at 1080p). Stored `float_ssim_hip`
  scores change by up to 4.8e-7
  ([ADR-1441](docs/adr/1441-hip-float-ssim-cpu-window-sums.md),
  [HIP backend](docs/backends/hip/overview.md#float_ssim_hip-at-1080p-and-4k)).


- **`float_vif_hip` is bit-identical to the CPU `float_vif` extractor, and
  no longer faults on small frames.** The HIP twin filtered with a table of
  Gaussian taps the CPU stopped using, called the device `log2f()` where the
  CPU evaluates a polynomial, took the noise variance as a `float` where the
  CPU keeps a `double`, and added per wave and per block where the CPU adds
  row by row. On a gfx1036 10 of 712 scores (four scales, 178 frames from
  480x270 to 3840x2160 at 8 to 16 bits) were the CPU's; the others were up to
  3.8e-5 away on typical content and 1.06e-4 on bright 16-bit content, more
  than the twin's 5e-5 gate tolerance. The twin now runs the arithmetic of
  the CUDA twin from one shared header (`float_vif_gpu_common.h`) and all 712
  scores are identical at `--precision max`, with `debug=true` and the
  feature options too. It gains the CPU's `vif_scale1_min_val`,
  `vif_scale2_min_val` and `vif_scale3_min_val`. Frames smaller than 72
  pixels in either dimension, which ended with a GPU memory fault, now run.
  The parity gate compares the CPU and HIP `float_vif` cells with tolerance
  0. A frame takes 26.0 ms instead of 20.7 at 1920x1080 and 147 ms instead of
  86 at 3840x2160 on that device. Stored `float_vif_hip` scores change by up
  to 3.8e-5 ([ADR-1444](docs/adr/1444-hip-float-vif-cpu-arithmetic.md),
  [HIP backend](docs/backends/hip/overview.md#float_vif_hip-returns-the-cpus-scores-bit-for-bit-2026-10-02)).


- **`integer_ssim_hip` scores frames of up to 4096 pixels exactly as the CPU
  `ssim` does (ADR-1400).** The CPU adds one term per pixel in raster order,
  and on an identical frame the result is 1 or an ulp or two below it,
  depending on the frame: with `enable_db` an identical 1x1 frame of zeros
  reports 156.54 dB and a flat 3x3 frame of 51 reports 159.55 dB. The HIP twin
  reduced per block and reported `+inf` for every identical frame. For frames
  of at most 64x64 pixels the device now writes one term per pixel and the
  host adds them in the CPU's order, so the score equals the CPU's bit for
  bit at 8, 10, 12 and 16 bits, identical frames or not. Larger frames are
  unchanged. Verified on a gfx1036 by `test_hip_ssim_tiny_frames`.


- **HIP twins stay inside their buffers on small frames, and `vif_hip` hands
  frames below 16 pixels to the CPU (ADR-1381).** The HIP motion kernel's tile
  loads and the integer ADM scale-0 vertical DWT reflect an index once, which
  leaves the plane for the padding threads of a plane smaller than the tile
  (the defect that faulted the SYCL twins); both now clamp the reflected row
  into the plane, which changes no score of any accepted frame.
  `float_motion_hip` had the same single reflection and read before its input
  plane on 3x3 to 9x9 and 17x17 frames; its tile loads clamp the same way.
  `vif_hip` scored frames below 16 pixels from other samples than the CPU (its filters
  need 16 pixels at every scale); model dispatch now computes those frames
  with the CPU `vif`, and `--feature vif_hip` below 16x16 fails at init. The
  device tests pass on a gfx1036 with no GPU memory fault; see
  [the HIP backend guide](docs/backends/hip/overview.md#measured-on-a-gfx1036-2026-10-01).


- **`motion_hip` now computes the CPU `motion` arithmetic (ADR-1377).** The
  HIP twin blurred each frame and differenced the blurred frames, while the
  CPU (since the upstream pipelined-motion port) blurs the frame difference
  and rounds after each filter pass; the two orders round differently, so
  `motion2` / `motion3` were up to 1.26e-5 off on the Netflix 576x324 pair on
  a gfx1036. `motion_hip` and `motion_v2_hip` now run one diff-first kernel,
  the one `motion_v2_hip` already used, and are expected to match
  `--backend cpu` bit for bit; the debug `motion` score now carries
  `motion_fps_weight` and `motion_max_val` like the CPU's, and a one-frame
  run reports `motion3 = 0`. Both motion twins copy the reference luma into
  pinned memory and upload it without a host wait in `submit()`. Measured on
  a gfx1036: `motion2` / `motion3` identical to `--backend cpu` on every
  frame (1.26e-5 apart before); at 4K `motion_hip` takes 12.95 ms per frame
  (14.25 before) and `motion_v2_hip` 13.24 (10.17 before), the staged upload
  costing more than the wait it removes on that iGPU; see
  [the HIP backend guide](docs/backends/hip/overview.md#measured-on-a-gfx1036-2026-10-01).


- **`motion_hip` and `float_motion_hip` no longer crash with
  `motion_force_zero=true`.** Both HIP twins switched to their synchronous
  zero path inside `init()` and cleared `submit()` / `collect()`, but libvmaf
  had already chosen the asynchronous path for them, so the first frame
  called a NULL `submit()` and the process died with SIGSEGV. The twins now
  keep the asynchronous interface and write the CPU's zeros from
  `collect()`. Measured on a gfx1036: `--feature motion_hip=motion_force_zero=true`
  exits 0 with every `integer_motion*_force_0` score 0, as on the CPU
  (`T-HIP-MOTION-FORCE-ZERO-NULL-SUBMIT-2026-09-30` in `docs/state.md`).


- **`test_hip_smoke` passes on a host without an AMD GPU again.** Its context
  case still expected `vmaf_hip_context_new()` to succeed without a device;
  since the context selects its device first, the function returns `-ENODEV`
  there, and the case now checks that contract (and a populated context when
  a device is present).


- **`speed_chroma_hip` and `speed_temporal_hip` match the CPU with
  `speed_prescale_method=lanczos4`.** Like the CUDA and SYCL twins before
  them, they evaluated the lanczos4 kernel weights on the device in fp32,
  where the CPU scaler uses fp64 `sin()`, and SpEED amplifies the few-ulp
  differences on smooth content: on a gfx1036, `speed_chroma_hip` at
  `speed_prescale=0.5` was 8.8e-3 relative away from the CPU on a smooth
  1920x1080 field, and `speed_temporal_hip` 0.24 on a 1080p checkerboard pair
  at 2.0. The scale kernel now reads the weights from the table the host
  builds with the CPU scaler's own routine, and lanczos4 at 0.5 and 2.0 is
  bit-identical to the CPU extractor on that device (given a correctly rounded
  `log2f` on the CPU side, as for the other prescale methods). It is also
  faster: `speed_chroma_hip` with lanczos4 at 0.5 goes from 21.8 to 16.0 ms
  per 3840x2160 frame. Scores with the other three methods, and without
  prescale, are unchanged. This closes the lanczos4 prescale drift on all
  three device backends
  ([SpEED](docs/metrics/speed_qa.md#hip-device-resident-cpu-fp32-arithmetic)).


- **`integer_ssim_hip` is bit-identical to the CPU `ssim` extractor at every
  frame size.** The CPU adds the SSIM term of every pixel into one `double`,
  left to right and top to bottom. The HIP twin computed the same terms and
  added them in that order only for frames of at most 4096 pixels; above
  that it added them per 16x8 block, and on a gfx1036 the score of 1 of 178
  measured frames was the CPU's, the others up to 1.1e-11 away. The twin now
  stores every term and the host adds the plane in the
  CPU's order: 178 of 178 frames from 480x270 to 3840x2160 at 8 to 16 bits
  are identical at `--precision max`, with `enable_db` and `clip_db` too, and
  an identical frame reports the CPU's value at every size. The parity gate
  compares the CPU and HIP `ssim` cells with tolerance 0. Cost on the
  gfx1036: 30.0 ms instead of 28.2 ms per 1920x1080 frame and 98.1 ms instead
  of 94.3 ms per 3840x2160 frame, and 66 MB more device and pinned host
  memory at 3840x2160. Stored `integer_ssim_hip` scores change by up to
  1.1e-11 ([ADR-1438](docs/adr/1438-hip-ssim-cpu-frame-sum.md),
  [HIP backend](docs/backends/hip/overview.md#integer_ssim_hip)).


- **`ssimulacra2_hip` is bit-identical to the CPU `ssimulacra2` extractor.**
  The CPU evaluates six terms per pixel and channel in `double` and adds each
  into one `double`, pixel after pixel. The HIP twin evaluated the terms as
  pairs of floats and added them in a fixed tree: on a gfx1036 none of 178
  measured frames (480x270 to 3840x2160, 8 to 16 bits) equalled the CPU, and
  the score was up to 7.6e-11 away. The twin now evaluates the CPU's double
  expressions and forms the sums with the bits of the CPU's loops, from
  integer increments per binade as the CUDA twin does (`ordered_sum.h`). All
  178 frames are identical at `--precision max`, with every `yuv_matrix`. The
  parity gate compares the CPU and HIP `ssimulacra2` cells with tolerance 0
  instead of 5e-3. The twin is slower: 167 ms instead of 58 per 1920x1080
  frame and 662 ms instead of 234 per 3840x2160 frame on that device. Stored
  `ssimulacra2_hip` scores change by up to 7.6e-11
  ([ADR-1445](docs/adr/1445-hip-ssimulacra2-cpu-sum-order.md),
  [ssimulacra2](docs/metrics/ssimulacra2.md#hip-device-resident-tiled-row-pass)).


- **Every HIP twin runs on the device `--hip_device` (or
  `VmafHipConfiguration.device_index`) names, and a broken HIP runtime is
  reported as an error.** `vmaf_hip_context_new()` stored its device index
  and selected nothing, and every HIP twin passed it a literal 0, so a twin
  ran on whatever device its thread had: a library caller scoring on another
  thread than the one that created the state got device 0. The twins now
  create their contexts on the imported state's device, libvmaf rebinds that
  device before each frame and the flush, and an index the runtime does not
  have is refused with `-EINVAL`. `vmaf_hip_device_count()` returned 0 when
  `hipGetDeviceCount()` failed; it now returns a negative errno (0 only when
  the runtime reports no device), and `vmaf_hip_list_devices()` and
  `vmaf_hip_state_init()` pass the error on instead of 0 or `-ENODEV`
  ([ADR-1523](docs/adr/1523-hip-twins-run-on-the-state-device.md)).


- **`vif_hip` is bit-identical to the CPU `vif` extractor.** The fixed-point
  VIF statistic takes every per-pixel logarithm from a 32768-entry table the
  CPU extractor fills with the host math library. The HIP twin evaluated
  `log2f()` on the device instead, which is one ulp from glibc's for about
  half of the arguments and rounded ties to even where the CPU rounds them
  away from zero: 77 entries were one lower, and on a gfx1036 only 49 of 440
  scores (four scales, 110 frames from 480x270 to 3840x2160) were the CPU's,
  the others up to 5.4e-7 away. The twin now uploads the CPU's table and
  looks every logarithm up; all 440 scores are identical at `--precision max`,
  and so are the numerator and denominator sums of `debug=true`, 12- and
  16-bit and 4:2:2 input, `vif_enhn_gain_limit=1.0` and `vif_skip_scale0`.
  The table has one definition, `vif_log2_table_generate()` in
  `integer_vif.h`. The parity gate compares the CPU and HIP `vif` cells with
  tolerance 0. Stored `vif_hip` scores change by up to 5.4e-7
  ([ADR-1435](docs/adr/1435-hip-vif-cpu-log2-table.md),
  [HIP backend](docs/backends/hip/overview.md#vif_hip-returns-the-cpus-scores-bit-for-bit-2026-10-01)).


- Local hooks: a commit in a linked worktree no longer risks rewriting that worktree's index when the pre-commit framework installs a node hook environment. `lefthook.yml` now runs `pre-commit install-hooks` with the commit's git variables unset before `pre-commit run` and `hook-impl`.


- **`test_icx_system_libm` passes on Windows.** Its glibc-probe cases patched
  `os.confstr`, which Windows' `os` module does not have, and the patch raised
  before the case ran (4 errors on the UCRT64 leg). The patches may now create
  the attribute, and a new case runs them all without it.


- **An Intel-compiler build returns a GCC build's CPU scores.** A build with
  `icx` / `icpx` (every SYCL build, the dev image's `vmaf`) linked Intel's math
  library `libimf`: the driver turns a `-lm` into `-limf -lm`, and the
  icx-built `vmaf` carried a static copy of it that `libvmaf.so`'s calls to
  `log10`, `pow`, `powf`, `log2f`, `exp`, `log` and 24 other math functions
  bound to. `psnr`, `psnr_hvs`, `ciede`, integer `adm`, `float_adm` (with a CSF
  weight override) and the default model's `vmaf` differed from a GCC build in
  the last digits (268 of 13288 values at `--precision max`, up to 1.3e-7 in
  `vmaf`). Every icx / icpx link now gets `-no-intel-lib=libimf`, so the math
  functions come from glibc's `libm`: 13288 of 13288 values are identical, and
  the twins of a SYCL build equal a GCC build's CPU extractor as well as their
  own build's
  ([ADR-1495](docs/adr/1495-icx-system-libm.md)). `ciede` in an icx build runs
  43 % faster (95.6 to 54.3 ms per 1920x1080 frame on four threads).
  `test_icx_system_libm` reads the build's `libvmaf.so` and `vmaf` and fails if
  a math call resolves to `libimf`; it skips on GCC and clang builds. Windows
  `icx-cl` builds are not covered yet.


- **Integer `adm` in Barten mode (`adm_csf_mode=1`) no longer wraps on
  high-contrast pictures (ADR-1472).** The contrast-masking reduction squares
  each weighted wavelet coefficient and narrows the square to 32 bits; with
  the weight limit of 2^30 that ADR-1325 chose for scales 1 to 3 the square,
  and on some pictures the weighted coefficient itself, could leave 32 bits.
  `vmaf --feature adm=adm_csf_mode=1` failed every frame of the 10 px
  checkerboard (`aim_num=-nan`) and returned `integer_adm2` 0.587 instead of
  0.784 on the 1 px checkerboard without any message. The weight limits now
  follow from the largest coefficient the wavelet can produce at each scale
  (`adm_csf_fixed_limit()`), so no picture can wrap. Barten-mode scores that
  were already right move by at most 7.7e-7 on the Netflix pair; Watson97 and
  the two blend modes are bit-identical. The GPU twins take the weights from
  the same header and follow without a kernel change (CUDA, HIP and SYCL
  measured: every `adm` output equals the CPU's).


- **Integer ADM and the models that read it return Netflix's values again;
  `vmaf` moves by up to 2e-5.** The Watson quantisation step of integer ADM
  raises 10 to `k * temp * temp`. Netflix multiplies the three `float`s in
  `float`; a static-analysis sweep (#552) had widened the product to
  `double`, which moved the CSF weights by one to three units in the last
  place and with them every integer ADM score. The fork goes back to
  Netflix's expression
  ([ADR-1475](docs/adr/1475-integer-adm-quant-step-upstream-float.md)).
  Measured against Netflix master `cea2b4d8` on 31 clips at `%.17g`:
  `vmaf_v0.6.1` is identical on 504 of 504 frames (32 before, up to 1.83e-5
  apart), as are `vmaf_v0.6.1neg`, `vmaf_4k_v0.6.1` and the bootstrap model
  `vmaf_b_v0.6.3`; `integer_adm2` is identical on every decoded picture. What
  you see: `integer_adm2`, `integer_adm3`, the per-scale ADM scores and the
  `vmaf` of those models move in the fifth or sixth decimal on some frames
  (59 of 720 values of the 576x324 snapshot, at most 2e-5), on the CPU and
  on the CUDA, SYCL, HIP and Metal twins alike. The Netflix golden gate
  passes unchanged. The `vmaf_v1.0.16` and float models are not affected by
  this change.


- **Integer ADM scores pictures whose contrast-masking row passes INT64_MAX.**
  The scale-0 masking reduction summed each row of non-negative cubes in a
  signed 64-bit integer. A 64-pixel-wide picture with full-range detail and no
  distortion took one row past INT64_MAX at the default settings, and any
  width did with a CSF weight near its limit: the sum wrapped and the frame
  failed with `invalid ADM reduction`. The CPU extractor, its AVX2 / AVX-512
  paths and the CUDA, HIP, SYCL and Metal twins sum the scale-0 rows unsigned
  now; scores that did not overflow are unchanged.


- **Integer ADM halves a scale-0 CSF weight from 43900 instead of 46603.**
  The CSF stage keeps the 1/30 magnitude of the weighted band in 16 bits,
  which wrapped negative (scalar code, AVX-512, CUDA, HIP, SYCL, Metal) or
  saturated (AVX2) for horizontal or vertical weights between 43900 and the
  old limit, and a masking row of a 31-32 pixel wide picture could pass 2^64
  from about 45200. Such a weight now takes one more halving on every backend
  ([ADR-1917](docs/adr/1917-integer-adm-scale0-weight-limit-csf-magnitude.md)).
  Watson97, the default Barten configuration and both blend modes are
  unchanged; a Barten configuration in that range (for example
  `adm_csf_scale=1.16:adm_csf_diag_scale=0.3`) moves by about 1e-6.


- **Integer VIF no longer converts an out-of-range `double` to `int32_t`
  ([ADR-1561](docs/adr/1561-integer-vif-sv-sq-defined.md)).** The residual
  variance of the gain model, `sigma2_sq - g * sigma12`, reaches about -2^45,
  and Netflix's code converted it to `int32_t` before clamping it at 0, which
  is undefined behaviour below INT32_MIN: x86 returns INT32_MIN (so 0 after
  the clamp), an aarch64 build that vectorises the loop keeps the low 32 bits.
  `vif_sv_sq()` returns x86's value on every target and the scalar statistic,
  the AVX2 and NEON helpers and the CUDA and HIP kernels call it. No score
  changes: the Netflix pair and both checkerboard pairs give the same values
  at every x86 dispatch level, and clang's `-fsanitize=undefined` no longer
  stops `test_integer_vif_sv_sq`.


- **Local hooks: the lefthook and pre-commit stack runs on Windows.** On Windows,
  lefthook passes each `run:` value to `sh -c` without escaping it, so the
  multi-line framework job ended at its first double quote and every commit
  failed in `framework-hooks`. The framework stages now run through
  `scripts/git-hooks/framework-hooks.sh`, which also finds a Windows virtualenv
  (`.venv/Scripts/pre-commit.exe`) and puts it on `PATH` for the checks. The
  pre-commit lock installs `reuse` with `charset-normalizer`, the only encoding
  detector reuse can use on Windows. Three checks that failed on Windows for
  path and line-ending reasons now pass there: the base-image local exceptions,
  the research-digest ID fixtures and `generate-adr-by-tag.sh --check`. The
  pre-push `govulncheck ./...` could not load `cmd/vmafx-node/bpf` outside
  Linux; that eBPF package now builds on Linux only, as ADR-0996 intended.
  `make install-hooks` now leaves lefthook's hooks in place, so the
  `commit-msg` and `pre-rebase` dispatchers install beside lefthook (ADR-2012).
  `.claude/settings.json` and `.codex/hooks.json` are stored in the form
  `lefthook uninstall` writes, so an uninstall no longer dirties them.
  [Local Git hooks](docs/development/pre-commit-hooks.md#windows-hosts)
  documents the Windows setup (`T-HOOKS-WINDOWS-LEFTHOOK-QUOTING-2026-09-30`).


- **The licence pages say what an embedder of VMAFx owes.** The README,
  `docs/licensing.md` and the GPU API page said the EUPL-1.2 source obligation
  applies to a *modified* `libvmaf`. Article 5 of the EUPL-1.2 applies to every
  copy you distribute, modified or not: provide the source or point to a
  repository that has it. The API page also listed `libvmaf_cuda.h` as
  EUPL-1.2; it is Netflix's BSD-2-Clause-Patent header. `docs/licensing.md`
  gains a section, *Embedding VMAFx in another product*. It covers
  distribution, static and dynamic linking, modification, network use and
  patents under both licences, quotes the EUPL text and the European
  Commission's FAQ with links, and says it is not legal advice. ADR-1250 (the
  EUPL relicence) and ADR-1199 (the CUDA picture handover barrier) are
  recorded as Accepted, since both are applied.


- **The process log level is safe to set and read from several threads.**
  `vmaf_init()` sets the level (and the stderr tty flag) that `vmaf_log()`
  reads on every thread, worker threads included; both were plain globals, so
  creating contexts on two threads, or one while another context logs, was a
  data race that ThreadSanitizer reports. They are atomic now; the level a
  caller sees and the log output are unchanged.


- **CI gates that were red on master read the right inputs again.** The Meson
  test-entry-point contract no longer governs the `.ci/pelorus` checkout of another
  repository, the documentation freshness check installs `jsonschema` from its hash
  lock, `Tooling Tests` checks out the full history that the retired-ADR record check
  needs and its default-model gate test deletes each scratch copy of the tree, and
  `Tidy Changed` leaves out three C++-only headers that the CPU build cannot parse
  (`T-CI-MESON-CONTRACT-PELORUS-CHECKOUT-2026-10-06`,
  `T-CI-DOCS-FRESHNESS-JSONSCHEMA-2026-10-06`,
  `T-CI-TOOLING-TESTS-SHALLOW-AND-DISK-2026-10-06`,
  `T-CI-TIDY-CHANGED-CXX-HEADERS-2026-10-06`).


- **`Cppcheck` is clean on the Metal host tests and shared math headers.** Seventeen
  findings are fixed (two by-value blocks carry a cited suppression because the
  headers are shared with Metal Shading Language, the rest are code changes); the six
  affected Metal host tests still pass (`T-CI-CPPCHECK-METAL-HOST-TESTS-2026-10-06`).


- **`go fix` and `cargo fmt` are clean on master.** Six Go files take the pinned
  toolchain's fixes (`maps.Copy`, `max`, `strings.SplitSeq`, `new(expr)`) and one
  Rust example is formatted; no behaviour changes
  (`T-CI-GO-FIX-RUST-FMT-2026-10-06`).


- **The `Go` workflow builds the node's eBPF object with the pinned clang again,
  and the Windows SYCL tester leg no longer fails on a locked installer.**
  `scripts/dev/gen-node-bpf.sh` now prefers `clang-19` (and `llvm-strip-19`) to a
  newer default `clang`, which the hosted runner ships. The oneAPI install step of
  `windows-tester-bundle.yml` checks the extractor's exit code and retries the
  removal of the installer a bounded number of times. No score, public API or
  FFmpeg patch impact.


- **The Metal integer ADM host files carry their provenance.**
  `integer_adm_metal_host.c` and `.h` now name the Netflix code they reproduce in part
  (`EUPL-1.2 AND BSD-2-Clause-Patent`), and `Licence Provenance` is clean again
  (`T-CI-LICENCE-PROVENANCE-METAL-ADM-HOST-2026-10-06`).


- **Seven Metal helper headers carry the licence of the Netflix code they reproduce, and the tester bundles are signed in a form Scorecard counts.**
  Five `core/src/feature/metal/metal_*_math.h` headers now list `Copyright 2016-... Netflix, Inc.` and
  `EUPL-1.2 AND BSD-2-Clause-Patent` like their CUDA, HIP and SYCL twins, and two are recorded as
  reproducing none of it ([ADR-1250](docs/adr/1250-eupl-fork-relicense.md),
  [ADR-1474](docs/adr/1474-relicense-helper-headers-and-ci-check.md)). The macOS and Windows tester
  workflows now write each Sigstore signature as `<asset>.sigstore.json` instead of `<asset>.bundle`;
  verify with `cosign verify-blob --bundle <asset>.sigstore.json`
  ([tester guide](docs/usage/tester-image.md)). Already published tester releases keep the old name.


- **The hosted `Tooling Tests` job installs its lock again, and the `vmaf-tune` suite no
  longer assumes a GPU host or a `vmaf` on `PATH`.** `reuse==6.2.0` has no wheel for
  Python 3.14, so pip builds it from source with `poetry-core`, which the locked build
  backend set lacked (`poetry-core==2.5.0` is now in `requirements/locks/package-build.in` and the
  locks that include it). The NVENC probe test needs a GPU the driver lists, and the two
  bisect cap tests stub the scoring step the way their docstring says.


- `list_backends`, `probe_backend` and `vmaf_version` of both MCP servers (Python
  and Go) now take a GPU backend as available only when `vmaf --list-backends`
  reports it usable. They read `vmaf --help` before, which names every backend
  on every build, so a CPU-only `vmaf` was reported with CUDA, SYCL, HIP and
  Metal and the backend allowlist admitted them. A `vmaf` that cannot print the
  report is treated as CPU-only.


- **The Go MCP server is checked against the Python server's own tool
  list.** Its parity tests compared it with 15 hand-copied tool names, so the
  four sidecar tools and every property type went unchecked. The Python server
  now writes `mcp-server/vmaf-mcp/tool-contract.json` from its tool list
  (`python3 -m vmaf_mcp.tool_contract --write`, checked by its test suite), and
  `vmafx-mcp`'s tests require every tool in it with the same properties, types
  and required arguments, and no undeclared extra tool. Both servers already
  agreed; no tool changes.


The Meson test secret-environment contract test reports repository paths with forward slashes on Windows, so it passes there as it does on Linux and macOS.


- **`--backend metal` reports CAMBI under the CPU's feature name, and the Metal
  twin writes heatmaps.** `integer_cambi_metal` built its feature names after it
  had written the resolved encode and source sizes into the `enc_width`,
  `enc_height`, `enc_bitdepth`, `src_width` and `src_height` option slots, so
  every name carried them: a 576x324 8-bit run reported
  `cambi_encbd_8_ench_324_encw_576_srch_324_srcw_576` instead of `cambi`, and a
  run of the default model `vmaf_v1.0.16_3d0h` on Metal stopped with "problem
  generating pooled VMAF score" because the model found no CAMBI score. The
  names are now built first, as the CPU extractor builds them. The twin also
  accepts `heatmaps_path`: it writes the per-scale `.gray` heatmaps with the CPU
  extractor's own writer, so the files equal those of `--backend cpu`. The CPU
  extractor's scores and heatmap files are unchanged; its close now reports
  `-EIO` when a heatmap file fails to close. Found by the Apple M4 Pro tester
  report (#2118); the Metal change is source only and waits for a device re-run
  (`T-METAL-CAMBI-SCORE-NAME-SUFFIXED-2026-10-05`,
  `T-BUG048-GPU-OPTION-PARITY-REMAINDER-2026-09-26`).


The oneAPI (icx) build links the Metal host tests again: the double comparison of the Metal headers uses the compiler's builtins instead of `<math.h>` macros that icx turns into calls into a library the build does not link.


- **The FFmpeg `libvmaf_metal` filter can score NV12 and P010 VideoToolbox
  frames.** It imported only the luma plane, and libvmaf refused every frame
  with `-EINVAL`, so the filter failed on its first frame. Importing the
  chroma planes would not have helped: libvmaf copied the interleaved CbCr
  plane as if it were the Cb plane, and copied P010 samples without shifting
  them, which scores them 64 times too large. The import now reads the
  surface's pixel format. It splits NV12 and P010 chroma into Cb and Cr and
  shifts P010 to the low 10 bits. It refuses any other layout with
  `-ENOTSUP`. The filter imports all three planes of both frames. It refuses
  other software formats with an error that names the format, and fails
  instead of passing an unimportable frame through unscored
  ([ADR-1679](docs/adr/1679-metal-iosurface-biplanar-import.md)). The
  documented command now uses `-hwaccel_output_format videotoolbox_vld`;
  `videotoolbox` is not a pixel format name. Checked on Linux: host tests and
  source checks, plus a syntax check of the Objective-C++ and the filter
  against the macOS 11.3 SDK. Nothing has run on an Apple device yet. The
  macOS tester bundle runs `test_metal_iosurface_import_parity` for that.


- **The macOS Metal build compiles again.** `float_motion_metal.mm` opened an
  anonymous namespace it never closed, and every macOS Metal build stopped
  there. A new device-free test checks the braces and namespaces of every
  Metal host file on every platform.


- `test_metal_float_motion_parity.c` sizes its key buffers for the longest key it formats, so a
  gcc build no longer reports `-Wformat-truncation` for it.


- **`integer_adm_metal` returns the CPU's `adm` scores again
  ([ADR-1806](docs/adr/1806-metal-kernels-host-replay.md)).** The first
  report of the macOS tester bundle on an Apple M4 Pro (issue #2118) showed
  every `adm` output off on every frame, by up to 2.8. Six defects caused it:
  the reduction kernels wrote their sums twice as far apart as the host read
  them (losing one band and writing past the buffer), scale 1 read the 16-bit
  band of scale 0 as 32-bit samples, the scales-1-3 masking terms and
  denominator rounded differently from the CPU, `adm_skip_scale0` left an AIM
  numerator at scale 0, and the noise floor multiplied in single precision.
  The twin now shares one definition of its uniforms and reduction layout
  between kernels and host, reads the 16-bit band at scale 1, and takes every
  shift, rounding term and score formula from the CPU extractor's own code. A
  new host test, `test_metal_integer_adm_host_replay`, runs the unmodified
  Metal kernels on Linux and macOS through a shim and compares every output
  with the CPU at `==`; each of the six defects makes it fail. The fix is not
  yet measured on an Apple GPU.


- **The Linux self-test of the Metal IOSurface import no longer leaks its
  fixtures.** `test_metal_selftest_iosurface_import` kept the planar pictures
  it imported from, and the AddressSanitizer job aborted on the leak report;
  the self-test now releases them as the device build does.


- **The Metal `float_vif`, integer SSIM, `float_ssim` and `float_ms_ssim`
  twins refuse a frame whose moment planes pass their 32-bit index.** They
  index five planes of N samples in `uint`, which wraps from N = 858,993,460
  (past 16K, or 16K with `vif_prescale` above about 2.544); the planes past the
  wrap aliased the start of the buffer. `init()` now fails with `-EINVAL` and
  names the limit; 16K at the default options is accepted as before
  ([Metal backend](docs/backends/metal/index.md#largest-frame-of-the-five-plane-twins)).


- **`integer_psnr_hvs_metal` computes the CPU's `psnr_hvs` scores.** The
  Metal twin added the 64 masked coefficient errors of each 8x8 block on the
  device and the host added the block sums, where the CPU extractor adds every
  error of a plane into one running `float`. It also formed the masking table
  as an fp32 product, which rounds 98 of its 192 entries differently from the
  CPU's `double` product. An outside tester's Apple M4 Pro (issue #2118)
  measured `psnr_hvs` up to 1.7e-3 dB from the CPU on the 1080p checkerboard
  pair and 191 of 192 values different on the Netflix 576x324 pair. The kernel
  now stores every error, and the host adds them in the CPU's order with the
  helpers the CUDA, HIP and SYCL twins use. The host forms the masking table in
  `double`, since Metal has no `double`
  ([ADR-1397](docs/adr/1397-psnr-hvs-twins-cpu-float-sum.md),
  [ADR-1498](docs/adr/1498-metal-twins-exact-designs.md)). Checked on Linux:
  the kernel's arithmetic, compiled as C, equals the CPU extractor on every
  output of the parity test's fixtures and the Netflix and checkerboard pairs.
  Nothing has run on an Apple device yet; the macOS tester bundle runs
  `test_metal_integer_psnr_hvs_parity` and the parity gate for that.


- **Metal: `ssimulacra2_metal` refuses 4:0:0 input instead of crashing
  (`T-METAL-SSIMULACRA2-YUV400-ACCEPTED-2026-10-05`).** The twin's `init()`
  discarded the pixel format, so a YUV400P picture reached the colour
  conversion, which read the missing U plane through a NULL pointer and killed
  the process. The Apple M4 Pro tester report of #2118 counted
  `test_metal_ssimulacra2_parity` failed for that reason while every case it
  printed passed. The twin now returns `-EINVAL` from `init()` with
  `ssimulacra2_metal: needs a YUV 4:2:0, 4:2:2 or 4:4:4 input, not 4:0:0`, as
  the CPU extractor and the CUDA, SYCL and HIP twins do. All five share one
  check (`core/src/feature/ssimulacra2_pixel_format.h`); the CUDA, SYCL and HIP
  refusals now end with `, not 4:0:0` like the CPU's. The `vmaf` CLI never
  produces 4:0:0 input; library callers of `vmaf_read_pictures()` were
  affected.


- **The Metal twins compute the CPU's scores by construction
  ([ADR-1498](docs/adr/1498-metal-twins-exact-designs.md)).** Each Metal
  extractor now runs the design that makes its CUDA, HIP or SYCL twin return
  the CPU's scores bit for bit, written in a header that also compiles on the
  host, where a test holds it against the CPU extractor. Changes a Metal user
  can see: `float_psnr_metal` and `float_moment_metal` are exact at 10 to 16
  bits, `float_moment_metal` also on 16-bit frames whose sum passes 2^53
  units; `integer_psnr_metal` no longer loses carries in its 64-bit error sum
  and takes `enable_apsnr`; `integer_motion_metal` differences frames before
  the blur, emits `motion_sad_score` and `motion3` and drops the
  `motion_add_uv` option the CPU never had; `motion_v2_metal` and
  `float_motion_metal` apply `motion_fps_weight` and `motion_max_val` per frame
  and take every CPU option; `integer_vif_metal` hands frames below 16 pixels
  to the CPU in a model run; `float_adm_metal` refuses frames below 17x17,
  floors its sums as the CPU and takes `adm_f1s0` to `adm_f2s3`;
  `float_vif_metal` runs every `vif_kernelscale`; `float_ssim_metal` gives an
  identical flat frame a finite `enable_db` score; `integer_cambi_metal` takes
  `src_width`, `src_height` and `full_ref`. No Apple device ran these builds
  yet: each twin's state row closes when a report of the macOS tester bundle
  shows its parity test passing. Guide: `docs/backends/metal/index.md`.


- **`integer_vif_metal` divides each scale's sums in single precision, as the
  CPU does.** The CPU `vif` extractor stores each scale's numerator and
  denominator in a `float` and returns their quotient in single precision.
  The Metal twin rounded the two sums the same way but divided them in
  `double`, so every `VMAF_integer_feature_vif_scale0..3_score` of every
  frame differed from the CPU's: an outside tester's Apple M4 Pro measured up
  to 3.0e-8 on the Netflix 576x324 pair (192 of 192 scores), 1.5e-8 on the
  1080p 1-pixel checkerboard pair and 2.7e-8 at 10 bits. The integer sums
  were already the CPU's; only the final division changes. Stored
  `integer_vif_metal` scale scores change by up to half a `float` step; the
  debug sums and the `integer_vif` frame ratio do not change.


- **The licence notices name the right holders of the bundled models.** The
  LPIPS-SqueezeNet weights are recorded as BSD-2-Clause (Zhang, Isola, Efros,
  Shechtman, Wang) on torchvision's BSD-3-Clause SqueezeNet features, the
  TransNet V2 weights as MIT (Tomáš Souček) and the FastDVDnet weights with their
  LICENSE's copyright line; before, `REUSE.toml` gave the first two the fork's
  licence and name, and attributed the fork-trained predictor and KoNViD models
  at the `model/` root to Netflix. Every published artifact's notices are
  computed from these records. The model cards and training metadata no longer
  call KoNViD-1k "CC BY 4.0": its database page names no licence and says it is
  "freely available to the research community"
  ([licensing](docs/licensing.md#models)).


- **A model collection's score can be read more than once.**
  `vmaf_score_at_index_model_collection()` failed with `-EINVAL` when the
  frame had been scored before, and so did
  `vmaf_score_pooled_model_collection()` over a range holding a frame already
  scored per frame (the collector refused to write the members' scores of
  that frame a second time, `feature "..." cannot be overwritten`). A frame
  already predicted now returns its stored bootstrap scores, as a single
  model's `vmaf_score_at_index()` does; the values are the first
  prediction's, bit for bit.


- **The tiny-model registry validator no longer validates less when `jsonschema` is
  missing.** `ai/scripts/validate_model_registry.py` used to fall back to a
  four-field structural check and print `OK`; it now exits 2 and names the
  install command (`pip install --require-hashes -r requirements/locks/jsonschema.txt`).
  `jsonschema` was already a declared dependency and the CI job installs it, so
  a run that passed before still passes.


- **`float_ms_ssim` with `enable_chroma` runs on CUDA and HIP, and HIP no
  longer drops the chroma scores.** The HIP twin `integer_ms_ssim_hip`
  accepted `enable_chroma=true`, scored luma only and wrote neither
  `float_ms_ssim_cb` nor `float_ms_ssim_cr`, without a warning: a HIP run
  that set the option has no chroma scores. The CUDA twin had no such option,
  so such a request ran on the CPU. Both twins now run the luma pipeline once
  per plane and return the CPU extractor's three scores bit for bit
  (`--precision max`, RTX 4090 and gfx1036: 1080p checkerboards, Netflix
  576x324 at 4:2:2 10 bit and 4:4:4 8 and 10 bit, BBB 1080p 4:4:4 and BBB 4K
  4:2:0, with `enable_lcs`, `enable_db` and `clip_db` too), and refuse, as
  the CPU does, a frame whose chroma is smaller than 176 pixels. The parity
  gate has a `float_ms_ssim_chroma` cell, exact on CUDA, HIP and SYCL. See
  [MS-SSIM](docs/metrics/ms-ssim.md).


- **The Windows MSVC builds compile `blur_array.c` again.** A lint cleanup on
  2026-10-02 had replaced `NULL` with the C23 keyword `nullptr` in that C file;
  MSVC's C mode does not know the keyword.


- **The Windows MSVC builds compile the SSIM, MS-SSIM, float VIF and motion
  sources again.** Lint cleanups on 2026-10-02 had replaced `NULL` with the C23
  keyword `nullptr` in seven C files; MSVC's C mode does not know the keyword.


- **The MSVC build's AVX2 and AVX-512 float ADM wavelet returns the scalar
  code's bits on signed zeros** (`T-MSVC-FLOAT-ADM-X86-TEST-FAILS-2026-10-04`).
  MSVC removes an intrinsic addition of +0 even under `/fp:precise`, so a
  sample whose four products were all -0 came out -0 on the vector path and
  +0 in `adm_dwt2_s()`, and `test_float_adm_x86` failed on the Windows tester
  zip. The kernels now form that first `+0 +` with a compare and a mask, which
  MSVC keeps. Picture data never reached the case, so no score changes. The
  `Windows MSVC+CUDA (full)` CI lane now runs `test_float_adm_x86`.


- **Mount mode works in the node image
  ([ADR-1593](docs/adr/1593-helm-node-fuse-and-ebpf.md)).** The published
  `vmafx-node` image had no FUSE helper, so a node with
  `VMAFX_STORAGE_MODE=mount` refused to start. The image now carries the
  setuid `fusermount3` and the util-linux `mount` and `umount` it runs, listed
  in the licence record with their Debian sources in the `-source` image. A
  container needs `/dev/fuse` and the capabilities `SYS_ADMIN` and
  `DAC_READ_SEARCH`; the node process itself keeps none.


- **vmafx-operator authenticates to the controller
  ([ADR-1569](docs/adr/1569-operator-controller-auth.md)).** The `VmafxJob`
  reconciler polled `GetJob` without a token, so with the controller's auth
  on every poll failed and no `VmafxJob` left `Pending`. It now reads the
  node's variables: `VMAFX_CONTROLLER_TOKEN_FILE` (read again on every poll,
  so a renewed token applies at once) or `VMAFX_CONTROLLER_TOKEN`, and
  `VMAFX_CONTROLLER_TLS` / `_CA_FILE` / `_SERVER_NAME`. Both programs refuse to
  send a JWT whose `exp` has passed and say which file holds it; a malformed
  combination stops the operator at startup.


- **Feature options with fractions work in any numeric locale.** A program
  that set a decimal-comma locale (`setlocale(LC_ALL, "")` under `de_DE`,
  `fr_FR` and similar) could not use the default model `vmaf_v1.0.16_3d0h`:
  `vmaf_use_features_from_model()` returned `-EINVAL`, because the option
  parser read `0.7` with `strtod()` in the caller's locale and stopped at the
  period. The feature dictionary's number normalisation had the same fault and
  could store `0.02` as `0` or `0,02`, and a feature named after a fractional
  option came out as `..._0,7`, so a model never found its scores. All three
  now parse and format option numbers
  in the C locale on the calling thread only, as the model reader and the report
  writers already do; the caller's locale is left as it was.


- **The vendored Pelorus x265 CSV reader builds without warnings under an
  optimising GCC.** `x265_csv_read_rows()` in
  `core/src/interop/pelorus_qp_report_csv.c` left its column-index struct
  uninitialised until the header row was seen, and GCC 16 at `-O2 -Wall -Wextra`
  raised seven `-Wmaybe-uninitialized` warnings (`type`, `poc`, `qp`, `bits`,
  `psnr_y`, `psnr_u`, `psnr_v`). Every index now starts at -1 (absent), which the
  reader already treats as a missing column. The fix is in VMAFx/pelorus
  (#79) and re-vendored here (pin `42cb17106a2d`); the parsed values are
  unchanged.


- **The ASan + UBSan job no longer kills `test_pic_preallocation`.** The test
  runs the `vmaf_v0.6.1` model on 1080p frames in the unoptimised sanitizer
  build, about 9 s of CPU, and a hosted runner running the fast suite in
  parallel needs more than meson's default 30 s for that. The test now has an
  explicit `timeout : 180`, as `test_speed_filter` has; its cases are
  unchanged.


- The pre-push documentation gate (`scripts/git-hooks/pre-push-mkdocs-strict.sh`)
  now blocks a push whose `mkdocs build --strict` warns. It ran the build with
  `--quiet`, which hides the warnings `--strict` counts, so broken anchors passed
  it. `scripts/ci/tests/test_pre_push_mkdocs_strict.py`, run by the docs
  workflow, fails on the quiet form.


- **`float_vif` and SpEED refuse a prescaled plane past the `int` index of
  their resampling and filter code.** `core/src/feature/vif_tools.c` indexes
  a plane with `int`, so with `vif_prescale` or `speed_prescale` above about
  1.414 at the 32768x32768 picture cap the index overflowed and the
  resampling wrote outside the plane. `init()` now fails with `-EINVAL` and
  names the plane size. 16K (15360x8640) and every smaller picture are
  accepted at every prescale up to 4.0, as before, and no score changes.


- **The production CPU and MCP server images carry the licences of what they
  contain, and publish the source their copyleft parts require.** Both images
  have `/usr/local/share/vmafx/licenses/THIRD_PARTY_NOTICES.txt` with every
  component, its licence and copyright notices, and the licence texts beside it;
  the VMAFx section is computed from the SPDX headers of every file the build
  compiled. The Debian sources of every installed package (and, for the server,
  the GCC source RPMs of the runtimes bundled in the numpy and scipy wheels) are
  published next to each image as `ghcr.io/vmafx/vmafx:<tag>-source` and
  `<tag>-server-source`, each platform image gets an attested SPDX SBOM, and
  the builds fail when a file of the image has no recorded licence. The server
  image no longer ships the Python build tools. `vmaf-mcp` and `vmaf-tune` now
  declare the licences their files carry (`EUPL-1.2`, and
  `EUPL-1.2 AND BSD-2-Clause-Patent` for `vmaf-tune`) and ship the texts in the
  wheel; the image labels name the VMAFx licence set instead of
  `BSD-2-Clause-Patent`, and the documentation footer states the per-file rule.
  The rules are [ADR-1513](docs/adr/1513-production-artifact-licensing.md); the
  audit of everything published before is
  [Research-2140](docs/research/2140-production-artifact-licence-audit.md)
  ([licensing](docs/licensing.md),
  [production images](docs/development/docker-production.md#licences-and-corresponding-source)).


The `--restore-tracked` step that drops unusable restored CI fixtures no longer needs a bash 4 builtin, so it runs on the macOS runners' bash 3.2 again.


- **`psnr_hvs` on HIP and SYCL scores 4:4:4 pictures past 16K.** The twins'
  scan of the per-block term counts stopped at 32,768 chunks of 256 blocks,
  so from 16384x8640 in 4:4:4 on the later chunks had no offset and their
  terms were written past the device buffer. The scan visits every chunk
  now, as the CUDA twin does; smaller pictures are unchanged.


- **`psnr_hvs` on aarch64 returns the scores of the scalar path and of an
  x86-64 build.** `calc_psnrhvs_neon()` multiplied the two `float` factors of
  the masking threshold as `float`; the scalar reference and the AVX2 function
  multiply them as `double` (`sqrt((double)mask * variance_ratio) / 32`). The
  rounded product put the threshold one `float` step off on about one 8x8
  block in twenty of real content (181 of 3772 luma blocks of frame 18 of the
  Netflix pair). Most differences vanish in the running sum, so NEON differed
  from scalar on 29 of 708 scores, on 14 of 177 frames of nine fixtures, by at
  most 5.7e-7 dB; it now differs on none, under GCC and under clang, and the
  aarch64 scores equal the x86-64 ones. x86-64 scores and the scalar path are
  unchanged. The new `test_psnr_hvs_dispatch_invariance` scores 165 picture
  pairs through the public API with the host's instruction set and with every
  flag masked and compares the four outputs bit for bit, on x86-64 and aarch64;
  with the old kernel it reports 50 differing scores. See
  [`psnr_hvs`](docs/metrics/psnr-hvs.md#cpu-instruction-sets).


- **The images published for 1.0.0-rc.1 and rc.2 are completed or withdrawn.**
  The ROCm images (they carried a binary-only AMD library whose licence
  forbids distributing it) are withdrawn from the registry, and the whole
  `vmafx-node` package is deleted (its FFmpeg was built `--enable-nonfree`);
  the rc.3 node image starts a new package. `vmaf-mcp` 1.0.0rc1 and 1.0.0rc2
  are yanked on PyPI for their wrong licence metadata. Every other rc.1 and
  rc.2 image stays unchanged and gets its notices on the release page, the
  source of its copyleft parts as `<tag>-source`, and an SBOM attested on its
  digest. The release files and `models.tar.gz` get their notices on the same
  pages. A manual workflow does this from recorded digests and licence scans
  of the release tags' builds; the licence tool now fetches Ubuntu sources
  from Launchpad
  ([ADR-1578](docs/adr/1578-published-rc-licence-companions.md),
  [licensing](docs/licensing.md#releases-100-rc1-and-100-rc2)).


- **The Python harness accepts `motion_force_zero` with more than one model.**
  `ExternalProgramCaller.call_vmafexec()` raised `AssertionError` for the
  second model when `motion_force_zero=True`: the loop over the models
  replaced the argument with the string `"true"` and then failed its own type
  check (the same statement is in Netflix upstream). Every model now gets the
  overload. A run with one model, including every golden test, produces the
  same command as before
  (`T-PYTHON-CALL-VMAFEXEC-FORCE-ZERO-SECOND-MODEL-2026-10-02`).


- **Every Python package declares the licences of the files it ships.**
  `vmaf` (the Python harness), `vmaf-train`, `vmaf-dev-llm`, `vmaf-roi-score`
  and the ensemble training kit declared `BSD-2-Clause-Patent` for files that
  are EUPL-1.2 or a mix, and shipped no licence texts; `vmaf-mcp` did not count
  the repository `.gitignore` its sdist carries. Each package's PEP 639
  `License-Expression` is now the union of the licences of its sdist and wheel
  files (`EUPL-1.2 AND BSD-2-Clause-Patent` for the hatch packages,
  `BSD-2-Clause-Patent AND BSD-2-Clause AND BSD-3-Clause-Clear AND EUPL-1.2`
  for `vmaf`, whose compiled ADM extension includes EUPL-1.2 headers), each
  text ships in its `LICENSES/`, and `python/test/setup_metadata_test.py`
  recomputes the union for every package instead of requiring
  `BSD-2-Clause-Patent`, which had failed on `master` since #1954
  ([ADR-1560](docs/adr/1560-python-package-licence-union.md),
  [licensing](docs/licensing.md#python-packages)).


- **Python tests that run the `vmaf` CLI use the build under test.** Several
  suites carried their own lookups, and some fell back to the `vmaf` on `PATH`
  or under `/usr/local/bin`, so they passed against a stale host install (the
  CHUG smoke test, the vmaf-tune fast-path parity test and the MCP golden-pair
  smoke test all ran `/usr/local/bin/vmaf` 3.2.0 on a host without a build).
  `scripts/lib/vmaftest.py` is now the one resolver: `VMAF_BIN`, then
  `VMAF_BIN_FOR_TESTS`, then `build/`, `core/build/` and `core/build-cpu/`. A
  variable that names no executable is an error, and with nothing built the
  tests skip with a message the CI jobs fail on. See
  [how a test finds the vmaf binary](docs/development/test-suites.md#how-a-test-finds-the-vmaf-binary).


- **The two PTQ stub scripts are removed.** `ai/scripts/gen_calibration.py` and
  `ai/scripts/quantize_int8.py` only printed "not yet implemented" and exited 1. Static PTQ is
  `vmaf-train quantize-int8`, which calibrates from a parquet feature cache
  ([quantization guide](docs/ai/quantization.md)).


- **`vmaf` no longer hangs after an out-of-memory on the device.**
  `vmaf_read_pictures()` kept the pair of pictures it was given when it failed
  before it reached an extractor (a non-increasing index, pictures that
  disagree with the stream, the picture pool, the CUDA ring buffer, the CUDA
  translation). Pictures from the CLI's pool then never came back, and
  `vmaf_close()` waited for them forever: with the device's memory taken by
  another process `vmaf --backend cuda` printed `problem reading pictures` and
  never exited, holding whatever lock the caller held. The call now owns both
  pictures on every return, as the failures after that point already did, so
  the CLI exits with `-ENOMEM` (status 244) at once. A caller that unref'd the
  pictures after an error, as `docs/api` used to say, must stop doing so
  ([ADR-1431](docs/adr/1431-read-pictures-owns-pictures-on-every-return.md),
  [API guide](docs/api/index.md#ownership-and-lifetime), Netflix/vmaf#1420,
  where the fork returns the error instead of asserting).


- **The float extractors refuse 9, 11, 13, 14 and 15-bit pictures instead of
  scoring them wrongly.** `float_ssim`, `float_ms_ssim`, `float_adm`,
  `float_vif` and `float_motion`, on the CPU and on every GPU backend, scaled
  10, 12 and 16-bit samples only and read every other depth above 8 as 8-bit
  bytes, so they returned wrong scores for those depths without an error. They
  now fail at initialisation with `-EINVAL` and a log line naming the extractor
  and the depth. Other extractors keep their odd-depth support. The CLI and the
  FFmpeg filters never passed these depths; programs using the C API could.


- **The model-registry schema tests run in the Python harness suite.** `python/test/model_registry_schema_test.py`
  skipped its whole module when `jsonschema` was not installed. `jsonschema` is now in
  `python/requirements-test.in` and its hash lock, and a missing install is an error, not a skip.


- **The files attached to a GitHub release carry their licence notices.** A
  release now has `THIRD_PARTY_NOTICES.txt` (every component, licence and
  copyright line of `libvmaf` and `vmaf`, computed from the SPDX headers of the
  files the build compiled) and `licenses.tar.gz` (the same notices with every
  licence text), and `models.tar.gz` carries a `licenses/` directory for the
  models in it; the release build fails when a file has no recorded licence. The
  SPDX SBOMs are attested on the release files and on the `vmaf-mcp` wheel and
  sdist ([release](docs/development/release.md#what-is-signed),
  [licensing](docs/licensing.md)).


- The oneAPI container image no longer crashes on Arc B580 (Battlemage)
  GPUs. Through v1.0.0-rc.2 it shipped the Intel GPU compute runtime of
  Intel's `oneapi-runtime:2025.3.1` image (version 25.18), and every
  `vmaf --backend sycl` run on a B580 ended with a segmentation fault (exit
  code 139) right after device selection. Arc A380 and UHD 770 GPUs were not
  affected. Replacing only that runtime with compute-runtime 26.35 stopped the
  crash; replacing only the Level Zero loader did not. The image now builds and
  runs on Debian 13, like the CPU image, with Intel's oneAPI 2026.1 compiler
  and SYCL runtime from Intel's apt repository at one exact build, and with the
  compute runtime (26.35.39758.10) and Level Zero loader (1.34.0) that the
  development container uses, all pinned in `build-config.env` (ADR-1368).


- `scripts/ci/release-pr-exempt.sh` and pre-push hooks now exempt PAT-mode
  `release-please` pull requests (`RELEASE_BOT_TOKEN`, author `lusoris`, `type: User`)
  from authoring-discipline CI gates (ADR-1388, closes #1608). The exemption is
  fail-closed: it requires the designated PAT author and verifies that 100% of the
  files in the PR diff belong strictly to the approved release file set
  (`.release-please-manifest.json`, `release-please-config.json`, `CHANGELOG.md`,
  `changelog.d/*`, `docs/changelog-archive/*`, and coordinated version markers).


- **The release-PR exemption test no longer fails on every release pull
  request.** Its "without diff" cases ran the gate inside the CI checkout, and
  without a diff file the gate diffs that checkout against `origin/master`. In
  the release pull request's own run that diff is release-shaped, so the test
  saw an exemption it did not expect. The test now runs the gate outside any
  Git checkout.


- Release provenance for the native Linux files and the `vmaf-mcp` wheel and
  sdist is a GitHub build-provenance attestation (SLSA v1 provenance
  predicate, signed through Sigstore) instead of `slsa-github-generator`
  output. The generator calls its own actions by tag, which the organisation's
  SHA-pinning policy rejects, so the v1.0.0-rc.2 publication failed until the
  policy was relaxed by hand. Releases now attach
  `vmafx-build-provenance.sigstore.json` and `vmaf-mcp-provenance.sigstore.json`
  in place of the `.intoto.jsonl` files; verify with
  `gh attestation verify FILE --repo VMAFx/vmafx`, or offline with `--bundle`
  (ADR-1356). PyPI's PEP 740 attestations are unchanged.


- **A tester prerelease no longer starts the production release workflows.**
  `tester-*` prereleases (macOS and Windows tester bundles) fire the `release`
  event; `docker-publish-production.yml`, `docker-publish-operator-node.yml` and
  `supply-chain.yml` then failed at "Validate tag" and turned master red. Their
  first job now runs only for `v*` tags (workflow dispatch recovery unchanged),
  the rest skip with it, and a contract test covers every `on: release`
  workflow.


- **`scripts/dev/relicense_fork_files.py --check` exits 0.** The tool no longer
  treats the exact-twin data fragments (`scripts/ci/exact_twins.d/`) or
  praetor's byte-locked files as sources that need a header, and it rewrites a
  licence grant only in a file's own header, so the mirrored-header template
  inside `scripts/sync-pelorus-interop.sh` is left alone. Five helper headers
  have `[ports]` entries in `scripts/dev/relicense_provenance.toml`, because
  their family names origins they do not reproduce; `sycl_ssimulacra2_math.h`
  resolves to libjxl instead of the SSIM lineages. Four headers that only
  configure and include a shared header, or hold a kernel argument block, have
  `[not_ports]` entries and stay `EUPL-1.2`
  (`T-RELICENSE-CHECK-PENDING-2026-10-02`,
  [ADR-1474](docs/adr/1474-relicense-helper-headers-and-ci-check.md)).


- Renovate updates `docker/Dockerfile.tester`'s base images together with
  `build-config.env`; the ROCm 10.1.0 update had left the AMD tester image on
  ROCm 10.0.0. ROCm image updates are labelled `rocm` and `manual-review` again.


- **The repository root states the fork's licence, and every package declares
  the licences of the files it ships.** `LICENSE` is now the EUPL-1.2, the
  licence of the files the fork wrote; Netflix's BSD-2-Clause-Patent text and
  copyright notice moved unchanged to `NOTICE`, a name licence detectors do not
  read as a licence file, so `LICENSE` is the only root licence file (licensee
  9.18.0 reports the project as EUPL-1.2; on master it found an MIT licence). The `vmafx-sys` and `vmafx` crates declare `EUPL-1.2`
  instead of `BSD-2-Clause-Patent`, the Helm chart's `artifacthub.io/license` is
  `EUPL-1.2`, the vendored Prometheus Pushgateway subchart is recorded as
  Apache-2.0, `vmafx-rc1-tester` declares `EUPL-1.2 AND MIT`, and the fork's
  `.toml` manifests and configuration files carry an SPDX header (two
  `pyproject.toml` files moved from `BSD-2-Clause-Patent` to `EUPL-1.2`), and the
  dev image's licence label names the licences of the VMAFx files it copies.
  `scripts/ci/check_licence_metadata.py` holds the root files and every manifest
  to the files in the required `Licence Provenance` check and on every commit.
  Published artifacts keep carrying the same licence texts
  ([licensing](docs/licensing.md), ADR-1699).


- The repository no longer carries `.github/rulesets/main.json`, praetor's
  template that declared two approvals, code-owner review and signed commits
  while the live ruleset on `master` enforces one approval and neither of the
  others. `.standards.yaml` declines the template (`adoption.decline:
  [branch-ruleset]`) and `repository-security-policy.json` stays the single
  declaration, compared with the live ruleset. A new test pins the decline, the
  absence and the declared review values. The live ruleset is unchanged.


- **The Arc runner's systemd unit starts the supervisor of this repository.**
  `dev/systemd/vmafx-sycl-arc-runner.service` named `%h/dev/vmaf/...`, the
  archived repository's path, so the documented install started nothing; it
  names `%h/dev/vmafx/vmafx/...` and the install guide says so. ADR-0931 (MCP
  direct cgo path) is `Accepted` for its implemented Phase 1.


- **A build with `-Denable_rust_features=true` registers TAD and no longer
  exports the Rust standard library from `libvmaf.so`
  ([ADR-1713](docs/adr/1713-rc4-rust-extractor-framework.md)).** The TAD
  pilot was compiled but never registered (`--feature tad` failed with
  "problem loading feature extractor"), because the define that gated it never
  reached `feature_extractor.cpp`. The Rust archive's symbols are now kept out
  of the dynamic symbol table with `--exclude-libs` (GNU ld, lld). The Rust
  build also needs no network any more: TAD's unused build-time cbindgen
  dependency is gone and cargo runs `--offline --locked`.


- **`--feature mobilesal` scores frames whose sides are not multiples of 8
  with the saliency students.** `saliency_student_v1` and `v2` need both
  sides divisible by 8; a 576x324 clip failed inside ONNX Runtime with a
  `Concat` dimension mismatch. The extractor now pads the frame to the next
  multiple of 8 by repeating its last column and row and averages the saliency
  map over the frame's own area
  ([ADR-1540](docs/adr/1540-saliency-pad-to-multiple-of-8.md)). Frames that
  already are multiples of 8 score exactly as before.


- **The UBSan and TSan jobs finish `test_integer_psnr_coverage` and
  `test_metal_psnr_hvs_math`.** The APSNR wrap case needs 4.3e9 samples by
  construction and ran into the default 30 s timeout in the debug sanitizer
  builds; it now has a timeout sized from measurement. The Metal psnr_hvs math
  test formed the same per-block terms twice per masking table and now forms
  them once, which halves its run time with an identical report.


- **`vmaf-tune` and `vmafx-tune` no longer pick a GPU backend that their
  `vmaf` cannot run.** `--score-backend auto` used to trust the `--help` text,
  which names every backend on every build, plus vendor tools such as
  `nvidia-smi`: a CPU-only `vmaf` on a GPU host was sent `--backend cuda` and
  refused the run. Both tools now read `vmaf --list-backends`, accept `metal`,
  and try `cuda`, `sycl`, `hip`, `metal`, then `cpu`. The Python module and
  the Go package share one table of selection cases.


- **A Scorecard master run whose master moved on during the scan ends
  cancelled, not failed.** The `Scorecard Master Gate` refused every report
  once the live master ref no longer named the scanned commit, so a merge-train
  landing during a scan turned master red although nothing was wrong (run
  37276297218: master `b0d991df7` read at `53e582831`, one commit ahead, report
  score 8.7). The gate now compares the two commits through GitHub's compare
  API: a final ref that descends from the scanned commit makes the run
  superseded, and the gate writes a receipt, a step summary and a notice naming
  the newer commit, then cancels its own run, so the run shows neither a pass
  nor a failure and the newer commit's push run gives the verdict. An invalid
  final ref or a move that is not to a descendant still fails. The gate job
  holds `actions: write` for the cancel and runs on `!cancelled()` instead of
  `always()`
  ([ADR-1686](docs/adr/1686-scorecard-superseded-master-runs.md)).


- **The server's API contracts name the default model it actually uses.** The
  gRPC proto, the OpenAPI document and the server pages said an omitted
  `model` selects `vmaf_v0.6.1`; `vmafx-server` uses the library default,
  `vmaf_v1.0.16_3d0h`. The OpenAPI document the server serves at
  `/openapi.json` (and in the Swagger UI) is regenerated from the current
  contract; it still carried the licence from before the EUPL move. The
  default-model gate now checks these contracts, and a test fails when the
  embedded document drifts from `api/openapi/vmafx-server-v1.yaml`. See
  [the REST page](docs/server/rest.md).


- The platform setup scripts (`scripts/setup/*.sh`, `scripts/setup/windows.ps1`)
  end with a configure command that works: `meson setup build core ...` from
  the repository root. They printed `meson setup build ...`, which Meson refuses
  because the root has no `meson.build`. The Windows script also prints the
  `/experimental:c11atomics` compiler flags every MSVC build needs.


- **Sidecars name the ONNX opset with one key, `opset`.** The C model loader read `onnx_opset`,
  a key only seven sidecars carried, while the registry, its schema and its validator use `opset`;
  `vmaf_model_meta.opset` stayed 0 for the rest. The loader, every shipped sidecar, the sidecar
  writers and `vmaf_train.registry.ModelMetadata` (field `onnx_opset` is now `opset`) agree.
  Migration: rename `onnx_opset` to `opset` in out-of-tree sidecars; the loader no longer reads
  the old key, and `ModelMetadata` refuses a sidecar that has it.


- **Eleven licence tags now say what the file's own notice says.** The SPDX
  backfill had given some files the identifier of their directory rather than
  of the notice in them. Files that carry only Daala's, Xiph.Org's, dav1d's,
  LIME's or Danny Yoo's two-condition BSD text are `BSD-2-Clause`
  (`core/tools/vidinput.c`, `core/tools/y4m_input.c`,
  `core/src/feature/third_party/xiph/psnr_hvs.c`,
  `core/src/feature/integer_ssim.h`, `core/src/compat/gcc/stdatomic.h`,
  `compat/python-vmaf/core/local_explainer.py`,
  `compat/python-vmaf/tools/scanf.py`); files with Netflix's header and a quoted
  third-party notice name both licences (`core/src/svm.cpp`:
  `BSD-2-Clause-Patent AND BSD-3-Clause`; `core/src/feature/ciede.c`:
  `BSD-2-Clause-Patent AND MIT`; `compat/python-vmaf/tools/sigproc.py`:
  `BSD-2-Clause-Patent AND BSD-2-Clause`); `core/src/feature/iqa/ssim_simd.h`,
  which has Netflix's header, is `BSD-2-Clause-Patent`. No notice text changed
  and no file changed licence: the tags were wrong, not the terms. A pre-commit
  check compares tag and notice from now on
  (`T-SPDX-TAG-DISAGREES-WITH-NOTICE-2026-10-02`,
  [ADR-1250](docs/adr/1250-eupl-fork-relicense.md)).


- **`speed_chroma` no longer overruns its frame buffers on odd-sized pictures
  in subsampled formats.** In YUV 4:2:0 and 4:2:2, picture allocators produce
  one extra chroma sample row or column to cover the odd luma extent
  (`vmaf_chroma_extent()`). `speed_chroma` (CPU, CUDA, and HIP) previously
  derived chroma dimensions with integer floor division, under-allocating its
  buffers by one row or column and causing `picture_copy()` to write past the
  buffer under AddressSanitizer. All three extractors now derive chroma extents
  via `speed_chroma_dimensions()`.


- **`speed_chroma` and `speed_temporal` compute the same covariance bits on
  every CPU dispatch level ([ADR-1459](docs/adr/1459-speed-cov-kernel-exact.md)).**
  The AVX2 and AVX-512 covariance kernels (upstream `30f472b14`) split each
  sum over vector lanes with fused multiply-adds and differed from the scalar
  kernel in the last bits of about one sum in five (up to 6.5e-12 relative),
  under a documented 1e-9 tolerance. They are replaced by row kernels that
  keep one lane per covariance sum and multiply and add separately, so every
  sum has the scalar kernel's bits; aarch64 gets the same kernel for NEON, its
  first SIMD path for SpEED. No score changed on the measured fixtures (60 of
  60 x86 reports and 68 of 68 aarch64 reports byte-identical at
  `--precision max`). The kernels are 2.3x to 4.1x faster than scalar and 1.6x
  to 2.0x slower than the ones they replace on 1080p and 2160p planes:
  `speed_chroma` + `speed_temporal` take up to 12 % more CPU time per frame on
  x86 (3840x2160, AVX-512), which `T-SPEED-COV-KERNEL-EXACT-THROUGHPUT-2026-10-02`
  tracks.


- **The CUDA, HIP and SYCL `speed_chroma` / `speed_temporal` twins divide each
  covariance sum by the exact element count.** They divided by the count
  rounded to fp32, which differs from the count above 2^24 elements per
  submatrix (a picture wider than 16K with `speed_prescale` above 2) and moved
  the covariance away from the CPU extractor's. Scores of every picture up to
  16K are unchanged.


- **`scripts/dev/speed_gpu_parity.py` accepts a relative `--vmaf` path.** The
  script refused every relative path, including its own default
  `build/tools/vmaf`, with `allowlisted executable must be bare or absolute`
  and exit status 2, so the commands in the guides and in `docs/state.md` did
  not run as written. A relative path is now taken from the working directory;
  an absolute path and a bare name on `PATH` work as before
  ([SpEED](docs/metrics/speed_qa.md#checking-a-gpu-twin-against-the-cpu)).


- **`speed_temporal` no longer overruns its frame buffers when `speed_prescale`
  is above 1.** The CPU extractor sized its four frame buffers with the source
  height, but resamples each frame in place at the prescaled height, so
  `--feature speed_temporal=speed_prescale=1.5` read and wrote past the end of
  the allocation: an AddressSanitizer build reports a heap-buffer-overflow and a
  release build aborts with `free(): invalid size` or corrupts the heap. The
  buffers now hold the upscaled plane, as `speed_chroma`'s already did. Scores
  at `speed_prescale` 1 and below are unchanged, and the CUDA, SYCL and HIP
  twins were not affected. Reported upstream as
  [Netflix/vmaf#1626](https://github.com/Netflix/vmaf/issues/1626)
  ([features](docs/metrics/features.md#options-shared)).


- **`speed_chroma` and `speed_temporal` return Netflix's values, and their
  GPU twins return the CPU's bit for bit.** The fork's port of the SpEED
  extractors had three single-precision forms where Netflix computes in
  double precision and rounds once (`1.0 / sqrt(1 + t * t)` in the Givens
  rotation, the `log2()` of the entropy and of the score). Against a build of
  Netflix/vmaf the CPU scores differed on most frames: `speed_chroma` by up
  to 2.3e-5, `speed_temporal` by up to 6.6e-4, the `vmaf_v1.0.16` models by
  up to 2.5e-5. `speed.c` carries Netflix's expressions again
  ([ADR-1477](docs/adr/1477-speed-upstream-double-math.md)): every value
  compared is identical (261 frames per `speed_chroma` output, 320
  `speed_temporal` frames, 204 scores per `vmaf_v1.0.16` model, 3564 option
  values), with the scalar kernels, the default dispatch and AVX2, and GCC
  and clang builds for x86-64 and aarch64 agree. The fork still differs
  where it decided to: `speed_max_val` clamps `speed_temporal` too, a
  prescale above 1 no longer reads past a buffer, and a frame too small for
  SpEED is refused. The CUDA, HIP and SYCL twins now run `speed.c` on the
  device up to the per-block variances and form the entropies and the score
  on the host with `speed.c`'s own statements, so they call the logarithm the
  CPU extractor calls: on an RTX 4090, a gfx1036 and an Arc A380 all 3409
  values compared equal `--backend cpu` of the same build, GCC or icx, where
  a GCC build's CPU and a twin used to differ on a few outputs by up to
  1.9e-6. The parity gate compares the six cells with tolerance 0 (`5e-6`
  and `4e-5` before). No twin is measurably slower. Stored `speed_chroma`,
  `speed_temporal` and `vmaf_v1.0.16` scores, CPU or GPU, change in the
  digits above; re-run them before comparing at full precision.


- **A `-Denable_float=false` build scores with the default model again
  (port of Netflix/vmaf `6046b1926` and the build hunk of `4718b4f5f`).** The
  default model `vmaf_v1.0.16_3d0h` reads `speed_chroma`, but `speed_chroma`
  and `speed_temporal` were compiled and registered only with
  `enable_float=true`, so such a build stopped with `could not initialize
  feature extractor "Speed_chroma_feature_speed_chroma_uv_score"`. Both
  extractors are now part of every build, as in Netflix/vmaf, and a
  `-Denable_float=true` build (the default) is unchanged: every score is
  identical before and after.


- **The CPU `ssimulacra2` extractor no longer crashes on 4:0:0 input.** Its
  init ignored the pixel format, so a luma-only picture passed to
  `vmaf_read_pictures()` through the C API reached the colour conversion, which
  read the missing U plane through a NULL pointer (a segmentation fault in
  `ssimulacra2_picture_to_linear_rgb_avx512` on an AVX-512 host). Init now
  refuses 4:0:0 with `-EINVAL` and an error message, as `ssimulacra2_sycl`
  does. The `vmaf` CLI was not affected: it rejects `-p 400` and converts Y4M
  `mono` input to 4:2:0. See [SSIMULACRA 2](docs/metrics/ssimulacra2.md).


- **Help strings, header comments, option descriptions and CI comments now say what the code does.**
  `vmaf --help` no longer shows `(default: auto)` for `--hip_device` and `--metal_device` (both
  are opt-in); `vmaf_vpl --help` names the real default model; the `VMAF_SYCL_NO_GRAPH`
  deprecation warning recommends `VMAF_SYCL_DISPATCH=<feature>:direct` (the old advice,
  `VMAF_SYCL_USE_GRAPH=false`, did nothing) and prints once; the headers `picture.h`,
  `libvmaf_mcp.h`, `libvmaf_hip.h` and `libvmaf_metal.h`, the HIP / Metal / MCP / TAD build
  options, the fuzz README and the CI comments are corrected
  ([CLI](docs/usage/cli.md), [pictures](docs/api/pictures.md), [env vars](docs/usage/env-vars.md)).
  FFmpeg patch impact: none.


- **`LICENSE-MIT` in 1.0.0-rc.1 and 1.0.0-rc.2 was stale.** The trees of both
  release candidates held a root `LICENSE-MIT` ("Copyright (c) 2026 Lusoris"),
  a leftover of ADR-0686's plan to dual-license fork code under
  BSD-3-Clause-Plus-Patent or MIT, which ADR-1250 replaced before the first
  candidate. ADR-1250 governs: fork-authored code is EUPL-1.2, Netflix's code is
  BSD-2-Clause-Patent, and each file's SPDX header is authoritative. The file is
  removed, the root `go.mod` retracts `v1.0.0-rc.1` and `v1.0.0-rc.2`, and the
  tags stay. Copies remain in GitHub's source archives of the tags made between
  2026-05-28 and 2026-10-05 and in the Go module proxy's zips of both versions;
  no published image, release file, tester bundle or Python package contained
  it ([licensing](docs/licensing.md#the-stale-license-mit-in-their-source-trees),
  ADR-1699).


- `scripts/ci/check-state-md-rows.sh` works with the `mawk` of Debian 12. That
  version reads regex intervals such as `{0,2}` literally, so the gate matched no
  bug row there and passed every `docs/state.md`, duplicates included. The gate
  now avoids intervals; CI's Ubuntu runner was not affected.


- **The markdown governance gate is back to seconds on `docs/state.md`.** Two
  ledger rows left a backtick unpaired; because most of the ledger is one
  paragraph, every later code span re-paired and a `[` fell outside any span,
  which made the GFM autolink-literal parser behind the gate quadratic and the
  lint of that file exceed the gate's 120 s budget. The rows are repaired, and
  `scripts/ci/check-state-md-rows.sh` now refuses a line of `docs/state.md`
  whose code spans or `[` brackets do not close on that line.


- **The post-commit state sync works in linked worktrees again.** The hook
  (`scripts/githooks/state-sync.sh`, ADR-1280) mirrored the private ledgers
  into a worktree without `questions.meta.json`, and Praetor's state sync
  reads it for every `QUESTIONS.md` entry, so every commit in a linked
  worktree ended its post-commit hook with `state sync list questions:
  QUESTIONS.md line 7: Q-001 metadata missing from questions.meta.json` and
  the worktree's state was not synchronised. The file is now mirrored with the
  others.


- **`vmaf_read_json_model_collection` rejects sub-model name truncation with `-EINVAL`.**
  Port of the sub-model name truncation check from upstream Netflix/vmaf commit
  `15f1447c6` ([Netflix/vmaf#1428](https://github.com/Netflix/vmaf/pull/1428)).
  In both `core/src/read_json_model.cpp` and `core/src/read_json_model.c`,
  the return value of `snprintf` when formatting generated sub-model names
  `"%s_%04u"` was ignored via `(void)snprintf`. When a model collection
  reaches index 9999, `++i` increments to 10000 (5 digits), exceeding
  `cfg_name_sz` (`strlen(name) + 5 + 1`) and truncating the sub-model name.
  Both the C++23 parser and its C twin now validate `n < 0 || (size_t)n >= cfg_name_sz`,
  tear down any allocated model and collection objects without leaking, and
  return `-EINVAL`.


- **SYCL integer ADM row reduction runs spill-free on DG2 and restores Arc A380 parity under `xe`.**
  In `integer_adm_sycl.cpp`, `launch_csf_den_cm` kept nine 64-bit accumulators live across the
  column loop, which IGC compiled at SIMD16 with an 864 B/thread register spill on DG2 (Arc A380).
  Under the Linux `xe` driver, scratch memory accesses return corrupted values, causing all ADM
  accumulators to evaluate to zero and failing `test_sycl_adm_parity` (`integer_adm3_csf_2_dlmw_0.7_egl_1_min_0.5_nw_0.02`
  reported CPU 0.5 vs SYCL 1.0). The kernel is restructured into two sequential column reduction
  phases (CSF denominator 3 sums, then DLM and AIM contrast measures 6 sums) with sub-group partials
  staged in local memory before folding, completely eliminating private and spill memory (`privateMemSize: 0`,
  `spillMemSize: 0` on Arc A380). All 7 parity cases in `test_sycl_adm_parity` and all 6 in
  `test_sycl_adm_tiny_frames` pass, and `speed_gpu_parity.py --backend sycl --feature adm` is bit-exact
  (0.000e+00 delta) against CPU on both 576x324 (48/48 frames) and 3840x2160 (50/50 frames).
  Throughput at 4K on BBB 3840x2160 8-bit is 10.92 ms/frame vs 10.70 ms/frame before
  (ADR-1395, `T-SYCL-ADM-CM-SCRATCH-2026-09-30`).


- **A SYCL build with a single AOT target no longer fails the image check.**
  Configuring `-Dsycl_icpx_aot_targets=dg2-g11`, the single-target example in
  the SYCL overview, stopped at `sycl_aot_image_check` with "holds no ocloc fat
  binary" although the build was correct: with exactly one target `ocloc`
  writes each image as a bare native binary instead of a fat binary, and the
  check only knew the fat binary. It now accepts both forms and reads the GPU
  IP version of a bare binary from its product-config note, the value
  `ocloc ids` prints for the target. It still fails the build when an image
  lacks a listed target, is built for a GPU IP version that no listed target
  uses, or is neither form. Builds with two or more targets are checked as
  before.


- SYCL builds now contain the native Intel GPU code that `sycl_icpx_aot_targets`
  asks for. Since ADR-0568 the images were compiled into the objects and then
  dropped at the link, so every `libvmaf.so` was SPIR-V only and compiled its
  kernels on each cold start: the default model's first frame took 524 ms on an
  Arc B580 with a cold compiler cache and now takes 201 ms (UHD 770: 609 to
  245 ms); scores are bit-identical. SYCL sources are compiled with
  `-fno-sycl-rdc --offload-compress`, so the images are built at compile time
  and survive every link; `libvmaf.so` grows from 4.6 to 7.7 MB. An AOT build
  now needs Intel's `ocloc` on `PATH` (`scripts/ci/install-intel-ocloc.sh`
  installs the release pinned as `INTEL_NEO_VERSION` in `build-config.env`),
  and configure stops with an explanation without it; configure with
  `-Dsycl_icpx_aot_targets=` for a SPIR-V-only build. On Linux the build fails
  if `libvmaf.so` lacks an image for any requested target (ADR-1360).


- **The default SYCL build compiles for Lunar Lake and Arc B-series again.**
  Six kernels required a sub-group size of 8, which Xe2 GPUs (`lnl-m`,
  `bmg-g21`, `bmg-g31`) do not offer, so the ahead-of-time compile of
  `float_motion_sycl`, `float_adm_sycl`, `float_vif_sycl` and
  `ssimulacra2_sycl` failed for those targets ("Kernel compiled with required
  subgroup size 8, which is unsupported on this platform") and the dev
  container image did not build; a build without ahead-of-time targets had
  the same failure on such a device at run time. The kernels require 16 now,
  and the build rejects any size but 16 or 32. On an Arc A380 the scores are
  bit-identical and a 4K frame takes at most 2 % longer. Not yet measured on
  an Xe2 device. `meson test --suite sycl-aot` compiles every SYCL source
  file for all default targets from any build
  ([SYCL backend](docs/backends/sycl/overview.md#sub-group-sizes-and-the-aot-targets-adr-1468),
  [ADR-1468](docs/adr/1468-sycl-sub-group-sizes-every-aot-target.md)).


- **SYCL: `psnr_hvs_sycl` no longer crashes on Intel Arc B580 (Xe2).** The Intel
  GPU compiler (IGC 2.41.5) crashed the host process with SIGSEGV while
  compiling the kernel at SIMD32 for Xe2, triggered by the 8x8 DCT running in
  one work-item's private memory. The DCT now runs in local memory, split
  across work-items. Scores are bit-identical on devices where the old kernel
  ran, and a 4K frame takes 130 ms instead of 208 ms on a UHD 770
  (`T-SYCL-PSNR-HVS-B580-SIGSEGV-2026-09-29`).
- **SYCL: small frames no longer lose the device.** `adm_sycl`, `vif_sycl`,
  `motion_sycl`, `motion_v2_sycl`, `float_motion_sycl` and `float_vif_sycl`
  loaded their local-memory tiles, padding lanes included, with a single edge
  reflection. On small planes that read outside the buffer and raised
  `UR_RESULT_ERROR_DEVICE_LOST` on an Arc B580 and a UHD 770: `adm_sycl` for
  frames 64 rows high or less, `vif_sycl` up to at least 96x64 and
  `motion_sycl` up to 33x33 on the B580. The loads now stay inside the plane;
  scores for larger frames are unchanged
  (`T-SYCL-TILE-HALO-OOB-READ-2026-09-29`).
- **SYCL: `vif_sycl` hands frames below 16 pixels to the CPU.** Its filters
  reach more than one reflection outside planes smaller than 16 pixels in
  either dimension, which lost the device at 8x8. Model dispatch now computes
  such frames with the CPU `vif` extractor, bit-identical to a CPU run; a
  direct `--feature vif_sycl` request fails with an error naming `vif`
  (`T-INTEGER-VIF-TINY-FRAME-GUARD-2026-09-29`).
- **SYCL: `vif_sycl` scores are correct for odd widths.** When the width at
  any scale was odd (for example 854x480, 1366x768 or 853x480), scales 1-3 read
  the downsampled plane at the wrong row stride and drifted from the CPU by up
  to 1.2e-3 at those sizes and 3.3e-2 on tiny frames, which also moved the
  VMAF score. They now match the CPU within 1e-6 at those sizes
  (`T-SYCL-VIF-ODD-WIDTH-RD-STRIDE-2026-09-29`).
- **SYCL: a device fault now fails the frame.** After a failed graph wait the
  `adm_sycl`, `vif_sycl`, `motion_sycl`, `psnr_sycl` and `float_moment_sycl`
  extractors emitted scores for that frame from stale buffers (about 1.0 for
  every ADM scale) and `vmaf_read_pictures()` returned 0; only a later frame's
  upload failed. They now return `-EIO` for the faulted frame
  (`T-SYCL-GRAPH-WAIT-ERROR-DROPPED-2026-09-29`).


- **`ciede_sycl` follows the CPU `ciede` to 1.4e-11.** The CPU extractor
  computes CIEDE2000 in `double` and stores in `float`. The SYCL twin
  computed in `float` throughout, used the device's `float` math functions
  and a rewritten form of one branch, and was up to 1.14e-5 from the CPU. A
  SYCL kernel has no `double`, so the kernel now runs the CPU's statements
  with every `double` as a pair of `float` values and every math-library
  call as a function on such pairs, and the host adds the per-pixel values
  in the CPU's order
  ([ADR-1436](docs/adr/1436-sycl-ciede-cpu-arithmetic.md)). Measured on an
  Arc A380 at `--precision max`: the Netflix 576x324 pair identical to
  `--backend cpu` on 47 of 48 frames, its 10-, 12- and 16-bit versions and
  both 1080p checkerboard pairs on every frame, 200 frames of BBB 3840x2160
  within 1.4e-11. These are the CUDA twin's figures. It is not
  bit-identical: the C library's `powf` is not correctly rounded and 18 to
  64 of the 8.3 million pixels of a 3840x2160 frame round the other way. The parity
  gate compares the twin at `1e-9` instead of `5e-3`. A 3840x2160 frame
  takes 50.3 ms on an Arc A380 instead of 16.2 ms, and the twin holds 33 MB
  more on the device and on the host at that size. Stored `ciede_sycl`
  scores change by up to 1.14e-5.


- **The SYCL dma-buf import no longer closes the caller's descriptor.**
  `vmaf_sycl_dmabuf_import()` handed the caller's descriptor to Level Zero,
  whose compute runtime 26.35 closes it when the buffer is imported again
  while its first import is alive; `libvmaf_sycl.h` leaves the descriptor to
  the caller, and the VA surface import closed the same number again, which
  could close an unrelated descriptor another thread had opened in between.
  Level Zero now gets a private duplicate, and the caller's descriptor stays
  open on every driver.


- **Editing a SYCL header now rebuilds the kernels that include it.** The
  build compiled each SYCL source through a custom target that tracked only
  the source file. After a change to a header such as
  `core/src/feature/sycl/sycl_exact_fp.h`, `ninja` reported nothing to do and
  the library kept the kernels built from the old text; only a clean build
  picked the change up. The targets now record the headers each source
  includes (compiler depfiles, as the CUDA and HIP kernels have had since
  ADR-1320). Builds from a clean tree, such as CI and the release container,
  were not affected. Not yet on Windows, which builds from clean.


- **`float_adm_sycl` returns the CPU's scores bit for bit.** The SYCL float
  ADM twin was up to 1.7e-5 from the CPU extractor: it associated the angle
  test's threshold differently, used `float` where the CPU uses `double` for the
  enhancement gain and two constants, added the masking threshold and the
  frame sums in another order, and floored the frame sums at `1e-2` where the
  CPU uses `1e-10`. The kernels now run the CPU's arithmetic operation for
  operation; a SYCL kernel has no `double`, so the three `double` expressions
  are evaluated as exact pairs of `float` values, with the CPU's operations
  replayed in 64-bit integers next to a rounding boundary
  ([ADR-1434](docs/adr/1434-sycl-float-adm-cpu-arithmetic.md)). Measured on
  an Arc A380 at `--precision max`, every output of every frame equals the
  CPU extractor of the same build on the Netflix 576x324 pair at 8, 10, 12
  and 16 bits, both 1080p checkerboard pairs and 200 frames of BBB
  3840x2160, with `debug=true`. A 3840x2160 frame takes 12.3 ms instead of
  15.1 ms; the twin uses 48 MB more device memory there. On near-flat
  content scored with `adm_noise_weight=0` the twin reported `adm2 = 1`
  where the CPU reports 0; it now reports the CPU's value. The twin also
  takes the CPU options it rejected: `adm_skip_scale0`, `adm_skip_aim_scale`,
  `adm_f1s0..3` and `adm_f2s0..3`. `adm_p_norm` other than 1 or 3 stays
  within 1.8e-7 of the CPU. Stored `float_adm_sycl` scores change by up to
  1.7e-5.


- **`float_adm_sycl` works on Arc A-series GPUs under the xe driver.** Its two
  contrast-masking kernels indexed a small private array with the band number,
  which the Intel graphics compiler keeps in scratch memory, and kernels that
  use scratch memory return wrong values on these GPUs under xe (ADR-1395). On
  an Arc A380 `--backend sycl --feature float_adm` returned NaN and stopped
  with `problem reading pictures`. The kernels now pick the band by value.
  The twin is within 2.5e-6 of the CPU on the Netflix 576x324 pair, 3.5e-7 on
  1080p checkerboards and 1.3e-5 at 3840x2160, as on other devices. This was
  the last entry of the scratch ratchet list: no libvmaf SYCL kernel uses
  scratch memory any more (110 kernels audited on the A380), the list stays
  empty, and the start-up warning on an affected device now says that no
  extractor's scores are affected.


- **`float_moment_sycl` is bit-identical to the CPU `float_moment` extractor
  at 16 bits.** The CPU forms each sample's square in `float` before adding
  it, which at 16 bits is the square rounded to 24 bits; the SYCL twin added
  exact integer squares. Its second moments (`float_moment_ref2nd`,
  `float_moment_dis2nd`) were up to 1.0e-4 from the CPU's on 16-bit content
  with real low bits (0 of 13 such frames identical on an Arc A380), while
  8-, 10- and 12-bit input and the first moments were identical. The kernel
  now adds the CPU's float square, and all four outputs are identical on 288
  of 288 measured frames at `--precision max`. The parity gate compares the
  CPU and SYCL `float_moment` cells with tolerance 0. One range stays within
  a derived bound instead: on a 16-bit frame of more than 2 097 152 pixels
  whose sum of squares passes 2^53 the CPU's own sum rounds as it goes (2.7e-7
  measured at 2560x1440). No measurable cost. Stored 16-bit
  `float_moment_sycl` second moments change by up to 1.0e-4
  ([ADR-1449](docs/adr/1449-sycl-float-moment-cpu-float-squares.md),
  [SYCL backend](docs/backends/sycl/overview.md#float_moment_sycl-matches-the-cpu-float_moment-at-16-bits-2026-10-02)).


- **`float_motion_sycl` returns the CPU's scores bit for bit.** The SYCL twin
  summed the absolute differences of each 32x4 work-group on the device and
  the groups in `double` on the host, where the CPU `float_motion` extractor
  keeps one `float` running sum per row and one over the rows. Measured on an
  Arc A380 that left `motion` and `motion2` up to 1.36e-4 from the CPU on
  1920x1080 checkerboards (above the 5e-5 cross-backend tolerance), 2.4e-5 at
  3840x2160 and 3.1e-6 on the Netflix 576x324 pair. The twin now adds each
  row on the device in the CPU's order, one work-item per row, and the host
  adds the rows
  ([ADR-1411](docs/adr/1411-sycl-float-motion-cpu-float-sum.md), after
  ADR-1409 for CUDA). At `--precision max` every frame is identical on the
  Netflix pair, both 1080p checkerboard pairs and 200 frames of BBB
  3840x2160, also at 10, 12 and 16 bits and with `motion_fps_weight` and
  `motion_max_val` set. The second pass over the blurred planes costs 0.38 ms
  per 3840x2160 frame on the A380 (3.85 to 4.23 ms). The parity gate compares
  this twin with tolerance 0. Stored `float_motion_sycl` outputs change in
  their low digits by at most those differences. The HIP and Metal twins
  still agree with the CPU to four decimal places.


- **`float_motion_sycl` honours `motion_force_zero` and weights its debug
  score.** The twin declared `motion_force_zero` but ignored it and emitted
  real motion scores; it now emits zeros, like the CPU `float_motion`. Its
  debug `VMAF_feature_motion_score` now carries `motion_fps_weight`, as on the
  CPU, instead of the unweighted SAD. The SSIM page no longer documents an
  `enable_chroma` option and `_cb` / `_cr` outputs that the `ssim` extractor
  does not have; it lists `enable_db` and `clip_db`
  ([SSIM](docs/metrics/ssim.md#options)).


- **`float_motion_sycl` emits `motion3`, like the CPU `float_motion`.** The
  SYCL twin wrote `motion` and `motion2` only, so `--backend sycl --feature
  float_motion` lost `VMAF_feature_motion3_score` without a warning, and a
  request with `motion_blend_factor` or `motion_blend_offset` ran on the CPU.
  It now publishes the CPU's `motion3` (the fps-weighted `motion2`, blended by
  `motion_blend_factor` / `motion_blend_offset` and capped at
  `motion_max_val`; frame 0 from the first SAD, `0` for a one-frame input) and
  accepts both blend options (aliases `mbf` / `mbo`). On an Arc A380 at
  `--precision max` every output equals the CPU's on the Netflix pair, both
  1080p checkerboard pairs and 200 frames of BBB 3840x2160, with and without
  the options. The cross-backend parity gate's `float_motion` cell now
  compares `motion3` too, and `test_sycl_twin_option_parity` carries the
  `motion3` cases and the regression cases of the earlier SYCL parity fixes
  (flat identical frames, a single-pixel frame, `apsnr` with `--subsample 2`,
  `motion_v2` weight, cap and one frame). The Metal twin still writes no
  `motion3` (`T-GPU-FLOAT-MOTION3-MISSING-2026-09-30`).


- **`float_ms_ssim_sycl` computes the CPU's arithmetic.** The SYCL twin
  added the decimate taps in two roundings where the CPU fuses them, kept the
  Gaussian window sums and the luminance, contrast and structure terms in
  fp32 where the CPU uses `double`, and combined unrounded per-scale means.
  Measured on an Arc A380 it matched the CPU on none of 104 frames (6.9e-8 on
  the Netflix 576x324 pair, up to 2.98e-6 on 1080p checkerboards, 1.23e-6 at
  3840x2160). It now follows the reference operation for operation, with the
  CPU's `double` values carried as exact pairs of floats and the frame sums
  in 64-bit fixed point
  ([ADR-1414](docs/adr/1414-sycl-float-ms-ssim-cpu-arithmetic.md), after
  ADR-1403 for CUDA). Every per-scale mean of every measured frame equals
  the CPU's, `enable_lcs` and `enable_chroma` outputs included; the score
  equals a GCC build's on 253 of 254 frames, the other differing by 1.1e-16
  through the host `pow()` of an Intel-compiler build. The parity gate
  compares the twin with tolerance 0. A 3840x2160 frame takes 42.6 ms on the
  A380 against 31.4 before. Stored `float_ms_ssim_sycl` outputs change in
  their low digits by at most the differences above. The HIP and Metal twins
  keep the old arithmetic.


- **`float_ms_ssim_sycl` returns the CPU's per-scale means on frames where a
  mean lies next to a `float` rounding boundary.** `float_ms_ssim` adds each
  scale's luminance, contrast and structure terms into a running `double`
  per scale; the SYCL twin added them as integers per work-group, an exact
  sum that is not the CPU's. On a 176x176 noise pair its
  `float_ms_ssim_l_scale0` was 0.9884905219078064 where the CPU returns
  0.9884904623031616. The twin now stores every window's terms, with the
  luminance and contrast terms as the CPU's doubles, and the host adds them
  in the CPU's order. On an Arc A380, 2208 of 2208 `enable_lcs` values on
  138 frames are bit-identical to the CPU. A 3840x2160 frame takes 75.9 ms
  instead of 44.7 ms (18.2 instead of 11.5 ms at 1920x1080) and the twin
  holds 219 MB of device and pinned host memory at 3840x2160
  ([SYCL backend](docs/backends/sycl/overview.md#float_ms_ssim_sycl-adds-its-per-scale-sums-in-the-cpus-order-2026-10-02),
  [ADR-1466](docs/adr/1466-sycl-float-ms-ssim-raster-sum.md)).


- **`float_psnr_sycl` is bit-identical to the CPU `float_psnr` extractor at
  every bit depth.** The CPU squares each sample difference in `float` and
  adds the squares in `double`, which does not round. The SYCL twin added
  each 16x16 work-group in single precision: exact at 8 bits, and at 10, 12
  and 16 bits only while the differences in a group are small. On an Arc A380
  it matched the CPU on every frame of real clips and was up to 7.4e-8 dB off
  on high-bit-depth input with large differences (0 of 19 such frames
  identical). The kernel now adds the squares as integers, and 288 of 288
  measured frames are identical at `--precision max`, with `uncapped=true`
  too. The parity gate compares the CPU and SYCL `float_psnr` cells with
  tolerance 0. A 3840x2160 frame takes 3.41 ms instead of 3.31. Stored
  `float_psnr_sycl` scores of such input change by up to 7.4e-8 dB
  ([ADR-1450](docs/adr/1450-sycl-float-psnr-exact-block-sums.md),
  [PSNR](docs/metrics/psnr.md#float_psnr)).


- **`float_ssim_sycl` combined formula residual eliminated.**
  Arithmetic alignment from PR #1645 (`core/src/feature/sycl/integer_ssim_sycl.cpp`,
  evaluating exact per-pixel $l \cdot c \cdot s$ in fp32 pairs, fixed-point work-group
  sums, and double host reduction) eliminated the residual against the CPU reference.
  Measured on an Intel Arc A380 under the Linux `xe` driver: max absolute difference
  against `--backend cpu` is 0.000e+00 across all 48 frames of Netflix 576x324 and
  all frames of BBB 3840x2160 at both auto scale and explicit `scale=1`.
  `test_sycl_twin_option_parity` passes 13/13 with exact match on flat identical frames.


- **`float_ssim_sycl` with `enable_db` no longer reports tens of dB below the
  CPU on near-identical frames.** The CPU rounds each frame's SSIM mean to fp32
  before converting it to dB, so a frame within half an fp32 step of 1 scores
  exactly 1 and reports `+inf` or the `clip_db` ceiling; the twin kept the
  double mean and reported a finite value (93.6 dB against the CPU's 121 dB on
  the first frames of a 4K pair). The twin now rounds the `float_ssim` and
  `float_ssim_l/c/s` means the same way; linear scores move by less than 6e-8.


- **`float_ssim_sycl` returns the CPU's `float_ssim` on frames whose
  per-window terms cancel.** The twin was declared exact and matched the CPU
  on every frame of real content measured, but on a constructed 64x64 pair it
  scored one `float` step away (-4.222829659283889e-07 where the CPU scores
  -4.222829943500983e-07): the CPU adds one `double` term per window in
  raster order, and the twin added the terms per work-group, which rounds
  elsewhere. The twin now forms each window's terms as the CPU's doubles and
  the host adds them in the CPU's order, for `float_ssim` and for
  `float_ssim_l`, `_c` and `_s` under `enable_lcs`, at every `scale`. On an
  Arc A380 the constructed pair and 2070 of 2070 values on 138 frames under
  six option sets are bit-identical to the CPU. With the automatic scale a
  1080p frame takes 1.51 ms instead of 1.32 and a 4K frame 4.11 ms instead of
  3.93; an explicit `scale=1` on a 4K frame takes 39.1 ms instead of 23.3 ms
  (48.6 instead of 25.5 ms with `enable_lcs`)
  ([SYCL backend](docs/backends/sycl/overview.md#float_ssim_sycl-adds-its-frame-sums-in-the-cpus-order-2026-10-02),
  [ADR-1463](docs/adr/1463-sycl-float-ssim-raster-sum.md)).


- **`float_vif_sycl` returns the CPU's scores bit for bit.** The SYCL twin
  filtered with a table of Gaussian taps the CPU extractor stopped using
  (it derives them at start-up with `vif_get_filter()`), called the device
  `log2` where the CPU evaluates a polynomial, took `vif_sigma_nsq` as a
  `float` where the CPU keeps it in `double`, and reduced per sub-group and
  per block where the CPU keeps one `float` running sum per row and one over
  the rows. Measured on an Arc A380 that left no frame of the Netflix
  576x324 pair identical, up to 3.8e-5 away, and 7.0e-6 at 3840x2160. The
  twin now takes the CPU's taps, evaluates the CPU's per-pixel statistic
  without a 64-bit floating-point type (pairs of floats, and the CPU's
  `double` operations replayed in integers next to a rounding boundary) and
  adds in the CPU's order
  ([ADR-1422](docs/adr/1422-sycl-float-vif-cpu-arithmetic.md), after
  ADR-1412 for CUDA). At `--precision max` every output of every frame is
  identical on the Netflix pair at 8, 10, 12 and 16 bits, both 1080p
  checkerboard pairs and 200 frames of BBB 3840x2160, with `debug=true` and
  with non-default options. `float_vif_sycl` also accepts the CPU's
  `vif_scale1_min_val`, `vif_scale2_min_val` and `vif_scale3_min_val` now. A
  3840x2160 frame takes 23.95 ms on the A380, 20.54 ms before, and 100 MB
  more device memory. The parity gate compares this twin with tolerance 0.
  Stored `float_vif_sycl` outputs change by up to 3.8e-5. The HIP and Metal
  twins still agree with the CPU to four decimal places.


- **SYCL and HIP `psnr_hvs` return the CPU extractor's scores bit for bit**
  (`T-SYCL-PSNR-HVS-EXACT-SUM-2026-10-01`, `T-HIP-PSNR-HVS-EXACT-SUM-2026-10-01`,
  [ADR-1401](docs/adr/1401-psnr-hvs-sycl-hip-exact-twins.md)). Like the CUDA
  twin (ADR-1397), `psnr_hvs_sycl` and `psnr_hvs_hip` now store the 64 terms of
  every block in the CPU's arithmetic and the host adds them in the CPU's order.
  Before, they summed each block on the device and were up to 1.7e-2 dB from
  `--backend cpu` at 3840x2160, beyond the parity tolerance. `psnr_hvs`,
  `psnr_hvs_y`, `psnr_hvs_cb` and `psnr_hvs_cr` are identical to the CPU at
  `--precision max` from 576x324 to 3840x2160 and at 8 to 12 bits, measured on
  an Arc A380 and a gfx1036, and the parity gate compares every `psnr_hvs` cell
  with tolerance 0. The SYCL kernel has no fp64: it takes the CPU's `double`
  masking threshold from a new integer square root of the exact product
  (`sqrt_prod_rn()` in `sycl_exact_fp.h`), and it stays free of scratch memory.
  The scores of both twins therefore change in their last digits (by up to
  1.7e-2 dB at 3840x2160). Both are slower for it: a 3840x2160 frame takes
  35.9 ms instead of 22.4 ms on an Arc A380 and about 38 ms instead of 18 ms on
  a gfx1036, and the term buffer needs 65 MB per 3840x2160 frame; tuning is
  tracked as `T-SYCL-HIP-PSNR-HVS-EXACT-SUM-THROUGHPUT-2026-10-01`. Compare a
  twin with the CPU extractor of the same `vmaf` binary: the dB value uses the
  host's `log10`, which differs by one unit in the last place between an `icx`
  and a gcc build. See
  [the psnr_hvs page](docs/metrics/psnr-hvs.md#agreement-with-the-cpu-extractor).


- **SYCL and HIP `psnr_hvs` score 4:0:0 input, and the HIP twin takes
  `enable_chroma`** (`T-SYCL-HIP-PSNR-HVS-YUV400-REFUSED-2026-10-01`).
  `psnr_hvs_sycl` and `psnr_hvs_hip` refused 4:0:0 pictures at `init()`
  (`YUV400P unsupported`), where the CPU extractor and `psnr_hvs_cuda` score the
  luma plane and emit `psnr_hvs_y` and `psnr_hvs`. Both twins now do the same.
  `psnr_hvs_hip` also gains the CPU extractor's `enable_chroma` option (default
  `true`): with `false` it uploads, dispatches and scores luma only, like the
  CUDA and SYCL twins. Scores for 4:0:0 and for `enable_chroma=false` are
  identical to the CPU's at `--precision max`, measured on an Arc A380, a
  gfx1036 and an RTX 4090. See
  [the psnr_hvs page](docs/metrics/psnr-hvs.md#sample-conversion).


- **SYCL: kernels stay out of scratch memory, and a wrong-result driver is
  reported.** On an Arc A380 under the Linux xe driver, a SYCL kernel that keeps
  a private array in memory or spills registers returns wrong values, with no
  error; 25 of the 109 SYCL kernels did. `vif_sycl`'s SIMD-32 kernels now use the
  256-entry register file and no longer spill: forced with the new
  `VMAF_SYCL_VIF_SUBGROUP_SIZE=32`, they scored every frame 0/0 on that device
  and now match the SIMD-16 kernels bit for bit. The first SYCL initialisation on
  each device runs two probe kernels and logs a warning naming the SYCL
  extractors that still use scratch memory when the probes come back wrong
  (`VMAF_SYCL_SCRATCH_SELFTEST=0` skips it); the device is used either way. The
  new `test_sycl_kernel_scratch` fails when a kernel outside
  `core/src/sycl/scratch_ratchet.txt` uses scratch memory. See
  [Scratch memory on Intel GPUs](docs/backends/sycl/overview.md#scratch-memory-on-intel-gpus-adr-1395)
  (ADR-1395, `T-SYCL-XE-SCRATCH-WRONG-RESULTS-2026-10-01`).


- **`motion_sycl` sizes chroma planes correctly for 4:2:2 and 4:4:4 input.**
  With `motion_add_uv=true`, `motion_configure_chroma()` previously assumed
  4:2:0 subsampling (`chroma_w = (w + 1) >> 1`, `chroma_h = (h + 1) >> 1`)
  for all input formats, causing `motion_stage_chroma()` on 4:2:2 and 4:4:4
  input to stage only a sub-rectangle of each chroma plane and normalize the
  SAD by an incorrect area. Chroma dimensions are now derived via
  `vmaf_chroma_extent()` from `picture_geometry.h` according to the pixel
  format, and `test_sycl_motion_add_uv_parity` verifies parity with the
  fixed-point oracle across 4:2:0, 4:2:2, and 4:4:4.


- **SYCL: `motion_sycl` and `motion_v2_sycl` kernels above 15 bpc are scratch-free on Intel Arc under xe (ADR-1395).**
  On the Linux `xe` driver on Intel Arc A380, scratch memory (private arrays and register spills) produces corrupted reads and writes. The 16-bit vertical filtering pipeline (`submit_sad<int64_t>`) spilled 768 B/thread at SIMD-32 due to the 128-register limit, causing `test_sycl_motion_tiny_frames` to fail on 16-bit frames. Implementing `MotionSadHbdKernel` derived from `VmafSyclKernelShape<32, 256>` (`sycl_compat.h`) requests the 256-entry register file, eliminating all spills and private memory (`spill_size: 0`, `private_size: 0`). On physical Arc A380 under `xe`, `test_sycl_motion_tiny_frames` passes (8, 10, and 16-bit across all 9 geometries, bit-exact vs scalar CPU), `test_sycl_motion3_parity`, `test_sycl_motion_add_uv_parity`, and `test_sycl_motion_v2_parity` pass, and 50 frames of 16-bit 4K BBB match the CPU reference bit-for-bit (`T-SYCL-MOTION-HBD-XE-SCRATCH-2026-10-01`).


- **`motion_sycl` emits `VMAF_integer_feature_motion_sad_score` and honours
  `motion_force_zero`, as the CPU `motion` extractor does.** The CPU writes
  the frame's SAD score on every frame (weighted by `motion_fps_weight`,
  capped at `motion_max_val`); the SYCL twin published it only as the debug
  `integer_motion` score, so the result of `--backend sycl --feature motion`
  lacked a key the CPU result has. With `motion_force_zero=true` the twin
  returned the measured `motion2` / `motion3` under the `_force_0` names
  where the CPU returns 0, so the shipped `vmaf_v0.6.1mfz` model scored
  76.668 on `--backend sycl` where the CPU scores 72.321 (Netflix 576x324
  pair). Both are fixed: on an Arc A380 every output of
  116 frames is bit-identical to the CPU under seven option sets, and the
  parity gate's `motion` and `motion_debug` cells now compare the SAD score.
  The kernels and the frame time are unchanged
  ([motion](docs/metrics/motion.md#output-features)).


- **`motion_sycl` matches the CPU `motion` exactly.** The SYCL twin blurred
  each frame and differenced the blurred frames, while the CPU (since the
  upstream pipelined-motion port) blurs the frame difference and rounds after
  each filter pass. The two orders round differently, so `motion2` was up to
  2.0e-4 off on 17x17 frames, 1.3e-5 on the Netflix 576x324 pair and 5.6e-6
  at 4K. `motion_sycl` and `motion_v2_sycl` now run one kernel with the CPU's
  arithmetic and agree with the CPU bit for bit at every size and bit depth
  tested, on an Arc B580 and a UHD 770 (ADR-1371). The 4K motion step costs
  about 11% more device time on both GPUs. With `motion_add_uv=true`,
  `motion_sycl` no longer waits on the device inside `submit()`: the U and V
  planes are staged in pinned memory and uploaded on the compute queue, which
  cuts host time per 4K frame from 5.4 to 0.6 ms on a UHD 770
  ([SYCL backend](docs/backends/sycl/overview.md#motion_sycl-matches-the-cpu-motion-exactly-2026-09-29)).


- **SYCL: `psnr_hvs_sycl` scores 9- and 11-bit input like the CPU.** The twin
  multiplied 9- and 11-bit samples by 16 before scoring them, so through the C
  API a 9-bit clip that the CPU scored at 22.47 dB came out at -1.57 dB
  (11 bits: 33.97 against 10.43). It now reads the raw sample at every bit
  depth, as `psnr_hvs` does; 8-, 10- and 12-bit scores are unchanged. The CLI
  accepts only 8, 10, 12 and 16 bits (`T-SYCL-PSNR-HVS-ODD-BPC-SCALE-2026-09-29`).


- **SYCL: `psnr_hvs_sycl` kernel is scratch-free on Intel Arc under xe (ADR-1395).**
  On the Linux `xe` driver, private memory causes corrupted reads and writes.
  The kernel had 2432 B/thread of private memory at SIMD16 on DG2 (Arc A380)
  due to dynamically indexed `means[4]` and `variances[4]` arrays in
  `hvs_variance_ratio()` and `args.plane[plane]` dynamic indexing in `hvs_locate()`,
  causing ~20 dB divergence at 4K (BBB frame 0 `psnr_hvs_y` 13.13 dB vs CPU
  33.17 dB). Replacing dynamic struct indexing with explicit member branches
  and restructuring quadrant accumulators into scalar members (`HvsQuadrants`)
  eliminates all private memory and register spills (`private_size: 0`,
  `spill: 0`). On Arc A380 under `xe`, scores match CPU reference across 576x324
  (max diff 8.37e-5 dB vs 5e-4 gate), 1080p (max diff 1.71e-3 dB), and 4K BBB
  (frame 0 33.161817 dB vs CPU 33.171624 dB, delta 0.0098 dB; max diff across
  22 frames 1.099e-2 dB). Throughput on 4K BBB improves from 12.55 ms/frame
  (corrupted) to 10.90 ms/frame (correct) (`T-SYCL-PSNR-HVS-XE-SCRATCH-2026-09-30`).


- **SYCL**: Fixed identical/flat-frame handling in `float_ssim_sycl` and `integer_ssim_sycl` by implementing the CPU's exact arithmetic without identical-window shortcuts, grouping integer terms as `((w*a)*b)/den`, and preserving ADR-1370 fp32 frame-mean rounding.
- **SYCL**: Fixed a bug where `psnr_sycl` produced incorrectly scaled scores under `--subsample` by adding the missing `VMAF_FEATURE_EXTRACTOR_TEMPORAL` flag.
- **SYCL**: Fixed a bug where `motion_v2_sycl` diverged from the CPU by applying `motion_fps_weight` and the `motion_max_val` cap in `collect()` and emitting scores for one-frame inputs in `flush()`.


- **Shared SYCL frame buffers re-allocate when geometry changes.** When a single
  `VmafSyclState` was shared across consecutive `VmafContext` instances of different
  frame dimensions or bit depths, `vmaf_sycl_shared_frame_init()` kept the old
  buffer allocations and pitch, resulting in out-of-bounds reads and incorrect
  metric scores. Re-initialization now drains queues and reallocates the shared
  frame and chroma buffers to match the new geometry.


- **`speed_chroma_sycl` and `speed_temporal_sycl` match the CPU with
  `speed_prescale_method=lanczos4`.** Like the CUDA twins before them, they
  evaluated the lanczos4 kernel weights on the device in fp32, where the CPU
  scaler uses fp64 `sin()`, and SpEED amplifies the few-ulp differences on
  smooth content: on an Arc A380 no frame of a 1920x1080 gradient matched at
  `speed_prescale=2.0`. The scale kernel now reads the weights from the table
  the host builds with the CPU scaler's own routine, and nearest, bilinear,
  bicubic and lanczos4 prescale at 0.5 and 2.0 are bit-identical to the CPU
  extractor on that device. The kernels use no scratch memory, so the result
  also holds under the Linux xe driver, and the xe scratch warning no longer
  names the two SpEED twins. Scores with the other three methods, and without
  prescale, are unchanged. The HIP twins still evaluate the weights on the
  device ([SpEED](docs/metrics/speed_qa.md#sycl-device-resident-and-bit-identical-to-the-cpu)).


- **SpEED SYCL kernels are now scratch-free on Intel Arc GPUs (ADR-1395).**
  On the Intel Arc A380 under the Linux `xe` driver, kernel execution that used
  scratch memory (private memory arrays or register spills) caused silent data
  corruption, resulting in singular covariance matrices and zeroed SpEED scores.
  By replacing dynamically-indexed captured plane arrays in `RawPlanes` with
  scalar plane members resolved once per work-item (`RawBound` / `FloatBound`)
  and unrolling bicubic/lanczos weighting loops, all eight SpEED `launch_scale`
  and `launch_decimate` kernels now compile with zero private memory and zero
  register spill (`private_mem_size == 0`, `spill_memory_size == 0`).
  All SpEED parity tests pass, and `speed_gpu_parity.py` achieves bit-identical
  scores (`0.000e+00` max absolute difference) against the CPU reference on
  both 576x324 and 3840x2160 fixtures.


- **`integer_ssim_sycl` returns the CPU's `ssim` bit for bit, and works on
  16-bit input.** The CPU `ssim` extractor computes each pixel's term in
  `double` and adds all terms in one running sum. A SYCL kernel has no
  `double`, and the twin used `float` and added per block, which left the
  score up to 3.1e-7 from the CPU (2.0e-4 with `enable_db`) and made 16-bit
  frames fail with `invalid ratio`. The kernel now performs the CPU's
  `double` operations in 64-bit integers and the host adds the terms in the
  CPU's order ([ADR-1443](docs/adr/1443-sycl-ssim-cpu-arithmetic.md)).
  Measured on an Arc A380 at `--precision max`, every frame is identical on
  the Netflix 576x324 pair at 8, 10, 12 and 16 bits, both 1080p checkerboard
  pairs and 200 frames of BBB 3840x2160, with `enable_db` and `clip_db` too.
  A 3840x2160 frame takes 31.9 ms instead of 17.8 ms (0.78 instead of 0.45 ms
  at 576x324), and the twin holds 66 MB more pinned host memory at that size.
  The parity gate compares this twin with tolerance 0. Stored
  `integer_ssim_sycl` scores change by up to 3.1e-7.


- **`ssimulacra2_sycl` is bit-identical to the CPU `ssimulacra2` extractor.**
  The CPU evaluates six terms per pixel and channel in `double` and adds each
  into one `double`, pixel after pixel. A SYCL device has no `double`, so the
  twin evaluated the terms as pairs of floats and added them in a fixed tree:
  on an Arc A380 none of 266 measured frames (576x324 to 3840x2160, 8 to 16
  bits) equalled the CPU, and the score was up to 7.6e-11 away. The twin now
  computes each term's `double` in 64-bit integers, the CPU's operations one
  for one, and forms the sums with the bits of the CPU's loops, from integer
  increments per binade as the CUDA and HIP twins do (`ordered_sum.h`). All
  266 frames are identical at `--precision max`, with every `yuv_matrix`. The
  parity gate compares the CPU and SYCL `ssimulacra2` cells with tolerance 0
  instead of 5e-3, and the Arc A380's 5e-2 calibration for this feature is
  removed. The twin is slower: 195 ms instead of 84 per 3840x2160 frame and
  13.5 ms instead of 5.3 per 576x324 frame on that device. Stored
  `ssimulacra2_sycl` scores change by up to 7.6e-11
  ([ADR-1446](docs/adr/1446-sycl-ssimulacra2-cpu-bits.md),
  [ssimulacra2](docs/metrics/ssimulacra2.md#sycl-device-resident-one-readback-per-frame)).


- **Every SYCL feature kernel now does fp32 arithmetic the way the CPU
  reference does (ADR-1367).** The SYCL guides said the kernels ran in IEEE-754
  strict mode under `-fp-model=precise`; in fact icpx still fused
  `a * b + c` into one FMA and computed `/` and `sqrt` approximately (29% and
  8% of random fp32 operands differed from the host). Every SYCL feature
  translation unit now compiles with
  `-fp-model=precise -ffp-contract=off -foffload-fp32-prec-div -foffload-fp32-prec-sqrt`,
  and the link carries the precision flags for the SPIR-V image that devices
  outside `sycl_icpx_aot_targets` compile at first launch. Nine twins' scores
  move, each still inside its cross-backend tolerance: `float_adm_sycl` is ten
  times closer to `--backend cpu` (2.5e-5 -> 2.5e-6 on the Netflix pair),
  `float_ssim_sycl` and `integer_ssim_sycl` about twice as close, and
  `ciede_sycl` halves its worst 3840x2160 difference (9.7e-5 -> 4.5e-5);
  `float_vif_sycl`, `float_ms_ssim_sycl`,
  `float_motion_sycl` and `vif_sycl` move within their existing spread, and
  `psnr_hvs_sycl` by at most 1.3e-6 dB. The
  twins that were bit-identical to the CPU stay so, and no twin's 4K cost on
  an Arc B580 changed by more than the run-to-run spread. AdaptiveCpp builds
  keep contraction-off only. See
  [the SYCL backend guide](docs/backends/sycl/overview.md#what-the-sycl-compile-line-guarantees).


- **The SYCL clang-tidy lane measures the SYCL sources again.** Since the
  SYCL build targets record their headers (PR #1764), the script that adds
  the SYCL compile commands to the lint database found 4 of 31 translation
  units: Ninja names a rule with a depfile differently and the script matched
  the old name only. `make tidy-ratchet LANE=sycl` and the changed-file SYCL
  lint job therefore saw none of the kernels. The script reads both rule
  forms, and it now stops with an error when a SYCL source is compiled by a
  command it cannot read, so the lane cannot go blind silently again.


- **The SYCL clang-tidy lane measures the x86 SIMD and DNN sources again.**
  Since the strict floating-point arguments became project-wide (ADR-1461),
  151 translation units of an icx build carry them twice, and stock clang
  answers the repeat with a driver warning that has no source location. The
  ratchet treats such a warning as an unusable measurement, so
  `make tidy-ratchet LANE=sycl` stopped with exit 4 and no baseline entry of
  those files could be tightened. `scripts/ci/clang-tidy-sycl.sh` silences
  that one driver note (`-Wno-overriding-option`); the compile commands and
  every check stay as they were
  (`T-SYCL-TIDY-OVERRIDING-OPTION-2026-10-02`).


- **`vmaf_sycl_upload_plane()` returns only after its copy has completed.**
  It used to return with the host-to-device copy still in flight. Nothing
  ordered the copy before the frame's compute, and the caller could free the
  source while the copy was still reading it. On an Arc A380, 3840x2160 frames
  uploaded with it and read at once scored the wrong pixels on every frame
  (`psnr_y` 4.9 to 6.1 where the CPU gives about 7.1). The function now waits
  for the copy, after ordering it behind the slot's previous readers, so the
  scores equal the CPU's. The Windows D3D11 import, which unmaps its staging
  texture right after the call, uses this function. The SYCL runtime's debug
  environment variables are now read through the thread-safe snapshot helper.


- **A SYCL error while de-tiling a VA surface no longer ends the process.**
  `vmaf_sycl_import_va_surface()` submitted its de-tile copy or kernel outside
  any `try`, so a synchronous `sycl::exception` (a kernel the device cannot
  build, an allocation the runtime cannot make) left an `extern "C"` function
  and terminated the program. The submit is caught now: the import is
  released and the call returns `-EIO`, as the readback path already did.


- **`vif_sycl` rounds its per-scale sums where the CPU does, and emits the
  CPU's default outputs.** The CPU `vif` extractor stores each scale's
  numerator and denominator sum in a `float` and divides in single precision.
  The SYCL twin kept the sums in `double`, so every score of every frame was
  up to 3.5e-7 from the CPU. It now rounds at the same points: measured on an
  Arc A380 at `--precision max`, the denominator sums are identical on every
  frame, and the scores on 12 to 41 of the 48 Netflix 576x324 frames
  (depending on the scale) and on 140 to 196 of 200 BBB 3840x2160 frames,
  where none was before. The remaining frames are one or a few `float` steps
  off in a numerator (at most 3.6e-7 in a score), because the kernel computes
  the per-pixel gain in `float`. The twin's `debug` option now defaults to
  `false`, as on the CPU; it defaulted to `true` and added eleven debug
  outputs to every run. Request them with `--feature vif_sycl=debug=true`.
  Stored `vif_sycl` scores change by up to 3.5e-7.


- **`vif_sycl` returns the CPU's scores bit for bit.** The CPU `vif` extractor
  computes a pixel's gain in `double` and truncates two results to integers
  before its log2 table. A SYCL kernel has no `double`, and the twin used
  `float`, which put a share of those integers one off and left a scale's
  score up to 3.6e-7 from the CPU on some frames. The kernel now computes
  both integers exactly: one integer division decides them, and a pixel whose
  value lies within the `double` chain's own rounding error of an integer
  (one in 300 000) replays the CPU's operations in 64-bit integers
  ([ADR-1432](docs/adr/1432-sycl-integer-vif-exact-gain.md)). Measured on an
  Arc A380 at `--precision max`, every output of every frame is identical on
  the Netflix 576x324 pair at 8, 10, 12 and 16 bits, both 1080p checkerboard
  pairs and 200 frames of BBB 3840x2160, with `debug=true`, with
  `vif_enhn_gain_limit` of 1.0, 1.2 and 37.5, and for a clip scored against
  itself. A 3840x2160 frame takes 0.75 ms longer (21.46 to 22.21 ms). The
  parity gate compares this twin with tolerance 0. Stored `vif_sycl` scores
  change by up to 3.6e-7.


- **`vif_sycl` with `vif_fused=true` returns the CPU's scores.** From 1920x1080
  up, scales 1 to 3 differed from the CPU `vif` on every frame (by up to 4.9e-4
  at 3840x2160): one fused launch read a scale from the downsampled buffers it
  was writing the next scale into. The fused scales now alternate between two
  buffers, which costs 4 MB of device memory at 3840x2160; the default separate
  passes were not affected.


- **SYCL: no kernel uses scratch memory on Xe2 (Arc B580, Arc Pro B60).** On an Arc B580
  the term kernel of `float_adm_sycl` spilled 128 bytes, and
  `test_sycl_kernel_scratch` failed; its scores were still exact there, but a
  kernel in scratch memory returns wrong values on an Arc A-series GPU under
  the xe driver. The kernel now takes the 256-entry register file with the
  sub-group size left to the compiler, and spills on no target of the default
  ahead-of-time list. `test_sycl_float_adm_math` no longer fails at random on
  the B580: its probe ran three dependent kernels on an out-of-order queue.
  With both fixes the whole SYCL device suite and the parity gate pass on the
  B580 and the Arc Pro B60 (ADR-1501, `T-SYCL-FLOAT-ADM-TERMS-XE2-SPILL-2026-10-03`,
  `T-SYCL-FLOAT-ADM-PROBE-OUT-OF-ORDER-QUEUE-2026-10-03`).


- **No SYCL kernel spills on Intel Xe-LP integrated GPUs any more.** Two
  tester reports from UHD 770 GPUs (issues #2116 and #2122) found 12 SYCL
  kernels using scratch memory, which Intel GPUs under the xe driver can turn
  into wrong values. The `ssimulacra2_sycl` slot kernel, the `ssim_sycl` term
  kernel and the scale-0 SIMD-16 horizontal `vif_sycl` kernel now leave their
  sub-group size to the compiler with the large register file, and the
  16-bit `motion_sycl` SAD kernel runs at SIMD-16: none of the four uses
  scratch memory on any of the 19 default ahead-of-time targets, and every
  twin still returns the CPU's scores bit for bit on an Arc A380. The other
  eight were the SIMD-32 `vif_sycl` kernels: they are removed, with the
  `VMAF_SYCL_VIF_SUBGROUP_SIZE` variable that forced them, and `vif_sycl`
  runs at SIMD-16 on every device
  ([ADR-1830](docs/adr/1830-sycl-vif-simd16-only.md)). SIMD-32 was never
  faster on an A380; setting the variable now has no effect.


- **The SYCL zero-copy path names what it cannot score instead of failing
  unnamed, crashing, or scoring wrong.** `vmaf_read_pictures_sycl()`, which
  FFmpeg's `libvmaf_sycl` filter uses on QSV frames, imports the luma plane
  only. On an Arc A380 the default model `vmaf_v1.0.16_3d0h` failed there with
  a bare `-22`; `speed_chroma_uv` needs chroma. `float_psnr_sycl` crashed
  FFmpeg. `motion_sycl` with `motion_add_uv=true` added the SAD of chroma it
  never imported (`integer_motion2_mau` 4.257894 instead of 5.536504). A CPU
  feature was dropped from the result without an error. The call now checks
  every registered extractor before it counts a frame. It returns `-ENOTSUP`
  with a libvmaf error naming each extractor that cannot run on luma alone,
  and the filter tells the user to use `hwdownload` and the `libvmaf` filter's
  `sycl_device` option ([ADR-1688](docs/adr/1688-sycl-zero-copy-luma-only-admission.md)).
  `vmaf_v0.6.1`, the filter's default model, still runs zero-copy, with the
  CPU's per-frame scores. The filter no longer prints `VMAF score: 0.000000`
  after a failed pooled score. The SYCL history and HIP upload pages no longer
  call the default model luma-only.


- **The tester image and the macOS tester bundle carry the licences of what
  they contain.** Each has `licenses/THIRD_PARTY_NOTICES.txt` (the container
  under `/opt/vmafx/licenses/`) with every component, its licence and
  copyright notices, and the licence texts beside it; the VMAFx section is
  computed from the SPDX headers of every source file the build compiled. The
  container's GPL and LGPL parts have their corresponding source published
  next to it as `ghcr.io/vmafx/vmafx:<version>-tester-source`, both packages
  get an attested SPDX SBOM, and both builds fail when a file of the package
  has no recorded licence (`tools/rc1-tester/image/licensing.json`). The image
  no longer strips the libraries the numpy, scipy, scikit-learn and Pillow
  wheels bundle, and the macOS bundle carries the licence texts of the
  libraries linked into its interpreter. The BRISQUE model is recorded under
  the LIVE laboratory's own notice (`LicenseRef-LIVE-BRISQUE`) and `mkdirp`
  under MIT. The rules every tester package follows, including the GPU and
  Windows kits, are
  [ADR-1503](docs/adr/1503-tester-artifact-licensing.md); the audit of the
  packages published before is
  [Research-2133](docs/research/2133-tester-artifact-licence-audit.md)
  ([tester guide](docs/usage/tester-image.md#licences-of-what-you-download)).


- **The macOS tester bundle's unit tests start on the tester's Mac**
  (`T-TESTER-BUNDLE-UNIT-PATHS-ABSOLUTE-2026-10-04`). The bundle's
  `image/unit-tests.json` named every test executable by its absolute path on
  the hosted runner that built it, so on any other Mac no unit test (and none of
  the Metal parity cases the state-row map reads) could start and the report's
  `unit_tests` section failed. Tester package manifests now name tests relative
  to the package root, and the report resolves them against the directory it
  runs from. The bundle published as `tester-20261003-c12763f3` has the defect;
  a newly published bundle does not.


- **The macOS tester bundle build runs under the hosted runner's bash 3.2, and the
  report schema check installs on Python 3.12.** `mapfile` in
  `scripts/ci/build-macos-tester-bundle.sh` is replaced by a loop; a contract test
  scans the macOS scripts for bash 4+ features and runs them under a real bash 3.2 when
  Docker is available. `requirements/locks/jsonschema.txt` is now a universal lock
  (`typing-extensions` for Python below 3.13). See
  [the maintainer notes](docs/development/tester-image.md).


- **The CUDA, SYCL and HIP tester images build again.** The exactness-matrix
  test names `scripts/ci/exact_twin_matrix.py`, which `meson setup` resolves
  for every enabled GPU backend, but the tester image's GPU build stages did
  not copy it, so every GPU image failed at configure time. The four Meson
  stages of `docker/Dockerfile.tester` now copy it, and a test refuses any
  file the Meson tree names outside `core/` that one of those stages lacks.


- **The macOS tester bundle's link check follows `@rpath` and skips install names,
  and both tester publish jobs run in the `tester-publish` environment.**
  `scripts/ci/check-macos-bundle-links.sh` resolves `@rpath` through each file's
  `LC_RPATH`, skips a dylib's own install name and accepts a reference only when it
  resolves inside the bundle or to `/usr/lib` or `/System/Library`; Tcl/Tk is no longer
  bundled. The tester image is published by dispatch on master only. See
  [the maintainer notes](docs/development/tester-image.md).


- **Tester report: a failure names its cause
  (`T-TESTER-REPORT-DROPS-FAILURE-CAUSE-2026-10-05`).** A failed `vmaf` run on a
  fixture used to keep only the last line it printed, often a warning printed
  while closing; its `error` now also carries every `problem ...`, `error: ...`
  and libvmaf `ERROR` / `WARNING` line (at most 20 lines, 4 KB) and the signal's
  name for a crash. A unit-test program that is killed by a signal, times out or
  exits with a failure status without reporting a failing case is named in
  `unit_tests.reason` with the case it was in, and that case is recorded as
  `fail` with `no verdict printed: ...`; a timeout keeps the cases printed before
  it. The report schema is unchanged.


- **The tester image's SPDX SBOM verifies on the digest you pull.** It was
  attested on a per-arch index that `imagetools create` does not publish, so
  `gh attestation verify` on the platform digest of the tag found nothing. It is
  now attested on each platform manifest the tag's index lists, and the
  publishing run verifies it. Commands:
  [tester guide](docs/usage/tester-image.md#licences-of-what-you-download).


- **The macOS tester bundle and the tester image name the version of the tested commit.** The file name and the report's version could read `tester-20261003-c12763f3-18-g2414774ea`, because `git describe` took the nearest `tester-*` release tag; they now read `v1.0.0-rc.2-311-g2414774ea`, the nearest `v*.*.*` tag. The macOS publish job also creates its prerelease as the release-bot identity, since the job token was refused (HTTP 403) when the tested commit was behind master.


- **The Arm64 Windows tester zip builds**
  (`T-TESTER-WINDOWS-ARM64-X64-VCRUNTIME-2026-10-04`). The Arm64 interpreter
  archive carries an x64 `vcruntime140_1.dll` that no program loads, and the
  zip's import check refused it, so the first hosted run published no Arm64
  zip. The build now leaves out every interpreter runtime DLL nothing imports.
  Each zip is also verified when its own build passed, and the build log shows
  the output of a unit test the zip's report counts as failed.


- **The Windows tester zips carry the Visual Studio 2026 licence terms for the
  Microsoft runtime code they ship** (`T-TESTER-WINDOWS-VS-TERMS-UNREAD-2026-10-04`).
  The terms page shows only a title and a date; the terms are a Word document it
  embeds. The licence record now pins that document by URL and SHA-256, the build
  writes its text to `licenses\texts\visual-studio-2026-license-terms.txt`, and the
  notes of both Microsoft components pass on what its Distributable Code section
  asks of a distributor.


- `scripts/dev/tidy-lane.sh --write` takes the baseline only from the run's own results.
  It copied `tidy-baseline-<lane>.json` from the shared report directory, so a run that
  wrote none could overwrite the checkout's baseline with another run's. A write that
  produces no baseline now leaves the file alone and exits non-zero.


The clang-tidy ratchet fails, naming the file, when a translation unit of the baseline was not measured, instead of counting it as clean; the hosted `cpu` lane runs the same Makefile targets as the dev container, so both measure the same translation units.
- **A cross-device parity run that compared nothing passed** (`T-TINY-AI-CROSS-DEVICE-PARITY-UNGATED-2026-09-25`).
  `vmaf_train.cross_backend.CrossBackendReport.ok` is now False when no output was compared, when a requested
  provider is missing, or when ONNX Runtime accepted a provider but ran the session on the CPU, so
  `vmaf-train cross-backend --fail-on-mismatch` no longer exits 0 on a CPU-only host. New
  `scripts/ci/tiny_ai_cross_device_parity_gate.py` checks `vmaf_tiny_v2` (1e-4) and `smoke_fp16_v0` (1e-2)
  between two providers and names the missing provider; it is not yet wired into a hardware job.


- **Feature-vector tiny models score the features the run computed.** A
  `--tiny-model` such as `vmaf_tiny_v2` or `fr_regressor_v1` read its input
  features from whatever the run happened to compute and took a missing one as
  0: with the default model alone `vmaf_tiny_v2` printed -0.853 on every
  frame, and even with `--feature adm --feature vif --feature motion` every
  frame after the first read `motion2 = 0`. Loading the model now registers the
  extractors of the features its sidecar names, the model is scored once the
  run is flushed, and a frame that lacks an input fails the run with a message
  naming it; a sidecar that names an unknown feature, or the wrong number of
  them, fails at load
  ([ADR-1520](docs/adr/1520-tiny-model-feature-inputs-at-flush.md)).
  Codec-aware models (`fr_regressor_v2`, `fr_regressor_v3`) no longer score a
  guessed codec block: they need `--tiny-codec` and `--tiny-crf`
  (`vmaf_dnn_set_codec_context()` in the C API) and stop on the first frame
  without them, and the `fr_regressor_v2_ensemble_v1_seed*` models, whose
  sidecars describe another codec block than their graphs take, are refused
  at load. Breaking for C API callers: tiny feature-vector scores exist only
  after `vmaf_read_pictures(ctx, NULL, NULL, 0)`.


- **Tiny-model metadata matches the shipped graphs, and CI keeps it so.**
  `nr_metric_v1` recorded opset 17 for files that import 18; the
  `fr_regressor_v2` notes described an 8-D codec block for a 14-wide input and
  the trainer's defaults built a smaller model than the shipped 3 x 32 one; the
  five `fr_regressor_v2_ensemble_v1_seed*` sidecars described other graphs;
  `transnet_v2.json` named an output the graph does not have; and
  `registry.schema.json` described a runtime digest check that does not exist
  and rejected `release_url`. The metadata now follows the graphs, the
  exporters record the opset the file imports, the `fr_regressor_v2` trainer
  defaults to `--hidden 32 --depth 3`, and
  `ai/scripts/validate_model_registry.py` reads every registered graph (no
  `onnx` package needed) and fails on a mismatch
  ([ADR-1546](docs/adr/1546-tiny-model-metadata-against-graphs.md)). The
  `ai/scripts/build_calibration_set.py` stub is removed; `vmaf-train
  quantize-int8` calibrates static PTQ from a parquet feature cache.


- **The vmaf-tune tools build FFmpeg command lines that do what they say.**
  Repeated `-x265-params` (two-pass stats, saliency zones, HDR SEI) or
  `-x264-params` / `-svtav1-params` / `-vvenc-params` options are joined into one
  (FFmpeg keeps only the last, so a pass-2 encode lost its stats file or its
  zones); the per-shot probe and signalstats passed the shot's start frame
  index to `-ss`, which reads seconds, and now convert it with the frame rate;
  `hevc_nvenc` no longer gets `-master_display` / `-max_cll`, which FFmpeg
  rejects and which aborted the encode; `/dev/null` became `os.DevNull`; the
  saliency check keys on the ROI keys instead of any `-x265-params`. The
  `libvmaf_cuda` recipes convert NVDEC's NV12 with `scale_cuda` (the filter
  accepts `yuv420p` and `yuv444p16` only).


- **The TransNet V2 exporter pins a commit that exists.** `ai/scripts/export_transnet_v2.py`, the
  `transnet_v2` sidecar, its registry `license_url` and the model page named upstream commit
  `77498b8e`, which returns 404. They now name `a0942ca347ee00aa455631147641954278b1d1a5`, the
  commit that added the weights; its Git LFS object ids are the exporter's two pinned SHA-256
  values. The shipped ONNX file and every score are unchanged.


- **`--feature transnet_v2` opens the shipped model and detects cuts.** The
  extractor failed to open `model/tiny/transnet_v2.onnx` (`-34`: its rank-5
  input exceeded the session's shape probe, and the output it bound had another
  name). Two more faults hid behind that: thumbnails were scaled to 0..1 where
  the network expects 0..255, and each frame's probability was read from the
  window's last slot, which has no later frame to compare with; either alone
  kept a hard cut below 0.1. The extractor now runs upstream TransNet V2's
  `predict_frames()` windows (100 frames, stepping by 50, the middle 50
  reported, padding at both ends), so a frame's `shot_boundary_probability` and
  `shot_boundary` appear up to 74 frames after it is read and the last ones at
  flush; a `1.0` marks the last frame of a shot
  ([ADR-1527](docs/adr/1527-transnet-v2-upstream-windows.md)). Frames must
  arrive in order without index gaps.


- **`vmaf_init()` accepts an uninitialised handle again, as upstream libvmaf
  does.** Since ADR-1032 it returned `-EINVAL` whenever `*vmaf` was not NULL.
  Callers written against upstream, whose own CLI and tests declare
  `VmafContext *vmaf;` without an initialiser, failed at random depending on
  what the stack held. Upstream's `test_context.c` failed 3 of 3 runs.
  `vmaf_init()` no longer reads `*vmaf`: it sets it to NULL on entry and to
  the new context on success (ADR-1396). A handle that still holds an open
  context is now overwritten instead of rejected; close it first.


- **`vmaf-tune auto --smoke` runs without `--src`.** The smoke planner probes
  nothing, but the subcommand required `--src` and exited 2 before reading
  `--smoke`. `--src` is now required unless `--smoke`, and always with
  `--execute`; a smoke plan without a source records an empty `"src"`, as
  `vmafx-tune auto --smoke` does. See `docs/usage/vmaf-tune-auto.md`.


- **`vmaf-tune` checks the coarse-to-fine window against the adapter, honours
  `ladder --workdir` and `--max-concurrent-decodes`, gives `auto --execute` the
  source geometry, and names a missing uncertainty interval.** The
  coarse-to-fine search used libx264's fixed 10..50 window for every encoder
  and died with a traceback for libx265, libsvtav1, libvvenc, AMF and ProRes;
  it now searches the part of
  each adapter's `quality_range` inside that window, refines toward the right
  side for `-q:v` adapters, and refuses a window the adapter rejects before the
  first encode (exit 2). `ladder` puts each rung's scratch directory under
  `--workdir` and caps reference decodes with `--max-concurrent-decodes`.
  `auto --execute` probes a container source's width, height, frame rate and
  pixel format and takes `--width/--height/--framerate/--pix-fmt` for raw YUV,
  refusing raw YUV without them. `recommend --with-uncertainty` on a corpus
  without interval columns prints `uncertainty=unavailable` and a stderr note
  instead of answering silently without intervals, and reports
  `rows_examined=N/M`.


- **`vmaf-tune` help texts match the code, its ADR references name the right
  records, and `fast` says when the proxy scores an encoder as `unknown`.**
  `ladder --crf-sweep` names the sampler's sweep (`20,25,30,35,40`), the
  `auto` help counts its ten short-circuits, `corpus --two-pass` lists the five
  adapters that run a 2-pass encode, and `compare` / `tune-per-shot --workdir`
  cite ADR-0598. A production `fast` run with an encoder outside the proxy's
  vocabulary (`libaom-av1`, AMF, VideoToolbox) now notes on stderr that the
  proxy used its `unknown` slot and adds `"proxy_encoder_slot": "unknown"` to
  the JSON. ADR numbers in the help, the usage pages and the code comments that
  pointed at renumbered, unrelated records now point at the vmaf-tune records
  they meant.


- **`vmaf-tune`'s predictor trainer exports ONNX through the shared exporter.**
  `predictor_train._export_onnx()` called the TorchScript exporter, deprecated
  since torch 2.9, and failed under warnings-as-errors with torch installed.
  It now calls `vmaf_train.models.exports.export_to_onnx()` like the other tiny
  model trainers: torch.export based, dynamic batch axis, op-allowlist and
  onnxruntime round-trip checks. The exported graph keeps the input name
  `input` and the output name `vmaf`; its batch axis is now dynamic.


- **`vmaf-tune` and `vmafx-tune-go recommend-saliency` accept frame heights that are
  not a multiple of 8.** Both tools refused them (a 576x324 clip included) before
  running the saliency model, although they already zero-pad the tensor to a multiple
  of 32 and crop the map back; the shipped `saliency_student_v1` was run at 576x324,
  8x8, 4x4 and 1x1 with that padding. The map has the frame's own shape. Frames that
  were accepted before score exactly as before
  ([ADR-1540](docs/adr/1540-saliency-pad-to-multiple-of-8.md) follow-up).


- **`vmaf-tune` honours `--vmaf-model` and `--neg`, keys its cache on every
  input, gives real QSV encodes their device chain, names each ladder rung's
  codec, and emits AMF's rate control once.** `corpus` and live `recommend`
  scored every row with the height-rule model whatever `--vmaf-model` or
  `--neg` said, and `ladder --neg` was ignored; an explicit `--vmaf-model` now
  scores every row with it, `--neg` takes the NEG variant of the model that
  applies, and stderr names the choice. The encode cache now keys on the
  adapter and ffmpeg versions, the pass count, the sample-clip window, the
  geometry, the model and the backend (old entries miss), and a hit replays the
  miss row. Every QSV encode (Python and `vmafx-tune-go`) carries the VA-API /
  QSV device chain and the upload filter, with the QSV device as the filter
  device, and `corpus`, `recommend` and `ladder` probe a hardware encoder,
  VideoToolbox included, before the first encode (exit 2 when the host cannot
  run it). The HLS and DASH writers name each rung's RFC 6381 codec string,
  read from a two-frame encode, instead of `avc1.640028`; VVC and ProRes
  ladders refuse those formats. AMF encodes carry `-quality / -rc / -qp_i /
  -qp_p` once.


- **`vmaf-tune`'s test suite passes from a plain `pip install -e
  "tools/vmaf-tune[dev]"`.** The `dev` extra now holds matplotlib and ONNX
  Runtime, which `vmaf-tune report` and the ONNX-backed features import but
  the package declared nowhere, and new extras `report`, `onnx` and `train`
  name them for users. Four tests that pinned the old `vmaf.c` /
  `cli_parse.c` sources, ran an upstream `vmaf` from `PATH`, or swallowed
  their own failure now test the current code, and each test runs in its own
  working directory, so the suite no longer leaves a `-version` file behind.


- The vmaf-tune Python tests no longer start the host's `vmaf` through the
  backend probe (a suite-wide fixture keeps the default probe off `PATH`), and
  `go test ./pkg/fast/` runs the vmaf CLI of the build under test
  (`VMAF_BIN` or `core/build-cpu`) instead of `vmaf` on `PATH`.


- **`vmaf-tune corpus --two-pass` encodes with `libx265`.** x265 refuses
  `-crf` in the second pass (exit 183), so every libx265 two-pass cell failed.
  A cell at a CRF now runs pass 1 at that CRF, measures the bitstream with
  `ffprobe` and runs pass 2 as ABR at that bitrate; the corpus row's
  `extra_params` ends with `-b:v <kbps>k` and its `crf` stays the pass-1 CRF, so
  a reader can tell the row belongs to an ABR encode. A missing bit rate fails
  the cell rather than guessing one. The Go `vmafx-tune-go` does the same
  (ADR-1565).


The vmaf-tune backend probe reads an injected runner's report whether or not the host has a `vmaf` on `PATH`; the test of that seam passes on the hosted runner again.


- **`vmafx-tune-go ladder` scores each rung with the VMAF model its height selects.**
  It passed no `--model`, so a 2160p rung was scored with the default 1080p model;
  the Python `vmaf-tune ladder` uses `vmaf_v1.0.16_1d5h_2160` from 2160 lines up
  (ADR-0289). The Go rule and the Python rule now read one golden table in their
  tests, so they cannot drift. Ladders with no rung of 2160 lines or more score as before.


- **`vmafx-tune-go compare` and `ladder` score their encodes, `ladder` encodes
  each rung at its own resolution, and usage errors exit 2.** The scorer handed
  `vmaf` the Matroska encode, which it cannot read, so every probe failed while
  both commands exited 0; it now decodes the encode (and a container reference)
  to Y4M first and refuses a raw `.yuv` reference, and bitrates are read from
  the container when the stream carries none. Each `ladder` rung now encodes
  with `-vf scale=W:H` and scores against the reference scaled the same way, as
  the Python `vmaf-tune ladder` does; a ladder in which no cell scores exits 2
  instead of writing an empty ladder. `auto --smoke` no longer needs `--src`.
  An unknown or unparseable flag and a missing required flag exit 2 on every
  subcommand, as in the Python CLI (they exited 1 outside `benchmark`,
  `encode-profile` and `sidecar`).


- **VPL decode retry ceiling contract and warning frame drop repair**:
  `vmaf_vpl` decode retry loop is formally verified under the 60,000-attempt
  bound on physical Intel Arc A380 hardware and hermetic unit tests. Frames
  published alongside warning status codes (`sts > 0 && sync != NULL`, e.g.
  `MFX_WRN_VIDEO_PARAM_CHANGED`) are now delivered instead of dropped, and
  transient `MFX_WRN_ALLOC_TIMEOUT_EXPIRED` retries cleanly.


- **The Windows ARM64 MSVC build links again.** A unit test added on
  2026-10-01 called `pthread_self()` and `pthread_equal()`, which the Windows
  thread shim does not define, so that build failed at link time on every
  commit since. The test now identifies the calling thread portably, and a
  contract test rejects pthread calls the shim lacks.


- **A Windows checkout passes the governance hooks on unmodified files.** The
  archetypes `.standards.lock` pins, `.standards.*`, `AGENTS.md`, its compiled
  agent-context files and the agent personas are now checked out with LF on
  every platform (`.gitattributes`). `* text=auto` had given them CRLF on
  Windows even with `core.autocrlf=false`, so the `hiss-audit` hook failed the
  lockfile digest and `context-check` reported `CLAUDE.md` out of sync. The
  Windows setup guide in `docs/development/pre-commit-hooks.md` now clones with
  `core.eol=lf` as well.


- **The `Windows Lefthook Pre-Commit` check no longer fails on every pull
  request.** The job runs the hooks in a scratch clone of the checkout, and a
  clone maps the checkout's local branches to `origin/*`. A pull-request
  checkout has no local `master`, so the always-run research-digest ID hook
  could not resolve `origin/master` and failed ("Needed a single revision"),
  which also turned the pull request's Required Checks Aggregator red. The
  step now copies the checkout's remote-tracking refs into the scratch clone.


- **The native Windows CUDA build compiles again, with one MSVC toolset.**
  NVCC's host compiler was the first `cl.exe` a recursive search of the
  Visual Studio install found, an older toolset (14.29 in Visual Studio 2026)
  than the one building the rest of the library; its standard library cannot
  compile the C++20 `<numbers>` header the CUDA `ciede` kernel uses. NVCC now
  uses the build's own `cl.exe` when the build compiles with MSVC, otherwise the
  newest installed toolset (`docs/getting-started/building-on-windows.md`).


- **Windows MSVC+SYCL compiles the exact-arithmetic SYCL headers again, and two
  contract tests pass on Windows.** A `max()` macro from `<windows.h>` broke
  `std::numeric_limits<float>::max()` in a SYCL header, and two Python tests
  looked up backslash path keys with forward-slash names.


- **SYCL: the native Windows build runs its kernels.** A Windows MSVC build
  linked `vmaf.exe` and the tests with `link.exe`, which ignored `-fsycl` and
  never registered the SYCL device images, so every SYCL kernel submit failed
  with `No kernel named ... was found` and 47 of the 50 SYCL tests failed on an
  Arc B580. The build now runs one `icpx -fsycl -fsycl-link` step over the SYCL
  objects and links its registration object into every program that uses the
  SYCL backend, including static consumers of `vmaf.lib`. On an Arc B580 and a
  UHD 770 all SYCL tests pass and all 19 SYCL extractors agree with the CPU
  within the parity gate. The `Windows MSVC+SYCL` CI leg now checks that the
  kernels are registered, and [SYCL on Windows](docs/backends/sycl/windows.md)
  documents the native build (ADR-1364, `T-SYCL-WINDOWS-MSVC-KERNELS-UNREGISTERED-2026-09-29`).
- **CI: Windows test lanes gate their tests again.** `scripts/ci/run_meson_test.py`
  replaced itself with `meson test` through `os.execvp`, which on Windows starts
  Meson and ends the runner with status 0 at once. The Windows MinGW64 and ARM64
  MSVC lanes reported success after 17 to 19 tests, over failing ones. The runner
  now waits for Meson on Windows and returns its status; four Windows test-harness
  failures it had hidden are fixed (`T-CI-WINDOWS-MESON-TEST-RUNNER-EXIT-0-2026-09-29`,
  `T-TEST-WINDOWS-HARNESS-MASKED-FAILURES-2026-09-29`).
- **Parity gate: the `cambi` cell compares scores.** `cross_backend_parity_gate.py`
  and `cross_backend_vif_diff.py` looked the score up as `Cambi_feature_cambi_score`,
  but `vmaf --json` writes it as `cambi`, so the cell stopped with `KeyError`
  (`T-CI-PARITY-GATE-CAMBI-KEY-2026-09-29`).


- The x86 AVX2 level now requires FMA (CPUID leaf 1 ECX bit 12) as well as AVX2,
  BMI1 and BMI2. Two AVX2 kernels are built with `-mfma`, so a virtual machine or
  emulator that reports AVX2 and masks FMA used to fault on them; it now runs the
  SSE paths with the same scores. `docs/backends/x86/avx512.md` lists what each
  level requires.


- **The CPU extractors of an Intel-compiler build match a GCC build where
  they use SIMD.** The two general x86 SIMD libraries were built without
  `-ffp-contract=off`, so `icx` (which every SYCL build uses) turned plain-C
  arithmetic in the tails of SIMD kernels into fused multiply-adds. In
  `ssim_avx512.c` that moved a score: on an AVX-512 host the CPU
  `float_ms_ssim` of an icx build differed from a GCC build and from its own
  scalar path by one fp32 unit in a per-scale mean, 7.7e-9 to 1.4e-8 in the
  score, on 4 of 104 frames (Netflix 576x324 pair, 1080p checkerboards, BBB
  3840x2160). `adm`, `vif` and `speed` files were contracted too, with no
  score difference measured. Every x86 SIMD library is now built with the
  strict floating-point flags
  ([ADR-1415](docs/adr/1415-x86-simd-libraries-strict-fp.md)). GCC builds
  are unchanged: the disassembly of all 28 objects is identical with and
  without the flag. `test_ssim_x86_simd` compares the AVX2 and AVX-512 SSIM
  kernels with the scalar reference bit for bit, and `test_integer_adm_simd`,
  which failed on icx builds since #1700 because its own translation unit
  carried no FP flag, passes again. An icx build still differs from a GCC
  build in `psnr`, `psnr_hvs`, `ciede` and `speed_chroma` (at most 7.1e-15,
  7.1e-15, 5.7e-12 and 1.2e-6) because it calls Intel's math library.


### Security

- **vmafx-controller enforces roles on every gRPC call
  ([ADR-1518](docs/adr/1518-controller-grpc-authorization.md)).** The roles of
  the auth gateway gated only HTTP `POST /v1/score`; any valid token could call
  every gRPC method, including `SubmitJob`, `CancelJob` and the node API. Each
  call now needs a role from the token: `vmafx:reader` for `GetJob`,
  `StreamJobs` and `Health`, `vmafx:writer` for `SubmitJob`, `CancelJob`,
  `Score` and `ScoreStream`, `vmafx:admin` for `RegisterNode`, `Heartbeat`,
  `PullWork` and `ReportResult`; other tokens get `PERMISSION_DENIED`
  (`role required: ...`). A method missing from the role table is refused for
  every caller. Clients with auth enabled need tokens carrying these roles
  (`VMAFX_CONTROLLER_TOKEN` of `vmafx-mcp`: writer to submit and cancel).


- **vmafx-controller refetches its JWKS keys and hides why a token was refused
  ([ADR-1519](docs/adr/1519-controller-tenant-registry.md)).** A key the
  identity provider withdrew kept verifying tokens until the controller
  restarted; keys are now refetched after 15 minutes (and kept for at most
  24 hours while the endpoint fails), failed fetches are rate-limited like
  successful ones, and an `https` JWKS fetch no longer follows a redirect to
  plain `http`. A refused gRPC call now gets the fixed message `invalid or
  missing token` instead of the verification error, which named the
  configured issuers.


- **vmafx-controller: compute nodes use a dedicated `vmafx:node` role, and
  `vmafx:admin` no longer reaches the node API
  ([ADR-1563](docs/adr/1563-controller-node-role.md)).** `RegisterNode`,
  `Heartbeat`, `PullWork` and `ReportResult` need `vmafx:node`, which reaches
  no other call; a node's token can no longer read, submit or cancel jobs or
  score, and an administrator token can no longer register a node. The
  `VmafxTenant` CRD offers `vmafx:node` in `rbac.allowedRoles` (not as
  `defaultRole`). **Migration:** issue node tokens with `vmafx:node` instead
  of `vmafx:admin`, and add `vmafx:node` to `allowedRoles` of every tenant
  that runs nodes; until then nodes are refused with
  `role required: vmafx:node`. `VMAFX_AUTH_DISABLED=true` holds both roles.


- **Each tenant scores only inputs under its own scoring roots
  ([ADR-1577](docs/adr/1577-scoring-paths-per-tenant.md)).** `Score`,
  `POST /v1/score` and `SubmitJob` took any path, URL or rclone remote, so on
  shared storage one tenant's writer could score another tenant's media or
  any file the controller or a node can open. An input must now lie under one
  of the caller tenant's roots (`VmafxTenant.spec.scoring.roots`, or
  `VMAFX_SCORING_ROOTS` with `{tenant}` without a tenant registry; Helm
  `auth.tenants[].scoring.roots` / `auth.scoringRoots`). `..` is refused and
  symlinks are resolved where the files are read: on the controller for
  direct scoring, on the node for jobs (the controller sends the roots with
  each job). **Migration:** a controller without roots refuses every input
  (deny by default); configure the roots before upgrading.


- **vmafx-controller scopes every job read and every node session to the
  caller's tenant
  ([ADR-1522](docs/adr/1522-controller-tenant-scoped-reads.md)).**
  `StreamJobs` (and the `list_jobs` MCP tool on top of it) returned every
  tenant's jobs; it now returns only the token's tenant's. A node session
  belongs to the tenant whose token registered it: `PullWork` gives the node
  only that tenant's jobs, and the session is refused with another tenant's
  token. `ReportResult` accepts a result only for a job assigned to the
  reporting node, or for a running job of the same tenant whose node is gone
  (a node that registered again after a controller restart); before, any
  node could complete any job, a pending one included. Cross-tenant refusals no longer name the owning tenant. A
  deployment serving several tenants needs a node registration per tenant.


- Go CI runs govulncheck at symbol level (`make govulncheck`,
  `scripts/ci/govulncheck-gate.py`, ADR-1899): a called vulnerable symbol fails
  the build, and an advisory whose code is required but never called needs an
  OpenVEX statement in `security/vex/go.openvex.json`. GO-2026-5932
  (`golang.org/x/crypto/openpgp`, no fixed version) is recorded as not affected:
  no vmafx binary compiles those packages.


- **Only the controller can read `VmafxTenant`s
  ([ADR-1592](docs/adr/1592-helm-split-service-accounts.md)).** With a tenant
  registry the chart bound the tenant reader Role to the service account the
  server, job and node pods share, and the operator's ClusterRole granted
  cluster-wide `vmafxtenants` access for a reconciler that does not exist. The
  controller now runs under its own account, `<name>-controller`, the only one
  bound to the Role, and the operator's tenant rules are gone.


- **The single-maintainer gaps of OpenSSF Scorecard are declared exceptions (ADR-2126).**
  `.config/lint-exceptions.d/scorecard-code-review.toml` and
  `scorecard-branch-protection.toml` name why Code-Review (alert 1) and
  Branch-Protection (alert 1054) cannot be met by a one-maintainer project that
  lands through the local merge train, and expire on 2027-03-31 or earlier. No
  ruleset or protection setting changes.


- The nine PyTorch advisories without a fixed release (PYSEC-2025-189, -190,
  -192 to -197, -210) no longer reach any runtime package, and
  `security/vex/torch.openvex.json` records why the two training packages are
  not affected. `scripts/ci/check-torch-scope.py` keeps torch out of every other
  package; the triage process is in `docs/development/dependency-advisories.md`.

## [1.0.0-rc.2] - 2026-09-28
### Changed

- The native Linux release bundle (`vmaf` and `libvmaf.so*`) now runs on
  Ubuntu 24.04, Debian 13 and newer distributions: it needs glibc 2.38 and the
  libstdc++ of GCC 12 instead of glibc 2.43. It is compiled on the fork's
  Debian 13 release track, the base of the published container images, and
  each release checks it on Ubuntu 24.04 and in the distroless `cc-debian13`
  runtime image. Ubuntu 22.04 and Debian 12 remain unsupported (ADR-1354).


- **The first-release candidate plan moved back by one candidate.**
  `v1.0.0-rc.2` is a stabilisation candidate: it ships the dependency updates
  and fixes merged since rc.1, and testers use the same report kit
  (`tools/rc1-tester/`) and exit bar as for rc.1. Benchmarking, profiling and
  tuning move to `v1.0.0-rc.3`, and the one-shot model retrain moves to
  `v1.0.0-rc.4`, so each phase number now matches its tag. The release guide,
  roadmap, tester guide, retrain runbook and `vmaf-rc1-report list-tools`
  inventory show the new mapping (ADR-1352).


### Fixed

- `--model` and `--feature` values keep their backslashes, so Windows paths work
  as typed: `path=..\..\models\m.json`, `path=\\server\share\m.json` and
  `path=C:\models\.cache\m.json` used to lose a backslash each (`\.` and `\\`
  were escapes in values too). `\:` and `\=` still escape a delimiter, and a
  backslash run directly before `:` or `=`, or at the end of a value, is read in
  pairs so a backslash in front of a delimiter stays writable. Keys and
  overload names keep the full escape set (ADR-1355). If you wrote a UNC path
  as `\\\\server\share` per the earlier advice, write `\\server\share` now.


- Container publication finishes for the large images. The GPU image jobs free
  runner disk before `syft` scans the pushed image (the 1.0.0-rc.1 ROCm SBOM
  failed with "no space left on device" after the image was pushed and
  signed), and the two-platform `vmafx-operator` build gets 60 minutes instead
  of 30. The CPU, MCP server, node and controller images no longer compile
  libvmaf's unit-test suite, which they never shipped: the arm64 builds were
  cancelled at 60 minutes while still linking tests under emulation.


- **Nightly Kubernetes E2E scores again** — the kind + kuttl scoring smoke sent
  a 64x64 clip to `/v1/score`. Since `vmaf_v1.0.16_3d0h` became the default
  model (ADR-1169), libvmaf refuses input that small (`cambi` needs one side of
  at least 216 pixels, `speed_chroma` needs 4:2:0 luma of at least 160x160), so
  the server answered HTTP 500 and every scheduled E2E run from 2026-09-24 on
  failed. The fixtures are now 216x160, the smallest size the default model
  accepts, and the test ConfigMap is created with server-side apply because the
  larger pair exceeds the 256 KiB annotation that client-side apply writes. The
  always-on E2E contract test now checks the fixture size against the
  thresholds in `core/src/feature/` on every pull request. On failure the score
  script now prints the `/v1/score` error body and the server Pods' logs; it
  previously discarded the body and, through `deployment/vmafx`, printed the
  operator's logs instead.


- The FFmpeg patched by `ffmpeg-patches/0019` builds without warnings on
  aarch64. GCC 14.2 flagged two `-Wstringop-overflow` false positives in
  `libavcodec/a64multienc.c`, which the node image's warning gate rejects, so
  the arm64 `vmafx-node` image could not build. The index tables are now
  filled per palette interval, with identical results. An image recovery run
  also takes `ffmpeg-patches/` from the dispatching commit (ADR-1350).


- **ffmpeg calls work with FFmpeg 9 again**: FFmpeg 9 removed the `-vsync`
  option, so the Python harness's decode step and the `describe_worst_frames`
  MCP tool (Python and Go servers) failed with "Unrecognized option 'vsync'"
  on the FFmpeg release this project pins. They now pass
  `-fps_mode passthrough`, the same mode as the old `-vsync 0`, which makes
  FFmpeg 5.1 the oldest release they work with. The harness change ports
  Netflix/vmaf `aeaf2877d`.


- The `vmafx-operator` and `vmafx-server` release images build each
  architecture on its own native runner, like `vmafx-node` (ADR-1349). Their
  arm64 halves were emulated with QEMU and took 30 to 46 minutes of a 60-minute
  limit; each still publishes one signed, attested multi-arch image.


- **Helm: the server Deployment no longer selects the operator and node Pods.**
  The chart's server Deployment and StatefulSet selected only the release
  labels, so they also matched the operator, node and `helm test` Pods, and
  `kubectl logs deployment/vmafx` could print the operator's log. Both now
  also select `app.kubernetes.io/component: server` (ADR-1353). Scoring
  traffic was not affected: the Services already selected the server Pods
  only. **Upgrade note for v1.0.0-rc.1 installs:** a workload's selector
  cannot be changed in place, so `helm upgrade` fails with
  `spec.selector: ... field is immutable`. Delete the server workload first;
  `--cascade=orphan` keeps its Pods serving until the upgrade replaces them:
  `kubectl delete deployment,statefulset -n <namespace> --cascade=orphan -l app.kubernetes.io/instance=<release>,app.kubernetes.io/component=server`,
  then run `helm upgrade` as usual. Uninstalling and installing again also
  works. See "Upgrading from 1.0.0-rc.1" in
  `docs/development/k8s-deployment.md`.


- A published release's container images can be recovered after a build
  recipe fix (ADR-1347). A `workflow_dispatch` of the image publish workflows
  on the default branch builds the release tag's source with that commit's
  `docker/` recipe and labels each image with `io.vmafx.build-recipe`; the
  dispatch path also reads the prerelease flag from the release itself, so it
  works for release candidates.
  The release guide now covers the `release-publish` environment step a
  recovery run needs (the environment admits only `v*` tags, so `master` is
  allowed for the recovery and removed afterwards) and how to make a new GHCR
  package public, which the REST API cannot do.
  The post-push smoke tests verify each image's signature against the
  identity of the run that signed it; they required the tag identity, which a
  recovery run cannot produce, so the first v1.0.0-rc.1 recovery failed its
  CPU smoke test after pushing and signing the image. The release guide shows
  how to verify a recovered image (`@refs/heads/master` identity and its
  `io.vmafx.build-recipe` label).


- `test_meson_secret_env_sanitization` passes when Meson is installed with
  `pip install --user`, which `scripts/setup/ubuntu.sh` and the nightly
  ThreadSanitizer job both do. Its probes replaced `HOME` with a temporary
  directory, which also moved Python's per-user package directory, so every
  probe stopped at `No module named 'mesonbuild'` before Meson ran. The probes
  now keep `PYTHONUSERBASE` pointed at the real user base while `HOME` stays
  synthetic.


- The `vmafx-node` image bundles rclone at `/usr/local/bin/rclone` again, as the
  storage guide states. The node resolves `s3://`, `gs://`, `rclone://` and
  `remote:path` inputs by running rclone (ADR-0719), but `docker/Dockerfile.node`
  never installed it, so every remote input failed. The static binary comes
  from the official rclone image, pinned by digest in `build-config.env`, and
  the release smoke test now runs it.


- The `vmafx-node` release image builds again. Its arm64 half compiled FFmpeg,
  libvmaf and the node binary under QEMU emulation and never finished within
  the job's two-hour limit, so no node image was published for v1.0.0-rc.1.
  Each architecture now builds on its own native runner, and the release
  publishes one merged multi-arch image, signed, attested and with an SBOM
  (ADR-1349).


- Release candidates after `1.0.0-rc.1` are numbered `1.0.0-rc.2`,
  `1.0.0-rc.3`, and so on (ADR-1348). release-please used its default
  versioning, which turned the first fix after `1.0.0-rc.1` into a proposed
  `1.0.1-rc.1` (release PR #1575); it now uses `prerelease` versioning, and the
  final cut becomes `1.0.0` once `prerelease` is switched off. The release guide
  also no longer claims that every new GHCR package starts private: with the
  organization's public-package setting on, a package first pushed from this
  repository is created public.
- The container quick start works for release candidates: it names the
  release tag instead of `latest` (release candidates are never tagged
  `latest`) and passes `--pixel_format 420` instead of the rejected `yuv420p`.
  The image docs list the `-rocm10` variant and the exact signing identities
  for recovered images, and a recovered image's
  `org.opencontainers.image.revision` label names the tag's source commit
  rather than the recipe commit it was built with.
- The container images carry the built-in models again: the CPU, MCP-server,
  CUDA and oneAPI builders lacked `xxd`, so libvmaf silently embedded no model
  and scoring without `--model` failed. The oneAPI image now installs the
  Unified Memory Framework runtime its SYCL adapters need; without it the image
  found no SYCL device. The publish smoke tests now score with the default model
  and check the oneAPI adapters, and the GPU image docs give working device and
  group flags, forced-backend scoring examples and measured parity figures.


- The native Linux `vmaf` CLI attached to a release runs next to the
  downloaded `libvmaf.so*` files without `LD_LIBRARY_PATH`. The
  `v1.0.0-rc.1` CLI kept Meson's build-tree RUNPATH `$ORIGIN/../src`, so it
  found `libvmaf.so.3` only when `LD_LIBRARY_PATH` pointed at the download
  directory. The release build now sets the staged CLI's RUNPATH to exactly
  `$ORIGIN`, and the release gate runs the CLI without `LD_LIBRARY_PATH` and
  rejects any other RUNPATH.


- libvmaf builds against libc++ 23 again. The vendored libsvm
  (`core/src/svm.cpp`) defined its own global `swap` template, and libc++ 23's
  `std::vector` internals now call `swap` unqualified, so both it and
  `std::swap` matched and the file failed with "call to 'swap' is ambiguous".
  libsvm now uses `std::swap`; scores are unchanged.


- **Whole-tree clang-tidy ratchet ignores generated build products**: `tidy-ratchet.py`
  now skips every translation unit, diagnostic and header under `--build-dir`, so a
  build directory inside the repository measures the same checked-in sources as one
  outside it. The nightly `Full clang-tidy scan` builds in `build/` and had been failing
  on the 18 `xxd`-generated model embeds (`build/src/*.json.c`,
  `build/src/brisque_live.model.c`, two `misc-use-internal-linkage` warnings each)
  that the cpu baseline no longer lists. `make tidy-ratchet` / `tidy-ratchet-write` with
  the default in-tree `core/build` no longer measure or record them either. The arm64
  baseline, recorded from an in-tree `build-arm64`, was re-measured on its own toolchain:
  764 to 615 warnings (36 generated-file warnings, 25 already-ignored Pelorus-mirror
  entries and 88 warnings cleaned since 2026-09-23; no count rose) (ADR-1142).

## [1.0.0-rc.1] - 2026-09-27

This release collects 2790 changelog entries.
They are recorded in full, unedited, in
[`docs/changelog-archive/1.0.0-rc.1.md`](docs/changelog-archive/1.0.0-rc.1.md) — too long to read inline here.

| Section | Entries |
| --- | --- |
| Changed | 625 |
| Added | 510 |
| Removed | 14 |
| Fixed | 1585 |
| Security | 56 |
