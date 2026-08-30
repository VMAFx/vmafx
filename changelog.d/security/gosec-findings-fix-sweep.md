- **gosec sweep across the Go surface — fix all findings + add CI gate
  (ADR-0983).** Ran `gosec ./...` against `cmd/vmafx-{controller,node,
  server,operator,mcp,tune}/...`, `pkg/...`, and `gen/go/...`. Of 38
  raw findings, six G115 (cgo int→uint32 casts) and four G103
  (`unsafe.Slice` in protoc-generated `pb.go`) live in code we do not
  hand-author and are gated via `-exclude-generated`. The remaining
  26 fork-original findings are now resolved:

  - **G304 (CWE-22): `cmd/vmafx-mcp/impl.go::describeModel` accepted
    a caller-supplied `name` and joined it onto the repo root before
    `os.Stat`-ing the result, so `{"name": "../../../etc/passwd"}`
    would have reached `/etc/passwd`.** Routed the candidate through
    `libvmaf.ValidatePath` so the lookup is bounded to
    `libvmaf.AllowedRoots()`. New regression test
    `cmd/vmafx-mcp/impl_gosec_test.go::TestDescribeModelRejectsTraversal`.
  - **G306 / G301: `cmd/vmafx-tune/cmd/compare.go::writeOutput`
    tightened from 0o644 / 0o755 to 0o600 / 0o750.** Comparison
    reports include dataset path strings; restricting to the owner
    matches the broader fork-write policy.
  - **G104: `runVmafScore`'s `outFile.Close()` and `os.Remove(outPath)`
    return values are now checked.** Close failure short-circuits the
    request with an error; remove failure logs to stderr (best-effort
    cleanup).
  - **G204 / G304: 22 false-positive suppressions previously written
    as `//nolint:gosec` were rewritten as `// #nosec G<rule> -- ...`
    with citations.** gosec does not parse golangci-lint nolint
    directives, so the old comments were dead text. Each new
    suppression names the rule and the validating helper (constant
    binary name, `libvmaf.ValidatePath`-filtered path, `os.CreateTemp`
    output).
  - **G202: `cmd/vmafx-controller/queue/queue.go::ListAll` SQL
    concatenation flagged but verified safe** — only `repeatCommaQ`
    output (pure `,?,?,...` placeholders) is concatenated; status
    values bind through `placeholders...`. Suppressed with a citation.

  CI gate added in `.github/workflows/go-ci.yml` (gosec step after
  `go vet`, before `go test`) plus a `make lint-go` Makefile target.
  `gosec -exclude-generated ./...` is the new zero-finding contract.
