## Protobuf files generated from the platform definition (2026-10-08)

`rc4/api-wp17-proto`, [ADR-2350](adr/2350-cloud-native-platform.md) D13. Every
file under `proto/` is generated: `proto/vmafx/v1/vmafx_api.proto` from
`core/api/vmafx.toml`, `proto/vmafx/v1/vmafx.proto` and
`proto/vmafx/controller/v1/controller.proto` from `api/vmafx-platform.toml`
(`scripts/codegen/vmafx-api.py --write`), and every `*.pb.go` under `gen/go` by
`scripts/codegen/proto_generate.py --write` (buf `BUF_VERSION` of
`build-config.env`, plugins pinned as `go.mod` tools). A change that edits a
proto or a binding by hand moves into the definition instead; on a conflict in
a generated file take either side and run both generators.
`test_vmafx_api_generated_current` and `test_proto_generated_current` guard
it, `proto_generate.py --breaking-against` the wire. no upstream file.
