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
# `module=` mode (not paths=source_relative): protoc-gen-go strips the module
# prefix from the proto's go_package (github.com/VMAFx/vmafx/gen/go/controller)
# and writes to gen/go/controller/ — the path the operator + controller import.
# paths=source_relative would instead write gen/go/controller.pb.go (wrong dir),
# silently leaving the compiled bindings stale.
protoc \
  --proto_path="$here" \
  --go_out="$repo" \
  --go_opt=module=github.com/VMAFx/vmafx \
  --go-grpc_out="$repo" \
  --go-grpc_opt=module=github.com/VMAFx/vmafx \
  controller.proto
echo "regenerated gen/go/controller/{controller,controller_grpc}.pb.go"
