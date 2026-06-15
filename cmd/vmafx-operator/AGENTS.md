# AGENTS.md — cmd/vmafx-operator

## Package role

vmafx-operator is a Kubernetes Operator built with kubebuilder v4 /
controller-runtime v0.24+.  It watches `VmafxJob`, `VmafxNode`, and
`VmafxModelTraining` CRDs in API group `vmafx.dev/v1` and reconciles
status subresources.  Stage 2 adds gRPC poll, stale-heartbeat detection,
checkpoint event emission, webhook validation, and per-controller RBAC.

As of ADR-1119 Phase 1 the binary is composed with the **golusoris fx
framework**: `main.go` is `fx.New(...).Run()` over golusoris's
`k8s/operator` module, not a hand-rolled `ctrl.NewManager` +
`mgr.Start`.  This is the only non-cgo vmafx binary and the cleanest
golusoris/operator fit.

See [ADR-0714](../../docs/adr/0714-vmafx-operator-skeleton.md),
[ADR-0786](../../docs/adr/0786-vmafx-operator-stage2-reconcilers.md),
[ADR-1119](../../docs/adr/1119-golusoris-go-framework-adoption.md), and
[docs/development/operator.md](../../docs/development/operator.md).

## Rebase-sensitive invariants

1. **DeepCopyObject is hand-written.**  `api/vmafx/v1/zz_generated_deepcopy.go`
   is written by hand (controller-gen codegen is a Stage 3 CI job).
   Do not delete or overwrite it without running `controller-gen object:headerFile=...`
   to regenerate it.

2. **CRD YAMLs live in two places.**  The canonical source is
   `config/crd/bases/`.  Helm ships copies in `deploy/helm/vmafx/crds/`.
   Keep both in sync whenever a CRD schema changes.

3. **`api/vmafx/v1/vmafxjob_types.go` has a `ControllerJobID` field.**
   This is the bridge between the external scheduler (vmafx-controller) and
   the operator.  The field is set by the scheduler, read by the reconciler.
   Do not rename it without updating the CRD YAML and the Helm CRD copies.
   Every new status field added to a Go types file must also be added to both
   CRD YAML files (`config/crd/bases/` and `deploy/helm/vmafx/crds/`); the
   Kubernetes API server's structural schema pruning silently drops unknown
   fields on status writes (ADR-1069).

4. **`status.lastHeartbeat` on VmafxNode is owned by the node agent.**
   The `VmafxNodeReconciler` must NOT write `status.lastHeartbeat`.  It is
   written exclusively by the vmafx-node agent via the controller's Heartbeat
   RPC.  The operator reads it for stale-threshold detection (ADR-1069).
   Introducing any write to that field in the reconciler defeats the staleness
   guard.

5. **`gen/go/controller/controller.pb.go` has a hand-added `FinalScore` field.**
   It was added in Stage 2 to propagate the VMAF score from COMPLETED jobs.
   When `buf generate` is next run to regenerate from proto, ensure this field
   is also present in the proto source (`cmd/vmafx-controller/proto/controller.proto`)
   before regenerating, otherwise it will be silently dropped.

6. **Helm `operator.enabled` defaults to false.**  The operator Deployment
   and RBAC are gated by `operator.enabled`.  Changing the default to `true`
   would affect all existing `helm upgrade` runs.

7. **Webhooks are opt-in.**  Disabled by default
   (`VMAFX_OPERATOR_WEBHOOK_PORT` unset / `0`).  Enabling sets the port (e.g.
   `9443`) and requires a valid TLS certificate.  Do not ship a non-zero
   default port without documenting the cert-manager dependency.
   `registerWebhooks` in `main.go` gates the validators on
   `operator.Options.WebhookPort > 0`.  golusoris **v0.5.0** (golusoris#227)
   owns the webhook-server bind: `operator.Module` sets
   `manager.Options.WebhookServer` from `WebhookPort`/`WebhookHost`, so the
   server listens on the configured port and `registerWebhooks` registers the
   per-CRD validators under the same gate.

8. **No shared state between reconcilers.**  Each reconciler has its own
   `client.Client` and `Scheme`.  Do not add package-level variables.

9. **Per-controller RBAC.** `config/rbac/role_vmafxjob.yaml`,
   `config/rbac/role_vmafxnode.yaml`, and `config/rbac/role_vmafxmodeltraining.yaml`
   are the minimum-permission roles.  The combined `config/rbac/role.yaml` is
   a convenience aggregate.  When adding verbs to a reconciler, update the
   corresponding per-controller role, not just the aggregate.

10. **fx owns signals and the run loop — do NOT call
    `ctrl.SetupSignalHandler()` or `mgr.Start()` anywhere.**  `main.go` is
    `fx.New(...).Run()`; golusoris `operator.Module`'s `runManager` invoke
    starts the manager on fx Start (in a goroutine bounded by an fx-managed
    context) and cancels it on fx Stop.  fx.Run installs the SIGINT/SIGTERM
    handler.  A second `ctrl.SetupSignalHandler()` would register a competing
    handler (it also panics if called twice) — that is a bug, not a
    redundancy.

11. **Do NOT call `ctrl.SetLogger` from the binary.**  golusoris **v0.5.0**'s
    `operator.Module` calls `ctrl.SetLogger` itself (golusoris#227), routing
    controller-runtime's own logs onto the injected `*slog.Logger` and its OTel
    correlation.  A second binary-side call would be a redundant override.  (The
    earlier `setupCtrlLogger` v0.4.0 shim was removed when the pin moved to
    v0.5.0.)

12. **golusoris pin floor is v0.5.0.**  The `k8s/operator` module landed in
    golusoris PR #224 (first tagged v0.4.0); its `Options.WebhookPort`/
    `WebhookHost` fields and the auto `ctrl.SetLogger` call (golusoris#227)
    landed in v0.5.0, which `main.go` now depends on.  `main.go` will not
    compile against a golusoris pin below v0.4.0 and loses webhook/logger wiring
    below v0.5.0.

13. **VMAFX_ env contract uses CompoundKeys.**  golusoris's env transform
    splits EVERY underscore on the delimiter, so without
    `config.Options.CompoundKeys` the operator's leaf keys (`metrics_addr`,
    `health_probe_addr`, `leader_election`, `leader_election_id`,
    `graceful_shutdown`, `webhook_port`, `webhook_host`) would mis-map
    (`VMAFX_OPERATOR_METRICS_ADDR` → `operator.metrics.addr` instead of
    `operator.metrics_addr`).  `operatorEnvOptions()` declares each leaf as a
    CompoundKey; `TestEnvOptionsContract` fails if a new operator option is
    added upstream without registering it here.

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

`cmd/vmafx-operator/main_test.go` validates the fx dependency graph
(`fx.ValidateApp` over the production option list — it resolves the graph
without starting a manager) and pins the `VMAFX_` env contract (compound-key
binding + app-level defaults).

```bash
go test ./cmd/vmafx-operator/ -run 'TestOptions|TestEnv|TestWith' -v
```

See [docs/development/operator.md#running-tests](../../docs/development/operator.md#running-tests)
for full instructions including CI environment setup.
