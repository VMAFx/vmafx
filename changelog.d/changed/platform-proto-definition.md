- **The gRPC services are generated from one platform definition
  ([ADR-2350](docs/adr/2350-cloud-native-platform.md)).** The scoring service
  and the controller are described in `api/vmafx-platform.toml`, from which
  `scripts/codegen/vmafx-api.py` writes `proto/vmafx/v1/vmafx.proto` and
  `proto/vmafx/controller/v1/controller.proto`; `vmafx_api.proto` moves to
  `proto/vmafx/v1/`. One buf configuration (`buf.yaml`, `buf.gen.yaml`, run by
  `scripts/codegen/proto_generate.py`) generates the Go bindings at their
  existing import paths, and `cmd/vmafx-controller/proto/` with its protoc
  script is gone. The wire format is unchanged: every message, field number,
  enum value and RPC is the same, and `buf breaking` with the wire and JSON
  rules guards it from now on. gRPC reflection reports the new file names
  (`vmafx/v1/vmafx.proto`, `vmafx/controller/v1/controller.proto`).
