<!-- markdownlint-disable MD013 MD060 -->

# ADR-3558: Opt-in node sidecar feedback client, HTTP /readyz readiness probe, and soft per-node affinity

- **Status**: Accepted
- **Date**: 2026-11-17
- **Deciders**: lusoris
- **Tags**: `helm`, `kubernetes`, `deploy`, `ai`, `observability`, `sycl`

## Context

Two operational gaps surfaced on live cluster deployments of `vmafx-node`:

1. **Unconditional sidecar dialing with blind TCP readiness.** `cmd/vmafx-node`
   always started `FeedbackClient.Start()` (`providers.go`), dialing
   `/tmp/vmafx-sidecar.sock` (`VMAFX_SIDECAR_SOCKET`) in the background with
   exponential backoff up to two minutes ([ADR-1049](1049-grpc-feedback-backoff.md)).
   Because [ADR-0781](0781-sidecar-sgd-ema-online-trainer.md) remains Proposed
   and neither a sidecar image nor a chart container exists, every `vmafx-node`
   pod logged `WARN sidecar not reachable — retrying` (`connect: no such file or
   directory`) forever while reporting Ready. Conversely, an operator who does
   co-locate a sidecar had no readiness barrier: `deploy/helm/vmafx/templates/node.yaml`
   used `tcpSocket` on port `grpc` for both liveness and readiness
   ([ADR-1094](1094-helm-rolling-update-correctness.md), written before the node
   exposed an HTTP listener on `VMAFX_HTTP_ADDR`), so Kubernetes never consulted
   `/readyz`.
2. **No per-node affinity override for heterogeneous GPU pools.** In
   `templates/node.yaml`, `nodeSelector` and `tolerations` already followed the
   `node.<field>` override with fallback to global `.Values.<field>`, whereas
   `affinity` read only the global `.Values.affinity`. On Intel GPU clusters
   running Node Feature Discovery and the Intel GPU device plugin (`gpu.intel.com/xe`),
   the Intel GPU plugin labels each GPU node with `gpu.intel.com/product`
   (`Arc_Pro_B60` or `Arc_B580`); `gpu.intel.com/device-id.<id>.present`
   labels are an alternative key. Operators need a node-scoped soft preference
   (`preferredDuringSchedulingIgnoredDuringExecution`) that prefers the B60 node
   and falls back to the B580 node without pinning server or controller pods.

## Decision

1. **Make the node sidecar client opt-in via `VMAFX_SIDECAR_ENABLED` (`sidecar.enabled`, default `false`).**
   - `provideFeedbackClient` parses `sidecar.enabled` with `strconv.ParseBool`;
     an unparseable value fails node startup naming `sidecar.enabled` rather
     than silently defaulting to `false`.
   - When disabled, `provideFeedbackClient` logs `INFO sidecar disabled`, starts
     no drainer goroutine, performs zero socket dials, registers no `sidecar`
     health check, and counts any `FeedbackClient.Send` call as a local drop.
   - When enabled, the drainer runs as before, tracks connection state and the
     last dial or transport error atomically on `FeedbackClient`, monitors open
     connections for peer EOF while the send queue is idle, and caps reconnect
     backoff at 15 s (lowering [ADR-1049](1049-grpc-feedback-backoff.md)'s 2 min
     ceiling) so readiness recovers within 30 s of the socket appearing.
     `mountNodeHealth` registers a readiness-only (`health.TagReadiness`)
     statuspage check `sidecar` that reads that state without dialing from
     `/readyz`.
   - `api/vmafx-platform.toml` adds chart value `node.sidecar.enabled` (boolean,
     default `false`) and `[[chart_env]]` entry `VMAFX_SIDECAR_ENABLED: "true"`
     rendered in `vmafx.env.node` only when `node.sidecar.enabled` is `true`.
2. **Move the chart's node `readinessProbe` to `httpGet /readyz` on port `metrics` (amending [ADR-1094](1094-helm-rolling-update-correctness.md)).**
   - `templates/node.yaml` keeps `livenessProbe` as `tcpSocket` on `grpc` and
     switches `readinessProbe` (same timings) to `httpGet` path `/readyz` on
     port `metrics` (`VMAFX_HTTP_ADDR`, pinned to `node.metricsPort`).
   - Because `/readyz` replaces the Kubernetes `tcpSocket` readiness probe on
     `grpc`, `mountNodeHealth` adds a statuspage readiness check `grpc` that
     performs a 1 s bounded loopback TCP dial of the configured `grpc.listen`
     address (`golusoris/grpc` exposes `grpc.health.v1` from statuspage rather
     than registering a listener check onto statuspage).
