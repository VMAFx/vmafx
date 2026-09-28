- `--model` and `--feature` values keep their backslashes, so Windows paths work
  as typed: `path=..\..\models\m.json`, `path=\\server\share\m.json` and
  `path=C:\models\.cache\m.json` used to lose a backslash each (`\.` and `\\`
  were escapes in values too). `\:` and `\=` still escape a delimiter, and a
  backslash run directly before `:` or `=`, or at the end of a value, is read in
  pairs so a backslash in front of a delimiter stays writable. Keys and
  overload names keep the full escape set (ADR-1355). If you wrote a UNC path
  as `\\\\server\share` per the earlier advice, write `\\server\share` now.
