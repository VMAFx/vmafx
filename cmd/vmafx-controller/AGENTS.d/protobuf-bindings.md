---
paths:
  - gen/go/controller/*.go
  - proto/vmafx/controller/v1/controller.proto
  - api/vmafx-platform.toml
  - cmd/vmafx-controller/wire_test.go
invariant: Never hand-edit .pb.go or proto; regenerate from api/vmafx-platform.toml; top-level JobStatus.
---
<!-- markdownlint-disable MD013 MD060 -->
# Generated protobuf bindings (ADR-1119, ADR-2350)

## Protobuf bindings (ADR-1119) — GENERATED, never hand-written

`controllerv1` Go bindings at `gen/go/controller/{controller,controller_grpc}.pb.go`
generated from `proto/vmafx/controller/v1/controller.proto`, itself generated
from `api/vmafx-platform.toml` (ADR-2350 D13): `scripts/codegen/vmafx-api.py
--write`, then `scripts/codegen/proto_generate.py --write` (one `buf generate`).

- **NEVER hand-edit `.pb.go` files.** Hand-written stubs lacked `proto.Message`
  implementation (no `protoimpl`/`ProtoReflect`); `VmafxController` RPCs
  failed marshaling while unit tests passed. Guard:
  `cmd/vmafx-controller/wire_test.go` round-trips `SubmitJob`/`GetJob`/`StreamJobs`
  over `bufconn` `grpc.Server`. Edit `api/vmafx-platform.toml`, run
  `scripts/codegen/vmafx-api.py --write` then
  `scripts/codegen/proto_generate.py --write`, keep test green.
- Job-status enum: **top-level `JobStatus`** (not nested in `Job`); Go type:
  `controllerv1.JobStatus`, matching grpc_server/queue/scheduler. Nesting in
  `Job` generates `Job_Status`, breaking call sites.
