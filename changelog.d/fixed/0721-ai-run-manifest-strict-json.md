Made shared AI run-manifest and report JSON writes strict RFC-8259 JSON by
serializing non-finite training diagnostics as `null` instead of `NaN` or
`Infinity`.
