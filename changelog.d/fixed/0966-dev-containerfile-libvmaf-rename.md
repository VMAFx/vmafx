Fix `dev/Containerfile` build failure caused by three stale `libvmaf/` path
references that survived the ADR-0700 rename of the C source root from
`libvmaf/` to `core/`. All `docker compose build dev-mcp` invocations were
failing at the first `COPY` step with `file not found: /libvmaf`.
(ADR-0966, Round 26 audit C.1)
