# Research-1338: Go fix clean-tree gate

## Tool contract

The repository currently declares Go 1.27.1 in `go.mod`, and the local
toolchain reports `go version go1.27.1-X:nodwarf5 linux/amd64`. Its installed
`go help fix` documents `-diff` as a non-mutating mode that prints a unified
diff and exits nonzero when the diff is not empty. `go tool fix help` lists the
modernization analyzers used by the default command.

Before this change, neither `.github/workflows/go-ci.yml` nor the Makefile ran
`go fix`. The first low-priority probe was therefore red and proposed current
standard-library and language rewrites, including `maps.Copy`,
`slices.Contains`, `errors.AsType`, integer ranges, and sequence iterators.

## Applying the sweep

`go fix ./...` was run repeatedly as directed by the tool. Two details needed
review rather than blind acceptance:

1. In `cmd/vmafx-mcp`, `stringsseq` and `slicescontains` proposed overlapping
   rewrites for the same path-component loop. Applying `go fix -stringsseq
   ./cmd/vmafx-mcp` first selected the allocation-free iterator form; the full
   fixer then completed without an exclusion.
2. `errorsastype` modernized a test that matches the `exitCoder` interface.
   `errors.AsType` constrains its type parameter to `error`, while the old
   interface declared only `ExitCode()`. Embedding `error` documents the actual
   contract and preserves the test's important negative control:
   `*exec.ExitError` still satisfies the interface, while the CLI accepts only
   its own `exitCodeError` type.

The hand-maintained `api/vmafx/v1/zz_generated_deepcopy.go` was included. Its
local `AGENTS.md` explicitly says the generated-style file is hand-maintained
until controller-gen becomes authoritative, so applying `maps.Copy` does not
violate a generated-source owner.

## Gate placement and validation

The check does not need libvmaf to be linked, so it belongs immediately after
`actions/setup-go` and before package installation, ONNX Runtime setup, and the
native build. This makes a source-only failure cheap while retaining ADR-1238's
existing impact routing and required check name.

Focused validation:

```bash
go fix -diff ./...
python3 scripts/ci/test_go_workflow_contract.py
actionlint .github/workflows/go-ci.yml
go test ./...
go vet ./...
```

The first command must produce no output and exit zero. The contract test also
proves the workflow step precedes native setup and that the local write/check
targets cannot silently drift from CI.
