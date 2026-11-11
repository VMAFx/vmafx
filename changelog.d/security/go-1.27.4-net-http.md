- **The Go toolchain moves from 1.27.3 to 1.27.4** for GO-2026-6859 (`net/http`). `go.mod`
  declares `go 1.27.4`, and the release, development and observability images build on the
  `golang:1.27-trixie` digest that carries Go 1.27.4. `govulncheck ./...` reached
  `net/http.Write` under Go 1.27.3; it now reports no reachable vulnerable symbol.
