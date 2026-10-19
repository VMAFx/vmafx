<!-- markdownlint-disable MD013 -->
# AGENTS.md — api/vmafx/v1

Kubernetes API types of `vmafx.dev/v1`. Every file generated (ADR-2350 D13) except `deepcopy_test.go`.

## Rebase-sensitive invariants

1. Source = `api/vmafx-platform.toml` (`[[groups]]`, `[[resources]]`, `[[messages]]` / `[[enums]]` with `group`). `groupversion_info.go`, `<kind>_types.go` <- `scripts/codegen/vmafx-api.py --write`; `zz_generated.deepcopy.go` <- controller-gen via `scripts/codegen/crd_generate.py --write`. Never hand-edit. Generated-file conflict -> take one side, rerun both generators at tip.
2. Within v1 resource only grows: no removed field, no new required field, no narrower enum / bound / type / format, no changed pattern or default. `test_crd_compat` (generated CRDs vs merge base with `origin/master`) refuses; break -> new API version.
3. Deep copy never aliases `ObjectMeta` labels or finalizers: `deepcopy_test.go` checks generated code for every kind.

## Test requirements

```bash
go test ./api/vmafx/v1/
python3 scripts/codegen/crd_generate.py --check --compat-against origin/master
```
