---
name: add-k8s-resource
description: Scaffold a new Kubernetes CRD + kubebuilder controller + RBAC + helm chart entry for the vmafx-operator. Follows the VmafxJob / VmafxNode / VmafxModelTraining precedent in cmd/vmafx-operator/ (ADR-0714, ADR-0709 parent).
---
<!-- markdownlint-disable MD013 -->

# /add-k8s-resource

Adds new CRD under `vmafx.dev` API group.
Generates kubebuilder-style controller stub.
Wires stub into vmafx-operator manager.
Ships matching CRD YAML in helm chart `crds/` directory.
Adds RBAC rules.
Exposes values.yaml toggle.
Follows conventions from `VmafxJob`, `VmafxNode`, `VmafxModelTraining` in
`cmd/vmafx-operator/internal/controller/` (ADR-0714).

## When to use

- Add new CRD reconciled by vmafx-operator (e.g. `VmafxBenchmarkRun`,
  `VmafxCorpusSync`, `VmafxModelDeployment`).
- NOT for adding new sub-resource or field to existing CRD -> schema migration;
  use `controller-gen` directly, follow CRD-versioning ADRs.
- NOT for adding non-CRD Kubernetes resource (Deployment, Service, etc.) -> goes
  directly into `deploy/helm/vmafx/templates/`.

## Invocation

```text
/add-k8s-resource <KindName>
```

`<KindName>` = `PascalCase`, no `Vmafx` prefix (scaffold adds it). Examples:
`/add-k8s-resource BenchmarkRun` -> CRD `VmafxBenchmarkRun`, plural
`vmafxbenchmarkruns`, short name `vmbench`.

## Files created

| Path                                                                                  | Purpose                                            |
|---------------------------------------------------------------------------------------|----------------------------------------------------|
| `api/vmafx/v1/vmafx<kind>_types.go`                                                   | Go types (Spec, Status, list type)                 |
| `cmd/vmafx-operator/internal/controller/vmafx<kind>_controller.go`                    | Controller reconciler stub                         |
| `cmd/vmafx-operator/internal/controller/vmafx<kind>_controller_test.go`               | envtest-style controller smoke test                |
| `deploy/helm/vmafx/crds/vmafx.dev_vmafx<plural>.yaml`                                 | CRD manifest (controller-gen output)               |
| `deploy/helm/vmafx/templates/operator-rbac-<kind>.yaml`                               | Per-kind ClusterRole rule additions                |
| `docs/k8s/crds/vmafx<kind>.md`                                                        | Human-readable CRD reference                       |
| `changelog.d/added/k8s-crd-vmafx<kind>.md`                                            | Changelog fragment                                 |

## Files patched

- `cmd/vmafx-operator/main.go`: add `SetupWithManager` call for new controller,
  append to `--enable-controllers` flag whitelist.
- `deploy/helm/vmafx/values.yaml`: add `operator.controllers.<kind>` section
  with `enabled: false` (opt-in by default -> see ADR-0714 staging).
- `docs/development/operator.md`: append row to controller table.
- `cmd/vmafx-operator/AGENTS.md`: note new CRD in "controllers shipped"
  invariant table.

## Workflow

1. Validate `<KindName>` matches `^[A-Z][A-Za-z0-9]+$`, not already present
   (grep `api/vmafx/v1/`, `deploy/helm/vmafx/crds/`).
2. Compute derived names:
   - `kind` = `Vmafx<KindName>` (Go type, CRD kind).
   - `kind_lower` = lowercased (file paths).
   - `plural` = naive pluralization (`<kind_lower>s`); override allowed via env
     `K8S_PLURAL_OVERRIDE`.
   - `short` = `vm<first-4-chars-of-kind>` (e.g. `vmbench`).
3. Copy templates with placeholder substitution (`@KIND@`, `@KIND_LOWER@`,
   `@PLURAL@`, `@SHORT@`, `@COPYRIGHT@`).
4. Apply patches to `main.go`, `values.yaml`, `operator.md`, `AGENTS.md`.
5. Regenerate CRD bundle: `make manifests` (delegates to `controller-gen`) ->
   keeps helm `crds/` YAML byte-identical to kubebuilder output.
6. Run `go build ./cmd/vmafx-operator/...` -> confirm manager compiles.
7. Run `go test ./cmd/vmafx-operator/...` -> confirm new controller test passes
   (stub reconcile only -> returns success without side effects).
8. Open PR checklist comment with:
   - Reconciliation logic TODO list (Spec field handling, Status conditions,
     finalizer, owner references).
   - RBAC review: ClusterRole verbs MUST be tight
     (`get,list,watch,update,patch` by default; no `delete` without
     justification).
   - Helm chart smoke (`helm template deploy/helm/vmafx | yq` -> verify new CRD
     lands).

## Guardrails

- **Never** activate controller by default. `values.yaml` ships `enabled: false`
  (users opt in per cluster -> matches Stage 1 posture in ADR-0714).
- **Never** add CRD without helm `crds/` YAML -> operators installing via helm
  rely on chart `crds/` directory pre-creation.
- **Never** add `delete` or `*` verbs to RBAC without ADR justifying it. CRDs
  operator owns default to `get,list,watch,update,patch` plus `create` only when
  controller materialises sub-resources.
- **Never** overwrite existing files. Scaffold refuses if target path exists.
- **Never** skip docs page: per-surface doc bar in
  [ADR-0100](../../../docs/adr/0100-project-wide-doc-substance-rule.md)
  mandatory.

## References

- [ADR-0714](../../../docs/adr/0714-vmafx-operator-kubebuilder-skeleton.md) —
  operator skeleton + CRD conventions
- [ADR-0709](../../../docs/adr/0709-vmafx-phase-4b-distributed-platform.md) —
  parent distributed-platform plan
- [`cmd/vmafx-operator/internal/controller/`](../../../cmd/vmafx-operator/internal/controller/)
  —
  reference controllers (Node, Job, ModelTraining)
- [`deploy/helm/vmafx/crds/`](../../../deploy/helm/vmafx/crds/) — shipped CRD
  manifests
