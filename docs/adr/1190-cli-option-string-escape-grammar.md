<!-- markdownlint-disable MD013 -->

# ADR-1190: Backslash escapes and a drive-letter affordance in the CLI option-string grammar

- **Status**: Accepted
- **Date**: 2026-09-06
- **Deciders**: Lusoris
- **Tags**: cli, parser, windows, upstream, bug

## Context

`--model` and `--feature` take a colon-delimited list of `key=value` pairs.
`core/tools/cli_parse.cpp` split those strings with raw `strsep`, so **every**
`:` and `=` in the string was a separator, whatever the user meant. Three
consequences, all reproduced on the pre-fix binary
(`build-cpu/tools/vmaf`, commit `cd52f2670`):

- `-m 'path=/a/dir=eq/m.json'` → `could not read model from path: "/a/dir"`.
  The second `=` ended the value and the remainder was dropped on the floor:
  a phantom path the user never typed, reported as a missing file.
- `-m 'path=C:\models\vmaf_v0.6.1.json'` → `bad option string
  "\models\vmaf_v0.6.1.json"`. This is the case Netflix/vmaf#766 reports; every
  absolute Windows model path is unusable.
- `--feature 'psnr=some_path=C:\x'` → `bad option string "\x"`, and there was no
  escape syntax to work around any of it.

The fork's blast radius is wider than upstream's because Go surfaces synthesise
the same strings from user-supplied paths (`pkg/libvmaf`, `pkg/corpus`,
`pkg/scorecli`, `cmd/vmafx-mcp`), so a path with a `:` or `=` in it silently
became a different path rather than an error.

## Decision

We will parse `--model` / `--feature` option strings with an escape-aware
splitter. `\:`, `\=`, `\.` and `\\` are literal characters; every other
backslash is data (so `C:\models\m.json` passes through byte-for-byte); a `:`
that spells a Windows drive letter — one ASCII letter at the start of a key or
value, followed by `:` and then `\` or `/` — is data rather than a separator;
and the value of a pair is *everything* after the first unescaped `=`, so an
inner `=` can no longer truncate a path. Splitting and unescaping are separate
passes (`cli_split()` leaves backslashes in place, `cli_unescape()` removes them
once at the leaf) so an escape written for the `:` pass survives into the `=`
pass. Go callers that build `path=<user path>` escape the path with
`cliopt.EscapeValue`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| **Escape-aware splitter + drive-letter affordance** (chosen) | Fixes all three reproduced failures; the common Windows path needs no escaping; grammar is expressible and documented | A UNC prefix (`\\server\share`) now needs `\\\\server\share` or forward slashes; new grammar to document | — |
| Backslash escapes only, no drive-letter rule | Simpler, one rule | Every Windows user must write `path=C\:\models\...`, which is exactly the ergonomic complaint in Netflix/vmaf#766 | Rejected: fixes the parser but not the reported user experience |
| Drive-letter rule only, no escapes | Zero new syntax | Leaves `:`/`=` in POSIX paths unusable, and leaves the silent truncation unaddressed for keys | Rejected: the truncation is the worst failure mode |
| Treat only the *last* `=` (or reject strings with two `=`) as an error | No grammar change | A hard error on a legitimate path is a regression for anyone whose path contains `=`; still no way to express a `:` | Rejected: fails loudly instead of working |
| Drop `\\` from the escape set so UNC paths survive | UNC works unescaped | No way to write a literal backslash before a delimiter; asymmetric, surprising grammar | Rejected: the ledger's closure criterion and the principle of a complete escape set both call for `\\` |
| Quote-based grammar (`path="C:\a:b"`) | Familiar from shells | Requires quote state through two split levels, and the shell already ate one quoting layer | Rejected: more state, worse interaction with shell quoting |

## Consequences

- **Positive**: Windows model paths work unquoted; an inner `=` can no longer
  silently truncate a path to a shorter, wrong one; `:`, `=` and `.` are
  expressible anywhere via a backslash; the `strsep` portability shim and its
  `HAVE_STRSEP` fork are gone, so POSIX and MSVC now run identical code (they
  previously disagreed on whether a trailing separator yields an empty token).
- **Negative**: a literal `\\` is now an escape, so a Windows UNC prefix must be
  written `\\\\server\share` or with forward slashes. The grammar is one more
  thing to learn; `docs/usage/cli.md` documents it with a table and examples.
- **Neutral / follow-ups**: `pkg/cliopt` holds the Go-side escaper so
  `pkg/libvmaf` and `pkg/corpus` stop handing the CLI unescaped user paths.
  The `libvmaf_tune` ffmpeg filter is **not** affected: its `load_model()` takes
  the whole remainder after the first `=` and never splits on `:`, and ffmpeg's
  own filtergraph parser owns escaping at that layer — double-unescaping there
  would be a new bug, so `ffmpeg-patches/` is unchanged.

## References

- Netflix/vmaf#766 — `--model path=C:\...` rejected as a bad option string.
- `docs/state.md` row `T-UPSTREAM-766-CLI-OPTION-STRING-DELIMITERS-2026-09-03`.
- [ADR-0165](0165-state-md-bug-tracking.md) — state ledger rule.
- [docs/usage/cli.md](../usage/cli.md) — "Option-string grammar".
