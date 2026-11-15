<!-- markdownlint-disable MD013 -->
# AGENTS.md — api

Declarative API definitions and OpenAPI contracts. Parent: [../AGENTS.md](../AGENTS.md).

## Rebase-sensitive invariants

1. `api/vmafx-platform.toml` = single source (ADR-2350 D13) for `proto/vmafx/v1/vmafx.proto`, `proto/vmafx/controller/v1/controller.proto`, Go types in `api/vmafx/v1/`, and spliced OpenAPI schemas in `api/openapi/vmafx-server-v1.yaml`. Never edit generated `.proto` or `.pb.go` files directly; edit `api/vmafx-platform.toml`, then run `python3 scripts/codegen/vmafx-api.py --write`, `python3 scripts/codegen/proto_generate.py --write`, and `python3 scripts/codegen/crd_generate.py --write`.
2. For `VmafxController` RPC bindings (`gen/go/controller/*.go`, `proto/vmafx/controller/v1/controller.proto`, `wire_test.go` marshaling guard, top-level `JobStatus` enum): read [protobuf-bindings](../cmd/vmafx-controller/AGENTS.d/protobuf-bindings.md) and [gen/go/AGENTS.md](../gen/go/AGENTS.md).
3. For Kubernetes `vmafx.dev/v1` resource compatibility and deepcopy rules: read [vmafx/v1/AGENTS.md](vmafx/v1/AGENTS.md).
