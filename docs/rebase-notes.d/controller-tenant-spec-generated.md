## Controller tenant spec is the generated type (2026-10-08)

`refactor/controller-tenant-spec-generated`,
[ADR-2350](adr/2350-cloud-native-platform.md) D13. `TenantSpec`,
`TenantOIDC`, `TenantRBAC` and `TenantScoring` in
`cmd/vmafx-controller/auth/tenants.go` are aliases of the `VmafxTenant` types
generated into `api/vmafx/v1` from `api/vmafx-platform.toml`; `rbac` and
`scoring` are pointers there (`pointer = true`), as the controller's structs
were. A rebase that brings back a struct declaration in `tenants.go`, or a new
tenant field outside the definition, fails
`auth/tenants_generated_type_test.go`. no upstream file.