3. **Add `node.affinity` to the Helm chart (`k8s:io.k8s.api.core.v1.Affinity`, default `{}`).**
   - `templates/node.yaml` renders `.Values.node.affinity` when non-empty and
     falls back to `.Values.affinity`, matching `node.nodeSelector` and
     `node.tolerations`.
   - `values.yaml` documents soft `preferredDuringSchedulingIgnoredDuringExecution`
     weights on `gpu.intel.com/product` (`Arc_Pro_B60` weight 100, `Arc_B580`
     weight 50) with no `requiredDuringSchedulingIgnoredDuringExecution` term for
     heterogeneous client Intel Arc pools.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep `FeedbackClient` always active and silence the `WARN` log | Zero new configuration keys | Background goroutine still wakes and dials an absent `/tmp` socket forever, and co-located sidecar deployments still lack readiness gating | Hides the symptom without fixing either defect |
| Dial the Unix socket directly inside the `/readyz` handler | Stateless check function | Opens and immediately drops Unix connections on every kubelet probe, racing with the single-client sidecar accept path and adding probe latency | Reading the drainer's own atomic connection state is non-intrusive and exact |
| Keep `readinessProbe` as `tcpSocket` on `grpc` and use gRPC health (`grpc.health.v1`) | Single probed port | Requires switching Kubernetes to a `grpc` probe and enabling `grpc.health` on the node listener while HTTP `/readyz` already serves statuspage checks on `metricsPort` | HTTP `/readyz` aligns the node with controller and server probe surfaces |
| Hard-pin Arc Pro B60 via `node.nodeSelector` or `requiredDuringSchedulingIgnoredDuringExecution` | Simpler selector | Leaves pods `Pending` whenever the B60 node is full or cordoned instead of spilling onto the B580 node | Soft `preferredDuringSchedulingIgnoredDuringExecution` preserves pool fallback |

## Consequences

- **Positive**: default `vmafx-node` pods emit one startup `INFO sidecar disabled`
  line and never dial `/tmp/vmafx-sidecar.sock`; operators who enable
  `node.sidecar.enabled` get automatic `NotReady` protection whenever the
  sidecar socket is absent or disconnects, recovering within 30 s when it
  returns; Kubernetes readiness now checks scorer, gRPC listener, and opt-in
  sidecar health through `/readyz`; heterogeneous GPU pools can express soft
  device-ID preferences for `vmafx-node` without affecting other chart workloads.
- **Negative**: standalone callers or tests that previously relied on
  `vmafx-node` dialing `VMAFX_SIDECAR_SOCKET` without setting
  `VMAFX_SIDECAR_ENABLED=true` must now opt in explicitly (`NewFeedbackClient`
  in Go unit tests remains enabled by default).
- **Neutral / follow-ups**: [ADR-1094](1094-helm-rolling-update-correctness.md)'s
  node `livenessProbe`, rolling-update strategy, `terminationGracePeriodSeconds`,
  and PDB decisions remain unchanged; only its node `readinessProbe` item is
  amended.

## References

- [ADR-0781](0781-sidecar-sgd-ema-online-trainer.md) — online-training sidecar protocol (Proposed).
- [ADR-1049](1049-grpc-feedback-backoff.md) — exponential retry backoff in `FeedbackClient`.
- [ADR-1094](1094-helm-rolling-update-correctness.md) — node rolling-update strategy and prior `tcpSocket` probe decision (readiness probe item amended here).
- [ADR-1119](1119-golusoris-go-framework-adoption.md) — golusoris fx composition and `R-node` stop order.
- [ADR-1309](1309-socket-path-ownership-and-owner-only-mode.md) — same-UID owner-only sidecar socket contract.
- [ADR-2350](2350-cloud-native-platform.md) D13 — generated chart values, Kubernetes-typed schema, and workload environment definitions (`api/vmafx-platform.toml`).
- Source: operator order (paraphrased) (vmafx-cluster-library-scoring-20261117, 2026-11-17, opt-in node sidecar with `/readyz` readiness gate and soft Intel Arc node affinity).
