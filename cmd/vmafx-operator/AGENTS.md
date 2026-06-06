# AGENTS.md — cmd/vmafx-operator

## Package role

vmafx-operator is a Kubernetes Operator built with kubebuilder v4 /
controller-runtime v0.19+.  It watches `VmafxJob`, `VmafxNode`, and
`VmafxModelTraining` CRDs in API group `vmafx.dev/v1` and reconciles
status subresources.  Stage 2 adds gRPC poll, stale-heartbeat detection,
checkpoint event emission, webhook validation, and per-controller RBAC.

See [ADR-0714](../../docs/adr/0714-vmafx-operator-skeleton.md),
[ADR-0786](../../docs/adr/0786-vmafx-operator-stage2-reconcilers.md), and
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
   guard.  Regression tests: `TestLastHeartbeat*` in
   `cmd/vmafx-operator/internal/controller/vmafxnode_lastheartbeat_test.go`.

5. **`gen/go/controller/controller.pb.go` has a hand-added `FinalScore` field.**
   It was added in Stage 2 to propagate the VMAF score from COMPLETED jobs.
   When `buf generate` is next run to regenerate from proto, ensure this field
   is also present in the proto source (`cmd/vmafx-controller/proto/controller.proto`)
   before regenerating, otherwise it will be silently dropped.

6. **Helm `operator.enabled` defaults to false.**  The operator Deployment
   and RBAC are gated by `operator.enabled`.  Changing the default to `true`
   would affect all existing `helm upgrade` runs.

7. **Webhooks are opt-in.**  `--webhooks-enabled=false` by default.  Enabling
   requires a valid TLS certificate.  Do not change the default to `true`
   without documenting the cert-manager dependency.

8. **No shared state between reconcilers.**  Each reconciler has its own
   `client.Client` and `Scheme`.  Do not add package-level variables.

9. **Per-controller RBAC.** `config/rbac/role_vmafxjob.yaml`,
   `config/rbac/role_vmafxnode.yaml`, and `config/rbac/role_vmafxmodeltraining.yaml`
   are the minimum-permission roles.  The combined `config/rbac/role.yaml` is
   a convenience aggregate.  When adding verbs to a reconciler, update the
   corresponding per-controller role, not just the aggregate.

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

See [docs/development/operator.md#running-tests](../../docs/development/operator.md#running-tests)
for full instructions including CI environment setup.
