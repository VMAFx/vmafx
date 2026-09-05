<!-- markdownlint-disable MD013 MD060 -->
# ADR-1180: Escape-aware CLI option-string parser and Windows drive-letter affordance

- **Status**: Accepted
- **Date**: 2026-09-05
- **Deciders**: Lusoris
- **Tags**: cli, tools, bug, windows, options

## Context

In upstream Netflix/vmaf and the fork, `--model` and `--feature` CLI option
strings are parsed with raw `strsep` delimiter splitting without escape awareness
(Netflix/vmaf#766). The delimiters are `:` (key/value pair separator), `=`
(key-to-value separator), and `.` (model feature-overload separator).

When a file path contains `=` (e.g. `--model "path=/dir=eq/m.json"`) or `:`
(e.g. `--model "path=/dir:colon/m.json"` or a Windows drive letter
`--model 'path=C:\models\vmaf_v0.6.1.json'`), the raw delimiter splitting breaks.
The path is either silently truncated at the delimiter (`".../dir"`) or fails
with `bad option string` (e.g. `bad option string "\models\..."`).
The blast radius is widened in the fork because Go wrappers (`pkg/libvmaf`,
`pkg/corpus`) and MCP servers synthesize `path=` strings from user-supplied paths.
In addition, in-tree FFmpeg filter patches (`ffmpeg-patches/0008-add-libvmaf_tune-filter.patch`)
parse model specifications using `strchr(spec, '=')`.

A robust escaping grammar and Windows drive-letter affordance are needed to allow
arbitrary filesystem paths to be passed safely without breaking existing syntax.

## Decision

We make `--model` and `--feature` option-string parsing escape-aware with a compact,
in-place scanner and a single Windows drive-letter affordance:

1. **Escape Grammar**:
   - `\:`, `\=`, `\.`, and `\\` escape the respective delimiter or backslash character.
   - An in-place splitter `vmaf_cli_split(char **sp, char sep)` scans the buffer,
     compacts `\<sep>` and `\\` out of the string, and terminates the token at the
     first unescaped `sep`.
   - All nine raw option-string split call sites in `core/tools/cli_parse.cpp`
     are replaced with `vmaf_cli_split`.
   - `vmaf_cli_split` is exposed in `core/tools/cli_parse.h` with C linkage for direct
     unit testing in `core/test/test_cli_parse.c`.
   - Legacy `vmaf_cli_strsep` is retained for non-option-string callers.

2. **Windows Drive-Letter Affordance**:
   - In `parse_model_config`, when key is `"path"`, a value beginning with
     `[A-Za-z]:[\\/]` is recognized and taken verbatim without splitting at the
     drive-letter colon. This is the only special case and requires no backslash
     escaping on the drive letter colon (e.g. `path=C:\models\x.json` works as typed).

3. **Go and FFmpeg Integration**:
   - In Go (`pkg/libvmaf`), add `escapeOptValue()` to escape delimiters when synthesizing
     `-m path=...` arguments.
   - In `ffmpeg-patches/0008-add-libvmaf_tune-filter.patch`, ensure model parsing
     accommodates drive-letter paths and escaped values.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Backslash escaping (`\:`, `\=`, `\.`, `\\`) + drive-letter affordance | Standard Unix/C convention; predictable; concise; works cleanly across shells | Requires escaping literal `\` before delimiters | **Chosen**: Natural for CLI users and matches common escape conventions. |
| Quoting with `"` or `'` (e.g. `path="C:\..."`) | Familiar in shell | Shell-hostile; shells strip or mangle nested quotes; different semantics across bash, zsh, cmd.exe, and PowerShell | Rejected: quoting introduces complex shell-escaping hazards for users and scripts. |
| URL-encoding (`%3A`, `%3D`) | Standardized encoding | Surprising and unintuitive for local filesystem paths; requires percent-decoding dependencies | Rejected: unfamiliar in CLI path options and inconsistent with standard file path representations. |

## Consequences

- **Positive**: Paths containing `=`, `:`, `.`, and Windows drive letters can now be passed to `--model` and `--feature`.
- **Positive**: Existing CLI invocations without backslash escapes continue to parse identically with zero performance overhead.
- **Negative**: Users specifying literal backslashes that immediately precede delimiters must escape them as `\\`.
- **Neutral / follow-ups**: Unit tests in `core/test/test_cli_parse.c` verify escaped paths, unescaped drive letters, and regressions. `docs/usage/cli.md` updated with grammar documentation.

## References

- Upstream issue: [Netflix/vmaf#766](https://github.com/Netflix/vmaf/issues/766)
- Related: [Netflix/vmaf#1242](https://github.com/Netflix/vmaf/issues/1242)
- Prior ADRs: [ADR-0119](0119-cli-precision-flag.md), [ADR-0690](0690-vmafx-binary-and-ai-aliases.md), [ADR-0696](0696-vmafx-netflix-compat.md), [ADR-1142](1142-whole-codebase-standards.md)
- Source: `req` (task specification).
