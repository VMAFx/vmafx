- **The Go toolchain moves from 1.27.2 to 1.27.3** for GO-2026-6809 (`crypto/x509`). `go.mod`
  declares `go 1.27.3`, and the release, development and observability images build on the
  `golang:1.27-trixie` digest that carries Go 1.27.3. `govulncheck ./...` reached
  `crypto/x509.Verify` under Go 1.27.2; it now reports no reachable vulnerable symbol.
