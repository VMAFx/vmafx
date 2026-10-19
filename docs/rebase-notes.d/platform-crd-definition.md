## Custom resources generated from the platform definition (2026-10-08)

`rc4/api-wp17-crd`, [ADR-2350](adr/2350-cloud-native-platform.md) D13. Every
file under `api/vmafx/v1` except `deepcopy_test.go` is generated: the types
from `api/vmafx-platform.toml` (`scripts/codegen/vmafx-api.py --write`), and
`zz_generated.deepcopy.go`, `deploy/helm/vmafx/crds/*.yaml` and
`config/rbac/role.yaml` by controller-gen (`scripts/codegen/crd_generate.py
--write`). `config/crd/bases/`, the hand-written `zz_generated_deepcopy.go` and
the per-kind `config/rbac/role_*.yaml` files are removed. A change that edits a
type, CRD or role by hand moves into the definition or an RBAC marker instead;
on a conflict in a generated file take either side and run both generators.
`test_crd_generated_current` guards drift, `test_crd_compat` narrowing within
v1, `scripts/ci/tests/test_helm_operator_rbac.py` the chart's operator rules.
no upstream file.
