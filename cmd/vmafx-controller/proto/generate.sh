#!/usr/bin/env bash
# Regenerate the controllerv1 Go bindings from controller.proto (ADR-1119).
#
# These bindings live at gen/go/controller/ and MUST be generated, never
# hand-written: hand-written stubs once shipped that did not implement
# proto.Message, so every VmafxController RPC failed to marshal at runtime
# (caught now by cmd/vmafx-controller/wire_test.go).
#
# Requires: protoc, protoc-gen-go, protoc-gen-go-grpc on PATH.
#   go install google.golang.org/protobuf/cmd/protoc-gen-go@latest
#   go install google.golang.org/grpc/cmd/protoc-gen-go-grpc@latest
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../../.." && pwd)"
protoc \
  --proto_path="$here" \
  --go_out="$repo/gen/go" \
  --go_opt=paths=source_relative,Mcontroller.proto=github.com/VMAFx/vmafx/gen/go/controller \
  --go-grpc_out="$repo/gen/go" \
  --go-grpc_opt=paths=source_relative,Mcontroller.proto=github.com/VMAFx/vmafx/gen/go/controller \
  controller.proto
echo "regenerated gen/go/controller/{controller,controller_grpc}.pb.go"
