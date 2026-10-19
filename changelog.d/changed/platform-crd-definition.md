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
