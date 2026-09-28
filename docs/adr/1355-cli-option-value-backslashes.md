<!-- markdownlint-disable MD013 -->

# ADR-1355: Backslashes in CLI option values are data, except in runs that touch a delimiter

- **Status**: Accepted
- **Date**: 2026-09-28
- **Deciders**: Lusoris
- **Tags**: cli, parser, windows, upstream, bug

## Context

[ADR-1190](1190-cli-option-string-escape-grammar.md) gave the `--model` /
`--feature` option strings one escape set, `\:`, `\=`, `\.` and `\\`, and
`core/tools/cli_parse.cpp` applied it to keys and values alike. Values are
where paths live, and on Windows that set eats path bytes. Reproduced through
`cli_parse()` on master `7d4d436b8`:

- `path=..\..\models\m.json` → `....\models\m.json` (`\.` is an escape).
- `path=\\server\share\m.json` → `\server\share\m.json` (`\\` is an escape).
- `path=C:\models\.cache\m.json` → `C:\models.cache\m.json`.
- `name=a\=b\\c` → `a=b\c`.

The `.` escape exists for one purpose, the split of a model overload key
`<feature>.<option>`, and never applies to a value. ADR-1190 knew about the UNC
case and documented `\\\\server\share` as the workaround; it rejected dropping
`\\` from the set because a backslash in front of a delimiter would then be
inexpressible. The relative-path and dot-directory cases were not considered,
and the upstream report this fork design answers (upstream issue 766) is about
Windows paths.

## Decision

We will unescape keys and values differently. Keys (the part before the first
`=`, the `--feature` name, and both halves of an overload key) keep ADR-1190's
set through `cli_unescape_key()`. Values go through `cli_unescape_value()`: a
backslash is data unless it belongs to a run that sits directly before `:` or
`=`, or that ends the value. Such a run is read in pairs, `\\` standing for one
backslash, and a single backslash left over escapes the `:` or `=` after it (at
the end of the value it stays a backslash). This is the rule the Microsoft C
runtime applies to backslashes before a double quote, with `:` and `=` in the
quote's place. `pkg/cliopt.EscapeValue` emits the same grammar: it prefixes `:`
and `=` with a backslash, doubles a backslash run that precedes either or ends
the value, and copies every other byte.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| **Paired runs at delimiters only** (chosen) | `..\`, `\\server` and `\.cache` pass through verbatim; `\:` / `\=` keep their meaning; every string stays expressible, so `EscapeValue` is total and position-independent; matches `cli_split()`'s pairing (a `:` after an odd run is literal) | One rule more than "`\:` and `\=` only"; a value that ends in `\\` now reads as one backslash | — |
| Only `\:` and `\=` are escapes, every other backslash kept (`\\` included) | Shortest rule to state | A backslash directly before a literal `:` cannot be written: `a\\:b` splits after `a\\` (the splitter pairs backslashes), and `a\\\:b` reads back as `a\\:b`; `EscapeValue` would have to fail or corrupt such a path | Leaves a hole in the grammar that the Go escaper cannot route around |
| Keep ADR-1190's single escape set | No change | The reported Windows paths keep losing bytes; users must double every UNC and `..\` backslash | Fails the task |
| Drop escapes from values entirely | Simplest | A `:` inside a POSIX path becomes inexpressible again; breaks ADR-1190's `path=/a/dir\=eq/m.json` and `\:` users | Regresses the ADR-1190 fix |
| A quoting syntax for values (`path="..."`) | Familiar | Needs quote state across two split levels and fights the shell's own quoting | Same reason ADR-1190 rejected it |

## Consequences

- **Positive**: Windows relative, UNC and dot-directory paths work as typed in
  `--model` and `--feature` values. Existing `\:` / `\=` escapes, the
  drive-letter affordance and model overload keys behave as before.
- **Negative**: a value's `\\` that does not touch a delimiter is now two
  backslashes. Anyone who followed ADR-1190's advice and wrote
  `\\\\server\share` now gets four backslashes and must write `\\server\share`.
  A value ending in `\\` reads as one backslash.
- **Neutral / follow-ups**: `pkg/cliopt.EscapeValue` changed in the same
  commit; its round-trip test mirrors `cli_unescape_value()` and `cli_split()`.
  `ffmpeg-patches/` is unaffected (the filter never splits on `:`).
  `docs/usage/cli.md` documents the key/value split.

## References

- [ADR-1190](1190-cli-option-string-escape-grammar.md) — the grammar this amends.
- Upstream issue 766 (Netflix/vmaf) — Windows model paths in `--model`.
- `docs/state.md` row `T-CLI-VALUE-BACKSLASH-ESCAPES-2026-09-28`.
- [docs/usage/cli.md](../usage/cli.md) — "Option-string grammar".
- Microsoft, "Parsing C command-line arguments" — the backslash-before-quote
  rule this mirrors.
- Source: `req` — task brief for this branch: "a value variant that only
  treats `\:` and `\=` as escapes and keeps every other backslash as data";
  the run pairing is the refinement that keeps `EscapeValue` total.
