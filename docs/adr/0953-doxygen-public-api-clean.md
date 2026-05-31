<!-- markdownlint-disable MD060 -->
# ADR-0953: Doxygen public-API build is warning-clean

- **Status**: Accepted
- **Date**: 2026-05-31
- **Deciders**: lusoris, Claude Code
- **Tags**: `docs`, `ci`, `api`, `public-surface`

## Context

The fork ships a `core/doc/Doxyfile.in` driven by the `enable_docs` meson
option that targets the wider source tree (public headers + internal
feature kernels). It runs ad-hoc — there is no CI job — and prior fork
work (PRs #302 / #327 / #388) documented many of the per-function
comments without ever closing the loop on "does doxygen actually accept
this input?". The most recent ad-hoc run produced **95 warnings** across
the public headers: 44 undocumented struct members, 21 functions missing
`@param`, 11 missing `@return`, 8 undocumented compound types, plus 11
miscellaneous (the JSDoc-style `@field` command is not a doxygen command,
unresolved `@ref` targets, etc.).

Two of the recurring patterns are structural rather than authoring
mistakes:

1. **`@field` in struct doc-blocks.** Several fork-added headers
   (`libvmaf_mcp.h`, `dnn.h`, `libvmaf_metal.h`) documented struct
   members via `@field name desc` inside the surrounding doc comment.
   Doxygen treats `@field` as an unknown command and the per-member
   docs disappear, leaving every field flagged as undocumented. The
   canonical C pattern is per-member inline `/**< desc */`.
2. **Cross-symbol `@ref` from struct doc-blocks.** Doxygen cannot
   resolve a `@ref function_name` when the target lives in a different
   compilation unit scope from the struct doc comment that emits the
   reference. The references work in function-doc blocks but not in
   struct-doc blocks. The portable workaround is backtick literals
   (`vmaf_picture_alloc`) which doxygen renders as code without
   trying to resolve them.

The pre-existing meson-driven Doxyfile is fine for full-tree generation
and stays untouched, but it is not a tight feedback loop for "is the
public API still doc-clean?" — it bundles the public headers with the
internal sources, and the warning bar drifts. A second, narrower
Doxyfile that targets only the installable public headers under
`core/include/libvmaf/` and an on-demand CI job that publishes the
warning log give the fork a concrete, measurable gate that can later be
promoted to a required check.

## Decision

We will add a standalone `core/doc/Doxyfile.public-api` that targets
only the public C headers under `core/include/libvmaf/`, fix every
warning the baseline run surfaced (95 -> 0), and wire it through an
on-demand `doxygen-public-api` CI workflow that publishes the generated
HTML and warning log as build artifacts. `WARN_AS_ERROR` stays OFF
until the workflow is added to `required-aggregator.yml`; until then
the workflow is informational only.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **New standalone Doxyfile + on-demand CI (chosen)** | Tight feedback loop on the public API only; no churn in the existing meson-driven full-tree generator; on-demand keeps it cheap until we promote it to required | One more Doxyfile to maintain; warning bar can still drift between manual runs until the workflow is required-gated | Best balance of "make the gate exist" against "don't break the existing full-tree generator" |
| Reuse `core/doc/Doxyfile.in` and flip its `WARN_AS_ERROR` ON | Single source of truth | The full-tree generator pulls in `src/feature/` internal headers; making those warning-clean is a much wider scope; tying CI to it now would either fail or require a Doxyfile that hides 80% of the source | Out of scope - the immediate need is the public API |
| Use `clang-doc` or `Sphinx-C` instead | Modern tooling, structured output | Two new dependencies, no incremental gain over doxygen for the public C surface, every existing fork-added doc comment uses doxygen tags | Cost of migration is not justified |

## Consequences

- **Positive**:
  - The public C API is warning-clean against doxygen 1.15 - every
    documented struct member, function parameter, and return value
    renders correctly.
  - The `@field` antipattern is removed from the headers it had crept
    into (`libvmaf_mcp.h`, `dnn.h`, `libvmaf_metal.h`).
  - The on-demand CI workflow makes the gate visible without blocking
    other work; a future PR can promote it to required by appending it
    to `required-aggregator.yml` and flipping `WARN_AS_ERROR=YES` in
    the Doxyfile.
- **Negative**:
  - Two Doxyfiles to keep in sync conceptually (the full-tree
    `Doxyfile.in` and the public-API `Doxyfile.public-api`). The
    public-API one is short (~60 lines) and stable, so the maintenance
    cost is small.
- **Neutral / follow-ups**:
  - Promote the workflow to required-aggregator + flip
    `WARN_AS_ERROR=YES` once it has demonstrated stability on master
    (one merge-train cycle).
  - Consider regenerating the public-API doxygen as part of the
    mkdocs-material site under `docs/api/` so the rendered API
    appears in the project's published docs.

## References

- `core/doc/Doxyfile.public-api` - the new standalone Doxyfile.
- `.github/workflows/doxygen-public-api.yml` - the on-demand CI job.
- `core/include/libvmaf/*.h` - every public header touched in this PR.
- Prior fork PRs that documented public symbols but never gated them:
  PR #302, PR #327, PR #388.
- Doxygen 1.15 warning reference: <https://www.doxygen.nl/manual/commands.html>
- Source: `req` - "Add + clean a doxygen build for the libvmaf public C
  API" (user request, this session).
