# AGENTS.md — cmd/vmafx-operator

## Package role

- Role: Kubernetes Operator built with kubebuilder v4 / controller-runtime
  v0.24+.
- Watches `VmafxJob`, `VmafxNode`, `VmafxModelTraining` CRDs in API group
  `vmafx.dev/v1`.
- Reconciles status subresources.
- Stage 2 adds: gRPC poll, stale-heartbeat detection, checkpoint event emission,
  webhook validation, per-controller RBAC.
- ADR-1119 Phase 1: binary composed with **golusoris fx framework**.
- `main.go` = `fx.New(...).Run()` over golusoris `k8s/operator` module, not
  hand-rolled `ctrl.NewManager` + `mgr.Start`.
- Only non-cgo vmafx binary; cleanest golusoris/operator fit.
- Refs: [ADR-0714](../../docs/adr/0714-vmafx-operator-skeleton.md),
  [ADR-0786](../../docs/adr/0786-vmafx-operator-stage2-reconcilers.md),
  [ADR-1119](../../docs/adr/1119-golusoris-go-framework-adoption.md),
  [docs/development/operator.md](../../docs/development/operator.md).

## Rebase-sensitive invariants

1. **DeepCopyObject is hand-written.** `api/vmafx/v1/zz_generated_deepcopy.go`
   hand-written (controller-gen codegen = Stage 3 CI job). Do not delete or
   overwrite without running `controller-gen object:headerFile=...` to
   regenerate.

2. **CRD YAMLs live in two places.** Canonical source: `config/crd/bases/`. Helm
   ships copies in `deploy/helm/vmafx/crds/`. Keep both in sync whenever CRD
   schema changes.

3. **`api/vmafx/v1/vmafxjob_types.go` has a `ControllerJobID` field.** Bridge
   between external scheduler (vmafx-controller) and operator. Field set by
   scheduler, read by reconciler. Do not rename without updating CRD YAML and
   Helm CRD copies. Every new status field added to Go types file must also be
   added to both CRD YAML files (`config/crd/bases/` and
   `deploy/helm/vmafx/crds/`); Kubernetes API server structural schema pruning
   silently drops unknown fields on status writes (ADR-1069).

4. **`status.lastHeartbeat` on VmafxNode is owned by the node agent.**
   `VmafxNodeReconciler` must NOT write `status.lastHeartbeat`. Written
   exclusively by vmafx-node agent via controller Heartbeat RPC. Operator reads
   for stale-threshold detection (ADR-1069). Introducing write to field in
   reconciler defeats staleness guard.

5. **`gen/go/controller/controller.pb.go` has a hand-added `FinalScore` field.**
   Added in Stage 2 to propagate VMAF score from COMPLETED jobs. When
   `buf generate` runs to regenerate from proto, ensure field also present in
   proto source (`cmd/vmafx-controller/proto/controller.proto`) before
   regenerating; otherwise silently dropped.

6. **Helm `operator.enabled` defaults to false.** Operator Deployment and RBAC
   gated by `operator.enabled`. Changing default to `true` affects all existing
   `helm upgrade` runs.

