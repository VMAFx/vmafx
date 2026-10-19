---
name: add-k8s-resource
description: Scaffold a new Kubernetes CRD + kubebuilder controller + RBAC + helm chart entry for the vmafx-operator. The types and CRD come from api/vmafx-platform.toml (ADR-2350 D13); follows the VmafxJob / VmafxNode / VmafxModelTraining precedent in cmd/vmafx-operator/ (ADR-0714, ADR-0709 parent).
---
<!-- markdownlint-disable MD013 -->

# /add-k8s-resource

Adds new CRD under `vmafx.dev` API group.
Appends resource + spec/status stub to `api/vmafx-platform.toml`; generators
write Go types, deepcopy, CRD YAML (`deploy/helm/vmafx/crds/`, only CRD tree)
and `config/rbac/role.yaml` (ADR-2350 D13).
Generates kubebuilder-style controller stub.
Wires stub into vmafx-operator manager.
Adds RBAC rules.
Exposes values.yaml toggle.
Follows conventions from `VmafxJob`, `VmafxNode`, `VmafxModelTraining` in
`cmd/vmafx-operator/internal/controller/` (ADR-0714).

## When to use

- Add new CRD reconciled by vmafx-operator (e.g. `VmafxBenchmarkRun`,
  `VmafxCorpusSync`, `VmafxModelDeployment`).
- NOT for adding field to existing CRD -> edit its `[[messages]]` entry in
  `api/vmafx-platform.toml`, regenerate
  ([API generation](../../../docs/development/api-generation.md#change-a-custom-resource));
  within v1 resource only grows (`test_crd_compat`).
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
| `api/vmafx-platform.toml` (appended)                                                  | `[[resources]]` + spec / status `[[messages]]`     |
| `cmd/vmafx-operator/internal/controller/vmafx<kind>_controller.go`                    | Controller reconciler stub                         |
| `cmd/vmafx-operator/internal/controller/vmafx<kind>_controller_test.go`               | envtest-style controller smoke test                |
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
   (`kind = "Vmafx<KindName>"` in `api/vmafx-platform.toml`, no
   `api/vmafx/v1/` or `deploy/helm/vmafx/crds/` file).
2. Compute derived names:
   - `kind` = `Vmafx<KindName>` (Go type, CRD kind).
   - `kind_lower` = lowercased (file paths).
   - `plural` = naive pluralization (`<kind_lower>s`); override allowed via env
     `K8S_PLURAL_OVERRIDE`.
   - `short` = `vm<first-4-chars-of-kind>` (e.g. `vmbench`).
3. Copy templates with placeholder substitution (`@KIND@`, `@KIND_LOWER@`,
   `@PLURAL@`, `@SHORT@`, `@COPYRIGHT@`).
4. Fill spec / status fields of the stub in `api/vmafx-platform.toml`, then
   generate types, deepcopy, CRD, RBAC role:
   `python3 scripts/codegen/vmafx-api.py --write` then
   `python3 scripts/codegen/crd_generate.py --write`. Never hand-edit output.
5. Apply patches to `main.go`, `values.yaml`, `operator.md`, `AGENTS.md`.
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
     lands); `scripts/ci/tests/test_helm_operator_rbac.py` -> chart grants
     every rule of generated `config/rbac/role.yaml`.

## Guardrails

- **Never** activate controller by default. `values.yaml` ships `enabled: false`
  (users opt in per cluster -> matches Stage 1 posture in ADR-0714).
- **Never** hand-write types, deepcopy or CRD YAML -> generated from
  `api/vmafx-platform.toml`; `test_crd_generated_current` fails on drift.
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
  manifests (generated)
- [ADR-2350](../../../docs/adr/2350-cloud-native-platform.md) D13 — platform
  definition generates types, CRDs, RBAC role
- [API generation](../../../docs/development/api-generation.md#kubernetes-resources)
  — definition tables, regeneration, gates
