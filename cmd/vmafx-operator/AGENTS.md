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

1. **Types, deepcopy, CRDs, RBAC role: generated (ADR-2350 D13).** Source =
   `api/vmafx-platform.toml`. `vmafx-api.py --write` -> `api/vmafx/v1/*_types.go`;
   `crd_generate.py --write` (controller-gen, go.mod tool pin) ->
   `zz_generated.deepcopy.go`, `deploy/helm/vmafx/crds/*.yaml`,
   `config/rbac/role.yaml`. Never hand-edit; gates `test_crd_generated_current`,
   `test_crd_compat` (v1 only grows).

2. **One CRD tree: `deploy/helm/vmafx/crds/`.** Chart installs it; envtest
   suite (`suite_test.go`) loads it. `config/crd/bases/` removed; never
   reintroduce second copy.

3. **`VmafxJobStatus.controllerJobID`: bridge scheduler -> reconciler.** Set by
   vmafx-controller, read by reconciler. Rename = v1 break (`test_crd_compat`).
   New status field -> definition first, regenerate; CRD schema follows, so
   structural pruning cannot drop it on status writes (ADR-1069).

4. **`status.lastHeartbeat` on VmafxNode is owned by the node agent.**
   `VmafxNodeReconciler` must NOT write `status.lastHeartbeat`. Written
   exclusively by vmafx-node agent via controller Heartbeat RPC. Operator reads
   for stale-threshold detection (ADR-1069). Introducing write to field in
   reconciler defeats staleness guard.

5. **`Job.final_score` = field 9 of `controller.proto`.** Carries VMAF score
   of COMPLETED jobs to reconciler. `gen/go/controller/controller.pb.go` =
   buf output from `api/vmafx-platform.toml` (ADR-2350 D13); new field ->
   definition first, regenerate (`vmafx-api.py --write`,
   `proto_generate.py --write`), never hand-add. CRD types: same definition
   (invariant 1).

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

9. **RBAC = `+kubebuilder:rbac` markers.** Reconcilers + `main.go` (leader
   election lease) carry markers; controller-gen writes
   `config/rbac/role.yaml`. New verb -> marker first, then chart rule in
   `deploy/helm/vmafx/templates/operator-rbac.yaml`;
   `scripts/ci/tests/test_helm_operator_rbac.py` fails when chart grants less
   than generated role. Per-kind role files removed.

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
    `operator.metrics_addr`). `operatorEnvOptions()` takes generated
    `compoundKeys` (`config_keys.gen.go`, `[[config]]` of
    `api/vmafx-platform.toml`, ADR-2350 D13); `TestEnvOptionsContract` fails
    if upstream adds new operator option without `[[config]]` entry.

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

16. **GetJob carries controller credentials** (`main.go`
    `provideControllerCredentials`, `VmafxJobReconciler.ControllerCredentials`,
    ADR-1569): `controllerclient.Load` reads `controller.tls/ca_file/
    server_name/token_file/token` (in generated `compoundKeys`;
    `controllerclient.CompoundKeys` deprecated, kept for API compatibility);
    bad combination = startup error.
    `DialOptions()` passed to `ConnFactory.Dial` (TLS replaces plaintext,
    bearer per RPC, file re-read every call, expired JWT never sent). One
    implementation with the node (HISS-19): never copy bearer/TLS code into
    the operator. Guards: `vmafxjob_auth_test.go`,
    `TestEnvBindsControllerCredentials`, `pkg/controllerclient` tests.

## Test requirements

### Controller envtest (requires kubebuilder-envtest binaries)

```bash
make setup-envtest
eval "$(make -s setup-envtest-env)"
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
