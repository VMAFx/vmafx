AI report-style stdout JSON now uses the shared strict manifest serializer so
non-finite diagnostics print as JSON `null` instead of `NaN` / `Infinity`.
