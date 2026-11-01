- The dependency pull request classifier now allows the exact path
  `cmd/vmafx-controller/internal/storetest/storetest.go`, so Renovate pull
  requests that update only the PostgreSQL container image digests
  (#3039) are recognized as dependency-only and pass the deliverables
  checklist. Sibling files and other Go source under `cmd/` stay gated.
