# ADR-1084: Use filepath.SplitList for VMAF_MCP_ALLOW path-list parsing

- **Status**: Accepted
- **Date**: 2026-06-06
- **Deciders**: Lusoris
- **Tags**: `build`, `windows`, `mcp`

## Context

`pkg/libvmaf/paths.go` parses the `VMAF_MCP_ALLOW` environment variable by
splitting on a hardcoded `":"`.  On Windows the OS path-list separator is `";"`,
not `":"`, so a value like `C:\foo;D:\bar` would be incorrectly split at the
drive-letter colon (`C`) rather than at the semicolons.  The project CI matrix
includes a `windows-2025` runner (see `.github/workflows/build.yml`), confirming
Windows is a supported target platform.  Go's standard library provides
`filepath.SplitList`, which splits on the correct platform separator
transparently.

## Decision

Replace `strings.Split(extra, ":")` with `filepath.SplitList(extra)` when
parsing `VMAF_MCP_ALLOW` in `AllowedRoots` (`pkg/libvmaf/paths.go`).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Keep `":"` and document that Windows must use `";"` | No code change | User-hostile; silently wrong on Windows; contradicts how the rest of the stdlib works | The stdlib already handles this correctly |
| Accept `":"` on Unix and `";"` on Windows via build tags | Explicit | Duplicated logic, fragile | `filepath.SplitList` already does exactly this |

## Consequences

- **Positive**: `VMAF_MCP_ALLOW` with multiple paths works correctly on Windows
  (`C:\foo;D:\bar`) and continues to work on Unix (`/a:/b`).
- **Negative**: None.  The change is a one-line swap with identical semantics on
  Unix (`:` is the list separator on all Unix-like OSes).
- **Neutral / follow-ups**: The `strings` import is retained because `HasPrefix`
  in `ValidatePath` still needs it; no import churn.

## References

- Go documentation: `path/filepath.SplitList` — splits a list of paths joined
  by the OS-specific `ListSeparator`.
- `.github/workflows/build.yml` — `os: windows-2025` matrix entry confirms
  Windows target.
