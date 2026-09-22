# AGENTS.md — api/vmafx/v1

Stage-1 Kubernetes API types and hand-maintained deep-copy implementations.

## Rebase-sensitive invariants

1. `zz_generated_deepcopy.go` is currently hand-maintained despite its
   generated-style name. Root resource copies share `deepCopyResource` so
   metadata maps/slices, specs, and status fields cannot drift between kinds.
2. A copied resource must never alias the original `ObjectMeta` labels or
   finalizers. Keep `zz_generated_deepcopy_test.go` green when adding a kind.
3. When controller-gen becomes authoritative in Stage 2, replace this entire
   hand-maintained file and its helper together; do not mix generated and
   hand-edited methods.

## Test requirements

```bash
go test ./api/vmafx/v1/
```
