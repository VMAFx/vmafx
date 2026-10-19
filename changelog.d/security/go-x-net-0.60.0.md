- **`golang.org/x/net` moves from v0.59.0 to v0.60.0 and the Go toolchain from 1.27.1 to 1.27.2**
  for GO-2026-6617 (an HTTP/2 server crash from an HPACK encoder race) and twelve standard-library
  advisories published with it (`html/template`, `net/http` and its HTTP/2 copy, `crypto/tls`, `os`,
  `mime/multipart`). `go.mod` declares `go 1.27.2`, and the release and development images build on
  the `golang:1.27-trixie` digest that carries Go 1.27.2. `govulncheck ./...` reached the vulnerable
  symbols from the controller, the tune executor, `pkg/libvmaf` and `tools/obssmoke`; it now reports none.
