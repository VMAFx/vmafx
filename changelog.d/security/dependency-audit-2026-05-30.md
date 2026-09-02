- **Dependency audit 2026-05-30 — Go `x/net` + `x/sys` bumped to clear 7
  advisories** — the cross-ecosystem dependency audit (pip-audit across
  four `requirements*.txt` and six `pyproject.toml` manifests + `govulncheck`
  against the Go workspace) surfaced seven Go advisories on
  `golang.org/x/net@v0.53.0` and `golang.org/x/sys@v0.43.0`. One is a
  **direct** symbol-reachable hit — `GO-2026-5026` (`idna.ToASCII`
  failure to reject ASCII-only Punycode-encoded labels) — reached via
  `cmd/vmafx-operator/internal/controller/vmafxnode_controller.go:111`
  on the operator's healthz probe path. The other six (`GO-2026-5024`,
  `GO-2026-5025`, `GO-2026-5027`–`5030`) are module-level findings on
  `x/net/html` (DoS, XSS via duplicate attributes, namespaced foreign-
  content mishandling, DOCTYPE character-reference handling) and one on
  `x/sys/windows` (`NewNTUnicodeString` integer overflow) where govulncheck
  cannot prove the vulnerable symbols are reached but the modules are in
  the require-graph and a future code change could light them up. Fix:
  `go get golang.org/x/net@v0.55.0 golang.org/x/sys@v0.45.0 && go mod
  tidy` — the bump pulls `x/term` and `x/text` along the
  transitively-implied minimum-version requirement. `govulncheck ./...`
  re-run after the bump: `No vulnerabilities found`. Python audit
  (`docs/`, `python/`, `python/test/`, `tools/ensemble-training-kit/`
  requirements; `pyproject.toml` for `ai/`, `mcp-server/vmaf-mcp/`,
  `dev-llm/`, `tools/vmaf-tune/`, `tools/vmaf-roi-score/`,
  `tools/ensemble-training-kit/`): no known vulnerabilities found.
  Container scan: `vmafx-dev-mcp:latest` not present locally, Trivy
  pass deferred to the CI image-scan workflow. See
  `docs/research/dependency-audit-2026-05-30.md`.