7. **Webhooks are opt-in.** Disabled by default (`VMAFX_OPERATOR_WEBHOOK_PORT`
   unset / `0`). Enabling sets port (e.g. `9443`), requires valid TLS cert. Do
   not ship non-zero default port without documenting cert-manager dependency.
   `registerWebhooks` in `main.go` gates validators on
   `operator.Options.WebhookPort > 0`. golusoris **v0.5.0** (golusoris#227) owns
   webhook-server bind: `operator.Module` sets `manager.Options.WebhookServer`
   from `WebhookPort`/`WebhookHost`, so server listens on configured port and
   `registerWebhooks` registers per-CRD validators under same gate.

8. **No shared state between reconcilers.** Each reconciler has own
   `client.Client` and `Scheme`. Do not add package-level variables.

9. **Per-controller RBAC.** `config/rbac/role_vmafxjob.yaml`,
   `config/rbac/role_vmafxnode.yaml`, and
   `config/rbac/role_vmafxmodeltraining.yaml` = minimum-permission roles.
   Combined `config/rbac/role.yaml` = convenience aggregate. When adding verbs
   to reconciler, update corresponding per-controller role, not just
   aggregate.

10. **fx owns signals and the run loop — do NOT call
    `ctrl.SetupSignalHandler()` or `mgr.Start()` anywhere.** `main.go` is
    `fx.New(...).Run()`; golusoris `operator.Module` `runManager` invoke starts
    manager on fx Start (goroutine bounded by fx-managed context), cancels on
    fx Stop. fx.Run installs SIGINT/SIGTERM handler. Second
    `ctrl.SetupSignalHandler()` registers competing handler (panics if called
    twice) — bug, not redundancy.

11. **Do NOT call `ctrl.SetLogger` from the binary.** golusoris **v0.5.0**
    `operator.Module` calls `ctrl.SetLogger` itself (golusoris#227), routing
    controller-runtime logs onto injected `*slog.Logger` + OTel correlation.
    Second binary-side call = redundant override. (`setupCtrlLogger` v0.4.0
    shim removed when pin moved to v0.5.0.)

12. **golusoris pin floor is v0.5.0.** `k8s/operator` module landed in
    golusoris PR #224 (first tagged v0.4.0); `Options.WebhookPort`/
    `WebhookHost` fields and auto `ctrl.SetLogger` call (golusoris#227) landed
    in v0.5.0, which `main.go` depends on. `main.go` will not compile against
    golusoris below v0.4.0; loses webhook/logger wiring below v0.5.0.

13. **VMAFX_ env contract uses CompoundKeys.** golusoris env transform splits
    EVERY underscore on delimiter. Without `config.Options.CompoundKeys`,
    operator leaf keys (`metrics_addr`, `health_probe_addr`, `leader_election`,
    `leader_election_id`, `graceful_shutdown`, `webhook_port`, `webhook_host`)
    mis-map (`VMAFX_OPERATOR_METRICS_ADDR` -> `operator.metrics.addr`, not
    `operator.metrics_addr`). `operatorEnvOptions()` declares each leaf as
    CompoundKey; `TestEnvOptionsContract` fails if upstream adds new operator
    option without registering here.

14. **`--version` exits before fx startup** (`main.go`, ADR-1129): release
    images inject `pkg/version.version` via Go ldflags; container smoke runs
    `vmafx-operator --version`. Keep exact early exit ahead of `app().Run()` so
    version verification needs no Kubernetes credentials, does not start
    long-running manager.

15. **The controller dial goes through golusoris's `ConnFactory`**
    (`internal/controller/vmafxjob_controller.go::getRemoteJob`, ADR-0782 /
    ADR-1095 / ADR-1119): `grpcmod.NewConnFactory().Dial` is `grpc.NewClient`
    plus `otelgrpc` client handler and insecure credentials. Every `GetJob`
    poll carries `traceparent`, joins controller server span. Do not
    reintroduce bare `grpc.DialContext` / `grpc.NewClient`. OTel init is
    `bootstrap.Base` (`main_test.go::TestOTelWiredThroughBootstrap` locks no-op
    default and `vmafx-operator` / `pkg/version` identity); controller-runtime
    reconcile loops carry no span of their own.

## Test requirements

### Controller envtest (requires kubebuilder-envtest binaries)

```bash
export KUBEBUILDER_ASSETS=$(setup-envtest use 1.31 -p path)
go test ./cmd/vmafx-operator/internal/controller/... -v
```

### Webhook unit tests (no envtest needed)

```bash
go test ./cmd/vmafx-operator/internal/webhook/... -v
```

### fx graph + env-contract tests (no envtest needed)

- `cmd/vmafx-operator/main_test.go` validates fx dependency graph
  (`fx.ValidateApp` over production option list; resolves graph without
  starting manager).
- Pins `VMAFX_` env contract (compound-key binding + app-level defaults).

```bash
go test ./cmd/vmafx-operator/ -run 'TestOptions|TestEnv|TestWith' -v
```

- Full instructions + CI setup:
  [docs/development/operator.md#running-tests](../../docs/development/operator.md#running-tests).

## Canonical envtest setup

- Controller suite setup/skip guidance MUST use `make setup-envtest` and
  `eval "$(make -s setup-envtest-env)"`.
- Shared helper verifies tool release from `build-config.env`.
- Do not reintroduce independent `@latest` installer in comments, messages, CI.
- Kubernetes 1.31 = default fixture generation.
- Ref: [Research-2058](../../docs/research/2058-envtest-version-owner.md).
