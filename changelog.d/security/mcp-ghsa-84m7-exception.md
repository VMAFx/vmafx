- **Open advisory GHSA-84m7-p3x7-pcfv (`mcp` 1.29.0) is declared, not dismissed.** `mcp` reaches
  the tooling-tests lock only through `semgrep`, which pins it exactly; the affected Streamable HTTP
  server mode is never run. A new `dependency-advisory` rule of the declared exception list
  records the reason with a 10-day expiry, the Dependabot alerts stay open, and the entry goes when
  a `semgrep` release allows `mcp` 1.30.0.
