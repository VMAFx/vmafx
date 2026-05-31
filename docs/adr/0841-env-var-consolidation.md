<!-- markdownlint-disable MD060 -->
# ADR-0841: Environment variable reference page and canonical naming

- **Status**: Accepted
- **Date**: 2026-05-29
- **Deciders**: lusoris
- **Tags**: `docs`, `sycl`, `cuda`, `ai`, `workspace`

## Context

Audit commit `a5d0c683af8f0d15f` identified 30+ environment variables spread
across the C library, AI scripts, MCP server, and four Go binaries, none of
which were documented in a single place. Operators had to grep the source tree
to discover which variables to set in production containers and Helm values.

A separate issue was found in `core/src/sycl/dispatch_strategy.cpp`: the old
`VMAF_SYCL_NO_GRAPH` knob (which existed before `VMAF_SYCL_USE_GRAPH` was
introduced as the canonical successor) continued to be silently read with no
deprecation signal, creating two live env vars with overlapping semantics.

The `docs/server/node.md` page also listed four env vars
(`VMAFX_CONTROLLER_ADDR`, `VMAFX_NODE_ID`, `VMAFX_BACKEND`,
`VMAFX_GPU_DEVICE`) that are not read in the Go source tree — they are
placeholders from the original Phase 4b spec (ADR-0713) that were never
implemented.

## Decision

1. Create `docs/usage/env-vars.md` as the canonical, code-audited reference
   for all env vars, grouped by surface, with name, type, default, and
   description columns.  Add the page to `mkdocs.yml` under `Usage:`.

2. Add a stderr deprecation warning in
   `core/src/sycl/dispatch_strategy.cpp` when `VMAF_SYCL_NO_GRAPH` is set,
   directing users to `VMAF_SYCL_USE_GRAPH=false`.  The old variable
   continues to function for one release.

3. Remove the four unimplemented env-var rows from `docs/server/node.md` and
   replace them with a clearly labelled "Planned (not yet implemented)" note
   listing those vars under ADR-0713.

4. Create `docs/server/operator.md` documenting the four
   `VMAFX_OPERATOR_*` env vars that `cmd/vmafx-operator/main.go` reads.

5. Add a "Backend dispatch knobs" paragraph to `docs/backends/cuda/overview.md`
   and `docs/backends/sycl/overview.md` cross-linking `VMAF_CUDA_DISPATCH` /
   `VMAF_SYCL_DISPATCH` to `docs/usage/env-vars.md` and ADR-0483.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Maintain per-surface env-var tables only (existing state) | No new page to maintain | Discovery requires opening 5+ docs; still no list for C vars | Discoverability gap too costly for operators |
| Auto-generate from source via a Meson script | Always up-to-date | Requires a build step in CI just for docs; complex to format nicely | Overkill for a stable-ish surface |
| Deprecate `VMAF_SYCL_NO_GRAPH` silently (no warning) | Zero code change | Operators keep using it indefinitely; tech debt compounds | Warning costs 3 lines of C++ and catches the migration early |

## Consequences

- **Positive**: single authoritative page for operators, helm chart authors,
  container maintainers.  Deprecation warning surfaces the `NO_GRAPH` → `USE_GRAPH`
  migration before v4.0 removal.  Ghost vars in `node.md` no longer mislead users.
- **Negative**: env-vars.md needs updating whenever a new var is introduced or
  removed; CLAUDE.md §12 r2 (every user-discoverable surface ships docs) already
  mandates this.
- **Neutral / follow-ups**: `VMAF_SYCL_NO_GRAPH` removal is scheduled for v4.0;
  a `changelog.d/fixed/` fragment records it now.

## References

- Audit commit: `a5d0c683af8f0d15f`
- [ADR-0483](0483-gpu-dispatch-parse-dedup.md) — GPU dispatch parse grammar
- [ADR-0713](0713-vmafx-node-impl.md) — vmafx-node spec (source of planned vars)
- [docs/usage/env-vars.md](../usage/env-vars.md) — the page created by this ADR
