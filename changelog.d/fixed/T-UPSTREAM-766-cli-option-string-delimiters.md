- **`--model` / `--feature` option strings mis-split on every `:` and `=`, so
  Windows paths were rejected and an inner `=` silently truncated a path.**
  `core/tools/cli_parse.cpp` split those strings with raw `strsep` at nine
  sites, with no escape state:
  `-m 'path=C:\models\vmaf_v0.6.1.json'` (the case Netflix/vmaf#766 reports)
  died with `Problem parsing model, bad option string
  "\models\vmaf_v0.6.1.json"`, `--feature 'psnr=some_path=C:\x'` died with
  `bad option string "\x"`, and — worst — `-m 'path=/a/dir=eq/m.json'` dropped
  everything after the second `=` and reported `could not read model from path:
  "/a/dir"`, a phantom path the user never typed. The Go surfaces made the
  truncation reachable from ordinary paths, since `pkg/libvmaf` and
  `pkg/corpus` paste user-supplied paths straight into `path=<path>`.
  The splitter is now escape-aware (ADR-1190): `\:`, `\=`, `\.` and `\\` are
  literal, every other backslash is data, a Windows drive-letter `:` is data,
  and a pair's value is everything after the **first** unescaped `=`. Go callers
  escape through the new `pkg/cliopt.EscapeValue`. The `strsep`/`HAVE_STRSEP`
  portability shim is gone, so POSIX and MSVC no longer disagree about trailing
  separators. Grammar documented in `docs/usage/cli.md`
  ("Option-string grammar"); eight regression cases in
  `core/test/test_cli_parse.c` plus round-trip tests in `pkg/cliopt`. Netflix
  CPU golden tests unaffected — no scoring path is touched.
